// agent_link OTA engine - receive a firmware image over the App link, verify it, boot it.
//
// Data flow:
//   transport RX task  -> FeedData()          writes a ring buffer, never blocks
//        ^ RX gate: WantsMore() goes false while the buffer is nearly full, the transport
//          then stops replenishing peer credit and the App pauses (no bytes are dropped);
//          the worker lifts it again through the resume hook after each chunk it consumes.
//   OTA worker task    -> esp_ota_begin (full erase of the target partition, 2-4s)
//                      -> mbedtls_sha256_update + esp_ota_write (4KB chunks)
//                      -> SHA-256 compare -> esp_ota_end (image self-check)
//                      -> esp_ota_set_boot_partition -> esp_restart after 2s
//
// State machine:
//   IDLE -> Start() -> RECEIVING -> (all bytes in) -> VERIFYING -> SUCCESS (reboot)
//                                        |
//                                        +-> FAILED (0x38 with the error code) -> IDLE
#include "ota_service.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace {
constexpr const char* TAG = "agent_link.ota";
}

#if !defined(CONFIG_AGENT_LINK_OTA_ENABLE)

// OTA compiled out (menuconfig -> agent_link SDK). Start() then always refuses, so the App
// gets 0x37 -> 1003 BusinessFailed; everything else in the SDK is unaffected.
namespace agentlink {
namespace ota {
namespace { char s_model[40] = ""; }
void        SetEventSink(EventSink) {}
void        SetResumeRxHook(ResumeRxHook) {}
void        SetDeviceModel(const char* m) {
    if (!m || !*m) return;
    strncpy(s_model, m, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
}
const char* DeviceModel() { return s_model; }
bool        Start(uint32_t, const uint8_t*, const char*, const char*, uint16_t* e) {
    if (e) *e = kErrBusinessFailed;
    return false;
}
size_t      FeedData(const uint8_t*, size_t) { return 0; }
bool        WantsMore() { return true; }
void        Abort(const char*) {}
bool        IsBusy() { return false; }
void        GetStatus(agent_ota_status_t* out) { if (out) *out = agent_ota_status_t{}; }
void        OnLinkState(bool connected) { if (connected) (void)agent_link_ota_mark_valid(); }
}  // namespace ota
}  // namespace agentlink

extern "C" void agent_link_ota_set_callback(agent_ota_progress_cb_t, void*) {}
extern "C" void agent_link_ota_get_status(agent_ota_status_t* out) { if (out) *out = agent_ota_status_t{}; }
extern "C" bool agent_link_ota_is_busy(void) { return false; }
extern "C" void agent_link_ota_abort(void) {}

#else  // CONFIG_AGENT_LINK_OTA_ENABLE

#include <atomic>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"   // xStreamBufferCreateWithCaps
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

namespace agentlink {
namespace ota {

// Board progress callback, at namespace scope so the extern "C" setter can reach it.
agent_ota_progress_cb_t s_cb     = nullptr;
void*                   s_cb_ctx = nullptr;

namespace {

constexpr size_t   kBufBytes      = CONFIG_AGENT_LINK_OTA_RX_BUFFER_KB * 1024u;
constexpr size_t   kFlashChunk    = 4096;        // one esp_ota_write
constexpr uint32_t kProgressMs    = 500;         // 0x38 throttle
constexpr uint32_t kPollMs        = 500;         // worker read timeout = abort latency
constexpr uint32_t kIdleTimeoutMs = 30 * 1000;   // no new bytes this long -> App is gone
constexpr uint32_t kRebootDelayMs = 2000;        // let the Success event get out first

// RX high-water mark: hold off credit while free space is below this. It must cover the SDUs
// already in flight when we stop granting credit (our_coc_mtu is 4096, so two of them), and
// must stay under half the buffer or the gate could never reopen.
constexpr size_t kGateFloor  = 8 * 1024;
constexpr size_t kGateWanted = (kBufBytes / 4 > kGateFloor) ? (kBufBytes / 4) : kGateFloor;
constexpr size_t kRxGateMinFree = (kGateWanted > kBufBytes / 2) ? (kBufBytes / 2) : kGateWanted;

std::atomic<agent_ota_state_t> s_state{AGENT_OTA_IDLE};
std::atomic<bool>              s_abort{false};
std::atomic<uint32_t>          s_received{0};    // bytes accepted into the ring buffer
std::atomic<uint16_t>          s_error{0};

// Session metadata: written by Start() before the worker exists, then owned by the worker.
uint32_t               s_total = 0;
uint8_t                s_sha_expect[32] = {};
char                   s_version[32] = "";
const esp_partition_t* s_part = nullptr;
esp_ota_handle_t       s_handle = 0;
bool                   s_handle_open = false;
mbedtls_sha256_context s_sha_ctx;
bool                   s_sha_open = false;
uint32_t               s_last_progress_ms = 0;

// The ring buffer is created once and kept forever: FeedData() (transport task) and the
// failure-path cleanup (worker task) can run on two cores at the same time, so deleting the
// handle would be a use-after-free. Start() only resets it, which also drops any tail left
// over from a failed session. Idle cost is CONFIG_AGENT_LINK_OTA_RX_BUFFER_KB of PSRAM.
StreamBufferHandle_t s_buf = nullptr;
TaskHandle_t         s_worker = nullptr;

EventSink    s_sink   = nullptr;
ResumeRxHook s_resume = nullptr;
char         s_model[40] = "";

uint32_t NowMs() { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

void Snapshot(agent_ota_status_t* out) {
    out->state          = s_state.load(std::memory_order_acquire);
    out->received_bytes = s_received.load(std::memory_order_acquire);
    out->total_bytes    = s_total;
    out->error_code     = s_error.load(std::memory_order_acquire);
}

// Wire phase for event 0x38 (App-facing numbering, distinct from agent_ota_state_t).
uint8_t WirePhase(agent_ota_state_t st) {
    switch (st) {
    case AGENT_OTA_VERIFYING: return 1;
    case AGENT_OTA_SUCCESS:   return 2;
    case AGENT_OTA_FAILED:    return 3;
    default:                  return 0;  // Receiving
    }
}

// 0x38 OtaUpgradeStatus: phase(1) + received(4 LE) + total(4 LE) + error(2 LE) = 11 bytes,
// plus the board callback so a screen can follow along.
void PushProgress() {
    agent_ota_status_t st;
    Snapshot(&st);

    if (s_sink) {
        uint8_t p[11];
        p[0]  = WirePhase(st.state);
        p[1]  = st.received_bytes & 0xFF;
        p[2]  = (st.received_bytes >> 8) & 0xFF;
        p[3]  = (st.received_bytes >> 16) & 0xFF;
        p[4]  = (st.received_bytes >> 24) & 0xFF;
        p[5]  = st.total_bytes & 0xFF;
        p[6]  = (st.total_bytes >> 8) & 0xFF;
        p[7]  = (st.total_bytes >> 16) & 0xFF;
        p[8]  = (st.total_bytes >> 24) & 0xFF;
        p[9]  = st.error_code & 0xFF;
        p[10] = (st.error_code >> 8) & 0xFF;
        s_sink(kEvtOtaProgress, p, sizeof(p));
    }
    if (s_cb) s_cb(&st, s_cb_ctx);
}

// Tell the board only - no 0x38 on the wire. Used once the session is over and the state is
// back to IDLE: the App already has its terminal event, but a board that put an upgrade screen
// up needs to hear that it can take it down again.
void NotifyBoard() {
    if (!s_cb) return;
    agent_ota_status_t st;
    Snapshot(&st);
    s_cb(&st, s_cb_ctx);
}

void FailWith(uint16_t err, const char* reason) {
    ESP_LOGE(TAG, "OTA failed: %s (err=%u, %u/%u bytes)", reason ? reason : "?", err,
             static_cast<unsigned>(s_received.load(std::memory_order_acquire)),
             static_cast<unsigned>(s_total));
    s_error.store(err, std::memory_order_release);
    s_state.store(AGENT_OTA_FAILED, std::memory_order_release);
    PushProgress();
}

// Release the session's flash resources. The caller must have moved the state out of
// RECEIVING first, so FeedData() is already refusing bytes.
void Cleanup() {
    if (s_sha_open) { mbedtls_sha256_free(&s_sha_ctx); s_sha_open = false; }
    if (s_handle_open) { esp_ota_abort(s_handle); s_handle_open = false; s_handle = 0; }
    s_part = nullptr;
    if (s_buf) xStreamBufferReset(s_buf);
    s_total = 0;
    s_received.store(0, std::memory_order_release);
    s_abort.store(false, std::memory_order_release);
    s_version[0] = '\0';
    s_last_progress_ms = 0;
}

// Lenient version compare: accept 1-3 numeric fields ("1.5" -> 1.5.0, "2.0-rc1" -> 2.0.0),
// missing fields count as 0, trailing text ignored. An unparsable *running* version lets the
// upgrade through - a device that can always be reflashed beats one locked out by a typo.
bool IsNewer(const char* candidate, const char* current) {
    auto parse = [](const char* s, int out[3]) -> bool {
        out[0] = out[1] = out[2] = 0;
        return s && std::sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]) >= 1;
    };
    int n[3], c[3];
    if (!parse(candidate, n)) return false;
    if (!parse(current, c)) {
        ESP_LOGW(TAG, "running version '%s' unparsable - allowing upgrade", current ? current : "");
        return true;
    }
    for (int i = 0; i < 3; ++i)
        if (n[i] != c[i]) return n[i] > c[i];
    return false;  // identical
}

const char* RunningVersion() { return agent_link_ota_running_version(); }

// Version gate. The comparison always runs and always says what it decided - "why did this
// device accept a downgrade" is exactly the question a field log has to answer - but it only
// refuses when the strict rule is configured on.
bool VersionAllowed(const char* version) {
    if (IsNewer(version, RunningVersion())) return true;
#if CONFIG_AGENT_LINK_OTA_REQUIRE_NEWER_VERSION
    ESP_LOGW(TAG, "version '%s' is not newer than '%s' - refused", version ? version : "?",
             RunningVersion());
    return false;
#else
    ESP_LOGW(TAG, "version '%s' is not newer than '%s' - allowed anyway "
                  "(AGENT_LINK_OTA_REQUIRE_NEWER_VERSION is off)",
             version ? version : "?", RunningVersion());
    return true;
#endif
}

void WorkerTask(void*) {
    ESP_LOGI(TAG, "worker started: %u bytes -> %s @ 0x%lx", static_cast<unsigned>(s_total),
             s_part->label, static_cast<unsigned long>(s_part->address));

    // Failure exit: roll the partition back, return to idle so the App can retry, self-delete.
    // The success path reboots instead and never comes here.
    auto exit_worker = [] {
        Cleanup();
        s_state.store(AGENT_OTA_IDLE, std::memory_order_release);
        // Lift the RX gate before leaving. An abort can land while it is shut, and with the
        // worker gone nobody else would ever reopen it - the data channel would then be dead
        // for everything else too (TTS included) until the link is rebuilt.
        if (s_resume) s_resume();
        NotifyBoard();   // "session over" - the board can drop its upgrade screen
        s_worker = nullptr;
        vTaskDelete(nullptr);
    };

    // Erasing the target partition takes 2-4s. It runs here rather than in Start() so the
    // 0x37 ACK is immediate; bytes the App sends meanwhile pile up in the ring buffer and the
    // RX gate back-pressures it if that fills, so nothing is lost.
    esp_err_t err = esp_ota_begin(s_part, s_total, &s_handle);
    if (err != ESP_OK) {
        FailWith(kErrPartitionFail, esp_err_to_name(err));
        exit_worker();
        return;
    }
    s_handle_open = true;

    mbedtls_sha256_init(&s_sha_ctx);
    if (mbedtls_sha256_starts(&s_sha_ctx, 0) != 0) {
        FailWith(kErrConfigFailed, "sha256 init");
        exit_worker();
        return;
    }
    s_sha_open = true;

    // Staging buffer for one flash write. PSRAM keeps internal RAM free; the flash driver
    // bounces external-RAM sources through its own internal buffer, so the source is safe.
    // The *stack* is what must be internal - hence xTaskCreate, never a PSRAM-stack task.
    uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(kFlashChunk, MALLOC_CAP_SPIRAM));
    if (!chunk) chunk = static_cast<uint8_t*>(heap_caps_malloc(kFlashChunk, MALLOC_CAP_INTERNAL));
    if (!chunk) {
        FailWith(kErrConfigFailed, "chunk alloc");
        exit_worker();
        return;
    }

    uint32_t written = 0;
    uint32_t idle_ms = 0;
    bool ok = true;

    while (written < s_total) {
        if (s_abort.load(std::memory_order_acquire)) {
            FailWith(kErrAborted, "aborted");
            ok = false;
            break;
        }
        const size_t want = (s_total - written < kFlashChunk) ? (s_total - written) : kFlashChunk;
        const size_t got = xStreamBufferReceive(s_buf, chunk, want, pdMS_TO_TICKS(kPollMs));
        if (got == 0) {
            idle_ms += kPollMs;
            // Retry lifting the gate: the transport may have failed to replenish credit
            // earlier (mbuf pool momentarily empty), and nobody else would try again.
            if (s_resume && WantsMore()) s_resume();
            if (idle_ms >= kIdleTimeoutMs) {
                FailWith(kErrWriteFail, "receive timeout");
                ok = false;
                break;
            }
            continue;
        }
        idle_ms = 0;

        if (mbedtls_sha256_update(&s_sha_ctx, chunk, got) != 0) {
            FailWith(kErrConfigFailed, "sha256 update");
            ok = false;
            break;
        }
        err = esp_ota_write(s_handle, chunk, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write at %u: %s", static_cast<unsigned>(written),
                     esp_err_to_name(err));
            FailWith(kErrWriteFail, esp_err_to_name(err));
            ok = false;
            break;
        }
        written += got;

        if (s_resume && WantsMore()) s_resume();  // space freed -> let the App send again

        const uint32_t now = NowMs();
        if (now - s_last_progress_ms >= kProgressMs) {
            s_last_progress_ms = now;
            PushProgress();
        }
    }

    heap_caps_free(chunk);

    if (ok) {
        s_state.store(AGENT_OTA_VERIFYING, std::memory_order_release);
        PushProgress();

        uint8_t actual[32];
        if (mbedtls_sha256_finish(&s_sha_ctx, actual) != 0) {
            FailWith(kErrConfigFailed, "sha256 finish");
        } else if (std::memcmp(actual, s_sha_expect, sizeof(actual)) != 0) {
            char want_hex[2 * 32 + 1], got_hex[2 * 32 + 1];
            for (int i = 0; i < 32; ++i) {
                std::snprintf(want_hex + i * 2, sizeof(want_hex) - i * 2, "%02x", s_sha_expect[i]);
                std::snprintf(got_hex + i * 2, sizeof(got_hex) - i * 2, "%02x", actual[i]);
            }
            ESP_LOGE(TAG, "SHA-256 mismatch\n  expected %s\n  actual   %s", want_hex, got_hex);
            FailWith(kErrSha256Mismatch, "sha256 mismatch");
        } else {
            err = esp_ota_end(s_handle);   // ESP-IDF's own image header / signature check
            s_handle_open = false;         // the handle is spent either way
            if (err != ESP_OK) {
                FailWith(kErrImageInvalid, esp_err_to_name(err));
            } else if ((err = esp_ota_set_boot_partition(s_part)) != ESP_OK) {
                FailWith(kErrPartitionFail, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "OTA complete (%s -> %s), rebooting in %ums",
                         RunningVersion(), s_version, static_cast<unsigned>(kRebootDelayMs));
                s_state.store(AGENT_OTA_SUCCESS, std::memory_order_release);
                PushProgress();
                vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
                esp_restart();
            }
        }
    }

    exit_worker();
}

}  // namespace

void SetEventSink(EventSink sink)       { s_sink = sink; }
void SetResumeRxHook(ResumeRxHook hook) { s_resume = hook; }

void SetDeviceModel(const char* model) {
    if (!model || !*model) return;
    strncpy(s_model, model, sizeof(s_model) - 1);
    s_model[sizeof(s_model) - 1] = '\0';
}
const char* DeviceModel() { return s_model; }

bool Start(uint32_t fw_size, const uint8_t sha256[32], const char* version,
           const char* model, uint16_t* err_code) {
    auto fail = [err_code](uint16_t code, const char* msg) {
        ESP_LOGW(TAG, "0x37 rejected: %s (err=%u)", msg, code);
        if (err_code) *err_code = code;
        return false;
    };

    if (s_state.load(std::memory_order_acquire) != AGENT_OTA_IDLE)
        return fail(kErrBusinessFailed, "already upgrading");

#if CONFIG_AGENT_LINK_OTA_REQUIRE_MODEL_MATCH
    // The App omits the model on older releases; only a mismatch is refused, never absence.
    if (model && *model && s_model[0] && strcmp(model, s_model) != 0) {
        ESP_LOGW(TAG, "model mismatch: firmware '%s' != device '%s'", model, s_model);
        return fail(kErrModelMismatch, "model mismatch");
    }
#else
    (void)model;
#endif
    if (!VersionAllowed(version)) return fail(kErrVersionIncompat, "version not newer");

    s_part = esp_ota_get_next_update_partition(nullptr);
    if (!s_part) return fail(kErrPartitionFail, "no ota partition");
    if (fw_size == 0 || fw_size > s_part->size) {
        ESP_LOGW(TAG, "fw_size=%u, partition=%u", static_cast<unsigned>(fw_size),
                 static_cast<unsigned>(s_part->size));
        s_part = nullptr;
        return fail(kErrFirmwareTooLarge, "firmware too large");
    }

    if (!s_buf) {
        // PSRAM keeps internal RAM for the BLE stack; boards without it fall back to DRAM.
        s_buf = xStreamBufferCreateWithCaps(kBufBytes, 1, MALLOC_CAP_SPIRAM);
        if (!s_buf) s_buf = xStreamBufferCreate(kBufBytes, 1);
        if (!s_buf) { s_part = nullptr; return fail(kErrConfigFailed, "buffer alloc"); }
    } else {
        xStreamBufferReset(s_buf);
    }

    s_total = fw_size;
    s_received.store(0, std::memory_order_release);
    s_error.store(kErrSuccess, std::memory_order_release);
    s_abort.store(false, std::memory_order_release);
    memcpy(s_sha_expect, sha256, sizeof(s_sha_expect));
    strncpy(s_version, version ? version : "", sizeof(s_version) - 1);
    s_version[sizeof(s_version) - 1] = '\0';
    s_last_progress_ms = 0;

    // Publish RECEIVING before the worker exists: FeedData() must already accept the bytes
    // the App starts pushing the moment it sees the ACK.
    s_state.store(AGENT_OTA_RECEIVING, std::memory_order_release);
    if (s_cb) { agent_ota_status_t st; Snapshot(&st); s_cb(&st, s_cb_ctx); }

    // Internal-DRAM stack is mandatory: esp_ota_write disables the SPI cache while it talks to
    // flash, and a PSRAM stack is unreachable in that window (assert in the FreeRTOS port).
    if (xTaskCreate(WorkerTask, "agentlink_ota", 8192, nullptr, 5, &s_worker) != pdPASS) {
        s_worker = nullptr;
        s_state.store(AGENT_OTA_IDLE, std::memory_order_release);  // stop FeedData first
        Cleanup();
        return fail(kErrConfigFailed, "worker task create");
    }

    ESP_LOGI(TAG, "OTA started: %u bytes, %s -> %s%s%s", static_cast<unsigned>(fw_size),
             RunningVersion(), s_version, (model && *model) ? ", model=" : "",
             (model && *model) ? model : "");
    if (err_code) *err_code = kErrSuccess;
    return true;
}

size_t FeedData(const uint8_t* data, size_t len) {
    if (s_state.load(std::memory_order_acquire) != AGENT_OTA_RECEIVING) return 0;
    if (!s_buf || !data || len == 0) return 0;
    const size_t written = xStreamBufferSend(s_buf, data, len, 0);  // never block the RX task
    if (written) s_received.fetch_add(written, std::memory_order_acq_rel);
    if (written < len) {
        // The RX gate should have stopped the peer long before this. If it ever happens the
        // SHA-256 check catches the corruption, so the upgrade fails cleanly rather than
        // installing a truncated image.
        ESP_LOGE(TAG, "buffer overflow, dropped %u/%u bytes (RX gate breached?)",
                 static_cast<unsigned>(len - written), static_cast<unsigned>(len));
    }
    return written;
}

bool WantsMore() {
    if (s_state.load(std::memory_order_acquire) != AGENT_OTA_RECEIVING) return true;
    StreamBufferHandle_t buf = s_buf;   // handle is never deleted -> safe to read cross-task
    if (!buf) return true;
    return xStreamBufferSpacesAvailable(buf) >= kRxGateMinFree;
}

void Abort(const char* reason) {
    if (!IsBusy()) return;
    ESP_LOGW(TAG, "abort requested: %s", reason ? reason : "?");
    s_abort.store(true, std::memory_order_release);
    // The worker polls the buffer with a kPollMs timeout, so it notices within one period.
}

bool IsBusy() {
    const agent_ota_state_t st = s_state.load(std::memory_order_acquire);
    return st == AGENT_OTA_RECEIVING || st == AGENT_OTA_VERIFYING;
}

void GetStatus(agent_ota_status_t* out) {
    if (!out) return;
    Snapshot(out);
}

void OnLinkState(bool connected) {
    if (!connected) {
        Abort("link down");
        return;
    }
    // The link is the only upgrade path, so a link that comes up is the self-test that
    // matters: confirm a pending image here and the bootloader stops holding a rollback.
    (void)agent_link_ota_mark_valid();
}

}  // namespace ota
}  // namespace agentlink

// Public API (include/agent_link_ota.h)
extern "C" void agent_link_ota_set_callback(agent_ota_progress_cb_t cb, void* ctx) {
    agentlink::ota::s_cb_ctx = ctx;
    agentlink::ota::s_cb     = cb;
}
extern "C" void agent_link_ota_get_status(agent_ota_status_t* out) { agentlink::ota::GetStatus(out); }
extern "C" bool agent_link_ota_is_busy(void) { return agentlink::ota::IsBusy(); }
extern "C" void agent_link_ota_abort(void) { agentlink::ota::Abort("board requested"); }

#endif  // CONFIG_AGENT_LINK_OTA_ENABLE

// Available with or without the OTA engine: the version string and the rollback confirmation
// (which only ever matters for an image a rollback-enabled bootloader is still watching).
extern "C" const char* agent_link_ota_running_version(void) {
    const esp_app_desc_t* d = esp_app_get_description();
    return (d && d->version[0]) ? d->version : "0.0.0";
}

extern "C" esp_err_t agent_link_ota_mark_valid(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return ESP_OK;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) return ESP_OK;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;   // factory flash, or already valid
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "image confirmed valid (rollback cancelled): %s", esp_err_to_name(err));
    return err;
}
