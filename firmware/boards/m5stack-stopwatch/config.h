#pragma once
// M5Stack StopWatch (SKU C152): ESP32-S3R8 (8 MB octal PSRAM), 16 MB flash, 1.75" 466x466 round
// AMOLED (CO5300 over QSPI) with CST820 touch, ES8311 codec + AW8737A amp, two keys, vibration
// motor, M5PM1 power management, M5IOE1 IO expander, BMI270 IMU, RX8130CE RTC.

#include <driver/gpio.h>
#include <driver/i2c_types.h>
#include <driver/i2s_types.h>
#include <hal/spi_types.h>

// ── System I2C: PMIC, IO expander, codec, touch, IMU and RTC share it ────────
#define SYS_I2C_PORT            I2C_NUM_0
#define SYS_I2C_SDA             GPIO_NUM_47
#define SYS_I2C_SCL             GPIO_NUM_48
#define PM1_ADDR                0x6E
#define IOE_ADDR                0x4F
#define ES8311_ADDR             0x18

// ── M5PM1 ─────────────────────────────────────────────────────────────────────
#define PM1_CHG_STAT_GPIO       2             // PM1 G2: the charger's status, low while charging

// ── M5IOE1 pins (IO1..IO14) ───────────────────────────────────────────────────
#define IOE_MUX_CTR             1             // rear header: low = UART0, high = USB D+/D-
#define IOE_AUDIO_EN            3             // codec and mic supply
#define IOE_TP_RST              4             // touch reset, active low
#define IOE_PANEL_RST           5             // panel reset, active low
#define IOE_PANEL_PWR           8             // 3V3_L3B, the panel's supply
#define IOE_MOTOR               9             // vibration motor, on PWM channel 1
#define IOE_SPK_EN              10            // AW8737A enable
#define MOTOR_PWM_CHANNEL       1
#define MOTOR_PWM_HZ            2000
#define MOTOR_DUTY              3000          // of 4095

// ── Display: CO5300 over QSPI ─────────────────────────────────────────────────
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_CS_PIN          GPIO_NUM_39
#define DISPLAY_SCLK_PIN        GPIO_NUM_40
#define DISPLAY_D0_PIN          GPIO_NUM_41
#define DISPLAY_D1_PIN          GPIO_NUM_42
#define DISPLAY_D2_PIN          GPIO_NUM_46
#define DISPLAY_D3_PIN          GPIO_NUM_45
#define DISPLAY_TE_PIN          GPIO_NUM_38   // not used
#define DISPLAY_WIDTH           466
#define DISPLAY_HEIGHT          466
#define DISPLAY_X_GAP           6             // the glass starts at column 6 of the controller's RAM
#define DISPLAY_Y_GAP           0
#define DISPLAY_QSPI_CLK_HZ     (40 * 1000 * 1000)   // M5's own firmware runs it at 80 MHz
#define DISPLAY_STRIPE_ROWS     12            // two 466x12 DMA stripes in internal RAM
#define DISPLAY_BGR             0             // 1 if red and blue come out swapped
#define DISPLAY_BRIGHTNESS      0xC0          // 0-255
#define DISPLAY_BRIGHTNESS_DIM  0x30          // after a while with nothing happening
#define UI_DRAW_BUF_ROWS        60            // LVGL's partial buffer, in PSRAM

// ── Audio: ES8311 DAC + ADC on one full-duplex I2S port ──────────────────────
#define AUDIO_I2S_PORT          I2S_NUM_0
#define AUDIO_I2S_MCLK          GPIO_NUM_18
#define AUDIO_I2S_BCLK          GPIO_NUM_17
#define AUDIO_I2S_WS            GPIO_NUM_15
#define AUDIO_I2S_DOUT          GPIO_NUM_21   // to the codec's DSDIN: speaker
#define AUDIO_I2S_DIN           GPIO_NUM_16   // from the codec's ASDOUT: mic
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_MIC_GAIN          30
#define AUDIO_DEFAULT_VOLUME    60            // 0-100; the blue key steps it by 20
#define AUDIO_PLAY_BUF_BYTES    (256 * 1024)  // 8 s of reply, in PSRAM

// ── Keys: active low, 10k pull-ups on the board ──────────────────────────────
#define KEY_TALK_PIN            GPIO_NUM_2    // yellow (KEY1): hold to talk
#define KEY_VOLUME_PIN          GPIO_NUM_1    // blue (KEY2): volume; hold to forget the pairing
#define FORGET_HOLD_MS          6000

// ── Platform, for the WiFi link (BLE ignores it) ─────────────────────────────
// The platform keys device records off an integer product id. Ask the backend for a product of
// its own and put its id here; until then this points at the only product that exists.
#define CLOUD_BASE_URL          "https://bot.zhaojun.work"
#define CLOUD_PRODUCT_ID        1
#define CLOUD_CHIP_TYPE         "esp32-s3"
