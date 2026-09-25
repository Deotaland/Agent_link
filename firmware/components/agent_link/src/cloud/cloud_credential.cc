// The WiFi channel's platform credential. See cloud_credential.h.

#include "cloud_credential.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char* TAG = "agent_link.cred";

// Existing namespace; renaming it would drop the key on devices that are already bound.
constexpr const char* kNs      = "al_dev";
constexpr const char* kKeyAuth = "auth_key";

char s_auth[CLOUD_AUTH_KEY_MAX] = {0};

esp_err_t StoreStr(const char* value) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(kNs, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = (value && value[0]) ? nvs_set_str(h, kKeyAuth, value) : nvs_erase_key(h, kKeyAuth);
    if (r == ESP_ERR_NVS_NOT_FOUND) r = ESP_OK;   // nothing to erase
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}
}  // namespace

esp_err_t cloud_cred_load(void) {
    s_auth[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   // no namespace yet: not bound
    size_t len = sizeof s_auth;
    if (nvs_get_str(h, kKeyAuth, s_auth, &len) != ESP_OK) s_auth[0] = '\0';
    nvs_close(h);
    ESP_LOGI(TAG, "%s", s_auth[0] ? "bound (auth_key stored)" : "not bound");
    return ESP_OK;
}

const char* cloud_cred_auth_key(void) { return s_auth; }
bool        cloud_cred_is_bound(void) { return s_auth[0] != '\0'; }

esp_err_t cloud_cred_set_auth_key(const char* key) {
    if (!key || !key[0]) return ESP_ERR_INVALID_ARG;
    snprintf(s_auth, sizeof s_auth, "%s", key);
    const esp_err_t r = StoreStr(s_auth);
    if (r != ESP_OK) {
        // The key is issued once per claim; without it the device must re-claim after a reboot.
        ESP_LOGE(TAG, "auth_key not persisted: %s", esp_err_to_name(r));
    } else {
        ESP_LOGI(TAG, "bound — auth_key stored");
    }
    return r;
}

esp_err_t cloud_cred_clear(void) {
    s_auth[0] = '\0';
    ESP_LOGW(TAG, "auth_key cleared — the device will ask for a new activation code");
    return StoreStr(nullptr);
}
