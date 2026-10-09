# Chinese font for rorolee-muse

Montserrat, the UI font, has no CJK glyphs. rorolee-muse draws Chinese with LVGL's TinyTTF from a
TrueType font kept in the `anim_pack` flash partition (`CJK_FONT_PARTITION` in its `config.h`; the
loader is `boards/common/cjk_font.cc`): memory-mapped, so it costs no RAM and only ~25 KB of
firmware. The boot log says `cjk_font: CJK font: 2184 KB from 'anim_pack'` when it is there;
without it the screen shows Latin text only and the log says `no CJK font image`. wownny-muse only
shows English text (in its menu), so it needs no font.

## Build the image

```sh
pip install fonttools
python make_cjk_font.py "NotoSansSC[wght].ttf" cjk_font.bin
```

`NotoSansSC[wght].ttf` is Google Fonts' Noto Sans SC (`ofl/notosanssc` in github.com/google/fonts),
under the SIL Open Font License 1.1 (`OFL.txt`, kept here because anything that ships the font must
carry it). The script pins the variable font to weight 500 (`--weight` to change; its default
instance is the hairline 100), keeps ASCII, Latin-1, punctuation, full-width forms and all of
GB2312 (7812 characters, about 2.2 MB), and adds a 32-byte header with the length and CRC-32 that
the firmware checks before using the partition.

## Flash it

Once per board, and again only when the font changes. `idf.py flash` leaves the partition alone.
From an ESP-IDF terminal in `firmware/`, with the serial monitor closed:

```sh
esptool.py --chip esp32s3 -p COM80 -b 460800 write_flash 0x412000 boards/rorolee-muse/font/cjk_font.bin
```

`0x412000` is `anim_pack` in `partitions.csv`. On a RoRoLee unit that partition held the production
animation pack, which this firmware does not use; the font overwrites it, so put the production
image back before returning the unit to the production firmware.
