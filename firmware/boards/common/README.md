# boards/common/ — cross-board drivers

Drivers and helpers that more than one board may use (chip drivers, bus helpers). The board only passes
pins; the driver itself is board-independent. `main/CMakeLists.txt` compiles every `*.cc`/`*.c` here, and
the include path `../boards/common` lets a board `#include "xxx.h"` directly.

| Files                                      | Purpose                                                                                                                                                             | Dependencies (in main's REQUIRES)                                   |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| `sh8501_panel.{h,cc}`                      | SH8501 AMOLED (SPI), **full vendor init**: init + solid fill + blit + brightness; pins come from `Sh8501Config`. Lights up sample panels whose OTP was never burned | `esp_lcd`, `esp_lcd_sh8501`, `esp_driver_spi`, `esp_driver_gpio`    |
| `sh8501_lk_panel.{h,cc}`                   | Same panel, **factory 简码 (short-code / LK) init** — what the mass-produced hardware needs. Same interface as `Sh8501Panel`; see the note below                    | `esp_lcd`, `esp_lcd_sh8501_lk`, `esp_driver_spi`, `esp_driver_gpio` |
| `es_codec.{h,cc}`                          | ES8311 + ES7210 full-duplex audio codec (speaker out / mic in)                                                                                                      | `esp_codec_dev`, `esp_driver_i2c`, `esp_driver_i2s`                 |
| `es8311_audio.{h,cc}`                      | ES8311-only full-duplex codec: one chip does both speaker (DAC) and mic (ADC), standard I2S                                                                         | `esp_codec_dev`, `esp_driver_i2c`, `esp_driver_i2s`                 |
| `co5300_panel.{h,cc}` + `co5300_hal.{c,h}` | CO5300 466x466 AMOLED (MIPI-DSI); ESP32-P4 only (stubbed out on other targets)                                                                                      | `esp_lcd`, `esp_lcd_co5300`                                         |
| `bq27220.{h,cc}`                           | BQ27220 fuel gauge (I2C); reuses an I2C bus another driver already created                                                                                          | `esp_driver_i2c`                                                    |

## Usage (from a board)

```cpp
#include "sh8501_panel.h"

Sh8501Config c = {};
c.spi_host = DISPLAY_SPI_HOST;  c.pin_sck = DISPLAY_SCK_PIN;  c.pin_mosi = DISPLAY_MOSI_PIN;
c.pin_cs = DISPLAY_CS_PIN;  c.pin_dc = DISPLAY_DC_PIN;  c.pin_rst = DISPLAY_RST_PIN;
c.width = DISPLAY_WIDTH;  c.height = DISPLAY_HEIGHT;  c.pclk_hz = DISPLAY_SPI_CLK_HZ;  c.spi_mode = DISPLAY_SPI_MODE;

Sh8501Panel panel;
panel.Init(c);                    // init + light up (ends black, full brightness)
panel.FillSolid(rgb565::kBlue);   // fill the whole screen
```

## Rules for adding a shared driver

- **Board-independent**: pass pin/address differences in through a parameter/Config; don't hardcode one board's pins here.
- **Only put things that are genuinely reused.** A private driver used by a single board belongs in that board's own directory (`boards/<board>/`).
- If a new driver needs a new ESP-IDF component, add it to `REQUIRES` in `main/CMakeLists.txt`.

## Note: two SH8501 panel drivers, pick by hardware

The same controller is driven two different ways, and picking the wrong one gives a black screen:

- **`Sh8501Panel`** (`components/esp_lcd_sh8501/`) writes the **full vendor sequence** — power, boost and
  gamma registers included — over `esp_lcd_panel_io_spi` with DMA. It does not depend on anything being
  burned into the panel, Used by `boards/rorolee-s3`.
- **`Sh8501LkPanel`** (`components/esp_lcd_sh8501_lk/`) runs the **factory 简码 short code**: standard user
  commands only, with power and gamma coming from the panel's **OTP**. It also talks to the panel
  differently (its own SPI device, 8-bit commands, blocking transfers, two `0x2C` before each pixel run).
  `boards/work-badge` does. A panel whose OTP was never burned stays black here.

Neither driver is in the Espressif component registry, so both are vendored under `components/`, which
keeps the project self-contained and buildable offline. `esp_lcd_sh8501_lk` is a byte-for-byte copy of the
production firmware's copy — see its README before touching it.
