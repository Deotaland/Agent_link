# Firmware

An ESP-IDF project with hardware drivers, applications and the
[Agent Link SDK](components/agent_link/README.md).

## Build and flash

Activate ESP-IDF 5.5.x or 6.1.x. Maintainers use 5.5.4 for existing S3/C3 profiles;
ESP32-S31 requires 6.1 with S31 support. From `firmware/`:

```sh
python tools/build_firmware.py --list
python tools/build_firmware.py korvo-ble --jobs 2
idf.py -p PORT flash monitor
```

Omit the profile name to choose from a menu. Each profile fixes the chip, board,
application and backend. [Profiles](profiles/README.md) lists the combinations.
Identify the hardware before flashing; ESP32-P4 is not adapted.

The tool reuses `build/`, reconfigures, compiles and merges the image. It verifies
that each merged segment matches the original build file. It never flashes.
A fresh checkout needs no saved config or build files; ESP-IDF restores managed
dependencies from the manifests and lock file. The first build needs network access.

| Output in `build/` | Use |
| --- | --- |
| `agent_link.bin` | Application image for OTA |
| `merged.bin` | USB full package; can overwrite saved settings |
| `agent_link.elf`, `agent_link.map` | Debugging this exact build |
| `flasher_args.json`, `flash_args` | Chip-specific files and flash offsets |

`idf.py flash` uses the separate build files and leaves NVS intact. A merged
image may overwrite WiFi, pairing and device identity. Check the package before
writing and explain its effect. Do not use a merged image as an OTA application.

## Configuration

```sh
python tools/build_firmware.py korvo-ble --configure-only
python tools/build_firmware.py korvo-ble --reset-config
python tools/build_firmware.py --check
```

Defaults load in this order: common, chip, profile fragments, private settings.
Keep supported credentials in ignored `sdkconfig.private`; do not commit or share
that file or an image containing its values. The first migration backs up the
previous config to `sdkconfig.before-profile` and imports recognized private
settings. `sdkconfig` is generated; see [profiles](profiles/README.md).

In `menuconfig`, select **Agent Link Device > Firmware profile**. The list is
limited to the active chip. Use the build tool to change chips or SDK versions;
it clears incompatible files in the existing build directory.

## Connect

- **BLE:** use the Deotaland phone app. The app provides the platform connection.
- **WiFi:** join the device hotspot, set WiFi at `http://192.168.4.1`, claim the
  six-digit activation code in the console and bind an Agent.
- **Muse:** pair and provision through the Muse app. See [Muse](components/muse_gadget/README.md).

The console and firmware center are at [bot.zhaojun.work](https://bot.zhaojun.work).
WiFi product identity and URL come from the application's `Platform()` settings.

## Source layout

| Path | Responsibility |
| --- | --- |
| `boards/<hardware>/` | Wiring and hardware initialization |
| `boards/common/` | Shared peripheral drivers |
| `apps/<app>/` | UI, audio workers, sessions and SDK callbacks |
| `apps/common/application.h` | Interface used by `main/app_main.cpp` |
| `profiles/` | Supported firmware combinations and defaults |
| `components/board_support/` | Drivers selected by the profile |
| `components/agent_link/` | BLE/WiFi/Muse transports, I/O, audio and OTA |

To add hardware, follow the [board guide](boards/README.md). For a standalone
ESP-IDF integration, see the [SDK guide](components/agent_link/README.md).

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No serial port | Data cable, correct USB connector, USB bridge driver |
| Cannot connect | Hold BOOT during reset, release, retry at 115200; check port ownership |
| Chip mismatch | Compare profile target, detected chip and package metadata |
| Old firmware boots | An OTA slot may still be selected; `idf.py flash` also writes initial otadata |
| Black screen | Board revision, panel driver and boot log; do not guess by reflashing |
| Missing `CONFIG_*` | Reconfigure with the selected profile |
| Platform error | WiFi, compiled URL, activation and Agent binding |

MIT; see [LICENSE](../LICENSE). Third-party components retain their own licenses.
