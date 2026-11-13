#pragma once
#include <atomic>
#include <array>
#include "pcm_levels.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace sdgoods {
class Audio {
   public:
    enum class Test { kSpeaker, kRecordPlayback };
    enum class Stage { kIdle, kPreparing, kRecording, kPlaying, kDone, kStopped, kFailed };
    static constexpr size_t kCaptureCapacity = 320;
    // Streaming ownership is separate from the Factory's self-test instance.
    esp_err_t Init(int volume);
    void Shutdown();
    esp_err_t StartMic();
    esp_err_t StopMic();
    bool MicOn() const { return mic_on_.load(); }
    esp_err_t ReadPcm(int16_t* dest, size_t capacity, size_t* got);
    esp_err_t WritePcm(const int16_t* pcm, size_t frames);
    esp_err_t SetSpeakerEnabled(bool enabled);
    const std::array<kc_audio::PcmLevels, 2>& ChannelLevels() const { return channels_; }
    esp_err_t Start(Test test);
    // Blocking PCM playback on the caller's worker; PCM stays owned by that caller.
    esp_err_t PlayClip(const int16_t* pcm, size_t frames);
    esp_err_t SetVolume(int percent);
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
    esp_err_t InitMicrophone(bool enable = true);
    esp_err_t Capture(int16_t* pcm, size_t frames);
    esp_err_t Play(const int16_t* pcm, size_t frames);
    esp_err_t Write(const int16_t* stereo, size_t frames);
    void Amplifier(bool enabled);
    void Release();
    i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    std::atomic<bool> busy_{false}, stop_{false}, mic_on_{false}, speaker_requested_{false};
    bool streaming_ = false;
    size_t warmup_ = 0;
    int32_t previous_ = 0, filtered_ = 0;
    std::array<kc_audio::PcmLevels, 2> channels_{};
    std::atomic<int> volume_{35};
    std::atomic<Test> test_{Test::kSpeaker};
    std::atomic<Stage> stage_{Stage::kIdle};
    std::atomic<esp_err_t> error_{ESP_OK};
};
}  // namespace sdgoods
