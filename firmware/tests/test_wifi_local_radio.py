"""Run production cloud-pause and local-radio methods with controlled workers."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
wifi = root / 'components/agent_link/src/transports/wifi'
session = (wifi / 'cloud/cloud_session.cc').read_text(encoding='utf-8')
transport = (wifi / 'transport_wifi.cpp').read_text(encoding='utf-8')
realtime = (wifi / 'cloud/cloud_realtime.cc').read_text(encoding='utf-8')
fixture = (root / 'tests/wifi_local_radio_fixture.cc').read_text(encoding='utf-8')
sections = {
    'SESSION_WAIT': session[session.index('bool NetReady()'):session.index('// The key was revoked')],
    'SESSION_API': session[session.index('esp_err_t cloud_session_pause('):session.index('void cloud_session_get_status')],
    'LOCAL_RADIO': transport[transport.index('esp_err_t agent_wifi_local_end()'):transport.index('extern "C" esp_err_t agent_link_wifi_setup')],
    'WIFI_INFO': transport[transport.index('extern "C" esp_err_t agent_link_wifi_info('):transport.index('extern "C" void agent_transport_wifi_set_config')],
    'VOICE_WAIT': realtime[realtime.index('esp_err_t cloud_realtime_wait_idle('):realtime.index('esp_err_t cloud_realtime_stream_start(')],
}
for marker, source in sections.items():
    fixture = fixture.replace('// INSERT_' + marker, source)
build = root / 'build/wifi-local-radio-tests'
build.mkdir(parents=True, exist_ok=True)
generated = build / 'test.cc'
generated.write_text(fixture, encoding='utf-8')
env = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    for line in subprocess.check_output(f'"{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set', shell=True,
                                        text=True, encoding='utf-8', errors='replace').splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key] = value
    compiler = shutil.which('cl.exe', path=env.get('Path') or env.get('PATH'))
    binary = build / 'test.exe'
    command = [compiler, '/nologo', '/std:c++17', '/utf-8', '/EHsc', '/W4',
               '/D_CRT_SECURE_NO_WARNINGS', str(generated), '/Fo' + str(build / 'test.obj'), '/Fe' + str(binary)]
else:
    binary = build / 'test'
    command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-pthread', str(generated), '-o', str(binary)]
subprocess.run(command, env=env, cwd=build, check=True)
subprocess.run([str(binary)], check=True, timeout=20)
