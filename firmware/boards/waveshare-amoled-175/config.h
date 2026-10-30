#pragma once

#include "driver/gpio.h"
#include "platform_config.h"
#include "platform_product.h"

// Actual hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75C.
// Keep the existing profile/directory names for installed firmware compatibility.
#define SYS_I2C_PORT I2C_NUM_0
#define SYS_I2C_SDA GPIO_NUM_15
#define SYS_I2C_SCL GPIO_NUM_14
#define DISPLAY_WIDTH 466
#define DISPLAY_HEIGHT 466
#define DISPLAY_CS_PIN GPIO_NUM_12
#define DISPLAY_SCLK_PIN GPIO_NUM_38
#define DISPLAY_D0_PIN GPIO_NUM_4
#define DISPLAY_D1_PIN GPIO_NUM_5
#define DISPLAY_D2_PIN GPIO_NUM_6
#define DISPLAY_D3_PIN GPIO_NUM_7
#define DISPLAY_RST_PIN GPIO_NUM_1
#define DISPLAY_X_GAP 6
#define DISPLAY_QSPI_CLK_HZ (40 * 1000 * 1000)
#define DISPLAY_BRIGHTNESS 0xB0
#define DISPLAY_BRIGHTNESS_DIM 0x30
#define UI_DRAW_BUF_ROWS 40
#define TOUCH_RST_PIN GPIO_NUM_2
#define TOUCH_INT_PIN GPIO_NUM_11
// Official C-board BSP: swap_xy=0, mirror_x=1, mirror_y=1.
#define TOUCH_MIRROR_X true
#define TOUCH_MIRROR_Y true
#define CALL_BUTTON_PIN GPIO_NUM_0
#define AUDIO_I2S_MCLK GPIO_NUM_16
#define AUDIO_I2S_BCLK GPIO_NUM_9
#define AUDIO_I2S_WS GPIO_NUM_45
#define AUDIO_I2S_DOUT GPIO_NUM_8
#define AUDIO_I2S_DIN GPIO_NUM_10
#define AUDIO_PA_PIN GPIO_NUM_46
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_HARDWARE_SAMPLE_RATE 16000
#define AUDIO_MIC_GAIN 24
#define AUDIO_DEFAULT_VOLUME 60
#define AUDIO_PLAY_BUF_BYTES (256 * 1024)
#define CLOUD_BASE_URL DEOTALAND_PLATFORM_BASE_URL
#define CLOUD_PRODUCT_ID AGENT_LINK_PRODUCT_ID
#define CLOUD_CHIP_TYPE "esp32-s3"
