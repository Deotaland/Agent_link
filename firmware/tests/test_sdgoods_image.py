"""Validate real ESP checksums/hashes and SDGOODS catalog/cover conversion."""
import binascii
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from sdgoods_image import image_info, convert_cover, COVER, COVER_BYTES


def image(name='CLOCK', chip=9):
    header = bytearray(24)
    header[0:2] = bytes([0xE9, 1])
    struct.pack_into('<H', header, 12, chip)
    header[23] = 1
    payload = bytearray(512)
    struct.pack_into('<I', payload, 0, 0xABCD5432)
    payload[16:22] = b'1.2.3\0'
    payload[48:48+len(name)+1] = name.encode() + b'\0'
    checksum = 0xEF
    for value in payload:
        checksum ^= value
    data = header + struct.pack('<II', 0x3C000020, len(payload)) + payload
    data.extend(bytes(15 - len(data) % 16) + bytes([checksum]))
    return data + hashlib.sha256(data).digest()


class SdgoodsImages(unittest.TestCase):
    def setUp(self):
        build = Path(__file__).resolve().parents[1] / 'build/agent-system-tests'
        build.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=build)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'clock.bin'
        self.app.write_bytes(image())

    def test_backend_metadata_preserves_original_image(self):
        before = self.app.read_bytes()
        entry = image_info(self.app, 0x4F2000)
        self.assertEqual(entry['image_format'], 'sdgoods')
        self.assertEqual(entry['app_id'], 'CLOCK')
        self.assertEqual(entry['version'], '1.2.3')
        self.assertEqual(entry['sha256'], hashlib.sha256(before).hexdigest())
        self.assertEqual(self.app.read_bytes(), before)

    def test_bad_chip_size_checksum_hash_and_launcher_fail(self):
        for bad in (image(chip=5), image('SDGOODS_LAUNCHER'), image()[:-1]):
            self.app.write_bytes(bad)
            with self.assertRaises(ValueError):
                image_info(self.app, 1024)
        for offset in (80, len(image())-1):
            bad = image(); bad[offset] ^= 1
            self.app.write_bytes(bad)
            with self.assertRaises(ValueError):
                image_info(self.app, 1024)
        self.app.write_bytes(image())
        with self.assertRaises(ValueError):
            image_info(self.app, 500)

    def test_cover_red_and_alpha_are_preserved(self):
        pixels = bytearray(COVER_BYTES - 64)
        pixels[0:2] = b'\xF8\x00'
        pixels[132*132*2] = 167
        original = COVER.pack(b'SDGCOVER', 1, 1, 132, 132, 0, len(pixels),
                              binascii.crc32(pixels) & 0xFFFFFFFF, b'platform-id') + pixels
        converted = convert_cover(original, 'CLOCK')
        self.assertEqual(converted[:8], b'ALCOVER1')
        self.assertEqual(converted[64:66], b'\x00\xF8')
        self.assertEqual(converted[64+132*132*2], 167)
        self.assertEqual(COVER.unpack_from(converted)[-1].split(b'\0')[0], b'CLOCK')
        self.assertEqual(COVER.unpack_from(converted)[-2], binascii.crc32(converted[64:]) & 0xFFFFFFFF)
        damaged = bytearray(original); damaged[-1] ^= 1
        with self.assertRaises(ValueError):
            convert_cover(damaged, 'CLOCK')


if __name__ == '__main__':
    unittest.main()
