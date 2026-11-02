"""Fault-inject the production S31 music worker and desktop screen callbacks.

Uses the installed host C++ compiler and local cJSON headers. No device or network.
"""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
app = root / 'apps/paper-s31-wifi'
build = root / '.cache/paper-runtime-tests'
build.mkdir(parents=True, exist_ok=True)
music = (app / 'paper_music.cc').read_text(encoding='utf-8')
desktop = (app / 'paper_s31_wifi.cc').read_text(encoding='utf-8')
driver = (root / 'boards/paper-s31/epd.c').read_text(encoding='utf-8')


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


fixture = (root / 'tests/paper_runtime_test.cc').read_text(encoding='utf-8')
music_class = (app / 'paper_music.h').read_text(encoding='utf-8')
music_class = music_class[music_class.index('class PaperMusic {'):].replace('private:', 'public:')
parts = {
    'MUSIC_CLASS': music_class,
    'MUSIC_WORKER': music[music.index('bool PaperMusic::Stop()'):].replace('fopen(', 'OpenTrack('),
    'CAPABILITIES': section(desktop, '    uint32_t Capabilities()', '    void OnLinkStatus('),
    'STATUS': section(desktop, '    void OnLinkStatus(', '    void ShowText('),
    'SHOW_TEXT': section(desktop, '    void ShowText(', '    void PlayAudio('),
    'AUDIO_END': section(desktop, '    void AudioEnd()', '    void AudioReset()'),
    'SCREEN_LOOP': section(desktop, '    void ScreenLoop()', '    void ApplyScreenText()'),
    'APPLY_TEXT': section(desktop, '    void ApplyScreenText()', '    void Tick()'),
    'VOICE_TEXT': section(desktop, '    void UpdateVoiceText(', '    static void ControlEntry('),
    'NAVIGATE': section(desktop, '    void Navigate(', '    void Shortcut('),
    'SHORTCUT': section(desktop, '    void Shortcut(', '    std::vector<std::string> Options()'),
    'PAGE_KEY': section(desktop, 'enum class Page', 'enum class Work'),
    'SCREEN_PUBLISH': section(desktop, '        { std::lock_guard<std::mutex> lock(mutex_);\n          if (!output.Same(screen_))', '    static void ScreenEntry('),
    'CONFIRM': section(desktop, '        if (screen_ready_.load() && confirm_.Take(', '        const int64_t now = esp_timer_get_time();'),
    'ROTATION': section(driver, 'static inline uint32_t logical_index(', '\n#if EPD_COLORS == 2'),
}
for marker, code in parts.items():
    fixture = fixture.replace('// INSERT_' + marker, code)
generated = build / 'paper_runtime_test.cc'
generated.write_text(fixture, encoding='utf-8')
json_dir = root / 'managed_components/espressif__cjson/cJSON'
if not (json_dir / 'cJSON.h').is_file():
    json_dir = Path(os.environ.get('IDF_PATH', '')) / 'components/json/cJSON'
if not (json_dir / 'cJSON.h').is_file():
    raise SystemExit('Local cJSON headers are required; build the S31 profile first')
includes = [app, root / 'boards/paper-s31', json_dir, root / 'tests/platform_api_stubs']
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    if not installation:
        raise SystemExit('An installed Visual C++ toolchain is required')
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    for line in subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True,
                                        encoding='utf-8', errors='replace').splitlines():
        if '=' in line and not line.startswith('='):
            name, value = line.split('=', 1)
            environment[name] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    binary = build / 'paper_runtime_test.exe'
    command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/utf-8',
               '/D_CRT_SECURE_NO_WARNINGS', *['/I' + str(p) for p in includes], str(generated),
               '/Fo' + build.as_posix() + '/', '/Fe' + str(binary)]
else:
    binary = build / 'paper_runtime_test'
    command = ['c++', '-std=c++17', '-Wall', '-Wextra', *['-I' + str(p) for p in includes],
               str(generated), '-o', str(binary)]
subprocess.run(command, env=environment, cwd=build, check=True)
subprocess.run([str(binary)], check=True, timeout=30)
