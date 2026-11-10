# Firmware host checks

This directory contains regression code and development fixtures. Production
firmware stays in `boards/`, `apps/`, `factory/` and `components/`; production
build/package/image logic stays in `tools/`. Tests compile or import those sources
and provide stubs only at host/device boundaries.

| Area | Entrypoints / supporting files |
| --- | --- |
| Board/application/layout and build selection | `test_profile_hardware.py`, `test_firmware_architecture.py`, `test_build_target.py`, `test_firmware_menu.py` |
| Multi-app installation, boot and covers | `test_agent_system.py`, `agent_system_test.cc`, `agent_system_stubs.h` |
| AMOLED board, audio, network modes and touch | `test_amoled_175.py`, `test_amoled_voice.py`, `test_amoled_network_mode.py`, `test_amoled_touch.py` and their C++ fixtures |
| Shared UI and Factory carousel | `test_amoled_ui.py`; renders actual production LVGL sources |
| Font assets | `test_cjk_font.py`, `test_raster_cjk.py` and font renderer checks |
| Paper 4IN0 controls, refresh, shared resources, touch/IME, UI and panel transfers | `test_paper_refresh.py --paper4`, `test_paper4_display.py`, `paper_resources_test.cc`, `paper4_state_test.cc`, `paper4_panel_test.cc`, `paper4_input_test.cc`, `paper4_touch_test.cc` |
| LAN application-store development service | [local_app_store/README.md](local_app_store/README.md), with its own HTTP tests and ignored user-supplied firmware/artwork |
| Other protocol, SDK, audio and board checks | Remaining `test_*.py` entrypoints and named C/C++ fixtures/stub directories |

Run checks from the repository root, for example:

```powershell
python firmware/tests/test_profile_hardware.py
python firmware/tests/test_firmware_architecture.py
python firmware/tests/test_build_target.py
python firmware/tests/test_firmware_menu.py
python firmware/tests/test_agent_system.py
python firmware/tests/test_amoled_ui.py --carousel --rotation 90
```

Menu checks require the activated ESP-IDF Python environment. Native C/C++ checks
use an available host compiler; UI tests also use the already restored LVGL
dependency. Follow each script's dependency notes. Run checks relevant to the
changed code and build the selected profile; do not start a whole-board build
matrix for a local refactor.

Executables, objects, preview PNGs and test logs belong under `firmware/build/`.
The local service keeps generated covers and temporary HTTP fixtures in its
ignored `.cache/`. Keep useful source tests in Git; never commit private settings,
downloadable firmware, serial logs or temporary build output. These checks do not
establish physical device acceptance or authorize flashing.
