# Shared AMOLED WiFi voice application

This directory contains the voice business implementation reused by
`apps/waveshare-amoled-175-wifi` and `apps/waveshare-amoled-175C-wifi`:

- `amoled_wifi_application.h`: Agent selection, conversations, call lifecycle,
  nearby friends and the application's SDK callbacks.
- `amoled_voice_audio.*`: microphone/playback workers and audio state.
- `amoled_pcm_diagnostics.h`: bounded numeric audio diagnostics.

The application entrypoint stays in each existing app directory. Each profile
selects its board's `config.h`, `amoled_hardware.h` and codec implementation.
Voice business code uses the public `amoled_ui` interface for presentation.

Shared rendering, fonts, gestures, language and the system menu live in
[components/amoled_ui](../../../components/amoled_ui/README.md). Factory carousel
pages live in [the non-C board factory](../../../factory/waveshare-amoled-175/README.md).
Factory does not compile these voice business/audio sources.

The C-board profile still uses its existing voice app and partition layout.
Moving shared UI code does not enable the non-C multi-app system on that board.

Host checks: `python firmware/tests/test_amoled_voice.py`,
`python firmware/tests/test_amoled_touch.py` and
`python firmware/tests/test_amoled_175.py`.

`apps/sdgoods-wifi` also selects this implementation, with its own SDGOODS
hardware facade and the restricted app-control policy. Its GPIO6 key controls
calls, bottom gestures stay inside the app, and Launcher exit is available only
in the pull-down menu. Managed applications store preferences through the system
settings partition, preserving SDGOODS agent_nvs isolation from vendor NVS.
The preinstalled Agent_link app explicitly enables physical top-edge pull-down
from the center and sides, opening the dark 2x2 **Quick Panel**: **Wi-Fi**, **Sound**,
**Exit** and **Settings**. **About** is inside Settings. The panel also works
during provisioning, calls and subpages. This input policy is independent of
Factory card rendering; other applications retain their default touch regions.
