#pragma once
// WOWNNY: rorolee PCB with a GC9D01 160x160 round TFT.
//
// Two hardware revisions, two board types:
//   BOARD_TYPE_WOWNNY_MUSE      WWY-01, production board (pins from the production firmware)
//   BOARD_TYPE_WOWNNY_OLD_MUSE  WWY-01-old: different display pins, display powered from GPIO46,
//                               no 32 kHz crystal, SD D3 on GPIO16, volume keys swapped
// Everything outside the WOWNNY_OLD_BOARD blocks is shared.

#include <driver/gpio.h>
#include <driver/i2c_types.h>

#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_WOWNNY_OLD_MUSE
#define WOWNNY_OLD_BOARD 1
#else
#define WOWNNY_OLD_BOARD 0
#endif

// ── Audio: ES8311 (out) + ES7210 (mic) on one I2S port ───────────────────────
// Same as rorolee except PA enable (GPIO13 on rorolee).
#define AUDIO_I2S_MCLK          GPIO_NUM_9
#define AUDIO_I2S_BCLK          GPIO_NUM_10
#define AUDIO_I2S_WS            GPIO_NUM_11
#define AUDIO_I2S_DOUT          GPIO_NUM_41
#define AUDIO_I2S_DIN           GPIO_NUM_12
#define AUDIO_PA_EN             GPIO_NUM_3
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_MIC_GAIN          30
#define AUDIO_DEFAULT_VOLUME    70            // 0-100

// ── I2C: codec + BQ27220 gauge ───────────────────────────────────────────────
// The codec driver creates the bus, the gauge reuses it.
#define AUDIO_I2C_PORT          I2C_NUM_0
#define AUDIO_I2C_SDA           GPIO_NUM_45
#define AUDIO_I2C_SCL           GPIO_NUM_48
#define ES8311_ADDR             0x18
#define ES7210_ADDR             0x40
#define BQ27220_ADDR            0x55
// 500 mAh cell (800 on rorolee). Not written to the gauge here; the production firmware does that.
#define BAT_DESIGN_CAPACITY_MAH 500

// ── Display: GC9D01 160x160, SPI mode 0, BOE init sequence ───────────────────
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_WIDTH           160
#define DISPLAY_HEIGHT          160
#define DISPLAY_SPI_CLK_HZ      (20 * 1000 * 1000)
#define DISPLAY_BACKLIGHT_ACTIVE_HIGH 0
#if WOWNNY_OLD_BOARD
#define DISPLAY_CS_PIN          GPIO_NUM_7
#define DISPLAY_DC_PIN          GPIO_NUM_15
#define DISPLAY_SCK_PIN         GPIO_NUM_6
#define DISPLAY_MOSI_PIN        GPIO_NUM_5
#define DISPLAY_RST_PIN         GPIO_NUM_4
// GPIO46 (active low) powers the whole display module, backlight included, so there is no
// separate backlight pin. The board power-cycles it at boot (see PowerOnRail()).
#define POWER_CTRL_PIN          GPIO_NUM_46
#define POWER_CTRL_ACTIVE_HIGH  0
#define DISPLAY_BACKLIGHT_PIN   -1
#else
// RST is on GPIO43 (UART0 TX): once the display is up the log is only on USB-Serial-JTAG.
// Same as the production firmware.
#define DISPLAY_CS_PIN          GPIO_NUM_6
#define DISPLAY_DC_PIN          GPIO_NUM_7
#define DISPLAY_SCK_PIN         GPIO_NUM_5
#define DISPLAY_MOSI_PIN        GPIO_NUM_4
#define DISPLAY_RST_PIN         GPIO_NUM_43
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_46   // backlight enable only, codec/gauge are always powered
#endif
// Diameter of the part of the panel visible through the enclosure. voice_ui.cc checks its layout
// against it at compile time.
#define UI_VISIBLE_DIAMETER     100

// ── 32.768 kHz crystal ────────────────────────────────────────────────────────
// Production board only (GPIO15/16, CONFIG_RTC_CLK_SRC_EXT_CRYS set by the root CMakeLists).
// The old board uses those pins for display DC and SD D3 and runs the RTC from the internal RC.
#if !WOWNNY_OLD_BOARD
#define XTAL_32K_P_PIN          GPIO_NUM_15   // for reference only
#define XTAL_32K_N_PIN          GPIO_NUM_16
#endif

// ── Buttons: active low, internal pull-up ─────────────────────────────────────
#define BUTTON_BOOT_PIN         GPIO_NUM_0
#if WOWNNY_OLD_BOARD
#define BUTTON_VOL_UP_PIN       GPIO_NUM_40   // swapped compared to the production board
#define BUTTON_VOL_DOWN_PIN     GPIO_NUM_39
#else
#define BUTTON_VOL_UP_PIN       GPIO_NUM_39
#define BUTTON_VOL_DOWN_PIN     GPIO_NUM_40
#endif

// ── Haptic motor ──────────────────────────────────────────────────────────────
#define HAPTIC_PIN              GPIO_NUM_1
#define HAPTIC_ACTIVE_HIGH      1

// ── SD card, SDIO 4-bit (unused) ──────────────────────────────────────────────
#define SD_PIN_D0               GPIO_NUM_8
#define SD_PIN_D1               GPIO_NUM_21
#define SD_PIN_D2               GPIO_NUM_47
#if WOWNNY_OLD_BOARD
#define SD_PIN_D3               GPIO_NUM_16
#else
#define SD_PIN_D3               GPIO_NUM_14   // GPIO16 is taken by the crystal
#endif
#define SD_PIN_CLK              GPIO_NUM_17
#define SD_PIN_CMD              GPIO_NUM_18
