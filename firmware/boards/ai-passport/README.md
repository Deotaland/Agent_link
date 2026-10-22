# AI Passport

ESP32-C3, 8 MB flash, ST7789 240x320, ES8311 full duplex at 16 kHz, PCM16 mono.
The BLE build sends microphone audio to the phone and plays its replies.

The UI draws directly into the panel driver's single 240x4 RGB565 DMA stripe
(1,920 bytes). It allocates no framebuffer or LVGL heap. Colors, layout,
Montserrat 14/20/28px glyph coverage and kerning match the previous UI.
The read-only font assets in `boards/common/raster_montserrat_*.c` retain their
upstream license. Unsupported characters use a placeholder, as with the old fonts.

The UI acknowledges a session token only after its changed regions finish
transferring. A state change interrupts the previous drawing pass; a transfer
failure closes the playback gate and stops reuse of a buffer that may still
belong to DMA.

Voice state follows VoiceReply commands rather than audio availability:

| App action | UI | Playback |
| --- | --- | --- |
| Prelude directly over L2CAP | Thinking | Play without a state change |
| `0x05 status=2` | Speaking | Wait for the Speaking screen, then play |
| Audio pauses without a command | Speaking | Re-prime while keeping the state |
| `0x05 status=3` | Speaking, then Ready | Drain PCM and 90 ms of I2S DMA |

`status=3` outside Speaking is ignored. Start/end commands arriving before the
button task runs are retained together. `status=0` or a failure ends a pending
reply; a new press or disconnect cancels the old generation. An empty formal
reply drains immediately. Speaking has no silence timeout; an explicitly
requested drain has a 30 s limit. Session IDs are logged without validation.

The 32 KiB playback buffer and bounded 24 KiB voice queue remain allocated.
The BLE profile uses 96 blocks in each MSYS pool and checks their free counts
separately, reserving 12 blocks per pool for control traffic. VoiceChunk carries
up to 494 bytes of continuous PCM at ATT MTU 512, with smaller packets at lower
MTUs. A congested packet can wait up to 2 seconds; queue overflow still ends the
session to avoid sending audio with gaps. Per-second flow logs show capture and
submission rates, queue depth and both pool counts. Submission is not a phone
receipt acknowledgement.
The BLE profile omits WiFi provisioning and cloud sources. Its dependencies are
selected from the firmware profile before ESP-IDF expands component requirements.

Memory snapshots are logged after display/codec initialization, BLE startup and
before voice creation. Voice and UI tasks log their remaining stack space.

Build in `firmware/build` with ESP-IDF 5.5.4:

```bash
python tools/build_firmware.py ai-passport-ble
```

## WiFi voice

Build from the repository root in the existing ESP-IDF 5.5.x or 6.1.x environment:

```sh
python firmware/tools/build_firmware.py ai-passport-wifi --jobs 2
```

The product is `FoloToy-AI-Passport`, platform product ID **1**, chip identifier
`esp32-c3`, at [bot.deotaland.cn](https://bot.deotaland.cn). Firmware keeps the
existing `AI-PASSPORT` model string. No account API key belongs in this profile.

After an authorized flash, join the `AIPassport-XXXX` hotspot shown on the screen
and open `http://192.168.4.1`. Configure WiFi, claim the six-digit activation code
in the platform console, and bind an Agent supporting realtime voice.

- Press OK to start/end a continuous conversation. The microphone starts only
  after `session.ready`. There is no hold-to-send interaction in the WiFi profile.
- UP/DOWN change volume by 5%, repeating while held. Volume is saved locally.
- Hold OK to end the call and open the menu. UP/DOWN select an entry, OK confirms;
  hold OK again to return home. Existing conversations have previous/next pages.
- The first call uses the latest returned conversation, or creates one if none
  exist. The selection survives restart only for the same bound Agent. The menu
  can select another conversation or create one; it does not delete conversations.
- Loss of network ends the call and stops capture. Connectivity retries through
  the SDK; press OK again after recovery to resume with a fresh voice grant.
- Speaker playback and its short echo tail upload silence. This is half-duplex
  feedback prevention, not acoustic echo cancellation or spoken interruption.

WiFi sessions request Opus in both directions, with 20 ms packets, 16 kHz mono
input and 24 kHz output decoded to the codec's 16 kHz PCM. On C3, encoding and
decoding share one 40 KB task stack; encoding also runs during playback pacing.
The encoder uses 24 kbit/s VBR and complexity 0. The queues hold up to eight
microphone frames and four encoded packets, without requiring PSRAM. The speaker
buffer is 8 KB. Incoming WebSocket storage starts at 2 KB, grows up to 16 KB for
complete messages, and shrinks after delivery. Larger messages fail explicitly.
Codec states are allocated before the voice TLS handshake. The WiFi profile
disables MQTT (`CONFIG_AGENT_LINK_MQTT_ENABLE=n`), including its task, TLS
connection and credential buffer. Voice grants, conversation management,
activation, HTTP heartbeat and Agent configuration polling remain enabled.
MQTT push commands and OTA commands delivered through MQTT are unavailable.
Gateway HTTPS connections are closed after each response on C3 to leave RAM
for voice; subsequent gateway calls pay a new handshake.
The screen renders in a shared 240x4 stripe without LVGL.
The small raster fonts cover Latin text; non-Latin answer text shows an explicit
placeholder while spoken replies remain available. Chinese typography is pending.

The WiFi application contains no BLE exchange, NFC web integration or contact
management. CW2017 battery reporting is not advertised by this version.

## Acceptance

Host checks cover button gestures, PCM packet assembly/conversion, playback
gating, bounded message growth/failure and device-authenticated platform APIs,
including C3 Opus negotiation.
A build is not hardware acceptance. Verify hotspot setup, activation, binding,
both audio directions, volume, call stop, network recovery, conversation paging
and repeated calls on the physical board. Keep the matching ELF for diagnosis.
