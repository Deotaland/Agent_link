# Waveshare AMOLED 1.75C touchscreen test

This standalone application tests the ESP32-S3-Touch-AMOLED-1.75C using
the production CO5300 and CST9217 drivers. It does not initialize WiFi, BLE,
audio or platform activation, and does not write NVS. The original voice
application remains available as `waveshare-amoled-175-wifi`.

## Build

Activate the existing ESP-IDF 5.5.x or 6.1.x environment, then from the repository root:

```sh
python firmware/tools/build_firmware.py waveshare-amoled-175-touch-test --jobs 2
```

The build reuses `firmware/build`. It produces `agent_link.bin`, `merged.bin`,
the matching ELF/MAP and flash metadata. The shared Noto Sans SC font is included
in `flash_assets/anim_pack.bin` and the merged image; keep `fonts-OFL.txt` with
the package. An app-only image does not install the font for the first time.
Use split flash files for an authorized USB write that retains NVS. A merged
package can overwrite settings. Building this test does not authorize flashing.

## On-device procedure

1. On an explicitly requested flash, use the board's native USB-C connector and
   the build's flash metadata. Read the serial log at 115200 baud.
2. Check `touch init = OK; Chinese font = OK` in the log. The screen displays
   Chinese labels, live screen coordinates, IRQ level and the D000 report bytes.
3. Tap and release each highlighted target in order: center, top, right, bottom,
   left. Holding or dragging across another target does not count as another tap.
   Completed targets become green. Five completed targets confirm those five
   tap locations only; they do not automatically calibrate or prove the entire panel.
4. Drag around the center and edge. The crosshair follows screen coordinates,
   with the latest 64 trail points. Text, targets and trails are clipped inside
   the status ring. Coordinates continue to be displayed for valid points
   outside the inner content mask; their marker can be clipped there.
5. Release your finger. The screen should show the released state and the UP
   counter should increase. Repeat taps and compare DOWN/UP counts.
6. Tap the Chinese Clear button to clear counters, targets and trail. This also
   checks the LVGL pointer-to-button path used by the voice interface.

Blue means ready/released, cyan means pressed/moving, green means five targets
completed, and red means no valid report for at least 250 ms (or initialization
failed). Invalid reports and I2C errors have separate counters. An invalid read
does not count as UP. A stale pointer cancels a pending LVGL click before it is
released; a bus fault must not trigger the Clear button.

Serial DOWN/UP events are logged immediately, MOVE at most five times a second,
and counters plus the full ten-byte report once a second. The production driver
is not automatically reset/reconfigured after a bad report, so failures remain
visible for diagnosis. Only the first contact is used; this does not validate
two-finger gestures, multitouch tracking or touch interrupt delivery.

## Coordinate orientation

The [official C-board ESP-IDF BSP](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/23a6012d5e87d27399cf90fb851cab3689e29159/bsp/esp32_s3_touch_amoled_1_75c/esp32_s3_touch_amoled_1_75c.c)
uses `swap_xy=0`, `mirror_x=1`, `mirror_y=1` for its default display orientation.
The board applies both mirrors once in the shared touch driver. For this 466-pixel
screen, `screen_x = 465 - raw_x` and `screen_y = 465 - raw_y`. Valid contacts
stay within 0..465, and release/error reports are not mirrored. The display and
text keep their existing orientation. The D000 bytes are unchanged; DOWN/MOVE
logs show both raw and screen coordinates. UP retains the last screen location
and uses -1 for raw coordinates because a release has no valid contact position.

## Host checks

```sh
python firmware/tests/test_amoled_touch.py
python firmware/tests/test_amoled_voice.py
python firmware/tests/test_cjk_font.py
```

These cover the production parser, tap/release state machine, stale/error handling,
target bounds and font asset coverage/CRC. Physical touch alignment, continuity
and response time still need the board.
