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
| `src/core/` | Lifecycle, capabilities, control protocol and device identity |
| `src/audio/` | PCM/ADPCM downlink, playback accounting and flow control |
| `src/ota/` | Firmware receive, validation and boot-slot selection |
| `src/transports/ble/` | GATT control, L2CAP data and bounded voice queue |
| `src/transports/wifi/` | Station, provisioning and platform clients under `cloud/` |
| `src/transports/muse/` | Muse adapter |
| `src/transports/disabled.cpp` | Unsupported implementations for omitted backends |
| `apps/<app>/` | UI, voice/recording workflows and hardware callbacks |
| `boards/` | Pins, hardware bring-up and shared drivers |

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

WiFi joins and reconnects have a 20-second limit, including the wait for an IP address. On timeout,
the device opens the setup hotspot; a failed attempt from the setup page leaves it open for retry.
Only a successful join replaces the saved credentials. Changing WiFi keeps the platform binding.

## Credentials

Keep WiFi passwords, platform user tokens, and direct-test API keys in `sdkconfig.private` or NVS.
Do not add real credentials to source, defaults, examples, logs, or distributable firmware images.
The platform user token in `sdkconfig` is embedded in the built image; use a device-scoped
authorization method before distributing that configuration.

This project builds with ESP-IDF 5.5. The board and transport are selected at build time; see the
[firmware profiles](../../profiles/README.md) for the supported combinations and build commands.

## Backend selection in another ESP-IDF project

This firmware passes the profile backend before dependency expansion. A project that copies
this component can choose a backend in its top-level `CMakeLists.txt`, before `project()`:

```cmake
set(ENV{AGENT_LINK_BUILD_BACKEND} "BLE")  # BLE, WIFI or MUSE
```

The BLE API and wire protocol are unchanged. Selecting WiFi adds the platform, provisioning
and Opus clients; Muse also needs the repository's Muse components. Calls to an omitted backend
return `ESP_ERR_NOT_SUPPORTED`. Without a backend selection, the component keeps its legacy
multi-backend build; that requires the same companion components as before.
