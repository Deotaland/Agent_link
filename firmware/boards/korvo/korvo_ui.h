#pragma once
// Korvo's on-device UI: a phone-style home screen you swipe between apps on, and the four apps
// themselves — Camera, ASR, Command and Recorder.
//
// Two things want the same panel, so this module owns it and hands it out:
//   - LVGL, for the home screen and the ASR / Command / Recorder apps;
//   - the board's preview task, while the Camera app is open.
// They never draw at once — DrawCameraFrame() is serialised against LVGL's flush and does nothing
// unless the Camera app is open, so the preview task can just call it every frame.

#include <cstdint>

#include "cst816_touch.h"
#include "esp_err.h"
#include "korvo_audio.h"
#include "st7789_lcd.h"

namespace korvo_ui {

enum class Mode {
    kLauncher,   // home screen: swipe between app icons, tap one to open it
    kCamera,     // live preview — the only mode where the preview task owns the panel
    kAsr,        // stream the mic up the App's L2CAP ASR channel, which it transcribes live
    kCommand,    // stream the mic up the GATT voice channel (0x40 VoiceChunk) for the App to act on
    kRecorder,   // record the mic to a WAV on the TF card
};

// Bring up LVGL on `lcd`, wire `touch` in as the pointer device, build the screens and start the
// render task. `audio` is what the ASR, Command and Recorder apps drive; it may be null if the codec
// failed, in which case those apps show why instead of doing nothing.
// `touch` may be null: with no way to navigate, the UI then goes straight to the Camera app.
esp_err_t Start(St7789Lcd* lcd, Cst816Touch* touch, KorvoAudio* audio);

Mode CurrentMode();

// Switch apps. Safe from any task — these only request; the render task applies the change so every
// LVGL call stays on one thread.
void OpenApp(Mode m);
void GoHome();

// Blit one camera frame. No-op unless the Camera app is open.
esp_err_t DrawCameraFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void* pixels);

// Reflect the App link on the home screen's status bar.
void SetLinkState(bool connected);

}  // namespace korvo_ui
