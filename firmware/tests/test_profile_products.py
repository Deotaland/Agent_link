"""Check platform-specific product selection before firmware compilation."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_firmware', ROOT / 'tools/build_firmware.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class ProductProfileTests(unittest.TestCase):
    def setUp(self):
        self.profiles = build.load_profiles()
        self.catalog = build.load_products(self.profiles)

    def test_registered_profiles_use_current_platform_ids(self):
        expected = {'korvo-wifi': 6, 'korvo-ble': 7, 'm5stack-stopwatch-wifi': 4,
                    'm5stack-stopwatch-ble': 4, 'waveshare-epaper-397-wifi': 5,
                    'ai-passport-wifi': 1, 'ai-passport-ble': 1, 'rorolee-ble': 2,
                    'work-badge-ble': 2, 'paper-s31-wifi': 8}
        for name, product_id in expected.items():
            with self.subTest(profile=name):
                self.assertEqual(build.product_for(self.profiles[name], self.catalog)['id'], product_id)

    def test_unregistered_wifi_cannot_reuse_an_old_platform_id(self):
        self.catalog['profiles']['paper-s31-wifi'] = None
        with self.assertRaisesRegex(ValueError, 'no registered product'):
            build.product_for(self.profiles['paper-s31-wifi'], self.catalog)
        self.assertEqual(build.product_for(self.profiles['gc2145-camera-ble'], self.catalog)['id'], 0)

    def test_wrong_platform_chip_and_ambiguous_ids_fail_validation(self):
        bad = []
        other_host = copy.deepcopy(self.catalog)
        other_host['platform'] = 'https://old-platform.example'
        bad.append(other_host)
        wrong_chip = copy.deepcopy(self.catalog)
        wrong_chip['products']['korvo-s3']['chip_type'] = 'esp32-c3'
        bad.append(wrong_chip)
        duplicate_id = copy.deepcopy(self.catalog)
        duplicate_id['products']['korvo-s3']['id'] = 1
        bad.append(duplicate_id)
        missing_mapping = copy.deepcopy(self.catalog)
        del missing_mapping['profiles']['korvo-wifi']
        bad.append(missing_mapping)
        unknown_product = copy.deepcopy(self.catalog)
        unknown_product['profiles']['korvo-wifi'] = 'missing-product'
        bad.append(unknown_product)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'products.json'
            for catalog in bad:
                with self.subTest(catalog=catalog):
                    path.write_text(json.dumps(catalog), encoding='utf-8')
                    with patch.object(build, 'PRODUCTS', path), self.assertRaises(ValueError):
                        build.load_products(self.profiles)

    def test_switching_profile_regenerates_selected_product(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'generated/platform_product.h'
            build.write_product_header(self.profiles['korvo-wifi'], output)
            self.assertIn('AGENT_LINK_PRODUCT_ID 6', output.read_text())
            build.write_product_header(self.profiles['korvo-ble'], output)
            self.assertIn('AGENT_LINK_PRODUCT_ID 7', output.read_text())


if __name__ == '__main__':
    unittest.main()
