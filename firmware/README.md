# Agent Link firmware

**English** | [简体中文](README.zh-CN.md)

Agent Link connects embedded devices to the Deotaland Agent platform. The reusable ESP-IDF
component is [`components/agent_link`](components/agent_link/README.md); this project supplies
board drivers, applications and supported firmware configurations.

## Build

Use ESP-IDF 5.5.x (validated with 5.5.4) or 6.1.x. Activate one environment, then run from this directory:

A fresh checkout needs no existing `build/`, `sdkconfig`, private configuration or test files.
The build tool generates the selected configuration; ESP-IDF downloads the managed dependencies
using the committed manifests and `dependencies.lock`.

```powershell
python tools/build_firmware.py
```

A menu lists the supported profiles; choose a number without typing a profile name. In the SDK
configuration editor or `menuconfig`, use **Agent Link Device → Firmware profile**, save, then build.
Available choices depend on the current target chip. Use the terminal menu to change chips.
Automated builds can still specify a name, such as `python tools/build_firmware.py korvo-ble`.

A firmware profile fixes the board, application, target chip and backend together. For Korvo,
`korvo-ble` provides the phone App firmware; `korvo-wifi` provides the platform UI and realtime
voice. Only combinations listed by the tool are supported. ESP32-P4 is not adapted.

The tool reuses `build/`, runs two compiler jobs, and produces `agent_link.bin`, `merged.bin`,
and matching debugging artifacts. It does not flash. Once configured, the IDE can continue to
build the same profile.

To change SDK versions, activate the other ESP-IDF environment and run the same build command.
The tool detects the SDK checkout/version, clears the old `build/` cache and regenerates the
profile configuration. Private settings remain in `sdkconfig.private`; neither SDK installation
is modified. Before an IDE build after switching SDKs, run the tool once.
The 5.5 build uses `dependencies.lock`; 6.1 uses `dependencies.idf6.lock` so the two
component managers retain their own lock formats.

Dual-version build validation covers `korvo-ble` (ESP32-S3). Other profiles have not
yet completed individual 6.1 build and device validation.

```powershell
python tools/build_firmware.py korvo-ble --configure-only
python tools/build_firmware.py korvo-ble --reset-config
python tools/build_firmware.py --check
```

Common defaults, chip defaults, backend defaults and the profile's own defaults are applied in
that order. Local credentials are loaded last from gitignored `sdkconfig.private`. The first
migration backs up the original `sdkconfig` to `sdkconfig.before-profile` and imports known
credentials and related options. Edit credentials in `sdkconfig.private`; do not commit it or
distribute an image containing them.

Saving a new profile applies its defaults on the next build and regenerates `sdkconfig`. The
terminal tool clears the existing build cache only when changing chips. Unchanged profiles build
incrementally. CMake delegates profile changes to the configuration tool and validates the result;
it does not append settings. See [firmware profiles](profiles/README.md) for details.

## Connection backends

| Backend | Connection                                                                 |
| ------- | -------------------------------------------------------------------------- |
| BLE     | GATT control and L2CAP audio/image channels through the Rorolee App        |
| WiFi    | Hotspot setup, platform activation, MQTT commands and realtime voice       |
| Muse    | BLE pairing through the Muse app, followed by WiFi access to Muse services |

The SDK reports connection status through `agent_link_status_t`. Hardware callbacks receive
PCM audio, text or peripheral commands. Backend support in the SDK does not imply that every
application has been adapted to every backend.

## Layout

```text
firmware/
  profiles/                  supported combinations and short sdkconfig fragments
  tools/build_firmware.py    configure, validate, build and merge one profile
  cmake/                    build integration
  main/                     app_main and profile selection menu
  apps/<app>/               UI, voice sessions, recording and SDK callbacks
  apps/common/              Application interface
  boards/<board>/           wiring, hardware initialization and catalog
  boards/common/            shared display, codec, camera and peripheral drivers
  components/board_support/ selected hardware drivers
  components/ui_lvgl/       optional LVGL integration
  components/agent_link/    core, audio, OTA and selected transport
  components/muse_gadget/   vendored Muse integration
```

`profiles/profiles.json` selects the application, hardware, drivers, UI and transport together.
Korvo BLE and WiFi share `boards/korvo/`; each keeps its own UI and audio workflow under `apps/`.
The SDK compiles the selected transport and keeps disabled APIs available with an unsupported result.
Public SDK interfaces, device models and flash partition layouts are preserved.

To add hardware, follow [boards/README.md](boards/README.md), then add a profile and validate
the board catalog. Code shared by multiple boards belongs in `boards/common/`.

## Using the component

Copy `components/agent_link/` into an ESP-IDF project's `components/`, add `agent_link` to its
dependencies, select the backend before `project()` (see the component README), and register capabilities and output callbacks through `agent_link.h`.
Microphone input uses `agent_link_stream_open/write/close`; hardware output arrives in callbacks.
Queue slow hardware work because callbacks may run on transport tasks.

See the [component README](components/agent_link/README.md) and
[realtime voice notes](components/agent_link/REALTIME.md) for API behavior and credentials.

## License

MIT; see [LICENSE](../LICENSE). Vendored components retain their own licenses.
