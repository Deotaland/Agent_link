#pragma once
#include <driver/gpio.h>

// Power latch: GPIO14 pulls low to power peripherals
#define POWER_CTRL_PIN          GPIO_NUM_14
#define POWER_CTRL_ACTIVE_HIGH  0

// Audio I2S, ES8311 playback + ES7210 recording, TDM with AEC reference
#define AUDIO_I2S_MCLK          GPIO_NUM_9
#define AUDIO_I2S_BCLK          GPIO_NUM_10
#define AUDIO_I2S_WS            GPIO_NUM_11
#define AUDIO_I2S_DOUT          GPIO_NUM_41   // ES8311 speaker
#define AUDIO_I2S_DIN           GPIO_NUM_12   // ES7210 mic
#define AUDIO_PA_EN             GPIO_NUM_13

// IIC:The audio codec is shared with the BQ27220 fuel gauge.
#define AUDIO_I2C_SDA           GPIO_NUM_45
#define AUDIO_I2C_SCL           GPIO_NUM_48
#define ES8311_ADDR             0x18
#define ES7210_ADDR             0x40
#define AUDIO_SAMPLE_RATE       16000

// Fuel gauge BQ27220
#define BQ27220_ADDR            0x55
#define BAT_DESIGN_CAPACITY_MAH 800

// Display: SH8501 AMOLED 120x240 (SPI), factory short-code ("LK") bring-up.
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

// SD card
#define SD_SDIO_CLK_HZ          (40 * 1000 * 1000)
#define SD_SDIO_WIDTH           4

#define SD_PIN_D0               GPIO_NUM_3
#define SD_PIN_D1               GPIO_NUM_21
#define SD_PIN_D2               GPIO_NUM_47
#define SD_PIN_D3               GPIO_NUM_17
#define SD_PIN_CLK              GPIO_NUM_18
#define SD_PIN_CMD              GPIO_NUM_8

// button
#define BUTTON_BOOT_PIN         GPIO_NUM_0
#define BUTTON_VOL_UP_PIN       GPIO_NUM_39
#define BUTTON_VOL_DOWN_PIN     GPIO_NUM_40

// motor
#define HAPTIC_PIN              GPIO_NUM_1

