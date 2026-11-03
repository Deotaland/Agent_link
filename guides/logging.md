# Logging and code maintenance

Normal firmware uses ESP-IDF Info verbosity. A boot log should identify the
board, firmware model/version and transport, followed by setup and connection
changes. Warn/Error messages explain a failure, its error code and the available
recovery. Do not hide all module logs to make a failure look quiet.

## Levels and content

| Level   | Use                                                                                  |
| ------- | ------------------------------------------------------------------------------------ |
| Error   | An operation or required resource failed                                             |
| Warning | A recoverable failure, rejected input, timeout, dropped audio or retry               |
| Info    | Startup identity, setup progress, connection/call start and stop, OTA outcome        |
| Debug   | Packet/chunk counts, register values, button edges, render/PCM/heap/stack statistics |

Use `ESP_LOGE/W/I/D` and a stable module tag. Prefer one bounded line with the
operation, result and useful numeric fields. Rate-limit repeated failures in
streaming loops; summarize at an existing interval or session boundary. Keep
errors at their original severity when moving routine statistics to Debug.

Never log WiFi passwords, pairing/session tokens, authorization headers, raw
request/response bodies, transcripts, replies or screen/badge text, even at
Debug. For platform business errors use the existing bounded
`cloud_log_safe_message` / realtime terminal-field sanitizer, passing every
applicable known credential. Do not serialize the whole server object. Avoid
unnecessary network names, personal Agent names and credential-bearing URLs.
Review logs before sharing: necessary hardware identifiers can still be present.

Do not perform a state change inside a logging argument. Increment counters,
update state and send messages separately so disabling a log cannot change the
operation. Return promptly from callbacks; do not add synchronous storage,
display work or packet-by-packet logging to a BLE/audio callback.

## Capture a diagnostic log

Use the selected source profile and an activated compatible ESP-IDF environment.
In `idf.py menuconfig`, set **Component config > Log output > Default log
verbosity** to **Debug**; the maximum verbosity must also allow Debug. Rebuild
only that profile. `sdkconfig` is generated and ignored. Return the default to
Info after diagnosis; changing only `esp_log_level_set()` cannot restore Debug
statements compiled out of an Info build.

Capture one bounded reproduction and retain the matching ELF. Flashing and
opening a port still require the user's explicit device-test request. Browser
installation of a published image does not change that image's compiled log
verbosity. Do not enable Debug in all public profile defaults.

Register/PCM/UI diagnostic summaries remain on their existing workers and
intervals. They do not record audio waveforms. The S31 optional display test is
a separate, explicitly selected diagnostic mode; normal firmware keeps it off.

## Code changes and reference projects

Keep hardware pins and initialization in `boards/`, application UI/session work
in `apps/`, and transport/protocol logic in `components/agent_link/`. Follow the
surrounding C/C++ naming, indentation and ownership conventions; avoid a whole
repository reformat during a behavior change. Comments should explain a hardware
constraint, lifetime or protocol decision instead of narrating an obvious call.
Preserve third-party copyright/license headers and the vendored upstream revision.

Before merging, run the checks in [AGENTS.md](../AGENTS.md#checks-before-merge),
relevant host regression checks and the selected profile build. Keep meaningful
test sources in Git; generated binaries, build/serial logs and local caches stay
ignored. A successful host check/build is not physical device acceptance.
