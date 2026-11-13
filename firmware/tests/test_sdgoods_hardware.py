"""Exercise the real Sugarat board drivers and Factory pages using host adapters."""
import os
from pathlib import Path
import shutil
import subprocess

root=Path(__file__).resolve().parents[1]
build=root/'build/sdgoods-hardware-tests'
stubs=build/'stubs'
headers=['driver/gpio.h','driver/i2c_master.h','driver/i2s_std.h','esp_adc/adc_oneshot.h',
         'esp_adc/adc_cali.h','esp_adc/adc_cali_scheme.h','esp_log.h','esp_heap_caps.h',
         'freertos/FreeRTOS.h','freertos/task.h','platform_config.h','platform_product.h',
         'amoled_system_menu.h','amoled_ui_language.h','agent_system.h','agent_link_network.h',
         'esp_ota_ops.h','esp_system.h','esp_timer.h','esp_wifi.h','nvs.h']
for name in headers:
    path=stubs/name;path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text('#include "button_hardware_stubs.h"\n',encoding='utf-8')
(stubs/'amoled_hardware.h').write_text('#include "button_hardware_facade.h"\n',encoding='utf-8')
env=os.environ.copy()
if os.name=='nt':
    locator=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
    installation=subprocess.check_output([str(locator),'-latest','-products','*','-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip()
    setup=Path(installation)/'VC/Auxiliary/Build/vcvarsall.bat'
    for line in subprocess.check_output(f'"{setup}" x64 >nul && set',shell=True,text=True).splitlines():
        if '=' in line and not line.startswith('='):
            key,value=line.split('=',1);env[key]=value
    compiler=shutil.which('cl.exe',path=env.get('Path') or env.get('PATH'))
else: compiler='c++'
sources=[root/'tests/button_hardware_test.cc',root/'boards/sdgoods-esp32s3/sdgoods_sensors.cc',
         root/'boards/sdgoods-esp32s3/sdgoods_audio.cc',root/'factory/sdgoods-esp32s3/factory_services.cc']
includes=[stubs,root/'tests',root/'boards/common',root/'boards/sdgoods-esp32s3']
binary=build/('hardware.exe' if os.name=='nt' else 'hardware')
if os.name=='nt':
    command=[compiler,'/nologo','/std:c++17','/utf-8','/EHsc','/D_CRT_SECURE_NO_WARNINGS',
             *['/I'+str(path) for path in includes],*map(str,sources),'/Fo'+build.as_posix()+'/', '/Fe'+str(binary)]
else:
    command=[compiler,'-std=c++17','-pthread',*['-I'+str(path) for path in includes],*map(str,sources),'-o',str(binary)]
subprocess.run(command,cwd=build,env=env,check=True)
subprocess.run([str(binary)],cwd=build,check=True,timeout=30)
