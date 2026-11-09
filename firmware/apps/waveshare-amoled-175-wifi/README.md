# Waveshare AMOLED 1.75 WiFi application

This is the existing Agent voice application for the **non-C, 16 MB** board.
Its entrypoint is `waveshare_amoled_wifi.cc`, with `app_id=agent.voice`.
The `voice` build role and `voice.bin` filename refer to this same application;
there is no additional `apps/voice` application.

Voice business/audio code is shared with the C-board app under
[apps/common/amoled-wifi](../common/amoled-wifi/README.md). Display, fonts,
gestures and the common system menu are supplied by
[components/amoled_ui](../../components/amoled_ui/README.md). Pins and hardware
initialization remain in the selected board directory.

In an activated ESP-IDF terminal, from the repository root:

```powershell
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role voice --jobs 2
```

This produces `build/agent_link.bin` plus matching ELF/MAP, IDF flash metadata and
`build/flash-plan.json`. The application manifest owns `agent.voice`; hardware and
Factory binding come from the selected board, and addresses come from its layout.

Normal VS Code Flash or `idf.py flash` writes Voice to `ota_0` at `0x310000` and
selects it for the next boot. `app-flash` writes just the app and boot choice.
Both preserve the Factory launcher at `0x10000`, NVS and other application slots.
The flash adapter first reads the device layout, launcher and destination identity;
it refuses a different application in the target slot. Use Factory's installer to
choose an empty slot instead. These checks only run during an explicitly requested
Flash, never during a build or packaging.

For initial USB installation use the production board package tool:

```powershell
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --jobs 2
```

`build/packages/waveshare-amoled-175-wifi/merged.bin` contains the launcher, Voice,
bootloader, layout and font. It starts Factory with Voice installed. This merged
initial package replaces settings and installed apps. An application-only build
has no contiguous `build/merged.bin`, because filling its gaps would erase Factory.

The same app binary remains usable by Factory's installer. It confirms OTA readiness,
validates the launcher, publishes its slot and enables Return to launcher. An old
standalone Voice image at the factory address remains supported by runtime detection,
but is no longer the normal USB delivery path. Only Voice starts Agent authentication
and the conversation session.

Menuconfig first selects **Firmware build target**. **Application firmware** shows
**Application firmware profile**. **Factory firmware** hides that list and shows
**Factory board**, with only registered hardware (currently this non-C board).
The application selection is retained separately from the Factory board.

The board's [factory and packaging guide](../../factory/waveshare-amoled-175/README.md)
covers the full USB packages, partitions, local app service and raw cover protocol.
