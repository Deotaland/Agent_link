# Agent_link Launcher for Sugarat Button

This Factory also supports the same Sugarat Button hardware sold by SDGOODS.

This board-bound Factory reuses the round launcher controller and system menu.
Its native 360px renderer uses a dark navy background, cyan accents, large
antialiased circular covers, a separate app name and gesture hint, page dots,
and explicit launch/remove actions. It supports Chinese and English.

- Swipe horizontally to browse installed apps and the final Add application card.
- Tap an app to open its actions; removal requires confirmation.
- Pull down from the top for WiFi, app service, device, language and cover settings.
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
- The default app service is the production community at `http://3.144.1.190:9004`,
  product `1`, hardware revision `1`. Factory anonymously queries published plays,
  pages through the list, reads version details and requests an expiring download
  ticket when installing. No account ID, device authentication or binding is needed
  for this store flow. These community IDs are separate from Agent platform product 13.
  The previous port-8765 test catalog migrates once; subsequent explicit overrides
  remain available at `http://DEVICE_IP:8081` and in the system menu.
- Reviewed original SDGOODS pure applications and native Agent Link applications
  keep their exact binary bytes. Installation discovers the actual image identity,
  validates chip/ABI/size/full image/SHA-256 and records the community play/version
  separately in `app_nvs`. ZIP, UF2 and full-device packages cannot be installed by
  the launcher; the backend must publish a reviewed pure `.bin`. This does not
  add an official SDGOODS browser or local firmware import endpoint on the device.
- A 10 MiB dynamic pool supports up to 16 applications, subject to image sizes,
  alignment, cover reserve and contiguous free space. Installing/removing an app
  restarts Factory to refresh the partition list. The matching V4 full USB package
  is required initially; application ABI remains 3 for existing native images.
  Vendor `appdata` occupies the upper 16 MiB. Hold GPIO6 at power-on for three
  seconds to recover Factory without erasing application files or settings.

## Memory and transfers

The LVGL heap is 2 MiB in external BSS/PSRAM. Two native RGB565 full-frame draw
buffers plus one asynchronous transfer staging buffer occupy 777,600 bytes
in PSRAM. Rendering and transfer run on separate cores; no scaling of a 466px
framebuffer is performed. Only dirty regions are transmitted.

Three recent cover sizes use bounded PSRAM caching and alpha-aware bilinear
resampling. At the largest 198px size, their pixel storage is at most 352,836
bytes, plus immutable source snapshots (184px home covers use less). Fonts also cache glyphs in PSRAM.
WiFi/LwIP and TLS prefer external memory; 64 KiB of internal memory is reserved
for tasks, radio and DMA needs.

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
