# Agent_link Launcher for Sugarat Button

This Factory also supports the same Sugarat Button hardware sold by SDGOODS.

This board-bound Factory reuses the round launcher controller and system menu.
Its native 360px renderer follows the warm charcoal and terracotta home layout
of [Sugarat's launcher](https://github.com/suoyousheng-bot/sugarat-sdgoods/tree/c07957fe9794808d6ee866553af169f5654d69e5).
The home summary shows installed app count, free app-pool space and an allocation
ring. Three tiles open **My apps**, **Download apps** and **Settings**. The grid,
app actions and download progress use the same palette, in Chinese and English.

- Open **My apps** (or swipe left on home) for a four-app grid with page controls.
  Up to 16 apps occupy four pages; the empty state opens download-code entry.
- **Download apps** opens six digit boxes and a numeric keypad, following the
  reference launcher's layout. Enter the code shown on the web platform (including
  leading zeroes), tap **Find**, review the app name/version/size and confirm
  **Download and install**. Back returns to the entered code; **?** explains where
  to find it. Administrators assign codes on the platform after submission approval.
- Tap an app to open its actions; removal requires confirmation.
- The upper-left arrow returns one level. Swiping up from the physical bottom
  edge returns directly home, including from settings and download progress.
- Download-code queries and installation run on a dedicated worker. Going home leaves
  a download running; **Download apps** reopens its progress or unacknowledged
  result. Opening another app asks whether to interrupt the download and start
  over next time. After confirmation the worker stops at a safe boundary before
  the selected app boots. Atomic layout publication finishes before switching.
- Errors retain their stage, code, received bytes and available HTTP/server
  details until acknowledged. Installation outcomes are stored in `app_nvs` and
  survive restart; pending outcomes are checked against the actual installed
  image after reboot. Partial images remain unavailable.
- Pull down from the top for WiFi, app service, device, language and cover settings.
- **Settings > WiFi** uses the common Agent Link provisioning flow and existing
  isolated settings. App downloads use the production community download-code API;
  provisioning continues to use the common Agent Link flow.
- Without WiFi the home summary becomes a tappable setup guide. While the
  setup hotspot is active it shows the actual hotspot name and `192.168.4.1`;
  a disconnected station instead links to network settings. Code lookup
  and installation direct users there first. Installed apps stay accessible and
  launch normally offline; connecting WiFi restores the storage summary.
- Open **Hardware and power** for battery voltage/estimated level, six-axis sensor
  readings, speaker tone, three-second record/playback, persistent brightness and
  sleep/power/restart controls. The existing volume page controls self-test audio.
- Hold the key for 1.2 seconds to open power controls. Sleep/off asks for confirmation,
  waits for idle background work and drains the display before changing power.
  GPIO6 wakes deep sleep; USB-powered off uses deep sleep as a fallback.
- The home page shows real estimated battery level and WiFi state. Charging is
  explicitly **unknown** because no charger-status signal is documented.
- Swipe up from the bottom, or press the GPIO6 key, to return home.
- Join `Sugarat-Button-XXXX` and open `http://192.168.4.1` for initial WiFi setup.
- The default app service is the production community at `https://sugapi.deotaland.cn`,
  product `1`, hardware revision `1`. Factory anonymously queries
  `GET /api/v1/community/download-codes/{code}` for published play/firmware details,
  then requests `POST /api/v1/community/download-codes/{code}/downloads` only after
  confirmation. It rechecks play/version/size/SHA-256 before requesting a ticket,
  and verifies the ticket against the displayed firmware. The six digits are
  never converted to a number. No account ID, device authentication or binding is needed
  for this store flow. These community IDs are separate from Agent platform product 13.
  Saved defaults using the previous production IP migrate to the HTTPS domain.
  Same-host HTTP file URLs emitted by the backend are upgraded to HTTPS.
  The previous port-8765 test catalog migrates once; subsequent explicit overrides
  remain available at `http://DEVICE_IP:8081` and in the system menu for cover sync
  and SDK catalog tooling. Device code entry always uses the configured production
  community origin. The device never calls the administrator code-assignment API.
- Reviewed original SDGOODS pure applications and native Agent Link applications
  keep their exact binary bytes. Installation discovers the actual image identity,
  validates chip/ABI/size/full image/SHA-256 and records the community play/version
  separately in `app_nvs`. ZIP, UF2 and full-device packages cannot be installed by
  the launcher; the backend must publish a reviewed pure `.bin`. This does not
  add an official SDGOODS browser or local firmware import endpoint on the device.
- A 10 MiB dynamic pool supports up to 16 applications, subject to image sizes,
  alignment, cover reserve and contiguous free space. After installation choose
  **Finish and restart** to refresh the partition list and confirm the result.
  Removal still restarts Factory. The matching V4 full USB package
  is required initially; application ABI remains 3 for existing native images.
  Vendor `appdata` occupies the upper 16 MiB. Hold GPIO6 at power-on for three
  seconds to recover Factory without erasing application files or settings.

## Memory and transfers

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
