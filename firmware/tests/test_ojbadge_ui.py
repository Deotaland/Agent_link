"""Compile/render production round UI with native LVGL; no serial/device access."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
app = root / 'apps/ojbadge-wifi'
lvgl = root / 'managed_components/lvgl__lvgl'
build = root / 'build/ojbadge-ui-tests'
build.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True, encoding='utf-8', errors='replace').strip()
env = os.environ.copy()
for line in subprocess.check_output(f'"{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set', shell=True, text=True, encoding='utf-8', errors='replace').splitlines():
    if '=' in line and not line.startswith('='):
        key, value = line.split('=', 1)
        env[key] = value
cl = shutil.which('cl.exe', path=env.get('Path') or env.get('PATH'))
lib = shutil.which('lib.exe', path=env.get('Path') or env.get('PATH'))
flags = ['/nologo', '/utf-8', '/w', '/D_CRT_SECURE_NO_WARNINGS', '/DLV_CONF_SKIP', '/DLV_KCONFIG_IGNORE',
         '/DLV_USE_STDLIB_MALLOC=1', '/DLV_USE_STDLIB_STRING=1', '/DLV_USE_STDLIB_SPRINTF=1',
         '/DLV_FONT_MONTSERRAT_20=1', '/DLV_FONT_MONTSERRAT_28=1', '/DLV_USE_TINY_TTF=1',
         '/DLV_USE_LOG=0', '/DLV_USE_ASSERT_MALLOC=1', '/I' + str(lvgl), '/I' + str(app),
         '/I' + str(root / 'components/agent_link/include'), '/I' + str(root / 'tests/platform_api_stubs')]
sources = sorted((lvgl / 'src').rglob('*.c'))
assert sources and len({s.stem for s in sources}) == len(sources)
stamp = hashlib.sha256((' '.join(flags) + ''.join(hashlib.sha256(s.read_bytes()).hexdigest() for s in sources)).encode()).hexdigest()
cache = build / 'lvgl.stamp'
with (build / 'compile.log').open('w', encoding='utf-8') as output:
    if not cache.exists() or cache.read_text() != stamp:
        for i in range(0, len(sources), 40):
            subprocess.run([cl, *flags, '/std:c11', '/MP2', '/c', *map(str, sources[i:i+40]), '/Fo' + str(build) + os.sep], env=env, cwd=build, stdout=output, stderr=subprocess.STDOUT, check=True)
        rsp = build / 'objects.rsp'
        rsp.write_text('\n'.join('"' + str(build / (s.stem + '.obj')) + '"' for s in sources))
        subprocess.run([lib, '/nologo', '/OUT:' + str(build / 'lvgl.lib'), '@' + str(rsp)], env=env, cwd=build, stdout=output, stderr=subprocess.STDOUT, check=True)
        cache.write_text(stamp)
    subprocess.run([cl, *flags, '/std:c++17', '/EHsc', str(root / 'tests/ojbadge_ui_test.cc'),
                    str(app / 'ojbadge_ui.cc'), str(build / 'lvgl.lib'), '/Fo' + str(build) + os.sep,
                    '/Fe' + str(build / 'preview.exe')], env=env, cwd=build, stdout=output, stderr=subprocess.STDOUT, check=True)
subprocess.run([str(build / 'preview.exe'), str(root / 'boards/common/fonts/cjk_font.bin')], cwd=build, check=True, timeout=45)
from PIL import Image, ImageDraw
names = ['home', 'nearby', 'confirm', 'connected']
sheet = Image.new('RGB', (len(names) * 260, 284), '#080e15')
draw = ImageDraw.Draw(sheet)
for i, name in enumerate(names):
    img = Image.open(build / (name + '.ppm'))
    img.save(build / (name + '.png'))
    sheet.paste(img, (i * 260 + 10, 10))
    draw.text((i * 260 + 15, 258), name, fill='#90e8c4')
sheet.save(build / 'ojbadge-ui-preview.png')
print(build / 'ojbadge-ui-preview.png')
