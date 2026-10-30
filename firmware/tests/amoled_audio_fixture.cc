#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
#include "pcm_levels.h"

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = -2, ESP_ERR_INVALID_SIZE = -3;
constexpr size_t kPlayFrames = 256;
constexpr int AUDIO_HARDWARE_SAMPLE_RATE = 16000;
#define ESP_LOGE(...) ((void)0)
struct esp_codec_dev_sample_info_t { int sample_rate = 0, bits_per_sample = 0; uint8_t channel = 0; uint16_t channel_mask = 0; };
using esp_codec_dev_handle_t = void*;
enum esp_codec_dev_type_t { ESP_CODEC_DEV_TYPE_IN = 1, ESP_CODEC_DEV_TYPE_OUT = 2 };
struct audio_codec_data_if_t {
    int (*open)(const audio_codec_data_if_t*, void*, int);
    bool (*is_open)(const audio_codec_data_if_t*);
    int (*enable)(const audio_codec_data_if_t*, esp_codec_dev_type_t, bool);
    int (*set_fmt)(const audio_codec_data_if_t*, esp_codec_dev_type_t, esp_codec_dev_sample_info_t*);
    int (*read)(const audio_codec_data_if_t*, uint8_t*, int);
    int (*write)(const audio_codec_data_if_t*, uint8_t*, int);
    int (*close)(const audio_codec_data_if_t*);
};
// INSERT_CHECKED_DATA
CheckedData* open_data = nullptr;
int open_error = 0, format_error = 0, enable_error = 0, closes = 0;
int esp_codec_dev_open(void*, esp_codec_dev_sample_info_t* fs) {
    // Reproduce the upstream API ignoring these driver results.
    open_data->api.set_fmt(&open_data->api, ESP_CODEC_DEV_TYPE_IN, fs);
    open_data->api.enable(&open_data->api, ESP_CODEC_DEV_TYPE_IN, true);
    return open_error;
}
int esp_codec_dev_close(void*) { ++closes; return 0; }

bool short_read = false, short_write = false;
int read_error = ESP_OK, write_error = ESP_OK;
size_t reads = 0, writes = 0;
std::vector<int16_t> speaker;
int i2s_channel_read(void*, void* data, size_t bytes, size_t* got, int) {
    ++reads;
    if (read_error) return read_error;
    assert(bytes == 320 * 2 * sizeof(int16_t));
    auto* pcm = static_cast<int16_t*>(data);
    for (size_t i = 0; i < 320; ++i) {
        pcm[i * 2] = i % 2 ? 1000 : -1000;  // MIC1 left
        pcm[i * 2 + 1] = -2000;            // MIC2 right
    }
    *got = bytes - (short_read ? 2 : 0); return ESP_OK;
}
int i2s_channel_write(void*, const void* data, size_t bytes, size_t* written, int) {
    ++writes;
    if (write_error) return write_error;
    const auto* pcm = static_cast<const int16_t*>(data);
    assert(bytes % 4 == 0);
    for (size_t i = 0; i < bytes / sizeof(int16_t); i += 2) assert(pcm[i] == pcm[i + 1]);
    speaker.insert(speaker.end(), pcm, pcm + bytes / sizeof(int16_t));
    *written = bytes - (short_write ? 2 : 0); return ESP_OK;
}
class AmoledAudio {
public:
    static constexpr size_t kHardwareFrames = 320;
    struct State {
        void* rx = nullptr; void* tx = nullptr;
        int16_t interleaved[kHardwareFrames * 2]{}, stereo[kPlayFrames * 2]{};
        CheckedData checked;
        // INSERT_OPEN
    };
    State* state_ = nullptr;
    bool mic_on_ = false;
    std::array<kc_audio::PcmLevels, 2> channels_;
    int ReadPcm(int16_t*, size_t, size_t*);
    int WritePcm(const int16_t*, size_t);
};
// INSERT_READ_WRITE

int main() {
    AmoledAudio audio;
    int16_t pcm[322]; std::fill(std::begin(pcm), std::end(pcm), int16_t(-123));
    size_t got = 99;
    assert(audio.ReadPcm(pcm, 320, &got) == ESP_ERR_INVALID_STATE && got == 0);
    AmoledAudio::State state;
    audio.state_ = &state; audio.mic_on_ = true;
    assert(audio.ReadPcm(pcm, 319, &got) == ESP_ERR_INVALID_SIZE && got == 0 && !reads);
    assert(audio.ReadPcm(pcm + 1, 320, &got) == ESP_OK && got == 320);
    assert(pcm[0] == -123 && pcm[321] == -123);
    for (size_t i = 0; i < got; ++i) assert(pcm[i + 1] == (i % 2 ? 1000 : -1000));
    assert(audio.channels_[0].samples == 320 && audio.channels_[0].Rms() == 1000);
    assert(audio.channels_[1].Rms() == 2000 && audio.channels_[1].Mean() == -2000);
    kc_audio::PcmLevels aggregate;
    aggregate.Merge(audio.channels_[0]); aggregate.Merge(audio.channels_[1]);
    assert(aggregate.samples == 640 && aggregate.minimum == -2000 && aggregate.maximum == 1000);
    assert(aggregate.Mean() == -1000);
    short_read = true;
    assert(audio.ReadPcm(pcm, 320, &got) == ESP_ERR_INVALID_SIZE && got == 0); short_read = false;
    read_error = -11; assert(audio.ReadPcm(pcm, 320, &got) == -11 && got == 0); read_error = 0;

    int16_t input[513];
    for (size_t i = 0; i < 513; ++i) input[i] = static_cast<int16_t>(i - 256);
    input[0] = -32768; input[512] = 32767;
    assert(audio.WritePcm(input, 513) == ESP_OK && writes == 3);
    assert(speaker.size() == 513 * 2);  // No rate conversion or duration drift.
    for (size_t i = 0; i < 513; ++i) assert(speaker[i * 2] == input[i] && speaker[i * 2 + 1] == input[i]);
    assert(input[0] == -32768 && input[512] == 32767);
    short_write = true; assert(audio.WritePcm(input, 256) == ESP_ERR_INVALID_SIZE); short_write = false;
    write_error = -12; assert(audio.WritePcm(input, 256) == -12); write_error = 0;

    audio_codec_data_if_t driver{};
    driver.set_fmt = [](const audio_codec_data_if_t*, esp_codec_dev_type_t, esp_codec_dev_sample_info_t* fs) {
        assert(fs->sample_rate == 16000 && fs->bits_per_sample == 16 && fs->channel == 2);
        return format_error;
    };
    driver.enable = [](const audio_codec_data_if_t*, esp_codec_dev_type_t, bool) { return enable_error; };
    state.checked.Init(&driver); open_data = &state.checked;
    assert(state.Open(nullptr, 2, 0) == ESP_OK);
    format_error = -17; assert(state.Open(nullptr, 2, 0) == ESP_FAIL && closes == 1);
    format_error = 0; enable_error = -18; assert(state.Open(nullptr, 2, 0) == ESP_FAIL && closes == 2);
    enable_error = 0; open_error = -19; assert(state.Open(nullptr, 2, 0) == ESP_FAIL && closes == 3);
    open_error = 0; assert(state.Open(nullptr, 2, 0) == ESP_OK);
    puts("AMOLED C audio: standard stereo MIC1/MIC2 extraction, complete DMA frames, unchanged mono uplink, stereo playback/extrema, error propagation and hidden I2S open failures passed.");
}
