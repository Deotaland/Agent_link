# Managed applications

`agent_system` owns installation, discovery and switching for boards with a
registered Factory and managed flash layout. It is separate from Agent platform
connectivity and from the board's launcher pages. The AMOLED factories and
Sugarat Button use managed applications; ordinary single-app boards retain A/B OTA.

| Path | Responsibility |
| --- | --- |
| `include/agent_system.h` | Public app identity, catalog, installation, boot selection and shared-settings API |
| `include/agent_cover.h` | Public cover format and retained pixel storage |
| `agent_system.cc` | Verified app-slot operations, installation markers, catalog/settings HTTP and launcher return |
| `cover_cache.cc`, `cover_cache.h` | Private download, validation and slot-tail cover storage |
| `community_store.cc`, `community_cover.cc` | Board-selected anonymous production community, download tickets and PSRAM image decoding |
| `CMakeLists.txt` | Dependencies and layout/identity constants resolved from manifests |
| [cover-format.md](cover-format.md) | Backend cover encoding and delivery contract |

Hardware pins and initialization belong to `boards/`; app sessions/audio belong
to `apps/`; carousel pages and cover scaling belong to `factory/<board>/ui/`.
Shared display presentation belongs to `components/amoled_ui/`. This component
does not depend on a specific board's UI sources.

## Startup and operations

`Init()` opens shared storage without erasing settings on failure. `Ready()`
confirms application readiness and verifies the launcher before arming a return
target. A running OTA app can then return through `ReturnToLauncher()`. Factory
and standalone images do not install themselves as ordinary applications.

`Install()` writes only an eligible OTA app slot, excludes its cover reservation
and rejects duplicate app IDs. Length, SHA-256, ESP image and embedded descriptor
checks complete before the installation is marked ready. Interrupted writes stay
unavailable and can be reused. `Remove()` removes the selected app and its cover;
shared WiFi, device identity, binding, volume and language settings remain intact.

Catalog, install and cover synchronization calls perform blocking work and belong
on controller workers. They must not run in LVGL, BLE or audio callbacks. Cover
readers retain immutable PSRAM data while a background update publishes a new
snapshot. UI pages consume the public API rather than reading flash or NVS.

## Build and delivery

The board selects a layout in [partitions/layouts.json](../../partitions/layouts.json)
and binds its Factory manifest. App manifests own app IDs and build roles. The
host-side tools in `firmware/tools/` resolve these sources into one flash plan:

- `firmware_manifest.py`: board/application/layout composition and destinations.
- `firmware_image.py`: shared host image descriptor and boot-selection encoding.
- `build_firmware.py`, `package_firmware.py`: selected image builds and verified packages.
- `flash_firmware.py`: checks the actual device before an authorized managed USB write.

The initial full package boots Factory with the selected app installed. Separate
application USB updates preserve Factory and other slots. Full merged packages
replace settings and installed apps. See the
[board Factory guide](../../factory/waveshare-amoled-175/README.md) for commands.

## Validation

From the repository root:

```powershell
python firmware/tests/test_agent_system.py
python firmware/tests/test_firmware_architecture.py
python firmware/tests/test_build_target.py
python firmware/tests/test_firmware_menu.py
python firmware/tests/local_app_store/tests/test_server.py
```

Menu checks need the activated ESP-IDF Python environment. Native tests exercise
production sources using host boundary stubs; the local service checks the HTTP
contract. They do not flash a device. Build only the selected board/profile before
committing firmware changes; device acceptance remains a separate step.

Sugarat Button production-store checks:

```powershell
python firmware/tests/test_agent_system.py --community --packages-report firmware/tests/local_app_store/.cache/sdgoods-http-report.json
python firmware/tests/test_community_cover.py
```

The community fixture exercises actual native/vendor installers, anonymous
pagination, fresh tickets, metadata mismatch, invalid images, ZIP rejection,
error messages/request IDs and isolated settings. Codec fixtures use the actual
WebP/PNG/JPEG sources; an available public cover snapshot is also decoded.
These checks simulate flash and do not prove execution on physical hardware.

## Community upload contract

Publish one original pure application `.bin` (`application/octet-stream`) and
upload its cover separately. The backend owns the title, release version,
product/hardware compatibility, file size and SHA-256. It must validate the actual
ESP application image; a `.bin` extension alone does not establish compatibility.
USB full-flash packages, ZIP/UF2, bootloaders, partition tables and OTA selection
records are not launcher applications. Keep full-device packages in a separately
identified delivery flow.

Native Sugarat Button applications require the `SDGOODS-ESP32S3` compatibility
model, application ABI 3 and SDK 0.1.0. Reviewed original SDGOODS applications
retain their IDF project/version and binary bytes. A download manifest's
`0x10000` address identifies its application file; the launcher chooses the
actual installation extent. Extra filesystems, fixed offsets and custom
bootloaders require separate adaptation and device acceptance.

Developers upload through `/api/v1/uploads`, save `/api/v1/me/plays` drafts and
submit them for review. Administrators collecting reviewed applications use
`/api/v1/admin/play-uploads` and `/api/v1/admin/plays` instead. Both flows request
a storage URL, PUT the raw file and confirm it before referencing the ready file
in a draft. User/admin files are not interchangeable. Storage PUTs carry only
the returned storage headers; account identity belongs to the business API.
The Factory's public list/detail/download-ticket flow requires no account or
device authentication on the configured deployment.
