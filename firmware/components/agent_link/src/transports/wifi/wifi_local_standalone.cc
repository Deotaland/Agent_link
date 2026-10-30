// Radio lifecycle for local-only applications. Never loads credentials, joins an
// AP, opens a portal, or starts a cloud client. Only the friends worker calls it.
#include "wifi_local_radio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

namespace {
bool initialized = false;
bool started = false;
}

esp_err_t agent_wifi_local_end() {
    if (!started) return ESP_OK;
    const esp_err_t result = esp_wifi_stop();
    if (result == ESP_OK || result == ESP_ERR_WIFI_NOT_STARTED) {
        started = false;
        return ESP_OK;
    }
    return result;
}

esp_err_t agent_wifi_local_begin(uint8_t channel) {
    if (started) return ESP_ERR_INVALID_STATE;
    esp_err_t result;
    if (!initialized) {
        result = esp_netif_init();
        if (result != ESP_OK) return result;
        result = esp_event_loop_create_default();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        config.nvs_enable = false;
        result = esp_wifi_init(&config);
        if (result != ESP_OK) return result;
        initialized = true;
    }
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK) result = esp_wifi_start();
    if (result != ESP_OK) return result;
    started = true;
    result = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) (void)agent_wifi_local_end();
    return result;
}
