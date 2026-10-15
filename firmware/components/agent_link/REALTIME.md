# WiFi realtime voice

Korvo Cloud's Voice screen uses the selected agent and conversation. It opens the microphone when
`session.ready` arrives and keeps it open until the user leaves Voice or ends the call. There is no
push-to-talk or text chat in this screen.

## Credentials and API calls

The device's `auth_key` authenticates `/api/device-gateway/*` and obtains its MQTT token. It does
not authorize the user-facing agent and conversation APIs. For on-device selection, the development
build reads `CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN` from private `sdkconfig` and sends it in both
`Authorization: Bearer` and `X-Token`. Do not put this token in source, defaults, logs, or a shared
firmware image. A distributable build needs a device-scoped way to authorize these calls.

The touch-screen flow is:

| Action | Platform request |
| --- | --- |
| List voice agents | `GET /api/agents?page=…&page_size=20`; keep agents supporting `realtime_voice` |
| Bind an agent | `GET /api/devices?keyword=<device SN>&page=1&page_size=20`, verify an exact SN match, then `POST /api/devices/{device_id}/bind-agent` |
| List conversations | `GET /api/agents/{agent_id}/actions/realtime_voice/conversations` |
| Create a conversation | `POST` to the same conversations path |
| Delete a conversation | `DELETE /api/agents/{agent_id}/actions/realtime_voice/conversations/{conversation_id}` |
| Start a call | `POST /api/agents/{agent_id}/actions/realtime_voice/realtime-grant` with `session_id` |
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

`cloud_platform.cc` owns the user API calls. `cloud_realtime.cc` owns grants, WebSocket I/O, and
audio conversion. `transport_wifi.cpp` exposes that worker to the SDK; the Korvo board owns mic,
speaker, and UI. The component does not access the codec or display.

After `session.ready`, the board sends PCM16LE, 16 kHz, mono microphone chunks as
`input_audio.append`. Platform conversations use server endpointing, so the microphone remains live
while replies play. The worker batches up to four queued 20 ms chunks per WebSocket message without
waiting for more data. Its bounded queue drops new mic chunks when WiFi cannot keep up.

Replies arrive as binary audio or `response.audio.delta`. The negotiated output is 24 kHz mono
PCM16 or Opus; the worker converts it to 16 kHz PCM16 for the board. `response.audio.done` marks the
end of this reply. Korvo Cloud holds playback PCM in PSRAM and starts after about 500 ms is queued;
after an underrun it waits for about 1000 ms. End-of-reply releases a shorter tail immediately.
The playback buffer cannot make up for a sustained shortage of network throughput. Acoustic echo
cancellation is not implemented.

Every five seconds during a continuous call, one `uplink` line reports captured, sent, and dropped
frames, queue depth, WebSocket send time, receive activity, and task headroom. Reply and speaker
summaries show duration, packet gaps, underruns, and dropped playback
bytes. These counters contain no audio or credentials.

## Other grant sources

Without a platform user token, the backend may deliver a short-lived grant over the authenticated
MQTT downlink. `CONFIG_AGENT_LINK_REALTIME_DIRECT` is a separate development path using a fixed
Agent Stack session and a private User API Key. Both feed the same WebSocket worker; neither is the
touch-screen conversation flow. Keep either credential in private `sdkconfig`.

The host-side resampler and WebSocket assembler checks are in
[`tests/realtime_audio_test.cpp`](tests/realtime_audio_test.cpp).
