# Deotaland Agent Link

A connectivity layer that puts embedded devices on the **Deotaland Agent platform**. A device declares which capabilities it has (microphone, speaker, screen, sensors, and so on) and registers a few callbacks; the library handles BLE advertising, GATT, and frame encoding. There is no Bluetooth or wire-protocol code to write on the device side.

## Repository layout

| Path | What it is |
|---|---|
| [`firmware/`](firmware/) | The ESP-IDF project: the `agent_link` SDK component, the shared app, and reference boards. Start here — see [`firmware/README.md`](firmware/README.md) ([简体中文](firmware/README.zh-CN.md)). |
| [`docs/`](docs/) | Protocol reference: [BLE](docs/agent_link_ble.md) (frames, GATT, L2CAP, every command and event), [device I/O](docs/device-io.md) (self-describing sensors and actuators), [OTA](docs/agent_link_ota.md), and the [MCP gateway](docs/mcp-gateway.md) design. |
| `app/` | Phone app (not yet published). |

## Quick start

```bash
git clone https://github.com/Deotaland/Agent_link.git
cd Agent_link/firmware
python tools/build_firmware.py   # choose the firmware profile from the menu
idf.py -p <port> flash monitor
```

Requires an activated ESP-IDF v5.5.x environment (maintainers use v5.5.4). The build tool generates the configuration and restores managed dependencies; a fresh checkout needs no local config or test files. Full build notes, the capability model, and the board-porting guide are in [`firmware/README.md`](firmware/README.md).

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 DEOTALAND LIMITED (德奧塔文化科技有限公司).
