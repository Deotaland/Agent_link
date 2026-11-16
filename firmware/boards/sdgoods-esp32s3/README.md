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
| LCD TE | 18; bounded interrupt-driven synchronization for stationary large frames |
| CST816 touch | I2C1 SDA 11, SCL 10, address 0x15; reset 5; interrupt 4 |
| User key | 6, active low; release after boot to arm |
| Battery power latch | 7, high keeps power on |
| Battery voltage | GPIO8 / ADC1 channel 7, 12 dB attenuation, 3:1 divider |
| Six-axis IMU | QMI8658, address 0x6B on the touch I2C bus |
| Microphone | I2S1, 32-bit mono left; BCLK 16, WS 2, DIN 17 |
| Speaker | I2S0, 16-bit stereo; BCLK 48, WS 38, DOUT 47, MCLK 15 (384 x Fs); PA 3 |
| USB | Native USB-C, GPIO19/20 |

Touch uses the vendor mapping `(raw_y, 359 - raw_x)` and LCD MADCTL 0x60.
Battery voltage and the six-axis sensor follow the upstream
[hardware implementation](https://github.com/SDGOODS/SDGOODS-ESP32S3/blob/dae46766aa345c680691420691d3d7170f0c6372/components/sdgoods_board/src/sdgoods_hw_info.c).
ADC calibration is preferred; when unavailable, the vendor's uncalibrated
conversion is explicitly labelled. A nine-sample median and smoothing stabilize
the reading; failures display unavailable. Percentage uses the vendor UI's
3.00–4.28 V linear estimate, not a fuel gauge. No charger-status input is documented;
**charging remains unknown**, as agreed, rather than inferred from voltage or USB.

Factory's **Device tools** page starts with **Factory Test**: microphone recording
starts automatically for three seconds, then plays back once. Taps independently
cycle six full-screen colors, including light purple. The recording uses temporary
PSRAM, is not uploaded or saved to a file, and is freed after playback.
Leaving the test stops capture/playback and releases both I2S/DMA channels.
Other tools provide battery/voltage, six-axis readings, a speaker tone, brightness,
deep sleep, power off/restart and display/touch status.
The amplifier stays off outside playback, on mute and after cancellation/errors.
The microphone self-test keeps WiFi connected and does not upload or save audio.
The common volume slider controls these tests. Factory self-tests do not create
Agent sessions; launch the separately installed Agent Voice app for realtime calls.

Brightness is stored in private `agent_nvs/button_hw/bright`. In Launcher, short
key release sleeps on home, closes an open menu or returns one subpage. Holding
for 1.2 seconds and then releasing powers off. Menu Sleep/off still requires
confirmation; every power action requires idle background work and a released key.
The boot/wake-held key is ignored until released. Audio stops and UI/DMA drain
before peripheral rails change; deep sleep holds the GPIO7 latch and wakes from
GPIO6. Power off releases the latch, using deep sleep when USB keeps power on.
Explicit sleep boots the same Launcher and skips its animation once on EXT0 wake;
power off/restart select Recovery and retain cold-start playback. Settings and
installed application partitions are not erased by these actions.

Initial battery connection and USB insertion stay off. The schematic's Q2 is
normally off; the side key supplies battery power temporarily, and GPIO7 latches
it only after a roughly three-second power-on hold. Release to enter Launcher.
USB supplies VCC directly
through D1, so its off state is dark deep sleep while charging remains available.
Recovery and Launcher enforce the same gate before peripheral initialization.
The matching bootloader's existing three-second recovery hold counts toward
cold power-on; it does not require a second hold. Off-state EXT0 wake also requires
three seconds, while intentional sleep accepts a short key wake. Release the
power-on key before Recovery routes through a software restart. The normal power-on hold cannot itself
select the backup Launcher. USB_DET exists in the supplied power schematic but
its MCU pin is not shown; this uniform policy does not depend on it.
Install the matching full USB package once to update Recovery as well as Launcher;
a Launcher-only update cannot replace an older Recovery's early latch behavior.

The public hardware name is **Sugarat Button**. The existing build profile
`sdgoods-wifi` and image compatibility model `SDGOODS-ESP32S3` remain stable so
previously built Agent Link images keep their hardware identity. The platform product is
`sdgoods` (ID 13, enterprise 0, supplied by the developer). Its installable
application is Agent Voice; microphone/speaker capabilities are advertised only after successful audio initialization. Its historical app_id remains agent.status for installed-app identity compatibility.
The board-bound [Factory](../../factory/sdgoods-esp32s3/README.md) owns app
management, with the existing round-screen controller and its own native UI.

## Build

From the repository root in an ESP-IDF 5.5.x or 6.1.x terminal:

```sh
python firmware/tools/build_firmware.py sdgoods-wifi --image-role factory --jobs 2
```

Use `--image-role standard` for Agent Voice. Build the initial launcher +
voice application package sequentially in the same `firmware/build` directory:

```sh
python firmware/tools/package_firmware.py sdgoods-wifi --jobs 2
```

The **sdgoods-v5** bootstrap table (physical version 5, application ABI 3)
contains a 512 KiB `recovery`, two 3 MiB `launcher_a` / `launcher_b` images and
a **10 MiB `app_pool`**. Recovery expands the pool into `app_00` through `app_13`.
Their ESP-IDF subtypes remain ota_0 through ota_13; launcher_a/b retain ota_14/ota_15.
Each installed image reserves 56 KiB for its cover and uses 64 KiB alignment.
A single image can be at most **10,428,416 bytes**; all apps share this pool.
Deletion returns space, but live applications are never moved to compact gaps.
`app_meta` stores installation metadata; `layout_journal` protects table changes.
The shared **13 MiB `appdata` FAT filesystem** starts at 0x1000000. It retains
its original label and `/appdata/<app_id>/` interface for SDGOODS developer apps;
Factory startup media also uses this space. Launcher B starts at 0x1D00000 and
requires experimental 32-bit quad-flash cache mapping. Hardware acceptance is pending.
`agent_nvs` isolates Agent Link WiFi credentials,
device identity, platform binding and app-catalog URL from vendor default-NVS
cleanup. An app-catalog URL stored by an earlier V3 build is migrated when read.
If default vendor NVS has an unsupported version or no usable pages, Factory
reinitializes only that vendor NVS so it can boot; private Agent Link settings
and `appdata` remain intact. Third-party WiFi/account setup follows that
application's own flow; Factory does not inject platform credentials into it.

Native Agent Link images must use layout descriptor 3; older native layouts are
rejected. Original SDGOODS applications use their standard IDF `project_name`
and do not require an Agent Link descriptor. Changing from V2/V3/V4, the earlier V5 labels or vendor firmware requires
the full USB package and replaces settings and installed applications. Updating
only an app is insufficient. The full package starts Factory. No full-chip erase
is required.

## Agent_link Launcher and the reviewed application catalog

The public launcher name is **Agent_link Launcher**. For Sugarat Button, Factory
uses physical **sdgoods-v5**, retaining application ABI **3**. Other boards keep their
own layouts. Vendor pure applications keep their original bytes and use runtime
partition discovery; they are not rebuilt or given a fabricated native descriptor.

All applications are reviewed and published by the operator to the operator's own
backend catalog. The device's Add application page downloads both native Agent Link
apps and approved original SDGOODS apps from that same service. There is no official
SDGOODS market browser and no local binary upload/import endpoint.

Configure the operator's catalog URL in the system app-service settings at
`http://DEVICE_IP:8081` (or `http://192.168.4.1:8081` on the setup hotspot).
The JSON contract is schema 1, target board `SDGOODS-ESP32S3`, layout 3:

```json
{
  "schema": 1,
  "board": "SDGOODS-ESP32S3",
  "layout": 3,
  "apps": [
    {
      "app_id": "CLOCK",
      "name": "Clock",
      "version": "1.2.3",
      "image_format": "sdgoods",
      "url": "/firmware/CLOCK.bin",
      "size": 123456,
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
    }
  ]
}
```

Replace the example size and digest with the published file's actual byte count
and SHA-256. URLs may be absolute HTTPS or relative to the catalog's origin.
Private-LAN HTTP remains available for testing. Up to eight catalog entries are
supported per catalog. Up to 16 applications can coexist when space permits.
Native applications for the renamed V5 layout require AGLCAP01 capability version 2; newly compiled native apps
advertise support for all 16 records. Reviewed compatible vendor SDK apps use
runtime partition discovery. Their image bytes remain unchanged.

- Native apps: `image_format: "agent-link"` (also the default when omitted).
  `app_id` and version must match the native image descriptor; board, chip, layout
  3 and SDK compatibility are checked before publishing the installation.
- Reviewed original apps: `image_format: "sdgoods"`. `app_id` must be the original
  IDF `project_name`, and version must match IDF `version`. The backend serves the
  original pure application `.bin` unchanged. Download size, SHA-256, image/chip
  validation and the 10,428,416-byte pool image limit are enforced. Merged USB packages,
  bootloaders and vendor launchers are not application catalog entries.
- Covers are optional. The backend can supply `cover_url`, `cover_size`,
  `cover_sha256` and `cover_format: "alcover-rgb565a8-le-v1"`. The publishing helper
  library [sdgoods_image.py](../../tools/sdgoods_image.py) performs read-only vendor
  image validation and converts original `SDGCOVER` pixels to the backend cover
  format. Firmware bytes are not rewritten. Apps without covers use placeholders.

Installation chooses a free extent, verifies the image, commits a guarded partition
table and restarts Factory to reload IDF's partition cache. Deletion also restarts
Factory after releasing the extent. Installed applications are never overwritten implicitly.
The user then selects the application to start it; updates require removing the
previous version first.

The Factory's standard IDF project name remains `SDGOODS_LAUNCHER` solely as a
vendor SDK compatibility identifier. This lets approved original applications
recognize their host and use their normal return action; it does not change the
public **Agent_link Launcher** name, Sugarat Button hardware name, hotspot
`Sugarat-Button-XXXX`, or Agent Link OTA model `SDGOODS-ESP32S3`.

Unmodified vendor applications do not confirm Agent Link rollback readiness;
this board therefore disables bootloader automatic rollback. For recovery,
hold GPIO6/side key while powering on, for at least three seconds. This clears
**only OTA selection**, boots Factory and preserves settings, installed apps
and `appdata`. Battery-power latch behavior still requires device acceptance.

Compatibility targets the vendor SDK contract at commit
`dae46766aa345c680691420691d3d7170f0c6372`. Applications above the slot limit,
applications requiring their own fixed partition addresses or custom bootloader,
and vendor standalone firmware updaters are outside this contract. Do not use
an application's full-device updater to replace the Agent Link partition table.
Source/host checks do not establish that every vendor application works on hardware.

Dynamic table writes require the matching V5 recovery bootloader and its two-bank
`layout_journal`. Full USB packages initialize this journal and Launcher trial state.
App-only USB flashing is refused; use Launcher for app downloads. Migration requires
the matching full V5 USB package and replaces settings and installed apps. Back up
the old 16 MiB FAT volume and recreate it as 13 MiB; ordinary firmware-only packages
do not rewrite appdata and therefore do not perform that filesystem migration.
The named partitions change the layout policy even though V5 addresses stay fixed.
Rebuild old Agent Link native apps against this V5 SDK: their former app_nvs/ota_N
lookups are not compatible. The installer rejects old native capabilities before
erasing flash. Original reviewed SDGOODS applications retain their exact bytes,
FAT label, boot subtypes and SDGOODS_LAUNCHER return identity.

The package directory includes `Sugarat-Button-full.bin` (Factory + Agent Voice) and `Sugarat-Button-factory-full.bin` (Factory only). These are named
copies of the existing merged packages and use USB offset `0x0`. The package
manifest includes Sugarat/Button display metadata and the SDGOODS search aliases;
its `board` field remains the image compatibility model.

The unchanged vendor glass initialization sequence retains its Apache-2.0
copyright attribution. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

## Device acceptance still required

Verify battery-only boot and retained power, all four touch quadrants, key
debounce, orientation, RGB colors, brightness and dim/wake. Then verify WiFi
setup, calibrated battery readings against a meter, six-axis signs/units,
speaker/microphone quality and mute/cancel, saved brightness, battery/USB power-off,
deep-sleep current and key wake; then service configuration, cover download while rendering, install/launch/
return/remove across more than two apps, vendor file persistence, private WiFi/binding
preservation, reboot persistence and GPIO6 boot recovery. Test representative
vendor apps (display/input, file-backed media and networking), including power
loss during install, before describing them as device-verified.
Desktop rendering and a successful build do not establish these device results.

## Agent realtime calls

The installable `sdgoods-wifi` app reuses the shared AMOLED realtime controller
and audio workers. After WiFi setup, claim the activation code and bind a voice
Agent in the platform console. Swipe left for conversations, select an existing
conversation or create one, then press GPIO6 to start/end a continuous call.
The microphone opens only after session.ready. SDK reconnects preserve call
intent; real capture pauses while disconnected. During replies the microphone
uploads silence until speaker drain and echo tail complete; this is not AEC.
System settings, volume, language and conversation selection use agent_nvs.

Pull down from the top for the system menu. Only **Exit to launcher** exits the
application. Bottom swipes return within the voice app or close its menu, and
GPIO6 never exits to Launcher. Opening the pull-down menu preserves a live call;
exiting first ends the call. Power-on recovery remains a separate boot mechanism.
The board shares the tested I2S configuration with its Factory self-tests:
16 kHz, left-channel 32-bit microphone samples converted to PCM16 mono, and
separate 16-bit stereo speaker TX with software volume and board-owned PA.
Hardware audio quality, echo, long calls and real network recovery remain unverified.
