# Sugarat Launcher for Sugarat Button

This Factory also supports the same Sugarat Button hardware sold by SDGOODS.

Agent Link WiFi uses a 15 dBm transmit-power ceiling: PHY initialization is capped
at 15 dBm and STA/AP startup applies driver value 60 (0.25 dBm units), logging
the readback. Actual transmit power may be lower for a particular rate or region.

This board-bound Factory reuses the round launcher controller and system menu.
Its native 360px home shows one large circular application cover. All cover views
clip the image itself to a circle, including opaque square uploads. Horizontal
swipes cycle through installed apps and the final **+** card, then wrap around.
Tapping an app opens a launch/delete dialog over its cover. The shared controller,
installation service, provisioning and A/B update APIs are unchanged.

Selecting Launch first shows a modal **Starting app... Please wait** message.
The controller waits for the display-transfer acknowledgement and a short visible
interval before requesting the reboot. This message stays on the panel during
boot; failures dismiss it and return to app actions instead of leaving a stuck modal.

Factory and the native Agent_link app default to English, including first boot,
WiFi setup, menus, download/update progress and errors. Missing or invalid saved
language settings fall back to English; an explicitly saved Chinese/English
choice is preserved. The phone's WiFi form and connection-result page also open
in English without JavaScript and offer a Chinese toggle. Other boards retain
their existing default language.

## Startup movie

Factory plays the prepared movie once, with its original soundtrack resampled
for the mono 16 kHz speaker, before starting LVGL and entering the home page.
Explicit sleep resumes the same Launcher and consumes a one-shot RTC marker to
skip the movie on GPIO6/EXT0 wake. Cold boot, restart and Power off (including
its USB-powered deep-sleep fallback) retain normal startup playback.
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

For the default initial package, first build the Agent_link application, then
Factory, and include the application along with the font and startup movie:

```powershell
python firmware/tools/build_firmware.py sdgoods-wifi --image-role standard --jobs 2
python firmware/tools/build_firmware.py sdgoods-wifi --image-role factory --jobs 2
python firmware/tools/prepare_boot_animation.py dist/start.mp4 --package-factory --install-agent-link
```

The package verifies both archived images, SDK/layout agreement and application
identity before merging. Recovery discovers the single preinstalled `agent.status`
image through the existing V5 seed path. It appears as **Agent_link** in the UI,
with the supplied 256px logo converted to the standard circular 132px cover and
preloaded into its normalized slot tail, so the initial cover works offline.
Factory-only
OTA updates preserve the installed list and do not reinstall a deleted app.

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

- After startup playback, home opens directly on the first installed app's large
  circular cover. Swiping left/right cycles through up to 14 apps and the **+**
  card. Left from **+** returns to the first app; right from the first returns to
  **+**. A device with no apps still offers the **+** card.
- **+** opens six digit boxes and a numeric keypad, following the
  reference launcher's layout. Enter the code shown on the web platform (including
  leading zeroes), tap **Find**, review the app name/version/size and confirm
  **Download and install**. Back returns to the entered code; **?** explains where
  to find it. Codes are shown on the platform's public approved firmware page.
- The **?** page shows `https://sugarat.vercel.app/` as the firmware catalog and
  source of six-digit download codes. After WiFi setup, the native Agent_link
  activation screen shows `https://bot.deotaland.cn/#/devices` beside its binding
  instructions and six-digit code. These are UI links; backend endpoints and
  activation/install behavior are unchanged.
- Tap an app to open a popup with **Launch** and **Delete**; deletion has a second
  confirmation popup. Tap **×** in the upper-right corner or outside the popup
  to dismiss it. Neither action opens a separate application detail page.
- The upper-left arrow returns one level. Swiping up from the physical bottom
  edge returns directly home, including from settings and download progress.
  Return arrows and download-code Help have a 10px invisible touch margin. Native launcher buttons use
  the full round touch surface, including the bottom Delete and Find keys, with
  up to 18px finger jitter; scrolling list rows retain their stricter drag guard.
- Download-code queries and installation run on a dedicated worker. Going home leaves
  a download running; **+** reopens its progress or unacknowledged
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
- Pull down from the physical top edge for a dark **Quick Panel**. Its header
  shows the battery percentage and a dot before the panel name. Four rounded
  tiles form two rows: **Wi-Fi | Sound**, then **Exit | Settings**. Wi-Fi opens
  the existing network page; Sound opens the existing vertical volume control.
  Back from these shortcuts returns to Quick Panel. In Launcher, Exit only shows **Already in
  Launcher** and leaves the current page/work running. In installed native apps,
  Exit requires confirmation, then restarts into the Launcher home. Bottom-up
  home navigation continues to leave downloads running.
  The top cap accepts the gesture from its center or sides,
  including on the download/progress and hardware-test pages. A normal tap or
  list scroll does not open the menu.
- Pull right from the physical left edge for a **Back** badge. Release after a
  sufficient pull to return one level; move back to the edge before release to
  cancel. One contact cannot return through multiple pages. Visible Back buttons
  and bottom-edge Home remain available. The app-cover carousel keeps both
  left/right app switching gestures, including a swipe started at the edge;
  it never turns that contact into Back or shows the Back badge.
- **Quick Panel > Wi-Fi** uses the common Agent Link provisioning flow and existing
  isolated settings. App downloads use the production firmware download-code API;
  provisioning continues to use the common Agent Link flow.
- Settings is an ordinary scrolling list: **Brightness**, **Launcher update**,
  **About**, device status, language, **User guide** and **Device tools**. Wi-Fi
  and volume are shortcuts in Quick Panel rather than repeated settings entries.
  App service and cover-sync entries are hidden; automatic cover sync continues.
  Back returns one level. **Settings > About** shows the running app name,
  actual image version, WiFi status, SSID/IP and battery.
- **User guide** has five scrollable topics: Launcher purpose, WiFi setup,
  download-code installation, apps/gestures, and updates/help. It defaults to
  English and follows the selected UI language. Guide Back returns to its topic
  list, then Settings; bottom-edge Home still goes directly home.
- **Quick Panel > Sound** opens a large vertical capsule: drag up/down for 0–100%, with live
  percentage/fill feedback. The existing volume callback applies changes and
  the existing release/commit path saves them. Dragging inside the capsule
  cannot navigate Back/Home; the physical bottom-edge Home gesture remains.
- Without WiFi, the carousel has a tappable setup hint. Network settings show
  the actual hotspot and `192.168.4.1` while provisioning. Installed apps stay
  accessible offline. Code lookup and installation direct users to setup first.
- The same pull-down/menu presentation ships in the native Agent_link app.
  Its Launcher update row confirms a restart and passes only a UI navigation request;
  Launcher opens the existing update page and checks once WiFi is connected.
  Installation remains explicit. Original SDGOODS binaries retain their own UI;
  a Launcher update cannot replace another app's compiled menu.
- **Device tools** appears immediately above **About** in Settings. Its first
  entry, **Factory Test**, automatically records three seconds and then plays
  the captured audio once while showing full-screen black. The only test input
  is tapping to cycle white, red, green, blue and light purple, then black.
  Recording/playback needs no extra button, and colors remain available after
  playback finishes. The clip stays temporarily in PSRAM and is freed afterward.
  Back, Home and opening
  Quick Panel stop the audio worker and release both I2S channels. Errors return
  to Device tools with a visible reason. Microphone capture stops before playback.
  Other tools provide battery voltage/estimated level, a live spirit level,
  speaker tone, persistent brightness and sleep/power/restart controls.
  The existing volume page controls self-test audio.
- **Brightness** opens the same large vertical capsule with a sun icon and a
  5–100% range. Changes apply live and save to the existing `button_hw/bright`
  setting on release, outside the LVGL task. Both the shortcut and hardware page
  use this view; native Agent_link uses the same UI and isolated setting.
- **Device tools > Screen test** starts with full-screen black. Each tap advances
  through white, red, green, blue and light purple, then wraps to black. No text, buttons or
  toast overlays cover the test colors. Swipe from the left edge to return to Device tools;
  bottom-edge Home and the top-down system menu remain available. Test colors
  stay awake and do not change the saved brightness.
- **Device tools > Display and touch > Start touch test** guides five taps
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
- The physical key follows the vendor's release gestures: short press on home
  enters deep sleep, short press in a menu closes it, and short press on a subpage
  goes back one level. Hold for at least 1.2 seconds and release to power off;
  holding alone never cuts power. A boot/wake-held key is ignored until released.
  The existing menu Sleep/off actions still require confirmation. All paths retain
  the background-work guards and drain the display before changing power.
  GPIO6 wakes deep sleep without replaying the startup movie; USB-powered off
  uses deep sleep as a fallback. Initial battery/USB power stays off; hold the key
  for about three seconds and release to start. Off-state wake requires the same hold and
  then plays the normal startup movie. Recovery shares this admission gate,
  counts the existing bootloader hold once and does not treat an ordinary
  power-on hold as a request to select the backup Launcher. Install the full
  matching USB package to update Recovery; Launcher-only OTA cannot change an
  older Recovery's unconditional power latch.
- The home page shows real estimated battery level and WiFi state. Charging is
  explicitly **unknown** because no charger-status signal is documented.
- Swipe up from the bottom to return home.
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

Every Launcher boot checks for updates once after WiFi connects, without opening
the update page. **Auto download** defaults on and persists in agent_nvs. Shared
menu switches use blue/white on the right for on and gray/white on the left for off.

Background downloads run at priority 1 and transfer at up to about 64 KiB/s.
Browsing, swiping, ordinary menu actions, code lookup and level controls do not
pause them. App installation/removal, cover writes, audio tests and restart/power
actions take priority; downloading resumes automatically as soon as those tasks
finish. Network or memory failures back off for 30 seconds before retrying, rather
than applying that delay to every resource pause. Network waits
never hold the app-operation mutex. Flash writes use 4 KiB sectors and durable
64 KiB checkpoints. Partial images resume after network loss, app launches or
power loss, using fresh signed URLs and validated 206 Content-Range responses.
Ignoring Range restarts safely from zero; invalid ranges or hashes cannot boot.

Ordinary completed downloads stay in the inactive Launcher partition. A home
notice and a one-time notification invite the user to **Restart into new version**.
Other reboots keep the current Launcher, and installed apps remain usable.
Errors and progress remain visible in update settings.

is_forced=true overrides the auto-download preference. Required updates still
download quietly at low priority and yield to resource-intensive work. Verification
arms the trial without restarting immediately: the next reboot, including an
app-launch reboot, enters the new Launcher through Recovery. A version/MD5 that
fails boot confirmation is quarantined to prevent automatic rollback loops.

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
