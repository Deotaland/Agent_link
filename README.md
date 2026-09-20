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
idf.py set-target esp32s3     # match your board's chip
idf.py menuconfig             # Agent Link Device -> Board Type
idf.py build
idf.py -p <port> flash monitor
```

Requires ESP-IDF v5.0 or newer. Full build notes, the capability model, and the board-porting guide are in [`firmware/README.md`](firmware/README.md).

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 DEOTALAND LIMITED (德奧塔文化科技有限公司).
