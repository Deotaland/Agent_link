# Hardware and applications

`boards/` holds wiring, drivers and the board catalog. User workflows live in `apps/`.
One firmware profile selects them together; unsupported board/transport combinations are not offered.

```text
profiles/profiles.json        app, hardware, drivers, UI, backend and defaults
boards/<board>/config.json   hardware identity and supported firmware metadata
boards/<board>/config.h      board/application settings
boards/common/              reusable peripheral drivers
boards/korvo/               shared Korvo wiring and hardware initialization
apps/<app>/                 UI, sessions, recording and SDK callbacks
apps/common/application.h   callback adapter used by main/app_main.cpp
```

Korvo BLE and WiFi share one `KorvoHardware` instance per application. The camera creates the I2C bus;
touch and audio attach afterwards. Each application owns its playback queue and workers. SD-card
and servo drivers are selected separately. Other ports retain their initialization inside the
application; extract it into `boards/` when sharing or changing the hardware.

## Application interface

Derive from `Application` and end the implementation with `DECLARE_APPLICATION(MyApplication)`.
`Name()`, `Model()` and `Capabilities()` declare device identity and available I/O. Keep the existing
`Model()` string when migrating a device: OTA checks it against the installed firmware.

| Operation | Application hook |
| --- | --- |
| Audio output and end/reset | `PlayAudio`, `AudioEnd`, `AudioReset` |
| Listening request | `OnListen` |
| Reply state | `OnVoiceReply` |
| Text, vibration, LED | `ShowText`, `Vibrate`, `SetLed` |
| Link progress | `OnLinkStatus`, `OnLinkState` |
| Product identity for direct platform access | `Platform` |
| Additional control command | `OnCommand` |
| Battery | `GetBatteryLevel`, `IsCharging` |

Call SDK stream and I/O APIs directly for input. Shared hardware drivers never depend on Agent Link.
SDK callbacks can run on the 6 KB NimBLE host stack: copy data or signal a worker, then return.
Advertise only capabilities that work. `main/board.h` retains `Board` and `DECLARE_BOARD` aliases
for existing external ports.

## Adding hardware or a firmware profile

1. Add `boards/<board>/config.h` and `config.json`; use a current entry as the catalog template.
   The catalog records chip, model, vendor, identification, flash port, capabilities and supported link.
2. Put the application in `apps/<app>/`. Add reusable drivers to `boards/common/`, hardware-specific
   drivers to `boards/<hardware>/`. Do not compile all common drivers with a source glob.
3. Add a choice in `main/Kconfig.profiles` and derive its board/backend in `main/Kconfig.projbuild`.
   Keep old `BOARD_TYPE_*` symbols when migrating existing hardware.
4. Add a dedicated `.defaults` and an entry in `profiles/profiles.json`. List the required drivers and
   hardware sources; use the BLE, network and UI fragments only where needed.
5. If a driver requires a new component, add it to `components/board_support/CMakeLists.txt`.
   Dependency decisions use the profile manifest, available during IDF's early expansion; do not
   use `CONFIG_*` to decide `REQUIRES`. Register managed versions in its `idf_component.yml`.
6. Run `python tools/build_firmware.py --check`, build the selected profile, and test it on hardware.
   From the repository root run `python tools/board_catalog.py` and `--check` before committing.

## Configuration and credentials

Use `python tools/build_firmware.py` for the selection menu, or the firmware profile choice in the
SDK configuration editor. Reuse `build/`. The tool validates the chip and profile, loads the selected
defaults and keeps private settings in the ignored `sdkconfig.private` file. Never add credentials to
source, public defaults or catalog entries. See [profiles](../profiles/README.md) for migration details.
