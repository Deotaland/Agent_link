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

Use the public build/package entrypoints rather than scripts under `tests/`:

```powershell
python firmware/tools/build_firmware.py --list
python firmware/tools/build_firmware.py waveshare-amoled-175-wifi --image-role factory --jobs 2
python firmware/tools/package_firmware.py waveshare-amoled-175-wifi --jobs 2
```

The package command builds Factory and the selected application sequentially in
the same build directory, ending on Application. For images offered through a
shared test service, add `--public` to reject private build settings. A completed
matching build can be repackaged with `--package-only`.

Board lookup and environment/catalog/documentation checks live in repository-root
`tools/` and `skills/`, because they also support onboarding without a firmware
build. The LAN-only app service remains under
[tests/local_app_store](../tests/local_app_store/README.md); it imports the
production image contract and does not implement a second build pipeline.

Build and package commands never flash or open a device. Flashing still needs an
explicit request for that run. See [profiles](../profiles/README.md) for defaults,
[layouts](../partitions/README.md) for storage and
[Factory](../factory/waveshare-amoled-175/README.md) for package contents.
