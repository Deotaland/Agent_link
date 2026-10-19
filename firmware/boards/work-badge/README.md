# RoRoLee work badge

`work-badge-ble` displays name, title, department, employee ID and company,
persisted in NVS. It uses the RoRoLee RRL-01 hardware and reports `Model()` as
`RRL-01` for OTA compatibility. BLE advertises as `ROROLEE_<MAC suffix>`.

Application sources are in `apps/work-badge/`; this directory holds hardware
configuration. The production panel needs
[Sh8501LkPanel](../common/sh8501_lk_panel.h), which uses the panel's factory OTP
settings. Confirm the panel variant before changing drivers.

## Display and I/O

The UI uses a landscape 240x120 canvas, rotated to the 120x240 panel. It redraws
when data changes and switches to an OTA progress screen during upgrades.
`BADGE_ROTATE_CCW` in `config.h` selects the canvas rotation.

The manifest exposes five string output endpoints, written through `IoActuate`:

| Endpoint | Value |
| --- | --- |
| `badge.name` | Employee name |
| `badge.title` | Job title |
| `badge.dept` | Department |
| `badge.id` | Employee ID |
| `badge.company` | Company |

Values are limited to 47 UTF-8 bytes and truncated at character boundaries.
Changed values are saved immediately; repeated values do not write NVS.
`screen0` shows a temporary message. Built-in Montserrat fonts have no CJK glyphs.

For a custom font, generate an LVGL font containing the required characters,
put its `.c` file in `apps/work-badge/`, declare it in `badge_ui.cc`, and change
`BADGE_FONT_NAME`. Adjust `FontForName()` to its glyph widths.

## Build

From `firmware/` in an activated ESP-IDF environment:

```sh
python tools/build_firmware.py work-badge-ble --jobs 2
```

The profile supplies PSRAM, RTC and font settings. After an authorized flash,
check rotation, field persistence, BLE pairing and OTA progress on the device.
