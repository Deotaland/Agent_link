// Gc2145Camera: reusable GC2145 DVP camera driver, see gc2145_camera.h.
//
// boards/common is GLOB-compiled for every board, but esp32-camera is only in main's REQUIRES for
// boards that have a camera — so its headers are on the include path exactly then. Gate on that and
// compile to nothing everywhere else, the way co5300_hal.c stubs out MIPI off the ESP32-P4.
#if __has_include("esp_camera.h")

#include "gc2145_camera.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void cam_stop(void);
extern "C" void cam_start(void);

namespace {
constexpr const char* TAG = "gc2145";
}  // namespace

esp_err_t Gc2145Camera::Init(const Gc2145Config& cfg) {
    cfg_ = cfg;

    camera_config_t cc = {};
    cc.pin_pwdn     = cfg_.pin_pwdn;
    cc.pin_reset    = cfg_.pin_reset;
    cc.pin_xclk     = cfg_.pin_xclk;
    cc.pin_sccb_sda = cfg_.pin_sccb_sda;
    cc.pin_sccb_scl = cfg_.pin_sccb_scl;
    cc.pin_d7       = cfg_.pin_d7;
    cc.pin_d6       = cfg_.pin_d6;
    cc.pin_d5       = cfg_.pin_d5;
    cc.pin_d4       = cfg_.pin_d4;
    cc.pin_d3       = cfg_.pin_d3;
    cc.pin_d2       = cfg_.pin_d2;
    cc.pin_d1       = cfg_.pin_d1;
    cc.pin_d0       = cfg_.pin_d0;
    cc.pin_vsync    = cfg_.pin_vsync;
    cc.pin_href     = cfg_.pin_href;
    cc.pin_pclk     = cfg_.pin_pclk;
    cc.xclk_freq_hz = static_cast<int>(cfg_.xclk_freq_hz);
    cc.ledc_timer   = cfg_.ledc_timer;
    cc.ledc_channel = cfg_.ledc_channel;
    cc.pixel_format = cfg_.pixel_format;
    cc.frame_size   = cfg_.frame_size;
    cc.fb_count     = cfg_.fb_count;
    cc.fb_location  = cfg_.fb_location;
    cc.grab_mode    = cfg_.grab_mode;

    const esp_err_t r = esp_camera_init(&cc);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(r));
        return r;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "camera sensor PID=0x%04x (GC2145 expected)", s->id.PID);
        s->set_vflip(s, cfg_.vflip ? 1 : 0);
        s->set_hmirror(s, cfg_.hmirror ? 1 : 0);
        RestartCaptureEngine(s);
    }

    ready_     = true;
    ae_locked_ = (cfg_.ae_lock_after_frames == 0);  // 0 = never freeze; nothing left to do
    frames_    = 0;
    ESP_LOGI(TAG, "camera ready (fmt=%d frame_size=%d)",
             static_cast<int>(cfg_.pixel_format), static_cast<int>(cfg_.frame_size));
    return ESP_OK;
}

// The GC2145 powers up with its capture engine held in reset, so the first fb_get would just time
// out. Pulsing CISCTL_restart_n (reg 0xfe bit4, active-low) low->high kicks it into streaming.
void Gc2145Camera::RestartCaptureEngine(sensor_t* s) {
    if (!s->set_reg) return;
    s->set_reg(s, 0xfe, 0xff, 0x00);  // CISCTL restart asserted, page 0
    vTaskDelay(pdMS_TO_TICKS(20));
    s->set_reg(s, 0xfe, 0xff, 0x10);  // CISCTL restart released, page 0
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "GC2145 capture engine restarted");
}

void Gc2145Camera::LockExposureWhiteBalance(sensor_t* s) {
    if (!s->set_reg) return;
    s->set_reg(s, 0xfe, 0xff, 0x00);  // select register page 0
    s->set_reg(s, 0xb6, 0x01, 0x00);  // AEC enable (reg 0xb6 bit0) -> 0: hold exposure at the converged value
    s->set_reg(s, 0x82, 0x02, 0x00);  // AWB_en (reg 0x82 bit1) -> 0: hold white balance
    ESP_LOGI(TAG, "AEC/AWB locked — exposure & white balance frozen for a steady preview");
}

// The sensor's first rows come out as a flickering black/white band. Copy the first clean row below
// over them, before anything else looks at the frame, so preview and snapshot both stay clean.
void Gc2145Camera::MaskTopRows(camera_fb_t* fb) const {
    if (cfg_.mask_top_rows <= 0 || !fb || !fb->buf) return;
    const size_t row_bytes = static_cast<size_t>(fb->width) * 2u;
    const size_t need      = static_cast<size_t>(cfg_.mask_top_rows + 1) * row_bytes;
    if (fb->len < need) return;  // frame too small to have a clean row to copy from

    uint8_t* b = static_cast<uint8_t*>(fb->buf);
    const uint8_t* clean = b + static_cast<size_t>(cfg_.mask_top_rows) * row_bytes;
    for (int r = 0; r < cfg_.mask_top_rows; ++r)
        memcpy(b + static_cast<size_t>(r) * row_bytes, clean, row_bytes);
}

void Gc2145Camera::SwapBytes(camera_fb_t* fb) {
    if (!fb || !fb->buf) return;
    uint16_t* p = reinterpret_cast<uint16_t*>(fb->buf);
    for (size_t i = 0, n = fb->len / 2; i < n; ++i) p[i] = __builtin_bswap16(p[i]);
}

void Gc2145Camera::Pause() {
    if (!ready_ || paused_) return;
    cam_stop();
    paused_ = true;
    ESP_LOGI(TAG, "capture paused");
}

void Gc2145Camera::Resume() {
    if (!ready_ || !paused_) return;
    // Frames queued before the pause are still in the driver's queue, and the next fb_get would hand
    // one back: a preview would flash the scene as it was minutes ago, and a snapshot would send it.
    // There can be no more of them than there are frame buffers.
    discard_ = cfg_.fb_count;
    cam_start();
    paused_ = false;
    ESP_LOGI(TAG, "capture resumed");
}

camera_fb_t* Gc2145Camera::Capture() {
    if (!ready_ || paused_) return nullptr;

    camera_fb_t* fb = esp_camera_fb_get();
    while (fb && discard_ > 0) {   // see Resume()
        --discard_;
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
    }
    if (!fb) {
        ESP_LOGW(TAG, "fb_get failed");
        return nullptr;
    }

    MaskTopRows(fb);
    if (cfg_.byte_swap) SwapBytes(fb);

    if (!ae_locked_ && ++frames_ >= cfg_.ae_lock_after_frames) {
        if (sensor_t* s = esp_camera_sensor_get()) {
            LockExposureWhiteBalance(s);
            ae_locked_ = true;
        }
    }
    return fb;
}

void Gc2145Camera::Release(camera_fb_t* fb) {
    if (fb) esp_camera_fb_return(fb);
}

#else   // no esp32-camera component for this board — nothing to build (keeps the TU non-empty)
typedef int gc2145_camera_not_built_t;
#endif
