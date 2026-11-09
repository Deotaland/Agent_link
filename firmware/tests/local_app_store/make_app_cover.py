#!/usr/bin/env python3
"""Convert uploaded artwork into the Agent Link 132x132 slot-cover contract.

python make_app_cover.py assets/voice.png --app-id agent.voice --output assets/voice.raw
Pillow is needed only on the backend. No device, flashing or image generation.
"""
import argparse
import hashlib
import io
import re
import struct
import zlib
from paths import local_path

EDGE = 132
HEADER = struct.Struct('<8sBBHHHII40s')
PIXEL_BYTES = EDGE * EDGE * 3
BLOCK_BYTES = HEADER.size + PIXEL_BYTES
RESERVE_BYTES = 0xE000
FORMAT = 'alcover-rgb565a8-le-v1'
MAGIC = b'ALCOVER1'


def validate_cover(data, app_id=None):
    if len(data) != BLOCK_BYTES:
        raise ValueError('Cover must be exactly 52336 bytes, including its 64-byte header')
    magic, version, fmt, width, height, reserved, length, crc, identity = HEADER.unpack_from(data)
    if (magic, version, fmt, width, height, reserved, length) != (MAGIC, 1, 1, EDGE, EDGE, 0, PIXEL_BYTES):
        raise ValueError('Unsupported Agent Link cover header')
    if b'\0' not in identity:
        raise ValueError('Cover app_id is not NUL terminated')
    identity = identity.split(b'\0', 1)[0].decode('utf-8')
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}', identity) or (app_id is not None and identity != app_id):
        raise ValueError('Cover app_id must match the embedded firmware app_id')
    if zlib.crc32(data[HEADER.size:]) & 0xffffffff != crc:
        raise ValueError('Cover pixel CRC-32 mismatch')
    return dict(size=len(data), sha256=hashlib.sha256(data).hexdigest(), format=FORMAT)


def convert_cover(source, app_id):
    from PIL import Image, ImageChops, ImageDraw, ImageOps
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}', app_id):
        raise ValueError('Use the firmware app_id: 1..31 ASCII letters, digits, dots, hyphens or underscores')
    with Image.open(io.BytesIO(source)) as original:
        if original.width * original.height > 4096 * 4096 or getattr(original, 'n_frames', 1) != 1:
            raise ValueError('Use a static image of at most 4096x4096 pixels')
        image = ImageOps.fit(ImageOps.exif_transpose(original).convert('RGBA'), (EDGE, EDGE),
                             method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    # Preserve source alpha and add an antialiased circle; RGB stays straight,
    # not premultiplied. A8 is its own plane after all RGB565 pixels.
    mask = Image.new('L', (EDGE * 4, EDGE * 4), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, EDGE * 4 - 1, EDGE * 4 - 1), fill=255)
    alpha = ImageChops.multiply(image.getchannel('A'), mask.resize((EDGE, EDGE), Image.Resampling.LANCZOS))
    colors = bytearray(EDGE * EDGE * 2)
    rgba = image.tobytes()
    for i in range(EDGE * EDGE):
        red, green, blue = rgba[i*4:i*4+3]
        value = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        struct.pack_into('<H', colors, i * 2, value)  # red=00 F8, green=E0 07, blue=1F 00
    payload = bytes(colors) + alpha.tobytes()
    header = HEADER.pack(MAGIC, 1, 1, EDGE, EDGE, 0, len(payload), zlib.crc32(payload) & 0xffffffff,
                         app_id.encode('ascii'))
    data = header + payload
    validate_cover(data, app_id)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=local_path)
    parser.add_argument('--app-id', required=True)
    parser.add_argument('--output', type=local_path, required=True)
    args = parser.parse_args()
    data = convert_cover(args.source.read_bytes(), args.app_id)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    import json
    print(json.dumps(validate_cover(data, args.app_id), indent=2))


if __name__ == '__main__':
    main()
