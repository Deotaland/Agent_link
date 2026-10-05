# The avatar on wownny-muse

When this directory holds a `muse_pixel.c`, the board shows an animated 64×64 pixel avatar
(blown up to 96×96) in the middle of the round screen, with one line under it: idle, listening
(it follows the mic level) and thinking. The link screens and the answer hide it to use the
circle for their words. Without one, those screens are text only. `main/CMakeLists.txt` picks
the file up by itself; nothing else changes.

`muse_pixel.c` is never committed (see `.gitignore`). Only the interface is tracked:
`muse_pixel.h` (from Meta's muse-gadget-sdk, Apache-2.0) and `muse_state.h` (the mode list it
needs). It is the same interface as `boards/rorolee-muse/avatar/`, so one renderer serves both
boards: copy it into each.

## Where a `muse_pixel.c` comes from

- **Meta's default character, Jollybot — internal demos only.** Copy
  `esp32/avatar/muse_pixel.c` from
  [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk) into
  this directory. It carries only a Meta copyright line and Meta states that the Apache License
  does not grant rights to it, so it must not go into a product.
- **Your own Muse's avatar.** Muse can redraw its avatar as a `muse_pixel.c` with the same
  interface: send it `esp32/tools/muse/avatar_prompt.md` from the same repository with the default
  renderer attached (the SDK's `tools/muse/AVATAR_RECIPE.md` describes the round trip), and save
  the C file it answers with here. That avatar is yours.

Delete the file to go back to the text screen. The file is looked for when CMake configures, so
after adding or removing it run `idf.py reconfigure` once before building.
