# Local application store

A LAN HTTP service for testing the AMOLED 1.75 factory's online application
list, raw covers and firmware downloads. It needs Python and Pillow, with no
ESP-IDF installation required to serve existing application images.

See the [Chinese guide](README.zh-CN.md) for the complete workflow.

## Layout

| Path | Purpose |
| --- | --- |
| `server.py` | HTTP catalog and downloads, default port 8765 |
| `packages/` | Application `.bin` files manually copied here |
| `covers.json`, `assets/` | Artwork mapping and original images |
| `make_app_cover.py` | Reference 132x132 RGB565LE+A8 cover converter |
| `multi_app_image.py` | App descriptor, compatibility and checksum reader |
| `tests/test_server.py` | Host HTTP/path/cover checks |
| `.cache/` | Generated covers and temporary test files; ignored by Git |

## Use

From this directory:

```powershell
python -m pip install -r requirements.txt
python server.py
```

Copy installable application images into `packages/` before starting the
server. The directory itself defines the catalog: no `package.json` is read.
The filenames are unrestricted; app IDs, versions, sizes and checksums are
read from the binaries. Keep one image for each app ID. Factory and merged USB
images are skipped with a diagnostic. An empty directory yields an empty list.

All relative `--bundle`, `--covers-config` and cover-converter paths resolve
from this service directory, even when starting the script from another working
directory. Relative artwork paths resolve from the cover configuration's
directory. The default bundle is `packages`, and the default configuration is
`covers.json`. No PC-specific path is stored in the configuration.

Set the device's application catalog URL to the printed LAN address ending in
`/api/apps`. Restart the server after adding, replacing or removing firmware or
changing artwork. Changed firmware is refused with HTTP 409 until restart.

```powershell
python tests/test_server.py
```

The checks use temporary files and an ephemeral localhost port, and close the
server afterward. They do not touch devices or flash firmware.

## Optional source build

In an activated ESP-IDF terminal:

```powershell
python ../../tools/package_firmware.py waveshare-amoled-175-wifi --public --jobs 2
```

The production package tool applies public-package checks. It reuses
`firmware/build` and produces `firmware/build/packages/waveshare-amoled-175-wifi/` with
the factory and Voice images and matching ELF/MAP/USB packages.
It does not copy them into `packages/`. Manually copy only the desired
application images into the HTTP service directory. See the
[factory guide](../../factory/waveshare-amoled-175/README.md) for image roles,
USB offsets and settings impact.
