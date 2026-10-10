---
name: agent-link-build
description: Build Deotaland Agent Link firmware from source for a given board with ESP-IDF 5.5. Sets up or activates ESP-IDF, selects the board and link without menuconfig, applies the board's own settings, builds, and produces a merged image. Use for "build/compile the firmware for board X" or when no prebuilt image fits. Does not flash.
---

**English** | [简体中文](SKILL.zh-CN.md)

# Build Agent Link firmware

Work in the user's checkout; if there is none, clone https://github.com/Deotaland/Agent_link. Run `git status --short --branch` first and leave unrelated changes alone. Details are in AGENTS.md, "Path C".

## 1. Environment

- `idf.py --version` must report ESP-IDF v5.5.x.
- Not on PATH: look for an existing install and activate it before installing anything. On Windows use the ESP-IDF PowerShell shortcut or `export.ps1`; on macOS/Linux run `. $IDF_PATH/export.sh`.
- Installing ESP-IDF downloads a lot and needs the user's OK. Don't replace another IDF version.

## 2. Configure

- Board type: from `agent-link-flash` (`scripts/find_board.py` prints the settings) or from the user. The target (`esp32s3` / `esp32p4`) is in the board's `config.json`.
- Don't open menuconfig. Write a defaults file **outside the repository** with the two lines `find_board.py` printed:
  ```
  CONFIG_BOARD_TYPE_<BOARD_TYPE>=y
  CONFIG_AGENT_LINK_TRANSPORT_<LINK>=y
  ```
  For a Muse build also add `CONFIG_GADGET_SDK_TOKEN` and, if wanted, `CONFIG_MUSE_TTS_MINIMAX_KEY`, values from the user, never printed or committed.
- Then:
  ```
  idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;<absolute path of the file>" set-target <target>
  idf.py reconfigure
  ```
  `set-target` replaces `sdkconfig` (the old one becomes `sdkconfig.old`). If the user's `sdkconfig` holds keys, say so before running it.
- Check:
  - the reconfigure log shows `agent_link: patched sdkconfig for BOARD_TYPE_…`;
  - `build/config/sdkconfig.h` defines `CONFIG_BOARD_TYPE_<BOARD_TYPE> 1`.
  Without the patch the board's own settings are missing and the Muse boards run out of RAM.

## 3. Build

- Run `idf.py build`. Fix the warnings and errors your changes caused, and report the others.
- A failed build is a failure: an older file left in `build/` is not the result.
- Merged image: `cd build && python -m esptool --chip <target> merge_bin -o merged.bin @flash_args`.
- Check the size line `agent_link.bin binary size … (N%) free`. The app slot is 2 MB.

## 4. Report

Report:
- the board type, link and IDF version;
- the warnings;
- the files: bootloader, partition table, `ota_data_initial.bin`, app and the merged image;
- the free space.

Images with a token or key inside stay private. Building doesn't authorize flashing: continue with `agent-link-flash`.
