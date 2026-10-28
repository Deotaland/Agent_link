#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "paper_wifi_audio.h"

class PaperMusic {
public:
    explicit PaperMusic(PaperWifiAudio& audio) : audio_(audio) {}
    esp_err_t Scan();
    std::vector<std::string> Tracks();
    esp_err_t Play(const std::string& name);
    bool Stop();
    bool Playing() const { return playing_.load(); }
    esp_err_t Error() const { return error_.load(); }
private:
    static void Entry(void* ctx) { static_cast<PaperMusic*>(ctx)->Loop(); }
    void Loop();
    PaperWifiAudio& audio_;
    std::mutex mutex_;
    std::vector<std::string> tracks_;
    std::string requested_;
    TaskHandle_t task_ = nullptr;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> playing_{false};
    std::atomic<esp_err_t> error_{ESP_OK};
};
