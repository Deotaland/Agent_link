"""Render the production AMOLED UI with native LVGL; no hardware or serial I/O."""
from pathlib import Path
import argparse
import hashlib
import os
import shutil
import subprocess
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--rotation', type=int, choices=(0, 90), default=0)
parser.add_argument('--sdgoods', action='store_true', help='render the native 360px SDGOODS Factory')
parser.add_argument('--carousel', action='store_true', help='render and exercise the factory cover carousel')
parser.add_argument('--voice-cover',type=Path,help='a backend-converted 132x132 Agent Link raw cover')
args = parser.parse_args()

root = Path(__file__).resolve().parents[1]
app = root / 'apps/common/amoled-wifi'
ui = root / 'components/amoled_ui'
factory_ui = root / 'factory/waveshare-amoled-175/ui'
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
         '/DLV_FONT_MONTSERRAT_16=1', '/DLV_FONT_MONTSERRAT_18=1', '/DLV_FONT_MONTSERRAT_36=1', '/DLV_FONT_MONTSERRAT_20=1', '/DLV_FONT_MONTSERRAT_24=1', '/DLV_FONT_MONTSERRAT_48=1',
         '/DLV_FONT_SIMSUN_14_CJK=1', '/DLV_USE_TINY_TTF=1', '/DLV_USE_LOG=0', '/DLV_USE_ASSERT_MALLOC=1',
         '/DLV_USE_LODEPNG=1','/DLV_USE_FS_WIN32=1','/DLV_FS_WIN32_LETTER=65',
         '/I' + str(lvgl), '/I' + str(root / 'boards/common'), '/I' + str(root / 'boards/sdgoods-esp32s3'), '/I' + str(app), '/I' + str(root / 'boards/waveshare-amoled-175C')]
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>
#include "lvgl.h"
#include "amoled_fonts.h"
#include "amoled_ui_layout.h"
#include "amoled_ui_controls.h"
#include "amoled_ui_guides.h"
#include "amoled_hardware_test.h"
#include "amoled_label_layout.h"
#include "amoled_orientation.h"
#include "amoled_launcher_model.h"

constexpr int DISPLAY_ROTATION_CW=0;
using esp_err_t = int;
constexpr int ESP_OK=0, DISPLAY_WIDTH=466, DISPLAY_HEIGHT=466, UI_DRAW_BUF_ROWS=40, AUDIO_DEFAULT_VOLUME=60;
constexpr int DISPLAY_BRIGHTNESS=200, DISPLAY_BRIGHTNESS_DIM=20;
enum phase_t { AGENT_LINK_PHASE_IDLE, AGENT_LINK_PHASE_SETUP, AGENT_LINK_PHASE_PAIRING, AGENT_LINK_PHASE_BLOCKED, AGENT_LINK_PHASE_CONNECTED, AGENT_LINK_PHASE_READY };
struct agent_link_status_t { phase_t phase=AGENT_LINK_PHASE_CONNECTED; int transport=0, detail=0; uint32_t expires_s=0; char title[32]={}, hint[112]={}, code[8]={}; };
std::atomic<uint64_t> clock_us{1000000};
int64_t esp_timer_get_time() { return clock_us; }
int portMUX_INITIALIZER_UNLOCKED=0;
using portMUX_TYPE=int;
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
unsigned uxTaskGetStackHighWaterMark(void*) { return 4000; }
unsigned pdMS_TO_TICKS(unsigned value) { return value; }
void vTaskDelay(unsigned value) { clock_us += value * 1000; }
struct Co5300Panel {
    uint16_t pixels[466*466]={};
    bool fail_next=false;
    int recover_error=0, recover_calls=0, draw_calls=0;
    int Recover(bool* recovered) { ++recover_calls; *recovered=recover_error==0; return recover_error; }
    int SetBrightness(int) { return 0; }
    int DrawBitmap(int x, int y, int w, int h, const void* data) {
        ++draw_calls;
        if(fail_next) { fail_next=false; return -1; }
        auto* p=static_cast<const uint16_t*>(data);
        for (int j=0;j<h;++j) for (int i=0;i<w;++i) {
            assert(x+i>=0 && x+i<466 && y+j>=0 && y+j<466);
            const uint16_t v=p[j*w+i]; pixels[(y+j)*466+x+i]=(v>>8)|(v<<8);
        }
        return 0;
    }
};
#include "cst9217_frame.h"
struct Cst9217Touch { cst9217::Point sample; int Poll(cst9217::Point* p) { *p=sample; return 0; } };
std::vector<char> font_image;
lv_font_t* LoadCjkFont(const char*, int32_t size, uint32_t cache) {
    return lv_tiny_ttf_create_data_ex(font_image.data()+32, font_image.size()-32, size, LV_FONT_KERNING_NONE, cache);
}
'''
prefix += '\nusing RoundDisplay = Co5300Panel; using RoundTouch = Cst9217Touch;\n#include "amoled_display_stubs.h"\n'
transfer_header = (ui/'include/amoled_display_transfer.h').read_text(encoding='utf-8')
transfer_source = (ui/'amoled_display_transfer.cc').read_text(encoding='utf-8')
prefix += '\n'.join(line for line in (transfer_header+'\n'+transfer_source).splitlines()
                    if not line.startswith(('#include', '#pragma'))) + '\n'
header = (ui/'include/amoled_ui.h').read_text(encoding='utf-8')
header = header[header.index('namespace amoled_ui {'):]
fonts = (ui/'amoled_fonts.cc').read_text(encoding='utf-8')
fonts = '\n'.join(l for l in fonts.splitlines() if not l.startswith('#include'))
source = (ui/'amoled_ui.cc').read_text(encoding='utf-8')
source = source[source.index('namespace amoled_ui {'):]
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

post = r'''
using namespace amoled_ui;
Input last_input{};
int row_inputs=0;
bool InputHandlerTest(const Input& input, void*) {
    last_input=input; if(input.action==TouchAction::kRow) ++row_inputs; return true;
}
void TouchSample(Cst9217Touch& touch, bool down, int x, int y) {
    if constexpr (DISPLAY_ROTATION_CW==90) {
        const int old_x=x; x=465-y; y=old_x;
    }
    touch.sample.x=x; touch.sample.y=y; touch.sample.pressed=down;
    clock_us+=40000; Render(nullptr);
}
void CheckVolumeDrag(lv_display_t* display) {
    Cst9217Touch touch; touch_=&touch;
    pointer_=lv_indev_create(); lv_indev_set_type(pointer_,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer_,display); lv_indev_set_read_cb(pointer_,ReadTouch);
    const auto feed=[&](bool down,int x,int y) { TouchSample(touch,down,x,y); };
    lv_area_t track; lv_obj_get_coords(slider_,&track);
    const int y=(track.y1+track.y2)/2;
    feed(true,track.x1+100,y);
    feed(true,track.x1+140,y);
    const int dragged=lv_slider_get_value(slider_); assert(dragged>=68 && dragged<=72);
    // CST9217 release frames carry no position. They decode to zero coordinates.
    touch.sample={}; clock_us+=40000; Render(nullptr);
    assert(lv_slider_get_value(slider_)==dragged);
    assert(last_input.action==TouchAction::kVolume && last_input.row==dragged && last_input.commit);
    feed(true,track.x1+140,y);
    SetAnswer("通话中的新回复。拖动音量时收到文本也不能打断滑块。");
    feed(true,track.x1+100,y+6);
    feed(true,track.x1+60,y-6);
    feed(true,track.x1+20,y+4);
    assert(lv_slider_get_value(slider_)>=8 && lv_slider_get_value(slider_)<=12);
    assert(!pending_ && shown_kind_==View::kHome); // A long volume drag is not a page swipe.
    touch.sample={}; clock_us+=40000; Render(nullptr);
    assert(last_input.action==TouchAction::kVolume && last_input.row>=8 && last_input.row<=12 && last_input.commit);
    lv_indev_delete(pointer_); pointer_=nullptr; touch_=nullptr;
    puts("Volume pointer input: in-call drag and zero-position release retain the dragged value.");
}
void CheckCallRefreshBudget(Co5300Panel& panel) {
    Screen call=screen_; call.calling=true; call.ended=false; call.activity=Activity::kListening;
    SetScreen(call);
    for(int i=0;i<3;++i) { clock_us+=100000; Render(nullptr); lv_refr_now(nullptr); }
    const int draws=panel.draw_calls;
    for(int i=0;i<10;++i) { clock_us+=100000; Render(nullptr); lv_refr_now(nullptr); }
    // An unchanged call must not keep redrawing the full display between input polls.
    assert(panel.draw_calls==draws);
    puts("In-call refresh: no background redraws while state and reply are unchanged.");
}
void CheckMenuTaps(lv_display_t* display) {
    Cst9217Touch touch; touch_=&touch;
    pointer_=lv_indev_create(); lv_indev_set_type(pointer_,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer_,display); lv_indev_set_read_cb(pointer_,ReadTouch);
    lv_area_t row; lv_obj_get_coords(rows_[0],&row);
    const int x=(row.x1+row.x2)/2, y=(row.y1+row.y2)/2;
    const auto feed=[&](bool down,int px,int py) { TouchSample(touch,down,px,py); };
    feed(true,x,y); feed(true,x+2,y+2); feed(false,x+2,y+2);
    assert(row_inputs==1 && last_input.action==TouchAction::kRow && last_input.row==0);
    pending_=false;
    // Slow movement stays inside one row and never exceeds LVGL's per-sample
    // movement threshold; accumulated displacement must still cancel the tap.
    feed(true,x,y);
    for(int dx=1;dx<=16;++dx) feed(true,x+dx,y);
    feed(false,x+16,y); assert(row_inputs==1);
    // Leaving and returning within one contact must not re-arm the option.
    feed(true,x,y); feed(true,row.x1-2,y); feed(true,x,y); feed(false,x,y);
    assert(row_inputs==1);
    // A vertical drag that ends over another option cannot select either row.
    feed(true,x,y); feed(true,x,y+kRowHeight); feed(false,x,y+kRowHeight);
    assert(row_inputs==1);
    lv_obj_scroll_to_y(list_,0,LV_ANIM_OFF);
    // Re-entry from outside the circular input area is still the same contact.
    feed(true,x,y); feed(true,0,0); feed(true,x,y); feed(false,x,y);
    assert(row_inputs==1);
    feed(true,x,y); feed(false,x,y); assert(row_inputs==2);
    pending_=false;
    feed(true,x,y); ++shown_view_; feed(false,x,y); assert(row_inputs==2); --shown_view_;
    lv_indev_delete(pointer_); pointer_=nullptr; touch_=nullptr;
    puts("Menu taps: jitter accepted; slow drag, leave/re-enter, cross-row scroll, circle exit and stale-view clicks rejected.");
}
void Frame(const char* name) {
    clock_us+=100000; Render(nullptr); lv_refr_now(nullptr);
    CompleteTransfer(true);
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
    static uint8_t rotated[sizeof buffer];
    if constexpr (DISPLAY_ROTATION_CW == 90) rotated_pixels_=rotated;
    auto* display=lv_display_create(466,466);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display,Flush);
    lv_display_set_buffers(display,buffer,nullptr,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);
    // Exercise the real flush callback with an asymmetric partial rectangle.
    lv_area_t area{20,30,23,31};
    uint16_t pattern[]{1,2,3,4,5,6,7,8};
    Flush(display,&area,reinterpret_cast<uint8_t*>(pattern));
    for(int y=0;y<2;++y) for(int x=0;x<4;++x) {
        const int px=DISPLAY_ROTATION_CW==90 ? 465-(30+y) : 20+x;
        const int py=DISPLAY_ROTATION_CW==90 ? 20+x : 30+y;
        assert(panel.pixels[py*466+px]==y*4+x+1);
    }
    memset(panel.pixels,0,sizeof panel.pixels);
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
    CheckCallRefreshBudget(panel);
    CheckVolumeDrag(display);
    lv_slider_set_value(slider_,66,LV_ANIM_OFF); lv_obj_send_event(slider_,LV_EVENT_VALUE_CHANGED,nullptr);
    assert(last_input.action==TouchAction::kVolume && last_input.row==66 && !last_input.commit);
    lv_obj_send_event(slider_,LV_EVENT_RELEASED,nullptr); assert(last_input.commit);
    screen.kind=View::kLauncher; screen.view=2; screen.navigation=2; screen.row_count=3;
    snprintf(screen.title,sizeof screen.title,"应用"); snprintf(screen.detail,sizeof screen.detail,"轻点打开 · 底部上滑返回");
    snprintf(screen.rows[0],112,"会话"); snprintf(screen.rows[1],112,"设置"); snprintf(screen.rows[2],112,"交友");
    SetScreen(screen); Frame("apps.ppm");
    panel.fail_next=true;
    Flush(display,&area,reinterpret_cast<uint8_t*>(pattern)); assert(panel_fault_);
    const int draws=panel.draw_calls;
    Flush(display,&area,reinterpret_cast<uint8_t*>(pattern)); assert(panel.draw_calls==draws);
    panel.recover_error=-1; clock_us=panel_retry_at_+1; Render(nullptr);
    assert(panel_fault_ && panel.recover_calls==1 && heartbeat_.load()>0);
    // Touch and menu actions remain functional while DMA recovery is pending.
    CheckMenuTaps(display);
    assert(panel_fault_);
    lv_slider_set_value(slider_,71,LV_ANIM_OFF);
    lv_obj_send_event(slider_,LV_EVENT_VALUE_CHANGED,nullptr);
    lv_obj_send_event(slider_,LV_EVENT_RELEASED,nullptr);
    assert(last_input.action==TouchAction::kVolume && last_input.row==71 && last_input.commit);
    panel.recover_error=0; clock_us=panel_retry_at_+1; Render(nullptr);
    assert(!panel_fault_ && panel.draw_calls>draws && panel_recoveries_.load()==1);
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
    puts("Native LVGL: render/bounds, reply retention, ended state, slider, menu/friend input, display-fault input continuity and full redraw recovery passed.");
}
'''
generated = build/'preview.cc'
prefix = prefix.replace('DISPLAY_ROTATION_CW=0;', f'DISPLAY_ROTATION_CW={args.rotation};')
factory_source = ''
if args.carousel:
    # These affect only the production C++ UI. Reuse the native LVGL C library.
    prefix = '#define AGENT_SYSTEM_ENABLED 1\n' + prefix
    prefix += '\n#include "amoled_launcher_view.h"\n'
    prefix += r'''
namespace agent_system {
std::shared_ptr<const CoverPixels> supplied_pixels;
std::shared_ptr<const CoverPixels> AcquireCover(const Cover& cover) {return cover.slot==0 ? supplied_pixels : nullptr;}
}
void CheckCoverCache() {
    const int before=native_allocations.load();
    {
        AmoledCoverCache cache;
        auto* bytes=new uint8_t[agent_system::kCoverBytes]{};
        auto image=std::make_shared<agent_system::CoverPixels>();
        image->blob=std::shared_ptr<const uint8_t>(bytes,[](const uint8_t* p){delete[] p;});
        auto* colors=reinterpret_cast<uint16_t*>(bytes+agent_system::kCoverHeaderBytes);
        auto* alpha=bytes+agent_system::kCoverHeaderBytes+132*132*2;
        for(int y=0;y<132;++y) for(int x=0;x<132;++x) {
            colors[y*132+x]=0x07E0;alpha[y*132+x]=(x-66)*(x-66)+(y-66)*(y-66)<65*65 ? 255 : 0;
        }
        const auto* scaled=cache.Get(image,256);assert(scaled && scaled->data_size==256*256*3);
        assert(reinterpret_cast<const uint16_t*>(scaled->data)[128*256+128]==0x07E0);
        assert(scaled->data[256*256*2]==0 && scaled->data[256*256*2+128*256+128]==255);
        const int allocated=native_allocations.load();
        assert(cache.Get(image,256)==scaled && native_allocations.load()==allocated);
        native_fail_allocation=true;assert(cache.Get(image,180)==nullptr);native_fail_allocation=false;
        assert(cache.Get(image,180));
        for(int i=0;i<5;++i) {
            auto revision=std::make_shared<agent_system::CoverPixels>();revision->blob=image->blob;
            assert(cache.Get(revision,256));
        }
        assert(native_allocations.load()==before+3);
    }
    assert(native_allocations.load()==before);
    puts("Cover cache: alpha/color, reused sizes, new revisions, bounded eviction and low-memory fallback passed.");
}
'''
    # Compile the real System controller too; only its network/NVS/system
    # boundaries are mocked, so standalone/installed menu decisions stay real.
    prefix += '#define AGENT_SYSTEM_LAYOUT 1\n#define AGENT_SYSTEM_BOARD_MODEL "ESP32-S3-Touch-AMOLED-1.75-16M"\n'
    system_header = (root/'components/agent_system/include/agent_system.h').read_text(encoding='utf-8')
    menu_header = (ui/'include/amoled_system_menu.h').read_text(encoding='utf-8')
    menu_source = (ui/'amoled_system_menu.cc').read_text(encoding='utf-8')
    system_header = '\n'.join(line for line in system_header.splitlines() if not line.startswith(('#include','#pragma')))
    menu_header = '\n'.join(line for line in menu_header.splitlines() if not line.startswith(('#include','#pragma')))
    menu_source = '\n'.join(line for line in menu_source.splitlines() if not line.startswith('#include'))
    menu_support = r'''
namespace agent_system {
bool menu_launcher=false, menu_return=false;
bool IsLauncher() {return menu_launcher;}
bool CanReturnToLauncher() {return menu_return;}
const Descriptor& Current() {static Descriptor value{};return value;}
const char* SettingsPartition() {return "nvs";}
esp_err_t CatalogUrl(char* value,size_t) {value[0]=0;return ESP_OK;}
}
struct agent_link_wifi_info_t {bool connected=false,setup_active=false;char ssid[33]{},ip[16]{},setup_ssid[33]{};int rssi=0;};
agent_link_wifi_info_t test_wifi{};
unsigned wifi_setup_calls=0;
esp_err_t agent_link_wifi_info(agent_link_wifi_info_t* out) {*out=test_wifi;return ESP_OK;}
esp_err_t agent_link_wifi_setup() {++wifi_setup_calls;return ESP_OK;}
struct NativeAppDescription {char version[32]="test";};
const NativeAppDescription* esp_app_get_description() {static NativeAppDescription value;return &value;}
uint32_t esp_get_free_heap_size() {return 1024*1024;}
const char* esp_err_to_name(esp_err_t) {return "test-error";}
using nvs_handle_t=int;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
esp_err_t nvs_open(const char*,int,nvs_handle_t*) {return ESP_ERR_INVALID_STATE;}
esp_err_t nvs_open_from_partition(const char*,const char*,int,nvs_handle_t*) {return ESP_ERR_INVALID_STATE;}
esp_err_t nvs_get_i8(nvs_handle_t,const char*,int8_t*) {return ESP_ERR_INVALID_STATE;}
esp_err_t nvs_get_u8(nvs_handle_t,const char*,uint8_t*) {return ESP_ERR_INVALID_STATE;}
esp_err_t nvs_set_i8(nvs_handle_t,const char*,int8_t) {return ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t,const char*,uint8_t) {return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t) {return ESP_OK;}
void nvs_close(nvs_handle_t) {}
'''
    menu_check = r'''
void CheckSystemMenu() {
    AmoledSystemMenu menu;amoled_ui::Screen s;
    menu.Open();menu.Render(s,1,1,70,false);
    assert(s.row_count==6);
    amoled_ui::Input input{};input.action=amoled_ui::TouchAction::kRow;input.row=6;
    menu.Handle(input);assert(!menu.TakeExit());
    agent_system::menu_return=true;menu.Render(s,1,1,70,false);
    assert(s.row_count==7 && !strcmp(s.rows[6],amoled_ui::Tr("退出到启动器","Exit to launcher")));
    menu.Handle(input);assert(menu.TakeExit());
    agent_system::menu_return=false;menu.Handle(input);assert(!menu.TakeExit());
    agent_system::menu_launcher=true;menu.Render(s,1,1,70,false);
    assert(s.row_count==7 && !strcmp(s.rows[6],amoled_ui::Tr("应用封面与同步","App covers and sync")));
    menu.Handle(input);assert(!menu.TakeExit());menu.Render(s,1,1,70,false);
    assert(!strcmp(s.title,amoled_ui::Tr("应用封面与同步","App covers and sync")));
    agent_system::menu_launcher=false;
    test_wifi.setup_active=true;
    snprintf(test_wifi.setup_ssid,sizeof test_wifi.setup_ssid,"Sugarat-Button-TEST");
    menu.OpenNetwork();menu.Render(s,1,1,70,false);
    assert(strstr(s.info,test_wifi.setup_ssid) && strstr(s.info,"192.168.4.1"));
    assert(strstr(s.info,amoled_ui::Tr("已安装应用可离线使用","Installed apps remain available offline")));
    input.row=0;menu.Handle(input);assert(wifi_setup_calls==1);
    test_wifi.setup_active=false;menu.Render(s,1,1,70,false);
    assert(!strstr(s.info,"192.168.4.1"));
    test_wifi.connected=true;snprintf(test_wifi.ssid,sizeof test_wifi.ssid,"Test WiFi");
    menu.Render(s,1,1,70,false);assert(strstr(s.info,"Test WiFi") && !strstr(s.info,"192.168.4.1"));
    menu.Close();assert(!menu.Active());test_wifi={};
    menu.OpenNetwork();input.action=amoled_ui::TouchAction::kBack;menu.Handle(input);
    assert(menu.Active());menu.Render(s,1,1,70,false);
    assert(!strcmp(s.title,amoled_ui::Tr("系统菜单","System")));
    menu.OpenNetwork();input.action=amoled_ui::TouchAction::kHome;menu.Handle(input);
    assert(!menu.Active());
    bool brightness_opened=false;
    menu.SetExtraPage({&brightness_opened,
        [](const amoled_ui::Input&,void*) {return false;},
        [](amoled_ui::Screen& screen,void* context) {
            snprintf(screen.title,sizeof screen.title,"%s",*static_cast<bool*>(context) ? "Brightness" : "Hardware");
        },
        [](void* context) {*static_cast<bool*>(context)=false;},
        [](void* context) {*static_cast<bool*>(context)=true;}});
    agent_system::menu_launcher=true;menu.Open();menu.Render(s,1,1,70,false);
    assert(s.row_count==9 && !strcmp(s.rows[8],amoled_ui::Tr("屏幕亮度","Screen brightness")));
    input.action=amoled_ui::TouchAction::kRow;input.row=8;menu.Handle(input);
    menu.Render(s,1,1,70,false);assert(brightness_opened && !strcmp(s.title,"Brightness"));
    input.action=amoled_ui::TouchAction::kBack;menu.Handle(input);menu.Render(s,1,1,70,false);
    assert(!brightness_opened && s.row_count==9);
    menu.Close();menu.SetExtraPage({});agent_system::menu_launcher=false;
    puts("System menu: standalone hides return, installed app can return, stale input rejected and launcher cover page retained.");
}
'''
    factory_source = system_header+'\n'+menu_support+'\n'+menu_header+'\n'+menu_source+'\n'+menu_check+'\n'
    factory_source += '\n'.join(line for line in (factory_ui/'factory_view.cc').read_text(encoding='utf-8').splitlines() if not line.startswith('#include'))+'\n'
    cover=args.voice_cover or root/'tests/local_app_store/assets/voice.raw'
    prefix += 'const char* supplied_cover='+json.dumps(cover.resolve().as_posix() if cover.is_file() else '')+';\n'
    post = post.replace('int main(int argc, char** argv) {', r'''
void CheckCarousel(lv_display_t* display) {
    CheckSystemMenu();
    CheckCoverCache();
    static uint8_t full_buffers[2][466*466*2];
    fast_ui_=true;
    assert(display_transfer_.Start(panel_,sizeof full_buffers[0],DISPLAY_ROTATION_CW,466)==ESP_OK);
    lv_display_set_buffers(display,full_buffers[0],full_buffers[1],sizeof full_buffers[0],LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_wait_cb(display,WaitFlush);
    lv_timer_set_period(lv_display_get_refr_timer(display),8);
    lv_timer_set_period(lv_anim_get_timer(),8);
    Screen s; s.kind=View::kCarousel; s.launcher_shell=true; s.view=100; s.navigation=100;
    s.card_count=3;
    {
        auto* data=new uint8_t[agent_system::kCoverBytes]{};
        if(supplied_cover[0]) {
            std::ifstream raw(supplied_cover,std::ios::binary);
            raw.read(reinterpret_cast<char*>(data),agent_system::kCoverBytes);assert(raw.gcount()==agent_system::kCoverBytes);
        } else {
            auto* rgb=reinterpret_cast<uint16_t*>(data+agent_system::kCoverHeaderBytes);
            auto* alpha=data+agent_system::kCoverHeaderBytes+132*132*2;
            for(int y=0;y<132;++y) for(int x=0;x<132;++x) {
                rgb[y*132+x]=static_cast<uint16_t>(((x*31/131)<<11)|((y*63/131)<<5)|15);
                const float distance=std::sqrt(float((2*x-131)*(2*x-131)+(2*y-131)*(2*y-131)))/2;
                alpha[y*132+x]=static_cast<uint8_t>(std::max(0.f,std::min(1.f,66-distance))*255);
            }
        }
        auto image=std::make_shared<agent_system::CoverPixels>();
        image->blob=std::shared_ptr<const uint8_t>(data,[](const uint8_t* p){delete[] p;});
        agent_system::supplied_pixels=image;s.cover_slot=0;s.cover_revision=1;
    }
    snprintf(s.title,sizeof s.title,"Agent Voice");snprintf(s.app_id,sizeof s.app_id,"agent.voice");
    SetScreen(s);Frame("launcher-voice.ppm");
    lv_point_t corner{115,150};
    const int corner_x=DISPLAY_ROTATION_CW==90 ? 465-corner.y : corner.x;
    const int corner_y=DISPLAY_ROTATION_CW==90 ? corner.x : corner.y;
    assert(panel_->pixels[corner_y*466+corner_x]==0);
    if(s.cover_slot==0) assert(lv_obj_get_width(lv_obj_get_child(amoled_factory::launcher_view.CardObject(),0))==256);
    assert(lv_obj_has_flag(heading_,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(battery_label_,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(ring_,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(list_,LV_OBJ_FLAG_HIDDEN));
    Cst9217Touch touch; touch_=&touch;
    pointer_=lv_indev_create();lv_indev_set_type(pointer_,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer_,display);lv_indev_set_read_cb(pointer_,ReadTouch);
    const auto feed=[&](bool down,int x,int y){TouchSample(touch,down,x,y);};
    int before=row_inputs;
    feed(true,233,215);feed(false,233,215);
    assert(row_inputs==before+1 && last_input.action==TouchAction::kRow && last_input.row==0);
    pending_=false;
    before=row_inputs;
    feed(true,310,220);feed(true,220,220);feed(false,220,220);
    assert(last_input.action==TouchAction::kNextCard && row_inputs==before);
    ++s.view;++s.navigation;s.card_index=1;s.cover_slot=-1;
    snprintf(s.title,sizeof s.title,"Test App");snprintf(s.app_id,sizeof s.app_id,"test.notes");
    SetScreen(s);Frame("launcher-sample.ppm");clock_us+=200000;Frame("launcher-sample.ppm");
    ++s.view;++s.navigation;s.card_index=2;s.add_card=true;s.cover_slot=-1;
    snprintf(s.title,sizeof s.title,"添加应用");SetScreen(s);Frame("launcher-add.ppm");
    clock_us+=200000;Frame("launcher-add.ppm");
    before=row_inputs;feed(true,233,215);feed(false,233,215);
    assert(row_inputs==before+1 && last_input.action==TouchAction::kRow);
    pending_=false;
    feed(true,180,220);feed(true,270,220);feed(false,270,220);
    assert(last_input.action==TouchAction::kPreviousCard);
    pending_=false;
    feed(true,233,55);feed(true,233,125);feed(false,233,125);
    assert(last_input.action==TouchAction::kSystem);
    ++s.view;++s.navigation;s.kind=View::kAppActions;s.card_index=0;s.add_card=false;
    if(agent_system::supplied_pixels) s.cover_slot=0;
    snprintf(s.title,sizeof s.title,"Agent Voice");
    snprintf(s.detail,sizeof s.detail,"2.0.0");SetScreen(s);Frame("launcher-actions.ppm");
    before=row_inputs;feed(true,233,185);feed(false,233,185);assert(row_inputs==before);
    feed(true,163,365);feed(false,163,365);assert(row_inputs==before+1 && last_input.row==0);
    pending_=false;feed(true,303,365);feed(false,303,365);assert(row_inputs==before+2 && last_input.row==1);
    pending_=false;
    feed(true,163,365);feed(true,303,365);feed(false,303,365);assert(row_inputs==before+2);
    lv_indev_delete(pointer_);pointer_=nullptr;touch_=nullptr;pending_=false;
    CompleteTransfer(true);
    uint16_t source[]{1,2,3,4,5,6,7,8};
    const uint16_t original[]{1,2,3,4,5,6,7,8};
    lv_area_t area{20,30,23,31};
    Flush(display,&area,reinterpret_cast<uint8_t*>(source));
    assert(display_transfer_.Submit({20,30,4,2},reinterpret_cast<uint8_t*>(source))==ESP_ERR_INVALID_STATE);
    CompleteTransfer(true);
    assert(memcmp(source,original,sizeof source)==0);
    for(int y=0;y<2;++y) for(int x=0;x<4;++x) {
        const int px=DISPLAY_ROTATION_CW==90 ? 465-(30+y) : 20+x;
        const int py=DISPLAY_ROTATION_CW==90 ? 20+x : 30+y;
        assert(panel_->pixels[py*466+px]==y*4+x+1);
    }
    const auto completed=flushes_.load();
    panel_->fail_next=true;Flush(display,&area,reinterpret_cast<uint8_t*>(source));CompleteTransfer(true);
    assert(panel_fault_ && flushes_.load()==completed+1);
    const int draws=panel_->draw_calls;
    Flush(display,&area,reinterpret_cast<uint8_t*>(source));assert(panel_->draw_calls==draws);
    clock_us=panel_retry_at_+1;Render(nullptr);CompleteTransfer(true);assert(!panel_fault_);
    const int allocations=native_allocations.load();
    display_transfer_.Stop();assert(native_allocations.load()==allocations-1);
    AmoledDisplayTransfer failed;
    native_fail_allocation=true;
    assert(failed.Start(panel_,sizeof full_buffers[0],DISPLAY_ROTATION_CW,466)==ESP_ERR_NO_MEM);
    native_fail_allocation=false;assert(!failed.Active());
    assert(native_allocations.load()==allocations-1);
    puts("Async display: borrowed source lifetime, rotation/byte order, completion, transfer failure/recovery and allocation failure passed.");
    puts("Factory carousel: cover taps, swipe/click separation, final add card, top-down menu and launch/delete targets passed.");
}
int main(int argc, char** argv) {''')
    post = post.replace('    puts("Native LVGL:', '    CheckCarousel(display);\n    puts("Native LVGL:')
if args.carousel:
    post = post.replace('int main(int argc, char** argv) {', 'int main(int argc, char** argv) {\n    ConfigureTouch(&amoled_factory::Renderer());')

if args.sdgoods:
    import re
    factory_ui = root / 'factory/sdgoods-esp32s3/ui'
    prefix = '#define AGENT_LINK_UI_DEFAULT_ENGLISH 1\n#define AGENT_SYSTEM_ENABLED 1\n#define AGENT_SYSTEM_LAYOUT 3\n#define AGENT_SYSTEM_BOARD_MODEL "SDGOODS-ESP32S3"\n#define AMOLED_UI_SIZE 360\n' + prefix
    prefix = re.sub(r'\b466\b', '360', prefix)
    prefix += r"""
#include "sdgoods_touch_frame.h"
#include "amoled_cover_cache.h"
namespace agent_system {
std::shared_ptr<const CoverPixels> supplied_pixels;
std::shared_ptr<const CoverPixels> supplied_grid[4];
std::shared_ptr<const CoverPixels> AcquireCover(const Cover& cover) {
    return cover.slot>=0 && cover.slot<4 ? supplied_grid[cover.slot] : nullptr;
}
}
"""
    system_header = (root/'components/agent_system/include/agent_system.h').read_text(encoding='utf-8')
    menu_header = (ui/'include/amoled_system_menu.h').read_text(encoding='utf-8')
    menu_source = (ui/'amoled_system_menu.cc').read_text(encoding='utf-8')
    factory_source = '\n'.join(line for line in system_header.splitlines() if not line.startswith(('#include', '#pragma'))) + '\n'
    factory_source += (root/'tests/sugarat_menu_stubs.h').read_text(encoding='utf-8') + '\n'
    factory_source += '\n'.join(line for line in (menu_header+'\n'+menu_source).splitlines() if not line.startswith(('#include', '#pragma'))) + '\n'
    factory_source += '\n#include "../factory_diagnostics.h"\n' + '\n'.join(line for line in (factory_ui/'factory_view.cc').read_text(encoding='utf-8').splitlines() if not line.startswith('#include'))+'\n'
    # Compile the actual update-page renderer into the native LVGL preview.
    update_header = (root/'components/agent_system/include/launcher_update.h').read_text(encoding='utf-8')
    factory_source += '\n'.join(line for line in update_header.splitlines() if not line.startswith(('#include', '#pragma'))) + '\n'
    factory_source += '''namespace agent_system::launcher {
Status preview_update;
bool preview_busy = false;
Status Read() { return preview_update; }
bool Busy() { return preview_busy; }
}
using amoled_ui::Tr;
'''
    controller = (root/'factory/common/round_factory_application.h').read_text(encoding='utf-8')
    start = controller.index('    void RenderUpdate(')
    end = controller.index('    bool launcher_confirmed_', start)
    factory_source += 'struct UpdatePreview {\n' + controller[start:end] + '};\n'
    post = (root/'tests/sdgoods_ui_fixture.cc').read_text(encoding='utf-8')

generated.write_text(prefix+header+fonts+factory_source+source+post,encoding='utf-8')
with log.open('a',encoding='utf-8') as output:
    subprocess.run([cl,*flags,'/I'+str(root/'tests/co5300_stubs'),'/I'+str(root/'tests'),'/I'+str(root/'components/agent_system/include'),'/I'+str(ui/'include'),'/I'+str(factory_ui),'/std:c++17','/EHsc',str(generated),str(build/'lvgl.lib'),'/Fo'+str(build/'preview.obj'),'/Fe'+str(build/'preview.exe')],env=env,cwd=build,stdout=output,stderr=subprocess.STDOUT,check=True)
font = root/'boards/common/fonts/cjk_font.bin'
subprocess.run([str(build/'preview.exe'),str(font),
    *([str(root/'apps/sdgoods-wifi/assets/agent_link_cover.raw')] if args.sdgoods else [])],cwd=build,check=True,timeout=45)
from PIL import Image
for ppm in build.glob('*.ppm'):
    rendered = Image.open(ppm)
    if args.rotation == 90:
        rendered = rendered.transpose(Image.Transpose.ROTATE_90)
    rendered.save(ppm.with_suffix('.png'))
print('Screenshots:',build)
