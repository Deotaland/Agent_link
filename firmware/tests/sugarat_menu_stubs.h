#include <map>
namespace agent_system {
bool menu_launcher = true;
Descriptor menu_descriptor{};
bool IsLauncher() {
    return menu_launcher;
}
bool CanReturnToLauncher() {
    return !menu_launcher;
}
const Descriptor& Current() {
    return menu_descriptor;
}
const char* SettingsPartition() {
    return "agent_nvs";
}
esp_err_t CatalogUrl(char* value, size_t) {
    value[0] = 0;
    return ESP_OK;
}
}  // namespace agent_system
struct agent_link_wifi_info_t {
    bool connected = false, setup_active = false;
    char ssid[33]{}, ip[16]{}, setup_ssid[33]{};
    int rssi = 0;
};
agent_link_wifi_info_t test_wifi{};
unsigned wifi_setup_calls = 0;
esp_err_t agent_link_wifi_info(agent_link_wifi_info_t* out) {
    *out = test_wifi;
    return ESP_OK;
}
esp_err_t agent_link_wifi_setup() {
    ++wifi_setup_calls;
    return ESP_OK;
}
struct NativeAppDescription {
    char version[32] = "1.0.0";
};
const NativeAppDescription* esp_app_get_description() {
    static NativeAppDescription value;
    return &value;
}
uint32_t esp_get_free_heap_size() {
    return 1024 * 1024;
}
const char* esp_err_to_name(esp_err_t) {
    return "test-error";
}
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
std::map<int, std::string> menu_handles;
std::map<std::string, int> menu_settings;
unsigned volume_saves = 0;
unsigned brightness_saves = 0;
esp_err_t nvs_open_from_partition(const char* part, const char* name, int, nvs_handle_t* out) {
    *out = menu_handles.size() + 1;
    menu_handles[*out] = std::string(part) + "/" + name + "/";
    return ESP_OK;
}
esp_err_t nvs_get_i8(nvs_handle_t h, const char* key, int8_t* out) {
    auto found = menu_settings.find(menu_handles[h] + key);
    if (found == menu_settings.end())
        return ESP_ERR_INVALID_STATE;
    *out = found->second;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* out) {
    int8_t value;
    auto err = nvs_get_i8(h, key, &value);
    if (err == ESP_OK)
        *out = uint8_t(value);
    return err;
}
esp_err_t nvs_set_i8(nvs_handle_t h, const char* key, int8_t value) {
    if (menu_handles[h] == "agent_nvs/voice/" && !strcmp(key, "vol"))
        ++volume_saves;
    menu_settings[menu_handles[h] + key] = value;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t value) {
    if (menu_handles[h] == "agent_nvs/button_hw/" && !strcmp(key, "bright"))
        ++brightness_saves;
    menu_settings[menu_handles[h] + key] = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
