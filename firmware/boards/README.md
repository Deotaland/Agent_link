# Boards

A board is a set of hardware (codec, screen, buttons, motor, sensors) plus the capabilities it declares. The shared app ([`../main/app_main.cpp`](../main/app_main.cpp)) and the [`agent_link`](../components/agent_link/) SDK depend only on the abstract [`Board`](../main/board.h) interface, not on any particular board or chip, so adding or swapping a board leaves both untouched.

## A board is three files

Copy `boards/rorolee-s3/` as a starting point:

```
boards/rorolee-s3/
├── config.h        pins and feature macros (GPIO_NUM_*, resolution, I2C addresses)
├── config.json     target chip, plus the catalog entries describing the hardware (see "The board catalog")
└── rorolee_s3.cc   the board class: subclass Board, implement capabilities, end with DECLARE_BOARD(...)
```

`config.json` carries `manufacturer`, `type`, and `target` (the chip, e.g. `esp32s3`), which tells you which `idf.py set-target` to run, and the board's catalog entries. The build doesn't parse it. Actual sdkconfig requirements go in two places (see "Adding a board" below): settings every board on a chip needs (flash size, PSRAM on/off, PSRAM mode/speed if the whole board family shares one module — every ESP32-S3 board here does, Octal @ 80MHz) in `sdkconfig.defaults.<target>`; settings only _this specific_ board needs, where the wanted value is itself a member of a Kconfig `choice` (an external RTC crystal on rorolee-s3/tem-monitor/work-badge but not the others, say), patched directly into `sdkconfig` by the top of [`../CMakeLists.txt`](../CMakeLists.txt) — plain Kconfig `select` cannot force those (kconfiglib ignores `select`/`imply` aimed at a choice member), so don't reach for it there.

## The board catalog

The `boards` list in `config.json` describes the hardware each board type runs on: one entry per `BOARD_TYPE_*`, so `wownny-muse/`, which builds two board types, has two. AI agents use it to work out which firmware a user's board needs from whatever the user knows (`skills/agent-link-flash/scripts/find_board.py`). [`../../tools/board_catalog.py`](../../tools/board_catalog.py) checks it and generates the board table in `AGENTS.md` and the copy bundled with the skill.

| Field | Meaning |
|---|---|
| `board_type` | The `BOARD_TYPE_*` suffix from `main/Kconfig.projbuild` |
| `model` | The hardware model a user would quote (`WWY-01`, `Korvo 2 V3`); `null` only for wiring examples. Board types on the same hardware share it |
| `product`, `vendor`, `buy`, `docs`, `aliases` | How users name it and where they get it. `aliases` adds other names, shop item ids, Chinese names |
| `hardware`, `identify`, `flash_port` | What is on it, how to recognize it, which USB port flashes it |
| `use` | What this firmware does; this is what tells apart board types on the same hardware |
| `transport` | `BLE`, `WIFI`, `MUSE` or `NONE` |
| `ble_name`, `fw_model` | The name it shows during setup, and what `Model()` (or `Name()`) returns, i.e. the boot log's `model = …` |
| `status` | `supported`, `legacy`, `example` (hand-wired, not a product) or `not-adapted` (don't flash) |
| `notes` | Anything else an agent should know |
| `zh` | Chinese `product`, `vendor`, `hardware`, `identify`, `flash_port`, `use`, `notes` |

`python tools/board_catalog.py` fails when a Kconfig board type has no entry, when an entry's directory doesn't match `main/CMakeLists.txt`, when `fw_model` isn't a string in the board's sources, or when entries for the same model disagree on vendor, shop or docs. Run it after every catalog change and commit what it generates.

## The Board interface

`Board` (in [`../main/board.h`](../main/board.h)) is capability-level, not driver-level. `Name()` and `Capabilities()` are required; the rest are optional overrides that default to no-ops.

| Method                                   | Direction       | Override when the board has                                                                                                                   |
| ---------------------------------------- | --------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `Name()`                                 | —               | required: display and BLE advertising name                                                                                                    |
| `Capabilities()`                         | —               | required: bitwise OR of the `AGENT_CAP_*` bits you support                                                                                    |
| `Model()`                                | —               | a hardware model string the App checks before pushing an OTA (default: `Name()`)                                                              |
| `OnLinkState(connected)`                 | —               | something to show or shut down when the link comes and goes                                                                                   |
| `OnLinkStatus(st)`                       | —               | a screen: draw `st.title` / `st.hint` (and `st.code` while pairing) — the SDK writes the words, identically shaped on BLE and WiFi. See below |
| `Platform()`                             | —               | always, if the board might ever ship on WiFi: which platform product this hardware is. Ignored on BLE, so returning it costs nothing          |
| `PlayAudio(pcm16, bytes)` / `AudioEnd()` | Agent to device | a speaker                                                                                                                                     |
| `ShowText(utf8)`                         | Agent to device | a screen                                                                                                                                      |
| `Vibrate(ms)`                            | Agent to device | a motor                                                                                                                                       |
| `SetLed(rgb)`                            | Agent to device | a controllable LED (the SDK synthesises a `led0` endpoint for it)                                                                             |
| `OnListen(start, max_ms)`                | Agent to device | a mic the App can switch on (commands 0x3C/0x3D) — open an `AGENT_STREAM_AUDIO` stream from it                                                |
| `GetBatteryLevel()` / `IsCharging()`     | device to Agent | a fuel gauge                                                                                                                                  |

Set a capability bit only when you implement its method; anything you leave out keeps the base no-op.

### One board, every transport

Nothing in the `Board` interface depends on which transport the build selects (`menuconfig` → Agent Link Device → Transport backend: BLE, WiFi or Muse). A board written against it builds and runs on each of them unchanged — `korvo-cloud/` and `rorolee-muse/` are the worked examples.

The one place the transports genuinely differ is how the device gets connected: over BLE a user opens the App; over WiFi they join a hotspot, then type an activation code into the console; as a Muse gadget they add it in the Muse app and press the device's button to confirm. The SDK folds all of them into one status, `agent_link_status_t`, delivered through `OnLinkStatus()`:

| `st.phase`   | BLE                       | WiFi                                                                  | Muse                                                              |
| ------------ | ------------------------- | --------------------------------------------------------------------- | ----------------------------------------------------------------- |
| `SETUP`      | advertising: open the app | captive portal up: join the `<Name>-XXXX` hotspot                     | add `MuseGadget-XXXXXX` in the Muse app / press to confirm        |
| `CONNECTING` | App connected, pairing    | joining WiFi / signing in / platform unreachable, retrying            | pairing, joining the WiFi from the app, reaching the Muse, retrying |
| `PAIRING`    | —                         | activation code in `st.code`, seconds left in `st.expires_s`          | —                                                                 |
| `BLOCKED`    | —                         | the platform refused: already bound, no agent attached, gateway error | WiFi or Muse sign-in failed                                       |
| `CONNECTED`  | —                         | authenticated, heartbeat running, no data plane yet                   | —                                                                 |
| `READY`      | App subscribed            | (once the WiFi data plane exists)                                     | registered with the Muse; `st.title` is the Muse's name          |

`st.title` and `st.hint` are already written for the situation in English. A board that draws them gets correct instructions on every transport and never learns which it is on. Switch on `st.phase` only to choose layout and colour, or to replace the wording.

A Muse gadget's voice stream (`AGENT_STREAM_VOICE`) becomes a voice note to the Muse, and the Muse's answer comes back through `ShowText()`, so a push-to-talk board needs nothing Muse-specific. See `components/muse_gadget/README.md`.

More calls that work the same everywhere:

- `agent_link_device_id()` — the device's identity, a UUIDv4 — one value on both transports: the `uuid` the App reads from 0x01 over BLE is the `device_sn` the platform lists over WiFi. For a settings or support screen.
- `agent_link_forget()` — a board's factory reset: BLE erases its bonds, WiFi drops its platform credential. The identity above is kept, so the platform still recognises the unit afterwards.
- `agent_link_confirm()` — forward a press of the main button here while the link is not READY. A transport that pairs only after a press on the device (Muse) takes it as the confirmation; everywhere else it does nothing.
- `agent_link_state()` — still the data-plane gate: open a stream only when it is `AGENT_STATE_READY`.

`Model()` is worth a second look: it is the string an incoming OTA image must claim, so the App
cannot flash a build for other hardware onto this board. **Boards that share a PCB must return the
same value** — `rorolee-s3` and `work-badge` both return `"RRL-01"`, which is also the model the
mass-produced unit reports, and that is what lets the App upgrade an existing product straight to
agent_link firmware.

## What goes in `<board>.cc`

Three kinds of thing:

1. **Hardware bring-up**, in the constructor. Use the public registry components (`esp_codec_dev`, `esp_lcd_*`, `esp_driver_i2c`, `LEDC`) or the shared drivers in [`common/`](common/). Keep pin numbers in `config.h`.
2. **Capability methods.** Override the ones your hardware supports and set the matching `AGENT_CAP_*` bits in `Capabilities()`.
3. **Board-specific logic.** For example, a task that reads a sensor and reports it through the device-I/O API:

   ```c
   agent_link_io_desc_t t = { .id = "temp0", .dir = AGENT_IO_IN, .kind = "temperature",
                              .value = AGENT_VAL_F32, .unit = "C" };
   agent_link_register_io(&t, NULL, NULL);          // once, before agent_link_start()
   ...
   float c = read_sensor();
   agent_link_push_reading("temp0", &c, sizeof c);  // periodically
   ```

   `#include "agent_link.h"` and call `agent_link_push_*` directly. Actuators register a callback with `agent_link_register_io` and act when the Agent drives them. See `boards/tem-monitor/` for a worked example. If a board grows, add more `.cc`/`.h` files in its directory; they are compiled automatically.

> The device stays thin. What the Agent says and how it decides live in the cloud, not on the device, so a board is usually hardware plus a little glue rather than a full dialog state machine.

## Adding a board

Say `my-board` on an ESP32-C6:

1. Copy a directory: `cp -r boards/rorolee-s3 boards/my-board`.
2. Edit `config.h` with your pins.
3. Edit `config.json`: set `"target": "esp32c6"`, and write the board's catalog entry (see "The board catalog"): its model, vendor, where to buy it, how to recognize it and what the firmware does.
4. Edit the `.cc`: rename the class, set `Name()` and `Capabilities()`, implement your methods, and end with `DECLARE_BOARD(MyBoard);`. Override `Model()` if the board shares a PCB with another one (see above). Nothing is needed for OTA — the SDK handles App-driven firmware upgrades for every board; register a callback only if you want to draw progress
5. Register it in three places:
   - add `config BOARD_TYPE_MY_BOARD` to the `choice BOARD_TYPE` in [`../main/Kconfig.projbuild`](../main/Kconfig.projbuild). `select` there works for a plain bool the board needs (`work-badge` selects the two extra `LV_FONT_MONTSERRAT_*` sizes its UI draws with), but **not** for a value that is a member of a Kconfig `choice` — see the next bullet;
   - add `elseif(CONFIG_BOARD_TYPE_MY_BOARD) set(BOARD_DIR "my-board")` to the board-select chain in [`../main/CMakeLists.txt`](../main/CMakeLists.txt). If the board needs a value that only _this_ board wants and that value is itself a member of a Kconfig `choice` (an external RTC crystal, a non-default console, ...), also add `BOARD_TYPE_MY_BOARD` (and its wanted `CONFIG_..._SRC=y`-style lines) to the per-board tables at the top of the repo-root [`../CMakeLists.txt`](../CMakeLists.txt) — plain Kconfig `select` on a choice member silently does nothing (kconfiglib: "select/imply has no effect on choice symbols"; verified against this project's own tree), so the values are patched directly into `sdkconfig` before Kconfig reads it instead. This reruns on every configure, so it self-corrects after switching `Board Type` in menuconfig, no manual `sdkconfig` editing needed. If instead the whole chip family shares the value (like PSRAM mode/speed for every ESP32-S3 board here), put it in `sdkconfig.defaults.<target>` instead — see the last bullet below.
   - If you use peripherals beyond what is already required, add their driver components (`esp_driver_i2c`, `esp_lcd`, `esp_codec_dev`) to the **unconditional** `REQUIRES` in `main/CMakeLists.txt` — or, for a managed component, to `main/idf_component.yml` (with a `rules:` target gate if it only exists on one chip). **Do not wrap a `REQUIRES` entry in `if(CONFIG_BOARD_TYPE_...)`:** ESP-IDF collects REQUIRES in an early-expansion pass that runs the file in script mode with no sdkconfig loaded, so every `CONFIG_*` is empty there and the entry is silently dropped. `SRCS` is not affected — it is re-evaluated in the normal pass, which is why the board-select chain above works.
   - If the board is on a chip target that isn't built yet (a new `esp32c6`, say), add a `sdkconfig.defaults.<target>` at the repo root for whatever _every_ board on that chip needs (flash size, PSRAM on/off, ...) — ESP-IDF merges it automatically on `idf.py set-target <target>`, the same way [`../sdkconfig.defaults.esp32p4`](../sdkconfig.defaults.esp32p4) already does for the P4 board. Plain `select` in `main/Kconfig.projbuild` is still the right tool for a board-specific value that is a plain bool, not a choice member (not common so far in this repo).

Then build:

```bash
idf.py set-target esp32c6
idf.py menuconfig        # Agent Link Device -> Board Type -> My Board
idf.py build flash
```

`../main/app_main.cpp` and `../main/board.h` do not change; they bind to the abstract `Board` and pick up the selected board automatically.

Finally run `python tools/board_catalog.py` from the repository root. It refuses a board type without a catalog entry and regenerates the board table in `AGENTS.md`.

Code that a second board could use (a panel or codec driver, a font loader) goes in [`common/`](common/) from the start, not in the board's directory.

## Boards in this repo

The Board Type menu currently offers:

| Directory            | Target   | Notes                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| -------------------- | -------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `rorolee-s3/`        | ESP32-S3 | Reference board: SH8501 AMOLED (factory short-code bring-up), ES8311/ES7210 codec, push-to-talk mic, BQ27220 fuel gauge, external 32kHz crystal                                                                                                                                                                                                                                                                                                                                                    |
| `tem-monitor/`       | ESP32-S3 | Sensor board on the OpenJumper ESP32-S3 AIoT Basic V2: SPA06 pressure/temperature and SHT30 temperature/humidity over I2C; a worked example of the device-I/O path; external 32kHz crystal                                                                                                                                                                                                                                                                                                         |
| `es8311-voice/`      | ESP32-S3 | Wiring example, not a product: any ESP32-S3 board plus one ES8311 codec doing full-duplex speaker + mic                                                                                                                                                                                                                                                                                                                                                                                           |
| `es8311-asr/`        | ESP32-S3 | Wiring example, not a product: one ES8311 codec, mic-only, streams PCM to the App for live ASR                                                                                                                                                                                                                                                                                                                                                                                                     |
| `gc2145-camera/`     | ESP32-S3 | OpenJumper ESP32-S3 AIoT Basic V2: GC2145 DVP camera live preview on its ST7789 240x240 LCD                                                                                                                                                                                                                                                                                                                                                                                                         |
| `korvo/`             | ESP32-S3 | 酷世DIY ESP32S3 Korvo 2 V3 (ESP32-S3-Korvo-2 V3 pinout; flash through the left USB-C): swipeable LVGL home screen on a CST816 touch ST7789 240x280 driven rotated 90° CCW, with three apps — live GC2145 preview, ES8311/ES7210 voice up the Agent's ASR channel, and WAV recording to the TF card                                                                                                                                                                                                                                                    |
| `korvo-cloud/`       | ESP32-S3 | Same PCB as `korvo/`, built for the **WiFi channel**: the home screen is the link status — join the hotspot, the activation code to type into the console, online — and the camera is launched from an icon on the second page. No TF card (WiFi and TLS want that internal RAM). Written only against `agent_link_status_t`, so it also builds and runs on BLE, where the same screen says "open the app". Deliberately does **not** share `korvo`'s `Model()` — see the note in `korvo_cloud.cc` |
| `work-badge/`        | ESP32-S3 | Electronic staff badge on the **mass-production** board (pins from the shipping firmware; factory short code SH8501 panel): an LVGL name card the App fills in ([README](work-badge/README.md))                                                                                                                                                                                                                                                                                                    |
| `rorolee-muse/`      | ESP32-S3 | The rorolee-s3 PCB with an LVGL screen: link status, hold BOOT to talk, the answer as text; VOL+/VOL- volume, hold VOL- 6 s to forget the pairing. Built for **Transport backend = Muse**, where it is a Muse gadget set up from the Muse app, but written against `agent_link_status_t` only, so it builds and runs on BLE and WiFi too |
| `wownny-muse/`       | ESP32-S3 | WOWNNY (the rorolee PCB with a GC9D01 160x160 round TFT, PA on GPIO3): same features as `rorolee-muse/`, with a round-screen UI that has no text outside the settings menu (battery, avatar, a small indicator). Two board types share this directory, `config.h` has the differences: `WOWNNY_MUSE` is WWY-01 (backlight on GPIO46, 32 kHz crystal), `WOWNNY_OLD_MUSE` is WWY-01-old (panel on GPIO4-7/15 powered from GPIO46, no crystal, volume keys swapped). `Model()` returns `WWY-01` / `WWY-01-old`, so neither takes the other's (or rorolee's) OTA image |
| `esp32p4-waveshare/` | ESP32-P4 | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (7-inch 1024x600 IPS, GT911 touch). **Not adapted yet:** the screen code still drives a CO5300 466x466 AMOLED, so the 7B's screen stays dark |

Code used by more than one board lives in [`common/`](common/).

## Reference

The layout (`boards/<board>/{config.h, config.json, <board>.cc}`, a Kconfig board choice, and a `target` in `config.json`) follows [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). The difference is that agent_link owns networking and transport, so `Board` here describes only local hardware and does not split into `WifiBoard` / `Ml307Board` the way xiaozhi does; it exposes capability-level operations instead of driver types.
