#include "amoled_audio.h"
#include "amoled_orientation.h"
#include "config.h"
#include <cassert>
#include <cstdio>
#include <cstring>

EsCodecConfig observed{};
int init_error = ESP_OK, read_error = ESP_OK, read_calls = 0, observed_volume = 0;

void CheckOrientation() {
    using namespace amoled_voice;
    static_assert(DISPLAY_ROTATION_CW == 90);
    constexpr auto rect = Clockwise90Rect({10, 20, 6, 4}, DISPLAY_WIDTH);
    static_assert(rect.x == 442 && rect.y == 10 && rect.w == 4 && rect.h == 6);
    static_assert(Clockwise90Touch(465, 0, 466).x == 0);
    static_assert(Clockwise90Touch(465, 0, 466).y == 0);
    static_assert(Clockwise90Touch(0, 465, 466).x == 465);
    static_assert(Clockwise90Touch(0, 465, 466).y == 465);
    // A rectangular partial flush, with distinct high/low bytes per pixel.
    const uint8_t source[]{1, 11, 2, 12, 3, 13, 4, 14, 5, 15, 6, 16};
    const uint8_t expected[]{4, 14, 1, 11, 5, 15, 2, 12, 6, 16, 3, 13};
    uint8_t target[14]{}; target[12] = 77; target[13] = 88;
    assert(!RotateRgb565Clockwise90(source, target, 3, 2, 11));
    assert(target[0] == 0);
    assert(!RotateRgb565Clockwise90(source, target, 0, 2, sizeof target));
    assert(RotateRgb565Clockwise90(source, target, 3, 2, 12));
    assert(memcmp(target, expected, sizeof expected) == 0);
    assert(target[12] == 77 && target[13] == 88 && source[0] == 1);
    // Full-panel corners and all intermediate points round-trip with touch.
    for (int y = 0; y < DISPLAY_HEIGHT; ++y) {
        for (int x = 0; x < DISPLAY_WIDTH; ++x) {
            const auto physical = Clockwise90Rect({x, y, 1, 1}, DISPLAY_WIDTH);
            const auto logical = Clockwise90Touch(physical.x, physical.y, DISPLAY_WIDTH);
            assert(logical.x == x && logical.y == y);
        }
    }
}

int main() {
    CheckOrientation();
    static_assert(DISPLAY_RST_PIN == 39 && TOUCH_RST_PIN == 40 && AUDIO_I2S_MCLK == 42);
    static_assert(CLOUD_PRODUCT_ID == 10 && DISPLAY_WIDTH == 466 && AUDIO_SAMPLE_RATE == 16000);
    static_assert(TOUCH_MIRROR_X && TOUCH_MIRROR_Y);
    assert(strcmp(AMOLED_FIRMWARE_MODEL, "ESP32-S3-Touch-AMOLED-1.75-16M") == 0);
    AmoledAudio audio;
    init_error = ESP_FAIL;
    assert(audio.Init(60) == ESP_FAIL);
    init_error = ESP_OK;
    assert(audio.Init(60) == ESP_OK);
    assert(observed.i2c_port == 0 && observed.pin_sda == -1 && observed.pin_scl == -1);
    assert(observed.pin_mclk == 42 && observed.pin_bclk == 9 && observed.pin_ws == 45);
    assert(observed.pin_dout == 8 && observed.pin_din == 10 && observed.pin_pa_en == -1);
    assert(observed.es7210_addr == 0x40 && observed.es8311_addr == 0x18 && observed.sample_rate == 16000);
    assert(observed.mic_gain == 24 && observed.out_volume == 60);
    int16_t pcm[322]{}; pcm[320] = 123; pcm[321] = 456;
    size_t got = 99;
    assert(audio.ReadPcm(pcm, 320, &got) == ESP_ERR_INVALID_STATE && got == 0);
    assert(audio.StartMic() == ESP_OK && audio.MicOn());
    assert(audio.ReadPcm(pcm, 319, &got) == ESP_ERR_INVALID_SIZE && read_calls == 0);
    assert(audio.ReadPcm(pcm, 322, &got) == ESP_OK && got == 320 && read_calls == 1);
    assert(pcm[320] == 123 && pcm[321] == 456);
    assert(audio.ChannelLevels()[0].samples == 320 && audio.ChannelLevels()[0].minimum == -2000);
    assert(audio.ChannelLevels()[1].samples == 0);  // MIC2 is not exposed by the shared driver.
    read_error = ESP_FAIL;
    assert(audio.ReadPcm(pcm, 320, &got) == ESP_FAIL && got == 0 && audio.ChannelLevels()[0].samples == 0);
    assert(audio.WritePcm(pcm, 320) == ESP_OK);
    assert(audio.SetVolume(70) == ESP_OK && observed_volume == 70);
    assert(audio.StopMic() == ESP_OK && !audio.MicOn());
    puts("Non-C AMOLED 1.75: 90-degree frame/touch rotation, product 10, pins, shared-bus codec, 20ms capture and error propagation passed.");
}
