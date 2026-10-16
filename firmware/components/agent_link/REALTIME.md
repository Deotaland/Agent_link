# WiFi realtime voice

Korvo Cloud's Voice screen uses the selected agent and conversation. It opens the microphone when
`session.ready` arrives and keeps it open until the user leaves Voice or ends the call. There is no
push-to-talk or text chat in this screen.

## Credentials and API calls

The device's `auth_key` authenticates `/api/device-gateway/*` and obtains its MQTT token. For
voice conversation and grant requests, the firmware also sends this stored key in
`X-Device-Auth-Key`. A captured successful platform request confirms this header for listing
conversations and requesting a realtime grant. Other voice operations use the same header but
still need device validation. The OpenAPI document does not describe this header.

Agent listing and device-to-agent binding remain user operations. They use the optional
`CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN` from private `sdkconfig` in `Authorization: Bearer` and
`X-Token`; voice requests fall back to it when no device key is stored. Do not put the user token
in source, defaults, logs, or a shared firmware image. Without it, select or change the bound
agent in the platform console; an agent and conversation already saved on the device can use the
device key for voice.

The touch-screen flow is:

| Action | Platform request |
| --- | --- |
| List voice agents | `GET /api/agents?page=…&page_size=20`; keep agents supporting `realtime_voice` |
| Bind an agent | `GET /api/devices?keyword=<device SN>&page=1&page_size=20`, verify an exact SN match, then `POST /api/devices/{device_id}/bind-agent` |
| List conversations | `GET /api/agents/{agent_id}/actions/realtime_voice/conversations` |
| Create a conversation | `POST` to the same conversations path |
| Delete a conversation | `DELETE /api/agents/{agent_id}/actions/realtime_voice/conversations/{conversation_id}` |
| Start a call | `POST /api/agents/{agent_id}/actions/realtime_voice/realtime-grant` with `session_id` and `audio_config` |
| Recover a busy grant | On HTTP 409 only, `POST /api/agents/{agent_id}/actions/realtime_voice/realtime-refresh` with the same `session_id` |
| End a call | `DELETE /api/agents/{agent_id}/actions/realtime_voice/realtime-grant/{session_id}` |

The selected agent and conversation are saved in NVS. **Voice** connects the selection; **New
session** creates and selects another conversation; **Settings** changes the selection. A deleted
selected conversation is cleared from NVS. HTTPS calls run on a worker, outside LVGL callbacks.

The grant returns a one-use WebSocket token. The worker connects over certificate-checked `wss://`
using subprotocol `tidb-link.realtime.v1`, then sends `session.connect`. A reconnect requires a new
grant; the old token is never reused. Application logs omit grant URLs, tokens, request and response
bodies, device IDs, conversation IDs, and saved WiFi names.

## Audio path

`cloud_platform.cc` owns the user API calls. `realtime_format.h` validates the negotiated audio.
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
