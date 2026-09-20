#include "badge_data.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

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
};
Storage s;

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
    for (int i = 0; i < static_cast<int>(Field::kCount); ++i)
        std::strncpy(s.value[i], kDefaults[i], kMaxFieldLen);

    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved badge yet - showing defaults");
        return;
    }
    int restored = 0;
    for (int i = 0; i < static_cast<int>(Field::kCount); ++i) {
        size_t len = sizeof(s.value[i]);
        if (nvs_get_str(h, kFieldKeys[i], s.value[i], &len) == ESP_OK) ++restored;
        else std::strncpy(s.value[i], kDefaults[i], kMaxFieldLen);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %d/%d field(s): name='%s' title='%s'", restored,
             static_cast<int>(Field::kCount), s.value[0], s.value[1]);
}

void Data::Set(Field f, const char* utf8, size_t len) {
    const int i = static_cast<int>(f);
    if (i < 0 || i >= static_cast<int>(Field::kCount)) return;
    if (!utf8) len = 0;
    if (len > kMaxFieldLen) len = Utf8SafeLen(utf8, kMaxFieldLen);

    char buf[kMaxFieldLen + 1] = {};
    if (len) std::memcpy(buf, utf8, len);
    if (std::strcmp(buf, s.value[i]) == 0) return;   // no change: skip the flash write and the redraw

    std::memcpy(s.value[i], buf, sizeof(buf));
    s.revision.fetch_add(1, std::memory_order_release);

    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, kFieldKeys[i], s.value[i]) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "NVS unavailable - '%s' will not survive a reboot", kFieldKeys[i]);
    }
    ESP_LOGI(TAG, "%s = '%s'", kFieldKeys[i], s.value[i]);
}

const char* Data::Get(Field f) const {
    const int i = static_cast<int>(f);
    return (i >= 0 && i < static_cast<int>(Field::kCount)) ? s.value[i] : "";
}

uint32_t Data::Revision() const { return s.revision.load(std::memory_order_acquire); }

}  // namespace badge
