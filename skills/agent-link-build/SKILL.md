---
name: agent-link-build
description: Build and package one supported Agent Link firmware profile using ESP-IDF 5.5.x or 6.1.x, with the correct chip, application and backend. Use for firmware builds or when flashing needs a local image. Does not flash.
---

# Build Agent Link firmware

Use the user's checkout and preserve existing edits. Read `AGENTS.md` and
`firmware/profiles/README.md`. Identify the board with the flash skill's
`scripts/find_board.py`; use the profile it prints. Ask about the backend only
when the hardware has several supported choices.

## Environment and build

Activate an existing ESP-IDF installation. Supported versions are 5.5.x and
6.1.x; S31 requires an S31-capable 6.1 SDK. If tools are missing, locate an
existing installation before proposing a download. Do not replace another SDK.

From the checkout's `firmware/` directory:

```sh
python tools/build_firmware.py --list
python tools/build_firmware.py PROFILE --jobs 2
```

The tool selects the chip, applies defaults, runs reconfigure, builds in the
existing `build/`, merges the image and checks its segments. Do not set board
and transport symbols manually or use the old `set-target` workflow. Do not run
several large builds or create a new build directory for each profile.

Supported private settings belong in ignored `sdkconfig.private`. The first
migration saves the previous config once and imports recognized credentials.
Changing profiles can regenerate `sdkconfig` and drop other local options;
inspect their names without printing values before changing the selection.
Do not add credentials to public defaults or distribute images containing them.

## Report

Confirm the selected profile, chip, backend and build result. Give paths to
`build/agent_link.bin`, `build/merged.bin`, and matching ELF/MAP and flash metadata.
The app image is for OTA; the merged image is a USB full package that can replace
settings. State clearly that no device was flashed. Hardware behavior remains
unverified until tested on the identified board.
