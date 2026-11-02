---
name: agent-link-flash
description: Identify an Agent Link device and guide direct browser flashing of published firmware without a source checkout or SDK, or use local flashing when needed, then complete BLE, WiFi or Muse setup. Use for device installation, not firmware changes.
---

# Flash an Agent Link device

Read `AGENTS.md` when the user's checkout is available. Without one, the
[repository instructions](https://github.com/Deotaland/Agent_link/blob/main/AGENTS.md)
are useful but not a prerequisite for browser flashing. Do not require a clone
or install Python/ESP-IDF just to guide an existing published firmware install.

## Prefer published firmware for existing devices

The [firmware center](https://bot.deotaland.cn/#/firmware) is in the developer
console and can flash directly in the browser. Check it before proposing a build.
This route works without this repository, Git, Python, ESP-IDF or compilation.
If GitHub is unavailable, continue using the firmware center and known hardware
identity; do not make repository access a prerequisite.

Browser flashing is limited to supported products with exact matching published
packages. It writes precompiled images, not source editing, online compilation
or new-board adaptation. Recommend Windows/macOS and current desktop Chrome/Edge
with Web Serial and serial permission. Do not promise compatibility based only
on Chromium branding, or guide Safari/Firefox/mobile/embedded browsers through
this route. Linux/ChromeOS API availability is not firmware-center acceptance.
The OS must recognize the correct serial device; a driver may still be needed.

Identify the exact board/revision and intended BLE/WiFi/Muse/local behavior.
Use the bundled `references/boards.json` if no checkout/Python is available;
otherwise use the lookup helper below. A broad vendor name needs clarification.
Confirm BLE app availability before selecting BLE; the Deotaland app is unpublished.

Guide the user through desktop Chrome/Edge, console sign-in, a USB data cable,
hardware-product/chip filters and release notes. Match board revision and backend;
a supported source profile alone does not prove a matching release is published.
Choose the corresponding full flash package, explain settings impact, click the
firmware row's Flash button and identify its device in the browser port chooser.
Leave full-chip erase off unless separately authorized. If browser control is
unavailable, walk the user through those same steps; do not substitute a build
just because the agent cannot operate the browser.

After writing, verify the page result and device restart. Guide WiFi hotspot
setup at `http://192.168.4.1`, restoring phone internet, console activation with
the device's current six-digit code, Agent binding and the first interaction.
Already activated devices may go online directly. BLE uses the existing phone
app; Muse uses its own app/developer mode and BOOT confirmation. Local-only
profiles do not use platform activation. Report browser write, observed boot
behavior and online acceptance separately; do not invent a serial-log check.

Use the local workflow below only for custom firmware, an unavailable matching
release, or the user's explicit local-flashing choice.

## Local workflow: identify and prepare

Run `python scripts/find_board.py "USER'S BOARD"` from this skill's folder.
Chinese aliases are accepted. A broad vendor name can return several boards;
confirm the actual model, revision and display before choosing.
The output gives compatible profiles, build commands, chip, USB connector and
boot model. Model names are enough to start; do not ask the user for Kconfig
symbols. If one model has several firmware choices, ask which behavior or
connection they want. Confirm weak matches. Do not flash `not-adapted` boards;
check pins before using a wiring example. `supported` denotes an available source
profile; read its acceptance caveats and report pending hardware tests.
Before selecting BLE, confirm the user already has the unpublished Deotaland app.
For a supported WiFi alternative, explain that it works without that app. Muse
uses its own app/cloud. Do not promise voice on a display-only/local-only profile.

Use an existing matching package, the firmware center, or build the returned
profile in the user's checkout with `firmware/tools/build_firmware.py`. Follow
[the build skill](../agent-link-build/SKILL.md) when available. Never infer flash
offsets from another chip's example. Prefer `idf.py flash` for a local build;
use its `flasher_args.json` to inspect the files and offsets.

When building is needed, run `python tools/check_env.py --profile PROFILE` from
the repository root. Follow the setup skill or `guides/environment.zh-CN.md`
for missing/inactive SDKs and China/offline downloads. Existing matching images
and the browser firmware center do not require a full SDK installation.

## Local workflow: write and verify

Find ports read-only with `python -m serial.tools.list_ports -v`. Resolve multiple
matching ports with the user. Explain the chosen image, port and settings impact
before writing. Use existing authorization for the requested flash; a connected
cable alone is not permission. A merged image can overwrite WiFi, pairing and
device identity. A full-chip erase needs explicit approval.

Flash with the selected chip and package layout. For connection failures, check
the data cable, BOOT/reset sequence and port ownership, then retry at 115200.
Do not erase settings as a troubleshooting shortcut or close another program.

Read a bounded 115200-baud boot log and verify its model and transport. Opening
the port can reset the board. Do not leave a monitor running after the task.
Guide BLE app pairing, WiFi hotspot/activation or Muse pairing using AGENTS.md
and `guides/getting-started.zh-CN.md`; use `guides/devices.zh-CN.md` for the
selected profile's first action. WiFi success still needs console activation,
Agent binding and a real first interaction. Explain recovery without erasing.
Report what was flashed, what the boot log confirmed and what setup or physical
checks remain. Do not claim online status from a successful write alone.
