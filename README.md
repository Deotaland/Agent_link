# Agent Link

Connect ESP32 devices to the Deotaland Physical Agent platform over BLE or WiFi. The SDK
handles connectivity, device I/O, audio streams and OTA; applications provide the
hardware and user interface. Existing Muse firmware is also included.

[Chinese README](README.zh-CN.md) · [Firmware guide](firmware/README.md) ·
[Add a board](firmware/boards/README.md) · [SDK](firmware/components/agent_link/README.md)

## Flash with an AI assistant

Open this repository in your coding assistant and tell it what you have:

> I have an M5Stack StopWatch. Flash Agent Link firmware onto it.

[AGENTS.md](AGENTS.md) tells the assistant how to identify the board, choose a
firmware profile, build or locate an image, find the USB port and check the boot
log. It asks about the connection only when the hardware supports several
firmware choices. You do not need to know the chip or configuration symbols.

Connect a USB data cable. BLE firmware needs the Deotaland phone app; WiFi
firmware uses hotspot setup and a six-digit activation code in the
[platform console](https://bot.deotaland.cn). Prebuilt images are available in the
[firmware center](https://bot.deotaland.cn/#/firmware).

## Build from source

Activate ESP-IDF 5.5.x or 6.1.x, then run:

```sh
git clone https://github.com/Deotaland/Agent_link.git
cd Agent_link/firmware
python tools/build_firmware.py
idf.py -p PORT flash monitor
```

Choose a profile from the menu. The tool reuses `firmware/build`, runs two
compiler jobs and creates app, merged and debugging files. ESP32-S31 profiles
require ESP-IDF 6.1 with S31 support. See the [firmware guide](firmware/README.md)
for configuration and flashing details.

## Add hardware

Start with the [board guide](firmware/boards/README.md), or ask your assistant:

> Add my ESP32-S3 board using the WiFi backend. Here are its schematic and pins.

Keep wiring and drivers in `firmware/boards/`, application behavior in
`firmware/apps/`, and select the combination in `firmware/profiles/`. BLE and
WiFi use the same SDK callbacks and device I/O model.

## Repository

| Path | Contents |
| --- | --- |
| [firmware/](firmware/README.md) | ESP-IDF project, SDK, hardware and applications |
| [skills/](skills/README.md) | AI workflows for flashing, building, porting and diagnosis |
| `tools/` | Board catalog, documentation checks and skill installer |
| [app/](app/readme.md) | Phone app placeholder |

MIT; see [LICENSE](LICENSE). Vendored code and fonts retain their own licenses.
