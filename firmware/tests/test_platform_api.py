"""Build and run the production platform API code against a deterministic HTTP fixture.

Usage: python tests/test_platform_api.py --idf-path <existing-esp-idf>
No device, credentials or network access is used.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--idf-path', type=Path, default=os.environ.get('IDF_PATH'))
parser.add_argument('--pcm', action='store_true', help='verify the PCM16-only S31 grant path')
parser.add_argument('--c3', action='store_true', help='verify C3 Opus grants and smaller HTTP pages')
parser.add_argument('--gateway', action='store_true', help='verify claim-code and gateway error logging')
parser.add_argument('--session', action='store_true', help='verify optional MQTT in the production session loop')
parser.add_argument('--voice-entry', action='store_true', help='verify HTTP voice starts without MQTT')
args = parser.parse_args()
if not args.idf_path:
    parser.error('provide an existing ESP-IDF path for its cJSON dependency')
json_dir = args.idf_path / 'components/json/cJSON'
if not (json_dir / 'cJSON.c').is_file():
    parser.error('cJSON.c was not found in this ESP-IDF installation')
build = root / '.cache/platform-api-auth-tests'
build.mkdir(parents=True, exist_ok=True)
includes = [root / 'tests/platform_api_stubs', root / 'components/agent_link/include',
            root / 'components/agent_link/src/transports/wifi/cloud', json_dir]
sources = [root / 'tests/platform_api_auth_test.cc',
           root / 'components/agent_link/src/transports/wifi/cloud/cloud_platform.cc']
if args.gateway:
    sources = [root / 'tests/cloud_gateway_test.cc',
               root / 'components/agent_link/src/transports/wifi/cloud/cloud_api.cc']
if args.session:
    production = (root / 'components/agent_link/src/transports/wifi/cloud/cloud_session.cc').read_text(encoding='utf-8')
    fixture = (root / 'tests/cloud_session_policy_test.cc').read_text(encoding='utf-8')
    stop = production[production.index('void StopMqtt()'):production.index('\nvoid HandleMqttRefused()')]
    loop = production[production.index('Leave StayOnline()'):production.index('\n// Authenticate, then stay online.')]
    begin = production.index('    if (kMqttEnabled && !s_token)')
    allocation = production[begin:production.index('    s_token_ok = false;', begin)]
    source = build / 'cloud_session_policy_test.cc'
    source.write_text(fixture.replace('// INSERT_STOP_MQTT', stop)
                     .replace('// INSERT_STAY_ONLINE', loop)
                     .replace('// INSERT_TOKEN_ALLOCATION', allocation), encoding='utf-8')
    sources = [source]
if args.voice_entry:
    production = (root / 'components/agent_link/src/transports/wifi/transport_wifi.cpp').read_text(encoding='utf-8')
    begin = production.index('extern "C" esp_err_t agent_transport_wifi_request_realtime(void) {')
    end = production.index('extern "C" esp_err_t agent_transport_wifi_end_realtime(void) {', begin)
    fixture = (root / 'tests/realtime_entry_test.cc').read_text(encoding='utf-8')
    source = build / 'realtime_entry_test.cc'
    source.write_text(fixture.replace('// INSERT_PRODUCTION_ENTRIES', production[begin:end]), encoding='utf-8')
    sources = [source]
environment = os.environ.copy()
defines = ['CONFIG_AGENT_LINK_REALTIME_OPUS=0'] if args.pcm else []
defines.append('PLATFORM_TEST_CAPTURE_LOGS=1')
if args.c3:
    defines.append('CONFIG_IDF_TARGET_ESP32C3=1')
vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
if os.name == 'nt' and vswhere.is_file():
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    if not installation:
        raise SystemExit('An installed Visual C++ toolchain is required')
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    result = subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True)
    for line in result.splitlines():
        if '=' in line and not line.startswith('='):
            name, value = line.split('=', 1)
            environment[name] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    if not compiler:
        raise SystemExit('Visual C++ activation did not provide cl.exe')
    subprocess.run([compiler, '/nologo', '/c', '/TC', '/D_CRT_SECURE_NO_WARNINGS',
                    str(json_dir / 'cJSON.c'), '/Fo' + str(build / 'cJSON.obj')], env=environment, cwd=build, check=True)
    binary = build / 'platform_api_auth_test.exe'
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
                    *['/D' + value for value in defines],
                    *['/I' + str(p) for p in includes], *map(str, sources), str(build / 'cJSON.obj'),
                    '/Fo' + build.as_posix() + '/', '/Fe' + str(binary)], env=environment, cwd=build, check=True)
else:
    binary = build / ('platform_api_auth_test.exe' if os.name == 'nt' else 'platform_api_auth_test')
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in includes],
                    *['-D' + value for value in defines],
                    *map(str, sources), str(json_dir / 'cJSON.c'), '-o', str(binary)], cwd=build, check=True)
subprocess.run([str(binary)], check=True)
