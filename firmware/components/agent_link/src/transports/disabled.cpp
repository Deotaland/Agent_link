#include "agent_link_transport.h"
#include "agent_link_platform.h"
#include "agent_link_chat.h"
#include "agent_link_network.h"

#if !AGENT_LINK_BUILD_BLE

extern "C" agent_transport_t* agent_transport_ble(void) { return nullptr; }
extern "C" void agent_transport_ble_set_name(const char*) {}
extern "C" void agent_transport_ble_set_recv(void (*)(const uint8_t*, size_t)) {}
extern "C" void agent_transport_ble_set_conn(void (*)(bool)) {}
extern "C" void agent_transport_ble_set_stream_recv(void (*)(agent_stream_t, const uint8_t*, size_t)) {}
extern "C" void agent_transport_ble_set_ready(void (*)(void)) {}
extern "C" uint16_t agent_transport_ble_att_mtu(void) { return 0; }
extern "C" void agent_transport_ble_set_device_info(const char*, const char*, const char*) {}
extern "C" bool agent_transport_ble_l2cap_ready(void) { return false; }
extern "C" bool agent_transport_ble_get_mac(uint8_t[6]) { return false; }
extern "C" void agent_transport_ble_update_battery(uint8_t) {}
extern "C" esp_err_t agent_transport_ble_forget(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
#if !AGENT_LINK_BUILD_WIFI
extern "C" esp_err_t agent_link_chat_create(int, char*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_chat_conversations(int, const char*, char*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_chat_messages(int, const char*, const char*, char*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_chat_send(int, const char*, const char*, char*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_chat_sync_prompt(int, const char*) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_wifi_info(agent_link_wifi_info_t*) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_wifi_setup(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_ble_enable(bool) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool agent_link_ble_enabled(void) { return false; }
extern "C" agent_transport_t* agent_transport_wifi(void) { return nullptr; }
extern "C" esp_err_t agent_transport_wifi_request_realtime_session(int, const char*) {
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" void agent_transport_wifi_set_config(const agent_wifi_config_t*) {}
extern "C" void agent_transport_wifi_set_name(const char*) {}
extern "C" void agent_transport_wifi_set_recv(void (*)(const uint8_t*, size_t)) {}
extern "C" void agent_transport_wifi_set_conn(void (*)(bool)) {}
extern "C" void agent_transport_wifi_set_stream_recv(void (*)(agent_stream_t, const uint8_t*, size_t)) {}
extern "C" void agent_transport_wifi_set_state(void (*)(agent_state_t)) {}
extern "C" void agent_transport_wifi_set_platform(const agent_platform_t*) {}
extern "C" void agent_transport_wifi_set_status(void (*)(const agent_link_status_t*)) {}
extern "C" esp_err_t agent_transport_wifi_forget(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" void agent_transport_wifi_set_audio(void (*)(const uint8_t*, size_t), void (*)(void),
                                               void (*)(void), void (*)(const char*)) {}
extern "C" esp_err_t agent_transport_wifi_request_realtime(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_transport_wifi_end_realtime(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool agent_transport_wifi_realtime_running(void) { return false; }


extern "C" bool agent_link_platform_available(void) { return false; }
extern "C" esp_err_t agent_link_platform_bound_agent(agent_link_platform_agent_t* agent) {
    if (agent) *agent = {};
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t agent_link_platform_agents(int, agent_link_platform_agent_t*, size_t, size_t* count, int* total) {
    if (count) *count = 0;
    if (total) *total = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t agent_link_platform_bind_agent(int) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_platform_conversations(int, size_t, agent_link_platform_conversation_t*, size_t, size_t* count, size_t* total) {
    if (count) *count = 0;
    if (total) *total = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
extern "C" esp_err_t agent_link_platform_create_conversation(int, char*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_link_platform_delete_conversation(int, const char*) { return ESP_ERR_NOT_SUPPORTED; }
#endif
#if !AGENT_LINK_BUILD_MUSE


// Not selected: the core still names these in its transport switch.
extern "C" agent_transport_t* agent_transport_muse(void) { return nullptr; }
extern "C" void agent_transport_muse_set_recv(void (*)(const uint8_t*, size_t)) {}
extern "C" void agent_transport_muse_set_state(void (*)(agent_state_t)) {}
extern "C" void agent_transport_muse_set_status(void (*)(const agent_link_status_t*)) {}
extern "C" void agent_transport_muse_set_audio(void (*)(const uint8_t*, size_t), void (*)(void)) {}
extern "C" esp_err_t agent_transport_muse_forget(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t agent_transport_muse_confirm(void) { return ESP_ERR_INVALID_STATE; }

#endif
