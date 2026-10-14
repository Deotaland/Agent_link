# AI Passport

ESP32-C3, 8 MB flash, ST7789 240x320, ES8311 full duplex at 16 kHz, PCM16 mono.
The BLE build sends microphone audio to the phone and plays its replies.

The UI draws directly into the panel driver's single 240x4 RGB565 DMA stripe
(1,920 bytes). It allocates no framebuffer or LVGL heap. Colors, layout,
Montserrat 14/20/28px glyph coverage and kerning match the previous UI.
The read-only font assets in `boards/common/raster_montserrat_*.c` retain their
upstream license. Unsupported characters use a placeholder, as with the old fonts.

The UI acknowledges a session token only after its changed regions finish
transferring. A state change interrupts the previous drawing pass; a transfer
failure closes the playback gate and stops reuse of a buffer that may still
belong to DMA.

Voice state follows VoiceReply commands rather than audio availability:

| App action | UI | Playback |
| --- | --- | --- |
| Prelude directly over L2CAP | Thinking | Play without a state change |
| `0x05 status=2` | Speaking | Wait for the Speaking screen, then play |
| Audio pauses without a command | Speaking | Re-prime while keeping the state |
| `0x05 status=3` | Speaking, then Ready | Drain PCM and 90 ms of I2S DMA |

`status=3` outside Speaking is ignored. Start/end commands arriving before the
button task runs are retained together. `status=0` or a failure ends a pending
reply; a new press or disconnect cancels the old generation. An empty formal
reply drains immediately. Speaking has no silence timeout; an explicitly
requested drain has a 30 s limit. Session IDs are logged without validation.

The 32 KiB playback buffer and bounded 8 KiB voice queue remain allocated.
The C3 BLE build omits the unused WiFi transport implementation, provisioning and
cloud sources. The shared component dependencies remain unconditional.

Memory snapshots are logged after display/codec initialization, BLE startup and
before voice creation. Voice and UI tasks log their remaining stack space.

Build in `firmware/build` with ESP-IDF 5.5.4 and low parallelism. For native checks,
run `tests/run_voice_session_tests.cmd` and `tests/run_raster_ui_tests.cmd` from a
Visual Studio developer command prompt at the firmware root. The renderer test
uses the production drawing code, checks full-frame/stripe equality and playback
gates, and writes `build/ui_*.ppm` previews. It uses AddressSanitizer.
