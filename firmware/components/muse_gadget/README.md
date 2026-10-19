# Muse gadget integration

Vendored from [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk),
revision `693cde9a884ad1edc87251b9f8944815f8de4809`. This component runs Meta's
Home Link under the Agent Link Muse transport. It and `noise_core` register empty
on other profiles. Only the RoRoLee/WOWNNY Muse profiles are offered.

## Build and setup

Build a Muse profile with `tools/build_firmware.py` from `firmware/`. Put
`CONFIG_GADGET_SDK_TOKEN` in ignored `sdkconfig.private`; an empty token produces
a warning and a malformed token stops the build. Optional
`CONFIG_MUSE_TTS_MINIMAX_KEY` enables spoken replies. Both values are embedded in
the image; keep the config and image private.

In the Muse app, enable developer mode, add `MuseGadget-XXXXXX` and press BOOT
when the device requests confirmation. The app sends WiFi credentials and device
tokens over the encrypted BLE session. The device restarts with BLE off and
connects to Muse over WiFi. The Agent Link hotspot portal is not used.

Hold BOOT to record a voice note, release to send. Replies arrive as text.
MiniMax TTS turns new reply text into MP3, decodes it to 16 kHz mono PCM and
routes it through normal SDK audio callbacks. Without a key, replies stay text.
The device needs network access to Muse services; a proxy on the PC alone does
not provide that access to the device.

## Source layout

| Path | Purpose |
| --- | --- |
| `link/` | Home Link and product-info sources; upstream copyright headers retained |
| `port/muse_gadget.c` | Hosted app task and public API adapter |
| `port/led_status_gadget.c` | Link status adapter |
| `port/muse_note.c` | Voice-note upload and reply subscription |
| `port/muse_tts.c`, `port/muse_tts_decode.c` | MiniMax requests and streaming MP3 decoding |
| `../noise_core/` | Noise handshake and HTTP-over-Noise transport |
| `../agent_link/src/transports/muse/` | Agent Link transport adapter |

## Port constraints

Profile fragments select PSRAM placement for LVGL, BLE pools and network state,
plus a 16 KB TCP send buffer. DMA buffers and task stacks still need internal
RAM. The adapter buffers up to eight seconds of microphone input in PSRAM.
Memory settings come from profiles rather than CMake edits to `sdkconfig`.

Local upstream changes include:

- Hosted mode delegates buttons to the application, exposes confirmation/reset
  APIs, restarts after pairing and retries the VM connection.
- Noise request APIs reserve a 12 KB stack; the hosted app also uses a 12 KB stack.
- IDF 5.5 compatibility covers GCM headers, optional ECDSA/NAPT paths and empty
  AES-GCM payloads through the PSA multipart API.
- Unused upstream UI, board, LED and standalone main files are omitted.

Muse OTA, the home-network tunnel and support-report upload are disabled.
When updating upstream, retain these adapters and compatibility changes, build
the selected Muse profile and verify pairing and a voice note on hardware.
