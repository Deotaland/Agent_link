// muse_gadget.h: Home Link (link/app.c) run on a task of its own, for agent_link's Muse transport.
//
// Upstream, app_run() is the firmware: app_main() calls it and it never returns. Here it is one
// task among the board's, started by the transport. It brings up NVS, Wi-Fi and BLE itself, which
// is why the agent_link core never starts its own radios on this transport.
#include "muse_gadget_priv.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app.h"
#include "identity.h"
#include "noise_control.h"

static const char* TAG = "muse_gadget";

// app_run() does TLS handshakes on this task (the device-token and VM lookups). Upstream gives it
// 8 KB as its main task, but here the first lookup left only 1.5 KB of that unused (high water
// mark on hardware, 2026-10-04), too close for a different certificate chain or the token
// refresh path. Internal RAM: NVS writes happen here with the cache disabled.
#define MUSE_LINK_STACK 12288
#define MUSE_LINK_PRIO  5

static muse_gadget_config_t s_cfg;
static bool s_started;

static void link_task(void* arg)
{
    (void)arg;
    app_run();   // never returns
    vTaskDelete(NULL);
}

esp_err_t muse_gadget_start(const muse_gadget_config_t* cfg)
{
    if (s_started) return ESP_ERR_INVALID_STATE;
    if (cfg) s_cfg = *cfg;

    // app_run() opens NVS on its own task. agent_link reads its device identity from NVS right
    // after this returns, so have the partition up before that, not racing it.
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing (%s)", esp_err_to_name(r));
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    if (r != ESP_OK) return r;

    // Only formats names from the MAC; app_run() repeats it. Done here so the BLE name is known
    // to the first status report, which can arrive before app_run() gets that far.
    identity_init();

    if (xTaskCreate(link_task, "muse_link", MUSE_LINK_STACK, NULL, MUSE_LINK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the Home Link task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

const char* muse_gadget_ble_name(void)
{
    return s_started ? identity_ble_name() : "";
}

bool muse_gadget_online(void)
{
    return s_started && noise_ctrl_is_connected();
}

bool muse_gadget_confirm_press(void)
{
    return s_started && app_confirm_pairing_press();
}

void muse_gadget_forget(void)
{
    if (!s_started) return;
    ESP_LOGW(TAG, "forgetting the Muse pairing and Wi-Fi; restarting into setup");
    app_reset_setup_async();
}

void muse_gadget_emit_state(muse_gadget_state_t state)
{
    if (s_cfg.on_state) s_cfg.on_state(state, s_cfg.ctx);
}

void muse_gadget_emit_title(const char* name)
{
    if (s_cfg.on_title) s_cfg.on_title(name ? name : "", s_cfg.ctx);
}
