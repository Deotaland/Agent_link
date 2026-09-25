// korvo-cloud: the board that reaches the Deotaland platform over the WiFi channel

#include "board.h"
#include "config.h"
#include "cst816_touch.h"
#include "gc2145_camera.h"
#include "korvo_cloud_audio.h"
#include "korvo_cloud_ui.h"
#include "st7789_lcd.h"

#include "agent_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"

#include <atomic>
#include <cstdlib>

#define TAG "KorvoCloud"

class KorvoCloudBoard : public Board {
public:
    KorvoCloudBoard() {
        if (lcd_.Init(MakeLcdConfig()) != ESP_OK) {
            ESP_LOGE(TAG, "LCD init failed");
            return;
        }
        lcd_ok_ = true;

        // esp_camera_init() also creates the I2C bus that touch and the codec use, so there is no
        // point going on without it.
        if (cam_.Init(MakeCameraConfig()) != ESP_OK) {
            ESP_LOGE(TAG, "camera init failed");
            lcd_.FillSolid(rgb565::kRed);  // red screen = the panel is alive but the sensor is not
            return;
        }
        cam_ok_ = true;

        InitTouch();                // shares the I2C bus esp_camera_init() just created
        audio_ok_ = (audio_.Init(MakeAudioConfig()) == ESP_OK);
        RegisterCaptureEndpoint();  // platform -> snapshot (device-I/O actuator "camera0")

        if (kc_ui::Start(&lcd_, touch_ok_ ? &touch_ : nullptr,
                         audio_ok_ ? &audio_ : nullptr) != ESP_OK) {
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
    void AudioEnd() override { ESP_LOGI(TAG, "AudioEnd (buffer plays out, then silence)"); }

    void OnListen(bool start, uint32_t max_ms) override {
        if (!audio_ok_) return;
        ESP_LOGI(TAG, "OnListen(%s, max_ms=%u)", start ? "start" : "stop",
                 static_cast<unsigned>(max_ms));
        if (start) audio_.StartAsr();
        else       audio_.StopAsr();
    }

    void OnLinkState(bool connected) override { kc_ui::SetLinkState(connected); }

private:
    static St7789LcdConfig MakeLcdConfig() {
        St7789LcdConfig c = {};
        c.spi_host     = DISPLAY_SPI_HOST;
        c.pin_sck      = DISPLAY_SCK_PIN;
        c.pin_mosi     = DISPLAY_MOSI_PIN;
        c.pin_cs       = DISPLAY_CS_PIN;
        c.pin_dc       = DISPLAY_DC_PIN;
        c.pin_rst      = DISPLAY_RST_PIN;
        c.pin_bl       = DISPLAY_BL_PIN;
        c.width        = DISPLAY_WIDTH;
        c.height       = DISPLAY_HEIGHT;
        c.pclk_hz      = DISPLAY_SPI_CLK_HZ;
        c.spi_mode     = DISPLAY_SPI_MODE;
        c.invert_color = true;            // ST7789 default
        c.swap_xy      = DISPLAY_SWAP_XY; // rotate 90 CCW in the panel's MADCTL - see config.h
        c.mirror_x     = DISPLAY_MIRROR_X;
        c.mirror_y     = DISPLAY_MIRROR_Y;
        c.gap_x        = DISPLAY_GAP_X;   // 280-row panel sits 20 into the 320 GRAM; swap_xy puts that on x
        c.gap_y        = DISPLAY_GAP_Y;
        c.stripe_rows  = DISPLAY_STRIPE_ROWS;  // halved from the default to leave WiFi its internal RAM
        return c;
    }

    static Gc2145Config MakeCameraConfig() {
        Gc2145Config c = {};
        c.pin_pwdn     = CAM_PIN_PWDN;
        c.pin_reset    = CAM_PIN_RESET;
        c.pin_xclk     = CAM_PIN_XCLK;
        c.pin_sccb_sda = CAM_PIN_SIOD;
        c.pin_sccb_scl = CAM_PIN_SIOC;
        c.pin_d7       = CAM_PIN_D7;
        c.pin_d6       = CAM_PIN_D6;
        c.pin_d5       = CAM_PIN_D5;
        c.pin_d4       = CAM_PIN_D4;
        c.pin_d3       = CAM_PIN_D3;
        c.pin_d2       = CAM_PIN_D2;
        c.pin_d1       = CAM_PIN_D1;
        c.pin_d0       = CAM_PIN_D0;
        c.pin_vsync    = CAM_PIN_VSYNC;
        c.pin_href     = CAM_PIN_HREF;
        c.pin_pclk     = CAM_PIN_PCLK;
        c.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
        c.pixel_format = PIXFORMAT_RGB565;  // straight to the LCD, no decode
        c.frame_size   = FRAMESIZE_240X240; // 1:1 with the panel's short edge
        c.fb_count     = 2;                 // double-buffer in PSRAM
        c.mask_top_rows        = CAMERA_MASK_TOP_ROWS;
        c.ae_lock_after_frames = CAMERA_AE_LOCK_FRAMES;
        // Off: JPEG encoding needs the sensor's byte order, so the preview loop swaps after
        // SendSnapshot() instead.
        c.byte_swap = false;
        return c;
    }

    static KorvoCloudAudio::Config MakeAudioConfig() {
        KorvoCloudAudio::Config c = {};
        c.i2c_port    = static_cast<i2c_port_t>(AUDIO_I2C_PORT);
        c.pin_mclk    = AUDIO_I2S_MCLK;
        c.pin_bclk    = AUDIO_I2S_BCLK;
        c.pin_ws      = AUDIO_I2S_WS;
        c.pin_din     = AUDIO_I2S_DIN;
        c.pin_dout    = AUDIO_I2S_DOUT;
        c.pin_pa_en   = AUDIO_PA_EN;
        c.es7210_addr = ES7210_ADDR;
        c.es8311_addr = ES8311_ADDR;
        c.sample_rate = AUDIO_SAMPLE_RATE;
        c.mic_gain    = AUDIO_MIC_GAIN;
        c.out_volume  = AUDIO_OUT_VOLUME;
        c.play_buf_bytes = AUDIO_PLAY_BUF_BYTES;
        return c;
    }

    // Touch shares the camera's SCCB bus (GPIO17/18). Without touch the status screen still works,
    // but the camera page cannot be opened.
    void InitTouch() {
        Cst816Config c = {};
        c.i2c_port   = TOUCH_I2C_PORT;
        c.addr       = TOUCH_I2C_ADDR;
        c.pin_rst    = TOUCH_RST_PIN;
        c.swap_xy    = TOUCH_SWAP_XY;
        c.mirror_x   = TOUCH_MIRROR_X;
        c.mirror_y   = TOUCH_MIRROR_Y;
        c.out_width  = DISPLAY_WIDTH;
        c.out_height = DISPLAY_HEIGHT;
        c.log_raw    = TOUCH_LOG_RAW;
        touch_ok_ = (touch_.Init(c) == ESP_OK);
        if (!touch_ok_) ESP_LOGW(TAG, "touch unavailable - status screen only, no camera");
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
                cam_.Pause();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            cam_.Resume();   // also drops frames captured before the pause

            camera_fb_t* fb = cam_.Capture();  // top rows masked, AEC/AWB frozen once converged
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
            cam_.Release(fb);
        }
    }

    St7789Lcd       lcd_;
    Gc2145Camera    cam_;
    Cst816Touch     touch_;
    KorvoCloudAudio audio_;
    bool            lcd_ok_   = false;
    bool            cam_ok_   = false;
    bool            touch_ok_ = false;
    bool            audio_ok_ = false;
    TaskHandle_t    preview_task_ = nullptr;
    std::atomic<bool> capture_req_{false};  // set by the platform command; consumed by the preview loop
};

DECLARE_BOARD(KorvoCloudBoard);
