"""Use ESP-IDF's actual Kconfig parser to exercise the two visible selectors."""
from pathlib import Path
import sys
import unittest

from esp_kconfiglib import Kconfig

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_firmware as build


class FirmwareMenuTests(unittest.TestCase):
    def setUp(self):
        self.folder = ROOT / "build/menu-tests"
        self.folder.mkdir(parents=True, exist_ok=True)
        wrapper = self.folder / "Kconfig"
        text = ['mainmenu "Firmware menu regression"',
                'config IDF_TARGET', '    string "Chip"', '    default "esp32s3"',
                'config AGENT_LINK_SAVED_APPLICATION_PROFILE', '    string',
                '    default "waveshare-epaper-397-wifi"']
        for chip in ("esp32s3", "esp32c3", "esp32s31"):
            text += ['config IDF_TARGET_' + chip.upper(), '    bool', f'    default y if IDF_TARGET = "{chip}"']
        for filename in ("Kconfig.build_target", "Kconfig.profiles"):
            text.append(f'source "{(ROOT / "main" / filename).as_posix()}"')
        text.append(f'source "{(ROOT / "components/agent_link/Kconfig").as_posix()}"')
        wrapper.write_text("\n".join(text) + "\n", encoding="utf-8")
        self.kconfig = Kconfig(str(wrapper), warn=False)
        self.profiles = build.load_profiles()

    def save(self, name):
        path = self.folder / (name + ".sdkconfig")
        self.kconfig.write_config(str(path))
        return path

    def test_application_has_current_chip_profiles_factory_has_only_bound_boards(self):
        k = self.kconfig
        k.syms["AGENT_LINK_PROFILE_WAVESHARE_EPAPER_397_WIFI"].set_value(2)
        app = k.named_choices["AGENT_LINK_FIRMWARE_SELECTION"]
        self.assertTrue(app.visibility)
        self.assertGreater(len([s for s in app.syms if s.visibility]), 1)
        self.assertIn(2, k.syms["AGENT_LINK_BUILD_FACTORY"].assignable)
        k.syms["AGENT_LINK_BUILD_FACTORY"].set_value(2)
        factory = k.named_choices["AGENT_LINK_FACTORY_BOARD_SELECTION"]
        self.assertFalse(app.visibility)
        options = [s for s in factory.syms if s.visibility]
        self.assertEqual([s.name for s in options], ["AGENT_LINK_FACTORY_BOARD_WAVESHARE_AMOLED_175"])
        self.assertEqual(options[0].nodes[0].prompt[0], "Waveshare ESP32-S3-Touch-AMOLED-1.75 (non-C)")
        path = self.save("factory")
        self.assertEqual(build.selected_profile(self.profiles, path), "waveshare-amoled-175-wifi")
        self.assertEqual(k.syms["AGENT_LINK_FIRMWARE_PROFILE"].str_value, "waveshare-amoled-175-wifi")
        self.assertEqual(k.syms["AGENT_LINK_MANAGED_APPS"].str_value, "y")
        self.assertEqual(k.syms["AGENT_LINK_OTA_ENABLE"].str_value, "n")

    def test_switch_back_and_saved_factory_reload_restore_application_selection(self):
        k = self.kconfig
        k.syms["AGENT_LINK_PROFILE_WAVESHARE_EPAPER_397_WIFI"].set_value(2)
        k.syms["AGENT_LINK_BUILD_FACTORY"].set_value(2)
        saved = self.save("factory-reload")
        k.load_config(str(saved))
        k.syms["AGENT_LINK_BUILD_APPLICATION"].set_value(2)
        self.assertFalse(k.named_choices["AGENT_LINK_FACTORY_BOARD_SELECTION"].visibility)
        self.assertEqual(k.syms["AGENT_LINK_FIRMWARE_PROFILE"].str_value, "waveshare-epaper-397-wifi")
        self.assertEqual(build.selected_profile(self.profiles, self.save("application")), "waveshare-epaper-397-wifi")

    def test_unsupported_chips_offer_no_factory(self):
        k = self.kconfig
        for chip in ("esp32c3", "esp32s31"):
            with self.subTest(chip=chip):
                k.syms["IDF_TARGET"].set_value(chip)
                self.assertNotIn(2, k.syms["AGENT_LINK_BUILD_FACTORY"].assignable)
                self.assertFalse(k.named_choices["AGENT_LINK_FACTORY_BOARD_SELECTION"].visibility)
                self.assertTrue(k.named_choices["AGENT_LINK_FIRMWARE_SELECTION"].visibility)


if __name__ == "__main__":
    unittest.main()
