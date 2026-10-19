# Applications

Each directory implements a device workflow: UI, voice sessions, recording and
SDK callbacks. `profiles/profiles.json` selects one application per build.

Derive from [Application](common/application.h) and register with
`DECLARE_APPLICATION`. Put wiring and hardware initialization in `boards/`,
shared peripheral drivers in `boards/common/`, and transport code in
`components/agent_link/`. Callback data may expire on return: copy or queue it
before handing work to display, storage or audio workers.

Korvo BLE/WiFi share `boards/korvo/`; StopWatch BLE/WiFi share
`boards/m5stack-stopwatch/`. The WOWNNY revisions share an application and have
separate pin settings. Some older applications still initialize their own
hardware. Extract that setup when it needs to be shared.

`main/board.h` preserves `Board` and `DECLARE_BOARD` aliases for existing ports.
See the [board guide](../boards/README.md) for adding hardware or a BLE/WiFi profile.
