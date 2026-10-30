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
| FoloToy AI Passport | `ai-passport-ble`, `ai-passport-wifi` | BLE / WiFi; ESP32-C3 |
| M5Stack StopWatch C152 | `m5stack-stopwatch-ble`, `m5stack-stopwatch-wifi` | BLE / WiFi |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | `waveshare-amoled-175-wifi`, `waveshare-amoled-175-touch-test` | WiFi / offline touch diagnostics |
| OpenJumper AIoT Basic V2 | `gc2145-camera-ble` | BLE |
| ESP32-S3 + ES8311 wiring examples | `es8311-asr-ble`, `es8311-voice-ble` | BLE |
| Waveshare ESP32-S3-ePaper-3.97 | `waveshare-epaper-397-wifi` | WiFi; ESP32-S3 |
| Mozhifang paper V2 | `paper-s31-wifi` | WiFi; ESP32-S31 |

S3/C3 profiles support ESP-IDF 5.5.x and 6.1.x. S31 needs an S31-capable 6.1 SDK.
ESP32-P4 has no adapted profile. A backend in the SDK does not imply that every
application supports it.

## Configuration

Platform products are registered in `products.json`: the `products` section stores
the platform's IDs, names and chip types; `profiles` maps firmware profiles to
product keys. The catalog is specific to the origin in `platform`, which must match
`boards/common/platform_config.h`. CMake generates `build/generated/platform_product.h`
for the selected profile; do not edit the generated file or put product IDs in
board headers, Kconfig or defaults.

For `https://bot.deotaland.cn`, Korvo WiFi uses `korvo-s3` (6), Korvo BLE uses
`korvo-s3-ble` (7), StopWatch uses 4, Waveshare ePaper 3.97 uses 5 and AI Passport
uses 1. Paper-S31 uses 8, ROROLEE uses 2 and WOWNNY uses 3. Muse uses a separate
backend; recording a product in the catalog does not change its Muse identity or
connection flow.

Before adding a board or changing platform origins, query the platform product
catalog with `python skills/agent-link-port/scripts/find_product.py --list --json`
from the repository root, then record the verified product key and ID here.
The tool needs an account token in `DEOTALAND_TOKEN`; never put it in firmware.
Normal builds and device startup use the checked-in mapping without a product-list
request. Validation rejects duplicate IDs, unknown mappings and wrong-chip products.
An unregistered WiFi profile must use a null mapping and cannot build until its
product is registered. IDs from another platform must not be reused.

Startup logs include the platform origin, firmware model, product ID and key.
WiFi API errors include HTTP status, business code and message. Request bodies,
response data and auth keys are not logged; echoed device keys in messages are redacted.

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
| `flash_assets` | Optional data images with partition labels and firmware-relative source files; included in USB flash and merged images |

Common defaults and chip defaults load first; the profile's fragments follow.
Common defaults contain software policy only. Chip defaults are reserved for
chip-wide invariants, not a particular module or board. Each named profile declares
its flash capacity and partition table. PSRAM presence/mode/speed, board clock
choices and console routing also belong to that profile. BLE/network fragments
may share buffer and allocator policies, but must not assume a module's memory
wiring. Existing S3 profiles require octal PSRAM at 80 MHz; AI Passport uses 8 MB
flash without PSRAM. Do not apply AI Passport's USB console pins to every C3 board.
Metadata checks reject inherited hardware settings, missing flash capacity and
conflicting flash selections before invoking ESP-IDF.
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
