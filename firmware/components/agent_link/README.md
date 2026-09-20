# agent_link

The connectivity component behind the [agent_link project](../../README.md): it brings an embedded device onto the **Deotaland Agent platform** without any Bluetooth or protocol code on the device side. A device declares its capabilities and registers a few callbacks; the component handles transport, framing, and capability routing.

- **Agent to device**: the `agent_output_cb_t` callbacks drive output and actuation (speak, show, vibrate, LED, generic actuators).
- **Device to Agent**: the `agent_link_push_*` calls report input, events, and state (voice, buttons, sensor readings, battery).
- The component does transport, protocol, and capability routing only. It never touches hardware directly, so it ports across boards unchanged.

## Layout

```
agent_link/
├── CMakeLists.txt
├── idf_component.yml
├── include/
│   ├── agent_link.h            lifecycle, config, capabilities, callbacks, push API
│   ├── agent_link_caps.h       capability bits, input-event types, connection state
│   ├── agent_link_io.h         self-describing sensors and actuators (register_io / push_reading)
│   ├── agent_link_ota.h        App-driven firmware upgrade (optional progress hook)
│   └── agent_link_transport.h  transport backend interface (BLE now; WiFi scaffolded)
└── src/
    ├── agent_link.cpp          core: config, capability routing, lifecycle
    ├── adpcm.h                 IMA-ADPCM decoder for the audio downlink (ported from production)
    ├── audio_downlink.cpp      downlink codec negotiation, ADPCM decode, 0x20 flow control
    ├── ota_service.cpp         OTA engine: flash write, SHA-256, boot switch, rollback confirm
    ├── protocol.cpp            control-plane frame encode/decode
    ├── transport_ble.cpp       BLE backend (NimBLE: GAP, GATT, L2CAP CoC)
    └── transport_wifi.cpp      WiFi backend (interface stub)
```

## Using it

The component is standalone. Copy `agent_link/` into your project's `components/`, then add it to your app's requirements:

```cmake
idf_component_register(... REQUIRES agent_link ...)
```

Minimal integration:

```c
#include "agent_link.h"

static void my_play(const uint8_t* pcm, size_t n, void* ctx) { /* write to codec */ }
static void my_draw(const char* utf8, void* ctx)            { /* render to screen */ }
static void my_buzz(uint32_t ms, void* ctx)                 { /* drive the motor */ }

void app_start_agent(void) {
    static agent_output_cb_t out = {
        .on_audio_out = my_play, .on_show_text = my_draw, .on_haptic = my_buzz,
    };
    agent_link_config_t cfg = {
        .device_name = "MyThing",
        .caps = AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_SCREEN | AGENT_CAP_HAPTIC,
        .output = &out,
    };
    agent_link_init(&cfg);
    agent_link_start();
    // then: microphone -> agent_link_push_voice(pcm, n); sensors -> agent_link_push_reading(...)
}
```

Leave the callbacks you do not support as `NULL`, and do not set their capability bits.

**Firmware upgrades come with the component.** The App pushes a new image over the link and the
SDK does the rest — command handling, flash writes, SHA-256 verification, boot switch, reboot,
and cancelling the bootloader's rollback once the new image proves it can still reach the App. A
board writes no OTA code; it only sets `cfg.model` (the string an incoming image must claim, so a
build for other hardware cannot be flashed onto it) and, if it wants to draw a progress screen,
registers one callback:

```c
#include "agent_link_ota.h"
static void on_ota(const agent_ota_status_t* st, void* ctx) { /* store it; do not block */ }
agent_link_ota_set_callback(on_ota, NULL);
```

## API at a glance

- Lifecycle: `agent_link_init`, `agent_link_start`, `agent_link_stop`, `agent_link_state`.
- Uplink: `agent_link_push_voice` / `agent_link_voice_end`, `agent_link_asr_start` / `agent_link_asr_push` / `agent_link_asr_end` (real-time ASR audio → App, BLE L2CAP), `agent_link_push_event` (device→App events; `AGENT_EVT_CUSTOM` for board-private packets), `agent_link_push_prompt` (firmware-authored text → App forwards verbatim to the Agent as a prompt, event `0x04`) `agent_link_report_battery`, `agent_link_register_io` / `agent_link_push_reading`. Also declared but not yet wired: `agent_link_report_selected_agent`, `agent_link_push_video`.
- Downlink: the `agent_output_cb_t` callbacks, invoked from the transport context. Keep them short and non-blocking; queue work and return.
- Downlink audio: `agent_link_audio_set_buffer_ms` — declare your play buffer so the SDK can throttle the App (event `0x20`) before it overruns you. The board always receives PCM16, whether the App sent raw PCM or IMA-ADPCM.
- OTA: `agent_link_ota_set_callback`, `agent_link_ota_get_status`, `agent_link_ota_is_busy`, `agent_link_ota_abort`, `agent_link_ota_mark_valid`, `agent_link_ota_running_version`. Tunables under menuconfig → **agent_link SDK**.

Requires ESP-IDF v5.0 or newer, and an OTA-capable partition table (`ota_0`/`ota_1` + `otadata`)
if OTA is left enabled. Private dependencies: `log`, `bt`, `nvs_flash`, `freertos`, `app_update`,
`esp_partition`, `esp_app_format`, `mbedtls`.

## Design notes

- **Capabilities.** The device declares its hardware as a `caps` bitmask and implements only the matching callbacks. Sensors and actuators are described in a manifest sent to the Agent on connect ,so new hardware integrates without platform-side changes.
- **Transport abstraction.** The core depends only on `agent_transport_t`. BLE is implemented; a WiFi or USB backend can be added without touching the device-facing API.
- **Escape hatch.** `on_custom` forwards any command outside the standard protocol verbatim, so a device can extend the protocol without changing the common layer.

## Status

Control plane (commands, responses, events), voice uplink over GATT Notify, voice downlink over L2CAP, the device-I/O path (`register_io` / `push_reading` / actuator downlink), and OTA firmware upgrade over BLE are implemented; they have not yet been validated on hardware. Recording/file transfer, video, and the WiFi/USB backends are not implemented. The [project README](../../README.md#status) has the full matrix and the on-wire protocol.
