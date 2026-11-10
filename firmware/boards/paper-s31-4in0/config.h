#pragma once

#include "sdkconfig.h"
#include "../paper-s31/board_pins.h"
#include "platform_config.h"
#include "platform_product.h"

#define PAPER_MODEL "PAPER-S31-4IN0"
#define CLOUD_BASE_URL DEOTALAND_PLATFORM_BASE_URL
#define CLOUD_PRODUCT_ID AGENT_LINK_PRODUCT_ID
#define CLOUD_CHIP_TYPE "esp32-s31"
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_PLAY_BUF_BYTES (256 * 1024)

#define PAPER_KEY_UP KEY3_GPIO
#define PAPER_KEY_DOWN KEY2_GPIO
#define PAPER_KEY_CONFIRM KEY1_GPIO

// Verified GT911 axes for this assembly: raw origin is at the screen's bottom-right.
#define PAPER_TOUCH_RAW_WIDTH 320
#define PAPER_TOUCH_RAW_HEIGHT 480
#define PAPER_TOUCH_REVERSE_X true
#define PAPER_TOUCH_REVERSE_Y true
