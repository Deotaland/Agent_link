# Mozhifang Paper V2 4IN0

An independent board/profile for the Paper V2 PCB with the Qiutianwei
E0400A01-AF-S 4.0-inch monochrome panel on **FPC3 (24 pins)**. The logical
portrait display is 528x792, rotated 270 degrees (180 degrees from the original
portrait orientation). This is a different firmware
from the 3.6-inch six-color `paper-s31` board.

The board owns its identity, memory defaults and key mapping. Its build manifest
explicitly reuses the Paper V2 SPI/I2C, ES8389, battery, motor, LED, RTC and SD
drivers under `../paper-s31/`. The existing panel driver contains the special
792-column/600-gate scan mapping and inversion for this panel. Do not replace
those controller settings with the logical 528x792 dimensions.

| Peripheral | Wiring |
| --- | --- |
| E-paper | BUSY 7, RST 8, DC 9, CS 10, MOSI 11, SCK 12; active-low power 44 |
| Application keys | KEY3 / GPIO15 up; KEY2 / GPIO14 down; KEY1 / GPIO13 confirm |
| BOOT | GPIO61, download strap only; no application action |
| I2C | SDA 0, SCL 1 |
| Touch | FPC2: shared SDA0/SCL1, RST4, INT5, EPD_3V3 power; GT911/FT6x36/CST816 auto-detection |
| ES8389 | Address 0x10; MCLK36, BCLK16, WS17, DOUT18, DIN37; amplifier enable19 |
| Battery / RTC | BQ27220 0x55 / PCF85063 0x51 |
| Motor / LED | GPIO42 / GPIO3 |
| USB | Type-C USB Serial/JTAG, GPIO33/34 |

Build from the repository root with an activated ESP-IDF 6.1 installation:

```sh
python tools/check_env.py --profile paper-s31-4in0-wifi
python firmware/tools/build_firmware.py paper-s31-4in0-wifi --jobs 2
```

The profile uses the dedicated `paper-4in0-v2` layout and `firmware/build`.
It selects the independent
[Paper 4IN0 desktop](../../apps/paper-s31-4in0-wifi/README.md), product **14**
(`paper-s31-4in0`, chip `esp32-s31`) and OTA model **PAPER-S31-4IN0**.
The product record was supplied by the platform maintainer during this port.
No account token is embedded in this metadata.

| Partition | Address | Capacity |
| --- | --- | --- |
| Future Factory | `0x10000` | 3 MiB |
| `ota_0` / `ota_1` / `ota_2` | `0x310000` / `0x610000` / `0x910000` | 3 MiB each |
| `app_nvs` | `0xC12000` | 64 KiB, reserved for future app settings |
| `font_cjk` | `0xC22000` | `0x230000` bytes (2.1875 MiB) |
| Future layout space | `0xE52000..0x1000000` | `0x1AE000` bytes |

Geometry is defined only in [the CSV](../../partitions/paper-4in0-v2.csv).
The [shared resource pack](../../components/paper_resources/README.md) contains
the raster font and offline pinyin dictionary. The desktop image embeds only a
small Latin fallback. Initial USB installation includes the resource; later
compatible app-only OTA reuses it. The six-color board keeps its original v1 layout.

**Migration from v1 requires a USB package with the new partition table, boot
selection, app and resource.** App-only OTA cannot migrate partition addresses.
NVS stays at `0x9000`, but a full merged image spans settings and reserved areas;
WiFi/activation setup may be needed again. No device write occurs during build.
The [Factory area](../../factory/paper-s31-4in0/README.md) is reserved for a later
launcher; this build explicitly starts the desktop in `ota_0`.

For first setup, join `Paper4IN0-XXXX`, open `http://192.168.4.1`, configure WiFi,
then claim the displayed activation code at `https://bot.deotaland.cn` and bind
an Agent. Firmware building does not flash the board. Hardware acceptance must
check the exact panel orientation/mapping, all three keys, audio, battery, RTC,
SD WAV playback, provisioning, activation and a real PCM16 voice session.
Also check default GT911 touch direction/scaling without calibration, optional
four-point calibration, preserved mapping after failure, saved mapping after restart, nine-key
and full-keyboard pinyin candidates, English/symbol input and explicit review/send.
Input conversion is offline; handwriting is not enabled.

The desktop uses pixel differences to choose partial or full refreshes, and skips
unchanged images. Partial windows support the 270-degree orientation and this
panel's interleaved gate mapping. Full refresh is used for first display, large
windows, periodic residual-image cleanup, manual refresh and error recovery.
The existing OTP waveform may still flash inside a partial window; optical
behavior and residual-image thresholds require hardware acceptance. BOOT remains
usable to enter the chip's USB download mode.
