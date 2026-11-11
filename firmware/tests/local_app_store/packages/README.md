# Manually supplied application firmware

Copy the installable `voice.bin` or other compatible application
images directly into this directory, then restart `server.py`.
Files may be renamed; identity, version, size and SHA-256 come from the image.
Keep one image per app ID. No `package.json` is required.

The server scans this directory only. It skips factory, bootloader, merged USB
packages and incompatible images, with a diagnostic message. Non-BIN files are
ignored. Firmware here is local test data and is ignored by Git.
# Sugarat Button test set

Select `--profile sdgoods-wifi` to serve the current native `agent_link.bin` and
the copied SDGOODS pure applications. The native binary is kept unchanged.
This profile uses physical V4 with a 10 MiB dynamic pool and application ABI 3.
The per-image limit is 10,428,416 bytes; apps share the pool and each reserves
56 KiB for its cover. Runtime allocation is aligned to 64 KiB.
Bootloaders, partition tables, recovery launchers and full-device packages are
not copied here. See the service README and `.cache` reports for test results.
