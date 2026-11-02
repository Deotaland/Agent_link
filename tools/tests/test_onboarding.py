"""Behavior checks for new-user environment and board selection, without an SDK/device."""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


envcheck = load("check_env", "tools/check_env.py")
build = load("build_firmware", "firmware/tools/build_firmware.py")
sys.path.insert(0, str(ROOT / "skills/agent-link-flash/scripts"))
import catalog
import find_board


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        profile_dir = self.root / "firmware/profiles"
        profile_dir.mkdir(parents=True)
        (profile_dir / "profiles.json").write_text(json.dumps({"s31": {"target": "esp32s31"}}))
        self.sdk = self.root / "sdk"
        (self.sdk / "tools/cmake").mkdir(parents=True)
        (self.sdk / "tools/idf.py").touch()
        self.set_version(6, 1)
        (self.sdk / "components/soc/esp32s31").mkdir(parents=True)
        self.args = argparse.Namespace(mode="build", profile="s31", idf_path=None,
                                       search_dir=[], network=False, ports=False)

    def set_version(self, major, minor):
        (self.sdk / "tools/cmake/version.cmake").write_text(
            f"set(IDF_VERSION_MAJOR {major})\nset(IDF_VERSION_MINOR {minor})\nset(IDF_VERSION_PATCH 0)\n")

    def inspect(self, active=True, command=None, module=True):
        with patch.object(envcheck, "ROOT", self.root), \
             patch.object(envcheck, "discover", return_value=[self.sdk]), \
             patch.object(envcheck, "activation", return_value="activate this SDK"), \
             patch.dict(os.environ, {"IDF_PATH": str(self.sdk)} if active else {}, clear=True), \
             patch.object(envcheck, "run", side_effect=command or (lambda _: (True, "cmake version 3.30.0"))), \
             patch.object(envcheck.importlib.util, "find_spec", return_value=object() if module else None):
            return envcheck.inspect(self.args)

    def test_installed_inactive_is_not_ready(self):
        result = self.inspect(active=False)
        self.assertFalse(result["ready"])
        self.assertEqual(next(c for c in result["checks"] if c["name"] == "activation")["status"], "FAIL")

    def test_compatible_active_tools_are_ready(self):
        self.assertTrue(self.inspect()["ready"])

    def test_wrong_version_and_missing_target_fail(self):
        self.set_version(6, 0)
        self.assertFalse(self.inspect()["ready"])
        self.set_version(6, 1)
        (self.sdk / "components/soc/esp32s31").rmdir()
        self.assertFalse(self.inspect()["ready"])

    def test_missing_compiler_and_python_dependencies_fail(self):
        for missing in ("riscv32-esp-elf-gcc", "check-python-dependencies"):
            with self.subTest(missing=missing):
                self.assertFalse(self.inspect(command=lambda cmd: (missing not in cmd, "cmake version 3.30.0"))["ready"])

    def test_old_cmake_and_missing_flash_module_fail(self):
        self.assertFalse(self.inspect(command=lambda _: (True, "cmake version 3.10.0"))["ready"])
        self.assertFalse(self.inspect(module=False)["ready"])

    def test_flash_only_does_not_require_idf(self):
        self.args.mode = "flash"
        result = self.inspect(active=False)
        self.assertTrue(result["ready"])
        self.assertNotIn("esp-idf", [c["name"] for c in result["checks"]])

    def test_source_build_requires_an_explicit_profile(self):
        self.args.profile = None
        self.assertFalse(self.inspect()["ready"])

    def test_eim_list_and_classic_dict_registries(self):
        for name, entries in (("eim_idf.json", [{"path": str(self.sdk)}]),
                              ("esp_idf.json", {"sdk": {"path": str(self.sdk)}})):
            registry = self.root / name
            registry.write_text(json.dumps({"idfInstalled": entries}))
            with self.subTest(name=name):
                self.assertEqual(envcheck.registry_entries([self.root]), [{"path": str(self.sdk)}])
            registry.unlink()

    def test_network_error_does_not_expose_proxy_secret(self):
        with patch.object(envcheck.urllib.request, "urlopen", side_effect=OSError("https://user:secret@proxy")):
            status, detail = envcheck.probe("https://example.com")
        self.assertEqual(status, "WARN")
        self.assertNotIn("secret", detail)


class SelectionTests(unittest.TestCase):
    def test_product_note_cannot_drift_from_build_mapping(self):
        board_tool = load("board_catalog", "tools/board_catalog.py")
        entries = copy.deepcopy(catalog.load()[0])
        entry = next(e for e in entries if e["profile"] == "waveshare-epaper-397-wifi")
        entry["notes"] = "Platform product ID 999."
        self.assertTrue(any("notes product ID differs" in p for p in board_tool.repo_problems(entries)))

    def test_clean_configuration_has_no_implicit_board(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(build, "read_state", return_value={}):
            self.assertIsNone(build.selected_profile(build.load_profiles(), Path(folder) / "sdkconfig"))

    def test_empty_menu_answer_cannot_choose_a_board_on_clean_checkout(self):
        answers = iter(["", "1"])
        profiles = build.load_profiles()
        self.assertEqual(build.choose_profile(profiles, None, lambda _: next(answers)), next(iter(profiles)))

    def test_existing_explicit_selection_is_preserved(self):
        profiles = build.load_profiles()
        self.assertEqual(build.choose_profile(profiles, "paper-s31-wifi", lambda _: ""), "paper-s31-wifi")

    def test_chinese_lookup_preserves_hardware_identity(self):
        entries, _, problems = catalog.load()
        self.assertFalse(problems)
        for query, model in (("护照", "AI-PASSPORT"), ("M5秒表", "C152"), ("墨纸坊", "paper V2")):
            matches = [e for e in entries if find_board.score(e, query) > 0]
            self.assertTrue(matches)
            self.assertEqual({e["model"] for e in matches}, {model})
        candidates = {e["model"] for e in entries if find_board.score(e, "微雪") > 0}
        self.assertGreater(len(candidates), 1)

    def test_translation_allowed_only_in_aliases(self):
        board = copy.deepcopy(catalog.load()[0][0])
        board["aliases"] = ["护照"]
        self.assertFalse(catalog._check_entry(board, "test"))
        board["fw_model"] = "护照"
        self.assertTrue(catalog._check_entry(board, "test"))
        board["aliases"] = None
        self.assertTrue(catalog._check_entry(board, "test"))

    def test_json_cli_is_parseable_and_exit_code_matches_ready(self):
        proc = subprocess.run([sys.executable, str(ROOT / "tools/check_env.py"), "--mode", "flash", "--json"],
                              capture_output=True, encoding="utf-8")
        result = json.loads(proc.stdout)
        self.assertEqual(proc.returncode, 0 if result["ready"] else 1)


if __name__ == "__main__":
    unittest.main()
