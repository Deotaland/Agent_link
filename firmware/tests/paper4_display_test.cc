#include "paper4_display.h"
#include "epd.h"
#include "paper_resources_host.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
bool probe = true, allocate = true, timeout = false;
bool base_valid = false;
int timeouts = 0, writes = 0, partials = 0, allocations = 0, probes = 0, budget = -1;
std::vector<uint8_t> pixels;
}
void* heap_caps_malloc(size_t bytes, unsigned) {
    if (!allocate || budget == 0) return nullptr;
    if (budget > 0) --budget;
    assert(bytes == EPD_WIDTH * EPD_HEIGHT);
    auto* block = static_cast<uint8_t*>(std::malloc(bytes + 32));
    assert(block); std::memset(block, 0xa5, bytes + 32); ++allocations;
    return block + 16;
}
void heap_caps_free(void* pointer) {
    auto* block = static_cast<uint8_t*>(pointer) - 16;
    for (int i = 0; i < 16; ++i) {
        assert(block[i] == 0xa5);
        assert(block[16 + EPD_WIDTH * EPD_HEIGHT + i] == 0xa5);
    }
    --allocations; std::free(block);
}
bool epd_probe() { ++probes; return probe; }
int epd_busy_timeouts() { return timeouts; }
void EPD_init() {}
void EPD_sleep() { if (timeout) { ++timeouts; base_valid = false; } }
void PIC_display(const unsigned char* data) {
    pixels.assign(data, data + EPD_WIDTH * EPD_HEIGHT); ++writes;
    base_valid = true;
    for (auto pixel : pixels) assert(pixel == 0 || pixel == 255);
}
void epd_partial(const unsigned char* data, int x, int y, int w, int h) {
    assert(base_valid && x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= EPD_WIDTH && y + h <= EPD_HEIGHT);
    pixels.assign(data, data + EPD_WIDTH * EPD_HEIGHT); ++partials;
    if (timeout) { ++timeouts; base_valid = false; }
}
bool epd_partial_ready() { return base_valid; }
void Save(const std::string& directory, const char* name, const paper4::Frame& frame) {
    paper4::Display display;
    assert(display.Draw(frame) == ESP_OK && allocations == 2);
    std::ofstream file(directory + "/" + name + ".pgm", std::ios::binary);
    file << "P5\n528 792\n255\n";
    file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
}
int main(int argc, char** argv) {
    assert(argc == 3);
    LoadPaperResources(argv[2]);
    paper4::Frame frame;
    frame.title = "我的桌面"; frame.status = "2026-11-09 12:30   电量 85%";
    frame.detail = "Agent：我的智能助手"; frame.footer = "长按 KEY1 返回上一页";
    frame.rows = {"语音通话", "文字消息", "会话管理", "每日闹钟", "月历", "SD 音乐", "设置"};
    frame.selected = 0; Save(argv[1], "home", frame);
    frame.title = "每日闹钟";
    frame.detail = "编辑中：上下修改，确认完成，长按退出";
    frame.rows = {"小时：07", "分钟：30", "状态：开启", "保存闹钟", "返回首页"};
    frame.selected = 1; Save(argv[1], "alarm", frame);
    frame.title = "阅读消息"; frame.detail = "Agent：我的智能助手";
    frame.rows.clear(); frame.selected = -1;
    frame.body = "你好，欢迎使用黑白墨水屏桌面。\n你可以在这里阅读助手的消息，\n使用上下键翻页，长按确认键返回。\n\n语音、日历、每日闹钟和 SD 音乐\n都可以从主菜单打开。";
    Save(argv[1], "reader", frame);
    frame.title = "激活设备"; frame.detail = "请在平台输入以下激活码";
    frame.body = "激活码：123456\n\n打开 bot.deotaland.cn\n\n在平台完成设备激活与 Agent 绑定。";
    Save(argv[1], "activation", frame);
    frame.title = "2026 年 11 月"; frame.detail = "上下切换月份";
    frame.calendar_year = 2026; frame.calendar_month = 11; frame.today = 9;
    frame.body = "日  一  二  三  四  五  六\n\n 1   2   3   4   5   6   7\n\n 8   9  10  11  12  13  14\n\n15  16  17  18  19  20  21\n\n22  23  24  25  26  27  28\n\n29  30";
    Save(argv[1], "calendar", frame);
    frame = {}; frame.title = "编写消息"; frame.status = "2026-11-09 15:30";
    frame.input.kind = paper4::InputKind::Compose;
    frame.input.text = "你好，请介绍一下你自己。"; frame.input.pending = "64426";
    frame.input.candidates = {"你好","你","好","智能体"};
    frame.input.pinyins = {"nihao","ni","hao","zhinengti"};
    Save(argv[1],"nine",frame);
    frame.input.mode = paper4::InputMode::Full; frame.input.pending = "nihao";
    Save(argv[1],"full",frame);
    frame.input.kind = paper4::InputKind::Review; frame.title = "确认发送";
    frame.detail = "请检查消息内容，确认后发送"; frame.body = "你好，请介绍一下你自己。";
    Save(argv[1],"review",frame);
    frame.input = {}; frame.input.kind = paper4::InputKind::Calibrate; frame.title = "触摸校准";
    frame.detail = "依次点击四个十字：1 / 4";
    Save(argv[1],"calibration",frame);
    frame = {}; frame.title = "测试";
    assert(allocations == 0);
    {
        paper4::Display display;
        const int before = writes;
        probe = false; assert(display.Draw(frame) == ESP_ERR_NOT_FOUND && writes == before);
        probe = true; assert(display.Draw(frame) == ESP_OK && writes == before + 1);
    }
    assert(allocations == 0);
    for (int allowed : {0, 1}) {
        budget = allowed;
        { paper4::Display display; assert(display.Draw(frame) == ESP_ERR_NO_MEM); }
        assert(allocations == 0);
    }
    budget = -1;
    {
        paper4::Display display;
        frame = {};
        frame.title = "菜单"; frame.rows = {"语音", "文字", "会话", "闹钟", "月历", "音乐", "设置", "不可见"};
        frame.selected = 0;
        int full = writes, small = partials;
        assert(display.Draw(frame) == ESP_OK && writes == full + 1);
        const int before_probe = probes;
        frame.body = "Hidden body"; frame.rows[7] = "Off-screen changes";
        assert(display.Draw(frame) == ESP_OK && writes == full + 1 && partials == small);
        frame.selected = 1;
        assert(display.Draw(frame) == ESP_OK && writes == full + 1 && partials == small + 1);
        assert(probes == before_probe);
        timeout = true; frame.selected = 0;
        assert(display.Draw(frame) == ESP_ERR_TIMEOUT);
        timeout = false;
        frame.selected = 1; // Returning to the old page must still repair a failed transfer.
        assert(display.Draw(frame) == ESP_OK && writes == full + 2 && probes == before_probe + 1);
        full = writes; small = partials;
        for (unsigned i = 0; i < 64; ++i) {
            frame.selected = 1 - frame.selected;
            assert(display.Draw(frame) == ESP_OK);
        }
        assert(partials == small + 64 && writes == full);
        frame.selected = 1 - frame.selected;
        assert(display.Draw(frame) == ESP_OK && writes == full + 1);
        assert(display.Draw(frame, true) == ESP_OK && writes == full + 2);
        // A broad change across the full page must use a full update.
        frame.rows.clear(); frame.calendar_year = 2026; frame.calendar_month = 11;
        assert(display.Draw(frame) == ESP_OK && writes == full + 3);
    }
    assert(allocations == 0);
    {
        paper4::Display display;
        paper4::HeaderCache header;
        frame = {}; frame.title = "设备激活";
        frame.status = header.Read("12:30 85%", 0, true);
        frame.detail = paper4::ActivationDetail("123456", "600s left");
        frame.body = "激活码：123456";
        const int full = writes, small = partials;
        assert(display.Draw(frame) == ESP_OK);
        for (int i = 1; i < 120; ++i) {
            frame.status = header.Read("clock/battery changed", int64_t(i) * 5000000, true);
            frame.detail = paper4::ActivationDetail("123456", std::to_string(600 - i * 5).c_str());
            assert(display.Draw(frame) == ESP_OK);
        }
        assert(writes == full + 1 && partials == small);
        frame.body = "激活码：654321";
        assert(display.Draw(frame) == ESP_OK && partials == small + 1);
    }
    assert(allocations == 0);
    std::puts("PASS: unchanged pixels/polling skipped, small updates partial, large changes/64-update cleanup full, timeout recovery and allocation guards");
}
