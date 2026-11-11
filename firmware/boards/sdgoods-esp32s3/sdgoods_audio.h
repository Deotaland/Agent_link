#pragma once
#include <atomic>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace sdgoods {
class Audio {
   public:
    enum class Test { kSpeaker, kRecordPlayback };
    enum class Stage { kIdle, kPreparing, kRecording, kPlaying, kDone, kStopped, kFailed };
    esp_err_t Start(Test test);
    void SetVolume(int percent);
    void Stop();
    bool StopAndWait();
    bool Busy() const {
        return busy_.load();
    }
    Stage State() const {
        return stage_.load();
    }
    esp_err_t Error() const {
        return error_.load();
    }

   private:
    static void Entry(void* context);
    void Run();
    esp_err_t InitSpeaker();
    esp_err_t InitMicrophone();
    esp_err_t Capture(int16_t* pcm, size_t frames);
    esp_err_t Play(const int16_t* pcm, size_t frames);
    esp_err_t Write(const int16_t* stereo, size_t frames);
    void Amplifier(bool enabled);
    void Release();
    i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    std::atomic<bool> busy_{false}, stop_{false};
    std::atomic<int> volume_{35};
    std::atomic<Test> test_{Test::kSpeaker};
    std::atomic<Stage> stage_{Stage::kIdle};
    std::atomic<esp_err_t> error_{ESP_OK};
};
}  // namespace sdgoods
