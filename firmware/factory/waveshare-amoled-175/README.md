# AMOLED 1.75 board factory and application bundle

Factory for the **non-C, 16 MB** Waveshare ESP32-S3-Touch-AMOLED-1.75.
It uses the existing pins, display rotation, touch, fonts and WiFi product.
The 1.75C profile is separate and does not select the multi-app component.

The board catalog's `build.factory` binds this directory to the board.
[manifest.json](manifest.json) declares only the launcher. The selected app owns
its own manifest; `partitions/layouts.json` owns the layout. Build and package tools
compose these definitions without a factory-owned application list or duplicate offsets:

| Build role | Production source | App ID |
| --- | --- | --- |
| `factory` | `factory/waveshare-amoled-175/` | `agent.launcher` |
| `voice` | `apps/waveshare-amoled-175-wifi/` | `agent.voice` |

The `voice` role builds the Waveshare WiFi application. This factory has its own controller and `ui/` carousel pages.
Factory and Voice share [amoled_ui](../../components/amoled_ui/README.md) and
system services. Factory does not compile voice business/audio sources.
Offsets are parsed from the layout CSV before configuration. Factory and installed
apps all read the `font_cjk` partition; update their binaries and the partition
table together. The font address/capacity and existing layout version stay the same.

## Build

In an activated ESP-IDF 5.5.x or 6.1.x terminal, from the repository root:

```powershell
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --jobs 2
```

Both images build sequentially in `firmware/build`; there is no board
matrix, separate checkout or device write. If a previous interrupted configure
left an incomplete generated sdkconfig, add `--reset-config`. Add `--public`
when preparing images for a shared test service; it rejects private build settings.

`firmware/build/packages/waveshare-amoled-175-wifi/` contains:

| File | Purpose |
| --- | --- |
| `merged.bin` | Initial USB package: launcher, voice app and shared font |
| `factory-only-merged.bin` | Initial USB package: launcher and shared font, with all three app slots empty |
| `flash_assets/font_cjk.bin` | Shared Chinese font at `0xC22000`, capacity `0x230000` |
| `factory.bin` | Factory launcher; never offered by the app test service |
| `voice.bin` | Voice application, with embedded app/board/layout identity |
| `factory.elf`, `voice.elf` and matching maps | Per-image debugging artifacts |
| `flasher_args.json` | Exact package offsets and flash settings |
| `package.json` | App IDs, versions, lengths and SHA-256 hashes for the test service |

Menuconfig first selects **Firmware build target**. Application mode exposes the
chip's application Profile list. Factory mode instead exposes **Factory board**;
it currently contains only **Waveshare ESP32-S3-Touch-AMOLED-1.75 (non-C)**.
Unsupported hardware and application backend names do not appear in that list.
Factory binding is resolved from board metadata and generated into Kconfig.
Switching back to Application uses the saved application selection. CLI Factory
builds continue to accept the existing profile name for compatibility.

From the repository root, explicitly build factory with:

```powershell
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role factory --jobs 2
```

Without `--image-role`, the build tool follows Menuconfig (voice on a fresh
configuration). An explicit `--image-role voice` or `--image-role factory` updates
the same choice. The normal Voice flash plan targets `ota_0` at `0x310000`; the
Factory plan targets `factory` at `0x10000`. Flash/app-flash and exported metadata
use the same plan and write a matching boot choice. Normal Voice writes preserve
Factory. A first-ready Voice app publishes its slot record, including after USB
replacement of a removed app. The shared device settings remain intact.

At actual USB flash time the adapter checks the device layout and target identity;
Voice requires a compatible launcher and an empty or same-app destination. A slot
containing another app is refused. Use the installer for automatic free-slot
selection. A blank board or standalone Voice at the factory address needs the
initial board package or a Factory flash first. No checks open a port during build.

Each role archives its BIN/ELF/MAP, flash metadata and SHA-256 record under
`build/images/<profile>/<role>/`. The board package tool verifies both roles and
combines them with the common layout/font. The package starts Factory with Voice
installed; it also produces the empty-selector variant. Application-only builds
omit `build/merged.bin` because its FF padding would erase Factory. The separate
flash files preserve NVS/other slots; contiguous initial packages reset them.

The same Voice binary remains installable by Factory. Installed apps confirm
rollback readiness and arm a fully verified compatible launcher. Returning checks
the launcher again. Runtime support for older standalone Voice installations is
retained, but standalone factory-partition writes are not the default delivery.

To start with an empty selector, use `build/packages/waveshare-amoled-175-wifi/factory-only-merged.bin`.
It includes the bootloader, partition table, initial OTA data, factory and shared
font, with `0xFF` padding across all app slots and installation records. It also
clears WiFi and binding settings when written as a contiguous USB image. In an
activated ESP-IDF terminal, from `firmware/`, replace `COMx` with the actual port:

```powershell
python -m esptool --chip esp32s3 --port COMx --baud 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 build/packages/waveshare-amoled-175-wifi/factory-only-merged.bin
```

This is a command for the user to run after choosing that write; packaging does
not execute it or open a port. No full-chip erase is needed. On first boot, set
up WiFi, configure the app service and install applications from the final + card.
`factory.bin` alone is only the app at `0x10000`, not a complete initial package.
`idf.py -p COMx flash` uses the active factory build's separate files and preserves
existing OTA app slots, so it does not create an empty selector on a used device.
Both USB packages can be regenerated without compilation after a completed build:
`python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --package-only`.

Initial installation requires a separately authorized USB flash. The partition
table is incompatible with the previous single-app image; do not OTA it over an
old layout. The merged package can replace WiFi, pairing and device identity;
plan to set up and bind again. The package does not require a full-chip erase.

## Device interaction

Factory opens a full-screen cover carousel. Each local application has a large
image and a name. The backend converts the original artwork to a 132x132 circular
RGB565+A8 cover. The carousel displays it at 256 pixels and actions at 180 pixels;
the source artwork stays unchanged. Swipe left/right to browse without wrapping at the ends. The
last page is always a large **+** card; tapping it opens the online app catalog.
An empty device starts on this add card. Installed apps and their artwork work
offline. Tap a local cover to show the app's cover with Launch and Delete actions;
deletion requires a separate confirmation and preserves device settings.
An installed app startup confirms local readiness and arms a verified launcher
as the next boot target, without stopping the current app. Standalone Voice has
no launcher return target. Cloud authentication is not the readiness criterion.

Swipe down from the top center to open the shared System menu. The voice app
also has a System entry in its existing menu; its BOOT call controls remain.
Factory BOOT returns to the carousel.
The selector's ordinary pages contain no battery, network or status ring; those
details are available through the top-down System menu. The common menu provides WiFi
setup/status, current app identity/version, device status, shared volume/language,
app service configuration and Return to launcher. Opening System ends a voice
call; leaving it does not resume that call automatically.
The battery percentage shows a lightning icon while charging. Device status
also lists Charging / Not charging, refreshed with the battery reading once per second.
Scrollable lists keep a visible position thumb at the right while content
overflows. The System root also says "Scroll for more" to make the gesture discoverable.

Factory selects a dedicated display performance policy: two full-screen RGB565
draw buffers plus a transfer/rotation buffer use about 1.24 MiB of PSRAM. Dirty
areas still update independently. A worker on core 0 rotates and transfers pixels
while the UI on core 1 can render the other buffer; LVGL regains a buffer only
after transfer completion. Brightness and recovery wait for the outstanding job.
Panel errors retain the existing bounded IO/recovery behavior.

Factory requests 32-row internal DMA stripes (about 29 KiB) at the existing
40 MHz QSPI clock. Display and animation deadlines are 8 ms; idle touch polling
and pointer reads run every 5 ms at most between rendering work. These are
scheduling targets, not measured frame rates. Actual throughput remains limited
by drawing, PSRAM and QSPI. Static pages do not continuously redraw.

Three recently used cover/size combinations are bilinearly resized once into
PSRAM, including their existing alpha masks. The resident cache is at most
576 KiB, with up to 192 KiB extra during replacement. Cache allocation failure
falls back to scaling the original cover. The existing 132x132 cover protocol
is unchanged. Voice retains its balanced display policy. Device acceptance must
measure animation frame rate, touch gaps,
tearing and free internal/PSRAM memory during cover downloads and installation.
Factory compiles LVGL and the display worker with speed optimization (`-O2`).
Debug diagnostics report completed refresh cycles, active refresh rate (excluding
gaps over 250 ms), transfer duration and maximum touch-poll gap every five seconds.
These diagnostics measure software transfer completion, not panel scanout/TE.

Factory uses WiFi-only networking: captive-portal setup, shared
WiFi credentials, SNTP time synchronization and ordinary HTTP/HTTPS downloads.
It does not start Agent claiming, bind-status polling, authentication, MQTT or
voice sessions. Only Voice enables the existing platform activation and conversation
flow, using the shared binding key when one has already been saved. Returning
to factory reboots into WiFi-only mode and ends Voice's platform session. App
switching does not erase WiFi, device identity or binding credentials.

## Local test app service

On a PC on the same LAN as the device, manually copy only the installable app
images (such as `voice.bin`) into
`firmware/tests/local_app_store/packages/`. That directory defines the HTTP
catalog; no `package.json` is needed. Build output is not published automatically.
See the [local test service guide](../../tests/local_app_store/README.md).

Start the server from the repository root:

```powershell
python firmware/tests/local_app_store/server.py
```

The server scans `packages/` relative to its own directory, even when started
from another working directory. It skips factory and merged USB images. Restart
the server after manually changing the firmware files.

The server prints the PC's candidate addresses. Choose its WiFi/LAN address and
allow the selected TCP port on the private network if Windows prompts. Keep
the process running during downloads; Ctrl+C stops it.

1. Configure the device's WiFi through System → Network. The existing phone
   hotspot portal remains at `http://192.168.4.1`.
2. Open System → App service. On your phone/PC open `http://DEVICE_IP:8081`.
3. Save the PC catalog address, for example `http://192.168.1.20:8765/api/apps`.
4. In factory swipe to the final **+** card, tap it, choose an online app and
   install. Installation does not automatically launch it. Tap the installed
   app's cover in the carousel and choose Launch.
5. Each `app_id` may be installed only once, regardless of version. Online rows
   show Installed, and the SDK rejects duplicates before opening HTTP or writing
   flash, even when an earlier slot is empty. To update, explicitly uninstall
   the current app first. Existing duplicates are not deleted automatically;
   choose the unwanted copy in the local carousel and remove it. Shared settings
   remain intact. A new app ID uses an empty slot; full slots require removal first.

The test server exposes only catalog entries and their pure app images. It
does not expose the merged package, launcher, debugging files or arbitrary paths.
It validates the files at startup and refuses changed bytes during a download.
This is a private-LAN development service, not a production publishing or signing
system. HTTP is accepted only for explicit private IPv4 LAN or `.local` URLs;
production catalogs should use HTTPS. Redirects are rejected.

The future platform can provide the same JSON contract at any configured URL:

```json
{
  "schema": 1,
  "layout": 1,
  "board": "ESP32-S3-Touch-AMOLED-1.75-16M",
  "apps": [{
    "app_id": "agent.voice",
    "name": "Agent Voice",
    "version": "2.0.0",
    "size": 123456,
    "sha256": "64 lowercase hexadecimal characters",
    "url": "/firmware/agent.voice.bin"
  }]
}
```

The size/hash above are illustrative; the server generates actual values.
Maximum response: 16 KB and eight apps. Downloads require a Content-Length.

Application cover URLs point to backend-converted binary blocks, independently
of firmware. Do not put the original PNG/JPEG URL in the device's `cover_url`:

```json
{
  "cover_url": "/covers/agent.voice/3abe0d04353c10a482ef9e2ae151da61962d7bf7a0c0a36f29db5b4703ba7000.raw",
  "cover_format": "alcover-rgb565a8-le-v1",
  "cover_size": 52336,
  "cover_sha256": "3abe0d04353c10a482ef9e2ae151da61962d7bf7a0c0a36f29db5b4703ba7000"
}
```

These values match the supplied Voice artwork converted by the current tool.
Regenerate metadata from the actual output whenever artwork or processing changes.
Omit all four fields for an app without artwork. The test server reads source
file/URL mappings from
[covers.json](../../tests/local_app_store/covers.json).
Relative source paths resolve from the configuration directory. At startup it
converts the source, saves the raw result under the test service's `.cache/covers/`
directory and serves it over the same LAN service. The device does not
need to contact the original picture host. Restart the service after changes.

The shared SDK owns the [backend cover contract](../../components/agent_system/cover-format.md),
including conversion, binary layout, validation and HTTP delivery requirements.

## Shared data and recovery

`nvs` and `otadata` retain their original offsets. `al_wifi` holds the shared WiFi
credentials, `agent_link` the device identity, and `al_dev` the platform binding.
The common menu preserves the existing `voice/vol` and `amoled175/language` keys.
All applications use the same storage and network SDK. Reprovisioning WiFi
preserves platform binding. Do not erase shared NVS to reset an application.

`app_nvs/al_apps` records per-slot installation state. A slot is marked unavailable
before erasing/writing and committed ready only after length, SHA-256, image and
embedded identity checks. Interrupted installs remain hidden and reusable. The
factory, bootloader, partition table, shared NVS and font are never firmware
installer targets. Each OTA slot reserves its last `0xE000` bytes for the cover.
`0xE52000..0x1000000` is unassigned space reserved for a future layout.
There are three application slots; covers use their reserved tails.

| App slot | Cover's absolute flash address |
| --- | --- |
| `ota_0` | `0x602000` |
| `ota_1` | `0x902000` |
| `ota_2` | `0xC02000` |

The installer and packager limit each app image to **`0x2F2000` = 3088384 bytes**
so it cannot overlap its cover. Before updating a pre-existing app's cover, the
SDK verifies the image's real length; oversized older apps are never overwritten.
The factory image retains its full 3 MiB limit. The launcher obtains raw covers
from the configured app service.

The descriptor at image offset `0x120` contains magic, schema, layout, app ID,
display name, hardware model and SDK version. The SDK currently requires layout
1 and SDK 0.1.0. Native firmware shares trust and address space conventions;
namespaces are not hardware isolation between untrusted applications.

Images are not embedded in firmware. In factory, a separate worker synchronizes
covers for matching installed app IDs/versions after WiFi connects (with a short
connection settling delay), when the service URL changes and after opening the
online catalog or installing an app. Successful sync repeats every five minutes;
failures retry after one minute. Browsing remains responsive during background
downloads; an explicit install/remove waits for the current sync to finish.
Installing an app also downloads its cover. Fetching the online catalog itself
does not wait for cover downloads. The SDK downloads into PSRAM, checks length,
whole-block SHA-256, header, app identity and payload CRC before erasing anything.
It writes the slot tail with the magic last, then reads back and verifies the
entire block. No filesystem, NVS cover binding or PNG decoder is used. A valid
matching tail needs no further download. The renderer retains an immutable PSRAM
snapshot while pages change; offline startup loads it from flash.
Download/validation failure preserves the old cover. A power cut or flash write
failure during the single reserved area's replacement can lose the cover, so
the device shows a placeholder and retries; the app image is outside that area.
Cover failure does not prevent firmware installation. Uninstall erases the app
and its cover together while retaining shared device settings.
Factory System → App covers and sync shows waiting/downloading/cached, missing
image metadata, absent catalog versions and failed stages/errors, with a Retry
cover sync action. An app without a configured URL normally uses a placeholder.
Info logs report slot-tail initialization, download start, HTTP status/length
and verified save address; Warn logs identify connection/TLS, receiving,
validation or flash save errors.
URLs, WiFi credentials and query tokens are not logged. Cache hits and repeated
local lookup details are Debug. The application descriptor ABI remains unchanged.

Factory itself is not updated through the app catalog in this prototype.
Failure before an app reaches common startup still depends on bootloader
rollback/USB recovery; arbitrary crash recovery and deep-sleep resume policy
are not claimed. GPIO0 is a ROM download strap, so holding BOOT across reset
is the existing USB recovery route, not a newly added factory recovery key.

## Validation

```powershell
python firmware/tests/test_agent_system.py
python firmware/tests/test_amoled_network_mode.py
python firmware/tests/local_app_store/tests/test_server.py
python firmware/tests/test_amoled_voice.py
python firmware/tests/test_amoled_touch.py
python firmware/tests/test_amoled_ui.py --carousel --rotation 90
python firmware/tools/build_firmware.py --check
python tools/board_catalog.py --check
python tools/check_docs.py
```

Native tests fault-inject production SDK flash/NVS/HTTP boundaries, startup
selection, malformed catalogs, failed downloads, hashes, wrong boards, full slots,
shared-setting retention and the top-edge gesture. The local server tests verify
real HTTP catalog/download behavior. Native LVGL checks render the actual carousel
and verify swipes do not activate covers, the final add card, system gestures and
separate Launch/Delete hit targets. Preview PNGs are exported in logical orientation
under `firmware/build/amoled-voice-tests/ui`. Hardware acceptance still requires the exact
non-C device: initial boot, shared WiFi across apps, offline launch, touch/menu,
download/unplug/retry, uninstall, application return, voice/audio and USB recovery.
