# Shared AMOLED UI

Round presentation shared by the 466px AMOLED profiles and 360px Sugarat Button.
Profiles opt in with `ui_component: "amoled_ui"`; this is not a universal UI
for every board or display shape.

This component owns:

- `amoled_ui.*`: LVGL task, bounded screen snapshots, generic list/text/volume
  presentation, pointer handling, scroll feedback and display recovery.
- `amoled_fonts.*`: resident font instances and the shared flash font loader.
- `amoled_ui_controls.h`, `amoled_orientation.h`: gestures, debouncing,
  mailboxes and frame/touch coordinate transforms.
- `amoled_ui_language.h`: shared language selection.
- `amoled_system_menu.*`: network/status/volume/language and return controls,
  enabled when the selected profile provides `agent_system`.
  Sugarat uses the same dark 2x2 Quick Panel in Factory and native apps, with
  Wi-Fi/Sound above Exit/Settings and a battery-percentage header. About remains
  in Settings. Settings is a scrollable list of brightness, Launcher update,
  About and device tools, without duplicate Wi-Fi/volume entries. Apps require
  confirmation before returning to Launcher. Other
  boards retain their existing menu. App-to-Factory update handoff stores only a
  UI request in isolated settings; download, verification and boot APIs stay in
  the Launcher.
  Sugarat defaults to English and restores an explicit saved language choice in
  both Factory and the Agent_link application.
  Its volume and brightness shortcuts use a shared vertical capsule with a live
  percentage and release commit through separate bounded mailboxes. Brightness
  retains its existing isolated 5–100% setting. The in-call horizontal control
  and other boards' volume pages remain available.
  Native apps can pass their touch policy to `StartUi` without a card renderer.
  Sugarat Agent_link enables the physical top cap, including its sides and busy
  views; Factory continues to use its renderer's existing touch policy.
  Both enable left-edge Back with a held-drag badge, release to commit, and
  drag-back cancellation. The carousel retains app-switch swipes, while other
  boards keep their existing gesture policy.
- `amoled_display_transfer.*`: optional launcher worker that owns a PSRAM
  staging buffer and returns borrowed LVGL pixels after synchronous panel IO
  finishes. The UI alone handles LVGL completion, panel errors and recovery.

Applications initialize the board hardware and pass ready panel/touch instances
to `amoled_ui::StartUi`. Hardware pins, bus setup, power and codec drivers stay
in `boards/`. UI compilation uses the selected board's display configuration;
the component does not instantiate `AmoledHardware` or depend on app directories.

Factory supplies an optional `CardRenderer` from its own `ui/` directory. It
owns carousel/action-page drawing and cover acquisition. Ordinary apps leave
this callback empty; the shared UI has no dependency on factory sources or the
voice application. Public screen updates contain bounded values; immutable raw
cover snapshots are acquired and retained by the factory renderer outside the
UI snapshot critical section.

`StartUi` defaults to `DisplayPerformance::kBalanced`. Factory explicitly selects
`kLauncher` for full-screen double buffers, asynchronous transfer and shorter
display/animation/input deadlines, while retaining dirty-area rendering. The
worker combines RGB565 wire byte order with a tiled rotation without modifying
the LVGL source. This is not a switch to forced FULL redraw or a guarantee of
tear-free scanout. FPS and input latency require measurement on the actual panel.

`amoled_ui` remains a presentation API: Agent conversations/call orchestration
and audio workers belong to the existing voice application. It does not select
an application's platform connection. Factory uses WiFi-only networking;
Voice retains Agent connectivity.

Native LVGL checks render the production component with optional factory pages:

```powershell
python firmware/tests/test_amoled_ui.py --rotation 90
python firmware/tests/test_amoled_ui.py --carousel --rotation 90
python firmware/tests/test_amoled_ui.py --sdgoods
```

These checks do not open a serial port or use a device. Target acceptance is
still required for touch, display recovery and audio interactions.
