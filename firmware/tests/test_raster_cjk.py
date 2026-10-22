"""Build and inspect the production flash-only raster font and stripe renderer."""
import os
from pathlib import Path
import shutil
import subprocess
from PIL import Image

root = Path(__file__).resolve().parents[1]
build = root / '.cache/raster-cjk-tests'
build.mkdir(parents=True, exist_ok=True)
hanzi = set()
for hi in range(0xb0, 0xf8):
    for lo in range(0xa1, 0xff):
        try:
            hanzi.add(bytes((hi, lo)).decode('gb2312'))
        except UnicodeDecodeError:
            pass
text = build / 'hanzi.txt'
text.write_text(''.join(sorted(hanzi)), encoding='utf-8')
common = root / 'boards/common'
sources = [root / 'tests/raster_cjk_test.cc', common / 'stripe_canvas.cc',
           common / 'raster_cjk_14.c', common / 'raster_montserrat_14.c', common / 'raster_montserrat_20.c']
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    variables = subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True)
    for line in variables.splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1); environment[key] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    binary = build / 'raster_cjk_test.exe'
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4',
                    '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(common), *map(str, sources),
                    '/Fo' + build.as_posix() + '/', '/Fe' + str(binary)], env=environment, cwd=build, check=True)
else:
    binary = build / 'raster_cjk_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-I' + str(common),
                    *map(str, sources), '-o', str(binary)], cwd=build, check=True)
ppm = build / 'passport-cjk.ppm'
subprocess.run([str(binary), str(text), str(ppm)], check=True, timeout=30)
with Image.open(ppm) as image:
    image.save(build / 'passport-cjk.png')
print('Raster preview:', build / 'passport-cjk.png')
