# The avatar on rorolee-muse

When this directory holds a `muse_pixel.c`, the board shows an animated 64×64 pixel avatar
(blown up to 96×96) beside the text: idle, listening (it follows the mic level), thinking, and
speaking while an answer comes in. Without one, the screen is text only. `main/CMakeLists.txt`
picks the file up by itself; nothing else changes.

`muse_pixel.c` is never committed (see `.gitignore`). Only the interface is tracked:
`muse_pixel.h` (from Meta's muse-gadget-sdk, Apache-2.0) and `muse_state.h` (the mode list it
needs).

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
