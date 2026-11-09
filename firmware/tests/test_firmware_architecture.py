"""Exercise production manifest geometry and managed USB writes without a device."""
import binascii
import copy
import json
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_firmware as build
from firmware_manifest import flash_plan, partitions, resolve_profiles
from firmware_image import DESCRIPTOR, boot_data, partition_records
from flash_firmware import check_device, compatible_identity, preflight


def image(app_id="agent.voice", model="ESP32-S3-Touch-AMOLED-1.75-16M"):
    value = bytearray(0x200)
    value[0] = 0xE9
    value[12] = 9
    value[0x30:0x36] = b"2.0.0\0"
    value[0x120:0x120 + DESCRIPTOR.size] = DESCRIPTOR.pack(
        b"AGSYS001", 1, 1, app_id.encode(), b"Example", model.encode(), b"0.1.0")
    return bytes(value)


def table(offset=0x10000):
    entry = struct.pack("<HBBII16sI", 0x50AA, 0, 0, offset, 0x300000, b"factory", 0)
    return entry + b"\xff" * (0x1000 - len(entry))


class FirmwareArchitectureTests(unittest.TestCase):
    def setUp(self):
        self.profiles = resolve_profiles()
        self.profile = self.profiles["waveshare-amoled-175-wifi"]
        self.plan = flash_plan(self.profile, "voice")
        self.folder = ROOT / "build/architecture-tests"
        self.folder.mkdir(parents=True, exist_ok=True)

    def test_board_layout_and_application_have_separate_owners(self):
        selections = json.loads((ROOT / "profiles/profiles.json").read_text(encoding="utf-8"))
        for name, profile in self.profiles.items():
            self.assertNotIn("target", selections[name])
            self.assertNotIn("drivers", selections[name])
            self.assertNotIn("hardware_sources", selections[name])
            board = json.loads((ROOT / "boards" / profile["board"] / "config.json").read_text(encoding="utf-8"))
            self.assertEqual(profile["target"], board["target"])
            if profile["layout_info"]["mode"] == "single":
                self.assertIsNone(build.system_images(profile))
                self.assertEqual(flash_plan(profile, "standard")["partition"], "ota_0")
        factory = json.loads((ROOT / "factory/waveshare-amoled-175/manifest.json").read_text(encoding="utf-8"))
        self.assertNotIn("images", factory)
        self.assertNotIn("offset", factory["image"])

    def test_application_and_factory_have_distinct_destinations(self):
        self.assertEqual((self.plan["partition"], self.plan["app_offset"]), ("ota_0", 0x310000))
        factory = flash_plan(self.profile, "factory")
        self.assertEqual((factory["partition"], factory["app_offset"]), ("factory", 0x10000))
        self.assertEqual(self.plan["app_max_bytes"], 0x2F2000)

    def test_same_board_and_backend_can_select_multiple_application_profiles(self):
        # Use an existing source directory as a metadata fixture only. This does
        # not register or build an additional hardware/firmware combination.
        profiles = copy.deepcopy(self.profiles)
        alternate = copy.deepcopy(self.profile)
        alternate.update(name="amoled-alternate", app="waveshare-amoled-175C-wifi",
                         application=profiles["waveshare-amoled-175c-wifi"]["application"])
        defaults = self.folder / "alternate.defaults"
        defaults.write_text("CONFIG_AGENT_LINK_PROFILE_AMOLED_ALTERNATE=y\n", encoding="utf-8")
        self.addCleanup(lambda: defaults.unlink(missing_ok=True))
        alternate["defaults"][-1] = defaults.relative_to(ROOT).as_posix()
        profiles[alternate["name"]] = alternate
        with patch.object(build, "resolve_profiles", return_value=profiles), patch.object(build, "load_products"):
            loaded = build.load_profiles()
        self.assertEqual(loaded["amoled-alternate"]["board"], self.profile["board"])
        self.assertIn('AGENT_LINK_FIRMWARE_PROFILE = "amoled-alternate"', build.factory_kconfig(loaded))

    def test_partition_geometry_rejects_overlap_and_accepts_idf_alignment(self):
        path = self.folder / "geometry.csv"
        self.addCleanup(lambda: path.unlink(missing_ok=True))
        path.write_text("nvs,data,nvs,,24K\napp,app,ota_0,,1M\n", encoding="utf-8")
        entries = partitions(path)
        self.assertEqual(entries["nvs"]["offset"], 0x9000)
        self.assertEqual(entries["app"]["offset"], 0x10000)
        path.write_text("a,data,nvs,0x9000,0x2000\nb,data,nvs,0xA000,0x1000\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            partitions(path)

    def test_boot_data_selects_ota_slot_with_idf_crc_and_new_state(self):
        for slot in range(3):
            data = boot_data(slot)
            self.assertEqual(len(data), 0x2000)
            self.assertEqual(struct.unpack_from("<I", data)[0], slot + 1)
            self.assertEqual(struct.unpack_from("<I", data, 24)[0], 0)
            self.assertEqual(struct.unpack_from("<I", data, 28)[0],
                             binascii.crc32(struct.pack("<I", slot + 1), 0xFFFFFFFF) & 0xFFFFFFFF)
            self.assertEqual(data[0x1000:], b"\xff" * 0x1000)
        self.assertEqual(boot_data(), b"\xff" * 0x2000)

    def test_managed_write_requires_matching_layout_and_real_launcher(self):
        check_device(self.plan, table(), table(), image("agent.launcher"), b"\xff" * 0x200)
        check_device(self.plan, table(), table(), image("agent.launcher"), image())
        for launcher in (image(), image("agent.launcher", "another-board"), b"\xff" * 0x200):
            with self.subTest(launcher=launcher[:8]), self.assertRaises(ValueError):
                check_device(self.plan, table(), table(), launcher, image())
        with self.assertRaisesRegex(ValueError, "layout differs"):
            check_device(self.plan, table(0x20000), table(), image("agent.launcher"), image())
        for target in (image("agent.clock"), bytes(0x200), image(model="another-board")):
            with self.assertRaisesRegex(ValueError, "another or unrecognized"):
                check_device(self.plan, table(), table(), image("agent.launcher"), target)

    def test_image_chip_identity_follows_board_target(self):
        for target, chip_id in (("esp32c3", 5), ("esp32s3", 9), ("esp32s31", 32)):
            plan = dict(self.plan, chip=target)
            data = bytearray(image())
            data[12] = chip_id
            compatible_identity(data, plan, "agent.voice")
            data[12] = 99
            with self.assertRaisesRegex(ValueError, "identity"):
                compatible_identity(data, plan, "agent.voice")

    def test_wrong_flash_address_is_rejected_before_any_serial_read(self):
        with self.assertRaisesRegex(ValueError, "destination"):
            preflight(self.plan, ["--chip", "esp32s3", "-p", "COM_TEST", "write-flash",
                                  "0x10000", "agent_link.bin"], self.folder)

    def test_real_preflight_reads_before_write_and_does_not_modify_arguments(self):
        (self.folder / "agent_link.bin").write_bytes(image())
        (self.folder / "partition_table").mkdir(exist_ok=True)
        (self.folder / "partition_table/partition-table.bin").write_bytes(table())
        calls = []
        values = {0x8000: table(), 0x10000: image("agent.launcher"), 0x310000: image()}

        def read(command, **kwargs):
            calls.append(command)
            address = int(command[-3], 0)
            Path(command[-1]).write_bytes(values[address])

        arguments = ["--chip", "esp32s3", "-p", "COM_TEST", "write-flash", "0x310000", "agent_link.bin"]
        original = arguments[:]
        preflight(self.plan, arguments, self.folder, run=read)
        self.assertEqual(arguments, original)
        self.assertEqual(len(calls), 3)
        self.assertTrue(all("read-flash" in call and "write-flash" not in call for call in calls))
        values[0x310000] = image("agent.game")
        with self.assertRaisesRegex(ValueError, "another or unrecognized"):
            preflight(self.plan, arguments, self.folder, run=read)

    def test_config_migration_preserves_local_logging_and_build_target(self):
        config = self.folder / "sdkconfig"
        config.write_text('CONFIG_LOG_DEFAULT_LEVEL=4\nCONFIG_LOG_MAXIMUM_LEVEL=4\nCONFIG_AGENT_LINK_BUILD_FACTORY=y\n', encoding="utf-8")
        state = {"profile": self.profile["name"], "schema": 3}
        with patch.object(build, "STATE", self.folder / "state.json"), patch.object(build, "read_state", return_value=state):
            build.prepare_config(self.profile["name"], self.profile, config)
        values = build.settings(config)
        self.assertEqual(values["CONFIG_LOG_DEFAULT_LEVEL"], "4")
        self.assertEqual(values["CONFIG_LOG_MAXIMUM_LEVEL"], "4")
        self.assertEqual(values["CONFIG_AGENT_LINK_BUILD_FACTORY"], "y")


if __name__ == "__main__":
    unittest.main()
