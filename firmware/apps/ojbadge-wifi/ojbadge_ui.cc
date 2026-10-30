#include "ojbadge_ui.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace ojbadge {
namespace {
constexpr uint32_t kBackground = 0x101923, kText = 0xECF5F6;
constexpr uint32_t kMuted = 0xA1B5BD, kMint = 0x90E8C4;
lv_obj_t *content, *battery_label, *toast;
const lv_font_t *body_font, *title_font;
Handler on_action;
void* handler_context;
std::string layout;
uint32_t toast_until = 0;

void SetText(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, int x, int y, int w, int h,
                const lv_font_t* font, uint32_t color = kText) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y); lv_obj_set_size(label, w, h);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}
void Click(lv_event_t* event) {
    const uintptr_t data = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (on_action) on_action(static_cast<Action>(data >> 8), data & 0xFF, handler_context);
}
void Button(const char* text, int x, int y, int w, int h, Action action, int index = 0,
            bool primary = false) {
    auto* button = lv_button_create(content);
    lv_obj_set_pos(button, x, y); lv_obj_set_size(button, w, h);
    lv_obj_set_style_radius(button, h / 2, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? kMint : 0x243640), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    auto* label = Label(button, text, 8, 0, w - 16, h, body_font, primary ? kBackground : kText);
    lv_obj_set_height(label, LV_SIZE_CONTENT); lv_obj_center(label);
    lv_obj_add_event_cb(button, Click, LV_EVENT_CLICKED,
        reinterpret_cast<void*>((static_cast<uintptr_t>(action) << 8) | index));
}
void Heading(const char* title, const char* subtitle) {
    Label(content, title, 40, 25, 160, 30, title_font);
    Label(content, subtitle, 30, 56, 180, 25, body_font, kMuted);
}
void Back() { Button("返回", 77, 202, 86, 26, Action::Home); }
std::string Key(const View& v) {
    // RSSI, counters and battery refresh without rebuilding buttons under a finger.
    std::string key = std::to_string(v.home) + ":" + std::to_string(v.friends.state) + ":" +
        std::to_string(v.peer_page) + ":" + std::to_string(v.friends.local_confirmed) + ":" +
        std::to_string(v.friends.remote_confirmed) + ":" + v.friends.remote_name + ":" +
        v.friends.code + ":" + std::to_string(v.friends.event) + ":" + std::to_string(v.friends.error);
    for (int i = 0; i < v.friends.peer_count; ++i) {
        key += v.friends.peers[i].name;
        key.append(reinterpret_cast<const char*>(v.friends.peers[i].mac), 6);
    }
    return key;
}
lv_obj_t* counters = nullptr;
void Render(const View& v) {
    lv_obj_clean(content); counters = nullptr;
    const auto& f = v.friends;
    if (v.home) {
        Heading("OJBadge", "遇见身边的同好");
        Label(content, "HELLO!", 40, 86, 160, 32, &lv_font_montserrat_28, kMint);
        Button("附近交友", 44, 125, 152, 44, Action::Start, 0, true);
        Label(content, v.touch_ok ? "点按开始 · 无需联网" : "触摸不可用，请重启",
              35, 177, 170, 24, body_font, kMuted);
        Label(content, "SW2 短按开始", 55, 202, 130, 22, body_font, kMuted);
        return;
    }
    switch (f.state) {
    case AGENT_FRIEND_SCANNING: {
        Heading("附近的人", f.local_name);
        if (!f.peer_count) {
            Label(content, "正在寻找伙伴…", 30, 99, 180, 30, body_font, kMint);
            Label(content, "请让对方也打开交友", 30, 139, 180, 26, body_font, kMuted);
        } else {
            const int pages = (f.peer_count + 1) / 2;
            const int page = std::min(v.peer_page, pages - 1);
            for (int row = 0; row < 2; ++row) {
                int i = page * 2 + row;
                if (i < f.peer_count) Button(f.peers[i].name, 32, 83 + row * 43, 176, 38, Action::Select, i, row == 0);
            }
            if (pages > 1) {
                char text[64]; snprintf(text, sizeof text, "下一页 %d/%d", page + 1, pages);
                Button(text, 58, 172, 124, 28, Action::Next);
            }
        }
        break;
    }
    case AGENT_FRIEND_WAITING:
        Heading("发出邀请", f.remote_name);
        Label(content, "等待对方回应…", 30, 103, 180, 30, body_font, kMint);
        Button("取消邀请", 55, 151, 130, 36, Action::Disconnect);
        break;
    case AGENT_FRIEND_CONFIRM:
        Heading("确认配对", f.remote_name);
        Label(content, f.code, 35, 83, 170, 32, &lv_font_montserrat_28, kMint);
        if (f.local_confirmed) {
            Label(content, "已确认，等待对方", 25, 126, 190, 26, body_font, kMint);
        } else {
            Button("配对码相同", 48, 119, 144, 38, Action::Accept, 0, true);
        }
        Button("拒绝", 70, 164, 100, 30, Action::Reject);
        break;
    case AGENT_FRIEND_CONNECTED:
        Heading("成为伙伴", f.remote_name);
        counters = Label(content, "", 28, 86, 184, 26, body_font, kMuted);
        Button("打个招呼", 44, 119, 152, 40, Action::Greet, 0, true);
        Button("寻找新伙伴", 52, 165, 136, 32, Action::Disconnect);
        break;
    case AGENT_FRIEND_ERROR: {
        Heading("交友暂不可用", "请重试");
        Label(content, esp_err_to_name(f.error), 27, 96, 186, 32, body_font, kMuted);
        Button("重新开始", 50, 143, 140, 40, Action::Start, 0, true);
        break;
    }
    default:
        Heading("附近交友", "稍等一下");
        Label(content, f.state == AGENT_FRIEND_STOPPING ? "正在关闭交友…" : "正在准备无线连接…",
              25, 102, 190, 45, body_font, kMint);
        break;
    }
    Back();
}
}

void InitUi(const lv_font_t* body, const lv_font_t* title, Handler handler, void* context) {
    body_font = body; title_font = title; on_action = handler; handler_context = context;
    layout.clear();
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBackground), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    auto* ring = lv_obj_create(screen);
    lv_obj_remove_style_all(ring); lv_obj_set_pos(ring, 5, 5); lv_obj_set_size(ring, 230, 230);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x355B57), 0);
    content = lv_obj_create(screen);
    lv_obj_remove_style_all(content); lv_obj_set_size(content, 240, 240);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    battery_label = Label(screen, "", 74, 8, 92, 18, &lv_font_montserrat_14, kMuted);
    toast = lv_obj_create(screen);
    lv_obj_remove_style_all(toast); lv_obj_set_pos(toast, 24, 81); lv_obj_set_size(toast, 192, 76);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x355B57), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0); lv_obj_set_style_radius(toast, 16, 0);
    Label(toast, "", 8, 10, 176, 56, body_font);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}
void UpdateUi(const View& view) {
    const std::string key = Key(view);
    if (layout != key) { layout = key; Render(view); }
    char battery[24];
    if (view.battery >= 0) snprintf(battery, sizeof battery, "%s %d%%", view.charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL, view.battery);
    else snprintf(battery, sizeof battery, "%s --", LV_SYMBOL_BATTERY_EMPTY);
    SetText(battery_label, battery);
    if (counters) {
        char text[64]; snprintf(text, sizeof text, "问候  发 %lu · 收 %lu", static_cast<unsigned long>(view.friends.sent), static_cast<unsigned long>(view.friends.received));
        SetText(counters, text);
    }
    if (toast_until && static_cast<int32_t>(lv_tick_get() - toast_until) >= 0) {
        toast_until = 0; lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    }
}
void Toast(const char* message) {
    lv_label_set_text(lv_obj_get_child(toast, 0), message);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_until = lv_tick_get() + 2200;
}
}
