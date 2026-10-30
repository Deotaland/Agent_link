#pragma once
#include "agent_link_friends.h"
#include "lvgl.h"

namespace ojbadge {
enum class Action { Start, Home, Select, Next, Accept, Reject, Greet, Disconnect };
using Handler = void (*)(Action, int, void*);
struct View {
    agent_friend_snapshot_t friends{};
    bool home = true, charging = false, touch_ok = true;
    int battery = -1, peer_page = 0;
};
// LVGL objects and all these calls belong to the application/UI task.
void InitUi(const lv_font_t* body, const lv_font_t* title, Handler handler, void* context);
void UpdateUi(const View& view);
void Toast(const char* message);
}
