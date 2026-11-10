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
| Waveshare ESP32-S3-Touch-AMOLED-1.75 (non-C, 16 MB) | `waveshare-amoled-175-wifi` | WiFi |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | `waveshare-amoled-175c-wifi` | WiFi |
| OpenJumper AIoT Basic V2 | `gc2145-camera-ble` | BLE |
| ESP32-S3 + ES8311 wiring examples | `es8311-asr-ble`, `es8311-voice-ble` | BLE |
| Waveshare ESP32-S3-ePaper-3.97 | `waveshare-epaper-397-wifi` | WiFi; ESP32-S3 |
| Mozhifang paper V2 | `paper-s31-wifi` | WiFi; ESP32-S31 |
| Mozhifang paper V2 4.0-inch monochrome | `paper-s31-4in0-wifi` | WiFi; ESP32-S31; independent three-key desktop |
| OJBadge | `ojbadge-wifi` | Local ESP-NOW friends; no platform backend |

S3/C3 profiles support ESP-IDF 5.5.x and 6.1.x. S31 needs an S31-capable 6.1 SDK.

ESP32-P4 has no adapted profile. A backend in the SDK does not imply that every
application supports it.

The non-C AMOLED 1.75 profile defaults to its Agent voice application. In
**Agent Link Device > Firmware build target** comes first. Application mode shows
**Application firmware profile**, with profiles supported by the current chip.
Factory mode hides that application list and shows **Factory board**, containing
only registered hardware names. Currently only the non-C Waveshare AMOLED 1.75
is listed. Chips without any registered Factory do not offer Factory mode.
The Factory source is bound to the selected board manifest.
The same Menuconfig selection is used by VS Code and the build tool. Old CMake
image-role cache entries do not override it. `--image-role voice` or
`--image-role factory` updates this selection explicitly. For example, from the repository root:

```sh
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --jobs 2
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role factory --jobs 2
```

The first command builds the saved target (voice on a fresh configuration).
Application and Factory are separate images; Factory sources are bound to the
selected board. The binding also generates `main/Kconfig.factories`, and the application menu, so adding
a factory does not require a second handwritten availability list.

For the managed non-C AMOLED board, normal Flash and `app-flash` write Voice to
`ota_0` (`0x310000`) and select that slot for boot. They preserve the Factory
launcher at `0x10000`. At actual flash time the shared adapter checks the device's
partition table, launcher identity and destination: an empty slot or the same app
is accepted; another app is refused. Use the launcher installer to select a free
slot instead. A successful first app startup publishes its installation record.

A blank board, or one whose factory partition contains Voice, needs the board's
initial package (or a Factory flash) first. Build a fresh matching launcher + Voice
USB package with:

```sh
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --jobs 2
```

Both images build sequentially in the existing `build/`; the saved target ends on
Voice. `build/packages/waveshare-amoled-175-wifi/merged.bin` includes both images and
starts the launcher. Its own `flasher_args.json`, ELF/MAP and SHA-256 manifest travel
with it. Initial merged packages replace settings and installed apps. Application
builds deliberately do not leave `build/merged.bin`: its padding would erase the
launcher. See the [factory guide](../factory/waveshare-amoled-175/README.md).
Other boards retain their existing single-app A/B layouts and flash behavior.

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

Build definitions have separate owners:

| Definition | Source and responsibility |
| --- | --- |
| Board | `boards/<board>/config.json`, each entry's `build`: hardware source/driver selection, board defaults, layout ID, shared assets and optional factory binding; chip comes from the catalog's `target` |
| Application | `apps/<app>/manifest.json`: application ID/name, backend, UI, shared business sources and software dependencies |
| Factory | `factory/<board>/manifest.json`: only that board's launcher entrypoint and extra pages; no application catalog or duplicate offsets |
| Layout | `partitions/layouts.json`: version, chip compatibility, CSV, slots and reserved bytes; existing CSV offsets remain unchanged |
| Profile | `profiles.json`: `board`, historical `board_type`, `app`, `backend`, ordered software `defaults`; public profile names remain stable |
| Delivery | Generated `build/flash-plan.json`: image identity, destination, boot choice and layout; consumed by IDF flash/app-flash, the serial adapter and package tools |

CMake consumes `build/generated/firmware_profiles.json`, resolved from these sources
before dependency expansion. This is generated output, not another editable catalog.
One physical board may have multiple application/backend profiles. Historical
catalog aliases and OTA `Model()` strings remain compatible. Never assume binaries
are portable between boards or chips just because application sources are shared.
Multiple apps on the same board/backend are distinct valid combinations. The board
lookup lists all their profile names; generated bindings enable the existing board
symbol for each app without copying hardware sources. Run `--generate-kconfig` after
adding a profile as well as after changing Factory bindings.

Defaults load common/chip software policy, shared profile fragments, board hardware
defaults, layout defaults, the profile's software overrides, then private settings.
Memory, clocks and console pins live beside their board; partition settings live
beside the layout. `sdkconfig` remains generated. A manifest migration within the
same profile retains local log levels and the saved application/factory selection;
an explicit profile switch or `--reset-config` still reapplies that profile.

A layout is stable across applications on a board. Adding an app does not add a
CSV; compatible profiles can share a layout. Change the layout ID/version and plan
an explicit USB migration when addresses change. Managed layouts own installation
and boot selection centrally and disable the generic round-robin OTA engine, which
could otherwise overwrite another installed app. Single-app boards retain A/B OTA.
The current launcher installs into empty slots and rejects duplicate app IDs;
application-store replacement still requires remove/install. USB development updates
only overwrite the explicitly designated slot after checking its app identity.

Private settings load last from ignored `sdkconfig.private`. Only recognized
credentials and direct-test settings are accepted there, not board or memory
settings; the allowed keys are in `tools/build_firmware.py`.

The first migration saves `sdkconfig.before-profile` once and imports recognized
private settings. Within the same profile, manifest/default changes reapply owned settings while
retaining other local options. Switching profiles or using `--reset-config`
regenerates the selection. The same profile builds incrementally. Chip
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

Follow the [board guide](../boards/README.md). Add the board build entry, application manifest, profile defaults,
Kconfig choice and board/backend mapping together. Run
`python firmware/tools/build_firmware.py --generate-kconfig` after changing Factory
bindings, then `--check`. Regenerate the board catalog,
validate metadata, build the selected profile and record hardware acceptance.

## Design references

The board/application boundary follows the hardware-capability separation shown by
[Muse's board interface](https://github.com/facebookincubator/muse-gadget-sdk/blob/463bd2a05380fea4211b0099ad0f70933c108b20/esp32/components/muse/muse_board.h).
[SDGOODS' runtime mode detection](https://github.com/SDGOODS/SDGOODS-ESP32S3/blob/dae46766aa345c680691420691d3d7170f0c6372/components/sdgoods_launcher/src/device_mode.c)
illustrates why an OTA slot alone does not imply launcher management. Agent Link
keeps fixed, versioned layouts across its different chips; managed USB delivery
preserves the board launcher and uses the same contract as application installation.

Menu selections are separate: the application's saved profile is retained in
`build/generated/Kconfig.application_selection` and the profile state, while
Factory resolves its board to a canonical build configuration. Switching modes
reapplies the active hardware/software defaults; hidden application entries never
select Factory hardware. The generated file is a local preference cache, not
another supported-board list. Regenerate both menu files with `--generate-kconfig`.
