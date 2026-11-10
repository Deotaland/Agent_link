# Agent Link Factory for Sugarat Button

This Factory also supports the same Sugarat Button hardware sold by SDGOODS.

This board-bound Factory reuses the round launcher controller and system menu.
Its native 360px renderer uses a dark navy background, cyan accents, large
antialiased circular covers, a separate app name and gesture hint, page dots,
and explicit launch/remove actions. It supports Chinese and English.

- Swipe horizontally to browse installed apps and the final Add application card.
- Tap an app to open its actions; removal requires confirmation.
- Pull down from the top for WiFi, app service, device, language and cover settings.
- Swipe up from the bottom, or press the GPIO6 key, to return home.
- Join `Sugarat-Button-XXXX` and open `http://192.168.4.1` for initial WiFi setup.
- Configure an Agent Link application service in the system menu. Catalog images
  must target `SDGOODS-ESP32S3` and layout version 2. Vendor app images are incompatible.

## Memory and transfers

The LVGL heap is 2 MiB in external BSS/PSRAM. Two native RGB565 full-frame draw
buffers plus one asynchronous transfer staging buffer occupy 777,600 bytes
in PSRAM. Rendering and transfer run on separate cores; no scaling of a 466px
framebuffer is performed. Only dirty regions are transmitted.

Three recent cover sizes use bounded PSRAM caching and alpha-aware bilinear
resampling. At the largest 198px size, their pixel storage is at most 352,836
bytes, plus immutable source snapshots. Fonts also cache glyphs in PSRAM.
WiFi/LwIP and TLS prefer external memory; 64 KiB of internal memory is reserved
for tasks, radio and DMA needs.

The QSPI transport owns one permanent **8,640-byte internal DMA stripe** and
its transaction descriptor. Each stripe has a complete window command; it
never lends a PSRAM pointer to DMA. A timed-out descriptor remains owned until
SPI returns it. The existing bounded recovery mechanism then reinitializes the
glass and requests a full redraw. CST816 polls are throttled with slow-read
backoff so flash/network work cannot create a tight I2C retry loop.

See the [board guide](../../boards/sdgoods-esp32s3/README.md) for build/package
commands, pins, first-install migration and pending device checks. Factory
does not initialize audio or claim battery telemetry.
