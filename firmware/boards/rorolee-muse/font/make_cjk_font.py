#!/usr/bin/env python3
"""Build the CJK font image rorolee-muse reads from its anim_pack flash partition.

    python make_cjk_font.py NotoSansSC[wght].ttf cjk_font.bin [--weight 500]

The board renders Chinese with LVGL's TinyTTF straight from flash (memory-mapped), so the font
costs no RAM and almost no firmware space. This script turns a TrueType font into the image it
expects:

  1. a variable font is pinned to one weight (TinyTTF would otherwise draw its default
     instance, which for Noto Sans SC is the hairline 100);
  2. it is cut down to what a reply needs: ASCII, Latin-1, general and CJK punctuation,
     full-width forms and the GB2312 character set (all 6763 hanzi), without hinting or
     OpenType layout tables, which TinyTTF does not use;
  3. it is prefixed with a 32-byte header the firmware checks before trusting the partition:
     magic "AGLKTTF1", version 1, TTF length and its CRC-32 (all little-endian), 12 reserved.

Needs fontTools (pip install fonttools). Noto Sans SC is under the SIL Open Font License 1.1,
which allows embedding it; keep OFL.txt with anything that ships the font.
"""

import argparse
import io
import struct
import sys
import zlib

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

MAGIC = b"AGLKTTF1"
VERSION = 1
HEADER = struct.Struct("<8sIII12s")   # 32 bytes


def gb2312_characters():
    """Every character GB2312 encodes: rows 0xA1-0xA9 (symbols) and 0xB0-0xF7 (hanzi)."""
    chars = set()
    for hi in list(range(0xA1, 0xAA)) + list(range(0xB0, 0xF8)):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes((hi, lo)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def wanted_codepoints():
    cps = {ord(c) for c in gb2312_characters()}
    for lo, hi in ((0x20, 0x7E),        # ASCII
                   (0xA0, 0xFF),        # Latin-1 supplement
                   (0x2000, 0x206F),    # general punctuation: dashes, quotes, ellipsis
                   (0x3000, 0x303F),    # CJK symbols and punctuation
                   (0xFF00, 0xFFEF)):   # full-width forms
        cps.update(range(lo, hi + 1))
    return cps


def build(src, weight):
    font = TTFont(src)
    if "fvar" in font:
        axes = {a.axisTag: (a.minValue, a.maxValue) for a in font["fvar"].axes}
        lo, hi = axes.get("wght", (weight, weight))
        if not lo <= weight <= hi:
            sys.exit(f"weight {weight} is outside the font's range {lo:g}-{hi:g}")
        font = instancer.instantiateVariableFont(font, {"wght": weight})

    opts = subset.Options()
    opts.layout_features = []        # single glyphs only: TinyTTF does no shaping
    opts.hinting = False
    opts.notdef_outline = True
    opts.glyph_names = False
    opts.name_IDs = ["*"]            # keep the copyright and licence records the OFL asks for
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE", "STAT", "vhea", "vmtx", "VORG",
                         "gasp", "DSIG"]
    subsetter = subset.Subsetter(opts)
    subsetter.populate(unicodes=sorted(wanted_codepoints()))
    subsetter.subset(font)

    cmap = font.getBestCmap()
    missing = [c for c in "你好洛杉矶今天还是大热天晴度左右高，。！？" if ord(c) not in cmap]
    if missing:
        sys.exit(f"subset lost characters it must have: {''.join(missing)}")

    out = io.BytesIO()
    font.save(out)
    return out.getvalue(), len(cmap)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", help="TrueType font, e.g. NotoSansSC[wght].ttf")
    ap.add_argument("out", help="image to write, e.g. cjk_font.bin")
    ap.add_argument("--weight", type=float, default=500,
                    help="weight to pin a variable font to (default 500, Medium)")
    args = ap.parse_args()

    ttf, chars = build(args.src, args.weight)
    header = HEADER.pack(MAGIC, VERSION, len(ttf), zlib.crc32(ttf) & 0xFFFFFFFF, bytes(12))
    with open(args.out, "wb") as f:
        f.write(header)
        f.write(ttf)
    print(f"{args.out}: {chars} characters, {len(ttf)} bytes of TTF + {HEADER.size}-byte header,"
          f" crc32 {zlib.crc32(ttf) & 0xFFFFFFFF:08x}")


if __name__ == "__main__":
    main()
