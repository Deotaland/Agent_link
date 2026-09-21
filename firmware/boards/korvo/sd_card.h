#pragma once
// SdCard: the Korvo TF slot, 1-line SDMMC, mounted as FAT.
//
// The board wires only CLK/CMD/D0, so this is 1-bit mode — slower than 4-bit but the only option
// here, and plenty for writing a 16kHz mono WAV (32KB/s against several MB/s of headroom).
//
// Board-private for now: promote it to boards/common the moment a second board wants an SD card
// (it takes no Korvo-specific knowledge — everything is in SdCardConfig).

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"

struct SdCardConfig {
    gpio_num_t  pin_clk;
    gpio_num_t  pin_cmd;
    gpio_num_t  pin_d0;
    const char* mount_point = "/sdcard";
    int         max_files   = 4;
    int         max_freq_khz = SDMMC_FREQ_DEFAULT;   // 20MHz; drop to SDMMC_FREQ_PROBING if flaky
    // Formatting is destructive, so it is off: a card that will not mount is reported, not wiped.
    bool        format_if_mount_failed = false;
};

class SdCard {
public:
    SdCard() = default;
    SdCard(const SdCard&) = delete;
    SdCard& operator=(const SdCard&) = delete;

    // Mount the card. Returns ESP_ERR_NOT_FOUND when no card is in the slot, which is a normal
    // state, not a fault — the caller should carry on with recording disabled.
    esp_err_t Init(const SdCardConfig& cfg);
    void      Deinit();

    bool        Ready()      const { return card_ != nullptr; }
    const char* MountPoint() const { return cfg_.mount_point; }

    uint64_t CapacityMb() const;
    uint64_t FreeMb()     const;   // 0 if unknown / not mounted

private:
    SdCardConfig  cfg_  = {};
    sdmmc_card_t* card_ = nullptr;
};
