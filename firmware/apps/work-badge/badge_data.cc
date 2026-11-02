#include "badge_data.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace badge {
namespace {

constexpr const char* TAG = "badge.data";
constexpr const char* kNvsNamespace = "badge";

// NVS keys, indexed by Field. NVS keys are capped at 15 characters.
const char* const kFieldKeys[static_cast<int>(Field::kCount)] = {
    "name", "title", "dept", "emp_id", "company",
};

// Shown until the App sends the real thing, so a freshly flashed badge still looks like a
// badge instead of a blank screen.
const char* const kDefaults[static_cast<int>(Field::kCount)] = {
    "NEW BADGE", "Unassigned", "", "----", "DEOTALAND",
};

struct Storage {
    char     value[static_cast<int>(Field::kCount)][kMaxFieldLen + 1] = {};
    std::atomic<uint32_t> revision{1};
    std::mutex lock;
    TaskHandle_t writer = nullptr;
};
Storage s;

void Persist(void*) {
    uint32_t pending = 0;
    while (true) {
        uint32_t changed = 0;
        xTaskNotifyWait(0, UINT32_MAX, &changed,
                       pending ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
        pending |= changed;
        for (size_t i = 0; i < kFieldCount; ++i) {
            if (!(pending & (1u << i))) continue;
            char value[kMaxFieldLen + 1];
            { std::lock_guard<std::mutex> guard(s.lock); memcpy(value, s.value[i], sizeof value); }
            nvs_handle_t h;
            esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
            if (result == ESP_OK) {
                result = nvs_set_str(h, kFieldKeys[i], value);
                if (result == ESP_OK) result = nvs_commit(h);
                nvs_close(h);
            }
            if (result == ESP_OK) pending &= ~(1u << i);
            else ESP_LOGW(TAG, "cannot save '%s'; retrying (%d)", kFieldKeys[i], result);
        }
    }
}

// Longest prefix of `len` bytes that does not split a UTF-8 sequence. Cutting mid-character
// would leave a stray continuation byte that the font renderer draws as garbage.
size_t Utf8SafeLen(const char* s_in, size_t len) {
    if (len == 0) return 0;
    size_t n = len;
    while (n > 0 && (static_cast<unsigned char>(s_in[n]) & 0xC0) == 0x80) --n;  // back off continuations
    return n;
}

}  // namespace

Data& Data::Instance() {
    static Data instance;
    return instance;
}

void Data::Load() {
    // Applications are constructed before the SDK starts its transport/NVS.
    const esp_err_t initialized = nvs_flash_init();
    for (int i = 0; i < static_cast<int>(Field::kCount); ++i)
        std::strncpy(s.value[i], kDefaults[i], kMaxFieldLen);

    nvs_handle_t h;
    if (initialized != ESP_OK || nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved badge yet - showing defaults");
    } else {
        int restored = 0;
        for (int i = 0; i < static_cast<int>(Field::kCount); ++i) {
            size_t len = sizeof(s.value[i]);
            if (nvs_get_str(h, kFieldKeys[i], s.value[i], &len) == ESP_OK) ++restored;
            else std::strncpy(s.value[i], kDefaults[i], kMaxFieldLen);
        }
        nvs_close(h);
        ESP_LOGI(TAG, "loaded %d badge fields", restored);
    }
    if (!s.writer && xTaskCreate(Persist, "badge_store", 3072, nullptr, 3, &s.writer) != pdPASS)
        ESP_LOGE(TAG, "storage worker unavailable; badge changes will not be saved");
}

void Data::Set(Field f, const char* utf8, size_t len) {
    const int i = static_cast<int>(f);
    if (i < 0 || i >= static_cast<int>(Field::kCount)) return;
    if (!utf8) len = 0;
    if (len > kMaxFieldLen) len = Utf8SafeLen(utf8, kMaxFieldLen);

    char buf[kMaxFieldLen + 1] = {};
    if (len) std::memcpy(buf, utf8, len);
    {
        std::lock_guard<std::mutex> guard(s.lock);
        if (std::strcmp(buf, s.value[i]) == 0) return;
        std::memcpy(s.value[i], buf, sizeof(buf));
        s.revision.fetch_add(1, std::memory_order_release);
    }
    if (s.writer) xTaskNotify(s.writer, 1u << i, eSetBits);
}

void Data::Copy(Snapshot& out) const {
    std::lock_guard<std::mutex> guard(s.lock);
    memcpy(out.values, s.value, sizeof out.values);
}

uint32_t Data::Revision() const { return s.revision.load(std::memory_order_acquire); }

}  // namespace badge
