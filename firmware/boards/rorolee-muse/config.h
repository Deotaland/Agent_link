#pragma once
// RoRoLee Muse: the rorolee-s3 PCB (pins identical to boards/rorolee-s3/config.h) with an LVGL
// screen for talking to an agent - link status, hold-to-talk, the answer as text.

#include <driver/gpio.h>
#include <driver/i2c_types.h>

// ── Power ─────────────────────────────────────────────────────────────────────
// Active low: pulling GPIO14 low powers the peripheral rail (display, codec, gauge).
#define POWER_CTRL_PIN          GPIO_NUM_14
#define POWER_CTRL_ACTIVE_HIGH  0

// ── Audio: ES8311 playback + ES7210 capture on one full-duplex I2S port ───────
#define AUDIO_I2S_MCLK          GPIO_NUM_9
#define AUDIO_I2S_BCLK          GPIO_NUM_10
#define AUDIO_I2S_WS            GPIO_NUM_11
#define AUDIO_I2S_DOUT          GPIO_NUM_41   // ES8311 speaker
#define AUDIO_I2S_DIN           GPIO_NUM_12   // ES7210 mic
#define AUDIO_PA_EN             GPIO_NUM_13
#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_MIC_GAIN          30
#define AUDIO_DEFAULT_VOLUME    70            // 0-100; the volume keys step it by 10

// ── I2C: the codec and the BQ27220 fuel gauge share one bus ───────────────────
// The codec creates the bus; the gauge attaches to the same port.
#define AUDIO_I2C_PORT          I2C_NUM_0
#define AUDIO_I2C_SDA           GPIO_NUM_45
#define AUDIO_I2C_SCL           GPIO_NUM_48
#define ES8311_ADDR             0x18
#define ES7210_ADDR             0x40
#define BQ27220_ADDR            0x55

// ── Display: SH8501 AMOLED 120x240, factory short-code ("LK") bring-up ────────
// 40MHz is the production value: 30MHz corrupts some panels' init writes (black screen).
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_CS_PIN          GPIO_NUM_7
#define DISPLAY_DC_PIN          GPIO_NUM_46
#define DISPLAY_SCK_PIN         GPIO_NUM_5
#define DISPLAY_MOSI_PIN        GPIO_NUM_4
#define DISPLAY_RST_PIN         GPIO_NUM_6
#define DISPLAY_WIDTH           120
#define DISPLAY_HEIGHT          240
#define DISPLAY_SPI_CLK_HZ      (40 * 1000 * 1000)
#define DISPLAY_BRIGHTNESS      0xC0          // 0-255

// Text reads across the long edge, so the UI is laid out landscape (240x120) and rotated onto the
// portrait panel at flush time, the same way work-badge does it.
#define UI_WIDTH                DISPLAY_HEIGHT
#define UI_HEIGHT               DISPLAY_WIDTH
// 1 = counter-clockwise (matches the production firmware's text screens), 0 = clockwise. Flip
// this one value if the screen reads upside down on your unit.
#define UI_ROTATE_CCW           1

// ── Chinese text ─────────────────────────────────────────────────────────────
// Montserrat has no CJK glyphs. A TrueType font (make_cjk_font.py in font/ builds the image)
// sits in this flash partition - the production animation pack's, unused by this firmware - and
// TinyTTF draws it straight from flash. Without the image the board shows Latin text only.
#define CJK_FONT_PARTITION      "anim_pack"
#define CJK_FONT_PX             16
#define CJK_FONT_CACHE_GLYPHS   128           // rendered glyphs kept (LVGL heap, PSRAM here)

// ── 32.768 kHz crystal on GPIO15/16 (CONFIG_RTC_CLK_SRC_EXT_CRYS, patched in by the repo-root
// CMakeLists). Nothing else may use these two pins. Documentation only.
#define XTAL_32K_P_PIN          GPIO_NUM_15
#define XTAL_32K_N_PIN          GPIO_NUM_16

// ── Buttons: active low, internal pull-up ─────────────────────────────────────
// BOOT: hold to talk; a press while the link is still being set up confirms pairing.
// VOL+/VOL-: speaker volume. Hold VOL- for FORGET_HOLD_MS to forget the pairing and set up again.
#define BUTTON_BOOT_PIN         GPIO_NUM_0
#define BUTTON_VOL_UP_PIN       GPIO_NUM_39
#define BUTTON_VOL_DOWN_PIN     GPIO_NUM_40
#define FORGET_HOLD_MS          6000

// ── Haptic motor ──────────────────────────────────────────────────────────────
#define HAPTIC_PIN              GPIO_NUM_1
#define HAPTIC_ACTIVE_HIGH      1

// ── SD card, SDIO 4-bit (not used by this firmware) ───────────────────────────
#define SD_PIN_D0               GPIO_NUM_3
#define SD_PIN_D1               GPIO_NUM_21
#define SD_PIN_D2               GPIO_NUM_47
#define SD_PIN_D3               GPIO_NUM_17
#define SD_PIN_CLK              GPIO_NUM_18
#define SD_PIN_CMD              GPIO_NUM_8
