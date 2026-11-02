# Paper S31 desktop

## Optional display diagnosis

The normal profile leaves CONFIG_PAPER_S31_DISPLAY_DIAGNOSTIC disabled. Enable
it explicitly in menuconfig for a display investigation, such as an image that
remains superimposed on an older page for several minutes.
Boot displays one static, white-background test frame headed S31 DISPLAY TEST.
After that single attempt, even on error, the screen worker exits without
retrying. Menu keys, ASR text submission and application timers are disabled
for this run; existing WiFi provisioning/authentication tasks continue.

Logs tagged [display-diag] record the frame's black/white pixel counts, packed
SPI transfer time, PON/DRF/POF BUSY states and waits, and total call duration.
For DRF, observed_low=0 or after_settle=0 emits an UNVERIFIED warning. A driver
return of ESP_OK only means no software timeout; it does not prove that the
panel optically completed its erase/refresh. No corrective delays, panel
register changes or repeated clear-screen cycles are introduced in this run.

After flashing, leave all keys untouched and capture logs through the HOLD
line, plus a photo after at least one minute. If no HOLD line appears, retain
at least 90 seconds of logs to include a possible 60-second BUSY timeout.

To restore the normal desktop, disable CONFIG_PAPER_S31_DISPLAY_DIAGNOSTIC in
menuconfig and rebuild the same profile. Kconfig defaults it off; the
[S31 profile](../../profiles/paper-s31-wifi.defaults) leaves it opt-in. The sections below describe normal desktop mode.

Build the existing profile with an ESP32-S31-capable ESP-IDF 6.1 environment:

```sh
python tools/build_firmware.py paper-s31-wifi --jobs 2
```

Boot opens an application menu: Agent voice, LLM messages, message history,
daily alarm, calendar, SD music and settings. Short presses keep the menu
controls below; holds provide direct actions while the panel is refreshing.
In the alarm and time editors, up/down edit the selected field and confirmation
advances it.
An alarm rings for up to one minute; any navigation key dismisses it.
Alarms require the device to remain powered on; this is not power-off wake-up.

This profile uses partitions_paper_s31.csv with two 4 MB application slots.
The first installation of this desktop version requires USB flashing of the
partition table and application, rather than application-only OTA from the old
2 MB layout. NVS and activation settings keep their original offsets. The old
unused animation partition is smaller; its previous contents are not preserved.

The default language is Simplified Chinese. Settings switches all local UI
labels between Chinese and English and saves the choice in NVS. A flash-resident
Noto Sans SC raster font supplies Chinese without an SD font file.
The calendar uses device local time, currently UTC+8. Network time is kept by
the existing platform clock service. The PCF85063 supplies time after reboot
when its oscillator-stop flag is clear; valid system time is saved back hourly.
Settings also allows manual date/time entry.

## Display and keys

This six-color panel still needs a full refresh, commonly around 15 seconds.
The display is rotated 180 degrees. Navigation changes are coalesced after
three seconds of stable content, retaining the ten-second cooldown after a
completed refresh. Clock, battery and ordinary link status changes only update
the cached page; they accompany the next necessary or manually requested
refresh. Setup, activation codes and blocked states remain important updates.
There is no periodic full refresh just to update the clock or battery.

HTTP loading pages are held until the request finishes, unless the user
navigates or requests an update. Voice text is published after audio end, or
after five seconds of unchanged text with no queued audio. Other redraws use
the last published voice reply so they cannot expose streaming fragments.
UI input, cloud requests and playback run on separate workers.

| Control | Short press | Hold for one second |
| --- | --- | --- |
| KEY1 / GPIO13 | Up / decrease | Start or end Agent voice |
| KEY2 / GPIO14 | Down / increase | Stop voice, SD music and the alarm |
| KEY3 / GPIO15 | Confirm | Request the latest cached page |
| BOOT / GPIO61 | Back | Home |

Holds fire once without waiting for release, and do not also trigger the short
press. Direct voice/stop actions do not wait for the display. The first voice
call still needs WiFi, an Agent and a selected voice conversation; when none is
selected, the voice shortcut opens the conversation list. Manual update
retains the refresh cooldown and skips an identical page. Short navigation and
confirmation give brief vibration feedback.

A confirmation pressed before its selected row appears is queued once, then
executed after that view is displayed. Navigation, a changed list/page, an
Agent change or a shortcut cancels the pending confirmation. A header-only
clock/battery update does not change the selected view.

GPIO3 (`TOP_LED_EN_GPIO`, FPC pin 8) drives the sub-board LED; it is an output,
not a fourth key. KEY1-3 are FPC pins 9-11. The separate SW4 power key is sensed
on GPIO6; the vendor test connects this signal to the SAM8108 power circuit and
notes that holding it for about three seconds turns power off. The desktop
does not emulate a power switch on KEY1-3 or BOOT. Physical power-button behavior
with battery/USB power still needs device acceptance.

If the boot display probe fails, the screen worker makes at most three more
probe attempts, at least five seconds apart. A successful probe resumes normal
refreshes with the latest queued page. After all three fail, check the panel
and restart. `screen0` remains registered during recovery and accepts text for
the desktop message page even outside a voice call. Callbacks copy/coalesce
text; the desktop worker changes the page. During a call, replies continue to
use the voice page. Acceptance means queued, not physically displayed.

## WiFi and BLE

Settings shows WiFi SSID, IP and RSSI. Confirming on that page reopens the
existing phone hotspot setup flow: join PaperS31-XXXX and open
http://192.168.4.1. Changing WiFi preserves the platform binding.

The BLE toggle starts/stops a NimBLE peripheral discovery service named
PaperS31 and disconnects its peer when disabled. The host remains initialized
after first use. It is not a classic Bluetooth music sink and does not implement
a phone-app BLE WiFi credential protocol. Phone hotspot provisioning remains
the supported setup method.

## Music

Use a FAT-formatted SD card and put files in /music. The firmware never formats
a card. It lists up to 64 files with .wav or .WAV extension. Supported audio is
RIFF WAV, uncompressed signed PCM16, one or two channels, 8-48 kHz. Stereo is
mixed to mono and converted to the existing 16 kHz speaker path. MP3, AAC, FLAC
and Bluetooth audio are not implemented. Selecting a track stops voice;
starting voice stops SD music. Playback is streamed with bounded buffers.
The 6 KB PCM conversion buffers are allocated on first playback, preferring
PSRAM and falling back to internal RAM. They are reused by the music worker;
its 6 KB task stack remains in internal RAM for SD I/O. Allocation or task
creation failure is reported without starting playback or retaining a failed
allocation.
A missing card, missing directory or unsupported WAV is reported by the UI.

## LLM chat and local tools

The first version sends preset messages; ASR is deliberately not included.
[paper_chat_input.h](paper_chat_input.h) exposes paper_s31_send_text() for a
future transcriber. It copies the UTF-8 text to the desktop worker queue.
The SDK's [agent_link_chat.h](../../components/agent_link/include/agent_link_chat.h)
provides the HTTPS send, conversations and message-history interfaces.

Calls use /api/agents/{agent_id}/actions/llm/ and the activated device key.
The SSE decoder assembles complete replies before JSON tool validation.
Messages and conversations use cursor pagination. The selected conversation
is saved per bound Agent. History is fetched from the platform; no pretend
persistent local transcript is substituted for a failed request.

Before the first message, and on explicit request in Settings, the desktop
GETs the bound Agent and PUTs only its prompt field. A marked device block is
replaced while preserving the rest of the user's prompt. The block contains
the actual registered I/O manifest and the following local reply contract:

```json
{
  "text": "Human-readable reply",
  "tools": [
    {"name": "motor0", "arguments": {"value": 300}}
  ]
}
```

Use an empty tools array for a normal reply. Supported local tools:

| Name | Arguments |
| --- | --- |
| led0 | value: "#RRGGBB"; zero means off and any other color means on |
| motor0 | value: integer 0-2000 milliseconds |
| screen0 | value: UTF-8 text, at most 2048 bytes |
| alarm0 | hour: 0-23, minute: 0-59, enabled: boolean |
| music0 | action: "list", "stop" or "play"; play requires a listed file name |

A whole batch, up to eight calls, is validated before execution. Unknown tools,
invalid types, traversal paths, trailing prose and incomplete SSE replies do
not execute any tools. Local results are returned as a subsequent user message
containing local_tool_results, for at most three tool rounds per user request.
"accepted" means dispatched or queued; it does not certify physical completion.
Replies from an abandoned request or previous Agent cannot execute hardware.
The S31 profile disables MQTT: this tool path uses HTTPS replies and local
dispatch only. Realtime voice retains the HTTP-granted WebSocket path.

If Agent detail/update rejects the device key, prompt sync reports a permission
error and chat can still return ordinary text. The optional
CONFIG_AGENT_LINK_PROMPT_USER_TOKEN may be set in ignored sdkconfig.private
for those two management requests only. It is sent as X-Token; conversation
and voice calls continue to use X-Device-Auth-Key. Do not store account tokens
in public profile defaults. Firmware containing private credentials stays local.

## Validation

Host checks exercise production SSE parsing, tool validation, malformed WAVs,
calendar boundaries, authenticated chat/history and preservation of user prompts:

```sh
python tests/test_platform_api.py --idf-path PATH_TO_IDF --pcm --chat
python tests/test_paper_refresh.py
python tests/test_paper_runtime.py
```

Build success is not device acceptance. Verify SD pin wiring/mounting, audio
rates, RTC retention, key direction, Chinese rendering, BLE toggling and WiFi
reconfiguration on the actual board. Check platform permission for device-key
LLM/history and prompt updates, then test a real JSON tool round trip.
Also test WAV playback/cancellation, transient and persistent boot probe
failures, and `screen0` text before and after a voice call. The runtime host
check injects allocation/task failures and simulates these screen paths; it
cannot verify panel output or sound.
