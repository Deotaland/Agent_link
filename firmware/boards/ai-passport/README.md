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

The 32 KiB playback buffer and bounded 24 KiB voice queue remain allocated.
The BLE profile uses 96 blocks in each MSYS pool and checks their free counts
separately, reserving 12 blocks per pool for control traffic. VoiceChunk carries
up to 494 bytes of continuous PCM at ATT MTU 512, with smaller packets at lower
MTUs. A congested packet can wait up to 2 seconds; queue overflow still ends the
session to avoid sending audio with gaps. Per-second flow logs show capture and
submission rates, queue depth and both pool counts. Submission is not a phone
receipt acknowledgement.
The BLE profile omits WiFi provisioning and cloud sources. Its dependencies are
selected from the firmware profile before ESP-IDF expands component requirements.

Memory snapshots are logged after display/codec initialization, BLE startup and
before voice creation. Voice and UI tasks log their remaining stack space.

Build in `firmware/build` with ESP-IDF 5.5.4:

```bash
python tools/build_firmware.py ai-passport-ble
```
