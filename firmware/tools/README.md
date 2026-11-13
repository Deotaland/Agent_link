# Firmware tools

These are the production host tools for building and delivering firmware.
They run from the checkout; generated plans, binaries, logs and caches belong
under the ignored `firmware/build/` directory.

| File | Responsibility |
| --- | --- |
| `build_firmware.py` | Supported profile selection, configuration, selected-image build, validation and artifact archive |
| `package_firmware.py` | Sequential Factory/application builds and verified initial USB packages |
| `flash_firmware.py` | IDF's managed USB adapter; checks layout, launcher and target before an authorized write |
| `firmware_manifest.py` | Pure host resolver for board/application/layout manifests and flash plans |
| `firmware_menu.py` | Generated Application/Factory menus and saved application selection |
| `firmware_image.py` | Host-side descriptor validation and OTA boot-selection encoding |

## Before running commands

Run the examples below from the **repository root**, in PowerShell. Commands
starting with `firmware/tools/` are not relative to this README's directory.
Activate an existing compatible ESP-IDF environment before configuring, building
or packaging. Metadata checks and offline manifest/image inspection need Python
only. See the [environment guide](../../guides/environment.md) for SDK setup.

Show the supported arguments:

```powershell
python firmware/tools/build_firmware.py --help
python firmware/tools/package_firmware.py --help
python firmware/tools/flash_firmware.py --help
```

## List profiles and check definitions

```powershell
# List supported board/application/backend combinations.
python firmware/tools/build_firmware.py --list

# Validate manifests, defaults and generated menu bindings without compiling.
python firmware/tools/build_firmware.py --check
```

## Build an application or Factory

```powershell
# Explicitly select the non-C AMOLED voice application.
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role voice --jobs 2

# Explicitly select that board's Factory launcher.
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role factory --jobs 2

# Build a normal single-app profile, such as Waveshare ePaper 3.97 WiFi.
python firmware/tools/build_firmware.py waveshare-epaper-397-wifi --jobs 2
```

Use `--jobs 1` when lower CPU/memory use is preferred. Omitting `--image-role`
keeps the saved Application/Factory target for a compatible profile; it does not
always select Application. The non-C AMOLED 1.75 and Sugarat Button support
Factory. The AMOLED Voice image targets `ota_0` at `0x310000`; Factory targets `factory`
at `0x10000`. Other profiles keep their own layout and application role.

The selected build writes `firmware/build/agent_link.bin`, matching ELF/MAP and
flash metadata. Managed application builds omit the root `build/merged.bin`;
use the package command below for an initial USB image.

## Configure without compiling

```powershell
# Apply the selected profile and Voice target, then stop after configuration.
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role voice --configure-only

# Regenerate configuration from defaults, then stop without compiling.
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role voice --reset-config --configure-only
```

`--reset-config` reapplies defaults and discards other local configuration
overrides. Recognized private settings stay in ignored `firmware/sdkconfig.private`.
Without `--configure-only`, the reset command continues with a build.

## Build or regenerate a board package

```powershell
# Build Factory and Voice sequentially, then verify and package both images.
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --jobs 2

# Prepare a package for a shared test service; reject private build settings.
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --public --jobs 2

# Repackage existing matching archives without compiling.
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --package-only

# Choose a different output directory, relative to the repository root here.
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --package-only --output firmware/build/packages/amoled-review
```

The package command builds Factory and the selected application sequentially in
the same build directory, ending on Application. This command requires a board
with a registered Factory; single-app boards use `build_firmware.py` instead.
`--package-only` requires both archived images to match the current manifests and
their recorded hashes; otherwise rebuild them first.

The default output is `firmware/build/packages/waveshare-amoled-175-wifi/`:

| File | Use |
| --- | --- |
| `merged.bin` | Initial USB package with Factory and Voice; boots Factory |
| `factory-only-merged.bin` | Initial USB package with Factory and empty app slots |
| `factory.bin`, `voice.bin` | Separate application images |
| `factory.elf`, `voice.elf`, matching `.map` files | Debugging those exact images |
| `flasher_args.json`, `package.json` | Flash settings/offsets and artifact hashes |
| `flash_assets/font_cjk.bin`, `fonts-OFL.txt` | Shared Chinese font and its license |

Initial merged packages replace saved settings and installed apps. Package
generation does not publish images to the local service's `packages/` directory.

## Inspect the current flash plan without accessing a device

After a successful configure/build:

```powershell
python firmware/tools/flash_firmware.py --plan firmware/build/flash-plan.json --describe
```

This prints the selected role, destination partition, offsets and shared assets.
It does not open a serial port or write flash. `flash_firmware.py` normally runs
as IDF's managed flash adapter; use the documented IDF flash workflow for an
actual write rather than assembling raw adapter/esptool arguments. A plan is
generated output; edit the source manifests or CSV instead.

## Regenerate menus after manifest changes

```powershell
python firmware/tools/build_firmware.py --generate-kconfig
python firmware/tools/build_firmware.py --check
```

This uses `firmware_menu.py` to update `firmware/main/Kconfig.profiles` and
`Kconfig.factories`. Run it after adding/removing a profile or changing Factory
bindings. Board catalog changes also need `python tools/board_catalog.py`.

## Inspect manifests and a packaged image offline

`firmware_manifest.py`, `firmware_menu.py` and `firmware_image.py` are imported
modules, not standalone command-line entrypoints. To inspect the current
AMOLED manifest and an already built Voice image without building or flashing:

```powershell
@'
import json
import sys
from pathlib import Path

sys.path.insert(0, "firmware/tools")
from firmware_manifest import flash_plan, resolve_profiles
from firmware_image import image_info

profile = resolve_profiles()["waveshare-amoled-175-wifi"]
plan = flash_plan(profile, "voice")
print(json.dumps(plan, indent=2))
image = Path("firmware/build/packages/waveshare-amoled-175-wifi/voice.bin")
info = image_info(image, plan["model"], plan["layout_version"],
                  plan["app_max_bytes"], chip=plan["chip"])
print(json.dumps(info, indent=2))
'@ | python -
```

The image example requires the package to exist. It validates the image's board,
chip, layout, SDK identity and usable slot size, then prints its app ID/version,
length and SHA-256. The host module APIs are shared by the build/package tools
and regression tests.

## Related tools and workflows

Board lookup and environment/catalog/documentation checks live in repository-root
`tools/` and `skills/`, because they also support onboarding without a firmware
build. The LAN-only app service remains under
[tests/local_app_store](../tests/local_app_store/README.md); it imports the
production image contract and does not implement a second build pipeline.

Build and package commands never flash or open a device. Flashing still needs an
explicit request for that run. See [profiles](../profiles/README.md) for defaults,
[layouts](../partitions/README.md) for storage and
[Factory](../factory/waveshare-amoled-175/README.md) for package contents.

Sugarat Button uses only sdgoods-v5: `recovery` at 0x10000, `launcher_a` at
0x90000, dynamic `app_pool` at 0x390000, and `launcher_b` at 0x1D00000.
Use package_firmware.py sdgoods-wifi --jobs 2 for initial USB packages. App-only
USB writes are refused; later apps are pure .bin files installed by Launcher.
The shared developer `appdata` label remains unchanged. When migrating from a
16 MiB volume, back up files and recreate the 13 MiB FAT volume separately.
