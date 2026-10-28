"""Run the production S31 refresh scheduler against deterministic event timings."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / '.cache/paper-refresh-tests'
build.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--busy-diagnostic', action='store_true', help='test production BUSY trace against simulated pin transitions')
args = parser.parse_args()
source = root / 'tests/paper_refresh_test.cc'
if args.busy_diagnostic:
    production = (root / 'boards/paper-s31/epd.c').read_text(encoding='utf-8')
    start = production.index('static void lcd_chkstatus_stage(')
    end = production.index('\nstatic void lcd_chkstatus(void)', start)
    fixture = (root / 'tests/paper_busy_diag_test.cc').read_text(encoding='utf-8')
    source = build / 'paper_busy_diag_test.cc'
    source.write_text(fixture.replace('// INSERT_PRODUCTION_BUSY_WAIT', production[start:end]), encoding='utf-8')
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
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS',
        '/I' + str(include), str(source), '/Fo' + str(build) + '/', '/Fe' + str(binary)],
        env=environment, cwd=build, check=True)
else:
    binary = build / 'paper_refresh_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-I' + str(include),
                    str(source), '-o', str(binary)], cwd=build, check=True)
subprocess.run([str(binary)], check=True)
