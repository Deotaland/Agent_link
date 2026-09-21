#pragma once
#include <driver/gpio.h>

// Not force-included by the build: without it CONFIG_SCCB_HARDWARE_I2C_PORT1 below would silently
// read as 0 and put the touch controller and the codec on the wrong I2C port.
#include "sdkconfig.h"

//
// GC2145 camera + ST7789 touch LCD + ES8311/ES7210 audio + a TF card, driven from a home screen
// you swipe between apps on.

// The one I2C bus
// Three chips share GPIO17/18 on this board: the camera's SCCB, the CST816 touch controller and
// the audio codec pair. esp32-camera creates the bus (on the port its Kconfig selects) and the
// other two attach to it — a second bus on the same pins is not possible. That is also why the
// board brings the camera up first.
#if CONFIG_SCCB_HARDWARE_I2C_PORT1
#define SHARED_I2C_PORT         1
#else
#define SHARED_I2C_PORT         0
#endif

// ── Camera: DVP 8-bit + SCCB(I2C) + XCLK, on the 24P FPC header. -1 = not wired ──
// The sensor labels its data lines D2..D9 (the top 8 of its 10-bit bus); esp32-camera calls the same
// wires d0..d7, so the schematic's D2 lands on CAM_PIN_D0 and its D9 on CAM_PIN_D7.
#define CAM_PIN_PWDN            -1              // not broken out on the FPC — sensor always powered
#define CAM_PIN_RESET           -1              // not broken out — reset over SCCB only
#define CAM_PIN_XCLK            GPIO_NUM_40     // master clock out to sensor
#define CAM_PIN_SIOD            GPIO_NUM_17     // SCCB SDA — the shared bus
#define CAM_PIN_SIOC            GPIO_NUM_18     // SCCB SCL — the shared bus
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

// GC2145 quirks, handled by boards/common/gc2145_camera.{h,cc}
#define CAMERA_MASK_TOP_ROWS    8               // overpaint the sensor's noisy top rows
#define CAMERA_AE_LOCK_FRAMES   50              // freeze AEC/AWB after ~2s so a static scene stops drifting
#define CAMERA_RGB565_BYTE_SWAP 0               // set to 1 if the preview colors come out wrong

// ── Display: ST7789 1.69" IPS, natively 240x280 portrait, driven rotated 90° counter-clockwise ──
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_SCK_PIN         GPIO_NUM_1      // LCD CLK
#define DISPLAY_MOSI_PIN        GPIO_NUM_0      // LCD MOSI (also the BOOT strapping pin; only driven after boot)
#define DISPLAY_DC_PIN          GPIO_NUM_2
#define DISPLAY_CS_PIN          GPIO_NUM_46
#define DISPLAY_RST_PIN         (-1)            // tied to the ESP32-S3 reset line — nothing to drive
#define DISPLAY_BL_PIN          (-1)            // no backlight control pin — always on

// Rotation is the panel's own MADCTL (swap_xy + mirror), not a CPU pass over the pixels, so it is
// free and everything drawn — the camera preview included — comes out rotated with it.
//   90° CCW : swap_xy=1, mirror_x=0, mirror_y=1   <- current
//   90° CW  : swap_xy=1, mirror_x=1, mirror_y=0   <- swap these two if the image lands upside down
#define DISPLAY_SWAP_XY         1
#define DISPLAY_MIRROR_X        0
#define DISPLAY_MIRROR_Y        1

// Logical size after rotation: the native 240x280 portrait becomes 280x240 landscape.
#define DISPLAY_PANEL_WIDTH     240             // native, before rotation
#define DISPLAY_PANEL_HEIGHT    280
#define DISPLAY_WIDTH           DISPLAY_PANEL_HEIGHT
#define DISPLAY_HEIGHT          DISPLAY_PANEL_WIDTH

// The ST7789 has 240x320 of GRAM and this 280-row panel starts 20 rows into it. swap_xy makes CASET
// address that 320-wide direction, so the 20 belongs on the x gap here — it would be the y gap if
// the panel were left portrait. If the image comes out shifted along its long edge, this is the knob.
#define DISPLAY_GAP_X           20
#define DISPLAY_GAP_Y           0

#define DISPLAY_SPI_CLK_HZ      (40 * 1000 * 1000)
#define DISPLAY_SPI_MODE        0

// Internal-RAM budget. This board runs BLE, LVGL, a camera and an SD card off ~150KB of internal
// RAM, and BLE fails to even start advertising if the rest of us take too much. Two knobs:
//   - the LCD keeps two ping-pong DMA stripes of DISPLAY_WIDTH * this * 2 bytes, and they MUST be
//     internal. 20 rows = 2 x 11KB, half of the driver's 40-row default.
//   - the LVGL draw buffer is this many rows and goes to PSRAM (see korvo_ui.cc).
#define DISPLAY_STRIPE_ROWS     20
#define UI_DRAW_BUF_ROWS        40

// The glass has rounded corners, so a pixel inside this distance of a corner is cut off. Full-width
// bars are still fine — it is their contents that have to be inset, which is what the UI does with
// UI_SAFE_PAD. Raise it if text still disappears into a corner.
#define DISPLAY_CORNER_RADIUS   20
#define UI_SAFE_PAD             DISPLAY_CORNER_RADIUS

// Camera preview is 240x240 — it fills the short edge of the rotated panel; centre it on the long one.
#define PREVIEW_WIDTH           240
#define PREVIEW_HEIGHT          240
#define PREVIEW_X               ((DISPLAY_WIDTH  - PREVIEW_WIDTH)  / 2)
#define PREVIEW_Y               ((DISPLAY_HEIGHT - PREVIEW_HEIGHT) / 2)

// ── Touch: CST816 capacitive controller, on the shared I2C bus ──
#define TOUCH_I2C_PORT          SHARED_I2C_PORT
#define TOUCH_I2C_ADDR          0x15            // CST816 default
#define TOUCH_RST_PIN           (-1)            // not in the board doc; assumed tied to the board reset
                                                // like the LCD's. If touch never answers, this is suspect #1.
// The panel is driven rotated, but the controller still reports coordinates in the glass's native
// 240x280 frame, so the rotation has to be undone here. These are the display's mirror flags
// swapped over — the transform runs the opposite way:
//   display 90° CCW (swap_xy=1, mirror_x=0, mirror_y=1) -> touch swap_xy=1, mirror_x=1, mirror_y=0
//   display 90° CW  (swap_xy=1, mirror_x=1, mirror_y=0) -> touch swap_xy=1, mirror_x=0, mirror_y=1
#define TOUCH_SWAP_XY           1
#define TOUCH_MIRROR_X          1
#define TOUCH_MIRROR_Y          0
// Set to 1 to log raw + mapped coordinates on every press — the fast way to check the mapping above.
#define TOUCH_LOG_RAW           0

// ── Audio: ES8311 DAC + ES7210 4-channel ADC + NS4150B amp ──
// The codec pair sits on the shared I2C bus, so EsCodec attaches instead of creating one.
// Watch the two data lines: the board doc names them from the codec's point of view, so its
// "DSDIN" is the ES8311's input (our DOUT) and its "SDOUT" is the ES7210's output (our DIN).
#define AUDIO_I2C_PORT          SHARED_I2C_PORT
#define AUDIO_I2S_MCLK          GPIO_NUM_16
#define AUDIO_I2S_BCLK          GPIO_NUM_9      // doc: I2S SCLK
#define AUDIO_I2S_WS            GPIO_NUM_45     // doc: I2S LRCK
#define AUDIO_I2S_DOUT          GPIO_NUM_8      // doc: DSDIN  — ESP -> ES8311 (speaker)
#define AUDIO_I2S_DIN           GPIO_NUM_10     // doc: SDOUT  — ES7210 -> ESP (mics)
#define AUDIO_PA_EN             GPIO_NUM_48     // NS4150B enable
#define ES8311_ADDR             0x18
#define ES7210_ADDR             0x40
#define AUDIO_SAMPLE_RATE       16000           // what the Agent platform expects (PCM16 mono)
#define AUDIO_MIC_GAIN          30
#define AUDIO_OUT_VOLUME        80
// TTS jitter buffer, in PSRAM. 32 bytes per ms at 16kHz/16-bit/mono, so 256KB ~= 8s of reply held
// against link jitter; the SDK is told this figure and throttles the App before it overflows.
#define AUDIO_PLAY_BUF_BYTES    (256 * 1024)

// ── TF card: 1-line SDMMC (only CLK/CMD/D0 are broken out) ──
#define SD_PIN_CLK              GPIO_NUM_15
#define SD_PIN_CMD              GPIO_NUM_7
#define SD_PIN_D0               GPIO_NUM_4
#define SD_MOUNT_POINT          "/sdcard"
// The App's 0x04 ListRecordings contract names these two directories: recordings sit flat in
// records/, and messages in the subdirectory below it. The .wav filter is what keeps a flat scan
// of records/ from also picking up the messages.
#define SD_REC_DIR              "/sdcard/records"
#define SD_MSG_DIR              "/sdcard/records/messages"
// Recordings are IMA-ADPCM (WAV fmt_tag 0x0011): 4:1 over PCM16, the same format agent_link already
// decodes on the downlink, and openable by any desktop player.
#define REC_FORMAT_ADPCM        1
