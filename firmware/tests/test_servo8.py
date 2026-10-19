"""Run the production Korvo servo driver against an in-memory I2C module."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / '.cache/servo8-tests'
build.mkdir(parents=True, exist_ok=True)
includes = [root / 'tests/servo8_stubs', root / 'boards/korvo']
sources = [root / 'tests/servo8_test.cc', root / 'boards/korvo/servo8.cc']
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-property', 'installationPath'], text=True).strip()
    if not installation:
        raise SystemExit('An installed Visual C++ toolchain is required')
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    output = subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True)
    for line in output.splitlines():
        if '=' in line and not line.startswith('='):
            name, value = line.split('=', 1)
            environment[name] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    binary = build / 'servo8_test.exe'
    command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/utf-8',
        *['/I' + str(p) for p in includes], *map(str, sources),
        '/Fo' + build.as_posix() + '/', '/Fe' + str(binary)]
else:
    binary = build / 'servo8_test'
    command = ['c++', '-std=c++17', '-Wall', '-Wextra',
        *['-I' + str(p) for p in includes], *map(str, sources), '-o', str(binary)]
subprocess.run(command, cwd=build, env=environment, check=True)
subprocess.run([str(binary)], check=True)
