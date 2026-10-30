# Shared Chinese font

`cjk_font.bin` is the existing Noto Sans SC Medium (weight 500) subset used by
rorolee-muse, now included once here as a versioned runtime asset for display profiles. It contains
all 6763 GB2312 Hanzi and 7812 mapped characters in total, including ASCII,
Latin-1 and available general/CJK punctuation and full-width forms. Characters
outside this subset, including emoji and many traditional/rare Hanzi, can still
fall back to `?`.

The source is [Noto Sans SC from Google Fonts](https://github.com/google/fonts/tree/main/ofl/notosanssc),
licensed under the [SIL Open Font License 1.1](OFL.txt). Keep OFL.txt with firmware
packages containing this font. The image is 2237460 bytes, including the 32-byte
AGLKTTF1 header (version, TTF length and CRC-32).

SHA-256: `723552b29cfafc615d53d8f618b2aee938beaac289db2af4efddc2391f0c5883`.

To regenerate from the upstream NotoSansSC variable TTF, install fontTools and run
from the repository root:

```sh
python firmware/boards/common/fonts/make_cjk_font.py "NotoSansSC[wght].ttf" firmware/boards/common/fonts/cjk_font.bin --weight 500
python firmware/tests/test_cjk_font.py
```

Waveshare's profile flash_assets entry copies this image into build/flash_assets and
registers it in flash_args, flasher_args.json and merged.bin. The existing
anim_pack data partition starts at 0x412000; partition offsets and NVS are not
changed. A segmented USB flash includes the font and retains NVS. A merged USB
image spans the intervening settings areas and can overwrite them. An app-only
OTA does not install the font: install both app and font for the first upgrade.
The font remains in flash across later app-only updates.

Only the display worker accesses TinyTTF, using a 32-glyph cache and memory-mapped
font data. A missing or damaged font image is logged and the old built-in subset
remains as a fallback. Successful loading logs cjk_font: CJK font: 2184 KB from
'anim_pack', 28 px. Check actual Chinese replies and punctuation on the device.

## Reuse across profiles

The font resource and generator are shared here. The flash-backed TinyTTF loader
is [cjk_font.cc](../cjk_font.cc), selected through a profile's drivers list. Each
board keeps its own pixel size, glyph cache budget and renderer; the same TTF
image supports all sizes. The legacy Muse generator path forwards to this tool.
The old ignored Muse font binary is not modified.

An LVGL profile can reuse the asset by selecting cjk_font.cc, enabling
CONFIG_LV_USE_TINY_TTF and declaring the resource once in profiles.json:

```json
"flash_assets": [
  {"partition": "anim_pack", "file": "boards/common/fonts/cjk_font.bin"}
]
```

Load it once from the display task after lv_init(), then add it as the fallback
of a writable copy of the UI's primary font. Keep the font and flash mapping
alive. Manual renderers must use the draw buffer returned by
lv_font_get_glyph_bitmap(), including its stride, and release the glyph data.
The loader and asset are independent of BLE/WiFi/Muse transport. Check the
profile's real partition layout and heap budget before adding it.

### Repository audit

The following profiles were inspected for text/font paths. This is a source
review, not a build or device verification of those other profiles.

| Profiles | Current text path | Reuse status |
| --- | --- | --- |
| waveshare-epaper-397-wifi | Manual monochrome renderer using LVGL fonts | This change installs and loads the shared font; keeps the built-in subset as fallback |
| waveshare-amoled-175-wifi | LVGL labels with Montserrat plus 14/20/24 px Noto Sans SC fallbacks | Installs the shared font asset |
| ojbadge-wifi | LVGL labels with Montserrat plus 14/20 px Noto Sans SC fallbacks | Installs the shared font asset; local friends UI |
| rorolee-muse | LVGL; shared LoadCjkFont and Montserrat fallback already present | Existing compatible consumer; USB font installation is still documented separately |
| m5stack-stopwatch-wifi | LVGL; SimSun 14 CJK subset for body text | Can use the same loader/resource with label font and cache settings |
| korvo-wifi | LVGL; SimSun 14 CJK subset for session/voice labels | Can reuse for session labels; SetVoiceText currently ignores incoming reply text, which requires separate UI work |
| m5stack-stopwatch-ble | LVGL; Montserrat for reply text | Needs a font fallback and resource selection |
| work-badge-ble | LVGL; Montserrat for badge fields and notes | Needs fallback at its 14/20/28 px sizes and an appropriate heap budget |
| ai-passport-ble | Custom raster Canvas with Montserrat bitmaps on ESP32-C3 | Cannot directly use the LVGL loader; needs a raster adapter/subset and memory review |
| ai-passport-wifi | Custom four-row raster Canvas with Noto Sans SC 14 px | Uses the derived flash-only raster font described below; no separate font partition |
| paper-s31-wifi | Custom 8x16 bitmap text; wrapper replaces non-ASCII before drawing | Needs UTF-8 wrapping and a renderer adapter; installing a font alone cannot restore Chinese |
| wownny-muse, wownny-old-muse | LVGL animation; reply text controls animation duration | No reply-text label is currently rendered; adding a font alone does not add text UI |
| rorolee-ble | ShowText currently fills the screen as a placeholder | Needs an actual text-rendering path first |
| korvo-ble, gc2145-camera-ble | No ShowText override; screen used by their existing hardware features | No current Agent-reply text consumer |
| es8311-voice-ble, es8311-asr-ble | No display UI | No font needed |

### AI Passport WiFi raster font

`ai-passport-wifi` reuses this same font source through an offline conversion:

```sh
python firmware/boards/common/fonts/make_raster_cjk.py
python firmware/tests/test_raster_cjk.py
```

The generated `boards/common/raster_cjk_14.c` contains 7812 mapped characters,
including all 6763 GB2312 Hanzi, at 14 px with four-bit coverage. Glyph bitmaps
use 687944 bytes; descriptors and indexes add about 109 KB. The existing stripe
renderer reads them directly from flash, without LVGL, runtime TTF allocation
or glyph caching. Rare symbols larger than the line box are scaled to fit while
retaining their entire outlines. UTF-8 clipping keeps complete codepoints.

The selected profile includes that font in `agent_link.bin`; no independent
font write command or partition change is needed. Do not use the S3 `anim_pack`
offset on the C3: the C3 address range belongs to its OTA slots. The build copies
this folder's OFL license to `build/fonts-OFL.txt` alongside the images. Generation
uses Pillow and fontTools offline; ordinary firmware builds use the checked-in C
asset. `make_raster_cjk.py --check` compares regenerated bytes in the same tooling
environment. Physical display acceptance remains separate from the host preview.
