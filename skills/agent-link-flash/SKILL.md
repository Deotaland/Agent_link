---
name: agent-link-flash
description: Identify a user's Agent Link board (RoRoLee, WOWNNY, Korvo, AIoT Basic and others) from its model, company, shop, Bluetooth name or looks, then flash Deotaland Agent Link firmware onto it with the user's consent and get it online over BLE, WiFi or Muse. Use when someone wants to flash, reflash, update or set up such a device. Not for changing firmware code.
---

**English** | [简体中文](SKILL.zh-CN.md)

# Flash an Agent Link device

The rules and every detail (flash addresses, setup steps, troubleshooting) are in `AGENTS.md`: read it in the user's checkout, or at https://github.com/Deotaland/Agent_link/blob/main/AGENTS.md. This skill is the order of work.

## 1. Identify the board

Run `python scripts/find_board.py "<what the user said>"` from this skill's folder. Pass whatever the user gave: model, product name, company, shop or link, the name it shows over Bluetooth, what it looks like.

- Several firmware builds for one hardware: ask what the device should do, then use that build.
- No match or a weak one: ask for more (the text printed on the board, a photo, the order page). Never flash firmware for a board you couldn't identify.
- `not-adapted`: stop and tell the user this board isn't supported yet.
- `example`: only for parts the user wired themselves. Check their wiring against the board's `config.h` first.

## 2. Get an image

1. **Browser flasher** (AGENTS.md path A): the user flashes from the firmware center while you guide them. Use this when you can't run commands. The firmware center needs the user's login, so you can't download from it yourself.
2. **An image you already have** plus esptool (path B).
3. **Build it** (path C): use the `agent-link-build` skill.

## 3. Get consent

- Find the port read-only: `python -m serial.tools.list_ports -v`. Several ports: ask which one. None: ask the user to plug the board in with a data cable, using the port in the catalog's `flash_port` (Korvo: the left USB-C).
- Before writing, tell the user the port, the image, the address and what happens to the data:
  - `idf.py flash`, or the four build files at their offsets: settings and pairing stay.
  - A merged image at `0x0`, or an erase: WiFi, pairing and the device ID are wiped, and the device must be set up and bound on the platform again.
- A connected device is not consent. A full erase needs its own yes.

## 4. Flash

Use the commands in AGENTS.md path B or C. If it fails, check download mode (hold BOOT and replug), the cable, a program holding the port, or try 115200 baud. Retry only after changing something. Don't erase the chip to fix a failed flash unless the user agrees.

## 5. Check and set up

- Read the boot log (115200 baud, or `idf.py -p PORT monitor`; opening the port can reset the device). `model = …` must equal the catalog's `fw_model`. If the old firmware still runs, see "Flashed, but the old firmware still runs" in AGENTS.md.
- Walk the user through setup for the board's link (AGENTS.md "After flashing"): BLE and the Deotaland App; WiFi hotspot plus the 6-digit code in the console; or the Muse app plus a BOOT press.

## 6. Report

Report these separately:
- **Flashed:** the port, image and address.
- **Boot log checked:** the board, model and link it reported.
- **Set up:** online, or the step the user is on.
- **Not verified:** what only the user can confirm (screen, sound, pairing).
