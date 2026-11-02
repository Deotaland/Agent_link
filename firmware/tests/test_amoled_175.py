"""Check the non-C board's production adapter and its profile without hardware."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_firmware', root / 'tools/build_firmware.py')
firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)
profiles = firmware.load_profiles()
profile = profiles['waveshare-amoled-175-wifi']
assert profile['board'] == 'waveshare-amoled-175' and profile['app_shared'] == 'amoled-wifi'
assert 'es_codec.cc' in profile['drivers']
assert firmware.expected_settings(profile)['CONFIG_ESPTOOLPY_FLASHSIZE_16MB'] == 'y'
assert firmware.expected_settings(profiles['waveshare-amoled-175c-wifi'])['CONFIG_ESPTOOLPY_FLASHSIZE_32MB'] == 'y'
build = root / 'build/amoled-175-tests'
build.mkdir(parents=True, exist_ok=True)
firmware.write_product_header(profile, build / 'platform_product.h')
includes = [build, root / 'tests/amoled_175_stubs', root / 'tests/amoled_touch_stubs',
            root / 'apps/common/amoled-wifi',
            root / 'tests/platform_api_stubs', root / 'boards/waveshare-amoled-175', root / 'boards/common']
sources = [root / 'tests/amoled_175_test.cc', root / 'boards/waveshare-amoled-175/amoled_audio.cc']
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
    binary = build / 'test.exe'
    command = [compiler, '/nologo', '/std:c++17', '/utf-8', '/EHsc', '/W4',
               *['/I' + str(p) for p in includes], *map(str, sources), '/Fo' + str(build) + os.sep, '/Fe' + str(binary)]
else:
    binary = build / 'test'
    command = ['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in includes],
               *map(str, sources), '-o', str(binary)]
subprocess.run(command, env=env, cwd=build, check=True)
subprocess.run([str(binary)], check=True)
