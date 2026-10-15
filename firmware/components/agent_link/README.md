# Agent Link component

This component connects a board to the Deotaland Agent platform. The board declares its
capabilities and supplies callbacks for hardware work; the component handles the link, protocol,
voice routing, device identity, and OTA. [`main/app_main.cpp`](../../main/app_main.cpp) wires the
selected board to the component.

## Where code belongs

| Location | Responsibility |
| --- | --- |
| `include/agent_link.h`, `agent_link_stream.h` | Lifecycle, status, callbacks, and media streams |
| `include/agent_link_io.h`, `agent_link_ota.h`, `agent_link_platform.h` | Device I/O, OTA, and platform conversation calls |
| `src/agent_link.cpp`, `protocol.cpp`, `audio_downlink.cpp` | Shared capability routing, control frames, and BLE audio decoding |
| `src/transport_ble.cpp`, `transport_wifi.cpp`, `transport_muse.cpp` | Transport adapters; no board hardware code |
| `src/cloud/cloud_session.cc`, `cloud_api.cc`, `cloud_credential.cc` | WiFi device claim, `auth_key`, and platform session |
| `src/cloud/cloud_mqtt.cc`, `cloud_json.cc` | MQTT connection and command/response bridge |
| `src/cloud/cloud_platform.cc`, `cloud_realtime.cc`, `realtime_audio.h` | User API calls, realtime WebSocket, and audio conversion |
| `boards/<board>/` | Pins, codec, display, controls, playback buffer, and board UI |

The board calls `agent_link_init()` and `agent_link_start()` through the shared app. Output
callbacks may run on transport or media tasks: queue slow hardware work and return promptly. Streaming input uses
`agent_link_stream_open/write/close`; callbacks receive PCM16 after any downlink decoding. Register
only callbacks covered by the board's capability bits.

## Links

- **BLE:** GATT control and L2CAP audio/image channels connect through the phone App.
- **WiFi:** the captive portal saves WiFi credentials; `/api/device-gateway/*` claims and
  authenticates the device; MQTT carries platform commands and replies. Korvo Cloud's touch UI can
  select an agent and conversation and start [realtime voice](REALTIME.md) directly.
- **Muse:** the Muse gadget transport handles its own pairing and cloud connection.

WiFi currently has no publisher for unsolicited SDK control events. Its MQTT bridge handles
command replies, including an explicit manifest fetch. Do not treat a ready WiFi link as proof that
BLE-style event notifications are available.

## Credentials

Keep WiFi passwords, platform user tokens, and direct-test API keys in private `sdkconfig` or NVS.
Do not add real credentials to source, defaults, examples, logs, or distributable firmware images.
The platform user token in `sdkconfig` is embedded in the built image; use a device-scoped
authorization method before distributing that configuration.

This project builds with ESP-IDF 5.5. The board and transport are selected at build time; see the
[firmware README](../../README.md) for the supported boards and build commands.
