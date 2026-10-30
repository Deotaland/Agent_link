"""Exercise the local-only production radio lifecycle without ESP32 hardware."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/ojbadge-radio-tests'
build.mkdir(parents=True, exist_ok=True)
includes = [root / 'tests/ojbadge_radio_stubs', root / 'tests/platform_api_stubs',
            root / 'components/agent_link/src/transports/wifi']
sources = [root / 'tests/ojbadge_radio_test.cc',
           root / 'components/agent_link/src/transports/wifi/wifi_local_standalone.cc']
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
    binary = build / 'radio.exe'
    command = [compiler, '/nologo', '/std:c++17', '/utf-8', '/EHsc', '/W4',
               *['/I' + str(p) for p in includes], *map(str, sources),
               '/Fo' + str(build) + os.sep, '/Fe' + str(binary)]
else:
    binary = build / 'radio'
    command = ['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in includes],
               *map(str, sources), '-o', str(binary)]
subprocess.run(command, env=env, cwd=build, check=True)
subprocess.run([str(binary)], check=True)
