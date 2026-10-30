"""Render the production AMOLED UI with native LVGL; no hardware or serial I/O."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import json

root = Path(__file__).resolve().parents[1]
app = root / 'apps/waveshare-amoled-175-wifi'
lvgl = root / 'managed_components/lvgl__lvgl'
build = root / 'build/amoled-voice-tests/ui'
build.mkdir(parents=True, exist_ok=True)
vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
env = os.environ.copy()
for line in subprocess.check_output(f'"{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set', shell=True, text=True).splitlines():
    if '=' in line and not line.startswith('='):
        key, value = line.split('=', 1); env[key] = value
cl = shutil.which('cl.exe', path=env.get('Path') or env.get('PATH'))
lib = shutil.which('lib.exe', path=env.get('Path') or env.get('PATH'))
flags = ['/nologo', '/utf-8', '/w', '/D_CRT_SECURE_NO_WARNINGS', '/DLV_CONF_SKIP', '/DLV_KCONFIG_IGNORE',
         '/DLV_USE_STDLIB_MALLOC=1', '/DLV_USE_STDLIB_STRING=1', '/DLV_USE_STDLIB_SPRINTF=1',
         '/DLV_FONT_MONTSERRAT_20=1', '/DLV_FONT_MONTSERRAT_24=1', '/DLV_FONT_MONTSERRAT_48=1',
         '/DLV_FONT_SIMSUN_14_CJK=1', '/DLV_USE_TINY_TTF=1', '/DLV_USE_LOG=0', '/DLV_USE_ASSERT_MALLOC=1',
         '/I' + str(lvgl), '/I' + str(app), '/I' + str(root / 'boards/waveshare-amoled-175')]
sources = sorted((lvgl / 'src').rglob('*.c'))
assert len({s.stem for s in sources}) == len(sources)
stamp = hashlib.sha256((' '.join(flags) + ''.join(hashlib.sha256(s.read_bytes()).hexdigest() for s in sources)).encode()).hexdigest()
cache = build / 'lvgl.stamp'
log = build / 'compile.log'
with log.open('w', encoding='utf-8') as output:
    if not cache.exists() or cache.read_text() != stamp:
        for i in range(0, len(sources), 40):
            subprocess.run([cl, *flags, '/std:c11', '/MP2', '/c', *map(str, sources[i:i+40]), '/Fo' + str(build) + os.sep], env=env, cwd=build, stdout=output, stderr=subprocess.STDOUT, check=True)
        rsp = build / 'objects.rsp'
        rsp.write_text('\n'.join('"' + str(build / (s.stem + '.obj')) + '"' for s in sources))
        subprocess.run([lib, '/nologo', '/OUT:' + str(build / 'lvgl.lib'), '@' + str(rsp)], env=env, cwd=build, stdout=output, stderr=subprocess.STDOUT, check=True)
        cache.write_text(stamp)

prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include "lvgl.h"
#include "amoled_fonts.h"
#include "amoled_voice_layout.h"
#include "amoled_voice_controls.h"
using esp_err_t = int;
constexpr int ESP_OK=0, DISPLAY_WIDTH=466, DISPLAY_HEIGHT=466, UI_DRAW_BUF_ROWS=40, AUDIO_DEFAULT_VOLUME=60;
constexpr int DISPLAY_BRIGHTNESS=200, DISPLAY_BRIGHTNESS_DIM=20;
enum phase_t { AGENT_LINK_PHASE_IDLE, AGENT_LINK_PHASE_SETUP, AGENT_LINK_PHASE_PAIRING, AGENT_LINK_PHASE_BLOCKED, AGENT_LINK_PHASE_CONNECTED, AGENT_LINK_PHASE_READY };
struct agent_link_status_t { phase_t phase=AGENT_LINK_PHASE_CONNECTED; int transport=0, detail=0; uint32_t expires_s=0; char title[32]={}, hint[112]={}, code[8]={}; };
uint64_t clock_us = 1000000;
int64_t esp_timer_get_time() { return clock_us; }
int portMUX_INITIALIZER_UNLOCKED=0;
using portMUX_TYPE=int;
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
unsigned uxTaskGetStackHighWaterMark(void*) { return 4000; }
unsigned pdMS_TO_TICKS(unsigned value) { return value; }
void vTaskDelay(unsigned value) { clock_us += value * 1000; }
struct Co5300Panel {
    uint16_t pixels[466*466]={};
    int SetBrightness(int) { return 0; }
    int DrawBitmap(int x, int y, int w, int h, const void* data) {
        auto* p=static_cast<const uint16_t*>(data);
        for (int j=0;j<h;++j) for (int i=0;i<w;++i) {
            assert(x+i>=0 && x+i<466 && y+j>=0 && y+j<466);
            const uint16_t v=p[j*w+i]; pixels[(y+j)*466+x+i]=(v>>8)|(v<<8);
        }
        return 0;
    }
};
namespace cst9217 { struct Point { int x=0, y=0; bool pressed=false; }; }
struct Cst9217Touch { int Poll(cst9217::Point* p) { *p={}; return 0; } };
std::vector<char> font_image;
lv_font_t* LoadCjkFont(const char*, int32_t size, uint32_t cache) {
    return lv_tiny_ttf_create_data_ex(font_image.data()+32, font_image.size()-32, size, LV_FONT_KERNING_NONE, cache);
}
'''
header = (app/'amoled_voice_ui.h').read_text(encoding='utf-8')
header = header[header.index('namespace amoled_voice {'):]
fonts = (root/'boards/waveshare-amoled-175/amoled_fonts.cc').read_text(encoding='utf-8')
fonts = '\n'.join(l for l in fonts.splitlines() if not l.startswith('#include'))
source = (app/'amoled_voice_ui.cc').read_text(encoding='utf-8')
source = source[source.index('namespace amoled_voice {'):]
start = source.index('esp_err_t StartUi(')
end = source.index('bool SameScreen(', start)
source = source[:start] + source[end:]
source = source.replace('    fonts_.Init(); Build();', '    static bool built=false; if (!built) { fonts_.Init(); Build(); built=true; }')
source = source.replace('    while (true) {', '    for (int preview_frame=0; preview_frame<3; ++preview_frame) {')
# The firmware calls Render once forever; the harness advances it in bounded
# frames, retaining exactly the locals which normally live across iterations.
for declaration in ['uint32_t seen =', 'size_t offset =', 'bool dim =', 'int64_t activity_at =', 'Activity state =', 'uint32_t ring_color =']:
    begin = source.index('void Render(')
    source = source[:begin] + source[begin:].replace('    ' + declaration, '    static ' + declaration, 1)
start = source.index('void Render(')
source = source[:start] + source[start:].replace('unsigned ring_opacity = 0;', 'static unsigned ring_opacity = 0;', 1)

post = r'''
using namespace amoled_voice;
Input last_input{};
bool InputHandlerTest(const Input& input, void*) { last_input=input; return true; }
void Frame(const char* name) {
    clock_us+=100000; Render(nullptr); lv_refr_now(nullptr);
    FILE* out=fopen(name,"wb"); assert(out); fprintf(out,"P6\n466 466\n255\n");
    for(int y=0;y<466;++y) for(int x=0;x<466;++x) {
        uint16_t v=panel_->pixels[y*466+x];
        if((x-233)*(x-233)+(y-233)*(y-233)>228*228) assert(v==0);
        unsigned char rgb[3]={static_cast<unsigned char>((v>>11)*255/31), static_cast<unsigned char>(((v>>5)&63)*255/63), static_cast<unsigned char>((v&31)*255/31)};
        fwrite(rgb,1,3,out);
    }
    fclose(out);
}
int main(int argc, char** argv) {
    assert(argc==2); std::ifstream in(argv[1],std::ios::binary); assert(in);
    font_image.assign(std::istreambuf_iterator<char>(in),{});
    lv_init(); lv_tick_set_cb(Tick);
    static Co5300Panel panel; panel_=&panel; handler_=InputHandlerTest;
    static uint8_t buffer[466*40*2];
    auto* display=lv_display_create(466,466);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display,Flush);
    lv_display_set_buffers(display,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);
    Screen screen; screen.view=1; screen.navigation=1; screen.calling=true; screen.activity=Activity::kListening;
    snprintf(screen.title,sizeof screen.title,"Agent Link"); snprintf(screen.detail,sizeof screen.detail,"日常对话");
    snprintf(screen.footer,sizeof screen.footer,"BOOT 挂断 · 左滑应用");
    SetScreen(screen); SetVolume(60);
    SetAnswer("你好！很高兴见到你。\n今天想聊些什么？\n\n可以随时上下滑动查看回复，或拖动下方滑块调节音量。\n\n阅读较长的回复时，可以停留在任意位置。新的内容不会打断你。");
    Frame("call.ppm");
    uint32_t before=revision_.load();
    agent_link_status_t same_status=status_;
    for(int i=0;i<2000;++i) { SetStatus(same_status); SetScreen(screen); SetVolume(60); SetBattery(-1,false); }
    assert(revision_.load()==before);
    same_status.hint[100]='x'; // Bytes after the NUL do not change displayed state.
    SetStatus(same_status); assert(revision_.load()==before);
    same_status.phase=AGENT_LINK_PHASE_READY; SetStatus(same_status); assert(revision_.load()==before+1);
    SetAnswer(answer_); assert(revision_.load()==before+1);
    auto semantic_copy=screen; semantic_copy.title[80]='x';
    SetScreen(semantic_copy); assert(revision_.load()==before+1);

    assert(lv_obj_has_flag(answer_view_,LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_scroll_to_y(answer_view_,30,LV_ANIM_OFF);
    const int old_y=lv_obj_get_scroll_y(answer_view_); assert(lv_obj_get_scroll_bottom(answer_view_)>24);
    SetAnswer("你好！很高兴见到你。\n今天想聊些什么？\n\n可以随时上下滑动查看回复，或拖动下方滑块调节音量。\n\n更新的内容不会打断你阅读前面的文字。\n这是新收到的回复。");
    Frame("reply.ppm"); assert(lv_obj_get_scroll_y(answer_view_)==old_y);
    screen.calling=false; screen.ended=true; screen.activity=Activity::kIdle; SetScreen(screen);
    Frame("ended.ppm"); assert(!strcmp(lv_label_get_text(heading_),"通话已结束"));
    lv_slider_set_value(slider_,66,LV_ANIM_OFF); lv_obj_send_event(slider_,LV_EVENT_VALUE_CHANGED,nullptr);
    assert(last_input.action==TouchAction::kVolume && last_input.row==66 && !last_input.commit);
    lv_obj_send_event(slider_,LV_EVENT_RELEASED,nullptr); assert(last_input.commit);
    screen.kind=View::kLauncher; screen.view=2; screen.navigation=2; screen.row_count=3;
    snprintf(screen.title,sizeof screen.title,"应用"); snprintf(screen.detail,sizeof screen.detail,"轻点打开 · 底部上滑返回");
    snprintf(screen.rows[0],112,"会话"); snprintf(screen.rows[1],112,"设置"); snprintf(screen.rows[2],112,"交友");
    SetScreen(screen); Frame("apps.ppm");
    screen.kind=View::kList; screen.view=3; screen.navigation=3; screen.row_count=4;
    snprintf(screen.title,sizeof screen.title,"设置"); snprintf(screen.detail,sizeof screen.detail,"网络、音量与设备");
    snprintf(screen.rows[0],112,"WiFi 状态"); snprintf(screen.rows[1],112,"音量调节");
    snprintf(screen.rows[2],112,"语言 / Language"); snprintf(screen.rows[3],112,"设备激活");
    SetScreen(screen); Frame("settings.ppm");
    const int limit=lv_obj_get_scroll_y(list_)+lv_obj_get_scroll_bottom(list_);
    lv_obj_scroll_to_y(list_,70,LV_ANIM_OFF); Frame("settings-scroll.ppm");
    assert(lv_obj_get_scroll_y(list_)==std::min(70,limit));
    screen.kind=View::kFriend; screen.view=4; screen.navigation=4; screen.row_count=2;
    snprintf(screen.title,sizeof screen.title,"核对两端验证码"); snprintf(screen.detail,sizeof screen.detail,"Link-AABBCC");
    snprintf(screen.pair_code,7,"123456"); snprintf(screen.info,sizeof screen.info,"本机待确认 / 对方待确认");
    snprintf(screen.rows[0],112,"验证码一致，确认连接"); snprintf(screen.rows[1],112,"拒绝连接");
    SetScreen(screen); Frame("friend.ppm");
    screen.kind=View::kHome; screen.navigation=5; screen.view=5; screen.calling=true; screen.ended=false;
    for(int trial=0;trial<300;++trial) {
        std::string message;
        for(int i=0;i<530;++i) {
            unsigned cp=0x4e00+((i+trial*43)%3000);
            message+=char(0xe0|(cp>>12)); message+=char(0x80|((cp>>6)&63)); message+=char(0x80|(cp&63));
        }
        screen.activity=trial%2 ? Activity::kListening : Activity::kSpeaking;
        SetScreen(screen); SetAnswer(message.c_str()); clock_us+=100000; Render(nullptr); lv_refr_now(nullptr);
        if(trial%25==0) { fprintf(stderr,"stress %d\n",trial); fflush(stderr); }
    }
    puts("Native LVGL: render/bounds, scrolling reply retention, ended state, slider preview/commit, menu scrolling and friend confirmation passed.");
}
'''
generated = build/'preview.cc'
generated.write_text(prefix+header+fonts+source+post,encoding='utf-8')
with log.open('a',encoding='utf-8') as output:
    subprocess.run([cl,*flags,'/std:c++17','/EHsc',str(generated),str(build/'lvgl.lib'),'/Fo'+str(build/'preview.obj'),'/Fe'+str(build/'preview.exe')],env=env,cwd=build,stdout=output,stderr=subprocess.STDOUT,check=True)
font = root/'build/flash_assets/anim_pack.bin'
subprocess.run([str(build/'preview.exe'),str(font)],cwd=build,check=True,timeout=45)
from PIL import Image
for ppm in build.glob('*.ppm'):
    Image.open(ppm).save(ppm.with_suffix('.png'))
print('Screenshots:',build)
