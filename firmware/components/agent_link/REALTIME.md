# Agent Stack realtime grant on WiFi

By default the WiFi SDK accepts short-lived Agent Stack grants through its authenticated MQTT
downlink. The board stays transport-neutral. In that mode the long-lived User API Key remains on
the application backend. Direct HTTP testing is an explicit alternative configured below.

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

On Korvo Cloud, swipe to Apps and tap **Voice**. The device publishes this event on its MQTT `up`
topic:

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

The implementation follows `test-mic.html`. `session.connect` includes
`"endpointing":{"mode":"client"}`. The link becomes `READY` only after `session.ready`;
MQTT alone leaves it `CONNECTED`.

On Korvo Cloud, open **Apps → Voice**, wait for the connection, then hold **Hold to talk** and
release it to send the turn. **End call** or leaving the Voice screen sends `session.end`, closes
WSS, stops capture, and clears queued speech. **Connect** requests a fresh grant after a failure.
The grant request times out after 30 seconds. Late grants after hangup are rejected.

The SDK maps `AGENT_STREAM_VOICE` (and a single `AGENT_STREAM_AUDIO`) to these ordered text frames:

```json
{"type":"input_audio.activity","kind":"start"}
{"type":"input_audio.append","audio":"<base64 PCM16LE, 16 kHz, mono>"}
{"type":"input_audio.activity","kind":"end"}
```

Microphone writes only enqueue data; TLS runs on the realtime worker. The bounded queue holds about
two seconds of audio and reserves space for turn control. An upload failure aborts the call rather
than sending a partial utterance. Only one microphone stream can be open at a time. Other media
stream kinds remain unsupported on this WiFi backend.

Replies can be binary messages or `response.audio.delta` with Base64 audio. PCM16LE replies are
24 kHz mono; Opus replies contain one raw packet per message. The format is taken from the grant
and confirmed from `session.ready` when it includes audio settings. Compressed packets retain
their boundaries across TCP reads and WebSocket continuation frames. Espressif's software Opus
decoder produces PCM16 mono at 16 kHz directly. PCM replies use continuous 24-to-16 kHz resampling.
Both paths pace output to the advertised playback buffer.
`response.audio.done` ends playback input; `transcript.partial` and `transcript.final` reach
`ShowText`. The bundled Korvo Cloud font has Latin glyphs only. English text and common typographic
punctuation are displayed using ASCII glyphs; unsupported text uses an English fallback. No Chinese
font is bundled. Starting a new turn, interruption,
hangup, and connection loss clear pending playback through the optional `on_audio_reset` callback.
The current UI uses client push-to-talk; server VAD, camera streaming and acoustic echo cancellation
are not included in this integration.

Ping/pong and a 45-second receive timeout detect a dead connection. A consumed connect token is
never reused: reconnect through **Connect** so the backend issues a fresh one. The backend grant
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

