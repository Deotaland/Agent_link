#include "wifi_local_radio.h"
#include "esp_wifi.h"
#include <cassert>
#include <cstdio>

namespace {
int inits = 0, starts = 0, stops = 0, netifs = 0, loops = 0;
int init_error = ESP_FAIL, channel_error = ESP_OK, stop_error = ESP_OK;
bool storage = false, station = false, running = false;
}
esp_err_t esp_netif_init() { ++netifs; return ESP_OK; }
esp_err_t esp_event_loop_create_default() { ++loops; return ESP_ERR_INVALID_STATE; }
esp_err_t esp_wifi_init(const wifi_init_config_t* cfg) {
    assert(!cfg->nvs_enable); ++inits; return init_error;
}
esp_err_t esp_wifi_set_storage(int value) { assert(value == WIFI_STORAGE_RAM); storage = true; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int value) { assert(storage && value == WIFI_MODE_STA); station = true; return ESP_OK; }
esp_err_t esp_wifi_start() { assert(station && !running); running = true; ++starts; return ESP_OK; }
esp_err_t esp_wifi_stop() {
    ++stops;
    if (stop_error == ESP_OK || stop_error == ESP_ERR_WIFI_NOT_STARTED) running = false;
    return stop_error;
}
esp_err_t esp_wifi_set_channel(uint8_t channel, int secondary) {
    assert(running && channel == 6 && secondary == WIFI_SECOND_CHAN_NONE); return channel_error;
}
int main() {
    assert(agent_wifi_local_end() == ESP_OK && stops == 0);
    assert(agent_wifi_local_begin(6) == ESP_FAIL && inits == 1 && starts == 0);
    init_error = ESP_OK;
    assert(agent_wifi_local_begin(6) == ESP_OK && inits == 2 && starts == 1);
    assert(agent_wifi_local_begin(6) == ESP_ERR_INVALID_STATE && starts == 1);
    stop_error = ESP_FAIL;
    assert(agent_wifi_local_end() == ESP_FAIL && running);
    assert(agent_wifi_local_begin(6) == ESP_ERR_INVALID_STATE);
    stop_error = ESP_ERR_WIFI_NOT_STARTED;
    assert(agent_wifi_local_end() == ESP_OK && !running);
    stop_error = ESP_OK;
    assert(agent_wifi_local_end() == ESP_OK && stops == 2);
    channel_error = ESP_FAIL;
    assert(agent_wifi_local_begin(6) == ESP_FAIL && !running && stops == 3);
    channel_error = ESP_OK;
    assert(agent_wifi_local_begin(6) == ESP_OK && inits == 2 && starts == 3);
    assert(netifs == 2 && loops == 2);
    assert(agent_wifi_local_end() == ESP_OK && !running && stops == 4);
    puts("OJBadge local radio: no credential loading, retry, channel cleanup, exclusive start and repeated exit/re-entry passed.");
}
