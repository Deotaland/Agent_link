# Mozhifang paper V2

ESP32-S31 with a Waveshare 3.6-inch e-Paper (E), 400x600 six-color panel and
ES8389 audio. Use an ESP-IDF 6.1 installation with `esp32s31` support; 5.5 does
not support this chip. Build from `firmware/`:

```sh
python tools/build_firmware.py paper-s31-wifi --jobs 2
```

The profile reuses `build/` and produces app, merged and ELF/MAP files. Building
does not flash. Hardware and live-platform acceptance remain pending.

## WiFi setup and controls

`paper-s31-wifi` uses the SDK WiFi backend, platform product 16 and chip type
`esp32-s31`. With no saved network, join `PaperS31-XXXX` and open
`http://192.168.4.1`. Claim the six-digit code in the platform console and bind
an Agent.

- Press BOOT to start/end a call. Without a selected conversation it opens the
  conversation picker.
- While idle, hold BOOT for more than one second and release to open the picker.
  Short presses advance; a long press selects. It includes New conversation and
  Back, and pages through the bound Agent's conversations.
- Wait for the highlighted item to finish displaying before choosing again.
  Menu input is ignored while that screen update is pending.

The selected conversation is stored per Agent in NVS. A platform rebind clears
it and ends the old call. The mic uploads only after the session is ready.
Speaker playback uploads silence through the echo tail; this is not acoustic
echo cancellation and does not support spoken interruption.

The current audio-codec package has no S31 Opus library. This profile requests
PCM16: 16 kHz input and 24 kHz output, converted to 16 kHz for playback. Stereo
mic input is mixed to mono; mono output is copied to both channels. An Opus
response is rejected. The platform must accept PCM16.

## Display behavior

Rendering supports ASCII; other characters appear as `?`. The panel uses full
refreshes with visible flashing, typically about 15 seconds. Network and audio
run on separate workers.

Updates wait for three seconds of stable content and compare rendered text
before refreshing. Routine updates wait at least 180 seconds after the last
refresh completes. Activation, stable errors, online state, menus and completed
replies can update sooner, with a ten-second minimum. Reply text is submitted
after audio completion or five seconds without a text change. Failed refreshes
wait at least 60 seconds before retrying. Battery and charging changes accompany
other updates instead of forcing a refresh.

The LED blinks while offline/connecting. Online and BOOT events give short
vibration feedback. `Display refresh starting/finished` logs show actual refresh
count and duration.

## Wiring

Connect the panel to U1 (50-pin), with H1 BS0 grounded for four-wire SPI with DC.
Confirm ribbon orientation and power against the board schematic.

| Peripheral | Pins or address |
| --- | --- |
| E-paper | BUSY 7, RST 8, DC 9, CS 10, MOSI 11, SCK 12; active-low power 44 |
| Shared I2C | SDA 0, SCL 1; 100 kHz |
| ES8389 | I2C 0x10; MCLK 36, BCLK 16, WS 17, DOUT 18, DIN 37 |
| NS4150B amplifier | Active-high enable 19 |
| Buttons | BOOT 61; KEY1/2/3 13/14/15; power 6 |
| Motor / LED | 42 / 3, active high |
| Battery / RTC / IMU | 0x55 / 0x51 / 0x18 or 0x19 |
| USB serial | USB-C, GPIO33/34 |

See `board_pins.h` for the full map. GPIO36 is also a flash-voltage strap; preserve
its boot level. Camera and TF-card power remain off in this profile.

For acceptance, check panel colors/orientation, keys, motor, battery and audio.
Also check provisioning, product-16 activation, MQTT and a real PCM16
call. Host tests and builds do not cover these device checks.
