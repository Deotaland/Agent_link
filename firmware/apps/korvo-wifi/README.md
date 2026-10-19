# Korvo WiFi

Profile: `korvo-wifi`. Shared hardware is in `boards/korvo/`; this application
provides camera preview, platform conversations, realtime voice and servo control.
Set up WiFi through `KorvoCloud-XXXX`, claim the activation code in the console
and bind an Agent there. See [realtime voice](../../components/agent_link/REALTIME.md)
for authentication, audio and validation status.

## Servo control

The HC32L110 eight-channel module uses address `0x10` on the dedicated I2C bus:
SDA GPIO19, SCL GPIO20, 100 kHz. Keep the existing Korvo wiring; the Arduino
reference's GPIO8/9 are used for audio on Korvo and must not be copied.
This application targets the updated servo board firmware with four-byte commands
and 28-byte status responses. Old three-byte-only module firmware does not expose
the new modes or status format.

## On the device

Open **Apps → Servo**. Hold the SS button and slide to select SS1–SS8 or ALL.
Tap the mode button to cycle through 180-degree positioning, 360-degree positioning
and continuous rotation speed. Selecting a mode alone sends no movement command;
turn the dial to apply it. Choose the mode matching the attached servo.

**Stop** sends continuous speed zero (neutral pulses). **Release** disables pulses.
**Read** fetches the controller's modes and values for all eight channels; opening
the page also reads them without moving anything. Released channels enable the
dial again after selecting a positioning or speed mode.

ALL broadcasts the chosen mode and value to every channel. Use it only when that
mode is suitable for every connected servo. The displayed values are controller
settings, not measured positions. Failed writes and failed status reads are shown
separately; a successful write does not prove mechanical motion completed.

## Platform endpoints

The existing `servo1` integer endpoint remains a 0–180 degree command for SS1.
New endpoints use the SDK's existing device-I/O channel:

| ID | Direction | Arguments / result |
| --- | --- | --- |
| `servo_control` | Output | `channel`, `mode`, `value`: three signed 32-bit integers |
| `servo_refresh` | Output | Boolean `value: true` requests a controller read |
| `servo_status` | Input | UTF-8 JSON string with write/read results and controller state |

`channel` is 1–8, or 255 for all. Modes and ranges:

| Mode | Meaning | Value |
| --- | --- | --- |
| 0 | Release pulses | 0 |
| 1 | 180-degree positioning | 0–180 |
| 2 | 360-degree positioning | 0–360 |
| 3 | Continuous rotation | -100–100; 0 stops |

Example `io_actuate` parameters:

```json
{"io":"servo_control","args":{"channel":2,"mode":2,"value":300}}
```

For speed -40 on SS3 use `channel: 3, mode: 3, value: -40`; stop with value 0.
Release all outputs with `channel: 255, mode: 0, value: 0`.

Status is read and reported after each accepted command and explicit refresh.
`write_error` and `read_error` are ESP-IDF error numbers (zero means success).
On successful read the JSON also contains `last_id` (wire ID 0–7 or 255),
`last_mode`, `last_value`, and `channels` with eight `{channel,mode,value}` entries
using human channel numbers 1–8. On read failure those state fields are omitted.

The application queues commands in order; SDK and UI callbacks do not perform I2C.
An unavailable/full queue is reported locally or in the device log.

## Validation

From the repository root run `python firmware/tests/test_servo8.py` for host tests
of the production driver's byte frames, bounds, signed status decoding and I2C
failure handling. Build only the selected profile with
`python firmware/tools/build_firmware.py korvo-wifi --jobs 2` in ESP-IDF 5.5.4.
Hardware testing still needs the updated module firmware and suitable servos.
