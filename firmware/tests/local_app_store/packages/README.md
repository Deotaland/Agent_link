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
This profile uses V3 and two 5 MiB slots; each image is limited to 5,185,536 bytes.
Bootloaders, partition tables, recovery launchers and full-device packages are
not copied here. See the service README and `.cache` reports for test results.
