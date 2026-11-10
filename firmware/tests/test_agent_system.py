"""Fault-inject the actual system SDK with deterministic flash/NVS/HTTP adapters."""
import os
import argparse
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--packages-report', type=Path,
                    help='HTTP download snapshot for real-binary SDK installation checks')
args = parser.parse_args()

root = Path(__file__).resolve().parents[1]
build = root / "build/agent-system-tests"
build.mkdir(parents=True, exist_ok=True)
headers = ("esp_err.h", "esp_app_desc.h", "esp_crt_bundle.h", "esp_http_client.h", "esp_http_server.h",
           "esp_image_format.h", "esp_ota_ops.h", "esp_partition.h", "esp_system.h", "nvs.h", "nvs_flash.h", "mbedtls/sha256.h", "esp_heap_caps.h", "esp_log.h")
for name in headers:
    path = build / "stubs" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "agent_system_stubs.h"\n', encoding="utf-8")
(build / "stubs/esp_idf_version.h").write_text('#define ESP_IDF_VERSION_MAJOR 5\n', encoding="utf-8")
json_dir = root / "managed_components/espressif__cjson/cJSON"
environment = os.environ.copy()
if os.name == "nt":
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    setup = Path(installation) / "VC/Auxiliary/Build/vcvarsall.bat"
    for line in subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True).splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1); environment[key] = value
    compiler = shutil.which("cl.exe", path=environment.get("Path") or environment.get("PATH"))
else:
    compiler = "c++"
includes = [build / "stubs", root / "tests", root / "components/agent_system/include",
            root / "components/agent_link/src/ota", root / "components/amoled_ui/include", root / "factory/waveshare-amoled-175/ui", json_dir]
sources = [root / "tests/agent_system_test.cc", root / "components/agent_system/agent_system.cc",
           root / "components/agent_system/cover_cache.cc",json_dir / "cJSON.c"]
for role in (("packages",) if args.packages_report else ("factory", "app", "sdgoods")):
    binary = build / (role + (".exe" if os.name == "nt" else ""))
    app_id = "agent.voice" if role == "app" else "agent.launcher"
    definitions = ['AGENT_SYSTEM_BOARD_MODEL="ESP32-S3-Touch-AMOLED-1.75-16M"', "AGENT_SYSTEM_LAYOUT=1", "AGENT_SYSTEM_ENABLED=1", f'AGENT_SYSTEM_APP_ID="{app_id}"', 'AGENT_SYSTEM_APP_NAME="Test"']
    supplied=root/'tests/local_app_store/assets/voice.raw'
    if supplied.is_file(): definitions += ['TEST_COVER_FILE="'+supplied.as_posix()+'"']
    if role == "app": definitions.append("SYSTEM_TEST_APP=1")
    selected_sources = list(sources)
    if role in ("sdgoods", "packages"):
        definitions = ['AGENT_SYSTEM_BOARD_MODEL="SDGOODS-ESP32S3"', 'AGENT_SYSTEM_LAYOUT=3',
            'AGENT_SYSTEM_SLOT_COUNT=2', 'AGENT_SYSTEM_SDGOODS_COMPAT=1',
            'AGENT_LINK_NVS_PARTITION="agent_nvs"', 'AGENT_SYSTEM_APP_MAX_BYTES=0x4F2000',
            'AGENT_SYSTEM_APP_ID="agent.launcher"', 'AGENT_SYSTEM_APP_NAME="Sugarat Button"']
        selected_sources[0] = root / "tests/agent_system_sdgoods_test.cc"
        if role == 'packages':
            definitions.append('AGENT_SYSTEM_REAL_SHA256=1')
            selected_sources[0] = root / 'tests/agent_system_packages_test.cc'
    if os.name == "nt":
        command = [compiler, "/nologo", "/std:c++17", "/constexpr:steps10000000", "/utf-8", "/EHsc", "/D_CRT_SECURE_NO_WARNINGS",
            *["/D" + d for d in definitions], *["/I" + str(p) for p in includes], *map(str, selected_sources),
            "/Fo" + build.as_posix() + "/", "/Fe" + str(binary)]
    else:
        command = [compiler, "-std=c++17", "-pthread", *["-D" + d for d in definitions],
            *["-I" + str(p) for p in includes], *map(str, selected_sources), "-o", str(binary)]
        if role == 'packages': command.append('-lcrypto')
    subprocess.run(command, env=environment, cwd=build, check=True)
    subprocess.run([str(binary), *([str(args.packages_report.resolve())] if args.packages_report else [])],
                   cwd=build,check=True)
