"""Run AI Passport WiFi input and PCM transport checks without hardware or network."""
import os
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / '.cache/passport-wifi-tests'
build.mkdir(parents=True, exist_ok=True)
includes = [root / 'apps/ai-passport-wifi', root / 'boards/common',
            root / 'tests/platform_api_stubs',
            root / 'components/agent_link/src/transports/wifi/cloud']
audio = (root / 'apps/ai-passport-wifi/passport_wifi_audio.cc').read_text(encoding='utf-8')
playback = audio[audio.index('void PassportWifiAudio::PlayLoop()'):audio.index('void PassportWifiAudio::MicLoop()')]
# Exercise the actual idle-delay statement, including conversion at both tick rates.
idle_wait = re.search(r'vTaskDelay\([^;]+;', playback).group(0)
fixture = (root / 'tests/passport_wifi_test.cc').read_text(encoding='utf-8')
source = build / 'passport_wifi_test.cc'
source.write_text(fixture.replace('// INSERT_PLAYBACK_IDLE_WAIT', idle_wait), encoding='utf-8')
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    result = subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True)
    for line in result.splitlines():
        if '=' in line and not line.startswith('='):
            name, value = line.split('=', 1)
            environment[name] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    binary = build / 'passport_wifi_test.exe'
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/DCONFIG_IDF_TARGET_ESP32C3=1',
        *['/I' + str(path) for path in includes], str(source), '/Fo' + str(build / 'test.obj'),
        '/Fe' + str(binary)], env=environment, cwd=build, check=True)
else:
    binary = build / 'passport_wifi_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-DCONFIG_IDF_TARGET_ESP32C3=1',
        *['-I' + str(path) for path in includes], str(source), '-o', str(binary)], cwd=build, check=True)
subprocess.run([str(binary)], check=True)
