# Flash layouts

Keep every layout's CSV and Kconfig defaults together in this directory.
[layouts.json](layouts.json) is the registry: boards select its layout ID in
`boards/<board>/config.json`; application profiles do not own partition tables.

| Layout ID | CSV / defaults basename | Hardware | Application slots |
| --- | --- | --- | --- |
| `standard-v1` | `standard-v1` | Existing ESP32-S3 boards using the shared layout | Two 2 MiB A/B slots |
| `passport-8m-v1` | `passport-8m-v1` | AI Passport, ESP32-C3 with 8 MiB flash | Two A/B slots; see CSV for sizes |
| `paper-s31-v1` | `paper-s31-v1` | Paper V2, ESP32-S31 | Two 4 MiB A/B slots |
| `paper-4in0-v2` | `paper-4in0-v2` | Paper 4IN0, ESP32-S31 | Reserved 3 MiB Factory plus three 3 MiB application slots |
| `amoled-175-v1` | `amoled-175-v1` | Non-C AMOLED 1.75, ESP32-S3 with 16 MiB flash | Factory launcher plus three 3 MiB application slots |

The CSV is the only source of partition addresses and capacities. Build tools
resolve it into `build/flash-plan.json`; CMake, package tools and USB updates
consume that plan. Do not copy offsets into a board or application manifest.

The managed AMOLED layout reserves the last `0xE000` bytes of each OTA slot for
its cover, so an installable app is limited to `0x2F2000` bytes. Its `font_cjk`
partition starts at `0xC22000` with capacity `0x230000`, shared by Factory and
all installed applications. Factory and app-slot images have distinct flash
destinations; selecting a layout does not select which image is built.

Boards with compatible flash geometry may share a layout. Add a new layout
when hardware or storage requirements differ, not for each new application.
Moving source files does not change layout IDs, versions or flash contents.
Address changes require an explicit layout migration and a matching USB package.

Paper 4IN0 v2 leaves `0xE52000..0x1000000` unallocated. Its `font_cjk` partition
is reduced to `0x230000` bytes and stores a shared raster/IME resource pack,
distinct from the LVGL font asset. The desktop is explicitly selected in
`ota_0`; the reserved Factory is not yet a registered or implemented launcher.
The original six-color `paper-s31-v1` layout remains unchanged. See the
[Paper board migration notes](../boards/paper-s31-4in0/README.md).

Validate with `python firmware/tools/build_firmware.py --check` from the
repository root, then build the selected profile. See the
[profile guide](../profiles/README.md) for configuration and the
[Factory guide](../factory/waveshare-amoled-175/README.md) for initial packages.
