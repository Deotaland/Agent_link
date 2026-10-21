#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "agent_link.h"
#include "agent_link_ota.h"
#include "application.h"
#include "platform_config.h"
#include "platform_product.h"

namespace {
constexpr const char* TAG = "agent_link.app";

void on_audio_out(const uint8_t* pcm16, size_t bytes, void*) { Application::GetInstance().PlayAudio(pcm16, bytes); }
void on_audio_end(void*) { Application::GetInstance().AudioEnd(); }
void on_audio_reset(void*) { Application::GetInstance().AudioReset(); }
bool on_voice_reply(uint32_t session_id, uint8_t status, void*) {
    return Application::GetInstance().OnVoiceReply(session_id, status);
}
void on_show_text(const char* utf8, void*) { Application::GetInstance().ShowText(utf8); }
void on_haptic(uint32_t duration_ms, void*) { Application::GetInstance().Vibrate(duration_ms); }
void on_led(uint32_t rgb, void*) { Application::GetInstance().SetLed(rgb); }
void on_listen(bool start, uint32_t max_ms, void*) { Application::GetInstance().OnListen(start, max_ms); }
bool on_command(uint16_t cmd, const uint8_t* payload, size_t len,
                uint8_t* resp, size_t resp_cap, size_t* resp_len, void*) {
    return Application::GetInstance().OnCommand(cmd, payload, len, resp, resp_cap, resp_len);
}

void on_status(const agent_link_status_t* st, void*) {
    if (!st) return;
    ESP_LOGI(TAG, "[link] %s (phase=%d)", st->title, static_cast<int>(st->phase));
    Application::GetInstance().OnLinkStatus(*st);
}

void on_state(agent_state_t state, void*) {
    ESP_LOGI(TAG, "[state] %s",
             state == AGENT_STATE_READY     ? "READY" :
             state == AGENT_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    Application::GetInstance().OnLinkState(state != AGENT_STATE_DISCONNECTED);
}
}  // namespace

extern "C" void app_main(void) {
    Application& board = Application::GetInstance();
    ESP_LOGI(TAG, "board = %s, model = %s, fw = %s, caps = 0x%04x", board.Name(), board.Model(),
             agent_link_ota_running_version(), static_cast<unsigned>(board.Capabilities()));

    //Callback function registration
    agent_output_cb_t out = {};
    out.on_audio_out = on_audio_out;
    out.on_audio_end = on_audio_end;
    out.on_audio_reset = on_audio_reset;
    out.on_voice_reply = on_voice_reply;
    out.on_show_text = on_show_text;
    out.on_haptic    = (board.Capabilities() & AGENT_CAP_HAPTIC) ? on_haptic : nullptr;
    out.on_led       = (board.Capabilities() & AGENT_CAP_LED) ? on_led : nullptr;
    out.on_listen    = on_listen;   // App-initiated listen (0x3C/0x3D); no-op on boards without a mic
    out.on_command   = on_command;  // board-claimed control commands; consulted before the SDK's 1001

    agent_link_config_t cfg = {};
    cfg.device_name = board.Name();
    cfg.caps        = board.Capabilities();
    cfg.output      = &out;
    cfg.on_state    = on_state;
    cfg.on_status   = on_status;       // link progress, same shape on BLE and WiFi
    cfg.model       = board.Model();   // OTA model check + DIS 0x2A24 (firmware_rev: SDK reads PROJECT_VER)
    cfg.platform    = board.Platform();   // WiFi needs it; BLE ignores it — set it either way

    ESP_LOGI(TAG, "platform = %s, model = %s, product_id = %d, product_key = %s",
             cfg.platform ? cfg.platform->base_url : DEOTALAND_PLATFORM_BASE_URL, board.Model(),
             cfg.platform ? cfg.platform->product_id : AGENT_LINK_PRODUCT_ID,
             AGENT_LINK_PRODUCT_KEY[0] ? AGENT_LINK_PRODUCT_KEY : "unregistered");

    // The single transport-aware line in the application: which backend to run. Everything above
    // is set unconditionally and each backend takes what it needs, so a board never branches on it.
#if CONFIG_AGENT_LINK_TRANSPORT_WIFI
    cfg.transport   = AGENT_TRANSPORT_WIFI;
#elif CONFIG_AGENT_LINK_TRANSPORT_MUSE
    cfg.transport   = AGENT_TRANSPORT_MUSE;
#else
    cfg.transport   = AGENT_TRANSPORT_BLE;
#endif

    ESP_ERROR_CHECK(agent_link_init(&cfg));     //agent_link initialization
    ESP_ERROR_CHECK(agent_link_start());

    while (true) {
        if (agent_link_state() == AGENT_STATE_READY) {
            const int batt = board.GetBatteryLevel();
            if (batt >= 0) {
                agent_link_report_battery(static_cast<uint8_t>(batt), board.IsCharging());
            }
            ESP_LOGD(TAG, "battery poll (batt=%d)", batt);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
