"""Native checks of production parsing/layout and extracted controller methods."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
app = root / "apps/waveshare-amoled-175-wifi"
source = (app / "waveshare_amoled_wifi.cc").read_text(encoding="utf-8")
ui = (app / "amoled_voice_ui.cc").read_text(encoding="utf-8")
fixture = (root / "tests/amoled_voice_fixture.cc").read_text(encoding="utf-8")
fixture = fixture.replace("// INSERT_RESOLVE", ui[ui.index("Activity Resolve("):ui.index("void Render(")])
fixture = fixture.replace("// INSERT_END_CALL", source[source.index("    void EndCall()"):source.index("    bool Error(")])
fixture = fixture.replace("// INSERT_TICK_CALL", source[source.index("    void TickCall()"):source.index("    void OpenFriends()")])
fixture = fixture.replace("// INSERT_BUTTON", source[source.index("    void PhysicalButton()"):source.index("    void ChangeVolume(")])
audio = (app / "amoled_voice_audio.cc").read_text(encoding="utf-8")
fixture = fixture.replace("// INSERT_PCM_PUBLISH", audio[audio.index("void AmoledVoiceAudio::BeginDiagnostics()"):audio.index("void AmoledVoiceAudio::ReportDiagnostics()")].replace("AmoledVoiceAudio::", ""))
fixture = fixture.replace("// INSERT_MIC_LOOP", audio[audio.index("void AmoledVoiceAudio::MicLoop()"):].replace("AmoledVoiceAudio::", ""))
build = root / "build/amoled-voice-tests"
build.mkdir(parents=True, exist_ok=True)
generated = build / "amoled_voice_test.cc"
generated.write_text(fixture, encoding="utf-8")
includes = [app, root / "boards/common"]
environment = os.environ.copy()
if os.name == "nt":
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    setup = Path(installation) / "VC/Auxiliary/Build/vcvarsall.bat"
    result = subprocess.check_output(f'"{setup}" x64 >nul && set', shell=True, text=True)
    for line in result.splitlines():
        if "=" in line and not line.startswith("="):
            name, value = line.split("=", 1)
            environment[name] = value
    compiler = shutil.which("cl.exe", path=environment.get("Path") or environment.get("PATH"))
    binary = build / "amoled_voice_test.exe"
    command = [compiler, "/nologo", "/std:c++17", "/utf-8", "/EHsc", "/W4", "/D_CRT_SECURE_NO_WARNINGS",
               *["/I" + str(p) for p in includes], str(generated),
               "/Fo" + str(build / "test.obj"), "/Fe" + str(binary)]
else:
    binary = build / "amoled_voice_test"
    command = ["c++", "-std=c++17", "-Wall", "-Wextra", *["-I" + str(p) for p in includes],
               str(generated), "-o", str(binary)]
subprocess.run(command, env=environment, cwd=build, check=True)
subprocess.run([str(binary)], check=True)

# Exercise the C-board production standard I2S path and checked-open boundary.
board_audio = (root / "boards/waveshare-amoled-175/amoled_audio.cc").read_text(encoding="utf-8")
audio_fixture = (root / "tests/amoled_audio_fixture.cc").read_text(encoding="utf-8")
audio_fixture = audio_fixture.replace("// INSERT_CHECKED_DATA",
    board_audio[board_audio.index("struct CheckedData {"):board_audio.index("\n}\n\nstruct AmoledAudio::State")])
audio_fixture = audio_fixture.replace("// INSERT_OPEN",
    board_audio[board_audio.index("    esp_err_t Open("):board_audio.index("    esp_err_t Init(int volume)")])
audio_fixture = audio_fixture.replace("// INSERT_READ_WRITE",
    board_audio[board_audio.index("esp_err_t AmoledAudio::ReadPcm("):board_audio.index("esp_err_t AmoledAudio::SetVolume(")])
audio_generated = build / "amoled_audio_test.cc"
audio_generated.write_text(audio_fixture, encoding="utf-8")
audio_binary = build / ("amoled_audio_test.exe" if os.name == "nt" else "amoled_audio_test")
if os.name == "nt":
    command = [compiler, "/nologo", "/std:c++17", "/utf-8", "/EHsc", "/W4",
               *["/I" + str(p) for p in includes], str(audio_generated),
               "/Fo" + str(build / "audio.obj"), "/Fe" + str(audio_binary)]
else:
    command = ["c++", "-std=c++17", "-Wall", "-Wextra", *["-I" + str(p) for p in includes],
               str(audio_generated), "-o", str(audio_binary)]
subprocess.run(command, env=environment, cwd=build, check=True)
subprocess.run([str(audio_binary)], check=True)

includes += [root / "components/agent_link/include", root / "components/agent_link/src/transports/wifi",
             root / "tests/platform_api_stubs"]
protocol_source = root / "tests/friends_protocol_test.cc"
if os.name == "nt":
    protocol_binary = build / "friends_protocol_test.exe"
    command = [compiler, "/nologo", "/std:c++17", "/utf-8", "/EHsc", "/W4", "/D_CRT_SECURE_NO_WARNINGS",
               *["/I" + str(p) for p in includes], str(protocol_source),
               "/Fo" + str(build / "friends.obj"), "/Fe" + str(protocol_binary)]
else:
    protocol_binary = build / "friends_protocol_test"
    command = ["c++", "-std=c++17", "-Wall", "-Wextra", *["-I" + str(p) for p in includes],
               str(protocol_source), "-o", str(protocol_binary)]
subprocess.run(command, env=environment, cwd=build, check=True)
subprocess.run([str(protocol_binary)], check=True)
