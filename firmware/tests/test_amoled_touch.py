"""Exercise the actual CST9217 production driver."""
import json
import sys
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
build = root / "build/amoled-touch-tests"
build.mkdir(parents=True, exist_ok=True)
includes = [root / "tests/amoled_touch_stubs", root / "tests/stopwatch_driver_stubs",
            root / "tests/platform_api_stubs", root / "boards/common"]
sources = [root / "tests/amoled_touch_test.cc", root / "boards/common/cst9217_touch.cc"]
environment = os.environ.copy()
if os.name == "nt":
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    setup = Path(installation) / "VC/Auxiliary/Build/vcvarsall.bat"
    result = subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True)
    for line in result.splitlines():
        if "=" in line and not line.startswith("="):
            name, value = line.split("=", 1)
            environment[name] = value
    compiler = shutil.which("cl.exe", path=environment.get("Path") or environment.get("PATH"))
    binary = build / "amoled_touch_test.exe"
    command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/utf-8", "/D_CRT_SECURE_NO_WARNINGS",
               *["/I" + str(p) for p in includes], *map(str, sources),
               "/Fo" + build.as_posix() + "/", "/Fe" + str(binary)]
else:
    binary = build / "amoled_touch_test"
    command = ["c++", "-std=c++17", "-Wall", "-Wextra", *["-I" + str(p) for p in includes],
               *map(str, sources), "-o", str(binary)]
subprocess.run(command, env=environment, cwd=build, check=True)
subprocess.run([str(binary)], check=True)
profiles = __import__('firmware_manifest').resolve_profiles(root)
for name in ("waveshare-amoled-175c-wifi", "waveshare-amoled-175-wifi"):
    profile = profiles[name]
    assert "cjk_font.cc" in profile["drivers"]
    assert profile["app_shared"] == "amoled-wifi"
    assert (root / "components" / profile["ui_component"] / "amoled_fonts.cc").is_file()
    assert profile["flash_assets"] == [{"partition": "font_cjk", "file": "boards/common/fonts/cjk_font.bin"}]
print("The AMOLED profile includes the shared Chinese font and resident font loader.")
