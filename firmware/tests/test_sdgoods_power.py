"""Exercise the shared production power gate used by Recovery and the board."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root/'build/sdgoods-power-tests'
stubs = build/'stubs'
for name in ('bootloader_common.h', 'driver/gpio.h', 'driver/rtc_io.h',
             'esp_attr.h', 'esp_check.h', 'esp_sleep.h', 'esp_system.h',
             'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/task.h'):
    path = stubs/name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "sdgoods_power_stubs.h"\n', encoding='utf-8')
env = os.environ.copy()
if os.name == 'nt':
    locator = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(locator), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    setup = Path(installation)/'VC/Auxiliary/Build/vcvarsall.bat'
    for line in subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True).splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key] = value
    compiler = shutil.which('cl.exe', path=env.get('Path') or env.get('PATH'))
else:
    compiler = 'c++'
source = root/'tests/sdgoods_power_test.cc'
includes = [stubs, root/'tests']
binary = build/('power.exe' if os.name == 'nt' else 'power')
if os.name == 'nt':
    command = [compiler, '/nologo', '/std:c++17', '/utf-8', '/EHsc',
               *['/I'+str(path) for path in includes], str(source),
               '/Fo'+str(build/'power.obj'), '/Fe'+str(binary)]
else:
    command = [compiler, '-std=c++17', *['-I'+str(path) for path in includes],
               str(source), '-o', str(binary)]
subprocess.run(command, cwd=build, env=env, check=True)
subprocess.run([str(binary)], cwd=build, check=True, timeout=15)
