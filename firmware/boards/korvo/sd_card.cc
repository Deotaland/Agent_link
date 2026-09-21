// SdCard: Korvo TF slot over 1-line SDMMC, mounted as FAT. See sd_card.h.

#include "sd_card.h"

#include <cstring>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

namespace {
constexpr const char* TAG = "sdcard";
}  // namespace

esp_err_t SdCard::Init(const SdCardConfig& cfg) {
    cfg_ = cfg;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = cfg_.max_freq_khz;
    host.flags       &= ~SDMMC_HOST_FLAG_8BIT;
    host.flags       &= ~SDMMC_HOST_FLAG_4BIT;
    host.flags       |= SDMMC_HOST_FLAG_1BIT;   // only CLK/CMD/D0 are broken out on this board

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width    = 1;
    slot.clk      = cfg_.pin_clk;
    slot.cmd      = cfg_.pin_cmd;
    slot.d0       = cfg_.pin_d0;
    slot.cd       = SDMMC_SLOT_NO_CD;           // no card-detect line on the Korvo slot
    slot.wp       = SDMMC_SLOT_NO_WP;
    slot.flags   |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // the board has no external pull-ups on CMD/D0

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = cfg_.format_if_mount_failed;
    mount.max_files              = cfg_.max_files;
    mount.allocation_unit_size   = 16 * 1024;

    const esp_err_t r = esp_vfs_fat_sdmmc_mount(cfg_.mount_point, &host, &slot, &mount, &card_);
    if (r != ESP_OK) {
        card_ = nullptr;
        if (r == ESP_ERR_NOT_FOUND || r == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "no card in the slot (%s)", esp_err_to_name(r));
            return ESP_ERR_NOT_FOUND;
        }
        // ESP_FAIL here is almost always "mounted but no FAT filesystem" — the card needs formatting
        // on a PC, which is the user's call, not ours.
        ESP_LOGE(TAG, "mount failed: %s%s", esp_err_to_name(r),
                 r == ESP_FAIL ? " (card present but not FAT-formatted?)" : "");
        return r;
    }

    ESP_LOGI(TAG, "mounted at %s: %s %lluMB (1-bit SDMMC @ %dkHz)", cfg_.mount_point,
             card_->cid.name, static_cast<unsigned long long>(CapacityMb()), cfg_.max_freq_khz);
    return ESP_OK;
}

void SdCard::Deinit() {
    if (!card_) return;
    (void)esp_vfs_fat_sdcard_unmount(cfg_.mount_point, card_);
    card_ = nullptr;
}

uint64_t SdCard::CapacityMb() const {
    if (!card_) return 0;
    return (static_cast<uint64_t>(card_->csd.capacity) * card_->csd.sector_size) / (1024ull * 1024ull);
}

uint64_t SdCard::FreeMb() const {
    if (!card_) return 0;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(cfg_.mount_point, &total, &freeb) != ESP_OK) return 0;
    return freeb / (1024ull * 1024ull);
}
