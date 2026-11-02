# Waveshare ESP32-S3-Touch-AMOLED-1.75 (non-C)

This port targets the standard **16 MB** board (SKU 31261), using the developer's
platform registration: product 10, enterprise 0, key `esp32-s3-touch-amoled-175`,
chip `esp32-s3`, at [bot.deotaland.cn](https://bot.deotaland.cn).
Product lookup needs an account token, which was unavailable during this port;
the supplied registration is recorded in `profiles/products.json`.

## Hardware reference

Pin assignments were checked against Waveshare's
[official documentation](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75/),
[hardware reference](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/e4344e70c2fa78a13e8a06566507f1ba8af6672a/HARDWARE_REFERENCE.md)
and [maintained BSP](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/e4344e70c2fa78a13e8a06566507f1ba8af6672a/firmware/brookesia/components/waveshare__esp32_s3_touch_amoled_1_75/esp32_s3_touch_amoled_1_75.c).

| Function | Non-C 1.75 | 1.75C |
| --- | --- | --- |
| Flash / PSRAM | 16 MB / 8 MB octal | 32 MB / 8 MB octal |
| CO5300 QSPI CS / clock / data 0-3 | GPIO12 / 38 / 4,5,6,7 | Same |
| Display / touch reset | GPIO39 / 40 | GPIO1 / 2 |
| Shared I2C SDA / SCL | GPIO15 / 14 | Same |
| CST9217 interrupt | GPIO11 | Same |
| Audio MCLK | GPIO42 | GPIO16 |
| BCLK / WS / playback / capture | GPIO9 / 45 / 8 / 10 | Same |
| NS4150B PA enable | GPIO46, active high | Same |
| BOOT | GPIO0, active low | Same |

The shared bus contains ES8311 (0x18), TCA9554 (0x20), AXP2101 (0x34),
ES7210 (0x40), PCF85063 (0x51), CST9217 (0x5A) and QMI8658 (0x6B).
This port reads PMU telemetry and retains the existing power/charging settings.
It does not drive expander outputs or repurpose PWR, SD, RTC, IMU or GNSS pins.
Display and touch use separate resets; touch mirrors X/Y, matching the BSP.
The UI is rotated 90 degrees clockwise relative to the panel's default direction;
touch coordinates are transformed back to the UI after those sensor mirrors.

Audio reuses `boards/common/es_codec.*`: standard I2S TX and four-slot TDM RX,
MCLK x256. The codecs run at 16 kHz (supported by the ES7210 clock table) so no
sample-rate conversion is needed at the SDK PCM16 mono boundary. Capture uses
MIC1/slot 0; the shared diagnostics do not sample MIC2 and leave that slot empty.
MIC3 is a playback reference and MIC4 is unconnected; hardware AEC is not enabled.

The firmware model is **`ESP32-S3-Touch-AMOLED-1.75-16M`**. The C-board firmware
already uses the unsuffixed model for installed-device compatibility, so this
new model prevents cross-board OTA selection. The platform product name remains
`ESP32-S3-Touch-AMOLED-1.75`. Select the non-C full package when first flashing.

## Application and build

`apps/waveshare-amoled-175-wifi` registers one Application. It shares the circular
UI, Chinese fonts, session management and audio workers in `apps/common/amoled-wifi`
with the C board. Pins and codecs stay board-specific; provisioning, activation,
MQTT, authenticated conversations, realtime grants and OTA use the existing SDK.

From the repository root, in an activated ESP-IDF 5.5.x or 6.1.x environment:

```sh
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --jobs 2
```

The build reuses `firmware/build` and includes the shared Chinese font asset.
Do not use the C-board image: its reset/MCLK pins and flash size differ.

After an explicitly requested flash, join `Amoled175-16M-XXXX`, open
`http://192.168.4.1`, configure 2.4 GHz WiFi, then claim the six-digit activation
code in the platform console and bind an Agent. Short-press BOOT to start/end a
call. Swipe left for conversations, settings and nearby ESP-NOW friends.

## Acceptance

Compilation and host checks do not verify the device. On this non-C board,
validate cold boot and USB recovery, display/reset/brightness, touch alignment,
BOOT, battery/charging telemetry, microphone level and intelligibility, speaker
volume/PA muting, WiFi provisioning/activation, two-way Agent calls, reconnect,
and entering/exiting nearby mode. SD, RTC, IMU and GNSS applications are omitted.
