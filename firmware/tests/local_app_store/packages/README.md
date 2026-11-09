# Manually supplied application firmware

Copy the installable `voice.bin` or other compatible application
images directly into this directory, then restart `server.py`.
Files may be renamed; identity, version, size and SHA-256 come from the image.
Keep one image per app ID. No `package.json` is required.

The server scans this directory only. It skips factory, bootloader, merged USB
packages and incompatible images, with a diagnostic message. Non-BIN files are
ignored. Firmware here is local test data and is ignored by Git.
