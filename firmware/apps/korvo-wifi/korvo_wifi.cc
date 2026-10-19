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
#include <cstdio>

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
        if (servo_queue_) c |= AGENT_CAP_ACTUATOR;
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
    using ServoCommand = KorvoCloudServo8::Command;
    using ServoMode = KorvoCloudServo8::Mode;

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
        servo_queue_ = xQueueCreate(16, sizeof(ServoCommand));
        if (!servo_queue_ ||
            xTaskCreate(&KorvoCloudBoard::ServoTaskEntry, "servo8", 6144, this, 5, nullptr) != pdPASS) {
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
        static agent_link_io_desc_t control = {};
        control.id = "servo_control";
        control.dir = AGENT_IO_OUT;
        control.kind = "servo.control";
        control.value = AGENT_VAL_BLOB;
        control.display_name = "Servo control SS1-SS8";
        control.args_schema = R"({"channel":"i32","mode":"i32","value":"i32"})";
        control.desc = "channel: 1-8 or 255 for all (same mode on every servo). "
            "mode: 0 release (value 0), 1 position 0-180 degrees, 2 position 0-360 degrees, "
            "3 continuous speed -100 to 100 (0 stops with neutral pulses). "
            "Choose a mode matching the attached servo. Results report on servo_status.";
        static agent_link_io_desc_t refresh = {};
        refresh.id = "servo_refresh";
        refresh.dir = AGENT_IO_OUT;
        refresh.kind = "servo.refresh";
        refresh.value = AGENT_VAL_BOOL;
        refresh.display_name = "Read servo board status";
        refresh.desc = "Send true to read all eight commanded modes and values into servo_status. "
            "These are controller settings, not measured positions.";
        static agent_link_io_desc_t status = {};
        status.id = "servo_status";
        status.dir = AGENT_IO_IN;
        status.kind = "servo.status";
        status.value = AGENT_VAL_STR;
        status.event = AGENT_EVT_ON_CHANGE;
        status.display_name = "Servo board status";
        status.desc = "JSON: write_error, read_error; on read success last_id (0-7 or 255), "
            "last_mode, last_value and channels [{channel,mode,value}]. "
            "Controller settings only; not measured servo positions.";
        if (agent_link_register_io(&control, &KorvoCloudBoard::OnServoControl, this) != ESP_OK ||
            agent_link_register_io(&refresh, &KorvoCloudBoard::OnServoRefresh, this) != ESP_OK ||
            agent_link_register_io(&status, nullptr, this) != ESP_OK)
            ESP_LOGE(TAG, "extended servo endpoint registration failed");
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

    static int32_t ReadServoArgument(const uint8_t* bytes) {
        const uint32_t raw = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
        return raw <= INT32_MAX ? static_cast<int32_t>(raw) :
            static_cast<int32_t>(static_cast<int64_t>(raw) - 0x100000000LL);
    }

    static void OnServoControl(const char*, const uint8_t* args, size_t len, void* ctx) {
        auto* self = static_cast<KorvoCloudBoard*>(ctx);
        if (!self || !args || len != 12) return;
        const int32_t channel = ReadServoArgument(args);
        const int32_t mode = ReadServoArgument(args + 4);
        const int32_t value = ReadServoArgument(args + 8);
        if ((channel != KorvoCloudServo8::kAll && (channel < 1 || channel > 8)) ||
            mode < 0 || mode > 3 ||
            !KorvoCloudServo8::Valid(static_cast<uint8_t>(channel), static_cast<ServoMode>(mode), value)) {
            ESP_LOGW(TAG, "invalid servo control arguments");
            return;
        }
        if (!self->QueueServo({static_cast<uint8_t>(channel), static_cast<ServoMode>(mode),
                               static_cast<int16_t>(value)}))
            ESP_LOGW(TAG, "servo control queue full");
    }

    static void OnServoRefresh(const char*, const uint8_t* args, size_t len, void* ctx) {
        auto* self = static_cast<KorvoCloudBoard*>(ctx);
        if (self && args && len == 1 && args[0] == 1 &&
            !self->QueueServo({1, ServoMode::Released, 0, true}))
            ESP_LOGW(TAG, "servo refresh queue full");
    }

    bool QueueServo(const ServoCommand& command) {
        if (!servo_queue_ || (!command.read_only &&
            !KorvoCloudServo8::Valid(command.channel, command.mode, command.value))) return false;
        return xQueueSend(servo_queue_, &command, 0) == pdPASS;
    }

    bool QueueServoAngle(uint8_t channel, uint8_t angle) {
        return QueueServo({channel, ServoMode::Position180, angle});
    }

    static bool OnLocalServoCmd(const ServoCommand& command, void* ctx) {
        auto* self = static_cast<KorvoCloudBoard*>(ctx);
        return self && self->QueueServo(command);
    }

    static void ReportServoStatus(esp_err_t write_error, esp_err_t read_error,
                                  const KorvoCloudServo8::Status& status) {
        char json[640];
        int used = snprintf(json, sizeof json, "{\"write_error\":%d,\"read_error\":%d",
                            write_error, read_error);
        if (read_error == ESP_OK) {
            used += snprintf(json + used, sizeof json - used,
                ",\"last_id\":%u,\"last_mode\":%u,\"last_value\":%d,\"channels\":[",
                status.last_id, static_cast<unsigned>(status.last_mode), status.last_value);
            for (unsigned i = 0; i < KorvoCloudServo8::kChannels; ++i) {
                used += snprintf(json + used, sizeof json - used,
                    "%s{\"channel\":%u,\"mode\":%u,\"value\":%d}", i ? "," : "", i + 1,
                    static_cast<unsigned>(status.channels[i].mode), status.channels[i].value);
            }
            used += snprintf(json + used, sizeof json - used, "]");
        }
        used += snprintf(json + used, sizeof json - used, "}");
        (void)agent_link_push_reading("servo_status", json, used);
    }

    static void ServoTaskEntry(void* arg) {
        auto* self = static_cast<KorvoCloudBoard*>(arg);
        ServoCommand command = {};
        while (true) {
            if (xQueueReceive(self->servo_queue_, &command, portMAX_DELAY) != pdTRUE) continue;
            const esp_err_t err = command.read_only ? ESP_OK :
                self->servo_.Write(command.channel, command.mode, command.value);
            if (err != ESP_OK) ESP_LOGE(TAG, "servo%u write failed: %s", command.channel, esp_err_to_name(err));
            KorvoCloudServo8::Status status = {};
            const esp_err_t read_error = self->servo_.ReadStatus(status);
            if (read_error != ESP_OK) ESP_LOGW(TAG, "servo status read failed: %s", esp_err_to_name(read_error));
            kc_ui::SetServoResult(command, err, read_error == ESP_OK ? &status : nullptr, read_error);
            ReportServoStatus(err, read_error, status);
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
