"""Render the production monochrome UI and fault-inject its hardware boundary."""
import os
from pathlib import Path
import shutil
import subprocess
from PIL import Image, ImageOps, ImageDraw

root = Path(__file__).resolve().parents[1]
output = root / '.cache/paper4-display-tests'
output.mkdir(parents=True, exist_ok=True)
app = root / 'apps/paper-s31-4in0-wifi'
common = root / 'boards/common'
resources = root / 'components/paper_resources'
ime = root / 'components/offline_ime'
resource_image = resources / 'assets/paper_resources.bin'
includes = [root / 'tests/paper4_display_stubs', root / 'tests', app, common,
            root / 'boards/paper-s31', resources / 'include', ime / 'include']
sources = [root / 'tests/paper4_display_test.cc', app / 'paper4_display.cc',
           common / 'stripe_canvas.cc', common / 'raster_montserrat_14.c', resources / 'resource_pack.cc']
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    for line in subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True).splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            environment[key] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    binary = output / 'paper4_display_test.exe'
    command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
               *['/I' + str(p) for p in includes], *map(str, sources),
               '/Fo' + output.as_posix() + '/', '/Fe' + str(binary)]
else:
    binary = output / 'paper4_display_test'
    command = ['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in includes],
               *map(str, sources), '-o', str(binary)]
subprocess.run(command, env=environment, cwd=output, check=True)
subprocess.run([str(binary), str(output), str(resource_image)], check=True, timeout=30)
names = ('home','alarm','reader','activation','calendar','nine','full','review','calibration')
sheet = Image.new('RGB', (5 * 284, 880), '#dddddd')
for i, name in enumerate(names):
    with Image.open(output / (name + '.pgm')) as image:
        image.save(output / (name + '.png'))
        preview = image.resize((264, 396), Image.Resampling.NEAREST).convert('RGB')
        sheet.paste(ImageOps.expand(preview, 1, 'black'), (i%5 * 284 + 9, i//5 * 440 + 26))
        ImageDraw.Draw(sheet).text((i%5 * 284 + 10, i//5 * 440 + 7), name, fill='black')
sheet.save(output / 'preview.png')
print('Production UI previews and allocation/probe/timeout/buffer-guard checks passed:', output / 'preview.png')

# Exercise the actual C pixel packing and partial-transfer implementation, not a second driver.
driver = (root / 'boards/paper-s31/epd.c').read_text(encoding='utf-8')
start = driver.index('/* 屏上当前画面')
shadow = driver[start:driver.index('\n#endif\n\n/* ===', start)]
start = driver.index('static inline uint32_t logical_index(')
transfer = driver[start:driver.index('\n#else\nstatic uint8_t Color_get', start)] + '\n#endif\n'
commands = driver[driver.index('#define PSR '):driver.index('\nstatic spi_device_handle_t')]
fixture = (root / 'tests/paper4_panel_test.cc').read_text(encoding='utf-8')
fixture = fixture.replace('// INSERT_COMMANDS', commands).replace('// INSERT_SHADOW_AND_PARTIAL', shadow)
fixture = fixture.replace('// INSERT_PIXEL_TRANSFER', transfer)
header = (root / 'boards/paper-s31/epd.h').read_text(encoding='utf-8')
for rotation in (270, 90):
    panel_output = output / f'panel-{rotation}'
    panel_output.mkdir(exist_ok=True)
    (panel_output / 'epd.h').write_text(header.replace('#define EPD_ROTATION    270',
                                                     f'#define EPD_ROTATION    {rotation}'), encoding='utf-8')
    generated = panel_output / 'panel_test.cc'
    generated.write_text(fixture, encoding='utf-8')
    panel_includes = [panel_output, root / 'tests/paper4_panel_stubs', root / 'boards/paper-s31']
    if os.name == 'nt':
        panel_binary = panel_output / 'panel_test.exe'
        panel_command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4',
                         '/D_CRT_SECURE_NO_WARNINGS', *['/I' + str(p) for p in panel_includes], str(generated),
                         '/Fo' + panel_output.as_posix() + '/', '/Fe' + str(panel_binary)]
    else:
        panel_binary = panel_output / 'panel_test'
        panel_command = ['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in panel_includes],
                         str(generated), '-o', str(panel_binary)]
    subprocess.run(panel_command, env=environment, cwd=panel_output, check=True)
    subprocess.run([str(panel_binary)], check=True, timeout=30)

application = (app / 'paper4_app.cc').read_text(encoding='utf-8')
def section(start, end):
    first = application.index(start)
    return application[first:application.index(end, first)]
fixture = (root / 'tests/paper4_input_test.cc').read_text(encoding='utf-8')
board_config = (root / 'boards/paper-s31-4in0/config.h').read_text(encoding='utf-8')
fixture = fixture.replace('// INSERT_TOUCH_CONFIG', '\n'.join(
    line for line in board_config.splitlines() if line.startswith('#define PAPER_TOUCH_')))
fixture = fixture.replace('// INSERT_FORMAT', section('std::string Format(', 'constexpr const char* kPresets'))
fixture = fixture.replace('// INSERT_PAGE_WORK', section('enum class Page', 'struct Item'))
fixture = fixture.replace('// INSERT_TOUCH_EVENT', section('struct TouchEvent', 'const char* String'))
fixture = fixture.replace('// INSERT_CONFIGURE_MAPPING', section('    void ConfigureTouchMapping()', '    bool SyncAgent('))
fixture = fixture.replace('// INSERT_BEGIN_COMPOSE', section('    void BeginCompose()', '    void ReviewCompose()'))
fixture = fixture.replace('// INSERT_REVIEW', section('    void ReviewCompose()', '    void HandleTouch('))
fixture = fixture.replace('// INSERT_HANDLE_TOUCH', section('    void HandleTouch(', '    void Handle(Action'))
generated = output / 'input_test.cc'
generated.write_text(fixture, encoding='utf-8')
input_sources = [generated, ime / 'offline_ime.cc', resources / 'resource_pack.cc', common / 'raster_montserrat_14.c']
if os.name == 'nt':
    input_binary = output / 'input_test.exe'
    input_command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
                     *['/I'+str(p) for p in includes], *map(str,input_sources), '/Fo' + output.as_posix() + '/', '/Fe' + str(input_binary)]
else:
    input_binary = output / 'input_test'
    input_command = ['c++','-std=c++17','-Wall','-Wextra',*['-I'+str(p) for p in includes],*map(str,input_sources),'-o',str(input_binary)]
subprocess.run(input_command,env=environment,cwd=output,check=True)
subprocess.run([str(input_binary),str(resource_image)],check=True,timeout=30)

resource_sources = [root / 'tests/paper_resources_test.cc', resources / 'resource_pack.cc',
                    resources / 'resource_partition.cc', common / 'raster_cjk_14.c', common / 'raster_montserrat_14.c']
resource_includes = [root / 'tests/paper_resources_stubs', *includes]
if os.name == 'nt':
    resource_binary = output / 'resources_test.exe'
    resource_command = [compiler,'/nologo','/std:c++17','/EHsc','/utf-8','/W4','/D_CRT_SECURE_NO_WARNINGS',
                        *['/I'+str(p) for p in resource_includes], *map(str,resource_sources),
                        '/Fo'+output.as_posix()+'/', '/Fe'+str(resource_binary)]
else:
    resource_binary = output / 'resources_test'
    resource_command = ['c++','-std=c++17','-Wall','-Wextra',*['-I'+str(p) for p in resource_includes],
                        *map(str,resource_sources),'-o',str(resource_binary)]
subprocess.run(resource_command,env=environment,cwd=output,check=True)
subprocess.run([str(resource_binary),str(resource_image)],check=True,timeout=30)

touch_fixture = (root / 'tests/paper4_touch_test.cc').read_text(encoding='utf-8')
touch_source = (root / 'boards/paper-s31/paper_touch.c').read_text(encoding='utf-8')
bus_source = (root / 'boards/paper-s31/i2c_bus.c').read_text(encoding='utf-8')
touch_fixture = touch_fixture.replace('// INSERT_TOUCH_READ',touch_source[touch_source.index('esp_err_t paper_touch_read('):])
touch_fixture = touch_fixture.replace('// INSERT_WRITE_REG16',bus_source[bus_source.index('esp_err_t i2c_write_reg16('):])
touch_generated = output / 'touch_test.cc'
touch_generated.write_text(touch_fixture,encoding='utf-8')
touch_includes = [root / 'tests/paper4_display_stubs',root / 'boards/paper-s31']
if os.name == 'nt':
    touch_binary = output / 'touch_test.exe'
    touch_command = [compiler,'/nologo','/std:c++17','/EHsc','/utf-8','/W4',
                     *['/I'+str(p) for p in touch_includes],str(touch_generated),
                     '/Fo'+output.as_posix()+'/', '/Fe'+str(touch_binary)]
else:
    touch_binary = output / 'touch_test'
    touch_command = ['c++','-std=c++17','-Wall','-Wextra',*['-I'+str(p) for p in touch_includes],str(touch_generated),'-o',str(touch_binary)]
subprocess.run(touch_command,env=environment,cwd=output,check=True)
subprocess.run([str(touch_binary)],check=True,timeout=30)
