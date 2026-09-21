// Korvo: a phone-style home screen you swipe between four apps on — a live GC2145 preview, an ASR
// app that streams the mic up the App's L2CAP channel for live transcription, a Command app that
// sends a spoken command over the GATT voice channel instead, and a Recorder that writes WAVs to
// the TF card. The LCD is natively 240x280 portrait but driven rotated 90° counter-clockwise, so
// 280x240 landscape.
//
// The drivers are the shared ones in boards/common (Gc2145Camera / St7789Lcd / Cst816Touch /
// EsCodec); the UI and the app switching live in korvo_ui.{h,cc}, and everything to do with sound
// in korvo_audio.{h,cc}. What is left here is pins, bring-up order, and the preview task.
//
// Bring-up order matters: the camera goes first because esp32-camera creates the I2C bus on
// GPIO17/18 that both the touch controller and the audio codec then attach to.

#include "board.h"
#include "config.h"
#include "cst816_touch.h"
#include "gc2145_camera.h"
#include "korvo_audio.h"
#include "korvo_ui.h"
#include "recordings.h"
#include "st7789_lcd.h"

#include "agent_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"

#include <atomic>
#include <cstdlib>

#define TAG "Korvo"

class KorvoBoard : public Board {
public:
    KorvoBoard() {
        if (lcd_.Init(MakeLcdConfig()) != ESP_OK) {
            ESP_LOGE(TAG, "LCD init failed");
            return;
        }
        lcd_ok_ = true;

        // Also the thing that creates the shared I2C bus, so a failure here takes the touch panel
        // and the codec down with it — there is no bus left for them to attach to. Bail out rather
        // than limp on with three drivers all reporting the same root cause.
        if (cam_.Init(MakeCameraConfig()) != ESP_OK) {
            ESP_LOGE(TAG, "camera init failed");
            lcd_.FillSolid(rgb565::kRed);  // red screen = the panel is alive but the sensor is not
            return;
        }
        cam_ok_ = true;

        InitTouch();                // shares the I2C bus esp_camera_init() just created
        audio_ok_ = (audio_.Init(MakeAudioConfig()) == ESP_OK);
        InitRecordings();
        RegisterCaptureEndpoint();  // Agent/App -> snapshot (MCP actuator "camera0")

        if (korvo_ui::Start(&lcd_, touch_ok_ ? &touch_ : nullptr,
                            audio_ok_ ? &audio_ : nullptr) != ESP_OK) {
            ESP_LOGE(TAG, "UI start failed");
            return;
        }
        xTaskCreate(&KorvoBoard::PreviewTaskEntry, "cam_preview", 4096, this, 5, &preview_task_);
    }

    const char* Name() const override { return "KORVO"; }

    // Camera + screen, plus the codec's speaker and mic. RECORDING says the board can capture to
    // its own storage and is what makes the SD recorder discoverable to the platform.
    uint32_t Capabilities() const override {
        uint32_t c = AGENT_CAP_CAMERA | AGENT_CAP_SCREEN;
        if (audio_ok_) c |= AGENT_CAP_SPEAKER | AGENT_CAP_MIC | AGENT_CAP_RECORDING;
        return c;
    }

    // Agent -> device: spoken reply. Returns immediately; the codec is written from an audio task.
    void PlayAudio(const uint8_t* pcm16, size_t bytes) override {
        if (audio_ok_) audio_.PlayPcm(pcm16, bytes);
    }
    void AudioEnd() override {
        ESP_LOGI(TAG, "AudioEnd (buffer plays out, then silence)");
    }

    // Agent -> device: "start/stop listening" (commands 0x3C/0x3D). Drives the same ASR uplink the
    // ASR app's button does, so the App and the user reach for one mechanism, not two.
    void OnListen(bool start, uint32_t max_ms) override {
        if (!audio_ok_) return;
        ESP_LOGI(TAG, "OnListen(%s, max_ms=%u) from the App", start ? "start" : "stop",
                 static_cast<unsigned>(max_ms));
        if (start) {
            audio_.StartAsr();
            korvo_ui::OpenApp(korvo_ui::Mode::kAsr);  // show what the board is doing
        } else {
            audio_.StopAsr();
        }
    }

    void OnLinkState(bool connected) override { korvo_ui::SetLinkState(connected); }

    // The SDK consults this before falling back to its own answer, which for the RoRoLee
    // production command IDs is a deliberate 1001. Claiming 0x04 is what lets the App list what is
    // on this card. Everything else stays unclaimed so the SDK keeps answering honestly.
    bool OnCommand(uint16_t cmd, const uint8_t* payload, size_t len,
                   uint8_t* resp, size_t resp_cap, size_t* resp_len) override {
        if (cmd != 0x04) return false;
        ESP_LOGI(TAG, "cmd 0x%02X ListRecordings (%uB payload)", cmd, static_cast<unsigned>(len));
        uint16_t error = 0;
        if (recordings_.HandleList(payload, len, resp, resp_cap, resp_len, &error)) return true;
        // HandleList failed. Returning false would make the SDK answer 1001 UnknownCommand, which
        // is the wrong story — the command IS implemented, this attempt failed. There is no way to
        // hand a specific error code back through on_command, so report success with an empty
        // listing: the App sees zero recordings, which is what a missing card actually means.
        ESP_LOGW(TAG, "ListRecordings failed (error=%u) — answering with an empty page", error);
        if (resp_cap < 7) { *resp_len = 0; return true; }
        resp[0] = 0; resp[1] = 0;        // entry_count = 0
        resp[2] = 0;                     // has_more = 0
        resp[3] = 0; resp[4] = 0;        // total_count = 0
        resp[5] = 0; resp[6] = 0;        // next_offset = 0
        *resp_len = 7;
        return true;
    }

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
        c.swap_xy      = DISPLAY_SWAP_XY; // rotate 90° CCW in the panel's MADCTL — see config.h
        c.mirror_x     = DISPLAY_MIRROR_X;
        c.mirror_y     = DISPLAY_MIRROR_Y;
        c.gap_x        = DISPLAY_GAP_X;   // 280-row panel sits 20 into the 320 GRAM; swap_xy puts that on x
        c.gap_y        = DISPLAY_GAP_Y;
        c.stripe_rows  = DISPLAY_STRIPE_ROWS;  // halved from the default to leave BLE its internal RAM
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
        // Left off on purpose: a snapshot's JPEG encode needs the sensor's original byte order, so
        // the swap (when the panel needs one) happens in the preview loop, after SendSnapshot().
        c.byte_swap = false;
        return c;
    }

    static KorvoAudio::Config MakeAudioConfig() {
        KorvoAudio::Config c = {};
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
        c.sd_clk      = SD_PIN_CLK;
        c.sd_cmd      = SD_PIN_CMD;
        c.sd_d0       = SD_PIN_D0;
        c.sd_mount    = SD_MOUNT_POINT;
        c.rec_dir     = SD_REC_DIR;
        c.msg_dir     = SD_MSG_DIR;
        c.rec_format  = REC_FORMAT_ADPCM ? WavRecorder::Format::kAdpcm : WavRecorder::Format::kPcm16;
        return c;
    }

    // Attach the touch controller to the I2C bus esp32-camera created for SCCB — same GPIO17/18, so
    // a second bus is not an option. Failure is survivable: korvo_ui then boots into the camera app.
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
        if (!touch_ok_) ESP_LOGW(TAG, "touch unavailable — the home screen would be a dead end, "
                                      "so the camera preview will run on its own");
    }

    void InitRecordings() {
        Recordings::Config rc = {};
        rc.dir          = SD_REC_DIR;
        rc.messages_dir = SD_MSG_DIR;
        rc.sample_rate  = AUDIO_SAMPLE_RATE;
        rc.format       = REC_FORMAT_ADPCM ? WavRecorder::Format::kAdpcm : WavRecorder::Format::kPcm16;
        recordings_.Init(rc);
    }

    void RegisterCaptureEndpoint() {
        static agent_link_io_desc_t desc = {};
        desc.id           = "camera0";
        desc.dir          = AGENT_IO_OUT;
        desc.kind         = "camera.capture";
        desc.value        = AGENT_VAL_BOOL;
        desc.desc         = "Capture a still image from the camera and upload it to the app";
        desc.display_name = "Camera Snapshot";
        agent_link_register_io(&desc, &KorvoBoard::OnCaptureCmd, this);
    }

    // 0x33 IoActuate for "camera0". Runs on an SDK thread, so it only raises a flag; the preview
    // loop owns the frame buffers and does the capture + encode + send.
    static void OnCaptureCmd(const char* /*id*/, const uint8_t* /*args*/, size_t /*len*/, void* ctx) {
        auto* self = static_cast<KorvoBoard*>(ctx);
        if (self) {
            self->capture_req_.store(true, std::memory_order_release);
            ESP_LOGI(TAG, "snapshot requested (App)");
        }
    }

    // Encode the current RGB565 frame to JPEG and hand it to the SDK's image channel. Must run on
    // the original frame bytes, before any LCD byte-swap. send_image returns fast (async worker).
    void SendSnapshot(camera_fb_t* fb) {
        uint8_t* jpg    = nullptr;
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

    static void PreviewTaskEntry(void* arg) { static_cast<KorvoBoard*>(arg)->PreviewLoop(); }

    void PreviewLoop() {
        ESP_LOGI(TAG, "preview task started (%dx%d at %d,%d on a %dx%d panel, rotated 90° CCW)",
                 PREVIEW_WIDTH, PREVIEW_HEIGHT, PREVIEW_X, PREVIEW_Y, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        while (true) {
            // Another app is on screen: leave the sensor alone and the panel to LVGL. A snapshot
            // asked for by the Agent still has to work, so that is the one reason to wake up.
            if (korvo_ui::CurrentMode() != korvo_ui::Mode::kCamera &&
                !capture_req_.load(std::memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            camera_fb_t* fb = cam_.Capture();  // top rows masked, AEC/AWB frozen once converged
            if (!fb) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Encode + send from the ORIGINAL RGB565, before the byte-swap below would corrupt the colors.
            if (capture_req_.exchange(false, std::memory_order_acq_rel)) SendSnapshot(fb);

#if CAMERA_RGB565_BYTE_SWAP
            Gc2145Camera::SwapBytes(fb);  // little-endian sensor pixels -> big-endian panel
#endif
            // Serialised against LVGL's flush, and a no-op if another app came up mid-frame.
            (void)korvo_ui::DrawCameraFrame(PREVIEW_X, PREVIEW_Y,
                                            static_cast<uint16_t>(fb->width),
                                            static_cast<uint16_t>(fb->height), fb->buf);
            cam_.Release(fb);
        }
    }

    St7789Lcd    lcd_;
    Gc2145Camera cam_;
    Cst816Touch  touch_;
    KorvoAudio   audio_;
    Recordings   recordings_;
    bool         lcd_ok_   = false;
    bool         cam_ok_   = false;
    bool         touch_ok_ = false;
    bool         audio_ok_ = false;
    TaskHandle_t preview_task_ = nullptr;
    std::atomic<bool> capture_req_{false};  // set by the App command; consumed by the preview loop
};

DECLARE_BOARD(KorvoBoard);
