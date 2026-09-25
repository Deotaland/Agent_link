// Device identity. See device_identity.h.

#include "device_identity.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char* TAG = "agent_link.ident";

// Canonical location, the one the BLE path has always used.
constexpr const char* kNs      = "agent_link";
constexpr const char* kKeyUuid = "dev_uuid";

// Legacy location: an early WiFi build stored its own string identity here. dev_identity_load()
// migrates it into the canonical key and erases it. Remove once no device carries it.
constexpr const char* kLegacyNs    = "al_dev";
constexpr const char* kLegacyKeySn = "sn";

std::mutex s_mtx;
bool       s_loaded = false;
uint8_t    s_uuid[16] = {};
char       s_sn[37] = {};

esp_err_t EnsureNvs() {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        r = nvs_flash_init();
    }
    return r;
}

void Format(const uint8_t b[16], char* out) {
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Accepts exactly 8-4-4-4-12 hex digits.
bool Parse(const char* s, uint8_t out[16]) {
    if (!s || strlen(s) != 36) return false;
    int n = 0;
    for (int i = 0; i < 36; ) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
            ++i;
            continue;
        }
        const int hi = HexVal(s[i]), lo = HexVal(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[n++] = static_cast<uint8_t>((hi << 4) | lo);
        i += 2;
    }
    return n == 16;
}

bool ReadCanonical(uint8_t out[16]) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 16;
    const bool ok = (nvs_get_blob(h, kKeyUuid, out, &len) == ESP_OK) && len == 16;
    nvs_close(h);
    return ok;
}

esp_err_t WriteCanonical(const uint8_t b[16]) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(kNs, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_blob(h, kKeyUuid, b, 16);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

bool ReadLegacy(uint8_t out[16], char* raw, size_t cap) {
    nvs_handle_t h;
    if (nvs_open(kLegacyNs, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    const bool got = (nvs_get_str(h, kLegacyKeySn, raw, &len) == ESP_OK);
    nvs_close(h);
    return got && Parse(raw, out);
}

void EraseLegacy() {
    nvs_handle_t h;
    if (nvs_open(kLegacyNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, kLegacyKeySn) == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}

void Take(const uint8_t b[16]) {
    memcpy(s_uuid, b, 16);
    Format(s_uuid, s_sn);
    s_loaded = true;
}
}  // namespace

esp_err_t dev_identity_load(bool allow_mint) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_loaded) return ESP_OK;

    esp_err_t r = EnsureNvs();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "nvs init: %s", esp_err_to_name(r));
        return r;
    }

    uint8_t canon[16] = {}, legacy[16] = {};
    char    legacy_raw[40] = {};
    const bool have_canon  = ReadCanonical(canon);
    const bool have_legacy = ReadLegacy(legacy, legacy_raw, sizeof legacy_raw);

    if (have_canon) {
        Take(canon);
        if (have_legacy) {
            if (memcmp(canon, legacy, 16) != 0) {
                // Keep the canonical one; it is the identity the App already knows.
                ESP_LOGW(TAG, "two identities on this unit: keeping %s, dropping the WiFi-only %s. "
                              "The platform's record under the second one is now orphaned — "
                              "unbind it in the console.", s_sn, legacy_raw);
            }
            EraseLegacy();
        }
        ESP_LOGI(TAG, "device identity %s", s_sn);
        return ESP_OK;
    }

    if (have_legacy) {
        // Only the legacy identity exists: adopt it, so the platform keeps its existing record.
        r = WriteCanonical(legacy);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "could not adopt %s as the device identity: %s", legacy_raw, esp_err_to_name(r));
            return r;
        }
        Take(legacy);
        EraseLegacy();
        ESP_LOGI(TAG, "device identity %s (adopted from the WiFi channel)", s_sn);
        return ESP_OK;
    }

    if (!allow_mint) return ESP_ERR_NOT_FOUND;

    uint8_t fresh[16];
    esp_fill_random(fresh, sizeof fresh);
    fresh[6] = static_cast<uint8_t>((fresh[6] & 0x0F) | 0x40);   // version 4
    fresh[8] = static_cast<uint8_t>((fresh[8] & 0x3F) | 0x80);   // variant RFC 4122
    r = WriteCanonical(fresh);
    if (r != ESP_OK) {
        // Don't use an identity the next boot would not reproduce.
        ESP_LOGE(TAG, "first boot: identity could not be persisted (%s) — not using a throwaway one",
                 esp_err_to_name(r));
        return r;
    }
    Take(fresh);
    ESP_LOGI(TAG, "first boot: minted device identity %s", s_sn);
    return ESP_OK;
}

const char* dev_identity_sn(void) {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_loaded ? s_sn : "";
}

bool dev_identity_uuid(uint8_t out[16]) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (!s_loaded || !out) return false;
    memcpy(out, s_uuid, 16);
    return true;
}
