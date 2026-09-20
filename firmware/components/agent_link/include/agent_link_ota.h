// agent_link — OTA firmware upgrade (App → device, over the existing link)
// Every board gets OTA for free: the SDK owns the whole flow (command handling,
// flash writes, SHA-256 verification, boot-partition switch, reboot). A board
// does not have to call anything. It may optionally register a progress callback
// to show "upgrading…" on its screen and to quiesce heavy peripherals.
//
// Wire protocol (BLE)
// the official App upgrades agent_link devices with the code path it
// already ships, and an existing product can be flashed over to agent_link:
//   App → device : 0x37 StartOtaUpgrade / 0x56 AbortOtaUpgrade  (GATT 0xFFB1, also
//                  accepted on 0xFFC1)
//   App → device : firmware bytes over L2CAP CoC PSM 0x0081
//   device → App : 0x38 OtaUpgradeStatus progress events (Notify 0xFFC4)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief OTA session state. */
typedef enum {
    AGENT_OTA_IDLE      = 0,  ///< No upgrade in progress
    AGENT_OTA_RECEIVING = 1,  ///< Receiving firmware bytes and writing flash
    AGENT_OTA_VERIFYING = 2,  ///< All bytes in; checking SHA-256 + image header
    AGENT_OTA_SUCCESS   = 3,  ///< Verified and armed; the device reboots in ~2s
    AGENT_OTA_FAILED    = 4,  ///< Aborted / verification failed (see error_code)
} agent_ota_state_t;

/** @brief Snapshot of the current OTA session (all zero when idle). */
typedef struct {
    agent_ota_state_t state;
    uint32_t          received_bytes;  ///< Bytes accepted from the App so far
    uint32_t          total_bytes;     ///< Firmware size announced by 0x37
    uint16_t          error_code;      ///< Meaningful in AGENT_OTA_FAILED
} agent_ota_status_t;

/**
 * @brief Progress callback (optional).
 * @param st  Current status snapshot.
 * @param ctx User context from agent_link_ota_set_callback().
 * @note Called from the OTA worker task on every state change and ~every 500ms
 *       while receiving. Must return quickly and must not touch flash
 *       (SPI cache is disabled around the writes this task performs).
 */
typedef void (*agent_ota_progress_cb_t)(const agent_ota_status_t* st, void* ctx);

/**
 * @brief Register a progress callback (e.g. to draw an upgrade screen).
 * @param cb  Callback, or NULL to unregister.
 * @param ctx Opaque pointer passed back to @p cb.
 * @note Optional. Boards that ignore OTA need no code at all.
 */
void agent_link_ota_set_callback(agent_ota_progress_cb_t cb, void* ctx);

/** @brief Current OTA status snapshot. */
void agent_link_ota_get_status(agent_ota_status_t* out);

/** @brief true while an upgrade is receiving or verifying (board may pause heavy work). */
bool agent_link_ota_is_busy(void);

/**
 * @brief Abort an upgrade in progress (same effect as the App's 0x56).
 * @note Cooperative: the worker notices within ~500ms, rolls the partition back and
 *       reports 0x38 phase=Failed error=2035. No-op when idle.
 */
void agent_link_ota_abort(void);

/**
 * @brief Confirm that the firmware running right now is healthy.
 *
 * Only meaningful when the app partition is still pending verification
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE); otherwise a no-op returning ESP_OK.
 * The SDK calls it automatically once the link has been up and usable for a
 * short while (the link is the only upgrade path, so "BLE works" is the
 * self-test that matters). Call it yourself if a board wants a stricter test.
 *
 * @return ESP_OK when the image is (or already was) marked valid.
 */
esp_err_t agent_link_ota_mark_valid(void);

/** @brief Firmware version string of the running image (app descriptor / PROJECT_VER). */
const char* agent_link_ota_running_version(void);

#ifdef __cplusplus
}
#endif
