#pragma once
#include <driver/gpio.h>

// Not force-included by the build: without it CONFIG_SCCB_HARDWARE_I2C_PORT1 below would silently
// read as 0 and put the touch controller and the codec on the wrong I2C port.
#include "sdkconfig.h"

//
// korvo-cloud — the board that reaches the Deotaland platform over the WiFi channel
//
// GC2145 camera + ST7789 touch LCD + ES8311/ES7210 audio.
#if CONFIG_SCCB_HARDWARE_I2C_PORT1
#define SHARED_I2C_PORT         1
#else
#define SHARED_I2C_PORT         0
#endif

// -- Cloud --
// The platform keys device records and firmware off an integer product id, so it has to be
// compiled in. Ask the backend to create a product whose product_key is "korvo-cloud" and put its
// id here; until then this points at the only product that exists.
#define CLOUD_BASE_URL          "https://bot.zhaojun.work"
#define CLOUD_PRODUCT_ID        1
#define CLOUD_CHIP_TYPE         "esp32-s3"

#define CAM_PIN_PWDN            -1              // not broken out on the FPC - sensor always powered
#define CAM_PIN_RESET           -1              // not broken out - reset over SCCB only
#define CAM_PIN_XCLK            GPIO_NUM_40     // master clock out to sensor
#define CAM_PIN_SIOD            GPIO_NUM_17     // SCCB SDA - the shared bus
#define CAM_PIN_SIOC            GPIO_NUM_18     // SCCB SCL - the shared bus
#define CAM_PIN_D7              GPIO_NUM_39     // sensor D9
#define CAM_PIN_D6              GPIO_NUM_41     // sensor D8
#define CAM_PIN_D5              GPIO_NUM_42     // sensor D7
#define CAM_PIN_D4              GPIO_NUM_12     // sensor D6
#define CAM_PIN_D3              GPIO_NUM_3      // sensor D5
#define CAM_PIN_D2              GPIO_NUM_14     // sensor D4
#define CAM_PIN_D1              GPIO_NUM_47     // sensor D3
#define CAM_PIN_D0              GPIO_NUM_13     // sensor D2
#define CAM_PIN_VSYNC           GPIO_NUM_21
#define CAM_PIN_HREF            GPIO_NUM_38
#define CAM_PIN_PCLK            GPIO_NUM_11

#define CAM_XCLK_FREQ_HZ        (20 * 1000 * 1000)

// GC2145
#define CAMERA_MASK_TOP_ROWS    8               // overpaint the sensor's noisy top rows
#define CAMERA_AE_LOCK_FRAMES   50              // freeze AEC/AWB after ~2s so a static scene stops drifting
#define CAMERA_RGB565_BYTE_SWAP 0               // set to 1 if the preview colors come out wrong

// Display: ST7789 1.69in IPS
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_SCK_PIN         GPIO_NUM_1      // LCD CLK
#define DISPLAY_MOSI_PIN        GPIO_NUM_0      // LCD MOSI (also the BOOT strapping pin; only driven after boot)
#define DISPLAY_DC_PIN          GPIO_NUM_2
#define DISPLAY_CS_PIN          GPIO_NUM_46
#define DISPLAY_RST_PIN         (-1)            // tied to the ESP32-S3 reset line - nothing to drive
#define DISPLAY_BL_PIN          (-1)            // no backlight control pin - always on

// Rotation
#define DISPLAY_SWAP_XY         1
#define DISPLAY_MIRROR_X        0
#define DISPLAY_MIRROR_Y        1

// Logical size after rotation: the native 240x280 portrait becomes 280x240 landscape.
#define DISPLAY_PANEL_WIDTH     240
#define DISPLAY_PANEL_HEIGHT    280
#define DISPLAY_WIDTH           DISPLAY_PANEL_HEIGHT
#define DISPLAY_HEIGHT          DISPLAY_PANEL_WIDTH

// The ST7789 has 240x320 of GRAM and this 280-row panel starts 20 rows into it
#define DISPLAY_GAP_X           20
#define DISPLAY_GAP_Y           0

#define DISPLAY_SPI_CLK_HZ      (40 * 1000 * 1000)
#define DISPLAY_SPI_MODE        0

#define DISPLAY_STRIPE_ROWS     20
#define UI_DRAW_BUF_ROWS        40

// The glass has rounded corners
#define DISPLAY_CORNER_RADIUS   20
#define UI_SAFE_PAD             DISPLAY_CORNER_RADIUS

// Camera preview is 240x240 - it fills the short edge of the rotated panel; centre it on the long one.
#define PREVIEW_WIDTH           240
#define PREVIEW_HEIGHT          240
#define PREVIEW_X               ((DISPLAY_WIDTH  - PREVIEW_WIDTH)  / 2)
#define PREVIEW_Y               ((DISPLAY_HEIGHT - PREVIEW_HEIGHT) / 2)

// Touch: CST816 capacitive controller, on the shared I2C bus
#define TOUCH_I2C_PORT          SHARED_I2C_PORT
#define TOUCH_I2C_ADDR          0x15            // CST816 default
#define TOUCH_RST_PIN           (-1)
// The panel is driven rotated, but the controller still reports coordinates in the glass's native
// 240x280 frame, so the rotation has to be undone here
#define TOUCH_SWAP_XY           1
#define TOUCH_MIRROR_X          1
#define TOUCH_MIRROR_Y          0
// Set to 1 to log raw + mapped coordinates on every press
#define TOUCH_LOG_RAW           0

// Audio: ES8311 DAC + ES7210 4-channel ADC + NS4150B amp
#define AUDIO_I2C_PORT          SHARED_I2C_PORT
#define AUDIO_I2S_MCLK          GPIO_NUM_16
#define AUDIO_I2S_BCLK          GPIO_NUM_9          
#define AUDIO_I2S_WS            GPIO_NUM_45     
#define AUDIO_I2S_DOUT          GPIO_NUM_8
#define AUDIO_I2S_DIN           GPIO_NUM_10     
#define AUDIO_PA_EN             GPIO_NUM_48     
#define ES8311_ADDR             0x18
#define ES7210_ADDR             0x40
#define AUDIO_SAMPLE_RATE       16000           // PCM16 mono
#define AUDIO_MIC_GAIN          30
#define AUDIO_OUT_VOLUME        80
// TTS jitter buffer, in PSRAM
#define AUDIO_PLAY_BUF_BYTES    (256 * 1024)
