"""Exercise the production updater with actual ESP image hashes and HTTP/flash faults."""
import hashlib
import argparse
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-config', type=Path, help='version header of a previously built Launcher')
parser.add_argument('--candidate', type=Path, help='real next-release OTA image instead of a synthetic image')
args = parser.parse_args()
if bool(args.baseline_config) != bool(args.candidate):
    parser.error('--baseline-config and --candidate must be used together')
sys.path.insert(0, str(root / 'tools'))
from dynamic_layout import header, definition
from firmware_manifest import resolve_profiles

build = root / 'build/launcher-update-tests'
stubs = build / 'stubs'
stubs.mkdir(parents=True, exist_ok=True)
for name in ('esp_err.h', 'esp_app_desc.h', 'esp_crt_bundle.h', 'esp_http_client.h',
             'esp_http_server.h', 'esp_image_format.h', 'esp_ota_ops.h', 'esp_partition.h',
             'esp_system.h', 'esp_timer.h', 'nvs.h', 'nvs_flash.h', 'esp_heap_caps.h',
             'esp_log.h', 'esp_flash.h', 'esp_flash_encrypt.h', 'esp_rom_md5.h',
             'esp_attr.h', 'esp_random.h', 'freertos/FreeRTOS.h', 'freertos/task.h',
             'mbedtls/sha256.h', 'mbedtls/x509.h'):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "launcher_update_stubs.h"\n', encoding='utf-8')
(stubs / 'esp_idf_version.h').write_text('#define ESP_IDF_VERSION_MAJOR 5\n', encoding='utf-8')
(stubs / 'esp_wifi.h').write_text('#include "wifi_download_stubs.h"\n', encoding='utf-8')
(stubs / 'strings.h').write_text('#pragma once\n', encoding='utf-8')
profile = resolve_profiles()['sdgoods-wifi']
(stubs / 'dynamic_layout_config.h').write_text(header(profile), encoding='utf-8')
# Minimal valid ESP image, including real segment checksum and appended SHA-256.
version_config = (args.baseline_config or root / 'factory/sdgoods-esp32s3/launcher_version.h').read_text(encoding='utf-8')
(stubs / 'launcher_version.h').write_text(version_config, encoding='utf-8')
candidate_code = int(re.search(r'^#define AGENT_LAUNCHER_VERSION_CODE (\d+)u', version_config, re.M)[1]) + 1
candidate_version = f'test.{candidate_code}'.encode('ascii')
data = bytearray(24 + 8 + 512)
data[0:2] = bytes([0xE9, 1])
struct.pack_into('<H', data, 12, 9)
data[23] = 1
struct.pack_into('<II', data, 24, 0x3C000020, 512)
struct.pack_into('<I', data, 32, 0xABCD5432)
data[0x30:0x50] = candidate_version.ljust(32, b'\0')
data[0x50:0x70] = b'SDGOODS_LAUNCHER'.ljust(32, b'\0')
struct.pack_into('<8sII32s64s48s16s', data, 0x120, b'AGSYS001', 1, 3,
                 b'agent.launcher', b'Agent_link Launcher', b'SDGOODS-ESP32S3', b'0.1.0')
struct.pack_into('<8sIII', data, 0x1D0, b'AGLCAP01', 2, 14, 1)
struct.pack_into('<8sIIII', data, 0x1E4, b'ALAB0001', definition(profile)[2], candidate_code, 25, 0)
checksum = 0xEF
for byte in data[32:]: checksum ^= byte
data.extend(b'\0' * (15 - len(data) % 16))
data.append(checksum)
data.extend(hashlib.sha256(data).digest())
(build / 'candidate.bin').write_bytes(args.candidate.read_bytes() if args.candidate else data)

env = os.environ.copy()
if os.name == 'nt':
    locator = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(locator), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    for line in subprocess.check_output(f'"{installation}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set', shell=True, text=True).splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key] = value
    compiler = shutil.which('cl.exe', path=env.get('Path') or env.get('PATH'))
else:
    compiler = 'c++'
includes = [stubs, root / 'tests', root / 'components/agent_system/include',
            root / 'components/agent_link/src/transports/wifi',
            root / 'components/agent_link/src/ota', root / 'factory/sdgoods-esp32s3',
            root / 'managed_components/espressif__cjson/cJSON']
definitions = ['AGENT_LINK_UI_DEFAULT_ENGLISH=1', 'AGENT_SYSTEM_BOARD_MODEL="SDGOODS-ESP32S3"', 'AGENT_SYSTEM_LAYOUT=3',
    'AGENT_SYSTEM_ENABLED=1', 'AGENT_SYSTEM_APP_ID="agent.launcher"', 'AGENT_SYSTEM_APP_NAME="Test"',
    'AGENT_SYSTEM_METADATA_PARTITION=\"app_meta\"', 'AGENT_SYSTEM_SLOT_COUNT=14', 'AGENT_SYSTEM_APP_MAX_BYTES=0x9F2000',
    'AGENT_SYSTEM_DYNAMIC_SLOTS=1', 'AGENT_SYSTEM_REAL_SHA256=1', 'AGENT_SYSTEM_SDGOODS_COMPAT=1',
    'AGENT_SYSTEM_DOWNLOAD_WIFI=1', 'AGENT_SYSTEM_COMMUNITY=1', 'AGENT_LAUNCHER_AB=1', 'AGENT_LAUNCHER_IMAGE=1',
    'AGENT_COMMUNITY_ORIGIN="https://sugapi.deotaland.cn"', 'AGENT_COMMUNITY_PRODUCT=1', 'AGENT_COMMUNITY_REVISION=1']
sources = [root / 'tests/launcher_update_test.cc', *[root / 'components/agent_system' / name for name in
    ('agent_system.cc', 'cover_cache.cc', 'dynamic_slots.cc', 'community_store.cc', 'launcher_update.cc')],
    root / 'managed_components/espressif__cjson/cJSON/cJSON.c']
binary = build / ('updater.exe' if os.name == 'nt' else 'updater')
if os.name == 'nt':
    command = [compiler, '/nologo', '/std:c++17', '/utf-8', '/EHsc', '/D_CRT_SECURE_NO_WARNINGS',
        *['/D' + d for d in definitions], *['/I' + str(p) for p in includes], *map(str, sources),
        '/Fo' + build.as_posix() + '/', '/Fe' + str(binary)]
else:
    command = [compiler, '-std=c++17', '-pthread', *['-D' + d for d in definitions],
        *['-I' + str(p) for p in includes], *map(str, sources), '-o', str(binary), '-lcrypto']
subprocess.run(command, cwd=build, env=env, check=True)
subprocess.run([str(binary), str(build / 'candidate.bin')], cwd=build, check=True, timeout=45)
