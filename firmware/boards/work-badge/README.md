# work-badge — Electronic Work Badge

An **electronic work badge**: on the screen is an employee card (name / title / department / employee ID / company), with the content delivered by an APP or Agent and stored in NVS. After a power cycle or OTA upgrade, it's still the same person.

> **The screen uses the manufacturer's abbreviated command (LK) driver.** The screen's power and Gamma are burned into the screen's internal OTP, so the abbreviated command path must be used.
> Driver `components/esp_lcd_sh8501_lk/`, wrapped in [`../common/sh8501_lk_panel.h`](https://../common/sh8501_lk_panel.h).
> If you switch to the full-sequence version (`Sh8501Panel`), production screens will show a black screen.

Because it's the same board, both boards report `Model()` as `"RRL-01"` — this is the **prerequisite for the APP to be able to push this firmware to existing production devices**.

Broadcast name: `ROROLEE_<last 6 hex digits of MAC>` (generated at boot using `esp_read_mac(ESP_MAC_BT)`)

text

```
boards/work-badge/
├── config.h        pins (board_config.h) + screen orientation/brightness
├── config.json     metadata
├── badge_data.*    five fields + NVS persistence
├── badge_ui.*      LVGL UI (badge screen + upgrade screen)
└── work_badge.cc   Board subclass: hardware init, I/O endpoints, battery, OTA progress hook
```



## UI

Two screens drawn with LVGL 9, rendered on a **landscape 240x120** logical canvas, rotated on flush to the portrait-mounted 120x240 panel
(the badge is read horizontally). The abbreviated-command driver is blocking SPI; when `DrawBitmap` returns, the flush is already complete, and the pixels are sent out via an internal DMA staging
buffer (so the LVGL canvas can be placed in PSRAM).

If the orientation is wrong, change one of these two places:

c

```
// Screen is lying on its side → boards/work-badge/config.h
#define BADGE_ROTATE_CCW  1   // 1 = counterclockwise 90° (default), 0 = clockwise
// Screen is upside down 180° → MADCTL in the abbreviated command table in boards/common/sh8501_lk_panel.cc
{0x36, (uint8_t[]){0x00}, 1, 0},   // 0x00 ↔ 0xC0 flips it
```



- **Badge screen**: top bar with company name + battery/connection icons; in the middle is a card with a blue side stripe, with the name in a large font
  (automatically stepping down between 28/20/14 depending on character count, rather than using an ellipsis — truncating the name on a badge defeats the purpose), and below it a line
  "title | department"; below the card is the employee ID. Short messages sent by the Agent via `on_show_text` are displayed in the lower right corner.
- **Upgrade screen**: OTA takes over full-screen as soon as it starts, with a progress bar + percentage + "Keep the badge powered".
  On success it turns green, on failure it turns red and displays the error code.

The two buffers are 57.6KB each and placed in PSRAM (one for LVGL to draw into, one to store the rotated result). The UI only redraws
when the data actually changes — the badge is a static image the vast majority of the time and should not constantly burn SPI and CPU.

## What the APP / Agent can change

The manifest publishes five `str`-type OUT endpoints, written using `0x33 IoActuate` (args = raw UTF-8, without a trailing `\0`):

| Endpoint        | Content                    |
| :-------------- | :------------------------- |
| `badge.name`    | Name (the large-font line) |
| `badge.title`   | Title                      |
| `badge.dept`    | Department                 |
| `badge.id`      | Employee ID                |
| `badge.company` | Company name (top bar)     |

In addition, the SDK automatically synthesizes `screen0` (i.e. `on_show_text`) → a temporary hint message in the lower right corner.

Each field is at most 47 bytes; overly long values are truncated at UTF-8 boundaries (so half a character is never cut off); writing persists to NVS immediately, and if the value hasn't changed, flash is not written and the UI is not redrawn.

## Chinese font (requires manual work)

**Currently the built-in LVGL Montserrat is used, which only has Latin letters and icons — Chinese will display as boxes.** LVGL
fonts must be generated offline; the repository does not include prebuilt font files. To display Chinese:

1. Use [lv_font_conv](https://github.com/lvgl/lv_font_conv) to generate an LVGL font from a Chinese TTF,
   including only the characters you need (the full font is too large, so trim it to your list):

   bash

   ```
   npx lv_font_conv --font SourceHanSansSC-Medium.ttf --size 24 --bpp 4 \
     --format lvgl --range 0x20-0x7F --symbols "张伟王芳研发部工程师..." \
     -o badge_font_cjk_24.c
   ```



2. Put the generated `.c` into this directory (it will be compiled automatically), then change one place at the top of `badge_ui.cc`:

   c

   ```
   LV_FONT_DECLARE(badge_font_cjk_24);
   #define BADGE_FONT_NAME (&badge_font_cjk_24)
   ```



3. The character-count thresholds in `FontForName()` are based on Montserrat's width; the width of Chinese characters is roughly equal to the font size, so remember to adjust them accordingly.

## Build

bash

```
idf.py set-target esp32s3          # first time or when switching chips
idf.py menuconfig                  # Agent Link Device -> Board Type -> ESP32-S3 Work Badge
idf.py build flash monitor
```



`set-target` brings in the PSRAM and Montserrat 20/28 font sizes from `sdkconfig.defaults.esp32s3`;
the external 32.768kHz crystal is baked into sdkconfig by the board-level table in the repository root `CMakeLists.txt` (same as rorolee-s3).

## OTA

No code needs to be written on this board: the SDK handles the APP's `0x37`/`0x56`, writes flash itself, verifies SHA-256, and reboots.
This board only does one extra thing — register a progress callback to draw the percentage on the screen:

cpp

```
agent_link_ota_set_callback(OnOtaProgress, nullptr);   // work_badge.cc: StartUi()
```



The callback runs on the OTA worker task, so it only writes a few atomics, and the actual drawing is handed off to the render task.
