# Sugarat Launcher for Sugarat Button

This Factory also supports the same Sugarat Button hardware sold by SDGOODS.

This board-bound Factory reuses the round launcher controller and system menu.
Its native 360px renderer follows the warm charcoal and terracotta home layout
of [Sugarat's launcher](https://github.com/suoyousheng-bot/sugarat-sdgoods/tree/c07957fe9794808d6ee866553af169f5654d69e5).
The home summary shows installed app count, free app-pool space and an allocation
ring. Three tiles open **My apps**, **Download apps** and **Settings**. The grid,
app actions and download progress use the same palette, in Chinese and English.

## Startup movie

Factory plays the prepared movie once, with its original soundtrack resampled
for the mono 16 kHz speaker, before starting LVGL and entering the home page.
Video targets 360x360 at 24 fps; late frames are skipped to keep sound and video
on the same timeline. Existing volume settings apply. Missing/corrupt media,
mount/allocation/decoder failures skip playback and continue to home. FAT is
never automatically formatted. Any previous install receipt remains accessible
from **Download apps** after the home page opens.

The `appdata,data,fat,0x1000000,0xD00000` partition holds two root files:
`START.MP4` preserves the supplied original; `START.BVA` contains bounded JPEG
frames plus mono PCM, since this S3 launcher does not decode H.264/AAC directly.
The player loads this small converted file into PSRAM, unmounts appdata before
playback, and releases its buffers before the normal UI starts. File framing,
dimensions, offsets, duration and CRC are checked before playback.

In the activated SDK environment, from the repository root:

```powershell
python firmware/tools/build_firmware.py sdgoods-wifi --image-role factory --jobs 2
python firmware/tools/prepare_boot_animation.py dist/start.mp4 --package-factory
python firmware/tests/test_boot_animation.py
```

The source video is a local input in ignored `dist/`; it is not required to build
the player. Output is in `firmware/build/boot-animation/`:

- `appdata.bin`: verified 13 MiB wear-levelled FAT image for **0x1000000**.
- `factory-with-animation-merged.bin`: verified initial V5 USB package for **0x0**.
- `animation.json` and `flash-files.json`: geometry, media details, hashes and flash files.

Writing `appdata.bin` replaces all existing appdata files. The initial merged
package also replaces settings and installed apps. Ordinary Factory build/flash
metadata deliberately excludes appdata so later firmware-only updates preserve it.
Build/packaging commands never flash a device. On-device playback, sound, frame
rate and transition still require hardware acceptance.

## Launcher behavior

- Open **My apps** (or swipe left on home) for a four-app grid with page controls.
  Up to 14 apps occupy four pages; the empty state opens download-code entry.
- **Download apps** opens six digit boxes and a numeric keypad, following the
  reference launcher's layout. Enter the code shown on the web platform (including
  leading zeroes), tap **Find**, review the app name/version/size and confirm
  **Download and install**. Back returns to the entered code; **?** explains where
  to find it. Codes are shown on the platform's public approved firmware page.
- Tap an app to open its actions; removal requires confirmation.
- The upper-left arrow returns one level. Swiping up from the physical bottom
  edge returns directly home, including from settings and download progress.
  Return arrows and download-code Help have a 10px invisible touch margin. Native launcher buttons use
  the full round touch surface, including the bottom Delete and Find keys, with
  up to 18px finger jitter; scrolling list rows retain their stricter drag guard.
- Download-code queries and installation run on a dedicated worker. Going home leaves
  a download running; **Download apps** reopens its progress or unacknowledged
  result. Opening another app asks whether to interrupt the download and start
  over next time. After confirmation the worker stops at a safe boundary before
  the selected app boots. Atomic layout publication finishes before switching.
- A completed installation automatically restarts into the Launcher after the
  worker finishes and saves its result. No confirmation or app launch is shown.
  On reboot, the installed image is verified and a successful receipt is cleared,
  leaving the home page ready to use. Failed/cancelled outcomes remain visible.
- Errors retain their stage, code, received bytes and available HTTP/server
  details until acknowledged. Installation outcomes are stored in `app_meta` and
  survive restart; pending outcomes are checked against the actual installed
  image after reboot. Partial images remain unavailable.
- Pull down from the physical top edge for WiFi, app service, device, language
  and cover settings. The top cap accepts the gesture from its center or sides,
  including on the download/progress and hardware-test pages. A normal tap or
  list scroll does not open the menu.
- **Settings > WiFi** uses the common Agent Link provisioning flow and existing
  isolated settings. App downloads use the production firmware download-code API;
  provisioning continues to use the common Agent Link flow.
- Without WiFi the home summary becomes a tappable setup guide. While the
  setup hotspot is active it shows the actual hotspot name and `192.168.4.1`;
  a disconnected station instead links to network settings. Code lookup
  and installation direct users there first. Installed apps stay accessible and
  launch normally offline; connecting WiFi restores the storage summary.
- Open **Hardware and power** for battery voltage/estimated level, a live spirit
  level, speaker tone, three-second record/playback, persistent brightness and
  sleep/power/restart controls. The existing volume page controls self-test audio.
- **Settings > Screen brightness** also opens the saved 5–100% brightness control
  directly. Changes take effect immediately and survive a restart.
- **Hardware and power > Display and touch > Start touch test** guides five taps
  (center and four quadrants). Each short tap displays its screen X/Y position;
  only the highlighted target counts within a 24px radius. All five must pass.
  Dragging does not count. **Restart test** clears progress; Back returns to display status.
- **Level and motion test** presents a live bubble level driven by QMI8658 gravity
  readings, with X/Y tilt in degrees. Place the device screen-up and hold the bubble
  centered within 3 degrees for one second, with rotation below 5 degrees/second,
  to pass. Acceleration outside 0.85–1.15 g or read failures cannot pass; the bubble
  is hidden until valid readings return. This is a functional check, not a calibrated
  measuring instrument or independent verification of every gyroscope axis.
  Test screens stay awake, and neither test writes calibration data.
- Hold the key for 1.2 seconds to open power controls. Sleep/off asks for confirmation,
  waits for idle background work and drains the display before changing power.
  GPIO6 wakes deep sleep; USB-powered off uses deep sleep as a fallback.
- The home page shows real estimated battery level and WiFi state. Charging is
  explicitly **unknown** because no charger-status signal is documented.
- Swipe up from the bottom, or press the GPIO6 key, to return home.
- Join `Sugarat-Button-XXXX` and open `http://192.168.4.1` for initial WiFi setup.
- Factory anonymously queries `https://bot.deotaland.cn/api/firmware/code/{code}/download-url`.
  One GET supplies the temporary `download_url`, `firmware` and `product` details.
  After confirmation, another GET refreshes the URL and must match the displayed
  firmware ID/version/size/MD5 before downloading. The nested firmware must be
  public, approved and published; product `hardware_id=2` identifies Sugarat Button,
  independently of which application product owns the firmware. Chip and actual
  image compatibility are checked separately. The six digits stay strings.
  The installer verifies the received size/MD5 and full ESP image before publishing
  an app. It preserves the public release label separately from the binary's IDF
  version. Backend `flash_address` is USB metadata; allocation uses the app pool.
  Code lookup and file downloads carry no account/device headers or cookies.
  Cover/catalog tooling retains `https://sugapi.deotaland.cn`, community product `1`
  and hardware revision `1`; these IDs do not select the firmware-code hardware.
  Saved defaults using the previous production IP migrate to the HTTPS domain.
  Same-host HTTP file URLs emitted by the backend are upgraded to HTTPS.
  The previous port-8765 test catalog migrates once; subsequent explicit overrides
  remain available at `http://DEVICE_IP:8081` and in the system menu for cover sync
  and SDK catalog tooling. Device code entry always uses the configured production
  firmware-code origin. The device never calls the administrator code-assignment API.
- Reviewed original SDGOODS pure applications and native Agent Link applications
  keep their exact binary bytes. Installation discovers the actual image identity,
  validates chip/ABI/size/full image and the service checksum, and records release identity
  separately in `app_meta`. ZIP, UF2 and full-device packages cannot be installed by
  the launcher; the backend must publish a reviewed pure `.bin`. This does not
  add an official SDGOODS browser or local firmware import endpoint on the device.
- A 10 MiB dynamic pool supports up to 14 applications, subject to image sizes,
  alignment, cover reserve and contiguous free space. Installation automatically
  restarts the Launcher to refresh the partition list and confirm the result.
  Removal still restarts the Launcher. The matching named-partition V5 full USB package
  is required initially; application ABI remains 3, with native slot capability version 2 required for the renamed labels.
  Vendor `appdata` has 13 MiB; the last 3 MiB hold Launcher B. Hold GPIO6 at power-on
  for three seconds to enter recovery; keep it held to select a verified previous
  Launcher when one exists. Application files and settings are not erased by this action.

## A/B Launcher updates

Settings includes **Firmware update**. The current version and numeric version code
come from `launcher_version.h`; increment the code for every published release.
Launcher product ID is **25** (`Sugarat_Launcher`, product key `sugarat_launcher`).
Agent Voice and Agent platform provisioning retain product **13** (`sdgoods`). The update
origin is `https://bot.deotaland.cn`; the check endpoint permits anonymous device
access without an account/user header:

- `GET /api/firmware/check-update?product_id=25&current_version_code=1&chip_type=esp32-s3&firmware_type=ota`

Responses use `code=0`, `success=true` and `data`; HTTP and business failures remain
visible on the update page. The check returns `data.has_update`. An update additionally
requires `firmware_id`, `product_id=25`, `is_official=true`, `version`, monotonically
increasing `version_code`, `firmware_type=ota`, `chip_type=esp32-s3`, `file_size`, `file_md5`,
`download_url`, `changelog` and `is_forced`. Publish the **pure** `agent_link.bin`,
not a merged USB image. Its embedded version/code/product/layout capability must
match the release metadata.
The request always filters `firmware_type=ota`; the backend must classify uploaded
USB merged images separately. Device-side size and image checks remain mandatory
even when the server returns a full package incorrectly.
Before downloading, the device calls the same filtered check endpoint again and uses
its fresh `data.download_url` directly (bounded, validated redirects). The returned
`firmware_id`, version/code, size and MD5 must still match the release the user
confirmed. Missing links, withdrawn releases or changed metadata stop the update
before writing the inactive Launcher. The Launcher updater does not call the authenticated
`/api/firmware/{id}/download-url` or `/download` endpoints.
USB metadata `flash_address` (including `0x0`) is not used
as an arbitrary device write destination: the device selects its inactive slot.
The initial USB destination of the complete Launcher image is **0x90000**.
The product-25 baseline is `1.0.0` / `1`; the next release is `1.0.1` / `2`.
Rebuild the image after editing both version macros, then publish the exact same
version/code/product as the embedded image. Changing only backend metadata cannot
create a new release. Devices built with Launcher product 13 need a USB baseline
update to product 25 before checking this independent Launcher release stream.
Keep the immutable recovery image and merged USB image out of the OTA release catalog.

V5 contains a fixed 512 KiB `recovery` entry, two 3 MiB Launchers (`launcher_a`/`launcher_b`, subtypes `ota_14`/`ota_15`),
a separate 10 MiB `app_pool` (expanded to `app_00`..`app_13`, subtypes `ota_0`..`ota_13`), and 13 MiB appdata. The
entry preserves `agent.launcher` / `SDGOODS_LAUNCHER` identity so reviewed vendor
apps can return through Factory. It normalizes the initial table and routes to
the current or pending Launcher. It is built in `build/recovery/` as a small
subproject, using the production source in `recovery/sdgoods-esp32s3/`.

Updates write only the inactive Launcher. Size, MD5, flash readback MD5, ESP image,
board, product, version code and exact layout capability are checked before a
two-sector boot-state journal publishes a trial. The new Launcher must complete
10 seconds of responsive UI/display/touch operation; a 45-second confirmation
deadline restarts an unresponsive trial. A bootloader hook rejects the next boot
of an unconfirmed trial and routes back through recovery. Confirmed images are
not copied, and the old image remains until the next update. This state is separate
from vendor applications' `otadata`; global IDF app rollback stays disabled.

The update page retains failures with HTTP status/reason and reports progress.
Returning home leaves work running; application changes and power actions wait
until it completes/cancels. A staged update offers **Restart into new version**.
Forced updates are indicated but do not silently erase data or interrupt use.

Migration requires a matching initial full USB image; a V4 app-only update cannot
install this layout. Configuration/font/journal addresses move, and an existing
16 MiB FAT volume must be backed up and recreated at 13 MiB. Launcher B executes
above 16 MiB, requiring ESP-IDF's **experimental** 32-bit quad-flash cache mapping.
Successful compilation/host tests do not establish support on the physical flash
chip: A/B boots, vendor-app return, real power cuts and UI confirmation require
device acceptance. If both Launchers are invalid the immutable entry stays in
USB recovery and logs the reason; it does not erase user data or launch arbitrary apps.

## Memory and transfers

This Factory enables `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` for
the R2 download service's cross-signed GTS Root R4 chain. Normal certificate,
hostname and validity checks remain enabled. Certificate failures stop the
current download without retrying and retain a specific user-facing reason plus
numeric TLS diagnostics. Connection failures and gateway/timeouts retain bounded
retries. The image policy restores the previous setting when building an app.

Only this board's Factory image uses software SHA and AES in mbedTLS. The
image-scoped build policy avoids the reported ESP32-S3 I2S/GDMA crypto interaction
after startup audio, while retaining the soundtrack and normal HTTPS certificate
and hostname verification. This is a workaround pending on-device acceptance.
The application image and other boards keep their existing crypto settings;
switching away from this Factory restores the prior values rather than retaining
its override in the shared build directory.

The LVGL heap is 2 MiB in external BSS/PSRAM. Two native RGB565 full-frame draw
buffers plus one asynchronous transfer staging buffer occupy 777,600 bytes
in PSRAM. Rendering and transfer run on separate cores; no scaling of a 466px
framebuffer is performed. Only dirty regions are transmitted.

Four 64px grid covers and one 104px detail cover use independent, single-entry
PSRAM caches with alpha-aware bilinear resampling. Their scaled pixel storage
is bounded to 81,600 bytes, plus at most five immutable 132px source snapshots.
Replacing a page cannot evict another visible image's pixels. Fonts also cache glyphs in PSRAM.
Home storage usage includes installed partition alignment and cover reserves;
the vendor's separate upper-16-MiB appdata partition is excluded.
WiFi/LwIP and TLS prefer external memory; 64 KiB of internal memory is reserved
for tasks, radio and DMA needs.

Installation logs separate cover-worker waiting, catalog/ticket requests,
connection/headers, allocation, erase, transfer, image verification and layout
publication. The final summary includes received bytes, elapsed time, network-read
and flash-write times. Repeated transfer statistics stay at Debug; normal stage
summaries and failures remain visible at Info/Error. For the requested download
diagnosis, `agent_system.install: download_url=...` prints the complete actual
firmware URL, including its short-lived ticket, at Info once per installation.
Each metadata/file-open attempt logs its method/phase, result and elapsed time.
Detail lookup, download-ticket POST and file response headers may all wait up to
60 seconds; once received, data reads return to a 15-second timeout. Each request
retries connection/header or JSON-body timeouts and transient 502/503/504 responses
at most twice, before reading firmware or changing flash. A logical download-ticket
POST keeps the same Idempotency-Key across retries; the next operation gets a new key.
Retries use one/two-second
backoff, respect a short Retry-After, and stop for cancellation; a single in-flight
header wait can still take up to 60 seconds. Expired/forbidden tickets, malformed
responses and validation failures are not retried. Cancellation is checked between
metadata I/O operations and retries. Cover file reads retain their original policy;
shared community metadata uses the same bounded request policy and does not print URLs.
Dynamic-slot erasure uses at most 64 KiB per call with explicit
scheduling gaps, retaining watchdog checks and cancellation between chunks.

Community WebP, PNG and baseline JPEG covers are decoded into the existing
132px RGB565LE+A8 cache. Input is bounded to 2 MiB, each dimension to 2048 and
total pixels to 1024x1024. Decode buffers and codec allocations use PSRAM;
animated images are rejected. No account headers accompany cover/download files.
The legacy raw-cover/SHA catalog remains available for explicit local testing.

See the [community upload contract](../../components/agent_system/README.md#community-upload-contract)
for developer submission and administrator collection of SDGOODS applications.

The QSPI transport owns one permanent **8,640-byte internal DMA stripe** and
its transaction descriptor. Each stripe has a complete window command; it
never lends a PSRAM pointer to DMA. A timed-out descriptor remains owned until
SPI returns it. The existing bounded recovery mechanism then reinitializes the
glass and requests a full redraw. GPIO18 TE synchronizes stationary large frames
with a 40 ms bound; four missing-signal timeouts disable waiting, preserving UI
responsiveness. CST816 GPIO4 interrupts reduce idle reads with polling fallback.
CST816 polls are throttled with slow-read
backoff so flash/network work cannot create a tight I2C retry loop.

See the [board guide](../../boards/sdgoods-esp32s3/README.md) for build/package
commands, pins, first-install migration and pending device checks. Audio is
initialized on an explicit test request. A three-second recording consumes
96,000 PSRAM bytes temporarily; it is not persisted or uploaded. ADC, I2S/PA,
touch and power behavior still need on-device acceptance.

## V5 partition names and developer storage

Labels are `recovery`, `launcher_a`, `launcher_b`, `app_pool`, `app_00`..`app_13`,
`app_meta` and `layout_journal`. Standard ESP-IDF boot subtypes are unchanged.
`appdata` remains a shared 13 MiB developer filesystem, not firmware install space
or a per-app quota. Preserve `/appdata/<app_id>/` directories; START.MP4/START.BVA
are reserved Factory media. Ordinary app and Launcher updates do not format it.
The named V5 policy requires a full USB migration, including matching bootloader
and recovery. Back up and recreate older 16 MiB appdata as 13 MiB explicitly.
Original SDGOODS pure applications remain byte-for-byte unchanged. Old Agent Link
native images using app_nvs/ota_N must be rebuilt with slot capability version 2;
they are rejected before allocation, rather than installed into an unusable layout.
