# WiFi realtime voice

Korvo WiFi and StopWatch WiFi use the agent bound to the device in the platform console.
The device lists, creates, selects and deletes that agent's conversations, and starts or ends
realtime voice calls. Agent selection and binding take place in the console only.
The microphone opens when `session.ready` arrives and stays open until the user ends the call
or leaves Voice. These screens do not provide push-to-talk or text chat.

## Credentials and API calls

After WiFi setup, the device displays a six-digit claim code. The user claims it in the
platform console; `bind-status` returns `auth_key`, which the device persists in NVS.
This is the device's only persistent platform credential. No account login token is required
or sent. The legacy `CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN` setting is hidden and ignored,
so existing private configuration files remain readable without changing authentication.

The existing `POST /api/device-gateway/auth` sends the device key and MAC address in its JSON
body. Its `data.agent` contains the platform-bound agent's ID, name and capabilities. The SDK
caches only this metadata for the current device key; it does not obtain it from Agent/device
management APIs or trust the Agent ID saved in NVS as the current platform binding.
`agent_link_platform_bound_agent()` reads the cache without blocking on network requests.
Authentication refreshes every heartbeat interval (60 seconds). A platform rebind invalidates
the previous conversation selection and ends its call; loss of binding or key also disables
session operations. The old list/bind Agent functions return `ESP_ERR_NOT_SUPPORTED` without
sending any HTTP requests.

The device uses `X-Device-Auth-Key` for conversation and voice operations:

| Action | Platform request |
| --- | --- |
| Read current platform binding | `POST /api/device-gateway/auth`; use `data.agent` |
| List conversations | `GET /api/agents/{agent_id}/actions/realtime_voice/conversations?limit=20` and `cursor` for later pages |
| Create a conversation | `POST` to the same conversations path |
| Delete a conversation | `DELETE /api/agents/{agent_id}/actions/realtime_voice/conversations/{conversation_id}` |
| Start a call | `POST /api/agents/{agent_id}/actions/realtime_voice/realtime-grant` with `session_id` and `audio_config` |
| Recover a busy grant | On HTTP 409 only, `POST /api/agents/{agent_id}/actions/realtime_voice/realtime-refresh` with the same `session_id` |
| End a call | `DELETE /api/agents/{agent_id}/actions/realtime_voice/realtime-grant/{session_id}` |

The selected conversation and its Agent context are saved in NVS. They are restored only if
the authenticated platform binding still matches. **Sessions** opens the list; **New session**
creates and selects another conversation. Deletion requires a second confirmation and clears
the local selection if it was deleted. HTTPS calls run on a worker, outside LVGL callbacks.
The SDK follows the server's `has_more` / `next_cursor` pagination; the UI shows the displayed
range rather than claiming an exact total while later pages remain available.

On 2026-10-17 real device-key requests confirmed gateway authentication and the bound Agent
metadata, conversation listing, an existing conversation's voice grant, verified TLS/WSS with
`tidb-link.realtime.v1`, `session.connect` / `session.ready`, and ending the test grant.
Agent and device management listing returned business code 1005: those are account APIs and
are intentionally absent from the device flow. Creating a conversation returned business
code 1002 because the upstream returned HTTP 404 `agent_template_unavailable` /
`Published Template unavailable.` That platform/template issue still prevents validating new
sessions. Existing conversations were preserved; real deletion, audio upload/playback and
physical-device behavior were not tested. Host API checks do not constitute device testing.

The grant returns a one-use WebSocket token derived from device authentication. It does not
require another manually configured credential. The worker connects over certificate-checked
`wss://` using subprotocol `tidb-link.realtime.v1`, then sends `session.connect`. Reconnection
requires a fresh grant. Logs omit grant URLs, tokens, request/response bodies, device IDs,
conversation IDs and saved WiFi names. Nonzero business codes are rejected even on HTTP 200;
only numeric error codes are logged.

## Audio path

`cloud_platform.cc` owns the device-authenticated platform API calls. `realtime_format.h` validates the negotiated audio.
`cloud_realtime.cc` owns grants, WebSocket I/O, and the encoder and decoder workers.
`transport_wifi.cpp` exposes them to the SDK; the Korvo board owns mic,
speaker, and UI. The component does not access the codec or display.

Platform grants request mono Opus in both directions: 16 kHz input, 24 kHz output, 20 ms packets.
`audio_config` contains the inner input/output object, using `sampleRate` and `packetDurationMs`.
The platform wraps it in `audio` for the upstream request. `bitrate` is sent only for output.
The current platform grant may report default PCM16 metadata even when Opus was accepted.
`session.ready` confirms the actual format; PCM16 fallback there is logged explicitly.
`CONFIG_AGENT_LINK_REALTIME_OPUS` applies to platform sessions as well as direct tests.

After `session.ready`, the board supplies PCM16LE, 16 kHz microphone chunks. A dedicated encoder
uses a PSRAM stack and produces 24 kbit/s Opus. Each `input_audio.append` contains one Base64 Opus
packet; packets are never concatenated. If PCM16 was negotiated, the network worker batches up to
eight queued chunks without waiting for more. Platform conversations use server endpointing, so
the microphone stays live. Both uplink queues are bounded.

Replies arrive as binary audio or `response.audio.delta`. The negotiated output is 24 kHz mono
PCM16 or Opus. The network worker copies complete messages to a bounded PSRAM queue. A separate
decoder worker converts them to 16 kHz PCM16 and paces playback, so waiting for the speaker does not
block microphone upload or WebSocket reads. TCP_NODELAY avoids waiting between small WebSocket
header and payload writes. Receive polling waits only when no uplink data is pending.
Interruption invalidates queued downlink audio immediately, without waiting for playback pacing.
`response.audio.done` marks the
end of this reply. Korvo Cloud holds playback PCM in PSRAM and starts after about 500 ms is queued;
after an underrun it waits for about 1000 ms. End-of-reply releases a shorter tail immediately.
The playback buffer cannot make up for a sustained shortage of network throughput.

Korvo Cloud currently prevents acoustic feedback by uploading silence during speaker playback.
The microphone keeps sampling; real microphone uploads resume after the I2S drain allowance
(100 ms) and acoustic tail (200 ms). This guard follows actual speaker writes, so a cloud thinking
pause does not disable the microphone. Speaking over the agent does not interrupt it in this mode.
This is a playback guard, not acoustic echo cancellation.

Korvo Cloud WiFi uses size optimization to keep both Opus codecs within the existing OTA slot.

Every five seconds during a continuous call, one `voice` line reports captured, sent, and dropped
20 ms frames, queued duration, WebSocket payload bytes/s, decoded duration, average/maximum send
time, encoder time, pending downlink bytes, and mic/decoder/encoder stack headroom. Reply and speaker
summaries show duration, packet gaps, underruns, and dropped playback
bytes. Reply start, interruption, end, and speaker drain counts distinguish a server interruption
from a playback underrun. Normal call cancellation does not count as an Opus codec failure.
These counters contain no audio or credentials.

## Other grant sources

The backend may also deliver a short-lived grant over the authenticated MQTT downlink.
`CONFIG_AGENT_LINK_REALTIME_DIRECT` is a separate development path using a fixed
Agent Stack session and a private User API Key. Both feed the same WebSocket worker; neither is the
touch-screen conversation flow. Keep either credential in private `sdkconfig`.
