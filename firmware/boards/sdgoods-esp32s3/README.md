# Sugarat Button

Sugarat is the hardware brand and Button is the model. The same hardware
sold by SDGOODS uses this firmware too. Board lookup accepts `Sugarat`, `Button`,
`Sugarat Button`, `SDGOODS` and the existing SDGOODS/Chinese aliases.
The setup hotspot is `Sugarat-Button-XXXX`.

Agent Link port of the hardware documented by the [SDGOODS BSP](https://github.com/SDGOODS/SDGOODS-ESP32S3/tree/dae46766aa345c680691420691d3d7170f0c6372/components/sdgoods_board).
The board uses a **360 x 360 ST77916 LCD**, not an AMOLED. It has 32 MB flash
and 8 MB of 80 MHz octal PSRAM (ESP32-S3-R8), as specified in the official
hardware table. PSRAM is initialized and checked at boot.

| Peripheral | GPIO / configuration |
| --- | --- |
| LCD QSPI | SCK 40; D0/D1/D2/D3 46/45/42/41; CS 21; 80 MHz |
| LCD power / reset / backlight | 12 (active low) / 9 / 13; PWM backlight |
| LCD TE | 18; synchronization not enabled |
| CST816 touch | I2C1 SDA 11, SCL 10, address 0x15; reset 5; interrupt 4 |
| User key | 6, active low; release after boot to arm |
| Battery power latch | 7, high keeps power on |
| Microphone (reserved) | BCLK 16, WS 2, DIN 17 |
| Speaker (reserved) | BCLK 48, WS 38, DOUT 47, MCLK 15; PA 3 |
| USB | Native USB-C, GPIO19/20 |

Touch uses the vendor mapping `(raw_y, 359 - raw_x)` and LCD MADCTL 0x60.
No PMU, battery ADC or charging telemetry is established by this BSP. The
firmware reports battery level as unknown and keeps the amplifier disabled.

The public hardware name is **Sugarat Button**. The existing build profile
`sdgoods-wifi` and image compatibility model `SDGOODS-ESP32S3` remain stable so
previously built Agent Link images keep their hardware identity. The platform product is
`sdgoods` (ID 13, enterprise 0, supplied by the developer). Its installable
application is Device status; audio/voice capabilities are not advertised.
The board-bound [Factory](../../factory/sdgoods-esp32s3/README.md) owns app
management, with the existing round-screen controller and its own native UI.

## Build

From the repository root in an ESP-IDF 5.5.x or 6.1.x terminal:

```sh
python firmware/tools/build_firmware.py sdgoods-wifi --image-role factory --jobs 2
```

Use `--image-role standard` for Device status. Build the initial launcher +
status application package sequentially in the same `firmware/build` directory:

```sh
python firmware/tools/package_firmware.py sdgoods-wifi --jobs 2
```

The **sdgoods-v2** partition table (layout version 2) has a 3 MB Factory,
three 3 MB app slots and shared settings/font partitions. Each app reserves
56 KiB for its raw cover, leaving 3,088,384 bytes for the application image.
All apps and the memory-mapped font are below 16 MB; physical flash capacity
remains 32 MB and the rest is unallocated. Experimental 32-bit cache mapping
is not enabled.

Application images must use layout descriptor 2; older layout images are
rejected. Changing from a different partition layout or vendor firmware requires
the full USB package and replaces settings and installed applications. Updating
only an app is insufficient. The full package starts Factory. No full-chip erase
is required.

The package directory includes `Sugarat-Button-full.bin` (Factory + Device
status) and `Sugarat-Button-factory-full.bin` (Factory only). These are named
copies of the existing merged packages and use USB offset `0x0`. The package
manifest includes Sugarat/Button display metadata and the SDGOODS search aliases;
its `board` field remains the image compatibility model.

The unchanged vendor glass initialization sequence retains its Apache-2.0
copyright attribution. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

## Device acceptance still required

Verify battery-only boot and retained power, all four touch quadrants, key
debounce, orientation, RGB colors, brightness and dim/wake. Then verify WiFi
setup, service configuration, cover download while rendering, install/launch/
return/remove across all three slots, reboot persistence and USB recovery.
Desktop rendering and a successful build do not establish these device results.
