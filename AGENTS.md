# Working with Agent Link

Agent Link connects ESP32 hardware to Deotaland Agent Platform over BLE or WiFi. Muse profiles
use a separate app and cloud. Start with [README.md](README.md); use the workflows
in [skills/](skills/README.md) for device tasks.

## Working rules

- Work in the user's checkout. Inspect Git status first and preserve existing
  edits. Do not create another checkout, commit, push or merge without permission.
- Default to compiling and packaging only. Flashing or opening a serial port
  requires an explicit request for that run; do not reuse earlier flashing consent.
- For a flashing request, identify the hardware and match a published package
  or build the selected source profile, then identify the port. Browser flashing
  of published firmware does not require compilation. Use existing consent for
  the requested flash. Explain the image,
  port and settings impact before writing. A full erase needs explicit consent.
- Keep credentials out of source, public defaults, logs and shared images. Private
  build settings belong in ignored `firmware/sdkconfig.private`; `sdkconfig` is
  generated. Firmware can contain these values.
- For a bug report, inspect logs and code first. Separate facts from hypotheses,
  propose the available fixes and their tradeoffs, and wait for the user's choice
  before changing code or configuration or building a trial fix.
- Reuse `firmware/build`; build the requested profile with one or two jobs. Do not
  run a board matrix unless requested. Stop processes started for the task when
  finished; leave the user's monitors and editors alone.

## Flash a device from its name

Start with [first use](guides/getting-started.md) for new users. Before choosing
BLE confirm the unpublished Deotaland app is available; otherwise check for a
supported WiFi alternative. Muse and local-only applications have separate flows.
For an existing device, first look for matching published firmware in the
[developer console's firmware center](https://bot.deotaland.cn/#/firmware).
It supports direct browser flashing, not just downloads. This route requires
no checkout, Git, Python, ESP-IDF or compilation. Do not clone this repository
or install development tools just because the user lacks them. If GitHub is
unavailable but the firmware center is accessible, continue with browser guidance.

Limit this route to supported products with matching packages actually published
in the center. It only writes precompiled firmware; it does not edit/build source
or port new boards. Recommend current desktop Chrome/Edge on Windows/macOS with
Web Serial enabled and port permission; Chromium branding alone is insufficient.
Safari/Firefox, mobile/tablet and embedded app browsers are outside this guide.
Web Serial availability on Linux/ChromeOS is not project device-test evidence.
Check OS serial recognition and the correct USB driver before blaming firmware.

Guide desktop Chrome/Edge, console sign-in, USB data cable, hardware/chip filters,
release notes and exact revision/backend matching, then the firmware row's Flash
action and the correct serial-port selection. Use a full flash package for USB,
explain settings impact, and guide post-flash setup. Without browser control,
give these steps for the user to perform. Do not claim a write or online status
without observed evidence. A catalog profile does not guarantee a published image.

Continue with the source/local-terminal steps below only when the user needs
custom firmware, no matching release is available, or explicitly chooses that route.
For missing/inactive tools use [agent-link-setup](skills/agent-link-setup/SKILL.md)
and the [China/offline environment guide](guides/environment.zh-CN.md). From the
repository root run `python tools/check_env.py --profile PROFILE`. Locate and
activate compatible existing installations before installing anything. Use only
the selected target; preserve global Git/pip/proxy settings and other SDKs.
Browser use of matching prebuilt firmware does not need ESP-IDF.

1. Run from the repository root:

   ```sh
   python skills/agent-link-flash/scripts/find_board.py "USER'S BOARD"
   ```

   The result includes the hardware, supported firmware, build command, USB port
   and expected boot model. Use the catalog instead of asking for Kconfig symbols.
   If the model has several builds, ask only which behavior or connection the user
   wants. Confirm weak matches; never guess a board revision or chip.

2. Use an existing matching image, or build the returned profile:

   ```sh
   python firmware/tools/build_firmware.py PROFILE --jobs 2
   ```

   Activate an existing ESP-IDF 5.5.x or 6.1.x installation first. S31 needs 6.1 with
   S31 support. Do not replace an SDK installation. ESP32-P4 is not adapted and has
   no build profile. Wiring examples require a pin check before use.

3. Find the port with `python -m serial.tools.list_ports -v`. Use the catalog's USB
   connector. If several ports fit, have the user identify the device. Do not close
   an unrelated monitor to free a port.
4. Prefer the build's separate flash files, which leave NVS settings intact. From
   `firmware/`, after the requested write is authorized:

   ```sh
   idf.py -p PORT flash
   ```

   For a prebuilt merged image, use the catalog's chip and the package's offset:

   ```sh
   python -m esptool --chip CHIP -p PORT -b 460800 write_flash 0x0 IMAGE.bin
   ```

   Merged images can overwrite settings, pairing and the device ID even without a
   full-chip erase. Explain that setup and platform binding may be needed again.
   Never reuse S3 offsets for C3 or S31; use the package or `build/flasher_args.json`.

5. Read a bounded boot log at 115200 baud. Verify `board`, `model` and
   `transport` against the selected catalog entry. Keep the matching ELF for crash
   decoding. Report flashing, boot verification and online setup separately.

Browser flashing does not require the terminal steps above. Leave full-chip
erase off unless explicitly requested. If no package is available, explain the
source-build route and use a terminal-capable assistant when the user chooses it.

## Setup after flashing

| Backend | Setup                                                                                                                                  |
| ------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| BLE     | Pair from the Deotaland phone app using the catalog's `ble_name`. Confirm the user has the app; it is not publicly released.           |
| WiFi    | Join the device hotspot, open `http://192.168.4.1`, set WiFi, then claim the six-digit code in the platform console and bind an Agent. |
| Muse    | In the Muse app enable developer mode, add `MuseGadget-XXXXXX`, press BOOT to confirm, then let the app provision WiFi.                |

WiFi hotspots include `KorvoCloud-XXXX`, `M5StopWatch-XXXX` and `PaperS31-XXXX`.
Agent selection happens in the platform console; devices manage that Agent's
conversations. The configured platform is [bot.deotaland.cn](https://bot.deotaland.cn).
If it becomes unavailable, confirm the replacement address with the user rather
than silently changing the compiled URL.

Muse needs network access to its services. On restricted networks the device may
need a router-level proxy. Optional MiniMax credentials enable spoken replies;
see the [Muse notes](firmware/components/muse_gadget/README.md).

## Board catalog

Generated from `firmware/boards/*/config.json` and `firmware/profiles/profiles.json`.
Run `python tools/board_catalog.py` after changing either.
`supported` means a source profile exists, not completed device acceptance.
Read each entry's notes for hardware variants and pending tests; first actions
are listed in the [device guide](guides/devices.md).

<!-- boards:begin -->
| Model | Board | Profile | Chip | Link | Status |
| --- | --- | --- | --- | --- | --- |
| RRL-01 | RoRoLee | `work-badge-ble` | ESP32-S3 | BLE | supported |
| RRL-01 | RoRoLee | `rorolee-muse` | ESP32-S3 | Muse | supported |
| RRL-01 | RoRoLee | `rorolee-ble` | ESP32-S3 | BLE | supported |
| WWY-01 | WOWNNY | `wownny-muse` | ESP32-S3 | Muse | supported |
| WWY-01-old | WOWNNY (old board) | `wownny-old-muse` | ESP32-S3 | Muse | old revision |
| AI-PASSPORT | FoloToy AI Passport | `ai-passport-ble` | ESP32-C3 | BLE | supported |
| AI-PASSPORT | FoloToy AI Passport | `ai-passport-wifi` | ESP32-C3 | WiFi | supported |
| Korvo 2 V3 | ESP32S3 Korvo 2 V3 development board | `korvo-ble` | ESP32-S3 | BLE | supported |
| Korvo 2 V3 | ESP32S3 Korvo 2 V3 development board | `korvo-wifi` | ESP32-S3 | WiFi | supported |
| C152 | M5Stack StopWatch | `m5stack-stopwatch-ble` | ESP32-S3 | BLE | supported |
| C152 | M5Stack StopWatch | `m5stack-stopwatch-wifi` | ESP32-S3 | WiFi | supported |
| paper V2 | Mozhifang paper V2 (3.6-inch six-color e-paper) | `paper-s31-wifi` | ESP32-S31 | WiFi | supported |
| OJBadge | OJBadge | `ojbadge-wifi` | ESP32-S3 | — | supported |
| ESP32-S3 AIoT Basic V2 | ESP32-S3 AIoT Basic V2 development board | `gc2145-camera-ble` | ESP32-S3 | BLE | supported |
| ESP32-S3-Touch-AMOLED-1.75 | Waveshare ESP32-S3-Touch-AMOLED-1.75 (non-C) | `waveshare-amoled-175-wifi` | ESP32-S3 | WiFi | supported |
| ESP32-S3-Touch-AMOLED-1.75C | Waveshare ESP32-S3-Touch-AMOLED-1.75C | `waveshare-amoled-175c-wifi` | ESP32-S3 | WiFi | supported |
| ESP32-S3-ePaper-3.97 | Waveshare ESP32-S3-ePaper-3.97 | `waveshare-epaper-397-wifi` | ESP32-S3 | WiFi | supported |
| — | Wiring example: ESP32-S3 + ES8311 (mic only) | `es8311-asr-ble` | ESP32-S3 | BLE | wiring example |
| — | Wiring example: ESP32-S3 + ES8311 (speaker and mic) | `es8311-voice-ble` | ESP32-S3 | BLE | wiring example |
| ESP32-P4-WIFI6-Touch-LCD-7B | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B | — | ESP32-P4 | — | **not adapted; do not flash** |
<!-- boards:end -->

## Add a board

Read [the board guide](firmware/boards/README.md), the
[application interface](firmware/apps/common/application.h) and the
[SDK guide](firmware/components/agent_link/README.md). The
[porting skill](skills/agent-link-port/SKILL.md) covers the same workflow for AI assistants.

- Keep pins and hardware initialization in `boards/`, UI and sessions in `apps/`,
  and connectivity in `components/agent_link/`. Shared drivers take pin settings;
  they do not depend on the transport.
- Register one `Application` with `DECLARE_APPLICATION`. Preserve existing
  `Model()` strings for OTA compatibility and advertise only working capabilities.
- Query `/api/products/all` with the porting skill before assigning a WiFi product
  ID. Match official boards directly; custom boards may reuse a compatible
  registered product while keeping their own hardware and firmware model.
- Copy or queue callback data and return promptly. BLE callbacks can run on a
  6 KB NimBLE host stack. Do not perform display, storage or blocking audio work there.
- Public SDK calls must tolerate use before `agent_link_init()`; applications are
  constructed first. Use `agent_link_status_t` to display link progress.
- Add a profile, Kconfig mappings, defaults and catalog entry together. Select
  driver dependencies from the profile manifest before IDF dependency expansion;
  do not decide component `REQUIRES` using Kconfig values.
- Build the requested profile before handing back firmware code changes. Hardware
  acceptance requires a device; a successful build is not a device test.

## Checks before merge

Follow the [logging and maintenance guide](guides/logging.md). Keep normal logs
at Info, repeated diagnostics at Debug and failures visible at Warn/Error. Never
log conversation content or credentials, including at Debug. Keep state changes
outside logging arguments and preserve third-party headers.

Run from the repository root:

```sh
python firmware/tools/build_firmware.py --check
python tools/board_catalog.py --check
python tools/check_docs.py
```

For firmware changes, also build the selected profile and run the relevant host
checks under `firmware/tests/`. Keep validation proportional to the change. State
which device behavior still needs testing. Never upload images built with secrets.
