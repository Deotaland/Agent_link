// korvo-wifi: the board that reaches the Deotaland platform over the WiFi channel

#include "application.h"
#include "config.h"
#include "korvo_hardware.h"
#include "korvo_wifi_audio.h"
#include "servo8.h"
#include "korvo_wifi_ui.h"

#include "agent_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "img_converters.h"

#include <atomic>
#include <cstdlib>

#define TAG "KorvoCloud"

class KorvoCloudBoard : public Application {
public:
    KorvoCloudBoard() {
        if (hardware_.Init() != ESP_OK) return;
        audio_ok_ = (audio_.Init(MakeAudioConfig()) == ESP_OK);
        InitServo();
        RegisterCaptureEndpoint();  // platform -> snapshot (device-I/O actuator "camera0")

        if (kc_ui::Start(&hardware_.lcd, hardware_.touch_ready ? &hardware_.touch : nullptr,
                         audio_ok_ ? &audio_ : nullptr,
                         servo_queue_ ? &KorvoCloudBoard::OnLocalServoCmd : nullptr, this) != ESP_OK) {
            ESP_LOGE(TAG, "UI start failed");
            return;
        }
        xTaskCreate(&KorvoCloudBoard::PreviewTaskEntry, "cam_preview", 4096, this, 5, &preview_task_);
    }

    // Platform identity: used on WiFi, ignored on BLE. Static because the SDK keeps the pointer.
    const agent_platform_t* Platform() const override {
        static const agent_platform_t kPlatform = {
            .base_url   = CLOUD_BASE_URL,
            .product_id = CLOUD_PRODUCT_ID,
            .chip_type  = CLOUD_CHIP_TYPE,
        };
        return &kPlatform;
    }

    // Runs on a transport task. SetLinkStatus only copies the status, so it never waits on LVGL.
    void OnLinkStatus(const agent_link_status_t& st) override { kc_ui::SetLinkStatus(st); }

    // Also the SoftAP SSID prefix during provisioning: the user looks for "KorvoCloud-XXXX".
    const char* Name() const override { return "KorvoCloud"; }

    // A product of its own: OTA only accepts images built for this model.
    const char* Model() const override { return "KORVO-CLOUD"; }

    // No RECORDING bit: this board has no TF card.
    uint32_t Capabilities() const override {
        uint32_t c = AGENT_CAP_CAMERA | AGENT_CAP_SCREEN;
        if (audio_ok_) c |= AGENT_CAP_SPEAKER | AGENT_CAP_MIC;
        return c;
    }

    void PlayAudio(const uint8_t* pcm16, size_t bytes) override {
        if (audio_ok_) audio_.PlayPcm(pcm16, bytes);
    }
    void AudioEnd() override { if (audio_ok_) audio_.EndPlayback(); }
    void AudioReset() override { if (audio_ok_) audio_.StopPlayback(); }
    void ShowText(const char* utf8) override { kc_ui::SetVoiceText(utf8); }

    void OnListen(bool start, uint32_t max_ms) override {
        if (!audio_ok_) return;
        ESP_LOGI(TAG, "OnListen(%s, max_ms=%u)", start ? "start" : "stop",
                 static_cast<unsigned>(max_ms));
        if (start) audio_.StartAsr();
        else       audio_.StopAsr();
    }

    void OnLinkState(bool connected) override { kc_ui::SetLinkState(connected); }

private:
    struct ServoCommand {
        uint8_t channel;
        uint8_t angle;
    };

    KorvoCloudAudio::Config MakeAudioConfig() {
        KorvoCloudAudio::Config c = {};
        c.codec = hardware_.codec.Ready() ? &hardware_.codec : nullptr;
        c.sample_rate = AUDIO_SAMPLE_RATE;
        c.play_buf_bytes = AUDIO_PLAY_BUF_BYTES;
        return c;
    }

    void RegisterCaptureEndpoint() {
        static agent_link_io_desc_t desc = {};
        desc.id           = "camera0";
        desc.dir          = AGENT_IO_OUT;
        desc.kind         = "camera.capture";
        desc.value        = AGENT_VAL_BOOL;
        desc.desc         = "Capture a still image from the camera and upload it";
        desc.display_name = "Camera Snapshot";
        agent_link_register_io(&desc, &KorvoCloudBoard::OnCaptureCmd, this);
    }

    void InitServo() {
        const esp_err_t err = servo_.Init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "servo I2C init failed: %s", esp_err_to_name(err));
            return;
        }
        servo_queue_ = xQueueCreate(1, sizeof(ServoCommand));
        if (!servo_queue_ ||
            xTaskCreate(&KorvoCloudBoard::ServoTaskEntry, "servo8", 3072, this, 5, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "servo task init failed");
            if (servo_queue_) {
                vQueueDelete(servo_queue_);
                servo_queue_ = nullptr;
            }
            return;
        }
        static agent_link_io_desc_t desc = {};
        desc.id           = "servo1";
        desc.dir          = AGENT_IO_OUT;
        desc.kind         = "servo.angle";
        desc.value        = AGENT_VAL_I32;
        desc.unit         = "deg";
        desc.range_min    = 0;
        desc.range_max    = 180;
        desc.desc         = "Set the servo on SS1 to an angle from 0 to 180 degrees";
        desc.display_name = "Servo SS1";
        if (agent_link_register_io(&desc, &KorvoCloudBoard::OnServoCmd, this) != ESP_OK) {
            ESP_LOGE(TAG, "servo endpoint registration failed");
        }
    }

    static void OnServoCmd(const char* /*id*/, const uint8_t* args, size_t len, void* ctx) {
        auto* self = static_cast<KorvoCloudBoard*>(ctx);
        if (!self || !self->servo_queue_ || !args || len != 4) return;
        const uint32_t angle = static_cast<uint32_t>(args[0]) |
                               (static_cast<uint32_t>(args[1]) << 8) |
                               (static_cast<uint32_t>(args[2]) << 16) |
                               (static_cast<uint32_t>(args[3]) << 24);
        if (angle > 180) {
            ESP_LOGW(TAG, "servo1 angle out of range: %lu", static_cast<unsigned long>(angle));
            return;
        }
        if (!self->QueueServoAngle(1, static_cast<uint8_t>(angle))) {
            ESP_LOGW(TAG, "servo1 queue unavailable");
        }
    }

    bool QueueServoAngle(uint8_t channel, uint8_t angle) {
        if (channel < 1 || channel > 8 || angle > 180 || !servo_queue_) return false;
        const ServoCommand command{channel, angle};
        return xQueueOverwrite(servo_queue_, &command) == pdPASS;
    }

    static bool OnLocalServoCmd(uint8_t channel, uint8_t angle, void* ctx) {
        auto* self = static_cast<KorvoCloudBoard*>(ctx);
        return self && self->QueueServoAngle(channel, angle);
    }

    static void ServoTaskEntry(void* arg) {
        auto* self = static_cast<KorvoCloudBoard*>(arg);
        ServoCommand command = {};
        while (true) {
            if (xQueueReceive(self->servo_queue_, &command, portMAX_DELAY) != pdTRUE) continue;
            const esp_err_t err = self->servo_.SetAngle(command.channel, command.angle);
            if (err == ESP_OK) ESP_LOGI(TAG, "servo%u -> %u degrees", command.channel, command.angle);
            else ESP_LOGE(TAG, "servo%u write failed: %s", command.channel, esp_err_to_name(err));
            kc_ui::SetServoResult(command.channel, command.angle, err);
        }
    }

    // Runs on an SDK thread, so it only raises a flag; the preview loop owns the frame buffers and
    // does the capture + encode + send.
    static void OnCaptureCmd(const char* /*id*/, const uint8_t* /*args*/, size_t /*len*/, void* ctx) {
        auto* self = static_cast<KorvoCloudBoard*>(ctx);
        if (self) {
            self->capture_req_.store(true, std::memory_order_release);
            ESP_LOGI(TAG, "snapshot requested");
        }
    }

    // Encode the current RGB565 frame to JPEG and hand it to the SDK's image channel. Must run on
    // the original frame bytes, before any LCD byte-swap. send returns fast (async worker).
    void SendSnapshot(camera_fb_t* fb) {
        uint8_t* jpg     = nullptr;
        size_t   jpg_len = 0;
        if (!frame2jpg(fb, 80 /*quality*/, &jpg, &jpg_len)) {
            ESP_LOGE(TAG, "snapshot: frame2jpg failed");
            return;
        }
        agent_stream_opts_t o = {};
        o.encoding = AGENT_ENC_JPEG;
        o.width    = static_cast<uint16_t>(fb->width);
        o.height   = static_cast<uint16_t>(fb->height);
        const esp_err_t r = agent_link_stream_send(AGENT_STREAM_IMAGE, &o, jpg, jpg_len);
        ESP_LOGI(TAG, "snapshot: %ux%u -> %uB jpeg, send=%s",
                 (unsigned)fb->width, (unsigned)fb->height, (unsigned)jpg_len, esp_err_to_name(r));
        free(jpg);
    }

    static void PreviewTaskEntry(void* arg) { static_cast<KorvoCloudBoard*>(arg)->PreviewLoop(); }

    void PreviewLoop() {
        ESP_LOGI(TAG, "preview task started (%dx%d at %d,%d on a %dx%d panel)",
                 PREVIEW_WIDTH, PREVIEW_HEIGHT, PREVIEW_X, PREVIEW_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        while (true) {
            // Stop capture while neither the camera page nor a snapshot needs frames; otherwise the
            // driver keeps dropping them (EV-VSYNC-OVF). Pause/Resume do nothing if already in
            // that state.
            const bool wanted = kc_ui::CurrentMode() == kc_ui::Mode::kCamera ||
                                capture_req_.load(std::memory_order_acquire);
            if (!wanted) {
                hardware_.camera.Pause();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            hardware_.camera.Resume();   // also drops frames captured before the pause

            camera_fb_t* fb = hardware_.camera.Capture();  // top rows masked, AEC/AWB frozen once converged
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Encode + send from the ORIGINAL RGB565, before the byte-swap below would corrupt it.
            if (capture_req_.exchange(false, std::memory_order_acq_rel)) SendSnapshot(fb);

#if CAMERA_RGB565_BYTE_SWAP
            Gc2145Camera::SwapBytes(fb);  // little-endian sensor pixels -> big-endian panel
#endif
            // Serialised against LVGL's flush; a no-op once the camera page has been left.
            (void)kc_ui::DrawCameraFrame(PREVIEW_X, PREVIEW_Y,
                                         static_cast<uint16_t>(fb->width),
                                         static_cast<uint16_t>(fb->height), fb->buf);
            hardware_.camera.Release(fb);
        }
    }

    KorvoHardware hardware_;
    KorvoCloudAudio audio_;
    KorvoCloudServo8 servo_;
    bool            audio_ok_ = false;
    TaskHandle_t    preview_task_ = nullptr;
    QueueHandle_t   servo_queue_ = nullptr;
    std::atomic<bool> capture_req_{false};  // set by the platform command; consumed by the preview loop
};

DECLARE_APPLICATION(KorvoCloudBoard);
