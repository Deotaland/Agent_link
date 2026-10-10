# AGENTS.md

**English** | [简体中文](AGENTS.zh-CN.md)

Instructions for AI assistants (ChatGPT, Claude, Codex, Cursor and similar) working with this repository. The usual job is to get firmware onto a user's device and the device online. The last sections cover changing the code. People should start with [README.md](README.md).

## The project in brief

- **Agent Link** connects ESP32 devices to the **Deotaland Agent platform**. A device declares its hardware (mic, speaker, screen, sensors) and the SDK in `firmware/components/agent_link` handles the link.
- **Platform console:** https://bot.zhaojun.work. The firmware center is https://bot.zhaojun.work/#/firmware. This address is temporary: the platform moves to the deotaland.ai domain once its ICP filing is done. If the address above stops working, ask the user for the current one.
- **Hardware:** ESP32-S3 with 16 MB flash and octal PSRAM (all boards except `esp32p4-waveshare`).
- **Toolchain:** ESP-IDF v5.5 (the maintainers build with v5.5.4).
- **Link, chosen at build time:** BLE to the Deotaland phone App (default), WiFi straight to the platform, or Muse (Meta's Muse app and cloud).
- **Releases:** https://github.com/Deotaland/Agent_link/releases has the release notes. Prebuilt images are in the firmware center, not on GitHub.

## Rules

1. Find out which board the user has before flashing anything (section 1). The model, the company, where it was bought, a photo or the name it shows over Bluetooth is enough to look it up. Never flash firmware for a board you couldn't identify.
2. Before a full-chip erase or a merged ("full") image, tell the user it wipes the device's settings: WiFi, Bluetooth and Muse pairing, and its device ID. Afterwards the device has to be set up and bound on the platform again.
3. Never print, commit or upload secrets: the Muse SDK token, the MiniMax API key, anything in `firmware/sdkconfig`. Firmware built with them contains them, so don't share those images either.
4. If you can't run commands (a chat assistant), use path A and guide the user one step at a time. Ask them to paste what the screen or the log shows.
5. If you can run commands, path C gives you the most control. Change nothing in the user's code or config beyond what the task needs.

## Skills

`skills/` holds three agent skills:

| Skill | Use it to |
|---|---|
| `agent-link-flash` | Identify the user's board, flash it with their consent, set it up |
| `agent-link-build` | Build firmware for a board with ESP-IDF 5.5 and produce a merged image |
| `agent-link-debug` | Diagnose a device from its serial log and symptoms |

Install them yourself before starting; don't ask the user to do it.

- Claude Code: `python tools/install_skills.py --user` copies them to `~/.claude/skills/`; `--project` puts them in this checkout's `.claude/skills/` instead (gitignored). Copying the folders by hand works as well.
- Other tools: put each `skills/<name>` folder wherever the tool loads skills from (see its documentation).
- No checkout: the folders are at https://github.com/Deotaland/Agent_link/tree/main/skills.
- A tool without skills (a chat assistant): read `skills/<name>/SKILL.md` and follow it.

Say a skill is installed only after the tool lists it. Installing a skill doesn't allow flashing, erasing, committing or pushing; each of those still needs the user's OK.

## 1. Identify the board

Ask the user for whatever they know: the model, the company, where they bought it (shop or link), the name it shows over Bluetooth, a photo, or what it looks like. Then look it up:

- With a terminal: `python skills/agent-link-flash/scripts/find_board.py "<what the user said>"`. It prints the matching hardware, every firmware build for it, and the settings to build each one.
- Without one: use the table below, or the full catalog with looks, ports and notes: https://raw.githubusercontent.com/Deotaland/Agent_link/main/skills/agent-link-flash/references/boards.json.

<!-- boards:begin -->
| Model | Board | Vendor | Where to buy | What this firmware does | `BOARD_TYPE_…` | Chip | Link | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RRL-01 | RoRoLee | Deotaland | [rorolee.com](https://www.rorolee.com) | Electronic staff badge: a name card filled in from the App over BLE | `ESP32S3_WORK_BADGE` | ESP32-S3 | BLE | supported |
| RRL-01 | RoRoLee | Deotaland | [rorolee.com](https://www.rorolee.com) | Muse voice assistant: hold BOOT to talk, the answer is shown as text | `ROROLEE_MUSE` | ESP32-S3 | Muse | supported |
| RRL-01 | RoRoLee | Deotaland | [rorolee.com](https://www.rorolee.com) | Agent Link reference firmware: push-to-talk with the agent through the Deotaland App over BLE | `ROROLEE_S3` | ESP32-S3 | BLE | supported |
| WWY-01 | WOWNNY | Deotaland | [rorolee.com](https://www.rorolee.com) | Muse voice assistant: hold BOOT to talk; answers are spoken when the build has a MiniMax key | `WOWNNY_MUSE` | ESP32-S3 | Muse | supported |
| WWY-01-old | WOWNNY (old board) | Deotaland | [rorolee.com](https://www.rorolee.com) | Muse voice assistant, same as WWY-01 | `WOWNNY_OLD_MUSE` | ESP32-S3 | Muse | old revision |
| ESP32-S3 AIoT Basic V2 | ESP32-S3 AIoT Basic V2 development board | OpenJumper | [docs](https://www.openjumper.com/doc/esp32aiot-basicv2) | Camera preview on the LCD; a button (or the App) takes a snapshot that goes to the App over BLE | `ESP32S3_GC2145_CAMERA` | ESP32-S3 | BLE | supported |
| Korvo 2 V3 | ESP32S3 Korvo 2 V3 development board | 酷世DIY (Kevincoooool) | [Taobao](https://item.taobao.com/item.htm?id=681702043224) | Touch home screen with three apps: camera preview, voice to the agent, WAV recording to the TF card; links over BLE with the Deotaland App | `ESP32S3_KORVO` | ESP32-S3 | BLE | supported |
| Korvo 2 V3 | ESP32S3 Korvo 2 V3 development board | 酷世DIY (Kevincoooool) | [Taobao](https://item.taobao.com/item.htm?id=681702043224) | WiFi straight to the Deotaland platform: hotspot setup, then a 6-digit activation code entered in the console; camera from the second page | `ESP32S3_KORVO_CLOUD` | ESP32-S3 | WiFi | supported |
| — | Wiring example: ESP32-S3 + ES8311 (mic only) | — | — | Minimal live speech-to-text example: hold the button and the mic audio streams to the App over BLE | `ESP32S3_ES8311_ASR` | ESP32-S3 | BLE | wiring example |
| — | Wiring example: ESP32-S3 + ES8311 (speaker and mic) | — | — | Minimal voice example: hold BOOT to talk to the agent over BLE, replies play on the speaker | `ESP32S3_ES8311_VOICE` | ESP32-S3 | BLE | wiring example |
| ESP32-P4-WIFI6-Touch-LCD-7B | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B | Waveshare | [waveshare.com](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7b.htm) | Screen bring-up only, no link | `ESP32P4_WAVESHARE` | ESP32-P4 | — | **not adapted, don't flash** |
<!-- boards:end -->

- A model identifies the hardware. When several rows share it (RRL-01, Korvo 2 V3), ask what the device should do and use that row.
- Don't flash a board marked "not adapted". "Wiring example" rows are not products: use them only when the user wired the parts themselves, and check their pins against the board's `config.h` first.
- Muse boards show up in the Muse app as `MuseGadget-XXXXXX` (last bytes of the MAC), not under their own name.
- The two WOWNNY builds use different display pins. If the screen stays dark after flashing one, flash the other.
- WiFi needs the platform address compiled into the board (`Board::Platform()`). Today only `korvo-cloud` has it.
- The boot log confirms what is running: `board = …, model = …, fw = …` (the `fw_model` in the catalog) and `init: … transport=ble|wifi|muse`.
- The Kconfig symbol is `CONFIG_BOARD_TYPE_<value>`; in menuconfig it is `Agent Link Device → Board Type`.
- The catalog lives in `firmware/boards/<directory>/config.json`, one entry per board type. The table above and the bundled `boards.json` are generated from it by `python tools/board_catalog.py`.

## 2. Pick a path

| Path | Needs | Use it when |
|---|---|---|
| A. Browser flasher in the firmware center | Desktop Chrome, Edge or Opera; a platform account | The user isn't a developer, or you can't run commands |
| B. Prebuilt image + esptool | Python | You have a `.bin` and a terminal |
| C. Build from source | ESP-IDF v5.5 | No prebuilt image for the board, or config or code has to change |

If the firmware center shows "No Firmware Packages Found" for the board, use path C.

## Path A: flash from the browser

1. Open https://bot.zhaojun.work/#/firmware in desktop Chrome, Edge or Opera. It uses Web Serial; Safari, Firefox and phones won't work. Log in.
2. Filter by product or chip and pick a version. Use a **Full Flash Package**. An **OTA Differential Package** is for over-the-air updates and can't be flashed over USB.
3. Click **Flash**. In the flasher, click **Fetch Cloud Firmware** (or pick a local `.bin`).
4. Connect the device by USB, click **Connect Serial Port** and choose its port. 921600 baud is the default; use 115200 if it fails. The flasher checks the chip type and stops on a mismatch.
5. **Flash Address** is filled in from the file name: `0x0` for merged images (`merged`, `all` or `full` in the name), `0x10000` for app-only images. Leave it as is.
6. Leave **Erase entire flash** off unless the user wants a clean device (rule 2). Keep auto reset on. Click **Start Flashing**.
7. If the port doesn't appear or connecting hangs: hold BOOT, plug in USB (or tap RESET while holding BOOT), release BOOT, try again.

An app-only image has no effect on a device that has taken an OTA update; see Troubleshooting. Then continue with "After flashing".

## Path B: flash with esptool

Install with `pip install esptool` (the ESP-IDF environment already has it). The underscore commands below work in esptool v4 and v5; v5 also accepts `write-flash` / `erase-flash`.

Serial port: Windows `COM3` and so on (Device Manager → Ports); macOS `/dev/cu.usbmodem*`; Linux `/dev/ttyACM0` (the chip's own USB) or `/dev/ttyUSB0` (USB-UART bridge), with the user in the `dialout` group (`uucp` on Arch).

```bash
# Merged image, written from 0x0. Wipes settings and pairing (rule 2).
python -m esptool --chip esp32s3 -p PORT -b 460800 write_flash 0x0 firmware-merged.bin

# Keep settings: the four files from a build, as idf.py flash writes them.
python -m esptool --chip esp32s3 -p PORT -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 bootloader.bin 0x8000 partition-table.bin 0xe000 ota_data_initial.bin 0x10000 agent_link.bin

# Erase everything, including the Chinese font on rorolee-muse.
python -m esptool --chip esp32s3 -p PORT erase_flash
```

Flash layout (`firmware/partitions.csv`, 16 MB):

| Offset | Size | Contents |
|---|---|---|
| `0x0` | | bootloader |
| `0x8000` | | partition table |
| `0x9000` | 20 KB | `nvs`: WiFi, pairing, device ID |
| `0xE000` | 8 KB | `otadata`: which app slot boots |
| `0x10000` | 2 MB | `ota_0`: the app |
| `0x210000` | 2 MB | `ota_1`: second app slot, used by OTA updates |
| `0x410000` | 8 KB | `nvs_keys`, `phy_init` |
| `0x412000` | 11.4 MB | `anim_pack`: the Chinese font on rorolee-muse |
| `0xF84000` | 496 KB | `model` |

## Path C: build from source

Install ESP-IDF v5.5: https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/index.html. On Windows use the ESP-IDF installer and its "ESP-IDF PowerShell" shortcut. On macOS and Linux run `./install.sh esp32s3`, then `. ./export.sh` in every new shell.

```bash
git clone https://github.com/Deotaland/Agent_link.git
cd Agent_link/firmware
```

Choose the board and link interactively:

```bash
idf.py set-target esp32s3        # esp32p4 for esp32p4-waveshare
idf.py menuconfig                # Agent Link Device → Board Type, Transport backend
idf.py reconfigure
idf.py build
```

Or without menuconfig (for agents). Write the choices into a file **outside the repository**, for example:

```
CONFIG_BOARD_TYPE_WOWNNY_MUSE=y
CONFIG_AGENT_LINK_TRANSPORT_MUSE=y
```

Then:

```bash
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;/absolute/path/board.defaults" set-target esp32s3
idf.py reconfigure
idf.py build
```

- Link values: `AGENT_LINK_TRANSPORT_BLE`, `AGENT_LINK_TRANSPORT_WIFI`, `AGENT_LINK_TRANSPORT_MUSE`.
- **Always run `idf.py reconfigure` after selecting a board.** The top-level `CMakeLists.txt` adds the board's own settings (PSRAM, RTC clock, RAM placement, TCP buffer) to `sdkconfig` only when it configures with the board already selected. The log then shows `agent_link: patched sdkconfig for BOARD_TYPE_…`. Without this step the Muse boards run out of RAM.
- `set-target` replaces `sdkconfig` and keeps the old one as `sdkconfig.old`. Secrets in it must be set again. The defaults file can hold them too, which is why it belongs outside the repository.
- Muse boards: set `CONFIG_GADGET_SDK_TOKEN` (`Component config → Muse gadget → Muse Gadgets SDK token`, from https://gadgets.muse.ai/settings/sdk-tokens). Without it the build warns, and pairing will stop working once Muse requires tokens. Optional: `CONFIG_MUSE_TTS_MINIMAX_KEY`, a MiniMax API key that makes the device speak its replies.

Flash and watch the log:

```bash
idf.py -p PORT flash monitor     # Ctrl+] leaves the monitor
```

`idf.py flash` writes the bootloader, partition table, otadata and app. Settings and pairing in NVS stay. For a single image to hand out: `cd build && python -m esptool --chip esp32s3 merge_bin -o merged.bin @flash_args`.

## After flashing: set up the device

### BLE boards

The device advertises under its name (`ble_name` in the catalog). Pair it from the Deotaland phone App. The App isn't public yet; ask the user whether they have it.

### WiFi (korvo-cloud)

1. The screen shows "Set up WiFi". Join the open hotspot `KorvoCloud-XXXX` from a phone. A setup page opens (otherwise go to http://192.168.4.1). Choose the network and enter its password.
2. The screen shows "Activation code" with six digits. The code is valid for 10 minutes and works once.
3. In the console: **Device Management → Add Device → 6-Digit Code Binding**. Enter the code, optionally a name, and choose an agent.
4. The screen shows "Online". "Already bound" means the device has to be unbound in the console first. "No agent" means no agent is attached to it yet.

The platform address is compiled in (`boards/korvo-cloud/config.h`: `CLOUD_BASE_URL`, `CLOUD_PRODUCT_ID`). When the platform moves to deotaland.ai, this firmware has to be rebuilt.

### Muse boards (WOWNNY, RoRoLee Muse)

1. In the Muse app: Settings → Devices → Developer mode, then Add Device. The device appears as `MuseGadget-XXXXXX`.
2. When the device asks for confirmation, press BOOT.
3. The app sends the WiFi credentials. The device restarts and connects to the user's Muse.
4. Hold BOOT and speak, release to send. RoRoLee Muse shows the answer as text; WOWNNY shows no text, only the avatar. With a MiniMax key in the build both also speak the answer.

Muse's servers can't be reached from mainland China without a proxy for the whole network (router, or a PC proxy in TUN mode shared as a hotspot). A proxy on the PC alone doesn't help the device.

Controls:

- WOWNNY: hold BOOT to talk. VOL+ / VOL- change the volume. Hold a volume key for about a second to open the settings menu (Volume, Speaker, Wi-Fi, Reset, Exit); BOOT selects. Reset forgets the Muse pairing.
- RoRoLee Muse: hold BOOT to talk. VOL+ / VOL- change the volume. Hold VOL- for 6 s to forget the pairing.
- Chinese text on RoRoLee Muse needs a font image in `anim_pack`: see `firmware/boards/rorolee-muse/font/README.md`.

## Troubleshooting

| Symptom | What to do |
|---|---|
| No serial port appears | Use a data cable; many are charge-only. Try another USB port. Use the port the catalog's `flash_port` names (Korvo: the left USB-C). Boards with a CH340, CH9102 or CP210x USB-UART chip need its driver on Windows. |
| `Failed to connect`, or stuck at connecting | Enter download mode: hold BOOT, plug in USB (or tap RESET), release BOOT. Try 115200 baud. Close anything else holding the port, such as a serial monitor. |
| Chip mismatch | The image is for another chip (ESP32-S3 vs ESP32-P4). |
| Flashed, but the old firmware still runs | The device took an OTA update and boots from `ota_1`. Also write `ota_data_initial.bin` at `0xe000` (`idf.py flash` does), or erase otadata: `python -m esptool -p PORT erase_region 0xe000 0x2000`. |
| Boot loop, or the screen stays dark | Wrong board build. For WOWNNY try the other revision. Read the log: `idf.py -p PORT monitor`, or any serial terminal at 115200. |
| WOWNNY (WWY-01): the log stops once the screen starts | Expected. GPIO43 (UART0 TX) is the display reset, so the log continues on the chip's own USB port only. |
| Old WOWNNY board: screen dark for a second at boot | Expected. The firmware power-cycles the display. |
| WiFi: "Reconnecting" or "Platform error" | Check the network and that the platform address can be reached from it. |
| Muse: never comes online | Network access to Meta's servers, see above. |
| Build: `sdkconfig.h` lacks a `CONFIG_…` | Run `idf.py reconfigure`. A new Kconfig option needs it. |
| Build: app too large | The app slot is 2 MB and the Muse builds already use about 92% of it. |

## Repository map

```
Agent_link/
├── README.md, AGENTS.md, CLAUDE.md
├── skills/                    agent skills; agent-link-flash/references/boards.json is the generated catalog
├── tools/                     board_catalog.py (check and regenerate the catalog), install_skills.py
├── app/                       phone app (not published yet)
└── firmware/                  the ESP-IDF project: run idf.py here
    ├── CMakeLists.txt         per-board sdkconfig patching, PROJECT_VER
    ├── partitions.csv
    ├── sdkconfig.defaults     plus .esp32s3 / .esp32p4
    ├── main/                  app_main.cpp, board.h (the Board interface), Kconfig.projbuild (board and link menus)
    ├── boards/<board>/        one directory per board: config.h, config.json (incl. its catalog entries), <board>.cc
    ├── boards/common/         code shared by several boards: panels, codecs, fuel gauge, touch, camera
    └── components/
        ├── agent_link/        the SDK: core, BLE / WiFi / Muse transports, OTA, platform client
        ├── muse_gadget/       Meta's Muse Home Link; with noise_core, empty unless the Muse link is selected
        ├── minimp3/           MP3 decoder for Muse speech
        └── esp_lcd_gc9d01/    GC9D01 round panel driver
```

Read before changing code: `firmware/README.md`, `firmware/boards/README.md` (the Board interface, adding a board), `firmware/components/muse_gadget/README.md`.

## Changing the code

- Build before handing work back. A change in `components/` or `boards/common/` has to build for every board that uses it.
- Boards don't know which link they run on. Never branch on BLE / WiFi / Muse in a board; link state reaches boards as `agent_link_status_t`.
- SDK callbacks run on the transport's task (the NimBLE host stack is 6 KB). Keep them short.
- Public SDK functions must work before `agent_link_init()`, because boards are constructed first.
- A new Kconfig option needs `idf.py reconfigure`. A component's `REQUIRES` can't depend on Kconfig; components register empty instead.
- Comments are short and plain. No links or doc references in code comments.
- Secrets go in `sdkconfig` only (it is gitignored), never in `sdkconfig.defaults`.
- Code used by more than one board goes in `firmware/boards/common/`, not in a board directory.
- New board: follow "Adding a board" in `firmware/boards/README.md`, including its catalog entry in `config.json`. Then run `python tools/board_catalog.py`, which also checks that every board type in Kconfig has an entry.
- Before committing, `python tools/board_catalog.py --check` must pass.
