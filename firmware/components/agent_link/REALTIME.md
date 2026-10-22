# WiFi realtime voice

AI Passport, Korvo, StopWatch, Waveshare ePaper 3.97 and paper WiFi profiles use the Agent bound in the platform
console. Devices list, create, select and delete that Agent's conversations and
start/end calls. Agent selection and binding happen in the console.

## Authentication and calls

Hotspot setup is followed by a six-digit activation code. After the user claims
it, `bind-status` returns a device `auth_key`, stored in NVS. This is the only
persistent platform credential needed. The legacy account-token option is
ignored by this flow.

`POST /api/device-gateway/auth` sends the device key and MAC in JSON. `data.agent`
provides the bound Agent's metadata. `agent_link_platform_bound_agent()` reads
the cached result without blocking. Authentication refreshes every 60 seconds;
a rebind ends the old call and clears its conversation selection.

Conversation/voice requests use `X-Device-Auth-Key`:

| Action | Request below `/api/agents/{agent_id}/actions/realtime_voice/` |
| --- | --- |
| List | `GET conversations?limit=20`, with `cursor` for later pages |
| Create | `POST conversations` |
| Delete | `DELETE conversations/{conversation_id}` |
| Start | `POST realtime-grant` with `session_id` and `audio_config` |
| Recover busy grant | On HTTP 409, `POST realtime-refresh` for the same session |
| End | `DELETE realtime-grant/{session_id}` |

The selected conversation is saved in NVS and restored only for the same
platform binding. Deletion requires confirmation. List UIs follow
`has_more`/`next_cursor`; they must not present a partial page as a total count.
HTTPS work runs outside UI callbacks.

A grant contains a one-use WebSocket token. The worker connects over verified
TLS with subprotocol `tidb-link.realtime.v1`, sends `session.connect` and opens
the microphone only after `session.ready`. Reconnection needs a fresh grant.
Logs omit URLs with credentials, request bodies and saved device/session IDs.
Nonzero business codes fail even when HTTP status is 200.

Outgoing text frames (audio and session controls) combine the masked WebSocket
header and payload in a reusable buffer for one TLS write, with a fresh random
mask per frame. TLS writes use a 3000 ms transport timeout. Slow successful
writes log wire size and write duration, at most once a second. A failed or partial frame is never replayed
on that stream. Platform calls retry transport failures at most twice per call,
after 2 and 4 seconds, releasing the previous grant and obtaining a fresh token.
Microphone capture pauses during recovery and queued audio is discarded; speech
during the interruption must be repeated. Leaving the call cancels recovery.
Explicit server session errors/close frames, invalid media and grant rejection
do not trigger automatic retries. MQTT/direct grants retain manual reconnection.
Logs include operation/result, errno, socket/TLS errors, WiFi association/RSSI,
internal heap availability and uplink queue duration, without server reason text.

Continuous Opus upload coalesces up to three complete WebSocket messages in one
TLS write; each retains its own JSON, Opus packet and random mask. The first
queued packet waits at most 40 ms to form a batch, excluding network stalls.
PCM and activity-control messages retain their existing packet boundaries.
Before dequeuing uplink audio, the worker polls writability without blocking;
while the socket is backpressured it continues receiving replies and checks a
3-second stall deadline. A write can still block after the readiness check.
The `writes` counter distinguishes TLS audio writes from the `ws` message count;
`send` reports average/maximum time per TLS write. Network stalls and queue
overflows can still lose microphone audio; coalescing does not guarantee delivery.

## Audio and workers

`cloud_platform.cc` owns device-authenticated APIs. `cloud_realtime.cc` owns
grants, WebSocket I/O and encoder/decoder workers. Applications own microphone,
speaker and UI. Calls capture continuously until ended; they are not BLE
push-to-talk turns.

S3 profiles request mono Opus: 16 kHz input, 24 kHz output, 20 ms packets.
`audio_config` uses `sampleRate` and `packetDurationMs`; output also specifies
`bitrate`. `session.ready` determines the actual format, including PCM16 fallback.
Each `input_audio.append` carries one Base64 Opus packet. Input and output
queues are bounded. Encoding and decoding do not block WebSocket reads.

Replies arrive as binary audio or `response.audio.delta`. The decoder converts
negotiated PCM16/Opus to 16 kHz PCM16 and paces playback. Interruption invalidates
queued audio. Korvo primes about 500 ms of playback, or 1000 ms after underrun;
`response.audio.done` releases a shorter tail without waiting.

During speaker playback the applications upload silence, resuming real mic
samples after the I2S drain and echo tail. This playback guard is not acoustic
echo cancellation and does not allow spoken interruption. Periodic voice logs
report frame drops, queue duration, throughput and stack space without audio
or credentials.

The [paper profile](../../boards/paper-s31/README.md) requests PCM16 because the
current codec library has no S31 Opus build. It uses 16 kHz input and 24 kHz
output, converted by the SDK. Reject unsupported negotiated formats.

## Device controls

StopWatch uses its touchscreen for calls, conversations and settings. Sessions
shows three entries per page. Settings can create/delete a conversation or reset
activation; delete/reset require confirmation. Reset keeps WiFi, and the device
must be unbound in the console before reactivation. Yellow/blue keys change
volume by 5%; hold to repeat. The first touch after idle dimming wakes the screen.
Network requests disable conflicting touch actions while volume keys remain usable.

Waveshare ePaper 3.97 uses its three-way key for conversations and calls; see
[its board guide](../../boards/waveshare-epaper-397/README.md).

Paper uses BOOT for calls and conversation selection; see its board guide.
Its full-refresh display runs on a separate worker.

AI Passport uses OK to start/end a continuous call and holds OK for its conversation
menu. UP/DOWN adjust volume, or move through menus. It restores the selected
conversation for the same bound Agent; the first call selects the latest existing
conversation or creates one if none exist. Its ESP32-C3 profile requests Opus,
disables MQTT to reserve RAM for audio (HTTP heartbeat/configuration polling and
HTTP-granted WSS voice remain enabled; MQTT push/OTA commands are unavailable),
uses a raster display and no BLE, and shares one 40 KB codec task stack between
encoding and decoding. Playback pacing continues pumping microphone frames;
full uplink queues never block that shared worker. Queues use internal RAM when
PSRAM is unavailable. Incoming message storage starts at 2 KB, grows to at most
16 KB, and shrinks after delivery. Oversized messages fail the call instead of
being truncated. See the
[board guide](../../boards/ai-passport/README.md).

## Validation status

Device-key API checks on 2026-10-17 covered authentication, bound-Agent metadata,
conversation listing, an existing conversation's grant, verified TLS/WSS,
`session.ready` and grant release. Conversation creation failed with business
code 1002 (`agent_template_unavailable`). Deletion, real audio and complete
physical-device acceptance were not covered by those checks.

Relevant host checks are `firmware/tests/test_platform_api.py` and
`firmware/tests/test_stopwatch_controls.py`. Keep these distinct from device tests.

## Other grant sources

Authenticated MQTT can deliver a short-lived grant. `CONFIG_AGENT_LINK_REALTIME_DIRECT`
is a separate development path using a fixed session and private User API Key.
Both use the same worker; neither changes the normal activation/conversation
flow. Keep direct-test values in `sdkconfig.private`.
