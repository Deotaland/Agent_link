---
name: agent-link-setup
description: Prepare or diagnose the Agent Link ESP-IDF development environment, including existing SDK discovery, activation, China mirrors and offline installation. Use for missing tools and first builds; not runtime firmware bugs or device flashing.
---

# Prepare Agent Link tools

First determine whether the user needs source development or just an existing
firmware install. For the latter, prefer direct browser flashing in the
[firmware center](https://bot.deotaland.cn/#/firmware) using the flash skill.
No local repository, Git, Python, ESP-IDF or compilation is required; missing
development tools are not a reason to install them for this route.
It is limited to supported products with matching releases, and only writes
precompiled firmware. Recommend Windows/macOS desktop Chrome/Edge with Web Serial;
USB drivers/port permissions may still be required even though no SDK is needed.

For a source build, use the user's Agent Link checkout and read AGENTS.md.
Locate that checkout before running helpers from an installed skill. Read
`guides/environment.md` or `guides/environment.zh-CN.md` there for OS-specific
installation, verified official sources, China downloads and offline limitations.
A prebuilt image flashed from the terminal needs only the flashing tools;
`python tools/check_env.py --mode flash --ports` enumerates without opening ports.
For BLE, guide Rorolee installation: iOS users search **rorolee** in an overseas
App Store; Android users download the APK from [rorolee.com](https://rorolee.com).
Do not treat the app as unpublished or require private access. If it cannot be
installed or the user prefers standalone use, check for a supported WiFi profile.

For source builds identify hardware with the flash skill's board lookup. Ask
only for behavior/backend when several choices fit. Run from the repository root:

```sh
python tools/check_env.py --profile PROFILE --json
```

The checker is read-only. It discovers active, registered, previously used and
conventional SDK installations; add `--search-dir PATH` for another install root.
Missing Python is handled by the guide's GUI installer bootstrap, not this script.
An inactive compatible SDK should be activated and checked again before proposing
a new download. Do not replace SDKs or change the user's default shell/global
Git, pip, proxy or certificate settings. Use per-terminal mirror settings.

For S3/C3 prefer 5.5.4 for a new install; existing 5.5.x and 6.1.x are supported.
S31 requires a 6.1.x SDK containing `components/soc/esp32s31`; if the official
installer does not offer that target/version, report the exact missing support
instead of substituting another chip or SDK. Install only the required target.

Use official EIM's version/target/mirror selection for a new installation.
China users should choose its non-GitHub mirrors; SDK tool assets can use
`IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets` (no scheme). The guide separates
SDK source, compiler archives, Python packages and project components. An
offline SDK installer does not supply this project's managed dependencies.
Probe endpoints with `--network` only when downloads need diagnosis; a warning
is not proof a particular archive is unavailable. No endless download retries:
after a failed retry identify the failing layer and use the documented fallback.

After an authorized install/activation rerun the same profile check. Report the
SDK path/version, target, tool failures and next build command. A zero exit code
does not verify dependency downloads, a firmware build, port identity or hardware.
Build only when requested or necessary for the user's requested firmware task;
reuse firmware/build with one or two jobs. Setup does not authorize flashing,
opening a port or erasing. Keep private settings and images local.
