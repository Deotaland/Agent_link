# muse_gadget

Meta's **Muse Home Link** (the ESP32 firmware of [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk))
run as a library under agent_link's **Muse transport** (`menuconfig → Agent Link Device → Transport backend → Muse`).
With it selected, any agent_link board becomes a Muse gadget: it pairs with the Muse app, joins the
Wi-Fi the app hands it, keeps an encrypted session to the user's Muse, and turns the board's voice
stream into voice notes whose answers come back through `on_show_text`, and with a MiniMax key also
as speech through `on_audio_out`.

On BLE and WiFi builds this component (and `../noise_core`) registers empty: nothing is compiled or
linked.

## Layout

| Path | What |
| --- | --- |
| `link/` | Home Link, vendored from `esp32/main` (plus `customer_product_info.*` from `esp32/components/product_info`). Apache-2.0, Meta's copyright headers kept. |
| `port/muse_gadget.c` | Runs `app_run()` on its own task; the API in `include/muse_gadget.h`. |
| `port/led_status_gadget.c` | Implements Home Link's `led_status.h`: status-light states become `muse_gadget_state_t`. |
| `port/muse_note.c` | Voice notes, ported from `esp32/components/muse/muse_chat_link.c`. |
| `port/muse_tts.c` | TTS: MiniMax T2A v2 over HTTPS, a small queue, playback pacing. |
| `port/muse_tts_decode.c` | Pulls the hex MP3 out of MiniMax's JSON and decodes it (`../minimp3`) to 16 kHz mono. No ESP-IDF, so it can be tested on a PC. |
| `Kconfig` | The upstream `GADGET_*` / `HOMEHUB_*` options the build uses; the rest stay undefined (off). |
| `../noise_core/` | `esp32/components/noise_core`, vendored as-is (Noise XX + HTTP-over-Noise framing). |
| `../agent_link/src/transport_muse.cpp` | The agent_link backend on top of this component. |

Upstream revision: `693cde9a884ad1edc87251b9f8944815f8de4809` (2026-10-03).

## How a Muse gadget works

1. **Setup (BLE).** Unpaired, it advertises as `MuseGadget-XXXXXX`. In the Muse app: Settings → Devices →
   Developer mode, then Add Device. The app runs community pairing v5 (P-256 ECDH, HKDF-SHA256,
   AES-256-GCM); the device then asks for a press on itself (`confirm_required`), which the board
   forwards with `agent_link_confirm()`. Inside the encrypted session the app scans Wi-Fi through the
   device and sends `provision_v2`: the Wi-Fi credentials, a device token pair, and the API and Noise
   hosts. The device joins the Wi-Fi, stores everything, answers `auth_ok` and restarts.
2. **Online (Wi-Fi).** `GET https://api.muse.ai/fetch_vms` with the device token gives the user's VM and a
   per-VM bearer; `wss://hatch.metaaivm.com/v1/noise?vm_id=…` with that bearer carries a
   Noise_XX_25519_AESGCM_SHA256 session; on it, `POST /link-control` registers the device
   (`link.register`) and receives `link.invoke` commands.
3. **Talking.** A voice note is `POST /chat/stream` with a base64 WAV (16 kHz mono) streamed while the
   user speaks; the answer arrives on `POST /chat/subscribe` as NDJSON (`message.user` = what Muse
   heard, `delta.text_append` / `message.assistant` = the answer). Answers are text: Muse sends no audio.
   The note needs about 43 KB/s up. Upstream drops it once the uplink takes nothing for 200 ms (its
   boards have no PSRAM to hold the speech meanwhile); here up to 8 s of speech waits in PSRAM
   (`transport_muse.cpp`) and the note carries on, and the Muse boards get a 16 KB TCP send buffer as
   Meta's firmware has (repo-root `CMakeLists.txt`): with lwIP's default 5.7 KB one lost segment holds
   the upload until the retransmission timer fires. A hardware log on 2026-10-04 lost a note that way
   0.9 s into it.

Wi-Fi comes from the Muse app in the same BLE session; agent_link's own SoftAP portal is not used on
this transport. Setting it up any other way would still need the BLE pairing, because that is the only
way the device gets its Muse token.

## Spoken answers

The Muse only replies in text, so TTS runs on the device, the same way as in the muse-gadget-sdk
MiniMax port and the rorolee app (`CONFIG_MUSE_TTS_MINIMAX`, on by default, does nothing without a
key):

1. `MUSE_NOTE_EV_REPLY` carries the whole reply so far. The new part is queued in `port/muse_tts.c`
   and posted to `https://api.minimaxi.com/v1/t2a_v2` (model and voice from Kconfig) as MP3, 16 kHz
   mono, 32 kbps.
2. The response JSON has the MP3 hex-encoded in `data.audio`. `port/muse_tts_decode.c` decodes it
   while it downloads, so playback starts before the whole response is in.
3. The PCM goes to the board through `on_audio_out` / `on_audio_end`, like spoken replies on the
   other transports, at most 3/4 of the board's play buffer (`agent_link_playback_set_buffer_ms`,
   max 2 s) ahead of playback. The text goes to the board when its audio starts, or right away if
   TTS returned nothing.
4. A new voice note stops the speech; the Muse boards also flush what they still have buffered.

The decoder was tested on a PC against ffmpeg, with the body fed in random-sized chunks: correlation
1.0000 for 16 kHz mono and stereo, 0.986 for 24 kHz (resampled). An error response or
`"audio": null` gives no audio, and the start of the body is logged.

## Building

Set the SDK token from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens) under
`Component config → Muse gadget → Muse Gadgets SDK token` (`CONFIG_GADGET_SDK_TOKEN`). It ships inside
the firmware: treat it as an identifier, never commit it. The build warns when it is empty and stops
when it is malformed.

For TTS set `Component config → Muse gadget → MiniMax API key` (`CONFIG_MUSE_TTS_MINIMAX_KEY`, from
platform.minimaxi.com). Same rules as the token: it ends up in sdkconfig and the image, so keep it
out of git and sdkconfig.defaults. Without a key the build prints a note and replies stay text.
The boot log shows `agent_link.muse: TTS on (MiniMax)`; each piece logs
`muse_tts: speaking, X s after asking` and `spoke X s (N bytes of MP3 at 16000 Hz)`, and a failed
request logs `no speech: HTTP <status>, <start of the response>`.

## RAM

While it pairs, a Muse gadget runs BLE, Wi-Fi and TLS at once, which is more internal RAM than a
board with a display and audio has left. On rorolee-muse the first hardware log showed 0 KB internal
free as soon as BLE started, and the Wi-Fi scan task could not even be created. That board now keeps
LVGL's heap, NimBLE's pools and the `.bss` of Wi-Fi, lwIP and BT in PSRAM (repo-root
`CMakeLists.txt` and `main/CMakeLists.txt`), freeing about 78 KB of static internal RAM; wownny-muse
is on the same lists. Another board put on this transport needs the same budget. After pairing the
device restarts with BLE off.

## Local changes to `link/`

All of them build on ESP-IDF 5.5 (upstream requires 6.0.1) or let agent_link host Home Link:

- `MUSE_GADGET_HOSTED=1` (set by `CMakeLists.txt`) widens these upstream `CONFIG_MUSE_ENABLED` cases:
  - `app.c`: no GPIO button of its own (the board owns its buttons); `app_confirm_pairing_press()` and
    `app_reset_setup_async()` are built (also declared in `app.h`); `PAIR_THEN_RESTART` — pairing ends
    with a restart instead of opening the VM session next to BLE, as on upstream's UI boards;
    `hosted_keep_vm_session()`, the hosted copy of the UI builds' `muse_keep_vm_session()`: when
    the VM could not be reached at boot, retry every 5 s doubling to 60 s instead of staying red
    until a power cycle.
  - `noise_control.cpp`: the extra-request API (`noise_ctrl_req_*`) the voice notes use, and the 12 KB
    session stack reserved from boot.
  - `noise_tunnel.cpp`: `noise_tx_has_dma_headroom_reclaiming()`, which that request queue needs.
- `link_pairing.c`: `mbedtls/gcm.h` on IDF < 6 (it moved to `mbedtls/private/` in mbedTLS 4); the ESP
  ECDSA PSA driver headers only with `CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH` (manufacturer attestation,
  never used by community gadgets).
- `tunnel_netif.c`: the NAPT call only when lwIP has NAPT. The home-network tunnel is off here
  (`CONFIG_HOMEHUB_TUNNEL=n`), so that function is compiled but never called.
- `led_status.c`, `main.c`, `muse_glue.c`, the voice-PE, SenseCAP and e-paper files are not vendored.
- `../noise_core/src/PsaCryptoBackend.cpp`: AES-GCM with an empty payload goes through the PSA
  multipart API. On IDF 5.5, psa_aead_encrypt hands a zero-length payload to the driver as NULL and
  ESP's GCM rejects it ("esp-aes-gcm: No input supplied"), which failed the third Noise handshake
  message (empty payload) every time; found on hardware 2026-10-04.
- `port/muse_gadget.c` runs `app_run()` with a 12 KB stack: 8 KB (upstream's main task) left 1.5 KB
  after the first TLS lookup.

Kept off on purpose: OTA from Muse (it would replace this firmware with Meta's), the home-network
tunnel (~60 KB internal RAM), and the support bug-report upload.

## Updating from upstream

Copy the files listed above over `link/` (and `noise_core/src`, `noise_core/include`), re-apply the
changes in the list, build with the Muse transport selected, and update the revision above.
