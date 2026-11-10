# Paper 4IN0 desktop

A separate `Application` and monochrome interface for `paper-s31-4in0-wifi`.
It does not select the six-color desktop. Screen layout, navigation, scheduling,
settings and session ownership are implemented in this directory. Audio playback
and SD WAV decoding are shared through `apps/common/paper-services`.
The offline input engine is `components/offline_ime`. The shared font and pinyin
data are loaded from `font_cjk` by `components/paper_resources`; they are excluded
from this app image. See the [board layout and migration notes](../../boards/paper-s31-4in0/README.md).

| Key | Short press | Hold for one second |
| --- | --- | --- |
| KEY3 | Up / previous month or message page | No additional action |
| KEY2 | Down / next month or message page | No additional action |
| KEY1 | Confirm | Back / leave field editing |
| BOOT | No application action | No application action |

Releasing KEY1 after its hold does not also confirm an item. Keys held at boot
must first be released. A confirmation waits until its selected item has appeared
on the screen. Navigation or an asynchronous page change cancels an old pending
confirmation. While a call is active, KEY1 immediately ends it without waiting
for screen refresh. During an alarm, any of KEY1/2/3 stops the reminder.

## Pages

- **Voice:** select/create a voice conversation and start/end a call with the
  platform-bound Agent. The microphone starts only after `session.ready`.
  Audio uses PCM16 because the S31 codec package has no Opus implementation.
  Speaker playback mutes the voice uplink through its echo tail; spoken
  interruption is not supported. The display shows the latest stable reply.
- **Text:** compose with the offline touch keyboard or send a preset message, select/create a text conversation, view message
  history and read replies with up/down pagination. Text and voice conversations
  have separate saved selections. Agent selection/binding occurs in the platform
  console. This app does not overwrite the Agent's prompt or parse replies as
  local JSON commands; SDK screen, LED and haptic callbacks remain available.
- **Conversations:** paginated text conversations; voice conversations are chosen
  from Voice. Requests run on an HTTP worker. Leaving the request's page or
  changing the bound Agent discards its result. A canceled HTTP request must finish
  before another one starts.
- **Daily alarm:** select hour/minute and confirm to edit; up/down change the value,
  confirm finishes editing. Toggle enabled and choose Save. Sound and vibration
  run for at most 60 seconds while the powered-on device is running. This is not
  an RTC wake-from-power-off alarm.
- **Calendar:** up/down switch months, confirm opens date/time settings.
- **SD music:** place PCM16 mono/stereo WAV files (8-48 kHz) under `/music` on an
  existing FAT SD card. Scan, select a track or stop playback. The app does not
  format the card. Music and Agent voice share the speaker and stop each other
  before taking ownership.
- **Settings:** WiFi provisioning, date/time (system and PCF85063), SD scan,
  manual screen refresh, stop audio and board information. Local civil time is
  UTC+8. App settings use the separate `paper4_wifi` NVS namespace.

## Touch and offline text input

FPC2 touch shares I2C SDA0/SCL1 and the panel's always-on 3.3 V rail, with
RST4/INT5. The driver follows the supplied board-test sequence and detects
GT911 (0x5D/0x14), FT6x36 (0x38), or CST816 (0x15). The exact installed controller
and optical/touch behavior still need device acceptance. A missing touch panel
leaves physical navigation and preset messages available.

GT911 touch works immediately with the board's default mapping: reverse both
raw axes and scale the reported 320x480 range to the rotated 528x792 screen.
The raw origin is at the physical bottom-right. Invalid reported dimensions fall
back to the board's 320x480 defaults. Boot and Compose do not require calibration.

Optional four-cross calibration is available with the physical keys under
Settings > Touch calibration. It fits swapped or mirrored axes and saves a
version-2 transform with the sensor dimensions in NVS. Older transforms or a
different sensor range fall back to the board mapping. Long-hold KEY1 cancels.
Tap and release each cross once, waiting for the next cross before continuing.
Duplicate points keep the current cross active; other fit failures return to
Settings and preserve the previous usable mapping. Explicit calibration logs
the four raw/target points and a failure reason to diagnose bad samples.

Open Text > Compose. Switch among **nine-key pinyin**, **full pinyin** (26 Latin
letters plus space), **English**, and **symbols**. Tap a candidate to commit it;
arrows page through candidates. `v` represents umlaut-u in full pinyin. You can
type successive syllables and select them in order; a small common phrase list
also provides complete-word candidates. This is a character-candidate input
method rather than a learned whole-sentence predictor. See the
[dictionary notice](ime-data/NOTICE.md) for the 6,763-character coverage, source
hashes and MIT licenses. Both pinyin layouts convert locally and need no network.
Handwriting is not included.

The draft supports 1,024 UTF-8 bytes. Backspace removes one whole character;
Clear removes the draft and pending compositions. Chinese keyboard modes retain
their own uncommitted composition across mode switches. Literal commits the
active pinyin/digits as typed. Space commits the first candidate when composing,
otherwise it inserts a space. Drafts survive navigation in RAM, not power loss.

Send first opens a review page. Only **Confirm send** invokes the existing Agent
Link message worker; typing, changing layouts and choosing candidates never
send a message. Sending requires WiFi and the bound Agent. Failed requests retain
the draft; a successful matching request clears it. Review can be paged with the
touch arrows or KEY3/KEY2. The original shortcuts and preset messages remain.

Touch taps use the keyboard layout actually shown on the panel. Keys on an
unchanged layout continue to accumulate while the display worker is busy, but
stale candidate taps and stale send confirmations are rejected. Touch sampling
does not redraw the screen on every sample or draw handwriting trails. Existing
pixel-difference/partial refresh policy remains active; the panel waveform still
limits visual response speed. Hardware acceptance must check fast typing,
candidate selection during refresh, default corner mapping after the 180-degree
flip, and optional calibration/save/failure recovery.

Provisioning, activation and blocked-link pages take precedence over the menu;
hidden menu choices cannot be confirmed. A stable activation code uses fixed
instructions: changing expiry hints from bind-status polling do not redraw it.
The network keeps polling normally. A new code or actual link-state change is
still displayed. Time/battery updates are sampled together at most once a minute
and held while setup/activation/blocked pages are visible.

The display worker compares the final pixels, including clipping and the visible
slice of long menus. Identical pixels send no panel commands. Small changes use
the 4IN0 driver's aligned partial window at the current 270-degree orientation,
including both interleaved gate strips. Full refresh is reserved for the first
image, an actual controller window covering at least one third of the panel,
cleanup after 64 successful partials, manual refresh or recovery from failure.
Changing pages does not itself force a full refresh.

The worker retains the last successful framebuffer and probes only at startup
or recovery. Failed transfers invalidate the base and retry with a full image
after 60 seconds. Changes coalesce for 300 ms with a one-second gap after a
completed update; key, network and audio workers remain independent. The current
panel OTP waveform can still flash inside a partial window. No unverified fast
waveform or Waveshare-specific controller register sequence is used.

## Build and verify

```sh
python firmware/tools/build_firmware.py paper-s31-4in0-wifi --jobs 2
python firmware/tests/test_paper_refresh.py --paper4
python firmware/tests/test_paper4_display.py
python firmware/tools/generate_paper_resources.py --check
```

Before device acceptance, verify panel direction and all columns, three-key
navigation/back/editing, saved settings after restart, RTC/time, one daily alarm,
SD files and audio stop, WiFi setup, product-14 activation, Agent rebinding,
conversation selection/history and a real PCM16 call. Host checks and compilation
do not establish these hardware/platform results.
