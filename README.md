# Agent Link

Connect ESP32 devices to the Deotaland Physical Agent platform over BLE or WiFi. The SDK
handles connectivity, device I/O, audio streams and OTA; applications provide the
hardware and user interface. Existing Muse firmware is also included.

[Chinese README](README.zh-CN.md) · [Firmware guide](firmware/README.md) ·
[Add a board](firmware/boards/README.md) · [SDK](firmware/components/agent_link/README.md) ·
[Developer console / Firmware center](https://bot.deotaland.cn/#/firmware)

## Start here

| Goal | Guide |
| --- | --- |
| Install published firmware directly in the browser | [Firmware center](https://bot.deotaland.cn/#/firmware) / [First-use guide](guides/getting-started.md) |
| Download the Rorolee phone app | iOS: search **rorolee** in an overseas App Store; Android: [rorolee.com](https://rorolee.com). [App setup](app/readme.md) |
| Prepare a new computer or fix downloads in China | [Environment setup, mirrors and offline installation](guides/environment.md) |
| Find hotspot names and first actions | [Device guide](guides/devices.md) |
| Customize or add hardware | [Firmware](firmware/README.md) / [Board porting](firmware/boards/README.md) |

BLE uses the **Rorolee phone app**: search **rorolee** in an overseas App Store
for iOS, or download the Android APK from [rorolee.com](https://rorolee.com).
Use a supported WiFi profile for standalone access. Muse uses its own app/cloud.
Catalog `supported` means a source profile exists; read device-acceptance caveats.

## Flash in your browser (recommended for existing firmware)

The [Agent Link developer console's firmware center](https://bot.deotaland.cn/#/firmware)
lets you **flash published firmware directly from the web page** or download it.
You do not need this GitHub repository, Python, ESP-IDF or a local build to use
browser flashing.

### Scope and requirements

| Item | Limit |
| --- | --- |
| Products | Only products already supported by Agent Link **and** having a matching published package in the firmware center. Match the exact board revision and BLE/WiFi purpose; the chip name alone is insufficient. |
| Web action | Write the selected, already compiled firmware over USB. The page does not edit source, compile custom firmware or adapt an unsupported board. |
| Recommended computer/browser | **Windows or macOS, current desktop Chrome or Microsoft Edge**, with Web Serial available and serial-port permission granted. A Chromium-based browser is not automatically compatible. |
| Other browsers/devices | Safari, Firefox, mobile/tablet browsers and embedded app browsers are outside this browser-flashing guide. Use the recommended desktop browser. |
| USB connection | A data cable, the correct connector and a serial device recognized by the OS are required. Install the appropriate USB driver if needed. |

[Web Serial](https://developer.chrome.com/docs/capabilities/serial#browser-support)
also exists in desktop Chrome on Linux/ChromeOS; that alone does not establish
firmware-center acceptance on those systems. Windows/macOS are the documented
recommended route here.

1. Open the firmware center in desktop Chrome or Edge on Windows/macOS and sign in.
2. Connect the device with a USB data cable. Filter by hardware product and
   chip, then check the release notes for the exact board/revision and intended
   behavior (for example, BLE or WiFi). For USB installation choose a matching
   **full flash package**.
3. Click **Flash** on that firmware row, select the device's serial port in the
   browser prompt, and follow the page's instructions until writing completes.
4. Let the device restart, then follow the [first-use guide](guides/getting-started.md)
   for WiFi setup, activation and Agent binding, or the selected app's pairing flow.

Read the package's settings impact before writing; full packages can replace
saved WiFi and pairing. Keep full-chip erase off unless you intend to erase it.
If your required firmware has not been published, ask a local coding assistant
to build the matching source profile using the environment guide below.

You can also give an AI assistant just the firmware-center link and your device
model, without a local checkout:

> I have a FoloToy AI Passport and want WiFi voice. Guide me through flashing
> published firmware at https://bot.deotaland.cn/#/firmware and completing setup.
> I do not have the source repository or a build environment; use browser flashing first.

## Flash with an AI assistant

Open this repository in your coding assistant and tell it what you have:

> I have an M5Stack StopWatch. Flash Agent Link firmware onto it.

[AGENTS.md](AGENTS.md) tells the assistant how to identify the board, choose
matching published firmware first, or build a source profile when needed, find
the USB port and verify the result. It asks about the connection only when the hardware supports several
firmware choices. You do not need to know the chip or configuration symbols.

Connect a USB data cable. BLE firmware needs the [Rorolee phone app](app/readme.md); WiFi
firmware uses hotspot setup and a six-digit activation code in the
[platform console](https://bot.deotaland.cn). Prebuilt images are available in the
[firmware center](https://bot.deotaland.cn/#/firmware).

BLE control and OTA require encrypted pairing. Changing phones requires a local
pairing reset; OTA requests must include the firmware model. See the
[pairing and OTA compatibility notes](firmware/components/agent_link/README.md#ble-pairing-and-ota-compatibility).

## Build from source

New computer: follow [environment setup](guides/environment.md) first. Prefer
5.5.4 for new S3/C3 installs; reuse compatible 5.5.x/6.1.x installations.
Identify your profile, then run from the repository root (AI Passport WiFi example):

```sh
python tools/check_env.py --profile ai-passport-wifi
python firmware/tools/build_firmware.py ai-passport-wifi --jobs 2
```

Alternatively choose explicitly from the build tool's interactive menu in firmware/.
The environment check verifies tools; a build is a separate result.
The tool reuses `firmware/build`, runs two
compiler jobs and creates app, merged and debugging files. ESP32-S31 profiles
require ESP-IDF 6.1 with S31 support. See the [firmware guide](firmware/README.md)
for configuration and flashing details.

## Add hardware

Start with the [board guide](firmware/boards/README.md), or ask your assistant:

> Add my ESP32-S3 board using the WiFi backend. Here are its schematic and pins.

Keep wiring and drivers in `firmware/boards/`, application behavior in
`firmware/apps/`, and select the combination in `firmware/profiles/`. BLE and
WiFi use the same SDK callbacks and device I/O model.

## Repository

| Path | Contents |
| --- | --- |
| [firmware/](firmware/README.md) | ESP-IDF project, SDK, hardware and applications |
| [skills/](skills/README.md) | AI workflows for flashing, building, porting and diagnosis |
| `tools/` | Board catalog, documentation checks and skill installer |
| [app/](app/readme.md) | Phone app placeholder |

MIT; see [LICENSE](LICENSE). Vendored code and fonts retain their own licenses.
