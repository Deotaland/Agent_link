#include "application.h"
#include "config.h"
#include "amoled_hardware.h"
#include "amoled_ui.h"
#include "amoled_system_menu.h"
#include "amoled_ui_controls.h"
#include "agent_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstdio>

class BadgeStatusApplication : public Application {
public:
    BadgeStatusApplication() {
        hardware_.InitDisplayAndTouch(); events_ = xQueueCreate(8, sizeof(amoled_ui::Input));
        ESP_ERROR_CHECK(events_ ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(amoled_ui::StartUi(&hardware_.panel_, &hardware_.touch_, Input, this));
        ESP_ERROR_CHECK(xTaskCreate(Entry, "badge_status", 8192, this, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    const char* Name() const override { return AMOLED_DEVICE_NAME; }
    const char* Model() const override { return AMOLED_FIRMWARE_MODEL; }
    const agent_platform_t* Platform() const override {
        static const agent_platform_t platform{.base_url=DEOTALAND_PLATFORM_BASE_URL,
            .product_id=AGENT_LINK_PRODUCT_ID, .chip_type="esp32-s3"}; return &platform;
    }
    uint32_t Capabilities() const override {
        return (hardware_.panel_.Ready() ? AGENT_CAP_SCREEN : 0) |
               ((hardware_.touch_.Ready() || hardware_.button_ok_) ? AGENT_CAP_BUTTON : 0);
    }
    void OnLinkStatus(const agent_link_status_t& status) override { amoled_ui::SetStatus(status); }
    void ShowText(const char* text) override { amoled_ui::Toast(text, 5000); }
private:
    static bool Input(const amoled_ui::Input& input, void* ctx) {
        return xQueueSend(static_cast<BadgeStatusApplication*>(ctx)->events_, &input, 0) == pdTRUE;
    }
    static void Entry(void* ctx) { static_cast<BadgeStatusApplication*>(ctx)->Loop(); }
    void Loop() {
        AmoledSystemMenu menu; amoled_ui::ButtonDebounce button;
        uint32_t view = 1, navigation = 1;
        while (true) {
            amoled_ui::Input input;
            if (xQueueReceive(events_, &input, pdMS_TO_TICKS(50)) == pdTRUE &&
                amoled_ui::CurrentInput(input, view, esp_timer_get_time())) {
                ++view; ++navigation;
                if (input.action == amoled_ui::TouchAction::kSystem) menu.Open();
                else if (menu.Active()) menu.Handle(input);
                else if (input.action == amoled_ui::TouchAction::kHome) (void)agent_system::ReturnToLauncher();
                else menu.Open();
            }
            if (button.Update(hardware_.ButtonDown(), esp_timer_get_time())) { menu.Open(); ++view; ++navigation; }
            if (menu.TakeExit()) (void)agent_system::ReturnToLauncher();
            amoled_ui::Screen screen{};
            screen.kind = amoled_ui::View::kInfo; screen.view = view; screen.navigation = navigation;
            snprintf(screen.title, sizeof screen.title, "%s", amoled_ui::Tr("设备状态", "Device status"));
            snprintf(screen.detail, sizeof screen.detail, "%s", BOARD_DISPLAY_NAME);
            snprintf(screen.info, sizeof screen.info, "360 x 360  /  ST77916\nFlash 32 MB\nPSRAM %u / %u KB",
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024), unsigned(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)/1024));
            snprintf(screen.footer, sizeof screen.footer, "%s", amoled_ui::Tr("下拉设置 · 上滑回桌面", "Pull for settings · Swipe up for home"));
            if (menu.Active()) menu.Render(screen, view, navigation, -1, false);
            amoled_ui::SetScreen(screen);
        }
    }
    AmoledHardware hardware_; QueueHandle_t events_ = nullptr;
};
DECLARE_APPLICATION(BadgeStatusApplication);
