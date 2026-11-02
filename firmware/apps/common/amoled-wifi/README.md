# Shared AMOLED WiFi application

The non-C `waveshare-amoled-175-wifi` and C `waveshare-amoled-175c-wifi` profiles
select this implementation through `app_shared: "amoled-wifi"`. Each app directory
contains one `DECLARE_APPLICATION` entrypoint. The selected board supplies
`config.h`, `amoled_hardware.h` and its codec implementation; this directory owns
UI, resident fonts, sessions and audio workers. Connectivity remains in the SDK.

Board config defines `AMOLED_DEVICE_NAME`, `AMOLED_FIRMWARE_MODEL` and
`AMOLED_AUDIO_DESCRIPTION`. Preserve the C board's existing name/model. The new
non-C model has a `-16M` suffix to distinguish incompatible OTA images.
`DISPLAY_ROTATION_CW` selects 0 or 90 degrees clockwise: the UI rotates partial
RGB565 flushes and inversely maps touch before hit testing and gesture tracking.
The non-C board selects 90; the C board retains 0.

Menu actions require a stationary press and release on the original option.
Movement beyond 12 pixels on either axis, leaving an option or starting a list
scroll cancels that contact's click until the finger is lifted.
The session picker always includes a New session row, including when opened by
BOOT without a saved selection. Its paging totals count conversations only.

The status ring changes color with call activity and stays opaque. A breathing
opacity animation invalidates almost the entire 466x466 screen, delaying touch
polls on the same task; unchanged calls therefore perform no background redraws.
Pointer releases retain LVGL's last valid position because CST9217 release/error
reports do not carry coordinates. This prevents a volume drag from jumping to
zero when the finger is lifted. Five-second UI diagnostics report input poll
counts, errors, maximum poll gap, volume events and commits.

Host controller/audio checks: `python firmware/tests/test_amoled_voice.py`.
Touch/profile checks: `python firmware/tests/test_amoled_touch.py`.
The C-board audio fixture continues to verify its unchanged standard-stereo path;
the non-C board adapter is checked separately against the shared TDM codec API.
