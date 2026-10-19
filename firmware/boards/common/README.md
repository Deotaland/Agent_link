# Shared hardware drivers

These drivers accept pins, addresses and bus handles from a board. They do not
implement application behavior or depend on BLE/WiFi. The selected profile lists
source files in `drivers`; `components/board_support` compiles that list.

| Driver | Hardware or purpose |
| --- | --- |
| `sh8501_panel.*`, `sh8501_lk_panel.*` | SH8501 AMOLED; full initialization or factory OTP variant |
| `gc9d01_panel.*` | WOWNNY round TFT |
| `st7789_lcd.*` | SPI LCD with DMA stripe transfers |
| `co5300_panel.*`, `co5300_hal.*` | CO5300 AMOLED over QSPI or supported MIPI hardware |
| `cst816_touch.*`, `cst820_touch.*` | Separate touch-controller protocols |
| `es_codec.*` | ES8311 speaker and ES7210 microphone |
| `es8311_audio.*` | ES8311 full-duplex codec |
| `m5ioe1.*`, `m5pm1.*` | M5Stack I/O expander and power controller |
| `bq27220.*` | Fuel gauge on a shared I2C bus |
| `gc2145_camera.*` | DVP camera using esp32-camera |
| `cjk_font.*` | Memory-mapped font partition for LVGL TinyTTF |
| `stripe_canvas.*`, `raster_font.h`, `raster_montserrat_*.c` | Small-memory drawing and fonts |
| `voice_playback.h` | Playback buffer helpers |

## Add a driver

Pass hardware differences in a config structure. Share existing bus handles
rather than initializing the same I2C bus twice. Board-specific code belongs in
`boards/<hardware>/`; a driver becomes common when it is reused.

Add the source to the profile's `drivers` list. Add new component requirements
in `components/board_support/CMakeLists.txt` and managed versions in its
`idf_component.yml`. Sources are selected explicitly, not compiled for every
board. Requirements must be resolvable before Kconfig is loaded.

## SH8501 variants

`Sh8501Panel` sends the full vendor initialization sequence. `Sh8501LkPanel`
uses factory OTP power/gamma settings and a different SPI command sequence.
Choose by the actual panel; the wrong variant can leave the screen black.
Their low-level components are vendored under `components/`.
