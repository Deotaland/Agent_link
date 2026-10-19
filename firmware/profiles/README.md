# Firmware profiles

A profile selects the chip, hardware, application, drivers, UI and backend. Run
these commands from `firmware/` in an activated ESP-IDF environment:

```sh
python tools/build_firmware.py --list
python tools/build_firmware.py
python tools/build_firmware.py korvo-wifi --jobs 2
```

| Hardware | Profile | Backend |
| --- | --- | --- |
| Korvo 2 V3 | `korvo-ble`, `korvo-wifi` | BLE / WiFi |
| RoRoLee RRL-01 | `rorolee-ble`, `work-badge-ble` | BLE |
| RoRoLee RRL-01 | `rorolee-muse` | Muse |
| WOWNNY | `wownny-muse`, `wownny-old-muse` | Muse; choose the correct revision |
| FoloToy AI Passport | `ai-passport-ble` | BLE; ESP32-C3 |
| M5Stack StopWatch C152 | `m5stack-stopwatch-ble`, `m5stack-stopwatch-wifi` | BLE / WiFi |
| OpenJumper AIoT Basic V2 | `gc2145-camera-ble` | BLE |
| ESP32-S3 + ES8311 wiring examples | `es8311-asr-ble`, `es8311-voice-ble` | BLE |
| Mozhifang paper V2 | `paper-s31-wifi` | WiFi; ESP32-S31 |

S3/C3 profiles support ESP-IDF 5.5.x and 6.1.x. S31 needs an S31-capable 6.1 SDK.
ESP32-P4 has no adapted profile. A backend in the SDK does not imply that every
application supports it.

## Configuration

`profiles.json` is read before IDF dependency expansion. It records:

| Field | Meaning |
| --- | --- |
| `board`, `board_type`, `target` | Catalog/config directory, Kconfig identity and chip |
| `app` | Application under `apps/` |
| `hardware`, `hardware_sources` | Hardware directory and its compiled source files |
| `drivers` | Selected files under `boards/common/` |
| `ui` | `lvgl`, `raster` or `none` |
| `backend` | `BLE`, `WIFI`, `MUSE` or `NONE` |
| `defaults` | Ordered sdkconfig fragments |

Common defaults and chip defaults load first; the profile's fragments follow.
Private settings load last from ignored `sdkconfig.private`. Only recognized
credentials and direct-test settings are accepted there, not board or memory
settings; the allowed keys are in `tools/build_firmware.py`.

The first migration saves `sdkconfig.before-profile` once and imports recognized
private settings. A profile/defaults/SDK change regenerates `sdkconfig`; other
local options may need reapplying. The same profile builds incrementally. Chip
or SDK changes clear the existing build cache. S3 profiles use PSRAM; AI Passport
uses C3 internal RAM and smaller buffers.

```sh
python tools/build_firmware.py korvo-ble --configure-only
python tools/build_firmware.py korvo-ble --reset-config
python tools/build_firmware.py --check
```

The build tool runs two compiler jobs by default. Outputs stay in `build/`; no
flash is performed. Keep images containing private settings local.

## Add a profile

Follow the [board guide](../boards/README.md). Add the manifest entry, defaults,
Kconfig choice and board/backend mapping together. Regenerate the board catalog,
validate metadata, build the selected profile and record hardware acceptance.
