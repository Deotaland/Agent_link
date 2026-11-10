#pragma once
#include <cstdint>
#include <initializer_list>
#include "amoled_ui_language.h"

namespace amoled_ui {
#ifndef AMOLED_UI_SIZE
#define AMOLED_UI_SIZE 466
#endif
constexpr int Scale(int value) { return (value * AMOLED_UI_SIZE + 233) / 466; }
constexpr int kDisplaySize = AMOLED_UI_SIZE, kCenter = kDisplaySize / 2;
constexpr int kRingInset = Scale(7), kRingDiameter = kDisplaySize - 2 * kRingInset, kRingWidth = Scale(6);
constexpr int kContentInset = Scale(25), kContentDiameter = kDisplaySize - 2 * kContentInset, kContentRadius = kContentDiameter / 2;
constexpr int kListRows = 12, kListStride = 8, kRowHeight = Scale(66);
struct Rect { int x, y, w, h; };
constexpr Rect ScaledRect(int x, int y, int w, int h) {
    return {Scale(x), Scale(y), Scale(x+w)-Scale(x), Scale(y+h)-Scale(y)};
}
constexpr Rect kHeading = ScaledRect(133, 51, 200, 27);
constexpr Rect kBattery = ScaledRect(132, 84, 202, 20);
constexpr Rect kTitle = ScaledRect(89, 111, 288, 33);
constexpr Rect kDetail = ScaledRect(89, 148, 288, 24);
constexpr Rect kBack = ScaledRect(85, 139, 38, 35);
constexpr Rect kAnswer = ScaledRect(85, 182, 296, 142);
constexpr Rect kList = ScaledRect(85, 181, 296, 195);
constexpr Rect kVolume = ScaledRect(103, 337, 260, 51);
constexpr Rect kFooter = ScaledRect(133, 391, 200, 23);
constexpr Rect kHandle = ScaledRect(197, 421, 72, 5);
constexpr Rect kToast = ScaledRect(113, 260, 240, 68);
constexpr bool Inside(int x, int y, int radius = kContentRadius) {
    return (x - kCenter) * (x - kCenter) + (y - kCenter) * (y - kCenter) <= radius * radius;
}
constexpr bool Fits(const Rect& rect) {
    return Inside(rect.x, rect.y) && Inside(rect.x + rect.w, rect.y) &&
           Inside(rect.x, rect.y + rect.h) && Inside(rect.x + rect.w, rect.y + rect.h);
}
constexpr bool LayoutFits() {
    for (const auto& rect : {kHeading, kBattery, kTitle, kDetail, kBack, kAnswer, kList, kVolume, kFooter, kHandle, kToast})
        if (!Fits(rect)) return false;
    return true;
}
static_assert(LayoutFits(), "All UI rectangles must fit inside the status ring");

enum class Activity { kOffline, kSetup, kConnecting, kIdle, kListening, kThinking, kSpeaking, kError };
constexpr uint32_t ActivityColor(Activity state) {
    switch (state) {
    case Activity::kOffline: return 0x526176;
    case Activity::kSetup: return 0xF3CE54;
    case Activity::kConnecting: return 0x4B8DFF;
    case Activity::kIdle: return 0x52D68A;
    case Activity::kListening: return 0x34DDD2;
    case Activity::kThinking: return 0xAB85FF;
    case Activity::kSpeaking: return 0xFFAD55;
    case Activity::kError: return 0xFF626F;
    }
    return 0x526176;
}
inline const char* ActivityName(Activity state) {
    switch (state) {
    case Activity::kOffline: return Tr("离线", "Offline");
    case Activity::kSetup: return Tr("等待设置", "Setup needed");
    case Activity::kConnecting: return Tr("连接中", "Connecting");
    case Activity::kIdle: return Tr("待机", "Ready");
    case Activity::kListening: return Tr("正在聆听", "Listening");
    case Activity::kThinking: return Tr("正在思考", "Thinking");
    case Activity::kSpeaking: return Tr("正在回复", "Speaking");
    case Activity::kError: return Tr("需要处理", "Needs attention");
    }
    return Tr("离线", "Offline");
}
}  // namespace amoled_ui
