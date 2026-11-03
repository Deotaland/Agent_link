# Device first actions

[Chinese device table](devices.zh-CN.md) · [First use](getting-started.md)

`supported` means a source profile exists. It does not establish successful
hardware acceptance. Check the selected profile's [catalog notes](../AGENTS.md#board-catalog),
revision and USB connector before installation. WiFi profiles need provisioning,
activation and Agent binding first; BLE profiles use [Rorolee](../app/readme.md):
iOS overseas App Store search **rorolee**, Android APK at [rorolee.com](https://rorolee.com).

| Hardware / profile | Discovery | First action |
| --- | --- | --- |
| AI Passport BLE | AIPassport | Hold OK to speak, release to send |
| AI Passport WiFi | AIPassport-XXXX | OK starts/ends a call; hold OK for conversations |
| StopWatch BLE | M5StopWatch | Hold yellow to talk; hold blue 6 seconds to reset pairing |
| StopWatch WiFi | M5StopWatch-XXXX | Touch Start/End and Sessions; device acceptance pending |
| Waveshare AMOLED 1.75 non-C / waveshare-amoled-175-wifi | Amoled175-16M-XXXX | 16 MB board, distinct pins/model from C; hardware acceptance pending |
| Waveshare AMOLED 1.75C / waveshare-amoled-175c-wifi | Amoled175-XXXX | 32 MB board; BOOT starts/ends call; swipe left for apps; nearby acceptance pending |
| Waveshare ePaper 3.97 WiFi | WavePaper397-XXXX | Hold center for conversations, tap center for a call; device acceptance pending |
| paper V2 / S31 | PaperS31-XXXX | KEY1/2/3 navigate/confirm; Voice selects conversations; hold KEY1 for call, KEY2 to stop audio; device acceptance pending |
| Korvo BLE | KORVO | Open Voice or Camera; confirm ST7789 240x280 panel variant |
| Korvo WiFi | KorvoCloud-XXXX | Select/create conversation on voice page; camera preview is local |
| RoRoLee reference BLE | ROROLEE_S3 | Hold BOOT to talk, release to send; distinct from staff badge |
| RoRoLee work badge | ROROLEE_XXXXXX | Set staff fields in app and verify display; not a voice profile |
| RoRoLee / WOWNNY Muse | MuseGadget-XXXXXX | BOOT confirms pairing; hold to record and release to send |
| WOWNNY old Muse | MuseGadget-XXXXXX | Same flow; confirm old hardware revision first |
| AIoT Basic V2 camera | GC2145_CAMERA | Verify LCD preview and send snapshot to app |
| OJBadge | Local ESP-NOW, no activation | SW2 finds/greets peers; both sides confirm code; hardware acceptance pending |
| ES8311 ASR wiring example | ROROLEE_ES8311_ASR | Verify pins, then hold speech key; transcription only |
| ES8311 voice wiring example | ROROLEE_ES8311 | Verify pins, then hold BOOT to talk |
| Waveshare P4 | None | Not adapted; do not install S3/S31 images |

Read the Chinese table for exact profile names, keys and links to board details.
Wait for e-paper updates before judging state. No screenshots or successful
hardware tests are implied by this source-based table.
