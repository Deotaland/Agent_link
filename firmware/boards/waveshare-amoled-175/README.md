# Waveshare ESP32-S3-Touch-AMOLED-1.75C

Target hardware is the **C variant**, confirmed by the developer on 2026-10-30.
The legacy directory/profile name `waveshare-amoled-175-wifi` remains in use. The installed firmware model
`ESP32-S3-Touch-AMOLED-1.75` is retained for OTA compatibility; it does not identify
this as the non-C PCB. Platform product ID remains 10 and the registered key
remains `esp32-s3-touch-amoled-175`, as explicitly requested by the developer.
Use these profiles only on the ESP32-S3-Touch-AMOLED-1.75C (SKU 33691/33692).

## Hardware

The pin settings follow the official [C-board pin definitions](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/blob/4bfdc80eedbf95031e384610796bed15e0240312/examples/arduino/libraries/board_config/pin_config.h)
and [C-board BSP](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/23a6012d5e87d27399cf90fb851cab3689e29159/bsp/esp32_s3_touch_amoled_1_75c).

| Function | Pins / configuration |
| --- | --- |
| MCU / memory | ESP32-S3R8, 8 MB octal PSRAM, 32 MB flash |
| Shared I2C | SDA 15, SCL 14; 400 kHz |
| CO5300 QSPI | CS 12, CLK 38, D0-D3 4/5/6/7, reset 1; 40 MHz |
| Display | 466 x 466, RGB565, column gap 6 |
| CST9217 | I2C 0x5A, reset 2, interrupt 11; mirror X and Y |
| ES8311 output | I2C 0x18, I2S DOUT 8; NS4150B enable 46 (active high) |
| ES7210 input | I2C 0x40, I2S DIN 10; standard stereo MIC1/MIC2 |
| Shared I2S | MCLK 16, BCLK 9, WS 45; PCM16 stereo at 16 kHz |
| AXP2101 | I2C 0x34; read-only battery/charging telemetry |
| USB | Native USB-C, GPIO19/20; USB Serial/JTAG console |
| Call button | BOOT GPIO0 active low; 40 ms debounce |

The previous non-C MCLK GPIO42 could not clock the codecs on this board. The C
board also has different LCD/touch reset pins and 32 MB flash. The shared font
and application partition offsets are retained. The application owns amplifier
gating; codec-dev does not also drive GPIO46. AXP2101 default power rails are
retained. MIC1 is the mono upload source and MIC2 has separate diagnostics.
The analog playback reference on MIC3 is not uploaded. Speaker playback uses
the existing microphone suppression guard, not acoustic echo cancellation.
BOOT ends a call during a reply or connection recovery; PWR keeps its hardware
power function. The IMU and RTC are not application controls.

Touch coordinates are mirrored once to `(465 - raw_x, 465 - raw_y)`.
Raw diagnostic report bytes remain unchanged.

## Touch UI and status ring

The 6-pixel ring is inset 7 pixels from the 466-pixel screen edge. It never
expands when pulsing. All viewport corners fit inside a concentric 416-pixel
circle, with a 12-pixel gap to the ring's inner edge. Layout rectangles are
checked at compile time. The black OLED interface uses rounded dark cards,
20-pixel body text, restrained accents, a breathing state ring and a home indicator.
Rectangular viewports clip scrolling content without a full circular subtree mask.
No control shadows or scale effects can extend outside the boundary.

| Ring | State |
| --- | --- |
| Gray | Offline / starting |
| Yellow | WiFi setup or activation required |
| Blue | Connecting, reconnecting or a pending request |
| Green | Online, no active call |
| Cyan | Voice session ready, listening |
| Purple | SDK reports the Agent is preparing its reply, before PCM arrives |
| Orange | Reply PCM buffered or playing, including the I2S drain and echo tail |
| Red | Platform rejection or local request/audio failure |

The text above the Agent name repeats the ring state. Purple follows the SDK's
`Receiving agent reply` status (`response.started`); it is not inferred from
microphone loudness. Orange persists until queued PCM and the speaker drain
and echo suppression finish. An idle screen dims after 60 seconds; the first touch wakes it without
activating a control.

The Chinese/English call home contains state, scrollable reply text and a live
0–100 volume slider. Call start/end use only the physical BOOT key. Press BOOT to start the selected conversation immediately, or hang up
an active/connecting call from any page. Without a selected conversation the key
opens selection. On an idle app page BOOT returns home; another press starts the
call. Holding the key produces only one event. A key held during startup is
ignored until released.

Swipe left on home to open the app launcher. Conversations, Settings and Friend
are separate pages. Swipe right in the launcher returns home; swipe right in an app goes back one
level. The visible Back button does the same. A swipe cannot also click a row under the finger.
Swipe up from the bottom home-indicator area (y=400–438) to return directly to
the call home; swipes in the body only scroll. All lists scroll vertically with
native momentum. Long session lists load automatically at either edge using a
12-row window with four overlapping rows; there are no previous/next buttons.
A data-window replacement retains its scroll anchor and rejects stale row taps.
Conversations offers selection, creation and deletion. Browsing apps keeps the
current call; selecting/creating/deleting a conversation ends it.

Replies follow new text only while the reader is already near the bottom and
is not touching the screen. Reading older text retains the scroll position;
the down-arrow returns to the latest text. BOOT hangup publishes idle/ended
feedback before waiting for audio/network shutdown, wakes the panel and retains
the last reply. Starting the next call clears the ended state.

The slider works on the call home and in Settings/Volume, including during a
reply. Touch callbacks publish only a coalesced value; the controller applies
codec changes and saves NVS only on release. Slider drags do not navigate apps.
The controller prints render/applied revisions, flush count, longest transfer
and heartbeat age every five seconds. A stale heartbeat reports a possible UI
stall; it does not reboot the device or alter the audio link.
Settings shows live WiFi connection, SSID, IP, RSSI and platform state; it also
changes volume by 5, switches between Chinese and English, or resets activation.
The language choice is applied immediately and persisted in NVS. Delete/reset require confirmation.
Reset retains WiFi; unbind the device in the console before activating again.
Blocking platform work runs on the controller worker. Touch events from an old
view or older than 500 ms are discarded.

## Setup and build

The developer supplied the platform registration on 2026-10-29:
`esp32-s3-touch-amoled-175`, product ID 10, chip `esp32-s3`, on
[bot.deotaland.cn](https://bot.deotaland.cn). The account-token lookup was not
available in the development environment; no account token is compiled in.

1. Build with an activated ESP-IDF 5.5.x or 6.1.x environment:
   `python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --jobs 1`.
   The helper reuses `firmware/build`.
2. After an explicitly requested flash, join `Amoled175-XXXX`, open
   `http://192.168.4.1`, and provision a 2.4 GHz WiFi network.
3. Claim the six-digit activation code in the platform console and bind an
   Agent with realtime voice support. Select or create a conversation on-device.
4. Short-press BOOT, wait for cyan, then speak. Press again to finish.

The SDK owns authenticated platform APIs, grants and bounded transport recovery.
Microphone capture waits for `session.ready` and pauses outside READY. The
initial call has a 30-second timeout; established calls have no application age
limit. Changing conversations or hanging up cancels recovery; opening the app
launcher and Settings alone does not end the call.

## Validation and device acceptance

Both profiles include the repository's [shared Noto Sans SC font](../common/fonts/README.md),
with all 6763 GB2312 Hanzi and Chinese punctuation. The existing `anim_pack`
partition stores the font. No partition offsets are changed. USB split flash
and the merged image include the font automatically; app-only OTA requires the
font to have been installed already. Keep `fonts-OFL.txt` with the firmware.
The display task loads flash-backed 14/20/24 px fonts with 64/32/32-glyph caches,
retains Montserrat for ASCII, and falls back to the built-in SimSun subset if
the shared image is missing or damaged. The offline test reports this condition.

Host checks: `python firmware/tests/test_amoled_voice.py` exercises the production
touch parser and circular layout constraints. Standard profile/product and
documentation checks apply. Build only this profile with one or two jobs.

Physical acceptance remains required: cold boot on USB and battery, panel colors
and ring bounds, touch alignment and wake behavior, audio input/output and PA
polarity, volume, AXP2101 telemetry, provisioning/activation, conversation APIs,
long replies, speaker echo suppression, WiFi reconnect and sustained calls.

## PCM capture diagnostics

Network voice remains Opus with the server-negotiated rates. The SDK application
boundary remains PCM16 mono at 16 kHz in both directions. This board's
`amoled_audio.cc` uses the official C-board topology: master standard I2S TX/RX,
16-bit stereo at 16 kHz, MCLK x256 on GPIO16, physical MIC1/MIC2 mask 0x03,
and 24 dB input gain. Capture reads interleaved left/right frames in 20 ms blocks,
uploads MIC1 as mono and measures MIC2 independently. Playback duplicates each
SDK mono sample to both stereo output channels. There is no TDM or resampling
in this C-board path. The shared `EsCodec` driver remains unchanged.

The board checks data-interface format/enable results even where codec-dev open
ignores them, and requires the complete DMA byte count. Capture start logs
ES7210 clock, format, mute and input gain registers. Scratch buffers reside off
task stacks; the microphone task retains its 8 KB stack. Reports run on the
controller worker every five seconds plus a final partial window on stop.
`PCM mic1` and `PCM mic2` measure hardware MIC1/MIC2 respectively.
`PCM raw` measures MIC1 mono before echo suppression.
`PCM uplink` measures the voice samples after the existing suppression and before
the SDK encodes them. Each line contains frames, samples, signed min/max, absolute
peak, RMS, DC mean, all-zero frames, quiet frames and near-clipped samples.
Quiet means frame peak at most 64 on the PCM16 scale and includes all-zero frames;
it is a diagnostic threshold, not VAD. Near clipping means absolute sample value
at least 32760. No waveform, recording or transcript is stored by this diagnostic.

`PCM path` records echo-muted frames, stream drops/errors, capture errors and the
microphone/playback tasks' historical minimum free stack in bytes. Capture IDs
separate recording runs. `final=1` marks a partial final window. A bounded latest
snapshot avoids queuing logs behind blocking platform requests; `skipped` reports
windows the controller did not print.

Speak for about ten seconds, then pause for three seconds. If raw levels stay near
zero, inspect capture/channel/codec configuration. If raw levels respond but
uplink levels are zero with echo-muted frames, inspect suppression timing. If both
respond to speech but downlink stays empty, continue into encoding/server VAD.
Keep the matching ELF and capture the `PCM` and existing `voice` lines together.

## Nearby friends over ESP-NOW

Both devices must run this compatible Agent Link firmware and enter the Friend
app. Entering ends voice, pauses cloud WiFi, and acquires channel 6 through the
SDK. Credentials, binding and selected conversation are retained. The cloud
workers wait for network readiness rather than racing a stop/start cycle.
Exiting by Back, a right swipe or BOOT releases ESP-NOW and restores WiFi.

The scrollable list shows up to six devices with a local `Link-XXXXXX`
name and RSSI. Advertisements expire after five seconds. Select a peer; both
screens show a six-digit code. Compare the codes and confirm on both devices.
Either party may reject. Invitations and confirmations time out after 30 seconds.

P-256 ECDH plus fresh radio-session nonces derive an ephemeral ESP-NOW LMK,
application authentication key and a separately labelled shared PMK. Both peers
must use the updated friends module; builds with independent random PMKs cannot
complete the encrypted confirmation exchange. OJBadge's local-only
`ojbadge-wifi` profile uses the same module. The six-digit code is a separate short
verification value, never a transmitted secret key. Discovery/public-key frames
are public; subsequent traffic uses encrypted unicast plus an HMAC. New nonces
prevent reuse of messages from a previous pairing. Callback work only copies
packets into a bounded queue; cryptography and lifecycle work run on a worker.

The connected screen offers Say hello and Disconnect/search. Greetings are
acknowledged, retried and deduplicated, with sent/received counters. A five-second
heartbeat timeout returns to discovery. Pairing is temporary; this version does
not save a friends address book or carry voice between devices.

`python firmware/tests/test_amoled_voice.py` runs the production protocol between
two simulated radios, including loss, retries, consent, replay rejection and
expiry. This is protocol validation; radio range, two-device pairing, BOOT
behavior, language persistence and cloud restoration need device acceptance.
`python firmware/tests/test_friends_radio.py` additionally exercises the production
radio adapter's symmetric PMK installation, failure cleanup and authentication
paths using deterministic crypto fakes; it does not replace a two-device RF test.

Official key sampling reference:
[ButtonTest.cpp](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/firmware/brookesia/components/ButtonTest/ButtonTest.cpp).
The application uses only BOOT/GPIO0, matching the official direct GPIO sampling.
PWR is left to the board power-management circuit; no PWR expander initialization
or polling is performed. Raw GPIO0 changes, accepted presses and controller
conditions are logged separately. Use the ELF from the exact flashed build when
reading logs; a checksum mismatch means the monitor's local ELF differs.


## UI stall diagnosis (2026-10-30)

The captured UI build `cc43bed36` successfully ended calls at 160.8 s and
192.9 s, but its renderer stopped progressing at about 126.5 s: applied revision
2520 and completed flush count 6756 stayed fixed. The log establishes a renderer
stall, not its exact blocking function. Native LVGL long-text/cache churn tests
have not reproduced the hardware stall; no display-driver reset is assumed safe.

Repeated identical status, battery, volume, screen and text notifications now
coalesce without invalidating the UI. Five-second diagnostics report the current
stage and age: touch/I2C, snapshot/lock, header, reply measure/set-text/layout/scroll,
list update/layout, brightness/SPI, LVGL timers/draw, panel/SPI, heap-check or idle.
Flush completion/start counters and the active rectangle distinguish an in-flight
panel transfer from work before it. Text receive/change/apply counts and byte
lengths distinguish incoming data from rendering without printing conversation
content. LVGL free/largest block and UI stack minimum are sampled only on the UI
task; the controller only reads atomics. The diagnostic never calls LVGL from a
second task, resets the panel, ends a call, or reboots the device.

`python firmware/tests/test_amoled_ui.py` renders production widgets with native
LVGL, checks scroll/state/volume behavior, repeated-notification coalescing and
300 long Chinese text/cache churn updates. Its preview images are generated
renderings, not on-device screenshots. Hardware stall resolution still requires
a new device log with `stage=`, `text rx=` and `render revision=` lines.
