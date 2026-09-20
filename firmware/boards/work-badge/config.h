#pragma once
// Work badge (electronic staff ID card)
// Only the screen, the fuel gauge and the App link are driven by this firmware; audio, SD and
// the extra buttons are wired on the board and listed here for reference.

#include <driver/gpio.h>
#include <driver/i2c_types.h>

// ── Power ─────────────────────────────────────────────────────────────────────
// Active low: pulling GPIO14 low powers the peripheral rail (display + gauge).
#define POWER_CTRL_PIN          GPIO_NUM_14
#define POWER_CTRL_ACTIVE_HIGH  0

// ── 32.768 kHz crystal ────────────────────────────────────────────────────────
// GPIO15 = XTAL_32K_P, GPIO16 = XTAL_32K_N, the RTC slow clock and the BLE controller's
// low-power clock (CONFIG_RTC_CLK_SRC_EXT_CRYS, patched in by the repo-root CMakeLists).
// These two pins are taken by the crystal - nothing else may use them. Documentation only.
#define XTAL_32K_P_PIN          GPIO_NUM_15
#define XTAL_32K_N_PIN          GPIO_NUM_16

// ── Display: SH8501 AMOLED 120x240
// The LK driver fixes the SPI device at mode 0, so there is no spi_mode setting here.
// 40MHz is the production value: 30MHz corrupts some panels' init writes (black screen),
// 2MHz lights up but only manages ~4fps.
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_CS_PIN          GPIO_NUM_7
#define DISPLAY_DC_PIN          GPIO_NUM_46
#define DISPLAY_SCK_PIN         GPIO_NUM_5
#define DISPLAY_MOSI_PIN        GPIO_NUM_4
#define DISPLAY_RST_PIN         GPIO_NUM_6
#define DISPLAY_WIDTH           120
#define DISPLAY_HEIGHT          240
#define DISPLAY_SPI_CLK_HZ      (40 * 1000 * 1000)

// A badge reads across its long edge, so the UI is laid out landscape (240x120) and rotated
// onto the portrait panel when it is flushed.
#define BADGE_UI_WIDTH          DISPLAY_HEIGHT
#define BADGE_UI_HEIGHT         DISPLAY_WIDTH

// Which way to rotate. 1 = counter-clockwise (matches the production firmware's text screens on
// this panel), 0 = clockwise. Flip this single value if the badge reads upside down on your
// unit - nothing else in the UI code depends on the orientation. If the whole picture is 180
// degrees out instead, that is the panel's MADCTL byte in boards/common/sh8501_lk_panel.cc.
#define BADGE_ROTATE_CCW        1

// Screen brightness (0-255). Dim is deliberate: a badge is worn all day, and the AMOLED at full
// brightness is both glaring at reading distance and the biggest drain on the battery.
#define BADGE_BRIGHTNESS        0xB0

// ── I2C: audio codec + BQ27220 fuel gauge share one physical bus ──────────────
// Port I2C_NUM_1 and these pins are what production uses. Both devices must sit on the *same*
// port number or each would create its own bus on the same pins and fight - which shows up as
// the gauge returning ESP_ERR_INVALID_STATE forever. The badge does not bring up the codec, so
// it creates this bus itself.
#define AUDIO_I2C_PORT          I2C_NUM_1
#define AUDIO_I2C_SDA           GPIO_NUM_45
#define AUDIO_I2C_SCL           GPIO_NUM_48
#define AUDIO_I2C_FREQ_HZ       100000

// Fuel gauge BQ27220 (7-bit 0x55), 800 mAh cell on this board.
#define BQ27220_ADDR            0x55
#define BAT_DESIGN_CAPACITY_MAH 800

// ── Audio I2S: ES8311 playback + ES7210 capture (not used by the badge firmware) ──
#define AUDIO_I2S_MCLK          GPIO_NUM_9
#define AUDIO_I2S_BCLK          GPIO_NUM_10
#define AUDIO_I2S_WS            GPIO_NUM_11
#define AUDIO_I2S_DOUT          GPIO_NUM_41   // ES8311 speaker
#define AUDIO_I2S_DIN           GPIO_NUM_12   // ES7210 mic
#define AUDIO_PA_EN             GPIO_NUM_13
#define ES8311_ADDR             0x18
#define ES7210_ADDR             0x40
#define AUDIO_SAMPLE_RATE       16000

// ── SD card, SDIO 4-bit (not used by the badge firmware) ──────────────────────
#define SD_SDIO_CLK_HZ          (40 * 1000 * 1000)
#define SD_SDIO_WIDTH           4
#define SD_PIN_D0               GPIO_NUM_3
#define SD_PIN_D1               GPIO_NUM_21
#define SD_PIN_D2               GPIO_NUM_47
#define SD_PIN_D3               GPIO_NUM_17
#define SD_PIN_CLK              GPIO_NUM_18
#define SD_PIN_CMD              GPIO_NUM_8

// ── Buttons (not used by the badge firmware) ──────────────────────────────────
// Active low, internal pull-up.
#define BUTTON_BOOT_PIN         GPIO_NUM_0
#define BUTTON_VOL_UP_PIN       GPIO_NUM_39
#define BUTTON_VOL_DOWN_PIN     GPIO_NUM_40

// ── Haptic motor (not used by the badge firmware) ─────────────────────────────
#define HAPTIC_PIN              GPIO_NUM_1
#define HAPTIC_ACTIVE_HIGH      1

