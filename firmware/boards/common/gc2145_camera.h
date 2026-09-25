#pragma once
// Gc2145Camera: reusable driver for the GC2145 DVP camera sensor, in boards/common.
//
// Espressif's esp32-camera component already speaks DVP + SCCB; what it does not do is the
// GC2145's own quirks, and those are what this wrapper owns:
//   - the capture engine comes up idle and has to be kicked (CISCTL restart) or every fb_get times out;
//   - the first rows of every frame are a flickering black/white band that has to be painted over;
//   - AEC/AWB keep drifting on a static scene ("breathing") unless they are frozen once converged.
// A board supplies pins through Gc2145Config and gets frames that are already fixed up.
//
// Note: boards/common is GLOB-compiled for every board, but esp32-camera is only in main's REQUIRES
//   for boards that actually have a camera. gc2145_camera.cc is therefore gated on esp_camera.h being
//   on the include path and compiles to nothing elsewhere — the same "stub out where unsupported"
//   approach co5300_hal.c uses for MIPI. This header is only ever included by camera boards.

#include <cstddef>
#include <cstdint>

#include "esp_camera.h"
#include "esp_err.h"

struct Gc2145Config {
    // DVP 8-bit data + SCCB(I2C) + XCLK. -1 = not wired.
    // The sensor labels its data lines D2..D9 (the top 8 of a 10-bit bus); esp32-camera calls the
    // same wires d0..d7, so a schematic's D2 goes to pin_d0 and its D9 to pin_d7.
    int pin_pwdn     = -1;      // power-down
    int pin_reset    = -1;      // hardware reset
    int pin_xclk     = -1;      // master clock out to the sensor
    int pin_sccb_sda = -1;
    int pin_sccb_scl = -1;
    int pin_d7 = -1, pin_d6 = -1, pin_d5 = -1, pin_d4 = -1;
    int pin_d3 = -1, pin_d2 = -1, pin_d1 = -1, pin_d0 = -1;
    int pin_vsync = -1;
    int pin_href  = -1;
    int pin_pclk  = -1;

    uint32_t       xclk_freq_hz  = 20 * 1000 * 1000;
    ledc_timer_t   ledc_timer    = LEDC_TIMER_0;        // esp32-camera generates XCLK with LEDC
    ledc_channel_t ledc_channel  = LEDC_CHANNEL_0;

    pixformat_t        pixel_format = PIXFORMAT_RGB565;  // straight to an RGB565 panel, no decode
    framesize_t        frame_size   = FRAMESIZE_240X240;
    int                fb_count     = 2;                 // double-buffer
    camera_fb_location_t fb_location = CAMERA_FB_IN_PSRAM;
    camera_grab_mode_t grab_mode    = CAMERA_GRAB_LATEST;  // always hand back the freshest frame

    bool vflip   = false;
    bool hmirror = false;

    // Overpaint this many noisy top rows with the first clean row below, so the flickering band is
    // gone from the preview and from any snapshot taken off the same buffer. 0 = leave frames alone.
    int mask_top_rows = 8;

    // Freeze AEC/AWB once they have had this many frames to converge (~50 = 2s), which stops a
    // static scene from drifting. 0 = never freeze, let the sensor keep adapting.
    uint32_t ae_lock_after_frames = 50;

    // The sensor emits RGB565 little-endian per pixel; panels such as the ST7789 latch big-endian.
    // When true, Capture() hands back frames already swapped — see the warning on Capture().
    bool byte_swap = false;
};

class Gc2145Camera {
public:
    Gc2145Camera() = default;
    Gc2145Camera(const Gc2145Camera&) = delete;
    Gc2145Camera& operator=(const Gc2145Camera&) = delete;

    // Bring up the sensor and kick its capture engine. Frames are available on return.
    esp_err_t Init(const Gc2145Config& cfg);

    bool Ready() const { return ready_; }

    // Next frame, GC2145 fix-ups already applied (top rows masked, AEC/AWB frozen once converged,
    // bytes swapped if configured). nullptr if the grab failed; every non-null frame must go to Release().
    //
    // Careful with byte_swap = true: JPEG encoding (frame2jpg) needs the sensor's original byte
    // order, so a board that both previews and snapshots should leave byte_swap off and call
    // SwapBytes() itself after encoding.
    camera_fb_t* Capture();

    void Release(camera_fb_t* fb);

    // Stop / restart capture on the ESP32 side (DVP DMA and VSYNC interrupt), for boards that show
    // the camera only part of the time. Otherwise the driver keeps receiving and dropping frames
    // ("EV-VSYNC-OVF"). The sensor keeps running, so AEC/AWB stay converged and Resume() is fast.
    //
    // esp_camera_deinit() is not used because it also releases the SCCB bus, which other devices
    // may share (e.g. a touch panel or an audio codec).
    //
    // Call both from the task that calls Capture(). While paused, Capture() returns nullptr.
    void Pause();
    void Resume();
    bool Paused() const { return paused_; }

    // Swap every pixel's two bytes in place (little-endian RGB565 -> big-endian).
    static void SwapBytes(camera_fb_t* fb);

private:
    void MaskTopRows(camera_fb_t* fb) const;
    static void RestartCaptureEngine(sensor_t* s);
    static void LockExposureWhiteBalance(sensor_t* s);

    Gc2145Config cfg_       = {};
    bool         ready_     = false;
    bool         ae_locked_ = false;
    uint32_t     frames_    = 0;
    bool         paused_    = false;
    int          discard_   = 0;   // frames to drop after Resume(): they may predate the pause
};
