---
name: agent-link-flash
description: Identify an Agent Link board from its name, model, shop, Bluetooth name or appearance, select matching firmware, flash it and guide BLE, WiFi or Muse setup. Use for flashing and device setup, not firmware changes.
---

# Flash an Agent Link device

Read `AGENTS.md` in the user's checkout. Without a checkout, use the
[repository instructions](https://github.com/Deotaland/Agent_link/blob/main/AGENTS.md).

## Identify and prepare

Run `python scripts/find_board.py "USER'S BOARD"` from this skill's folder.
The output gives compatible profiles, build commands, chip, USB connector and
boot model. Model names are enough to start; do not ask the user for Kconfig
symbols. If one model has several firmware choices, ask which behavior or
connection they want. Confirm weak matches. Do not flash `not-adapted` boards;
check pins before using a wiring example.

Use an existing matching package, the firmware center, or build the returned
profile in the user's checkout with `firmware/tools/build_firmware.py`. Follow
[the build skill](../agent-link-build/SKILL.md) when available. Never infer flash
offsets from another chip's example. Prefer `idf.py flash` for a local build;
use its `flasher_args.json` to inspect the files and offsets.

## Write and verify

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
Guide BLE app pairing, WiFi hotspot/activation or Muse pairing using AGENTS.md.
Report what was flashed, what the boot log confirmed and what setup or physical
checks remain. Do not claim online status from a successful write alone.
