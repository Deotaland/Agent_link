# Development environment

[Chinese installation walkthrough](environment.zh-CN.md) · [First use](getting-started.md)

Browser installation of a matching prebuilt image needs no ESP-IDF. Command-line
USB flashing needs Python, esptool and pyserial. Source builds need a working
SDK and the selected target's tools. Identify the board before installing tools.

## Versions and existing installations

For new S3/C3 installations prefer ESP-IDF 5.5.4; existing 5.5.x and 6.1.x
installations are supported. S31 needs 6.1.x containing
`components/soc/esp32s31`. P4 has no supported firmware profile. Agent Link's
Python tools require 3.10+ and CMake 3.22+. Keep SDK/project paths short, ASCII
and without spaces. Reuse the user's checkout and installed SDKs.

From the repository root:

```sh
python tools/check_env.py --list
python tools/check_env.py --profile ai-passport-wifi
python tools/check_env.py --profile ai-passport-wifi --lang en
python tools/check_env.py --profile ai-passport-wifi --search-dir D:/esp_idf --json
```

Replace the example profile with the selected device's profile. The checker
reads metadata and runs bounded tool/dependency checks; it never installs,
builds or opens a port. It finds conventional/registered and previously used
SDKs. A found-but-inactive SDK needs activation, not another download.
Open its installer-created terminal or EIM Dashboard activation command. For
classic installs run `export.ps1` in PowerShell or source `export.sh` on POSIX.
Activate again in each new terminal; do not guess a virtualenv folder name.

## New installation, including China

Use [official EIM](https://dl.espressif.com/dl/eim/). Windows can bootstrap
Python/Git through the GUI; do not require an environment-check script before
Python exists. macOS/Linux need their system prerequisites installed first;
see [official prerequisites](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/linux-macos-setup.html).

In Expert Installation select the required version and target, deselect `all`,
and choose both SDK-source and tool-archive mirrors. In China use the installer's
non-GitHub mirrors. Keep caches and existing installations. If the required
S31 SDK/target is unavailable, resolve that specific issue rather than choosing
S3 or an unsupported SDK. GUI steps are described in the
[official wizard](https://docs.espressif.com/projects/idf-im-ui/en/latest/gui_configuration.html).

The [China Windows 5.x installer](https://dl.espressif.cn/dl/esp-idf/) is an
alternative for 5.5.4, not a source for S31/6.x. Native Windows is sufficient;
WSL is not required. The Chinese walkthrough includes concrete PowerShell,
Linux and macOS steps and missing-Python recovery.

For complete existing SDK source missing tools, set per-terminal
`IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets` (no URL scheme), optionally
`PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple`, then run that SDK's
`install.ps1 TARGET` / `install.sh TARGET` and activation script. Use only the
selected chip. This [official asset mirror](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/tools/idf-tools.html#github-assets-mirror)
does not proxy source clones, submodules or component dependencies. Do not
rewrite global Git/pip/proxy settings or disable certificate/hash verification.

## Offline and recovery

Download an official offline archive matching OS, CPU and SDK version. Extract
the outer ZIP but leave `.zst` compressed for EIM. Check the current offline
Python prerequisites in the [official offline guide](https://docs.espressif.com/projects/idf-im-ui/en/latest/offline_installation.html).
An SDK archive does not include Agent Link's managed dependencies. Fully offline
builds additionally need verified caches matching this checkout, profile and
lock files; never substitute another project's LVGL archive or edit hashes.

Optional `--network` probes have bounded timeouts and report warnings separately
from tool failures; an index rejecting a probe does not prove an archive is
unavailable. Separate source, tool archive, Python package and component failures.
Retry a failed download once, then identify its layer and choose a documented
mirror/offline fallback. Do not print credential-bearing proxy URLs.

## Acceptance

Re-run the selected profile check in the activated terminal. Exit 0 / JSON
`ready: true` means tools are ready, not that downloads, build or hardware work.
When a build is requested run
`python firmware/tools/build_firmware.py PROFILE --jobs 2` from the root.
`build_firmware.py --check` checks profile metadata only.

`python tools/check_env.py --mode flash --ports` checks minimal flashing tools
and enumerates ports without opening them. Check the data cable, actual USB
connector, power and driver when no port appears. Opening a monitor or writing
firmware still requires an explicit device request.
