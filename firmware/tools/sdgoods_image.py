"""Read-only validation of SDGOODS pure ESP32-S3 images and slot covers."""
import binascii
import hashlib
from pathlib import Path
import re
import struct

from firmware_image import string

COVER = struct.Struct('<8sBBHHHII40s')
COVER_BYTES = 64 + 132 * 132 * 3


def verify_application(path, maximum):
    data = Path(path).read_bytes()
    if len(data) < 512 or len(data) > maximum:
        raise ValueError('Application size exceeds the selected OTA slot or image is truncated')
    if data[0] != 0xE9 or struct.unpack_from('<H', data, 12)[0] != 9:
        raise ValueError('Expected a pure ESP32-S3 application image')
    if struct.unpack_from('<I', data, 0x20)[0] != 0xABCD5432 or data[23] != 1:
        raise ValueError('Expected an IDF application with appended SHA-256; merged/bootloader images are not accepted')
    count = data[1]
    if not 1 <= count <= 16:
        raise ValueError('Invalid ESP segment count')
    offset, checksum = 24, 0xEF
    for _ in range(count):
        if offset + 8 > len(data):
            raise ValueError('Truncated segment header')
        length = struct.unpack_from('<I', data, offset + 4)[0]
        offset += 8
        if offset + length > len(data):
            raise ValueError('Truncated segment data')
        for value in data[offset:offset + length]:
            checksum ^= value
        offset += length
    checksum_offset = offset | 15
    hash_offset = checksum_offset + 1
    if hash_offset + 32 > len(data) or data[checksum_offset] != checksum:
        raise ValueError('Invalid ESP image checksum')
    if hashlib.sha256(data[:hash_offset]).digest() != data[hash_offset:hash_offset + 32]:
        raise ValueError('Invalid ESP image SHA-256')
    # Reject concatenated/merged payloads. Ordinary FF flash-alignment padding is allowed.
    if any(value != 0xFF for value in data[hash_offset + 32:]):
        raise ValueError('Unexpected data after the application image')
    return dict(project_name=string(data[0x50:0x70]), version=string(data[0x30:0x50]),
                size=len(data), sha256=hashlib.sha256(data).hexdigest())


def image_info(path, maximum):
    value = verify_application(path, maximum)
    with Path(path).open('rb') as stream:
        stream.seek(0x120)
        if stream.read(5) == b'AGSYS':
            raise ValueError('Use the Agent Link image workflow for native applications')
    app_id, version = value['project_name'], value['version']
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}', app_id) or app_id in {'.', '..', 'SDGOODS_LAUNCHER'} or not version:
        raise ValueError('Invalid application identity, or a launcher instead of an application')
    return dict(app_id=app_id, name=app_id, version=version, size=value['size'],
                sha256=value['sha256'], image_format='sdgoods')


def convert_cover(data, app_id):
    if len(data) != COVER_BYTES:
        raise ValueError('Invalid SDGOODS cover size')
    magic, version, fmt, width, height, reserved, length, crc, old_id = COVER.unpack_from(data)
    if (magic, version, fmt, width, height, reserved, length) != (b'SDGCOVER', 1, 1, 132, 132, 0, COVER_BYTES - 64):
        raise ValueError('Invalid SDGOODS cover header')
    string(old_id)
    pixels = bytearray(data[64:])
    if binascii.crc32(pixels) & 0xFFFFFFFF != crc:
        raise ValueError('Invalid SDGOODS cover CRC')
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}', app_id) or app_id in {'.', '..'}:
        raise ValueError('Invalid cover application identity')
    rgb_end = 132 * 132 * 2
    pixels[:rgb_end:2], pixels[1:rgb_end:2] = pixels[1:rgb_end:2], pixels[:rgb_end:2]
    return COVER.pack(b'ALCOVER1', 1, 1, 132, 132, 0, len(pixels),
                      binascii.crc32(pixels) & 0xFFFFFFFF, app_id.encode()) + pixels
