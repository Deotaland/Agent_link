"""Decode real backend/image fixtures with the production WebP/PNG/JPEG sources."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
from PIL import Image

root = Path(__file__).resolve().parents[1]
build = root / 'build/community-cover-tests'
stubs = build / 'stubs'
stubs.mkdir(parents=True, exist_ok=True)
platform = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_NOT_SUPPORTED 5
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DEFAULT 0
static inline void* heap_caps_malloc(size_t n,int caps){assert(caps==3 || caps==0);return malloc(n);}
static inline void* heap_caps_calloc(size_t n,size_t s,int caps){assert(caps==3);return calloc(n,s);}
static inline void* heap_caps_realloc(void* p,size_t n,int caps){assert(caps==3);return realloc(p,n);}
static inline void heap_caps_free(void* p){free(p);}
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_GOTO_ON_FALSE(a,code,label,...) do{if(!(a)){ret=code;goto label;}}while(0)
#define ESP_RETURN_ON_FALSE(a,code,...) do{if(!(a))return code;}while(0)
#ifdef __cplusplus
#include <string>
struct Http {std::string body;size_t offset=0;};
using esp_http_client_handle_t=Http*;
struct esp_http_client_event_t;
inline int64_t esp_http_client_get_content_length(Http* h){return h->body.size();}
inline int esp_http_client_read(Http* h,char* out,size_t size){size=std::min(size,h->body.size()-h->offset);memcpy(out,h->body.data()+h->offset,size);h->offset+=size;return (int)size;}
inline bool esp_http_client_is_complete_data_received(Http* h){return h->offset==h->body.size();}
#endif
'''
platform = platform.replace('#include <assert.h>', '#include <assert.h>\n#include <string.h>')
(stubs/'cover_platform.h').write_text(platform, encoding='utf-8')
for name in ['esp_err.h', 'esp_heap_caps.h', 'esp_http_client.h', 'esp_system.h',
             'esp_rom_caps.h', 'esp_log.h', 'esp_check.h', 'freertos/FreeRTOS.h']:
    path = stubs/name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "cover_platform.h"\n', encoding='utf-8')
(stubs/'sdkconfig.h').write_text('#define CONFIG_JD_SZBUF 512\n#define CONFIG_JD_FORMAT 0\n#define CONFIG_JD_USE_SCALE 1\n#define CONFIG_JD_FASTDECODE 0\n', encoding='utf-8')
(stubs/'src/webp').mkdir(parents=True, exist_ok=True)
(stubs/'src/webp/config.h').write_text('/* Scalar host configuration, matching the S3 build. */\n', encoding='utf-8')
image = Image.new('RGBA', (320, 180), (200, 80, 32, 128))
image.save(build/'cover.png')
image.save(build/'lossless.webp', lossless=True)
image.save(build/'lossy.webp', quality=85)
image.convert('RGB').save(build/'cover.jpg', quality=90)
image.save(build/'animated.webp', save_all=True, append_images=[Image.new('RGBA',image.size,(0,0,0,255))], duration=100, loop=0)
public = root/'build/community-api/cover-1.bin'
fixtures = [build/name for name in ('cover.png','lossless.webp','lossy.webp','cover.jpg')]
if public.is_file(): fixtures.append(public)
environment = os.environ.copy()
vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True).strip()
for line in subprocess.check_output(f'"{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set',shell=True,text=True).splitlines():
    if '=' in line and not line.startswith('='):
        key,value=line.split('=',1);environment[key]=value
cl=shutil.which('cl.exe',path=environment.get('Path') or environment.get('PATH'))
webp=root/'components/webp_decoder'
png=root/'components/png_decoder'
jpeg=root/'managed_components/espressif__esp_jpeg'
definitions=['AGENT_SYSTEM_LAYOUT=3','AGENT_SYSTEM_BOARD_MODEL="SDGOODS-ESP32S3"',
             'LODEPNG_NO_COMPILE_ENCODER','LODEPNG_NO_COMPILE_DISK','LODEPNG_NO_COMPILE_CPP','LODEPNG_NO_COMPILE_ALLOCATORS']
flags=['/nologo','/utf-8','/w','/D_CRT_SECURE_NO_WARNINGS',*[f'/D{x}' for x in definitions],
       *[f'/I{x}' for x in (stubs,root/'components/agent_system/include',webp/'third_party',png/'third_party',jpeg/'include',jpeg/'tjpgd')]]
sources=[*sorted((webp/'third_party/src').glob('dec/*.c')),*sorted((webp/'third_party/src').glob('dsp/*.c')),*sorted((webp/'third_party/src').glob('utils/*.c'))]
with (build/'compile.log').open('w',encoding='utf-8') as log:
    stamp=hashlib.sha256(Path(__file__).read_bytes()+(' '.join(flags)+''.join(hashlib.sha256(p.read_bytes()).hexdigest() for p in sources)).encode()).hexdigest()
    cache=build/'webp.stamp'
    if not cache.is_file() or cache.read_text()!=stamp:
        subprocess.run([cl,*flags,'/std:c11','/c','/DHAVE_CONFIG_H',
                        '/Dmalloc=agl_webp_malloc','/Dcalloc=agl_webp_calloc','/Dfree=agl_webp_free',
                        *map(str,sources),'/Fo'+build.as_posix()+'/'],env=environment,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
        cache.write_text(stamp)
    subprocess.run([cl,*flags,'/std:c11','/c',str(jpeg/'jpeg_decoder.c'),str(jpeg/'tjpgd/tjpgd.c'),'/Fo'+build.as_posix()+'/'],env=environment,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
    objects=[build/(p.stem+'.obj') for p in sources]+[build/'jpeg_decoder.obj',build/'tjpgd.obj']
    binary=build/'cover.exe'
    subprocess.run([cl,*flags,'/std:c++17','/EHsc',str(root/'tests/community_cover_test.cc'),
                    str(root/'components/agent_system/community_cover.cc'),str(png/'third_party/lodepng.cpp'),
                    str(png/'png_memory.cc'),str(webp/'webp_memory.cc'),*map(str,objects),
                    '/Fo'+build.as_posix()+'/', '/Fe'+str(binary)],env=environment,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
subprocess.run([str(binary),str(build),*map(str,fixtures)],cwd=build,check=True)
print(f'Production cover codecs passed: {len(fixtures)} valid fixtures, truncated/animated/oversize rejection, RGB565/A8/CRC and public-file conversion')
