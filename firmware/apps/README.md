# Applications

Each folder implements one user workflow: UI, voice sessions, recording, and Agent Link callbacks.
`profiles/profiles.json` selects one folder; two WOWNNY revisions share the same application.

- Derive from `Application` in `common/application.h` and register with `DECLARE_APPLICATION`.
- Use `boards/` for peripheral wiring and initialization. Shared drivers belong in `boards/common/`.
- Keep transport and protocol implementation in `components/agent_link/`.
- SDK callbacks copy data or queue work; camera, storage and audio workers perform blocking I/O.

Korvo applications share `boards/korvo/korvo_hardware.h`: LCD, camera, touch and codec are initialized
in that order. BLE owns SD recording; WiFi owns platform sessions and servo commands. Their existing
device model strings stay unchanged for App discovery and OTA validation.

Other applications moved from `boards/` without changing their bring-up sequence. Their remaining
peripheral setup can be extracted when sharing or changing hardware; a device with one application
does not need a duplicate hardware layer. `main/board.h` preserves the old Board name for external ports.
