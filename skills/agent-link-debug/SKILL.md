---
name: agent-link-debug
description: Diagnose Agent Link flashing or runtime problems from board identity, serial logs and symptoms, including boot loops, dark screens, link failures and crashes. Analyze first and wait for the user's choice before implementing a fix.
---

# Diagnose an Agent Link device

Read `AGENTS.md`. Identify the board, firmware profile and reported boot model;
use the flash skill's `scripts/find_board.py` when the user only knows its name.
Collect the symptom and a bounded serial log. Keep credentials and identifiers
out of shared output. Opening a port may reset the device.

| Symptom | Inspect |
| --- | --- |
| No port or failed connection | Cable, catalog USB connector, BOOT/reset and port ownership |
| Old firmware after flashing | Selected OTA slot; compare package otadata and build metadata |
| Boot loop or black screen | Board revision, detected chip, flash/PSRAM config and panel driver |
| WiFi blocked | Network, compiled URL, activation and platform Agent binding |
| Muse offline | Pairing state and device network access to Muse |
| No speech | Negotiated format, audio queues, codec state; Muse TTS key/error when relevant |
| Crash | Full backtrace and the ELF from the exact flashed build |

Decode only with a matching ELF and the target's toolchain. C3 and S31 use
RISC-V tools; do not use the S3 Xtensa decoder for them. Without a matching ELF,
state that source locations cannot be verified.

Report confirmed log evidence, likely causes and uncertainty. Offer the feasible
fixes with tradeoffs and a recommendation; one feasible fix still needs the
user's confirmation. Wait for their choice before changing code/configuration,
building a trial fix, flashing or erasing. Put an approved fix in production
source; tests are supporting verification.
