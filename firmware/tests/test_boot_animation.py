"""Exercise the production startup player with deterministic display/audio/flash boundaries."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root/'build/boot-animation-tests'
stubs = build/'stubs'
headers = ('amoled_hardware.h','agent_system.h','config.h','esp_heap_caps.h','esp_log.h',
           'esp_partition.h','esp_rom_crc.h','esp_timer.h','esp_vfs_fat.h','jpeg_decoder.h','nvs.h')
for name in headers:
    path=stubs/name;path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text('#include "boot_animation_stubs.h"\n',encoding='utf-8')
env=os.environ.copy()
if os.name=='nt':
    locator=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
    vs=subprocess.check_output([str(locator),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip()
    for line in subprocess.check_output(f'"{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set',shell=True,text=True).splitlines():
        if '=' in line and not line.startswith('='):
            key,value=line.split('=',1);env[key]=value
    compiler=shutil.which('cl.exe',path=env.get('Path') or env.get('PATH'))
else:
    compiler='c++'
includes=[stubs,root/'tests']
source=root/'tests/boot_animation_test.cc'
binary=build/('boot.exe' if os.name=='nt' else 'boot')
if os.name=='nt':
    command=[compiler,'/nologo','/std:c++17','/utf-8','/EHsc','/D_CRT_SECURE_NO_WARNINGS',*['/I'+str(p) for p in includes],str(source),'/Fo'+str(build/'boot.obj'),'/Fe'+str(binary)]
else:
    command=[compiler,'-std=c++17','-pthread',*['-I'+str(p) for p in includes],str(source),'-o',str(binary)]
subprocess.run(command,cwd=build,env=env,check=True)
subprocess.run([str(binary),str(root/'build/boot-animation/appdata/START.BVA')],cwd=build,check=True,timeout=30)
