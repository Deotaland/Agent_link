#include "agent_link_network.h"
#include <atomic>
#include <cstring>
#if AGENT_LINK_DESKTOP_BLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
namespace {
std::atomic<bool> enabled{false};
bool initialized = false;
uint8_t address_type;
std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
void Advertise();
int Gap(ble_gap_event* event, void*) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            connection = event->connect.conn_handle;
            if (!enabled.load()) (void)ble_gap_terminate(connection.load(), BLE_ERR_REM_USER_CONN_TERM);
        }
        else Advertise();
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        connection = BLE_HS_CONN_HANDLE_NONE; Advertise();
    } else if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) Advertise();
    return 0;
}
void Advertise() {
    if (!enabled.load() || !ble_hs_synced() || connection != BLE_HS_CONN_HANDLE_NONE) return;
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char* name = ble_svc_gap_device_name();
    fields.name = reinterpret_cast<const uint8_t*>(name);
    fields.name_len = std::strlen(name); fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) return;
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    (void)ble_gap_adv_start(address_type, nullptr, BLE_HS_FOREVER, &params, Gap, nullptr);
}
void Sync() {
    if (ble_hs_id_infer_auto(0, &address_type) == 0) Advertise();
}
void Host(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }
}
extern "C" esp_err_t agent_link_ble_enable(bool value) {
    if (value && !initialized) {
        const auto result = nimble_port_init();
        if (result != ESP_OK) return result;
        ble_svc_gap_init(); ble_svc_gatt_init();
        ble_hs_cfg.sync_cb = Sync;
        ble_svc_gap_device_name_set("PaperS31");
        initialized = true; enabled.store(true);
        nimble_port_freertos_init(Host);
        return ESP_OK;
    }
    enabled.store(value);
    if (!initialized) return ESP_OK;
    if (value) Advertise();
    else {
        (void)ble_gap_adv_stop();
        if (connection != BLE_HS_CONN_HANDLE_NONE)
            (void)ble_gap_terminate(connection.load(), BLE_ERR_REM_USER_CONN_TERM);
    }
    return ESP_OK;
}
extern "C" bool agent_link_ble_enabled(void) { return enabled.load(); }
#else
extern "C" esp_err_t agent_link_ble_enable(bool) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool agent_link_ble_enabled(void) { return false; }
#endif
