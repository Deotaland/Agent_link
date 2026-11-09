"""Exercise production WiFi startup and role/platform decisions without network or devices."""
import os
from pathlib import Path
import shutil
import subprocess

root=Path(__file__).resolve().parents[1]
wifi=(root/'components/agent_link/src/transports/wifi/transport_wifi.cpp').read_text(encoding='utf-8')
system=(root/'factory/waveshare-amoled-175/factory_application.cc').read_text(encoding='utf-8')
voice=(root/'apps/common/amoled-wifi/amoled_wifi_application.h').read_text(encoding='utf-8')

def platform(source):
    start=source.index('    const agent_platform_t* Platform() const override')
    opening=source.index('{',start)
    depth=1
    end=opening+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]

fixture=(root/'tests/amoled_network_mode_test.cc').read_text(encoding='utf-8')
sections={
    'SYSTEM_PLATFORM':platform(system), 'VOICE_PLATFORM':platform(voice),
    'COMPOSE':wifi[wifi.index('void Compose('):wifi.index('void ReportStatus()',wifi.index('void Compose('))],
    'CLOUD_START':wifi[wifi.index('void StartCloudSession()'):wifi.index('// agent_transport_t interface')],
    'WIFI_START':wifi[wifi.index('esp_err_t wifi_start('):wifi.index('void wifi_stop(')],
}
for marker,production in sections.items():fixture=fixture.replace('// INSERT_'+marker,production)
build=root/'build/amoled-network-tests';build.mkdir(parents=True,exist_ok=True)
source=build/'test.cc';source.write_text(fixture,encoding='utf-8')
env=os.environ.copy()
if os.name=='nt':
    vswhere=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
    vs=subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip()
    for line in subprocess.check_output(f'"{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set',shell=True,text=True).splitlines():
        if '=' in line and not line.startswith('='):
            key,value=line.split('=',1);env[key]=value
    compiler=shutil.which('cl.exe',path=env.get('Path') or env.get('PATH'))
    binary=build/'test.exe'
    command=[compiler,'/nologo','/std:c++20','/utf-8','/EHsc','/W4','/D_CRT_SECURE_NO_WARNINGS',str(source),'/Fo'+str(build/'test.obj'),'/Fe'+str(binary)]
else:
    binary=build/'test'
    command=['c++','-std=c++20','-Wall','-Wextra','-pthread',str(source),'-o',str(binary)]
subprocess.run(command,env=env,cwd=build,check=True)
subprocess.run([str(binary)],check=True,timeout=10)
