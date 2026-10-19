#pragma once
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <hal/adc_types.h>

// FoloToy AI Passport (ESP32-C3, 8 MB flash, no PSRAM)
// Pins from https://github.com/FoloToy/ai-passport components/bsp/include/bsp_pins.h

// Display: ST7789P3 240x320, 4-wire SPI
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_MOSI_PIN        GPIO_NUM_9
#define DISPLAY_SCK_PIN         GPIO_NUM_8
#define DISPLAY_CS_PIN          GPIO_NUM_1
#define DISPLAY_DC_PIN          GPIO_NUM_20
#define DISPLAY_RST_PIN         GPIO_NUM_NC   // hard-wired to 3.3V; software SWRESET
#define DISPLAY_BL_PIN          GPIO_NUM_21
#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          320
#define DISPLAY_SPI_CLK_HZ      (40 * 1000 * 1000)
#define DISPLAY_SPI_MODE        0
// DISPLAY_INVERT_COLOR: INVON. The panel's reference init (FoloToy's bsp_pins.h) sets this to 1
// unconditionally, and the colour test confirmed it: black comes out black. An earlier build
// without the vendor gamma commands below showed a cream background, which looked like wrong
// polarity; it was not - with the gamma registers in place the colours are right. A byte-order
// slip in the LVGL flush was the other thing tinting the screen, and that is fixed in voice_ui.cc.
//
// There is deliberately no BGR option. FoloToy's bsp_display.c uses LCD_RGB_ELEMENT_ORDER_RGB,
// and the panel takes red and blue the way the app draws them.
#define DISPLAY_INVERT_COLOR    1

// The ST7789P3 on this board needs the panel vendor's porch / power / gamma commands. esp_lcd's
// built-in init only sends SLPOUT, COLMOD and RAMCTRL, and the gamma the panel then runs with
// tints the whole screen orange. Values from FoloToy's bsp_display.c; do not reuse on another
// panel without its reference init.
#define DISPLAY_HAS_VENDOR_CMDS 1

// ── Buttons: three keys on one ADC pin (GPIO0 / ADC1_CH0), resistor ladder ────
// UP ≈ 0 mV, DOWN ≈ 300 mV, OK ≈ 595 mV, released ≈ 3300 mV
#define BUTTON_ADC_UNIT         ADC_UNIT_1
#define BUTTON_ADC_CHANNEL      ADC_CHANNEL_0
#define BUTTON_GPIO             GPIO_NUM_0
#define BUTTON_MV_UP_MAX        150
#define BUTTON_MV_DOWN_MAX      447
#define BUTTON_MV_OK_MAX        1900
#define BUTTON_MV_RELEASED_MIN  1900

// ── I2C: ES8311 codec + CW2017 fuel gauge share one bus ───────────────────────
#define AUDIO_I2C_PORT          I2C_NUM_0
#define AUDIO_I2C_SDA           GPIO_NUM_10
#define AUDIO_I2C_SCL           GPIO_NUM_7
#define ES8311_ADDR             0x18
#define CW2017_ADDR             0x63

// ── Audio: ES8311 full-duplex I2S ─────────────────────────────────────────────
#define AUDIO_I2S_MCLK          GPIO_NUM_6
#define AUDIO_I2S_BCLK          GPIO_NUM_5
#define AUDIO_I2S_WS            GPIO_NUM_3
#define AUDIO_I2S_DOUT          GPIO_NUM_2    // MCU → codec (speaker)
#define AUDIO_I2S_DIN           GPIO_NUM_4    // codec → MCU (mic)
#define AUDIO_PA_EN             GPIO_NUM_NC   // PA always on
#define AUDIO_SAMPLE_RATE       16000
