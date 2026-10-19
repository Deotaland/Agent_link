"""Run the production S31 refresh scheduler against deterministic event timings."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / '.cache/paper-refresh-tests'
build.mkdir(parents=True, exist_ok=True)
source = root / 'tests/paper_refresh_test.cc'
include = root / 'apps/paper-s31-wifi'
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
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
    binary = build / 'paper_refresh_test.exe'
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
        '/I' + str(include), str(source), '/Fo' + str(build) + '/', '/Fe' + str(binary)],
        env=environment, cwd=build, check=True)
else:
    binary = build / 'paper_refresh_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-I' + str(include),
                    str(source), '-o', str(binary)], cwd=build, check=True)
subprocess.run([str(binary)], check=True)
