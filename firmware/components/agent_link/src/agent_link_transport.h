/**
 * @file agent_link_transport.h
 * @brief Transport layer abstraction for agent_link.
 * @details Defines the interface for transport backends (BLE, WiFi) and the
 *          stream types used for data-plane communication.
 *          Control-plane frames (commands/responses/events) are sent via
 *          send_ctrl(). Data-plane streams of every kind (see agent_stream_t) go through
 *          stream_start/send_stream/stream_end.
 */


#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// agent_stream_t and the stream vocabulary are part of the public API, not of this backend
// interface: a backend implements the kinds the API defines, never the other way round.
#include "agent_link.h"        // agent_link_status_t / agent_platform_t / agent_wifi_config_t
#include "agent_link_caps.h"   // agent_state_t
#include "agent_link_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Transport backend operation table
 * @note All functions must be implemented by the backend
 *       Control plane is reliable but low‑bandwidth (GATT, WS, MQTT)
 *       Data plane is high‑throughput (L2CAP, WebRTC)
 */
typedef struct agent_transport_s {
    /**
     * @brief Start the transport (advertise/connect)
     * @param impl Backend private context
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t (*start)(void* impl);

    /**
     * @brief Stop the transport (disconnect/release)
     * @param impl Backend private context
     */
    void      (*stop)(void* impl);

    /**
     * @brief Send a control frame (command/response/event)
     * @param impl  Backend private context
     * @param frame Frame buffer (header + payload)
     * @param len   Frame length
     * @return ESP_OK on success, error code otherwise
     * @note The control channel is reliable and ordered
     */
    esp_err_t (*send_ctrl)(void* impl, const uint8_t* frame, size_t len);

    /**
     * @brief Start a data‑plane stream session
     * @param impl     Backend private context
     * @param type     Stream kind (see agent_stream_t)
     * @param meta     Optional metadata (e.g., codec, sample rate)
     * @param meta_len Length of meta
     * @return ESP_OK on success, error code otherwise
     * @note The backend allocates a session ID and resets sequence numbers
     */
    esp_err_t (*stream_start)(void* impl, agent_stream_t type, const uint8_t* meta, size_t meta_len);

    /**
     * @brief Send a chunk of data over an active stream
     * @param impl Backend private context
     * @param type Stream type
     * @param data Data buffer
     * @param len  Data length
     * @return ESP_OK on success, error code otherwise
     * @note BLE carries VOICE on GATT Notify 0xFFA1 (event 0x40 VoiceChunk) and the bulk kinds
     *       on L2CAP CoC; VIDEO needs a WiFi transport
     */
    esp_err_t (*send_stream)(void* impl, agent_stream_t type, const uint8_t* data, size_t len);

    /**
     * @brief End a data‑plane stream session
     * @param impl     Backend private context
     * @param type     Stream type
     * @param complete true if the stream ended normally, false if aborted
     * @param meta     Optional trailing metadata (60-byte final_header for the 0x53 event); NULL if none
     * @param meta_len Length of meta
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t (*stream_end)(void* impl, agent_stream_t type, bool complete, const uint8_t* meta, size_t meta_len);

    /**
     * @brief Check if the transport is ready (connected and encrypted)
     * @param impl Backend private context
     * @return true if ready, false otherwise
     */
    bool      (*is_ready)(void* impl);

    void*     impl;  // Backend private context
} agent_transport_t;

/**
 * @brief Get the BLE transport instance
 * @return Pointer to the BLE transport operation table
 * @note Control plane is fully implemented.
 *       Data plane: VOICE via GATT Notify, AUDIO and IMAGE via L2CAP CoC; FILE and VIDEO
 *       are declared but not yet carried by this backend
 */
agent_transport_t* agent_transport_ble(void);

/**
 * @brief Set BLE advertising name
 * @param name Device name (used in advertising and GAP)
 * @note Called by agent_link_init() with the device name from config
 */
void agent_transport_ble_set_name(const char* name);

/**
 * @brief Register callback for incoming control frames
 * @param cb Function called when a control frame is received (0xFFC1 writes)
 * @note Called by agent_link_init() to wire the core's OnCtrlFrame
 */
void agent_transport_ble_set_recv(void (*cb)(const uint8_t* data, size_t len));

/**
 * @brief Register callback for connection state changes
 * @param cb Function called when BLE connects or disconnects
 * @note Called by agent_link_init() to wire the core's OnConn
 */
void agent_transport_ble_set_conn(void (*cb)(bool connected));

/**
 * @brief Register callback for incoming data frames
 * @param cb Function called when a data frame is received
 * @note Called by agent_link_init() to wire the core's OnStreamData
 */
void agent_transport_ble_set_stream_recv(void (*cb)(agent_stream_t type, const uint8_t* data, size_t len));

/**
 * @brief Register callback for transport readiness
 * @param cb Function called when the transport becomes ready
 * @note Called by agent_link_init() to wire the core's OnLinkReady
 */
void agent_transport_ble_set_ready(void (*cb)(void));

/**
 * @brief Get the current ATT MTU.
 * @return MTU size in bytes, or 0 if not connected.
 * @note Used by the core to adapt manifest fragment size.
 */
uint16_t agent_transport_ble_att_mtu(void);

/**
 * @brief Set device information for the standard Device Information Service (0x180A).
 * @param manufacturer Manufacturer name (0x2A29), NULL/empty keeps default.
 * @param model        Model number (0x2A24), NULL/empty keeps default.
 * @param firmware_rev Firmware revision (0x2A26), NULL/empty keeps default.
 * @note Called once during init.
 */
void agent_transport_ble_set_device_info(const char* manufacturer, const char* model, const char* firmware_rev);

/**
 * @brief Is the App's L2CAP data channel (PSM 0x0081) open?
 * @return true when BLE is connected and the peer has opened the channel.
 * @note The core requires it before accepting an OTA — the firmware bytes arrive there.
 */
bool agent_transport_ble_l2cap_ready(void);

/**
 * @brief Copy the device's own BLE address, MSB first.
 * @param out 6-byte buffer, filled in the order the App sees in the advertisement.
 * @return false if the stack has no address yet (called before sync).
 */
bool agent_transport_ble_get_mac(uint8_t out[6]);

/**
 * @brief Update the battery level in the standard Battery Service (0x180F, 0x2A19).
 * @param percent Battery percentage (0–100).
 * @note Stores the value for read and notifies if connected.
 *       Called by the core when battery changes.
 */
void agent_transport_ble_update_battery(uint8_t percent);

/**
 * @brief Erase all stored bonds; the App has to pair again.
 * @note The BLE side of agent_link_forget(). A peer connected right now stays connected.
 */
esp_err_t agent_transport_ble_forget(void);

/**
 * @brief Get the WiFi transport instance.
 * @return Pointer to the WiFi transport operation table.
 */
agent_transport_t* agent_transport_wifi(void);

/**
 * @brief Preset station credentials.
 * @param cfg Usually NULL: the backend then uses the credentials saved by its captive portal.
 * @note Called by agent_link_init().
 */
void agent_transport_wifi_set_config(const agent_wifi_config_t* cfg);

/**
 * @brief Set the device name used as the SoftAP SSID prefix for WiFi provisioning.
 * @param name Device name; the captive-portal SoftAP is advertised as "<name>-XXXX".
 * @note Called by agent_link_init() with the device name from config.
 */
void agent_transport_wifi_set_name(const char* name);

/**
 * @brief Register callback for incoming control frames.
 * @param cb Function called when a control message is received (WS/DataChannel).
 * @note Called by agent_link_init() to wire the core's OnCtrlFrame.
 */
void agent_transport_wifi_set_recv(void (*cb)(const uint8_t* data, size_t len));

/**
 * @brief Register callback for connection state changes.
 * @param cb Function called when WiFi connects or disconnects.
 * @note Not used by the core, which registers agent_transport_wifi_set_state() instead.
 */
void agent_transport_wifi_set_conn(void (*cb)(bool connected));

/**
 * @brief Register callback for incoming data-plane data.
 * @param cb Function called when a data chunk arrives (e.g., WebRTC audio/video).
 * @note Called by agent_link_init() to wire the core's OnStreamData.
 */
void agent_transport_wifi_set_stream_recv(void (*cb)(agent_stream_t type, const uint8_t* data, size_t len));

/**
 * @brief Register the link-state callback.
 *
 * Replaces set_conn on this backend, because WiFi has two "up" states: CONNECTED (authenticated,
 * heartbeat running) and READY (data plane up as well). Only the backend knows which applies.
 *
 * @note Called by agent_link_init().
 */
void agent_transport_wifi_set_state(void (*cb)(agent_state_t state));

/**
 * @brief Platform identity, needed to claim and authenticate the device.
 * @note Called by agent_link_init(). With NULL, only the station link is brought up.
 */
void agent_transport_wifi_set_platform(const agent_platform_t* platform);

/**
 * @brief Register the link status callback.
 *
 * The backend translates its cloud session state (cloud/cloud_status.h) into agent_link_status_t;
 * the core passes it on to agent_link_config_t::on_status unchanged.
 *
 * @note Called by agent_link_init().
 */
void agent_transport_wifi_set_status(void (*cb)(const agent_link_status_t* st));

/**
 * @brief Erase the platform credential, so the device asks for a new activation code.
 * @note The WiFi side of agent_link_forget().
 */
esp_err_t agent_transport_wifi_forget(void);

#ifdef __cplusplus
}
#endif
