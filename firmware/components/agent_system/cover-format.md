# Application cover format

Use [make_app_cover.py](../../tests/local_app_store/make_app_cover.py) as the reference producer.
It requires Pillow on the backend; the device has no Pillow/PNG dependency.
From the repository root:

```powershell
python firmware/tests/local_app_store/make_app_cover.py "C:\artwork\voice.png" --app-id agent.voice --output assets/voice.raw
```

The tool prints `size`, `sha256` and `format` for the catalog. Production backends
can call `convert_cover(uploaded_bytes, embedded_app_id)` directly and store its
returned bytes in object storage; the local source URL option is for development.
Apply upload size/dimension limits at the backend as well.

1. Apply EXIF orientation, convert to RGBA, center-crop and resize to **132x132**
   using Lanczos. Preserve existing transparency and multiply it by an antialiased
   circle mask. The reference accepts static sources of at most 4096x4096 pixels.
2. Convert straight (not alpha-premultiplied) RGB to RGB565:
   `((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)`.
3. Serialize pixels left-to-right, top-to-bottom as **little-endian uint16**.
   Red is `00 F8`, green `E0 07`, blue `1F 00`. Then append **all A8 bytes**
   in the same pixel order; alpha 0 is transparent and 255 is opaque. Do not
   interleave alpha with individual RGB565 pixels.
4. Prefix the following 64-byte header, all multibyte fields little-endian.
   Python's layout is `struct.Struct('<8sBBHHHII40s')`.

| Offset | Bytes | Field | Required value |
| --- | --- | --- | --- |
| `0x00` | 8 | magic | ASCII `ALCOVER1`, no NUL |
| `0x08` | 1 | version | 1 |
| `0x09` | 1 | format | 1: RGB565LE plane followed by A8 plane |
| `0x0A` | 2 | width | 132 |
| `0x0C` | 2 | height | 132 |
| `0x0E` | 2 | reserved | 0 |
| `0x10` | 4 | data_len | 52272 = 132x132x3 |
| `0x14` | 4 | crc32 | `zlib.crc32(payload) & 0xffffffff`; pixels only |
| `0x18` | 40 | app_id | NUL-terminated embedded `Descriptor.app_id`, padded with zeroes |
| `0x40` | 34848 | RGB565LE | 132x132x2 bytes |
| `0x8860` | 17424 | A8 | 132x132 bytes |

The app ID must match the firmware descriptor, such as `agent.voice`, not a
database UUID or the generic ESP-IDF project name `agent_link`. Current app IDs
use 1..31 ASCII letters, digits, dots, hyphens or underscores. The different magic
and explicit little-endian pixels distinguish this format from SDGOODS' format:
**do not reuse their byte-swap setting**. Our renderer swaps the final display
buffer once, after drawing.

5. Calculate **SHA-256 over the entire 52336-byte block**, including the header,
   and return the four `cover_*` fields with that digest. Prefer content-hash URLs.
6. Serve the exact bytes with `Content-Type: application/octet-stream`,
   `Content-Length: 52336`, HTTP 200 and no `Content-Encoding` compression.
   Relative URLs resolve against the app service origin; private-LAN HTTP is
   supported for development and HTTPS is preferred for production. Redirects
   remain unsupported by the device.

Storage consumption is **52336 bytes (51.11 KiB)** per block, inside **57344 bytes
(56 KiB)** reserved at each OTA slot's end. The 132-pixel source is scaled up for
the large card, so it has less detail than the earlier 256-pixel PNG. UI geometry
and the three-app limit are unchanged.
