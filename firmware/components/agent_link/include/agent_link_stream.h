#pragma once
/**
 * @file agent_link_stream.h
 * @brief Device → Agent data-plane streams: one API for every kind of bulk payload.
 *
 * @details The control plane (commands, events, small readings) moves a few hundred bytes at a
 *          time and is covered by agent_link.h / agent_link_io.h. Anything bulk — speech, an
 *          audio recording, a snapshot, video, a file — goes through this one interface instead:
 *
 *              open(kind, opts) -> write(...) x N -> close(complete)
 *
 *          The @ref agent_stream_t kind is the only thing that varies. The transport decides how
 *          that kind travels (BLE uses GATT notifications for one kind and separate L2CAP
 *          channels for the others; a WiFi backend would use different tracks), and it owns
 *          session ids, framing, slicing and backpressure. Callers just hand over bytes.
 *
 *          The SDK does not interpret those bytes. It will not encode, transcode or compress —
 *          whatever a caller writes is what the App receives, so the payload format is an
 *          agreement between the board and the App, not something this layer constrains.
 *
 * @note Streams of different kinds are independent and may run concurrently. Only one stream of
 *       any given kind may be open at a time; opening a second returns the same handle.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a stream carries.
 *
 * @note The distinction between VOICE and AUDIO is where the audio is *going*, not what it sounds
 *       like or what anyone does with it downstream. Both are plain PCM as far as this layer is
 *       concerned.
 */
typedef enum {
    /**
     * Speech meant for the Agent. The App relays it on as if the user had spoken, so this is the
     * channel behind "talk to the agent". Sized for utterances: it rides the low-latency control
     * path (BLE: GATT notifications), which needs no side channel to be open but is not built for
     * sustained throughput.
     */
    AGENT_STREAM_VOICE = 0,

    /**
     * Audio for the App itself — of any length, and whatever the App wants to do with it is the
     * App's business. Rides the bulk data path (BLE: an L2CAP channel the App must have opened),
     * so it sustains real-time capture where VOICE would not.
     */
    AGENT_STREAM_AUDIO,

    /** A still image. */
    AGENT_STREAM_IMAGE,

    /** Encoded video frames. Needs the bandwidth of a WiFi transport. */
    AGENT_STREAM_VIDEO,

    /** An arbitrary file from the device's storage. */
    AGENT_STREAM_FILE,

    AGENT_STREAM_KIND_COUNT,
} agent_stream_t;

/** @brief How the bytes in a stream are encoded, when the kind allows a choice. */
typedef enum {
    AGENT_ENC_RAW  = 0,  ///< Uncompressed: PCM16 for audio, big-endian RGB565 for images
    AGENT_ENC_JPEG = 1,  ///< JPEG (images)
} agent_encoding_t;

/**
 * @brief Optional description of a stream, passed at open time.
 *
 * @note Zero-initialize and set only what the kind needs; everything here is optional and the
 *       fields a kind does not use are ignored. A caller that knows @ref total_bytes up front
 *       should say so — it is the only way the App can show progress.
 */
typedef struct {
    const char*      name;         ///< Short label carried to the App (may be NULL)
    agent_encoding_t encoding;     ///< Payload encoding (default AGENT_ENC_RAW)
    uint16_t         width;        ///< Pixel width, for IMAGE / VIDEO (0 if not applicable)
    uint16_t         height;       ///< Pixel height, for IMAGE / VIDEO
    uint32_t         total_bytes;  ///< Total payload size when known in advance; 0 = unknown
} agent_stream_opts_t;

/** @brief Handle to an open stream. Opaque; obtained from agent_link_stream_open(). */
typedef struct agent_stream_session* agent_stream_handle_t;

/**
 * @brief Open a stream.
 *
 * @param kind What the stream will carry.
 * @param opts Optional description (may be NULL for kinds that need none, such as VOICE).
 * @param out  Receives the handle. Required.
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_ARG if @p out is NULL or @p kind is out of range;
 *         ESP_ERR_INVALID_STATE if the link is not ready;
 *         ESP_ERR_NOT_SUPPORTED if the active transport cannot carry this kind.
 * @note Opening a kind that is already open succeeds and returns the existing handle, so a caller
 *       that cannot easily track state may simply open before every write.
 */
esp_err_t agent_link_stream_open(agent_stream_t kind, const agent_stream_opts_t* opts,
                                 agent_stream_handle_t* out);

/**
 * @brief Append bytes to an open stream.
 *
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_STATE if the stream is not open;
 *         ESP_ERR_NO_MEM if the transport's queue is full.
 * @note ESP_ERR_NO_MEM means this chunk was dropped, not that the stream died. Whether to keep
 *       going or close is the caller's call and depends on the payload: a gap in live audio is
 *       survivable, a gap in a file is not.
 */
esp_err_t agent_link_stream_write(agent_stream_handle_t handle, const void* data, size_t bytes);

/**
 * @brief Close a stream.
 *
 * @param complete true if everything intended was written; false if the stream was cut short.
 *                 The App is told which, so a truncated payload is not mistaken for a whole one.
 * @note Closing an already-closed stream is a no-op returning ESP_OK.
 */
esp_err_t agent_link_stream_close(agent_stream_handle_t handle, bool complete);

/**
 * @brief Open, write and close in one call — for a payload already complete in memory.
 *
 * @note Returns once the payload is queued, not once it has reached the App; the transport
 *       drains it in the background.
 */
esp_err_t agent_link_stream_send(agent_stream_t kind, const agent_stream_opts_t* opts,
                                 const void* data, size_t bytes);

/** @brief Whether a stream of this kind is currently open. */
bool agent_link_stream_is_open(agent_stream_t kind);

#ifdef __cplusplus
}
#endif
