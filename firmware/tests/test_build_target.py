"""Check Menuconfig/CLI image selection without building or touching real settings."""
import copy
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_firmware", ROOT / "tools/build_firmware.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildTargetTests(unittest.TestCase):
    def setUp(self):
        self.profiles = build.load_profiles()
        self.profile = self.profiles["waveshare-amoled-175-wifi"]
        folder = ROOT / "build/build-target-tests"
        folder.mkdir(parents=True, exist_ok=True)
        self.config = folder / (self._testMethodName + ".sdkconfig")
        self.addCleanup(lambda: self.config.unlink(missing_ok=True))
        self.original = ("# Local diagnostic settings\nCONFIG_LOG_DEFAULT_LEVEL_INFO=y\n"
                         "CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y\nCONFIG_IDF_TARGET=\"esp32s3\"\n")
        self.config.write_text(self.original, encoding="utf-8")

    def test_fresh_configuration_defaults_to_profile_application(self):
        self.assertEqual(build.selected_image_role(self.profile, self.config), "voice")
        for profile in self.profiles.values():
            bundle = build.system_images(profile)
            self.assertEqual(build.selected_image_role(profile, self.config),
                             build.default_image_role(profile, bundle))

    def test_menuconfig_selection_round_trips_without_losing_local_options(self):
        for role, symbol in (("factory", "CONFIG_AGENT_LINK_BUILD_FACTORY"),
                             ("voice", "CONFIG_AGENT_LINK_BUILD_APPLICATION"),
                             ("factory", "CONFIG_AGENT_LINK_BUILD_FACTORY"),
                             ("voice", "CONFIG_AGENT_LINK_BUILD_APPLICATION")):
            with self.subTest(role=role):
                build.select_image_role(self.profile, self.config, role)
                self.assertEqual(build.selected_image_role(self.profile, self.config), role)
                self.assertTrue(self.config.read_text(encoding="utf-8").startswith(self.original))
                selected = [key for key in build.BUILD_TARGET_SYMBOLS if build.settings(self.config).get(key) == "y"]
                self.assertEqual(selected, [symbol])

    def test_old_cmake_environment_cannot_override_menuconfig(self):
        build.select_image_role(self.profile, self.config, "voice")
        result = subprocess.run([sys.executable, str(ROOT / "tools/build_firmware.py"),
            self.profile["name"], "--resolve-image-role", "--sdkconfig", str(self.config)],
            env=dict(os.environ, AGENT_LINK_IMAGE_ROLE="factory"), capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), "voice")

    def test_unsupported_build_targets_are_rejected(self):
        profile = self.profiles["korvo-wifi"]
        for role in ("factory", "unknown"):
            with self.subTest(role=role), self.assertRaisesRegex(ValueError, "board/application combination"):
                build.select_image_role(profile, self.config, role)
        self.config.write_text("CONFIG_AGENT_LINK_BUILD_FACTORY=y\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "board/application combination"):
            build.selected_image_role(profile, self.config)
        with self.assertRaisesRegex(ValueError, "board/application combination"):
            build.select_image_role(self.profile, self.config, "info")

    def test_factory_binding_cannot_select_another_board(self):
        profile = dict(self.profile, factory="waveshare-amoled-175C")
        with self.assertRaisesRegex(ValueError, "belong to its selected board"):
            build.selected_image_role(profile, self.config)

    def test_manifest_application_must_match_the_profile(self):
        bundle = copy.deepcopy(build.system_images(self.profile))
        bundle["images"]["voice"]["path"] = "apps/missing-application"
        with self.assertRaisesRegex(ValueError, "exactly one matching profile application"):
            build.default_image_role(self.profile, bundle)

    def test_conflicting_targets_are_rejected(self):
        self.config.write_text("CONFIG_AGENT_LINK_BUILD_FACTORY=y\nCONFIG_AGENT_LINK_BUILD_APPLICATION=y\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Multiple firmware build targets"):
            build.selected_image_role(self.profile, self.config)

    def test_factory_board_is_independent_of_the_application_profile(self):
        application = self.profiles["waveshare-epaper-397-wifi"]
        self.config.write_text('CONFIG_IDF_TARGET="esp32s3"\n' + build.profile_symbol(application["name"]) + '=y\n', encoding="utf-8")
        build.select_image_role(self.profile, self.config, "factory")
        self.assertEqual(build.selected_profile(self.profiles, self.config), self.profile["name"])
        self.assertEqual(build.settings(self.config)[build.profile_symbol(application["name"])], "y")
        build.select_image_role(application, self.config, "standard")
        self.assertEqual(build.selected_profile(self.profiles, self.config), application["name"])

    def test_unregistered_factory_board_cannot_silently_fall_back(self):
        self.config.write_text('CONFIG_IDF_TARGET="esp32s3"\nCONFIG_AGENT_LINK_BUILD_FACTORY=y\n'
                               'CONFIG_AGENT_LINK_FACTORY_BOARD_UNSUPPORTED=y\n', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Unsupported Factory board"):
            build.selected_profile(self.profiles, self.config)


if __name__ == "__main__":
    unittest.main()
