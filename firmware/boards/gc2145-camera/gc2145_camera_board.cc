// GC2145 Camera: live camera preview on an ST7789 240x240 SPI LCD
//
// A minimal example board. The sensor and the panel are both driven by the shared drivers in
// boards/common (Gc2145Camera / St7789Lcd), so what is left here is pins, the WS2812 status LED,
// and a task that continuously grabs frames and blits them to the ST7789.
// Frames live in PSRAM; the ST7789 driver blits them out in stripes via an internal DMA buffer.

#include "board.h"
#include "config.h"
#include "gc2145_camera.h"
#include "st7789_lcd.h"
#include "ws2812_led.h"

#include "agent_link.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"

#include <atomic>
#include <cstdlib>

#define TAG "Gc2145Cam"

static const TickType_t kAutoPromptInterval = pdMS_TO_TICKS(10000); // 10s
static constexpr const char *kAutoPromptText = "拍一张照片，然后分析照片中有没有人";

class Gc2145CameraBoard : public Board
{
public:
    Gc2145CameraBoard()
    {
        InitLed(); // independent of the camera; also doubles as a bring-up/fault indicator
        if (lcd_.Init(MakeLcdConfig()) != ESP_OK)
        {
            ESP_LOGE(TAG, "LCD init failed");
            return;
        }
        lcd_ok_ = true;
        if (cam_.Init(MakeCameraConfig()) != ESP_OK)
        {
            ESP_LOGE(TAG, "camera init failed");
            lcd_.FillSolid(rgb565::kRed);
            (void)led_.SetColor(16, 0, 0); // dim red = camera fault
            return;
        }
        cam_ok_ = true;
        (void)led_.SetColor(0, 16, 0);                // dim green = camera up
        last_auto_prompt_tick_ = xTaskGetTickCount(); // first auto prompt fires ~10s out, not at boot
        InitButton();                                 // BOOT key -> snapshot
        RegisterCaptureEndpoint();                    // App/LLM -> snapshot (MCP actuator "camera0")
        xTaskCreate(&Gc2145CameraBoard::PreviewTaskEntry, "cam_preview", 4096, this, 5, &preview_task_);
    }

    const char *Name() const override { return "GC2145_CAMERA"; }

    // Hardware present: a camera, a screen, and the onboard WS2812 RGB LED
    uint32_t Capabilities() const override { return AGENT_CAP_CAMERA | AGENT_CAP_SCREEN | AGENT_CAP_LED; }

    // Agent -> Device: the SDK's synthetic "led0" endpoint routes here (0x00RRGGBB) -> drive the WS2812.
    void SetLed(uint32_t rgb) override { (void)led_.SetRgb(rgb); }

private:
    static St7789LcdConfig MakeLcdConfig()
    {
        St7789LcdConfig c = {};
        c.spi_host = DISPLAY_SPI_HOST;
        c.pin_sck = DISPLAY_SCK_PIN;
        c.pin_mosi = DISPLAY_MOSI_PIN;
        c.pin_cs = DISPLAY_CS_PIN;
        c.pin_dc = DISPLAY_DC_PIN;
        c.pin_rst = DISPLAY_RST_PIN;
        c.pin_bl = DISPLAY_BL_PIN;
        c.width = DISPLAY_WIDTH;
        c.height = DISPLAY_HEIGHT;
        c.pclk_hz = DISPLAY_SPI_CLK_HZ;
        c.spi_mode = DISPLAY_SPI_MODE;
        c.invert_color = true; // ST7789 default
        return c;
    }

    static Gc2145Config MakeCameraConfig()
    {
        Gc2145Config c = {};
        c.pin_pwdn = CAM_PIN_PWDN;
        c.pin_reset = CAM_PIN_RESET;
        c.pin_xclk = CAM_PIN_XCLK;
        c.pin_sccb_sda = CAM_PIN_SIOD;
        c.pin_sccb_scl = CAM_PIN_SIOC;
        c.pin_d7 = CAM_PIN_D7;
        c.pin_d6 = CAM_PIN_D6;
        c.pin_d5 = CAM_PIN_D5;
        c.pin_d4 = CAM_PIN_D4;
        c.pin_d3 = CAM_PIN_D3;
        c.pin_d2 = CAM_PIN_D2;
        c.pin_d1 = CAM_PIN_D1;
        c.pin_d0 = CAM_PIN_D0;
        c.pin_vsync = CAM_PIN_VSYNC;
        c.pin_href = CAM_PIN_HREF;
        c.pin_pclk = CAM_PIN_PCLK;
        c.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
        c.pixel_format = PIXFORMAT_RGB565; // draw straight to the LCD, no decode
        c.frame_size = FRAMESIZE_240X240;  // 1:1 with the screen
        c.fb_count = 2;                    // double-buffer in PSRAM
        c.mask_top_rows = CAMERA_MASK_TOP_ROWS;
        // Snapshots are encoded from the original byte order, so the swap stays in the preview loop.
        c.byte_swap = false;
        return c;
    }

    void RegisterCaptureEndpoint()
    {
        static agent_link_io_desc_t desc = {};
        desc.id = "camera0";
        desc.dir = AGENT_IO_OUT;
        desc.kind = "camera.capture";
        desc.value = AGENT_VAL_BOOL;
        desc.desc = "Capture a still image from the camera and upload it to the app";
        desc.display_name = "Camera Snapshot";
        agent_link_register_io(&desc, &Gc2145CameraBoard::OnCaptureCmd, this);
    }

    // 0x33 IoActuate for "camera0". Runs on an SDK thread ,just flag the preview loop to do the capture + encode + send.
    static void OnCaptureCmd(const char * /*id*/, const uint8_t * /*args*/, size_t /*len*/, void *ctx)
    {
        auto *self = static_cast<Gc2145CameraBoard *>(ctx);
        if (self)
        {
            self->capture_req_.store(true, std::memory_order_release);
            ESP_LOGI(TAG, "snapshot requested (App)");
        }
    }

    // Bring up the onboard WS2812 RGB LED (RMT). Safe if it fails — SetLed() just no-ops then.
    void InitLed()
    {
        if (led_.Init(WS2812_LED_PIN) != ESP_OK)
        {
            ESP_LOGE(TAG, "WS2812 init failed");
            return;
        }
        (void)led_.Off();
        ESP_LOGI(TAG, "WS2812 LED ready on GPIO%d (App endpoint: led0)", (int)WS2812_LED_PIN);
    }

    // Snapshot button, polled in the preview loop with edge detection. This button is ACTIVE-HIGH
    void InitButton()
    {
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << CAPTURE_BUTTON_PIN;
        c.mode = GPIO_MODE_INPUT;
        c.pull_up_en = GPIO_PULLUP_DISABLE;
        c.pull_down_en = GPIO_PULLDOWN_ENABLE; // active-high: idle low, pressed = high
        c.intr_type = GPIO_INTR_DISABLE;       // polled, not interrupt-driven
        gpio_config(&c);
        ESP_LOGI(TAG, "snapshot button ready: press GPIO%d to capture", (int)CAPTURE_BUTTON_PIN);
    }

    // Encode the current RGB565 frame to JPEG and hand it to the SDK's image channel
    // Must run on the ORIGINAL frame bytes, before any LCD byte-swap. send_image returns fast (async worker).
    void SendSnapshot(camera_fb_t *fb)
    {
        uint8_t *jpg = nullptr;
        size_t jpg_len = 0;
        if (!frame2jpg(fb, 80 /*quality*/, &jpg, &jpg_len))
        {
            ESP_LOGE(TAG, "snapshot: frame2jpg failed");
            return;
        }
        agent_stream_opts_t o = {};
        o.encoding = AGENT_ENC_JPEG;
        o.width    = static_cast<uint16_t>(fb->width);
        o.height   = static_cast<uint16_t>(fb->height);
        esp_err_t r = agent_link_stream_send(AGENT_STREAM_IMAGE, &o, jpg, jpg_len);
        ESP_LOGI(TAG, "snapshot: %ux%u -> %uB jpeg, send=%s",
                 (unsigned)fb->width, (unsigned)fb->height, (unsigned)jpg_len, esp_err_to_name(r));
        free(jpg);
    }

    // Text-only: no image leaves the board here. The Agent is expected to call the "camera0" MCP
    // endpoint itself (0x33 IoActuate -> OnCaptureCmd -> SendSnapshot()) to get a fresh frame.
    void PushAutoPrompt()
    {
        esp_err_t r = agent_link_push_prompt(kAutoPromptText);
        ESP_LOGI(TAG, "auto prompt: %s", r == ESP_OK ? "sent" : esp_err_to_name(r));
    }

    static void PreviewTaskEntry(void *arg) { static_cast<Gc2145CameraBoard *>(arg)->PreviewLoop(); }

    void PreviewLoop()
    {
        ESP_LOGI(TAG, "preview loop started");
        bool btn_pressed_prev = false; // active-high button idles low
        while (true)
        {
            // Top rows masked and AEC/AWB frozen once converged — both handled by Gc2145Camera.
            camera_fb_t *fb = cam_.Capture();
            if (!fb)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Snapshot trigger: button rising edge (active-high; polled once per frame = natural debounce) or App command.
            const bool btn_pressed = (gpio_get_level(CAPTURE_BUTTON_PIN) != 0); // high = pressed
            if (!btn_pressed_prev && btn_pressed)
            {
                capture_req_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "snapshot requested (button)");
            }
            btn_pressed_prev = btn_pressed;
            // Encode + send from the ORIGINAL RGB565, before the LCD byte-swap below would corrupt the JPEG colors.
            if (capture_req_.exchange(false, std::memory_order_acq_rel))
                SendSnapshot(fb);

            // Auto prompt on a fixed cadence, independent of button/App triggers.
            // Unsigned tick subtraction stays correct even across a TickType_t wraparound.
            const TickType_t now_tick = xTaskGetTickCount();
            if (static_cast<TickType_t>(now_tick - last_auto_prompt_tick_) >= kAutoPromptInterval)
            {
                last_auto_prompt_tick_ = now_tick;
                PushAutoPrompt();
            }

#if CAMERA_RGB565_BYTE_SWAP
            // Camera RGB565 is little-endian per pixel; ST7789 latches big-endian. Swap in place.
            Gc2145Camera::SwapBytes(fb);
#endif
            // DrawBitmap blocks until the DMA finishes, so returning the fb right after is safe.
            (void)lcd_.DrawBitmap(0, 0, static_cast<uint16_t>(fb->width), static_cast<uint16_t>(fb->height), fb->buf);
            cam_.Release(fb);
        }
    }

    St7789Lcd lcd_;
    Gc2145Camera cam_;
    Ws2812Led led_;
    bool lcd_ok_ = false;
    bool cam_ok_ = false;
    TaskHandle_t preview_task_ = nullptr;
    std::atomic<bool> capture_req_{false}; // set by App command / BOOT button; consumed by the preview loop
    TickType_t last_auto_prompt_tick_ = 0; // xTaskGetTickCount() at the last auto prompt
};

DECLARE_BOARD(Gc2145CameraBoard);
