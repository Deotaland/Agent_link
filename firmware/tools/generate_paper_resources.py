"""Pack shared 14 px raster glyphs and offline pinyin into a pointer-free flash image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'components/paper_resources/assets'


def build(font_source, dictionary):
    source = font_source.read_text(encoding='utf-8')
    def array(name):
        return re.search(r'\b' + name + r'\[\]\s*=\s*\{(.*?)\};', source, re.S)[1]
    bitmap = bytes(int(v, 0) for v in re.findall(r'0x[0-9a-f]+|\d+', array('bitmap')))
    glyphs = [tuple(map(int, row.split(','))) for row in re.findall(r'\{([^{}]+)\}', array('glyphs'))]
    sparse = list(map(int, re.findall(r'\d+', array('sparse'))))
    metadata = re.search(r'const raster_font_t raster_cjk_14\s*=\s*\{(.*?)\};', source, re.S)[1]
    sparse_count, glyph_count, kerning, line_height, baseline, sparse_base = map(int, metadata.split(',')[6:])
    if (sparse_count != len(sparse) or glyph_count != len(glyphs) or kerning or
            sparse_count+96 != glyph_count):
        raise ValueError('Unsupported raster font metadata')
    entries = []
    for line in dictionary.read_text(encoding='utf-8').splitlines():
        pinyin, text, frequency = line.split('\t')
        if not re.fullmatch('[a-z]{1,32}', pinyin) or not text or '\0' in text or not 0 < int(frequency) <= 0xffffffff:
            raise ValueError('Invalid pinyin entry')
        entries.append((pinyin, text, int(frequency)))
    strings, offsets = bytearray(), {}
    def string(value):
        if value not in offsets:
            offsets[value] = len(strings)
            strings.extend(value.encode('utf-8') + b'\0')
        return offsets[value]
    records = b''.join(struct.pack('<III', string(pinyin), string(text), frequency)
                       for pinyin, text, frequency in entries)
    payload = bytearray(bitmap)
    def align():
        payload.extend(bytes((-len(payload)) % 4))
    align()
    glyph_offset = 96 + len(payload)
    payload.extend(b''.join(struct.pack('<IHBBbb2x', *glyph) for glyph in glyphs))
    sparse_offset = 96 + len(payload)
    payload.extend(struct.pack('<' + 'H'*len(sparse), *sparse))
    align()
    dictionary_offset = 96 + len(payload)
    payload.extend(records)
    strings_offset = 96 + len(payload)
    payload.extend(strings)
    image = struct.pack('<8s22I', b'AGLKRSC1', 1, 96+len(payload), zlib.crc32(payload),
                        96, len(bitmap), glyph_offset, len(glyphs), sparse_offset, len(sparse),
                        dictionary_offset, len(entries), strings_offset, len(strings),
                        line_height, baseline, sparse_base, *([0]*6)) + payload
    record = dict(format='AGLKRSC1', version=1, bytes=len(image), glyphs=len(glyphs),
                  dictionary_entries=len(entries), sha256=hashlib.sha256(image).hexdigest(),
                  font_source_sha256=hashlib.sha256(font_source.read_bytes()).hexdigest(),
                  dictionary_source_sha256=hashlib.sha256(dictionary.read_bytes()).hexdigest())
    return image, (json.dumps(record, indent=2) + '\n').encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    image, metadata = build(ROOT / 'boards/common/raster_cjk_14.c', ASSETS / 'pinyin.tsv')
    for name, data in [('paper_resources.bin', image), ('paper_resources.json', metadata)]:
        path = ASSETS / name
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                raise SystemExit('Stale shared resources; run firmware/tools/generate_paper_resources.py')
        else:
            path.write_bytes(data)
    print(f'Shared raster/IME resources: {len(image)} bytes')


if __name__ == '__main__':
    main()
