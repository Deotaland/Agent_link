#include "application.h"
#include "config.h"
#include "ojbadge_hardware.h"
#include "ojbadge_ui.h"
#include "cjk_font.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr char TAG[] = "OJBadge";
struct Input { ojbadge::Action action; int index; uint8_t mac[6]; };
}

class OjBadgeWifi : public Application {
public:
    const char* Name() const override { return "OJBadge"; }
    const char* Model() const override { return "OJBadge"; }
    uint32_t Capabilities() const override {
        return AGENT_CAP_SCREEN | (hardware_.button_ready ? AGENT_CAP_BUTTON : 0) |
            (hardware_.battery.Ready() ? AGENT_CAP_BATTERY : 0);
    }
    int GetBatteryLevel() override { return view_.battery; }
    bool IsCharging() override { return view_.charging; }

    void Run() {
        // Do not erase NVS to recover a version/partition error: report it.
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(hardware_.Init());
        inputs_ = xQueueCreate(8, sizeof(Input));
        ESP_ERROR_CHECK(inputs_ ? ESP_OK : ESP_ERR_NO_MEM);
        lv_init(); lv_tick_set_cb(Tick);
        display_ = lv_display_create(240, 240);
        ESP_ERROR_CHECK(display_ ? ESP_OK : ESP_ERR_NO_MEM);
        lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
        lv_display_set_flush_cb(display_, Flush);
        lv_display_set_user_data(display_, this);
        constexpr size_t bytes = 240 * 40 * 2;
        auto* pixels = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_ERROR_CHECK(pixels ? ESP_OK : ESP_ERR_NO_MEM);
        lv_display_set_buffers(display_, pixels, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
        auto* pointer = lv_indev_create();
        ESP_ERROR_CHECK(pointer ? ESP_OK : ESP_ERR_NO_MEM);
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(pointer, display_);
        lv_indev_set_user_data(pointer, this);
        lv_indev_set_read_cb(pointer, ReadTouch);
        body_ = lv_font_montserrat_14; title_ = lv_font_montserrat_20;
        body_.fallback = LoadCjkFont("anim_pack", 14, 96);
        title_.fallback = LoadCjkFont("anim_pack", 20, 48);
        if (!body_.fallback) body_.fallback = &lv_font_simsun_14_cjk;
        if (!title_.fallback) title_.fallback = &lv_font_simsun_14_cjk;
        view_.touch_ok = hardware_.touch.Ready();
        ojbadge::InitUi(&body_, &title_, OnAction, this);
        Refresh(); ojbadge::UpdateUi(view_); lv_refr_now(display_);
        ESP_ERROR_CHECK(hardware_.panel.Backlight(true));
        ESP_LOGI(TAG, "board=%s model=%s fw=%s transport=local-ESP-NOW; platform disabled", Name(), Model(), esp_app_get_description()->version);
        ESP_LOGI(TAG, "SW2: short press starts/greetings; hold 1s returns home. Pairing confirmation uses touch.");
        uint32_t last_poll = Tick();
        while (true) {
            if (hardware_.touch.Ready()) (void)hardware_.touch.Poll();
            PollButton();
            Input input{};
            while (xQueueReceive(inputs_, &input, 0) == pdTRUE) Handle(input);
            if (Tick() - last_poll >= 200) { last_poll = Tick(); Refresh(); }
            ojbadge::UpdateUi(view_);
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
private:
    static uint32_t Tick() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
    static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
        auto* self = static_cast<OjBadgeWifi*>(lv_display_get_user_data(display));
        lv_draw_sw_rgb565_swap(pixels, lv_area_get_width(area) * lv_area_get_height(area));
        // A failed transfer must not release/reuse a buffer still owned by DMA.
        ESP_ERROR_CHECK(self->hardware_.panel.Draw(area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels));
        lv_display_flush_ready(display);
    }
    static void ReadTouch(lv_indev_t* device, lv_indev_data_t* data) {
        auto* self = static_cast<OjBadgeWifi*>(lv_indev_get_user_data(device));
        int32_t x, y; self->hardware_.touch.GetPoint(&x, &y);
        data->point.x = x; data->point.y = y;
        data->state = self->hardware_.touch.Pressed() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
    static void OnAction(ojbadge::Action action, int index, void* context) {
        auto* self = static_cast<OjBadgeWifi*>(context);
        Input input{}; input.action = action; input.index = index;
        if (action == ojbadge::Action::Select && index >= 0 && index < self->view_.friends.peer_count)
            memcpy(input.mac, self->view_.friends.peers[index].mac, 6);
        if (xQueueSend(self->inputs_, &input, 0) != pdTRUE) ojbadge::Toast("操作繁忙，请重试");
    }
    void Handle(const Input& input) {
        using ojbadge::Action;
        esp_err_t result = ESP_OK;
        switch (input.action) {
        case Action::Start:
            result = agent_link_friends_start();
            if (result == ESP_OK) { view_.home = false; view_.peer_page = 0; }
            break;
        case Action::Home:
            result = agent_link_friends_stop();
            if (result == ESP_OK) view_.home = true;
            break;
        case Action::Select: result = agent_link_friends_select(input.mac); break;
        case Action::Next:
            view_.peer_page = (view_.peer_page + 1) % std::max(1, (view_.friends.peer_count + 1) / 2);
            break;
        case Action::Accept: result = agent_link_friends_confirm(true); break;
        case Action::Reject: result = agent_link_friends_confirm(false); break;
        case Action::Greet: result = agent_link_friends_greet(); break;
        case Action::Disconnect: result = agent_link_friends_disconnect(); break;
        }
        if (result != ESP_OK) ojbadge::Toast(result == ESP_ERR_INVALID_STATE ? "无线切换中，请稍后重试" : "操作失败，请重试");
        Refresh();
    }
    void Refresh() {
        const auto previous = view_.friends;
        agent_link_friends_snapshot(&view_.friends);
        view_.peer_page = std::min(view_.peer_page, std::max(0, (view_.friends.peer_count - 1) / 2));
        if (view_.friends.received > previous.received) ojbadge::Toast("收到伙伴的问候！");
        else if (view_.friends.sent > previous.sent) ojbadge::Toast("问候已送达");
        if (view_.friends.event != previous.event) {
            switch (view_.friends.event) {
            case AGENT_FRIEND_EVENT_REJECTED: ojbadge::Toast("配对已取消"); break;
            case AGENT_FRIEND_EVENT_TIMEOUT: ojbadge::Toast("配对超时，请重新邀请"); break;
            case AGENT_FRIEND_EVENT_LOST: ojbadge::Toast("伙伴已离开，重新寻找"); break;
            case AGENT_FRIEND_EVENT_GREETING_FAILED: ojbadge::Toast("问候未送达，请重试"); break;
            default: break;
            }
        }
        if (!battery_at_ || Tick() - battery_at_ >= 5000) {
            battery_at_ = Tick();
            view_.battery = hardware_.battery.Ready() ? hardware_.battery.Soc() : -1;
            view_.charging = hardware_.battery.Ready() && hardware_.battery.IsCharging();
        }
    }
    void PollButton() {
        if (!hardware_.button_ready) return;
        const bool raw = gpio_get_level(static_cast<gpio_num_t>(USER_BUTTON)) == 0;
        const uint32_t now = Tick();
        if (raw != key_raw_) { key_raw_ = raw; key_changed_ = now; }
        if (now - key_changed_ < 30) return;
        if (raw != key_down_) {
            key_down_ = raw;
            if (raw) { key_start_ = now; key_long_ = false; }
            else if (!key_long_) {
                if (view_.home) OnAction(ojbadge::Action::Start, 0, this);
                else if (view_.friends.state == AGENT_FRIEND_CONNECTED) OnAction(ojbadge::Action::Greet, 0, this);
            }
        }
        if (key_down_ && !key_long_ && now - key_start_ >= 1000) {
            key_long_ = true; OnAction(ojbadge::Action::Home, 0, this);
        }
    }
    OjBadgeHardware hardware_;
    ojbadge::View view_{};
    QueueHandle_t inputs_ = nullptr;
    lv_display_t* display_ = nullptr;
    lv_font_t body_{}, title_{};
    uint32_t battery_at_ = 0, key_changed_ = 0, key_start_ = 0;
    bool key_raw_ = false, key_down_ = false, key_long_ = false;
};

DECLARE_APPLICATION(OjBadgeWifi);

// Local-only entrypoint deliberately omits agent_link_init/start and Platform().
extern "C" void app_main() {
    static_cast<OjBadgeWifi&>(Application::GetInstance()).Run();
}
