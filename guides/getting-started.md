# First use

[Chinese walkthrough](getting-started.zh-CN.md) · [Environment](environment.md) · [Device actions](devices.md)

## Choose a route

| Goal | Start here |
| --- | --- |
| Use an existing device | Identify exact hardware/revision, choose firmware and install a matching image |
| Build or customize | Prepare the [environment](environment.md), then build one selected profile |
| Add hardware | Supply schematic/pins and follow the [board guide](../firmware/boards/README.md) |
| Diagnose a failure | Record hardware, profile, step and sanitized error/log; analyze before changing firmware |

Use a USB data cable. A chip name or vendor alone does not identify a board.
Confirm the AMOLED C revision, WOWNNY revision and Korvo display variant.
P4 is not adapted; wiring examples require a pin check.

WiFi connects directly and requires a supported WiFi profile, 2.4 GHz network
and console account. BLE uses the **Rorolee phone app**: search **rorolee** in an
overseas App Store for iOS, or download the Android APK from
[rorolee.com](https://rorolee.com). Follow [app setup](../app/readme.md) before pairing.
Muse uses its own app/cloud.
Local-only and display-only profiles do not acquire voice just by installing them.

## Install

Use desktop Chrome/Edge and the [firmware center](https://bot.deotaland.cn/#/firmware)
for an existing matching package. It flashes directly in the browser; no local
checkout, Python, ESP-IDF or compilation is required. An AI assistant can guide
this route using only the device identity and firmware-center link, even when
GitHub is unavailable. Sign in, filter by hardware product/chip, check release
notes and select the matching full flash package. Click Flash and choose the
device's serial port. Downloading the file first is optional.
This route only writes precompiled releases for already supported products;
it cannot edit/build source or add support for another board. Use Windows/macOS
with current desktop Chrome/Edge providing Web Serial and serial permission.
Chromium branding alone is insufficient; Safari/Firefox/mobile/embedded browsers
are outside this guide. OS USB/serial drivers may still need installation.
Linux/ChromeOS Web Serial capability has not established this project's browser
flashing acceptance there.
Check hardware, purpose, package type and settings impact. If no matching image
exists, use a terminal-capable assistant to prepare tools/build; do not guess.
Identify the correct port/connector. No port: check power, cable and actual USB
driver. Do not close another task's monitor. Keep full-chip erase off.

Merged images can replace settings even without a full erase. Prefer segmented
`idf.py flash` for local builds. App-only images are for OTA, not a full package
written at zero. Use package offsets, never another chip's example. Report write,
boot-model verification and online acceptance separately.

## Connect and verify

WiFi: join the displayed hotspot, keep the connection despite the phone's
no-internet warning, open `http://192.168.4.1`, and enter 2.4 GHz credentials.
After success restore the phone's internet, read the device's current six-digit
code, sign in to the [console](https://bot.deotaland.cn), activate/add the device
and bind a voice-capable Agent. An already activated device may go online without
a new code. If no Agent is available, resolve that in the console/account first.
Choose/create a conversation as required by the [device table](devices.md), start
the first interaction, verify a reply and end it. E-paper refresh takes time.

BLE: add the advertised name from inside Rorolee with Bluetooth/nearby
permissions. System Bluetooth connection alone does not verify encrypted app
pairing. Use the exact board's local pairing reset before changing phones.
Muse: enable app developer mode, add MuseGadget-XXXXXX, press BOOT to confirm and
let the app provision WiFi. Hold BOOT to record and release to send. Speech may
require private MiniMax configuration; see the [Muse guide](../firmware/components/muse_gadget/README.md).

## Recover

Wrong WiFi password: retry from the portal, without reflashing. With an invalid
saved network, failed reconnect opens setup after about 20 seconds; board-specific
settings may also reopen it. Changing WiFi need not reset activation. A portal
that stops polling may have lost the phone's hotspot connection; check the actual
device status before diagnosing a failure.

Already bound: unbind in the original console account; a local reset does not
release cloud ownership. No Agent: bind one and check voice capability/session.
Black screen/reboot: collect hardware identity and bounded logs for diagnosis,
not repeated erases. Share no passwords, tokens or device keys.
