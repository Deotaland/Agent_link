# Shared Paper raster and input resources

Paper 4IN0 layout v2 stores one `AGLKRSC1` image in the `font_cjk` data partition.
It contains the existing 14 px, four-bit Noto Sans SC raster font, its glyph and
Unicode indexes, and the offline pinyin dictionary with frequencies. These bytes
are excluded from the app image. This format is different from the LVGL
`AGLKTTF1` asset; keep each board's matching loader and resource together.

`Init()` maps the resource once before UI/input tasks start. It validates the
version, bounds, payload CRC-32, bitmap extents, sorted Unicode index, dictionary
offsets and terminated strings. Font data and dictionary strings remain in
mapped flash. Candidate entries hold small pointer views; the full dictionary
is not copied to RAM. A mapping lives until reboot. Do not call `Install()` or
replace/erase the resource while consumers are running.

`Font()` exposes the existing `raster_font_t` interface. The component includes
`raster_montserrat_14.c` as a small built-in Latin fallback. Missing or damaged
resources produce an English recovery notice and disable the app's composer.
The [shared input engine](../offline_ime/README.md) uses `Dictionary()` directly.

## Regeneration

```sh
python firmware/tools/generate_paper_ime.py firmware/.cache/paper4-ime-sources
python firmware/tools/generate_paper_resources.py
python firmware/tools/generate_paper_resources.py --check
python firmware/tests/test_paper4_display.py
```

Ordinary builds use the checked-in `assets/paper_resources.bin`; they need no
downloads or font conversion tools. Its source inputs are the shared
`boards/common/raster_cjk_14.c` and `assets/pinyin.tsv`. Generation is deterministic
and records the input/image SHA-256 values in `assets/paper_resources.json`.
Update the pack when either input changes. The [input notices](assets/ime-data/NOTICE.md)
preserve both MIT licenses. The font uses the [SIL OFL](../../boards/common/fonts/OFL.txt).
Build archives include all three license files with the resource image.

## Binary format

All integers are little-endian. The 96-byte header is declared in
`include/paper_resources.h`. It starts with `AGLKRSC1`, format version 1, total
length and IEEE payload CRC-32. Remaining fields describe canonical sections:
bitmap bytes, 4-byte-aligned 12-byte glyph records, 16-bit Unicode index,
4-byte-aligned 12-byte dictionary records, and a shared UTF-8/NUL string pool.
Dictionary records hold offsets relative to that string pool and a frequency;
they contain no linked addresses. Header font metrics describe the raster data.
Reserved fields must be zero. Consumers reject unknown versions and malformed
sections before creating any views.

## Installation and updates

The board manifest declares the resource as a flash asset. Normal USB `flash`
and the merged initial package include it at the layout's `font_cjk` address;
`app-flash` and OTA write only the application. Install the v2 partition table,
app, boot selection and resource together on first migration from S31 v1.
An old-layout app-only OTA cannot perform that migration.

Compatible app updates reuse the resource. A future incompatible format must
use an explicit coordinated app/resource installation; no in-place resource OTA
or live resource replacement is implemented. Geometry lives only in
[the versioned layout](../../partitions/paper-4in0-v2.csv).
