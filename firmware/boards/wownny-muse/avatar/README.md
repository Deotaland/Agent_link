# Optional Muse avatar

Place a compatible `muse_pixel.c` in this directory to replace the pulsing disc
with an animated avatar. The renderer uses the tracked `muse_pixel.h` and
`muse_state.h` interface. One renderer can be used for both RoRoLee and WOWNNY;
copy it into each board's avatar directory.

The generated C file is ignored by Git. `main/CMakeLists.txt` includes it when
present. After adding or removing it, rebuild the selected Muse profile with
`tools/build_firmware.py`, which reconfigures automatically.

Use your own renderer with this interface. The upstream Muse SDK includes avatar
generation instructions and a default Jollybot renderer; the default character
is for internal demos and is not covered by the code's Apache license for use
in a product. See [muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
for the source and asset terms. Removing `muse_pixel.c` restores the fallback.
