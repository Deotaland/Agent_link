# Add a board

A port has three parts: hardware under `boards/`, an application under `apps/`,
and a profile under `profiles/`. BLE and WiFi use the same application callbacks;
the SDK owns provisioning, authentication and transport protocols.

## Choose a starting point

| Need | Read first |
| --- | --- |
| Small BLE voice device | `apps/es8311-voice/` and `profiles/es8311-voice-ble.defaults` |
| BLE camera | `apps/gc2145-camera/` |
| BLE/WiFi on shared hardware | `boards/m5stack-stopwatch/` and its two applications |
| WiFi voice and conversations | `apps/m5stack-stopwatch-wifi/` |
| C3 without PSRAM | `boards/ai-passport/` and `profiles/ai-passport-ble.defaults` |

Confirm the chip, flash/PSRAM size, panel and codec, GPIO assignments, power
controls, button polarity and USB connector before copying a port. Use its
schematic; similar product names do not guarantee the same wiring.

## 1. Hardware

Create `boards/my-board/config.h` for pins and product settings. Put board-specific
initialization and drivers beside it. Reuse drivers from `boards/common/` by
passing pins and bus handles. Initialize shared buses once, then attach devices.
Do not put BLE/WiFi branches or protocol code in hardware drivers.

Create `config.json` using an existing catalog entry as the schema. Set the
outer `type` to the directory name and `target` to the IDF chip name. Each item in
`boards` describes one firmware variant: unique `board_type`, model, product,
vendor, aliases, identification details, USB port, use, transport, BLE name,
`fw_model`, status and notes. All catalog fields and aliases use English;
do not add translated copies.

Use distinct board types for BLE and WiFi variants. `fw_model` must match the
application's `Model()` exactly. Preserve existing model strings when migrating
an installed product; OTA validates them.

## 2. Application

Create `apps/my-board-ble/my_board.cc`. Derive from
[Application](../apps/common/application.h), initialize hardware and register one
implementation:

```cpp
#include "application.h"
#include "config.h"

class MyBoard : public Application {
public:
    MyBoard() { /* Initialize hardware and workers. */ }
    const char* Name() const override { return "MyBoard"; }
    const char* Model() const override { return "MY-BOARD"; }
    uint32_t Capabilities() const override { return 0; }
};

DECLARE_APPLICATION(MyBoard);
```

Add capabilities only as their handlers work:

| Function | Application/API |
| --- | --- |
| Speaker output | `PlayAudio`, `AudioEnd`, `AudioReset`; queue PCM16 mono at 16 kHz |
| Microphone request | `OnListen`; capture on a worker |
| Button voice input | `agent_link_stream_open/write/close` with `AGENT_STREAM_VOICE` |
| Text, motor, LED | `ShowText`, `Vibrate`, `SetLed` |
| Link status | `OnLinkStatus`; copy `title`, `hint` and activation `code` |
| Sensor/actuator | Register endpoints with `agent_link_register_io` |
| Battery | `GetBatteryLevel`, `IsCharging` |

Copy callback data before returning and queue slow work. The NimBLE host stack
is 6 KB. Stream APIs can block; use them on a capture worker. Before starting a
capture, check `agent_link_state() == AGENT_STATE_READY` and handle stream errors.
Constructors run before SDK initialization; defer operations that require a link.

For **BLE**, the phone app supplies the platform connection. Start with the
voice example and adapt its pins, input polarity and buffering.

For **WiFi**, also implement `Platform()` with a static `agent_platform_t`
containing the real platform URL, registered product ID and chip type. Do not
copy another product's ID. The SDK handles hotspot setup and activation; display
`OnLinkStatus` output or provide another way to read the code. Use
`agent_link_platform_bound_agent()` and the conversation/call APIs from
[agent_link_platform.h](../components/agent_link/include/agent_link_platform.h).
Select and bind Agents in the platform console. Microphone upload starts after
`session.ready`. Follow the [realtime guide](../components/agent_link/REALTIME.md)
for formats, worker ownership and playback buffering.

## 3. Profile and build registration

For `my-board-ble` on ESP32-S3, add these together:

1. In `main/Kconfig.profiles`, add `AGENT_LINK_PROFILE_MY_BOARD_BLE` to the choice
   with `depends on IDF_TARGET_ESP32S3`, and map it to `"my-board-ble"` in
   `AGENT_LINK_FIRMWARE_PROFILE`.
2. In `main/Kconfig.projbuild`, add `BOARD_TYPE_MY_BOARD_BLE` with a default for
   that profile. Add it to the defaults selecting `AGENT_LINK_TRANSPORT_BLE`.
   WiFi profiles select `AGENT_LINK_TRANSPORT_WIFI` instead.
3. Create `profiles/my-board-ble.defaults` with
   `CONFIG_AGENT_LINK_PROFILE_MY_BOARD_BLE=y` and hardware-specific settings.
   Declare flash capacity, the custom partition table, PSRAM wiring, board clocks
   and console routing here; never inherit them from common or chip defaults.
4. Add an entry to `profiles/profiles.json`:

```json
"my-board-ble": {
  "board": "my-board",
  "board_type": "MY_BOARD_BLE",
  "target": "esp32s3",
  "backend": "BLE",
  "defaults": ["profiles/ble.defaults", "profiles/s3-ble.defaults", "profiles/my-board-ble.defaults"],
  "app": "my-board-ble",
  "hardware": "my-board",
  "hardware_sources": [],
  "drivers": [],
  "ui": "none"
}
```

List hardware `.c`/`.cc` files in `hardware_sources` and common drivers in
`drivers`; application sources are collected from the selected app directory.
Add `profiles/lvgl.defaults` and `ui: "lvgl"` only if needed. The S3 fragments
assume PSRAM: adapt defaults to the actual memory instead of copying them to a
board without PSRAM. For WiFi on S3, use `profiles/s3-network.defaults` in place
of the BLE fragments and set product identity in the application config.

If a new driver needs a component, add its dependency selection in
`components/board_support/CMakeLists.txt` and managed version in
`idf_component.yml`. Dependency selection reads the manifest before Kconfig is
available. Keep common defaults free of board-specific settings and credentials.

## 4. Validate

From the repository root:

```sh
python firmware/tools/build_firmware.py --check
python tools/board_catalog.py
python tools/board_catalog.py --check
python tools/check_docs.py
python firmware/tools/build_firmware.py my-board-ble --jobs 2
```

After an authorized flash, check the boot model, link status, advertised I/O,
buttons, audio and reconnect behavior. For WiFi also test provisioning,
activation and a conversation with the platform-bound Agent. Record pending
hardware checks. A build alone does not verify wiring or audio.
