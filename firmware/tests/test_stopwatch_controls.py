"""Native checks for production StopWatch touch parsing, controls and controller methods.

Runs without device/network/credentials. The controller methods are extracted verbatim
from production source and compiled against platform/audio fixtures, not reimplemented.
"""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
app = root / 'apps/m5stack-stopwatch-wifi'
source = (app / 'm5stack_stopwatch_wifi.cc').read_text(encoding='utf-8')
ui = (app / 'watch_wifi_ui.h').read_text(encoding='utf-8')
declarations = source[source.index('enum class Page'):source.index('}  // namespace')]
methods = source[source.index('    void SaveSelection()'):source.index('    void ControlLoop()')]
members = source[source.index('    uint32_t view_'):source.index('\n};', source.index('    uint32_t view_'))]
screen = ui[ui.index('struct Screen {'):ui.index('using InputHandler')]
build = root / '.cache/stopwatch-controls-tests'
build.mkdir(parents=True, exist_ok=True)
fixture = (root / 'tests/stopwatch_controls_fixture.cc').read_text(encoding='utf-8')
fixture = fixture.replace('// INSERT_SCREEN', screen).replace('// INSERT_DECLARATIONS', declarations)
fixture = fixture.replace('// INSERT_METHODS', methods).replace('// INSERT_MEMBERS', members)
generated = build / 'stopwatch_controls_test.cc'
generated.write_text(fixture, encoding='utf-8')
includes = [app, root / 'boards/common', root / 'tests/platform_api_stubs',
            root / 'components/agent_link/include']
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    result = subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True)
    for line in result.splitlines():
        if '=' in line and not line.startswith('='):
            name, value = line.split('=', 1)
            environment[name] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    binary = build / 'stopwatch_controls_test.exe'
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
                    *['/I' + str(p) for p in includes], str(generated),
                    '/Fo' + str(build / 'test.obj'), '/Fe' + str(binary)], env=environment, cwd=build, check=True)
else:
    binary = build / 'stopwatch_controls_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in includes],
                    str(generated), '-o', str(binary)], cwd=build, check=True)
subprocess.run([str(binary)], check=True)

# Compile the real I2C drivers and the two real hardware methods against a register fixture.
hardware = (root / 'boards/m5stack-stopwatch/m5stack_stopwatch_hardware.cc').read_text(encoding='utf-8')
expander = hardware[hardware.index('void M5StopWatchHardware::InitExpander()'):hardware.index('void M5StopWatchHardware::InitDisplay()')]
speaker = hardware[hardware.index('esp_err_t M5StopWatchHardware::SetSpeakerEnabled'):]
driver_fixture = (root / 'tests/stopwatch_driver_fixture.cc').read_text(encoding='utf-8')
driver_fixture = driver_fixture.replace('// INSERT_EXPANDER_METHOD', expander).replace('// INSERT_SPEAKER_METHOD', speaker)
driver_source = build / 'stopwatch_driver_test.cc'
driver_source.write_text(driver_fixture, encoding='utf-8')
driver_includes = [root / 'tests/stopwatch_driver_stubs', *includes]
driver_sources = [driver_source, root / 'boards/common/cst820_touch.cc', root / 'boards/common/m5ioe1.cc']
if os.name == 'nt':
    binary = build / 'stopwatch_driver_test.exe'
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
                    *['/I' + str(p) for p in driver_includes], *map(str, driver_sources),
                    '/Fo' + build.as_posix() + '/', '/Fe' + str(binary)], env=environment, cwd=build, check=True)
else:
    binary = build / 'stopwatch_driver_test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in driver_includes],
                    *map(str, driver_sources), '-o', str(binary)], cwd=build, check=True)
subprocess.run([str(binary)], check=True)
