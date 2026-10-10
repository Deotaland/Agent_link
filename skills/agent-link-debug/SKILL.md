---
name: agent-link-debug
description: Diagnose a Deotaland Agent Link device from its serial log and symptoms - no serial port, failed flashing, boot loops, dark screen, link stuck on BLE/WiFi/Muse, crashes with backtraces. Use when something goes wrong after flashing or at runtime. Read-only unless the user asks for a fix.
---

**English** | [简体中文](SKILL.zh-CN.md)

# Diagnose an Agent Link device

Read-only unless the user asks for a fix. The symptom table is in AGENTS.md, "Troubleshooting".

## 1. Collect

- **What the user sees**, and which board it is: run `agent-link-flash`'s `scripts/find_board.py` with whatever they know.
- **What is running:** the boot log lines `board = …, model = …, fw = …` and `init: … transport=…`.
- **The serial log:** use `idf.py -p PORT monitor`, which decodes backtraces when `build/agent_link.elf` matches the flashed image, or any serial terminal at 115200.
  - Opening the port can reset the device; ask first if that matters.
  - On WOWNNY (WWY-01) the log continues on the USB port only once the screen starts.
- **Link status:** lines starting `[link]` carry it: the title and hint the device shows. Quote them to the user.

## 2. Usual causes

| Symptom | Check |
|---|---|
| No port, or can't connect | Data cable, download mode (hold BOOT and replug), the right port (Korvo: left USB-C), something else holding the port |
| Old firmware still runs after flashing | `otadata` points to `ota_1` after an OTA update: write `ota_data_initial.bin` at `0xe000` or erase that region |
| Boot loop right after flashing | Wrong board type or target; a different PSRAM/flash size than the board needs (16 MB flash, octal PSRAM) |
| Screen dark | Wrong build for the board (WOWNNY: try the other revision); boards marked `not-adapted` |
| WiFi: "Platform error", "Reconnecting", "Already bound", "No agent" | Network, platform reachable, binding state in the console |
| Muse never online | Network access to Meta's servers (blocked from mainland China without a network-wide proxy) |
| `muse_tts: no speech: HTTP …` | MiniMax key or quota; the text after the status is MiniMax's answer |
| `Guru Meditation` / backtrace | Decode with the matching ELF only: `xtensa-esp32s3-elf-addr2line -pfiaC -e build/agent_link.elf <addresses>` |
| Build: `CONFIG_…` missing | `idf.py reconfigure` after Kconfig changes |

With no ELF matching the flashed image, say so: don't guess source lines.

## 3. Report

Report:
- what the log shows, quoting the lines;
- the likely cause, and how sure you are;
- the fix you propose;
- what you couldn't check.

Change code, rebuild or flash only after the user agrees.
