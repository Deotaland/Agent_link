"""Exercise production BLE admission, OTA checks, manifest frames and recording pagination.

No device, network, credentials or firmware flashing. Uses the installed host C++ compiler.
"""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/control-safety-tests'
build.mkdir(parents=True, exist_ok=True)
core = root / 'components/agent_link/src/core'
source = (core / 'agent_link.cpp').read_text(encoding='utf-8')
ota = (root / 'components/agent_link/src/ota/ota_service.cpp').read_text(encoding='utf-8')
ble = (root / 'components/agent_link/src/transports/ble/transport_ble.cpp').read_text(encoding='utf-8')


def section(text, start, stop):
    begin = text.index(start)
    return text[begin:text.index(stop, begin)]


fixture = (root / 'tests/control_safety_test.cc').read_text(encoding='utf-8')
parts = {
    'MANIFEST': section(source, 'bool SendManifest(bool force)', '\n// BLE: fired'),
    'REPLY': section(source, 'uint32_t agent_link_command_generation(', '\nesp_err_t agent_link_register_io('),
    'PEER_POLICY': section(ble, '    bool KnownOrFirstPeer(', '\n    // Set once nimble_port_init'),
    'SECURITY_EVENTS': section(ble, '        case BLE_GAP_EVENT_ENC_CHANGE:', '        case BLE_GAP_EVENT_SUBSCRIBE:'),
    'GATT_ACCESS': section(ble, '    int GattAccess(', '\n    // Service C (device control)'),
    'DEFERRED_REPLY': section(ble, '    struct DeferredReply {', '\n    // Data plane: VOICE over GATT'),
    'OTA_START': section(ota, 'bool Start(uint32_t fw_size,', '\nsize_t FeedData('),
}
for marker, production in parts.items():
    fixture = fixture.replace('// INSERT_' + marker, production)
generated = build / 'control.cc'
generated.write_text(fixture, encoding='utf-8')
includes = [root / 'tests/control_safety_stubs', root / 'tests/platform_api_stubs',
            root / 'components/agent_link/include', core,
            root / 'components/agent_link/src/ota', root / 'apps/korvo-ble', root / 'apps/work-badge']
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    for line in subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True,
                                         encoding='utf-8', errors='replace').splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            environment[key] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
else:
    compiler = shutil.which('c++')
for name, sources in [('control', [generated, core / 'protocol.cpp']),
                      ('recordings', [root / 'tests/recordings_test.cc', root / 'apps/korvo-ble/recordings.cc']),
                      ('badge', [root / 'tests/badge_storage_test.cc', root / 'apps/work-badge/badge_data.cc'])]:
    binary = build / (name + ('.exe' if os.name == 'nt' else ''))
    if os.name == 'nt':
        command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4',
                   '/D_CRT_SECURE_NO_WARNINGS', '/DCONFIG_AGENT_LINK_OTA_REQUIRE_MODEL_MATCH=1',
                   *['/I' + str(p) for p in includes], *map(str, sources),
                   '/Fo' + build.as_posix() + '/', '/Fe' + str(binary)]
    else:
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-DCONFIG_AGENT_LINK_OTA_REQUIRE_MODEL_MATCH=1',
                   *['-I' + str(p) for p in includes], *map(str, sources), '-o', str(binary)]
    subprocess.run(command, env=environment, cwd=build, check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
