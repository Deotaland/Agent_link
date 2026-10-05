#pragma once
// WOWNNY Muse: the WOWNNY board - the rorolee PCB with a GC9D01 160x160 round TFT instead of the
// SH8501 AMOLED, and a few pins moved to make room for it - with the same LVGL talk screen as
// rorolee-muse. Pins from the production firmware's WOWNNY column.

#include <driver/gpio.h>
#include <driver/i2c_types.h>

// ── Audio: ES8311 playback + ES7210 capture on one full-duplex I2S port ───────
// Same wiring as rorolee except the PA enable (GPIO13 there).
#define AUDIO_I2S_MCLK          GPIO_NUM_9
#define AUDIO_I2S_BCLK          GPIO_NUM_10
#define AUDIO_I2S_WS            GPIO_NUM_11
#define AUDIO_I2S_DOUT          GPIO_NUM_41   // ES8311 speaker
#define AUDIO_I2S_DIN           GPIO_NUM_12   // ES7210 mic
#define AUDIO_PA_EN             GPIO_NUM_3
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
// The cell is 500 mAh (800 on rorolee). This firmware only reads the gauge; the production firmware
// writes the capacity into it on first boot, and the gauge keeps it while the battery is connected.
#define BAT_DESIGN_CAPACITY_MAH 500

// ── Display: GC9D01 round TFT 160x160, SPI mode 0, BOE init sequence ──────────
// Upright as mounted, so the UI is drawn straight onto the panel, no rotation.
// GPIO43 doubles as UART0 TX, the console's primary output: the panel takes it over as its reset
// line when the display starts, after which the log continues on USB (USB-Serial-JTAG, the
// secondary console) only. The production firmware runs the same way.
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_CS_PIN          GPIO_NUM_6
#define DISPLAY_DC_PIN          GPIO_NUM_7
#define DISPLAY_SCK_PIN         GPIO_NUM_5
#define DISPLAY_MOSI_PIN        GPIO_NUM_4
#define DISPLAY_RST_PIN         GPIO_NUM_43
#define DISPLAY_WIDTH           160
#define DISPLAY_HEIGHT          160
#define DISPLAY_SPI_CLK_HZ      (20 * 1000 * 1000)
// Backlight enable, active low. Where rorolee switches its whole peripheral rail with GPIO14, this
// board powers the codec and the gauge directly and switches only the backlight.
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_46
#define DISPLAY_BACKLIGHT_ACTIVE_HIGH 0

// ── Chinese text ─────────────────────────────────────────────────────────────
// Montserrat has no CJK glyphs. A TrueType font sits in this flash partition - the production
// animation pack's, unused by this firmware - and TinyTTF draws it straight from flash. The image
// is the one boards/rorolee-muse/font builds; without it the board shows Latin text only.
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
// D3 moved to GPIO14 because GPIO16 belongs to the crystal.
#define SD_PIN_D0               GPIO_NUM_8
#define SD_PIN_D1               GPIO_NUM_21
#define SD_PIN_D2               GPIO_NUM_47
#define SD_PIN_D3               GPIO_NUM_14
#define SD_PIN_CLK              GPIO_NUM_17
#define SD_PIN_CMD              GPIO_NUM_18
