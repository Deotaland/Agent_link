"""Fault-inject the production CO5300 QSPI IO and panel without hardware."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/co5300-tests'
build.mkdir(parents=True, exist_ok=True)
includes = [root / 'tests/co5300_stubs', root / 'boards/common']
c_source = root / 'boards/common/co5300_qspi_io.c'
cpp_sources = [root / 'boards/common/co5300_panel.cc', root / 'tests/co5300_io_test.cc']
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
    flags = ['/nologo', '/utf-8', '/W4', *['/I' + str(p) for p in includes]]
    obj = build / 'co5300_qspi_io.obj'
    subprocess.run([compiler, *flags, '/std:c11', '/c', str(c_source), '/Fo' + str(obj)],
                   env=env, cwd=build, check=True)
    binary = build / 'test.exe'
    command = [compiler, *flags, '/std:c++17', '/EHsc', *map(str, cpp_sources), str(obj),
               '/Fo' + str(build) + os.sep, '/Fe' + str(binary)]
else:
    obj = build / 'co5300_qspi_io.o'
    flags = ['-Wall', '-Wextra', *['-I' + str(p) for p in includes]]
    subprocess.run(['cc', *flags, '-std=c11', '-c', str(c_source), '-o', str(obj)], cwd=build, check=True)
    binary = build / 'test'
    command = ['c++', *flags, '-std=c++17', *map(str, cpp_sources), str(obj), '-o', str(binary)]
subprocess.run(command, env=env, cwd=build, check=True)
subprocess.run([str(binary)], check=True, timeout=15)
