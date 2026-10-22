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
input and 24 kHz output decoded to the codec's 16 kHz PCM. On C3, network I/O,
encoding and decoding share one resident 40 KB task stack, created at SDK startup
and retained between calls. Microphone capture and I2S playback keep their own
workers and the 8 KB playback buffer. Encoding and nonblocking TLS writes also
advance during playback pacing; a full uplink queue never blocks this worker.
The board reports its actual queued PCM bytes. Realtime output waits for real
space (75% watermark), including playback prebuffer/rebuffer pauses and pending
queue resets, rather than estimating consumption from elapsed wall time.
The encoder uses 24 kbit/s VBR and complexity 0. The queues hold up to eight
microphone frames and two encoded packets, without requiring PSRAM. C3 batches
at most two packets, so a full encoded queue is immediately eligible for sending.
The speaker
buffer is 8 KB. Incoming WebSocket storage starts at 2 KB, grows up to 16 KB for
complete messages, and shrinks after delivery. Larger messages fail explicitly.
TLS handshaking and codec processing reuse the resident stack at different times.
After the verified WSS upgrade, when handshake scratch is free, continuous Opus
calls reserve one 25600-byte codec workspace. The C3 worker's vendor codec
allocations use that workspace without falling back to the system heap. Codec
wrapper/core allocations are reclaimed together when a state closes, and the
whole reservation is released at call end. Other tasks retain their normal
allocators. Continuous Opus calls follow the application's half-duplex policy:
listening keeps only the encoder; `response.started` frees it before opening the
decoder. During replies, the existing microphone cadence uploads a cached valid
20 ms Opus silence packet. Reply completion or interruption closes the decoder
before reopening the encoder. First binary audio also enters reply mode if the
start event is absent. Switching discards queued obsolete mic packets, while any
partial TLS write finishes with its original bytes. Tasks and I2S stay resident.
Calls no longer request a second contiguous 40 KB task stack or retain
the separate 12 KB network stack. Per-call connections and codec states are freed
at call end; the resident worker waits on its grant queue until the next call.
TLS stays nonblocking. Partial headers/payloads and incomplete writes survive
across worker passes; masked frames are neither interleaved nor replayed. Writes
have a 3-second deadline while receive and codec processing continue.
On C3, a shared memory lock serializes HTTPS requests and the live voice call.
Periodic HTTP heartbeat/configuration polling defers during a call and resumes
when it ends; platform binding changes are therefore refreshed after the call.
The WiFi profile
disables MQTT (`CONFIG_AGENT_LINK_MQTT_ENABLE=n`), including its task, TLS
connection and credential buffer. Voice grants, conversation management,
activation, HTTP heartbeat and Agent configuration polling remain enabled.
MQTT push commands and OTA commands delivered through MQTT are unavailable.
Gateway HTTPS connections are closed after each response on C3 to leave RAM
for voice; subsequent gateway calls pay a new handshake.
The screen renders in a shared 240x4 stripe without LVGL. The 14 px Chinese raster
font is derived offline from the existing common Noto Sans SC image. All 6763
GB2312 Hanzi, punctuation and mixed Latin text render directly from flash, with
no runtime TTF cache. Reply text and conversation names retain UTF-8; truncation
keeps complete characters. Unsupported glyphs retain a visible missing-glyph box.
The source/generator/license are in [common/fonts](../common/fonts/README.md).
The generated raster font is part of the application image: app-only updates
install it, with no separate font offset, font partition or partition-table
change. The shared 2.13 MiB TTF image is not separately flashed on this profile.

## WiFi memory budget

The 2026-10-22 device log for ELF `98ec192f07fe51cc` measured the earlier shared
worker build, before the following memory fixes. Values below are bytes of
internal heap; the new build has not yet been measured on hardware.

| Earlier device checkpoint | Free | Largest block |
| --- | ---: | ---: |
| Application initialized | 201732 | 114688 |
| WiFi/cloud and voice queues initialized | 155684 | 114688 |
| Resident 40960-byte worker started | 114380 | 73728 |
| Verified WSS upgrade completed | 76528 | 57344 |
| Both codec states, PCM buffer and encoded queue allocated | 24212 | 9728 |
| First reply / failed 4437-byte TLS TX allocation | 10004 | 4096 |

The subsequent device build `e59daf070d79b602` held 56988 bytes free with a
40960-byte largest block after encoder setup. It completed 12 replies before
encoder reopening failed with Opus `-7`: 66992 bytes were free, but the largest
block was only 18432 bytes, smaller than the encoder's 24524-byte core state.
That is why the current implementation retains a single workspace through all
reply transitions. The same log recorded dropped 640-byte speaker frames and
up to 879 ms between audio deliveries. Actual queue feedback fixes the local
overflow path; network jitter and long-call performance still need device tests.

The next device build `1ed309f4fd7b22d2` successfully reused its 25600-byte codec
workspace and completed the first reply, but failed during the second reply's
TLS read: `alloc(16749 bytes) failed`, `-0x7F00`. Immediately beforehand the heap
had 21396 bytes free and an 8704-byte largest block; at the error log the largest
block was 16384, still smaller than the requested allocation. WiFi stayed
associated and the application returned online without a reboot. That log does
not establish a leak. Codec reuse alone did not reserve the incoming record peak.
The current fix therefore uses IDF's post-handshake static RX strategy, reserves
the complete message before codec preparation, and borrows ordinary audio-delta
strings directly from that message. RX and message buffers are retained only
through the call; the audio workers remain resident between calls.

The codec preparation delta was 52316 bytes. The final error is a TLS allocation
failure (`-0x7F00`), with insufficient contiguous memory. The log does not identify
every allocation between readiness and the failure: peer TLS records, JSON
parsing and WiFi/TCP buffers must be included in that peak, rather than treating
the idle free-heap figure as the required budget.

| Allocation | Earlier policy | Current policy / evidence |
| --- | --- | --- |
| IRAM reservation | 89600, then 81920 | 70144; current linked ELF confirms 11776 additional bytes reclaimed by disabling RX acceleration, 19456 total versus the original layout |
| Static DRAM data + BSS | 40156 | 40164 in the current linked build, including allocator routing state; this excludes dynamic heap allocations |
| Application workers | 21504 total | Unchanged: UI 4096, control 6144, keys 3072, mic 4096, playback 4096 |
| Realtime + cloud stacks | 40960 + 8192 | Unchanged; main/SDK/WiFi/lwIP task stacks are additional allocations |
| Speaker stream buffer | 8192 | Unchanged, 256 ms of 16 kHz mono PCM16 |
| Full-duplex I2S DMA payload (initial configuration) | Up to 11520 | Initial stereo request: 6 descriptors x 240 frames x 4 bytes in each direction; codec setup subsequently selects mono slots. Descriptors/driver objects are additional |
| Mic queue payload | 8 x 652 = 5216 | Unchanged, up to 160 ms; queue control/heap metadata are additional |
| Encoded queue payload | 4 x 1288 = 5152 | 2 x 1288 = 2576, saving 2576 bytes; each slot still accepts a full 1275-byte Opus packet |
| Decoder PCM buffer | 3840 | 640, saving 3200 bytes for negotiated 20 ms, 16 kHz mono output; longer packets fail explicitly |
| Opus core state | Encoder 24524 + decoder 17784 | One 25600-byte reservation holds either state plus its wrapper. The encoder/decoder sizes are derived from the C3 library's size functions and static CELT mode. Reopening no longer needs a new system-heap block |
| Chinese font | Latin-only rendering | 687944 bitmap bytes plus glyph/index tables in flash (about 0.8 MB total); no dynamic glyph cache |
| TX scratch | 5810 | Unchanged: masked wire, PCM batch, JSON and 1024-byte read area; Call metadata and frame carry are additional |
| TLS TX record allocation | 4437 | 1365 at the standard write entry; final ELF confirms 1357-byte TLS buffer plus 8-byte dynamic-buffer header, saving 3072 at TX peak |
| TLS RX record | Peer-dependent dynamic allocations; observed failure at 16749 | Full 16384-byte content limit preserved; SDK allocates 17058 once after handshake, before Opus, and reuses it through disconnect; verified in linked disassembly |
| Complete WebSocket message | 2048 initially, up to 16384; realloc during replies | 2048 during upgrade, then 16384 reserved before Opus and retained; no reply-time realloc |
| Base64 audio delta | JSON message + cJSON string + decoded allocation | Ordinary audio objects validate without allocation and decode in the message itself; unusual/other events retain cJSON, with in-place decoding in its string |
| WiFi/TCP buffers | RX static 4, RX/TX dynamic 8 each; TCP TX 4096 / window 8192 | Preserved; dynamic buffer counts are limits, not constant heap charges |
| Gateway/config buffers | 2048 + 2048 | Preserved; HTTPS requests cannot overlap a live call; conversation pages use a bounded 12288-byte buffer outside calls |

Fixed queue/PCM savings are 5776 bytes, in addition to 19456 bytes of reclaimed
IRAM, the smaller TX peak and elimination of one live codec state. These are
allocation/layout comparisons, not a claimed new device free-heap measurement.
The codec reservation retains its full 25 KB during replies instead of returning
the encoder-sized block to other allocators. That trades roughly 7.8 KB of reply
headroom for deterministic state switching. The cached silence packet, allocator
overhead and new metadata are additional. Fixed receive buffers increase idle
call occupancy: the message adds 14336 bytes over its 2 KB upgrade capacity, and
static RX retains 17058 instead of the dynamic idle representation. They prevent
large allocations from being deferred until playback; they do not create RAM.
The additional 11776-byte RX IRAM saving offsets only part of that reservation.
General WiFi throughput trades some speed for RAM; voice
and worst-case scheduling still require physical acceptance. Do not reduce the
40 KB stack based only on its pre-encoding watermark: vendor codec stack depth
and TLS handshake depth differ.

`voice memory` logs report free heap, largest block and lifetime minimum every
five seconds, plus stack watermark and workspace allocation count/peak at
reply/listening transitions. Keep these
alongside uplink drop, encoding time and pacing diagnostics when testing long
calls, interrupted replies and repeated call teardown/reconnect.

The WiFi application contains no BLE exchange, NFC web integration or contact
management. CW2017 battery reporting is not advertised by this version.

## Acceptance

Host checks cover button gestures, PCM packet assembly/conversion, playback
gating, bounded message growth/failure and device-authenticated platform APIs,
including C3 Opus negotiation, HTTPS/voice exclusion, deferred polling recovery,
resident-task lifetime, upgrade validation, fragmented frames and partial writes.
`test_platform_api.py --c3 --voice-memory --idf-path <IDF>` executes the production
Call implementation across 1000 replies using the production codec allocator
with a fixture that forbids both states at once. The model's general heap is
restricted to 18432-byte blocks after setup; every reopen must reuse the original
reservation. It covers foreign-task allocator isolation, silence packets,
resumed speech, real-queue pacing, congestion, maximum Opus packet size, missing
start events, interruption, admission failure and unpublished-allocation rollback.
It also compiles IDF's real Base64 implementation to verify in-place decoding.
Audio-delta fast-path checks compare borrowed strings with real cJSON parsing,
exercise escaped ASCII, nested metadata, duplicate keys, malformed/truncated
objects and large messages, and assert zero cJSON allocations for ordinary audio.
The recovery test repeats 1000 complete 16 KB messages in 1 KB read fragments
without reallocating the reserved storage. `test_tls_receive_memory.py` extracts
the local IDF's actual RX allocation functions: a fixture reproduces the dynamic
16749-byte allocation failure with an 8704-byte block limit, then runs 10000 full
record-buffer cycles against a single 17058-byte static reservation under the
same limit. It checks counter/IV preservation, admission failure and teardown;
it does not run TLS decryption, networking or model physical heap occupancy.
`test_raster_cjk.py` checks every GB2312 Hanzi, punctuation, UTF-8 clipping/wrapping
and the real four-row renderer; its preview is a host render, not a device image.
A build is not hardware acceptance. Verify hotspot setup, activation, binding,
both audio directions, volume, call stop, network recovery, conversation paging
and repeated calls on the physical board. Keep the matching ELF for diagnosis.

The 2026-10-23 built increment uses ESP-IDF 5.5.4 and the unchanged C3 partition
table. Its application is 2286960 bytes, with 44% of the OTA slot free. Matching
ELF SHA-256: `5531bb2f0acc9dd702d98faad2a0f85163f4f73d04803338396bd5ec6560ad64`.
`build/voice-cjk-manifest.json` records image/debug hashes; the merged segments
and embedded app ELF identity were checked. The host font/allocator/playback
and receive-memory checks passed; this increment has not been flashed or
accepted on hardware.
