# Waveshare ESP32-S3-ePaper-3.97 WiFi

Profile: `waveshare-epaper-397-wifi`; firmware model: `WS-EPAPER-397`.
ESP32-S3-WROOM-1-N16R8, 16 MB flash and 8 MB octal PSRAM. The 3.97-inch
800 x 480 panel is monochrome; this is a different board from paper-s31.

## Hardware sources

Pins, active-low inputs, panel commands and TG28 power rails follow the
[official documentation](https://docs.waveshare.com/ESP32-S3-ePaper-3.97),
[schematic](https://files.waveshare.com/wiki/ESP32-S3-ePaper-3.97/ESP32-S3_e-Paper-3.97-schematic.pdf)
and [official ESP-IDF examples](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/tree/main/ESP-IDF).
The official integrated example drives the TG28 with an AXP2101-compatible
register map at 0x34. This port checks its chip ID before writing power registers.

| Function | Pins / device |
| --- | --- |
| Shared I2C | SDA 41, SCL 42; PMIC 0x34, ES8311 0x18 |
| ES8311 I2S | MCLK 13, BCLK 14, WS 47, DIN 21, DOUT 48 |
| NS4150B amplifier | Enable GPIO39, active high |
| E-paper SPI3 | SCLK 11, MOSI 12, CS 10, DC 9, RST 46, BUSY 3 (active high) |
| Three-way key | Up 4, center 5, down 6, all active low |
| BOOT / USB | BOOT 0; native USB D- 19 / D+ 20 |
| Power rails | ALDO1 amplifier, ALDO2 codec/mic, ALDO3 panel, all 3.3 V |

DC1, charger settings and the independent PWR key retain their hardware behavior.
Only display, buttons, microphone, speaker and battery reporting are advertised.
TF storage, IMU, RTC and temperature/humidity sensors are outside this profile.

## Build and connect

Activate the existing ESP-IDF 5.5.x installation (validated with 5.5.4), then run from the root:

```sh
python firmware/tools/build_firmware.py waveshare-epaper-397-wifi --jobs 2
```

`profiles/products.json` selects the platform product for this profile.
On `bot.deotaland.cn` this profile uses product ID 5, product key
`esp32-s3-epaper-397`, and platform name `waveShare-ESP32-S3-ePaper-3.97`.
An absent or wrong-chip product mapping stops the build. The firmware model
remains WS-EPAPER-397.

1. Join `WavePaper397-XXXX`, open `http://192.168.4.1`, and enter 2.4 GHz WiFi
   credentials. Successful joins are saved; unsuccessful attempts keep setup open.
2. Enter the screen's six-digit activation code in the
   [platform console](https://bot.deotaland.cn) and bind a voice-capable Agent.
3. Hold center, choose an existing conversation or create a new one, then press
   center on the home page to start a call. Creation depends on the platform's
   Agent template being available; an API failure is shown instead of a fake session.

The SDK owns HTTPS device authentication, MQTT, activation and WSS realtime voice.
Conversations are accessed with device authentication, follow cursor pagination
through the SDK and retain the selection in NVS for the same bound Agent.
Rebinding clears the old selection. Network calls run outside input/display tasks.
Audio capture starts only after `session.ready`, uses the existing S3 Opus path
with negotiated PCM16 fallback, and stops on end, disconnect or a 30-second
connection timeout. A new call requests a fresh grant.

## Controls and display

| Control | Home | Menu / conversation list |
| --- | --- | --- |
| Center tap | Start/end call; opens conversations if none is selected | Confirm highlighted row |
| Center hold (1 second) | Open menu | Open menu |
| Up / down | Volume +/-5, clamped to 0-100 | Previous / next row |
| BOOT | End an active call | Return to the previous menu or home |

The UI is portrait (480 x 800), rotated 90 degrees counterclockwise relative to
the native panel image. Its top bar shows link state, volume, battery percentage
and a charging marker; when the PMIC detects no battery it shows USB. While the
WiFi setup page is active the keys are ignored and setup is completed on a phone.
The bottom shows controls for the current page. The menu
lists existing conversations, creates a conversation, and resets platform
activation with a separate confirmation. Reset retains WiFi; unbind the device in
the console before activating again. PWR keeps its independent hardware action.

A dedicated worker renders English and common Chinese glyphs, activation codes,
session names, voice state and up to 1 KB of reply text. The profile includes a
[Noto Sans SC font](../common/fonts/README.md) with all 6763 GB2312 Hanzi and common punctuation,
loaded from the existing anim_pack partition at 0x412000 with TinyTTF. Its data is
memory-mapped and a 32-glyph cache bounds bitmap memory. Unsupported characters
still use `?`; a missing/damaged image logs a warning and uses the old subset.
Both the app and font must be installed for the first upgrade; an app-only OTA
does not contain the font. Normal segmented USB flash and merged.bin include it.
Partition offsets and NVS layout are unchanged. Prefer segmented flash to retain
existing settings; merged.bin can overwrite settings between its segments.
Long replies are clipped to the available screen area. Streaming text settles
before display updates. Refresh comparisons use the status line actually drawn:
while the activation code stays the same, hidden expiry-countdown updates do not
refresh the panel. A new code or visible status/page change still refreshes it;
activation polling keeps its normal frequency. Changes notify the display worker
immediately. Updates arriving during a transfer are coalesced into the next
snapshot; there is no fixed delay after a successful refresh. Selection-only
changes reuse the framebuffer and redraw the marker; volume and battery changes
redraw only the top bar. Small changes use the partial waveform with the complete
800 x 480 native framebuffer and fixed (0, 0) origin, following the
[official menu example](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/main/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc).
Arbitrary partial-window addressing is not used. Full refresh is used when moving
between setup, connection, activation, blocked and online scenes, on application
page changes, or when the changed bounding rectangle covers at least one third
of the panel. It also establishes the initial base image, restores it after an
error, and follows at most 64 partial updates. The display stays powered between updates
to retain base RAM, increasing idle power. Partial BUSY waits have a 5-second
timeout; full refresh waits have a 20-second timeout. Failed transfers retry with
a full image after 0.5, 1, 2 and then at most 4 seconds.

Up/down act on debounced press and repeat while held. BOOT acts once on press.
Menu navigation can accumulate while its page refreshes. Confirming during refresh
freezes the current target and executes once it is displayed; BOOT cancels it.
Conversation list/create requests run in a separate HTTP worker. BOOT cancels the
wait without blocking input; a late result cannot reopen the page. An in-flight
HTTP operation still finishes before another is submitted. Creating a conversation
may still succeed on the platform after the local wait is cancelled.
Diagnostic logs record key/confirmation events, rendering and transfer duration,
window size, HTTP duration and BUSY timeouts.

The playback buffer and echo-tail guard reuse the existing voice implementation.
Microphone samples are replaced with silence during speaker playback; this is
half-duplex conversation behavior, without acoustic echo cancellation or spoken
barge-in.

The board uses the amplified ES8311 volume curve; 100% selects the codec driver's
maximum +32 dB DAC setting and reads the volume register back. This can clip or
sound distorted with loud source audio; reduce the setting if that occurs. Actual
output level also depends on the audio source, speaker and amplifier and needs
device testing.

## Acceptance

The current shared ES8311 driver uses the I2S port type from IDF 5.5. IDF 6.1
compilation is not supported for this port until that shared interface is adapted.

Host checks: `python firmware/tests/test_epaper_controls.py` and
`python firmware/tests/test_platform_api.py --idf-path PATH_TO_IDF_5_5`.
The platform fixture covers device-authenticated list/create/grant behavior
without contacting a real account.

Device acceptance remains pending: TG28 rails, portrait orientation, full/partial panel refresh and
orientation, key polarity, ES8311 microphone/speaker levels, provisioning with
wrong/correct WiFi, activation, platform binding/rebinding, conversation paging,
creation and restore, session.ready, actual two-way voice, disconnect/end/retry
and reboot persistence. A successful build does not verify these behaviors.
