#pragma once
#include "driver/gpio.h"
#include "platform_config.h"
#include "platform_product.h"
// Verified against SDGOODS BSP board_pins.h at dae46766aa345c680691420691d3d7170f0c6372.
#define DISPLAY_WIDTH 360
#define DISPLAY_HEIGHT 360
#define DISPLAY_ROTATION_CW 0
#define DISPLAY_BRIGHTNESS 190
#define DISPLAY_BRIGHTNESS_DIM 35
#define UI_DRAW_BUF_ROWS 40
#define AUDIO_DEFAULT_VOLUME 35
#define BOARD_DISPLAY_NAME "Sugarat Button"
#define AMOLED_DEVICE_NAME "Sugarat-Button"
#define AMOLED_FIRMWARE_MODEL "SDGOODS-ESP32S3"
#define SYS_I2C_PORT I2C_NUM_1
#define SYS_I2C_SDA GPIO_NUM_11
#define SYS_I2C_SCL GPIO_NUM_10
#define DISPLAY_SCLK_PIN GPIO_NUM_40
#define DISPLAY_D0_PIN GPIO_NUM_46
#define DISPLAY_D1_PIN GPIO_NUM_45
#define DISPLAY_D2_PIN GPIO_NUM_42
#define DISPLAY_D3_PIN GPIO_NUM_41
#define DISPLAY_CS_PIN GPIO_NUM_21
#define DISPLAY_RST_PIN GPIO_NUM_9
#define DISPLAY_POWER_PIN GPIO_NUM_12
// Vendor BSP lcd_pow_on(): this LCD supply enable is active low.
#define DISPLAY_POWER_ON_LEVEL 0
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_13
#define DISPLAY_TE_PIN GPIO_NUM_18
#define DISPLAY_QSPI_CLK_HZ (80 * 1000 * 1000)
#define TOUCH_RST_PIN GPIO_NUM_5
#define TOUCH_INT_PIN GPIO_NUM_4
#define CALL_BUTTON_PIN GPIO_NUM_6
#define BATTERY_LATCH_PIN GPIO_NUM_7
#define BATTERY_ADC_PIN GPIO_NUM_8
#define IMU_I2C_ADDRESS 0x6B
// Separate microphone and speaker I2S buses; vendor speaker MCLK is 384 * Fs.
#define AUDIO_MIC_BCLK GPIO_NUM_16
#define AUDIO_MIC_WS GPIO_NUM_2
#define AUDIO_MIC_DIN GPIO_NUM_17
#define AUDIO_SPK_BCLK GPIO_NUM_48
#define AUDIO_SPK_WS GPIO_NUM_38
#define AUDIO_SPK_DOUT GPIO_NUM_47
#define AUDIO_SPK_MCLK GPIO_NUM_15
#define AUDIO_PA_PIN GPIO_NUM_3

// The voice application reuses the common realtime controller and PCM workers.
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_PLAY_BUF_BYTES (256 * 1024)
#define AMOLED_AUDIO_DESCRIPTION "SDGOODS mono 32-bit I2S RX -> PCM16 16kHz; separate stereo I2S TX"
#define CLOUD_BASE_URL DEOTALAND_PLATFORM_BASE_URL
#define CLOUD_PRODUCT_ID AGENT_LINK_PRODUCT_ID
#define CLOUD_CHIP_TYPE "esp32-s3"
