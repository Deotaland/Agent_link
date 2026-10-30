#include "ojbadge_ui.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
uint16_t frame[240 * 240]{};
uint32_t ticks = 100;
uint32_t Tick() { return ticks; }
ojbadge::Action last_action{};
int last_index = -1;
void Action(ojbadge::Action action, int index, void*) { last_action = action; last_index = index; }
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    auto* source = reinterpret_cast<uint16_t*>(pixels);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 240 && y >= 0 && y < 240);
            frame[y * 240 + x] = *source++;
        }
    lv_display_flush_ready(display);
}
lv_obj_t* Find(lv_obj_t* object, const char* text) {
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (auto* found = Find(lv_obj_get_child(object, i), text)) return found;
    return nullptr;
}
void Click(const char* text, ojbadge::Action action, int index = 0) {
    auto* label = Find(lv_screen_active(), text); assert(label);
    last_index = -1;
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
    assert(last_action == action && last_index == index);
}
void Frame(ojbadge::View& view, const char* name) {
    ojbadge::UpdateUi(view); ticks += 40; lv_timer_handler(); lv_refr_now(nullptr);
    FILE* out = fopen(name, "wb"); assert(out); fprintf(out, "P6\n240 240\n255\n");
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) {
        uint16_t v = frame[y * 240 + x];
        if ((x - 120)*(x - 120) + (y - 120)*(y - 120) > 120*120) v = 0;
        unsigned char rgb[]{static_cast<unsigned char>((v >> 11)*255/31),
            static_cast<unsigned char>(((v >> 5)&63)*255/63), static_cast<unsigned char>((v&31)*255/31)};
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
}
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream in(argv[1], std::ios::binary); assert(in);
    std::vector<char> image{std::istreambuf_iterator<char>(in), {}};
    lv_init(); lv_tick_set_cb(Tick);
    auto* display = lv_display_create(240, 240); assert(display);
    static uint8_t pixels[240 * 40 * 2];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, Flush);
    lv_display_set_buffers(display, pixels, nullptr, sizeof pixels, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_font_t body = lv_font_montserrat_14, title = lv_font_montserrat_20;
    body.fallback = lv_tiny_ttf_create_data_ex(image.data()+32, image.size()-32, 14, LV_FONT_KERNING_NONE, 96);
    title.fallback = lv_tiny_ttf_create_data_ex(image.data()+32, image.size()-32, 20, LV_FONT_KERNING_NONE, 48);
    assert(body.fallback && title.fallback);
    ojbadge::InitUi(&body, &title, Action, nullptr);
    ojbadge::View view{}; view.battery = 82;
    Frame(view, "home.ppm"); Click("附近交友", ojbadge::Action::Start);
    view.home = false; view.friends.state = AGENT_FRIEND_SCANNING;
    snprintf(view.friends.local_name, 24, "Link-001122");
    Frame(view, "scanning.ppm");
    view.friends.peer_count = 6;
    for (int i = 0; i < 6; ++i) {
        snprintf(view.friends.peers[i].name, 24, "Link-AABB%02d", i);
        view.friends.peers[i].mac[5] = i + 1;
    }
    Frame(view, "nearby.ppm");
    Click("Link-AABB00", ojbadge::Action::Select, 0);
    Click("下一页 1/3", ojbadge::Action::Next);
    // Periodic telemetry must leave the existing button tree intact.
    auto* peer = Find(lv_screen_active(), "Link-AABB00");
    view.battery = 81; view.friends.peers[0].rssi = -45;
    ojbadge::UpdateUi(view); assert(Find(lv_screen_active(), "Link-AABB00") == peer);
    view.peer_page = 2; Frame(view, "nearby-last.ppm");
    Click("Link-AABB05", ojbadge::Action::Select, 5);
    view.friends.state = AGENT_FRIEND_WAITING;
    snprintf(view.friends.remote_name, 24, "Link-AABB05");
    Frame(view, "waiting.ppm"); Click("取消邀请", ojbadge::Action::Disconnect);
    view.friends.state = AGENT_FRIEND_CONFIRM; snprintf(view.friends.code, 7, "123456");
    Frame(view, "confirm.ppm"); Click("配对码相同", ojbadge::Action::Accept);
    Click("拒绝", ojbadge::Action::Reject);
    view.friends.local_confirmed = true;
    Frame(view, "confirmed.ppm"); assert(!Find(lv_screen_active(), "配对码相同"));
    view.friends.state = AGENT_FRIEND_CONNECTED; view.friends.sent = 3; view.friends.received = 5;
    Frame(view, "connected.ppm"); Click("打个招呼", ojbadge::Action::Greet);
    Click("寻找新伙伴", ojbadge::Action::Disconnect); Click("返回", ojbadge::Action::Home);
    ojbadge::Toast("收到伙伴的问候！"); Frame(view, "greeting.ppm");
    auto* toast = lv_obj_get_parent(Find(lv_screen_active(), "收到伙伴的问候！"));
    assert(!lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN));
    ticks += 2300; ojbadge::UpdateUi(view); assert(lv_obj_has_flag(toast, LV_OBJ_FLAG_HIDDEN));
    view.friends.state = AGENT_FRIEND_ERROR; view.friends.error = ESP_FAIL;
    Frame(view, "error.ppm"); Click("重新开始", ojbadge::Action::Start);
    puts("OJBadge production UI: pages, pagination, mutual confirmation, actions, stable telemetry updates and toast expiry passed.");
}
