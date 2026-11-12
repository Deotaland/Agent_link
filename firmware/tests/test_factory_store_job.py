"""Run the production Factory worker against threaded task and NVS adapters."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/factory-job-tests'
stubs = build / 'stubs'
for name in ('esp_err.h', 'esp_log.h', 'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/task.h', 'nvs.h'):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "factory_store_job_stubs.h"\n', encoding='utf-8')
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
includes = [stubs, root / 'tests', root / 'components/agent_system/include', root / 'components/amoled_ui/include']
source = root / 'tests/factory_store_job_test.cc'
binary = build / ('worker.exe' if os.name == 'nt' else 'worker')
if os.name == 'nt':
    command = [compiler, '/nologo', '/std:c++17', '/utf-8', '/EHsc', '/D_CRT_SECURE_NO_WARNINGS',
        *['/I' + str(p) for p in includes], str(source), '/Fo' + str(build / 'worker.obj'), '/Fe' + str(binary)]
else:
    command = [compiler, '-std=c++17', '-pthread', *['-I' + str(p) for p in includes], str(source), '-o', str(binary)]
subprocess.run(command, cwd=build, env=env, check=True)
subprocess.run([str(binary)], cwd=build, check=True, timeout=30)
