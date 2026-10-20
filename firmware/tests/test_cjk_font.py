"""Check the shared Chinese font, including every GB2312 Hanzi.

Requires fontTools for the offline TrueType cmap/outline inspection.
"""
import io
import json
from pathlib import Path
import struct
import zlib

try:
    from fontTools.ttLib import TTFont
except ImportError:
    raise SystemExit("Install fontTools to inspect the Chinese font: python -m pip install fonttools")

root = Path(__file__).resolve().parents[1]
profile = json.loads((root / 'profiles/profiles.json').read_text(encoding='utf-8'))['waveshare-epaper-397-wifi']
asset, = profile['flash_assets']
assert asset['partition'] == 'anim_pack'
source = root / asset['file']
assert source == root / "boards/common/fonts/cjk_font.bin"
image = source.read_bytes()
magic, version, length, crc, reserved = struct.unpack('<8sIII12s', image[:32])
assert magic == b'AGLKTTF1' and version == 1
assert length == len(image) - 32 and reserved == bytes(12)
assert zlib.crc32(image[32:]) & 0xffffffff == crc

# Check the complete Hanzi rows, rather than a small sample of common characters.
hanzi = set()
for hi in range(0xb0, 0xf8):
    for lo in range(0xa1, 0xff):
        try:
            hanzi.add(ord(bytes((hi, lo)).decode('gb2312')))
        except UnicodeDecodeError:
            pass
assert len(hanzi) == 6763
with TTFont(io.BytesIO(image[32:])) as font:
    cmap = font.getBestCmap()
    assert not (hanzi - cmap.keys()), 'GB2312 Hanzi missing from the shipped font'
    assert all(cmap[cp] != '.notdef' and font['glyf'][cmap[cp]].numberOfContours != 0
               for cp in hanzi), 'A mapped Hanzi has no outline'
    punctuation = {ord(c) for c in '，。！？：“”‘’（）【】《》、；—…'}
    assert not (punctuation - cmap.keys()), 'Chinese punctuation missing'
    assert set(range(0x20, 0x7f)) <= cmap.keys(), 'ASCII missing'
    assert 'fvar' not in font, 'TinyTTF needs a static font instance'
    count = len(cmap)

rows = [line.split('#')[0].strip().split(',') for line in
        (root / 'partitions.csv').read_text(encoding='utf-8').splitlines()]
partition, = [row for row in rows if row[0].strip() == asset['partition']]
assert partition[1].strip() == 'data'
assert len(image) <= int(partition[4].strip(), 0), 'Font exceeds the existing partition'
assert int(partition[3].strip(), 0) == 0x412000
assert (source.parent / 'OFL.txt').is_file(), 'Font license missing'
print(f'Chinese font OK: {count} mapped characters, all 6763 GB2312 Hanzi with outlines, '
      f'punctuation, header/CRC, license and partition fit ({len(image)} bytes).')
