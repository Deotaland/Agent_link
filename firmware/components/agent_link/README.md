# Agent Link SDK

An ESP-IDF component for BLE/WiFi connectivity, device I/O, audio streams and OTA.
Hardware is supplied through callbacks. The repository's
[app entrypoint](../../main/app_main.cpp) adapts one
[Application](../../apps/common/application.h) to this API.

## Public interfaces

| Header | Purpose |
| --- | --- |
| [agent_link.h](include/agent_link.h) | Lifecycle, capabilities, status and callbacks |
| [agent_link_stream.h](include/agent_link_stream.h) | Voice, audio and image input |
| [agent_link_io.h](include/agent_link_io.h) | Sensor/actuator descriptors, registration and readings |
| [agent_link_platform.h](include/agent_link_platform.h) | Bound Agent, conversations and realtime calls |
| [agent_link_ota.h](include/agent_link_ota.h) | Optional OTA progress and image validation |

Initialize with a persistent `agent_link_config_t` and callback data, then call
`agent_link_start()`. The repository does this in `main/app_main.cpp` after
constructing the application. Report only capabilities with working handlers.

Copy or queue callback data and return promptly. BLE callbacks may run on the
6 KB NimBLE host stack; hardware I/O belongs on workers. Audio output callbacks
receive PCM16 mono at 16 kHz. Tell the SDK the playback buffer capacity with
`agent_link_playback_set_buffer_ms()` and handle end/reset separately.

Input uses `agent_link_stream_open/write/close`. Use `AGENT_STREAM_VOICE` for
button voice turns and `AGENT_STREAM_AUDIO` for requested capture; handle errors
and disconnects. Use the dedicated platform APIs for WiFi conversations and
calls, as described in [realtime voice](REALTIME.md).

Register I/O with `agent_link_register_io()` before starting the link. Descriptors
and their strings need static or equivalent lifetime. Use `agent_link_push_reading()`
for typed sensor values and keep actuator callbacks short. Built-in capabilities
supply standard endpoints; register board-specific sensors/actuators explicitly.
See the [board guide](../../boards/README.md) for porting examples.

## Backends

| Backend | Implementation |
| --- | --- |
| BLE | Phone app; GATT control, voice notifications and L2CAP media/OTA |
| WiFi | Hotspot provisioning, device activation/authentication, MQTT commands and WebSocket realtime voice |
| Muse | Muse pairing/cloud adapter; requires companion components |

WiFi reads the current bound Agent from device authentication. Binding and Agent
selection happen in the console; devices manage that Agent's conversations.
WiFi has no publisher for unsolicited SDK control events. MQTT command replies
and explicit manifest queries work; a ready link does not imply BLE-style event
notifications. Video and hybrid transport enum values are not supported firmware
profiles.

WiFi joins have a 20-second limit including IP acquisition, then open the setup
hotspot. A failed portal attempt leaves it available for retry. Only successful
joins replace saved credentials. Changing WiFi preserves platform binding.

## Source layout

`src/core/` owns lifecycle, control frames and identity; `src/audio/` handles
playback accounting; `src/ota/` handles image writes. `src/transports/` holds BLE,
WiFi and Muse adapters. WiFi platform clients are under `wifi/cloud/`.
`disabled.cpp` supplies unsupported results for omitted backends. No codec or
panel drivers belong in the SDK.

## Use in another ESP-IDF project

Copy this component into the project's `components/`, add `agent_link` to its
requirements and select the backend in the top-level `CMakeLists.txt` before
`project()`:

```cmake
set(ENV{AGENT_LINK_BUILD_BACKEND} "BLE")  # BLE, WIFI or MUSE
```

Supply matching Kconfig settings and dependencies. Muse also needs this
repository's Muse components. Without explicit backend selection the legacy
multi-backend build requires all companion components. Use
[firmware profiles](../../profiles/README.md) as the supported integration examples.

The project supports ESP-IDF 5.5.x and 6.1.x. IDF 6 uses managed cJSON/MQTT and
PSA-based OTA hashing; the control protocol and digest format stay unchanged.

## Credentials

WiFi setup and platform device keys are stored in NVS. Put supported build-time
credentials in ignored `sdkconfig.private`. Never commit tokens/passwords or
share firmware containing them. Normal WiFi activation needs no account token;
direct realtime tests are a separate private configuration.

## Exclusive nearby mode

`agent_link_friends.h` provides queued start/stop, peer selection, mutual
confirmation, greeting and disconnect commands plus a copied status snapshot.
Start after the SDK WiFi backend is initialized; end application audio first.
The worker temporarily leases channel 6, suppresses cloud joins/downlinks and
returns the existing WiFi credential lifecycle on exit. It does not clear NVS or
restart cloud workers. The feature is opt-in and unused by other applications;
omitted WiFi backends return `ESP_ERR_NOT_SUPPORTED`.

Only peers in this protocol's discovery mode appear. Pairing requires comparing
a six-digit verification code and confirming on both devices. The implementation
uses ephemeral P-256 ECDH keys, fresh nonces, encrypted ESP-NOW unicast and
application HMACs. Never treat a successful send as delivery: greeting counters
advance on acknowledgement, with bounded retries and duplicate suppression.
Read snapshots from the application worker and perform display work there.
