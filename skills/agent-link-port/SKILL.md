---
name: agent-link-port
description: Add a new Agent Link hardware port or BLE/WiFi firmware profile, including drivers, Application callbacks, Kconfig, defaults and board catalog registration. Use for new-board development, not flashing an existing supported board.
---

# Add an Agent Link board

Work in the user's real checkout and preserve existing changes. Read `AGENTS.md`,
`firmware/boards/README.md` and `firmware/apps/common/application.h`.

Confirm the chip, flash/PSRAM, peripheral parts, schematic/pins, USB connector
and intended BLE or WiFi connection. Inspect supplied BSPs before asking for
information already present. Select the closest existing application and reuse
common drivers; do not create a second source tree.

Implement hardware under `firmware/boards/` and behavior under `firmware/apps/`.
Register one `Application` and expose only working capabilities. Keep callbacks
short and queue blocking I/O. BLE uses SDK streams through the phone app. WiFi
requires the registered product ID and a static `Platform()` configuration;
use SDK provisioning and device-authenticated conversation APIs. Do not add a
second MQTT/provisioning client or copy another product's identity.

Add the catalog entry, profile manifest, dedicated defaults, Kconfig profile
choice and derived board/backend mappings together. Select dependencies from
the profile manifest. Preserve existing firmware model strings for OTA.

From the repository root:

```sh
python firmware/tools/build_firmware.py --check
python tools/board_catalog.py
python tools/board_catalog.py --check
python tools/check_docs.py
python firmware/tools/build_firmware.py PROFILE --jobs 2
```

Report the profile, pins, supported I/O and build result. List remaining hardware
checks for power, input, display, audio and the selected connection. Flash only
when requested; use the identified board's connector and build metadata.
