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
mask per frame. S3/S31 TLS writes use a 3000 ms transport timeout. Slow successful
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
3-second stall deadline. On S3/S31 a write can still block after the readiness check.
On C3, TLS remains nonblocking after verified connection and HTTP upgrade.
Partial writes retain the exact masked buffer and retry only its remaining
suffix on later worker passes, with a 3-second deadline. Reads and codec work
continue during backpressure. Partial frame headers/payloads are retained too,
including control frames interleaved between message fragments. The upgrade
validates HTTP 101, Upgrade/Connection, the accept digest and the subprotocol;
unnegotiated extensions and masked server frames are rejected.
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
uses a raster display and no BLE. One resident 40 KB worker is created at SDK
startup and owns network I/O plus Opus encoding/decoding. Its stack is retained
between calls, replacing the separate 12 KB network and 40 KB codec stacks.
The board's capture/playback workers remain independent. Playback pacing also
pumps microphone frames and pending TLS writes; full uplink queues never block
the shared worker. After the verified WSS upgrade, continuous Opus calls reserve
one 25 KB workspace. C3 overrides the codec library's weak module allocator only
for that voice worker; other tasks use their normal allocator. Both wrappers and
core states reuse the reservation, so reopening a codec never needs a new large
system-heap block. Failure before a handle is published rolls back the allocator
transaction. The reservation is freed at call teardown.
Continuous C3 Opus calls follow the application's half-duplex policy: listening
retains the encoder, replies retain the decoder, and transitions close one before
opening the other. During a reply, microphone frames upload cached valid Opus
silence to preserve endpointing cadence. Interruption/reply completion restores
the encoder. Actual queued PCM feedback controls the 75% playback watermark,
including prebuffer/rebuffer pauses and asynchronous queue resets. Boards without
feedback retain clock-based pacing. Capture/playback tasks remain resident. Encoded queues hold two
packets with two-packet batching; the decoder's PCM buffer matches the negotiated
20 ms duration. TLS uses 1024-byte outbound records and preserves the standard
16384-byte incoming record limit. With dynamic TLS buffers enabled, C3 selects
the SDK's `ESP_TLS_DYN_BUF_RX_STATIC` strategy: the handshake stays dynamic, then
one full RX buffer is reserved before Opus and reused until disconnect. On IDF
5.5.4 the linked allocation is 17058 bytes including the SDK header. The 16 KB
message buffer is also reserved after upgrade, before codec setup, and retained
through all replies. Ordinary audio-delta objects are validated without allocation
and Base64 is decoded in the original writable message, including ASCII JSON
escapes. Other events and unsupported fast-path forms keep the cJSON path.
The AI Passport profile disables both general and RX WiFi IRAM acceleration;
the measured link layout reclaims 11776 additional SRAM bytes from RX acceleration.
This trades bulk throughput for receive/codec headroom; scheduling still needs
physical acceptance. Per-call state is released with the connection. No large task stack is allocated
mid-call. A C3 memory lock excludes other HTTPS requests
through voice teardown; periodic HTTP heartbeat/configuration polling defers
while a call is active, then resumes with overdue deadlines. Platform binding
changes are refreshed after the call ends.
Queues use internal RAM when
PSRAM is unavailable. C3 uses 2 KB message storage during HTTP upgrade and reserves
the full 16 KB before enabling audio, with no playback-time growth/shrink cycle.
Other targets retain their existing dynamic message storage. Oversized messages fail the call instead of
being truncated. See the
[board guide](../../boards/ai-passport/README.md).

## Validation status

Device-key API checks on 2026-10-17 covered authentication, bound-Agent metadata,
conversation listing, an existing conversation's grant, verified TLS/WSS,
`session.ready` and grant release. Conversation creation failed with business
code 1002 (`agent_template_unavailable`). Deletion, real audio and complete
physical-device acceptance were not covered by those checks.

Relevant host checks are `firmware/tests/test_platform_api.py` (including
`--c3 --voice-worker` and `--c3 --voice-memory`), `firmware/tests/test_realtime_recovery.py`,
`firmware/tests/test_tls_receive_memory.py --idf-path <IDF>` (actual SDK RX allocation
functions with a bounded allocator; decryption/networking are not simulated),
`firmware/tests/test_passport_wifi.py` and `firmware/tests/test_stopwatch_controls.py`.
Keep these distinct from device tests. The resident C3 task and nonblocking
transport require physical acceptance for long calls, jitter, reconnects,
playback underruns and worst-case stack/heap headroom.
AI Passport's Chinese raster font is derived from the common font image and
embedded in its application; it uses no separate flash partition or TTF cache.

## Other grant sources

Authenticated MQTT can deliver a short-lived grant. `CONFIG_AGENT_LINK_REALTIME_DIRECT`
is a separate development path using a fixed session and private User API Key.
Both use the same worker; neither changes the normal activation/conversation
flow. Keep direct-test values in `sdkconfig.private`.
