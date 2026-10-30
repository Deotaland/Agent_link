#pragma once
// OJBadge ESP32-S3R8 / W25Q128 (16 MB flash, 8 MB octal PSRAM).
constexpr int LCD_MOSI = 10, LCD_SCLK = 12, LCD_CS = 13, LCD_DC = 14;
constexpr int LCD_RESET = 11, LCD_BACKLIGHT = 16;
constexpr int TOUCH_SDA = 43, TOUCH_SCL = 44, TOUCH_INT = 38, TOUCH_RESET = 46;
constexpr int USER_BUTTON = 0;  // SW2, active low. SW1 is hardware power only.
constexpr int CODEC_SDA = 6, CODEC_SCL = 7;
constexpr int AUDIO_MCLK = 45, AUDIO_BCLK = 39, AUDIO_LRCK = 41;
constexpr int AUDIO_DOUT = 40, AUDIO_DIN = 42;
// AW8010 enable/control pin has not been supplied: audio remains disabled.
