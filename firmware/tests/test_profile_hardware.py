"""Check profile ownership and the C3 flash-choice validation regression."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_firmware', ROOT / 'tools/build_firmware.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class ProfileHardwareTests(unittest.TestCase):
    def setUp(self):
        self.profiles = build.load_profiles()

    def test_c3_generated_flash_choice_passes_validation(self):
        profile = self.profiles['ai-passport-ble']
        actual = build.expected_settings(profile)
        actual.update(CONFIG_IDF_TARGET='"esp32c3"',
                      CONFIG_ESPTOOLPY_FLASHSIZE_16MB='n',
                      CONFIG_ESPTOOLPY_FLASHSIZE_8MB='y')
        self.assertEqual(build.validate_settings(profile, actual), [])
        actual['CONFIG_ESPTOOLPY_FLASHSIZE_8MB'] = 'n'
        self.assertIn('CONFIG_ESPTOOLPY_FLASHSIZE_8MB does not match the profile',
                      build.validate_settings(profile, actual))

    def test_existing_board_memory_and_routing_are_preserved(self):
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                values = build.expected_settings(profile)
                if profile['target'] == 'esp32c3':
                    self.assertEqual(values['CONFIG_ESPTOOLPY_FLASHSIZE_8MB'], 'y')
                    self.assertNotIn('CONFIG_ESPTOOLPY_FLASHSIZE_16MB', values)
                    self.assertNotEqual(values.get('CONFIG_SPIRAM'), 'y')
                    self.assertEqual(values['CONFIG_PARTITION_TABLE_CUSTOM_FILENAME'],
                                     '"partitions_esp32c3.csv"')
                    self.assertEqual(values['CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG'], 'y')
                else:
                    self.assertEqual(values['CONFIG_ESPTOOLPY_FLASHSIZE_16MB'], 'y')
                    self.assertEqual(values['CONFIG_SPIRAM'], 'y')
                    self.assertEqual(values['CONFIG_SPIRAM_IGNORE_NOTFOUND'], 'n')
                    self.assertEqual(values['CONFIG_PARTITION_TABLE_CUSTOM_FILENAME'], '"partitions.csv"')
                    if profile['target'] == 'esp32s3':
                        self.assertEqual(values['CONFIG_SPIRAM_MODE_OCT'], 'y')
                        self.assertEqual(values['CONFIG_SPIRAM_SPEED_80M'], 'y')
                    else:
                        self.assertEqual(values['CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_320'], 'y')

    def test_inherited_hardware_setting_is_rejected(self):
        original = build.settings
        for key in ('CONFIG_ESPTOOLPY_FLASHSIZE_16MB', 'CONFIG_SPIRAM',
                    'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG', 'CONFIG_RTC_CLK_SRC_EXT_CRYS'):
            with self.subTest(setting=key):
                def misplaced(path):
                    values = original(path)
                    if path == ROOT / 'sdkconfig.defaults':
                        values[key] = 'y'
                    return values
                with patch.object(build, 'settings', side_effect=misplaced):
                    with self.assertRaisesRegex(ValueError, 'hardware settings belong'):
                        build.load_profiles()

    def test_missing_or_conflicting_flash_capacity_is_rejected(self):
        profile = self.profiles['ai-passport-ble']
        for sizes in ({}, {'CONFIG_ESPTOOLPY_FLASHSIZE_8MB': 'y',
                           'CONFIG_ESPTOOLPY_FLASHSIZE_16MB': 'y'}):
            values = build.expected_settings(profile)
            values.pop('CONFIG_ESPTOOLPY_FLASHSIZE_8MB')
            values.update(sizes)
            with patch.object(build, 'expected_settings', return_value=values):
                with self.assertRaisesRegex(ValueError, 'exactly one flash capacity'):
                    build.check_hardware_defaults(profile)


if __name__ == '__main__':
    unittest.main()
