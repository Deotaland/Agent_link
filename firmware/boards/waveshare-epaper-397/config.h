#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "sdkconfig.h"
#include "platform_config.h"
#include "platform_product.h"

// Waveshare ESP32-S3-ePaper-3.97, N16R8. Verified against the official IDF BSP.
#define EPAPER_MODEL "WS-EPAPER-397"
#define CLOUD_BASE_URL DEOTALAND_PLATFORM_BASE_URL
#define CLOUD_PRODUCT_ID AGENT_LINK_PRODUCT_ID
#define CLOUD_CHIP_TYPE "esp32-s3"
#define SYS_I2C_PORT I2C_NUM_0
#define SYS_I2C_SDA GPIO_NUM_41
#define SYS_I2C_SCL GPIO_NUM_42
#define PMIC_ADDR 0x34
#define ES8311_ADDR 0x18
#define AUDIO_I2S_MCLK GPIO_NUM_13
#define AUDIO_I2S_BCLK GPIO_NUM_14
#define AUDIO_I2S_WS GPIO_NUM_47
#define AUDIO_I2S_DIN GPIO_NUM_21
#define AUDIO_I2S_DOUT GPIO_NUM_48
#define AUDIO_PA GPIO_NUM_39
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_DEFAULT_VOLUME 60
#define AUDIO_PLAY_BUF_BYTES (256 * 1024)
#define EPD_SPI_HOST SPI3_HOST
#define EPD_SCLK GPIO_NUM_11
#define EPD_MOSI GPIO_NUM_12
#define EPD_CS GPIO_NUM_10
#define EPD_DC GPIO_NUM_9
#define EPD_RST GPIO_NUM_46
#define EPD_BUSY GPIO_NUM_3
#define CJK_FONT_PARTITION "anim_pack"
#define CJK_FONT_PX 28
#define CJK_FONT_CACHE_GLYPHS 32
#define EPD_WIDTH 800
#define EPD_HEIGHT 480
#define KEY_UP GPIO_NUM_4
#define KEY_CENTER GPIO_NUM_5
#define KEY_DOWN GPIO_NUM_6
#define KEY_BOOT GPIO_NUM_0
