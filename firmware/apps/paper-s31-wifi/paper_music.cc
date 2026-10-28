#include "paper_music.h"
#include "paper_storage.h"
#include "paper_protocol.h"
#include <algorithm>
#include <dirent.h>

esp_err_t PaperMusic::Scan() {
    const auto result = paper_sd_mount();
    if (result != ESP_OK) return result;
    std::vector<std::string> tracks;
    DIR* dir = opendir("/sdcard/music");
    if (!dir) return ESP_ERR_NOT_FOUND;
    while (auto* item = readdir(dir)) {
        if (paper::SafeTrack(item->d_name)) tracks.emplace_back(item->d_name);
        if (tracks.size() == 64) break;
    }
    closedir(dir);
    std::sort(tracks.begin(), tracks.end());
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_ = std::move(tracks);
    return ESP_OK;
}
std::vector<std::string> PaperMusic::Tracks() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_;
}
bool PaperMusic::Stop() {
    generation_.fetch_add(1);
    { std::lock_guard<std::mutex> lock(mutex_); requested_.clear(); }
    for (unsigned i = 0; i < 200 && playing_.load(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (playing_.load()) return false;
    audio_.Stop();
    return true;
}
esp_err_t PaperMusic::Play(const std::string& name) {
    if (!audio_.Ready()) return ESP_ERR_INVALID_STATE;
    const auto list = Tracks();
    if (std::find(list.begin(), list.end(), name) == list.end()) return ESP_ERR_NOT_FOUND;
    if (!Stop()) return ESP_ERR_TIMEOUT;
    if (!task_ && xTaskCreate(Entry, "paper_music", 6144, this, 3, &task_) != pdPASS) return ESP_ERR_NO_MEM;
    { std::lock_guard<std::mutex> lock(mutex_); requested_ = name; }
    return ESP_OK;
}
void PaperMusic::Loop() {
    while (true) {
        std::string name;
        uint32_t generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            name.swap(requested_); generation = generation_.load();
            if (!name.empty()) playing_.store(true);
        }
        if (name.empty()) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        error_.store(ESP_OK);
        FILE* file = fopen(("/sdcard/music/" + name).c_str(), "rb");
        paper::Wav wav;
        if (!paper::ReadWav(file, wav)) error_.store(ESP_ERR_NOT_SUPPORTED);
        else {
            int16_t input[1024], output[2048];
            unsigned phase = 0;
            while (wav.bytes && generation == generation_.load()) {
                const size_t count = std::min<size_t>(wav.bytes, sizeof input);
                const size_t got = fread(input, 1, count, file);
                if (got != count) { error_.store(ESP_ERR_INVALID_RESPONSE); break; }
                wav.bytes -= got;
                size_t samples = 0;
                for (size_t i = 0; i < got / 2; i += wav.channels) {
                    const int16_t value = wav.channels == 1 ? input[i] : (int(input[i]) + input[i + 1]) / 2;
                    phase += 16000;
                    while (phase >= wav.rate) { output[samples++] = value; phase -= wav.rate; }
                }
                if (!audio_.PlayLocal(reinterpret_cast<const uint8_t*>(output), samples * 2, generation_, generation)) break;
            }
        }
        if (file) fclose(file);
        audio_.End();
        while (generation == generation_.load() && !audio_.PlaybackIdle()) vTaskDelay(pdMS_TO_TICKS(10));
        playing_.store(false);
    }
}
