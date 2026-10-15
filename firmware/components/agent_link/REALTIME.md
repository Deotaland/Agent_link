# Agent Stack realtime grant on WiFi

Korvo Cloud can manage voice conversations from its touch screen when
`CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN` is set in the private `sdkconfig`. After WiFi setup and
six-digit device claim, open **Apps → Voice → Settings**. The UI fetches `GET /api/agents`, shows agents that
support `realtime_voice`, and binds the selected agent with
`POST /api/devices/{device_id}/bind-agent`. It finds the device ID by querying `/api/devices` for
its own device SN and requires an exact SN match. The next screen lists
`GET /api/agents/{agent_id}/actions/realtime_voice/conversations` and creates a voice session with
`POST` to the same path. The selected agent and conversation are saved on the device. Opening Voice
with a saved selection immediately sends
`POST /api/agents/{agent_id}/actions/realtime_voice/realtime-grant` with that conversation's
`conversation_id` as `session_id`. The grant's `websocket_url` and `connect_token` go straight to
the existing WSS worker. **New session** creates and selects another conversation, then connects
to it. **Settings** changes the agent or conversation. On the conversation list, tap **Del** and
then **OK?** within five seconds to delete a conversation with
`DELETE /api/agents/{agent_id}/actions/realtime_voice/conversations/{conversation_id}`.
Deleting the selected conversation also clears the device's saved selection. Ending the call closes WSS and sends
`DELETE /api/agents/{agent_id}/actions/realtime_voice/realtime-grant/{session_id}`. All platform
user requests carry `X-Token` and `Authorization: Bearer`; the credential and grant are never logged.
The UI has no text messaging or message-history page. Korvo Cloud enables the SimSun 14 CJK font
for Chinese agent and session names.

This user token is embedded in the development firmware. Keep `sdkconfig`, binaries and full-flash
images private. A distributable product needs a device-scoped credential for these user APIs.

Without that token, the WiFi SDK accepts short-lived Agent Stack grants through its authenticated
MQTT downlink. In that mode the long-lived User API Key remains on the application backend.
Direct HTTP testing is another explicit alternative configured below.

For device-side testing, `CONFIG_AGENT_LINK_REALTIME_DIRECT=y` selects direct HTTP grant requests.
Configure `CONFIG_AGENT_LINK_REALTIME_BASE_URL`, `CONFIG_AGENT_LINK_REALTIME_SESSION_ID`, and
`CONFIG_AGENT_LINK_REALTIME_USER_API_KEY` in the private sdkconfig used by the build. Leave these
strings empty in source and defaults. The resulting firmware contains the key and must stay private.

In direct mode, opening Voice queues `POST /api/v1/user/sessions/{sessionId}/realtime` with Bearer
authorization. `CONFIG_AGENT_LINK_REALTIME_OPUS` defaults on in this mode: the request selects
PCM16 mono 16 kHz input and Opus mono 24 kHz output in 20 ms packets. Disable it to request the
default PCM output. HTTP 201 supplies the grant to the same WSS/audio worker described below.
HTTP 409 first checks the existing connection's audio format with `GET /realtime`, then requests
`/realtime/connect-tokens` to recover the fixed test session. This mode does not
wait for an MQTT grant. The HTTP client closes before WSS starts; ending the call also issues
`DELETE /realtime` so the fixed session can be used for the next test. HTTP status codes are logged,
but credentials and response bodies are not printed.

In the legacy MQTT flow, requesting realtime publishes this event on its MQTT `up` topic:

```json
{
  "msg_id": "msg_1791880000000_123abc",
  "type": "event",
  "action": "realtime_request",
  "timestamp": 1791880000000,
  "params": {},
  "reply_to": null,
  "code": null,
  "message": null
}
```

The backend must consume this event, resolve the user's selected agent/session, and request a
realtime grant (`POST /api/v1/user/sessions/{sessionId}/realtime`) with its User API Key. If those
objects do not exist yet, it first associates a published template with the user
(`POST /api/v1/user/agents`) and creates a session (`POST /api/v1/user/sessions`). It then publishes
the returned `websocketUrl` and `connectToken` to that user's bound device on its MQTT `down` topic.
The downlink uses the existing device command envelope:

```json
{
  "msg_id": "msg_1791880000000_123abc",
  "type": "request",
  "action": "realtime_grant",
  "timestamp": 1791880000000,
  "params": {
    "websocketUrl": "wss://example.com/api/v1/user/sessions/sess_example/realtime/ws",
    "connectToken": "rtct_example"
  },
  "reply_to": null,
  "code": null,
  "message": null
}
```

Publish promptly: the token is valid for 60 seconds and can be used once. The device's
`realtime_grant_status` response means the grant was queued, not that Agent Stack accepted the
WebSocket handshake. Do not retain this MQTT message. The device validates `wss://`, verifies the
server certificate, negotiates `tidb-link.realtime.v1`, and sends
`{"type":"session.connect","connect_token":"..."}` as the first text frame. It does not print
the token.

## Voice conversation

The implementation follows `test-mic.html`. Platform conversations send
`"endpointing":{"mode":"server"}`; legacy MQTT and direct sessions continue to use `client`.
The link becomes `READY` only after `session.ready`;
MQTT alone leaves it `CONNECTED`.

On Korvo Cloud, choose an agent and conversation in Settings once. Tapping Voice connects the
selected conversation and opens the microphone continuously as soon as `session.ready` arrives.
Server endpointing decides turn boundaries. Microphone upload continues while replies play; the
firmware sends only `input_audio.append`, without `input_audio.activity` messages. **End call** or
leaving the Voice screen sends `session.end`, closes WSS, stops capture, and clears queued speech.
**Retry** requests a fresh grant after a failure.
The grant request times out after 30 seconds. Late grants after hangup are rejected.

The SDK maps `AGENT_STREAM_VOICE` (and a single `AGENT_STREAM_AUDIO`) to these ordered text frames:

```json
{"type":"input_audio.activity","kind":"start"}
{"type":"input_audio.append","audio":"<base64 PCM16LE, 16 kHz, mono>"}
{"type":"input_audio.activity","kind":"end"}
```

Microphone writes only enqueue data; TLS runs on the realtime worker. The bounded queue holds about
two seconds of audio and reserves space for turn control. In a platform conversation, a full queue
drops the newest microphone frame so capture stays live; client PTT still aborts a truncated turn.
The platform worker combines up to four queued 20 ms PCM chunks into one WebSocket message to
reduce TLS write overhead, without waiting for more chunks. Five-second uplink logs report captured,
sent, and dropped PCM, WebSocket message count, queue depth, send timing, and microphone stack margin.
Only one microphone stream can be open at a time. Other media
stream kinds remain unsupported on this WiFi backend.

Replies can be binary messages or `response.audio.delta` with Base64 audio. PCM16LE replies are
24 kHz mono; Opus replies contain one raw packet per message. The format is taken from the grant
and confirmed from `session.ready` when it includes audio settings. Compressed packets retain
their boundaries across TCP reads and WebSocket continuation frames. Espressif's software Opus
decoder produces PCM16 mono at 16 kHz directly. PCM replies use continuous 24-to-16 kHz resampling.
Both paths pace output to the advertised playback buffer.
`response.audio.done` ends playback input. Korvo Cloud's voice UI does not display transcripts.
Starting a client PTT turn, interruption,
hangup, and connection loss clear pending playback through the optional `on_audio_reset` callback.
Server endpointing permits full-duplex playback and capture; acoustic echo cancellation is not
included in this integration.

Ping/pong and a 45-second receive timeout detect a dead connection. A consumed connect token is
never reused: reconnect through **Retry** so the backend issues a fresh one. The backend grant
handler described above is required; firmware cannot obtain a grant from the device credential.

Korvo Cloud waits for end-of-audio on replies shorter than six seconds. Longer replies start
after six seconds are buffered; sustained gaps use the same threshold before resuming. This
favors uninterrupted speech over time to first sound. The threshold is reduced for smaller
buffers and kept below the transport's pacing watermark, so a long reply can start before the
sender fills the buffer. End-of-audio flushes shorter replies and tails. This board's WiFi build uses a 32 KB TCP receive window and a
26-entry receive mailbox. WiFi power saving is suspended during WSS voice and restored when the
connection closes. `reply audio` logs compressed bytes, decoded duration and maximum packet gap;
`speaker drained` logs playback duration, underruns and dropped bytes.
`reply processing` records maximum decoder time, PCM callback time and transport pacing wait.

## Host validation

`tests/realtime_audio_test.cpp` tests the production resampler and message assembler with random
byte boundaries, all continuation boundaries, buffer limits, interrupted sample carry, and a
one-second 1 kHz signal (24,000 input samples → 16,000 output samples, unchanged pitch).
Compile and run it with any C++17 host compiler, for example:

```sh
c++ -std=c++17 -Wall -Wextra -Werror tests/realtime_audio_test.cpp -o realtime_audio_test
./realtime_audio_test
```

