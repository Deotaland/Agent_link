#pragma once
// korvo-cloud's on-device UI — phone-shaped, three surfaces and two edge gestures.
//
// Two pages you swipe between, plus two things you drop into and come back out of.
//
//   Home    page 0, and the link is its content: whatever agent_link_status_t says — "open the
//           app" on BLE, "join the hotspot" or an activation code on WiFi, online, or what is
//           blocking it. Drawn from the status alone, so the same screen serves both transports.
//   Apps    page 1: the camera icon. Just a launcher — the sensor stays idle until it is tapped.
//   Camera  the live preview, opened from that icon. Not LVGL at all; the preview task owns the
//           panel while it is up.
//   Shade   network, platform and identity detail.
//
//   swipe LEFT                      -> next page, wrapping (the phone idiom: the finger drags the
//                                      content left, revealing what is to the right)
//   swipe RIGHT                     -> previous page, wrapping
//   swipe up from the BOTTOM edge   -> leave Camera or Shade, back to the page you came from
//   swipe down from the TOP edge    -> Shade

#include <cstdint>

#include "agent_link.h"
#include "cst816_touch.h"
#include "esp_err.h"
#include "korvo_cloud_audio.h"
#include "st7789_lcd.h"

namespace kc_ui {

enum class Mode {
    kHome,     // page 0: link status
    kApps,     // page 1: the camera icon
    kCamera,   // live preview; the preview task owns the panel here
    kShade,    // pull-down detail panel
};

// Bring up LVGL on `lcd`, wire `touch` in as the pointer device, build the screens and start the
// render task. `touch` may be null: with no pointer and no gestures the board stays on Home forever
esp_err_t Start(St7789Lcd* lcd, Cst816Touch* touch, KorvoCloudAudio* audio);

Mode CurrentMode();

// Switch surfaces. Safe from any task - these only request; the render task applies the change so
// every LVGL call stays on one thread.
void OpenCamera();
void GoHome();

// Blit one camera frame. No-op unless Camera is up.
esp_err_t DrawCameraFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels);

// The link moved. Safe from any transport task - this only copies and flags.
void SetLinkStatus(const agent_link_status_t& st);

// The agent_link transport came up or went down (the dot in the status bar).
void SetLinkState(bool connected);

}  // namespace kc_ui
