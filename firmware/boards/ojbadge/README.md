# OJBadge

ESP32-S3R8 round touch badge, 45.8 x 45.8 x 12.3 mm. Hardware and product
identity were supplied by the developer: product `ojbadge`, ID 11, enterprise ID
0, chip `esp32-s3`. The product lookup helper was attempted but no account token
was available; the identity has not been independently verified against the API.

| Function | Pins / configuration |
| --- | --- |
| Memory | W25Q128 16 MB flash, S3R8 8 MB octal PSRAM |
| GC9A01 LCD | 240 x 240; MOSI 10, SCLK 12, CS 13, DC 14, reset 11, backlight 16 |
| CST816D | SDA 43, SCL 44, interrupt 38, reset 46; polled using shared CST816 driver |
| BQ27220 | Same SDA 43 / SCL 44 bus as touch, address 0x55; read-only |
| SW2 | GPIO0, active low with pull-up; also ROM BOOT |
| SW1 | Hardware power control; not accessed by firmware |
| ES8311 (reserved) | SDA 6, SCL 7; MCLK 45, BCLK 39, LRCK 41, ESP DOUT 40, ESP DIN 42 |
| USB | Native Type-C, D- 19, D+ 20 |

`ojbadge-wifi` currently builds with backend `NONE`: it uses the SDK's existing
ESP-NOW friendship worker and packet protocol, with a local-only radio lifecycle.
It does not link the cloud transport or start Agent Link, provisioning, platform
authentication, MQTT, voice, or WiFi credential loading. Product metadata is
reserved for future platform integration. Audio and the AW8010 remain disabled;
its amplifier control wiring has not been supplied.

The GC9A01 uses Espressif's managed driver and its default initialization table,
40 MHz mode-0 SPI, RGB565/BGR and inversion. Vertical panel mirroring is enabled
to correct the reversed image reported on the device. Touch coordinates are unrotated.
Panel color order, backlight polarity and touch/display alignment need hardware
verification. Do not infer these results from a successful compile.

See [app operation and acceptance](../../apps/ojbadge-wifi/README.md).
