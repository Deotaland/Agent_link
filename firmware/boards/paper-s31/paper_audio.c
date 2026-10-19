#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "board_pins.h"
#include "i2c_bus.h"
#include "paper_audio.h"

static const char *TAG = "paper.audio";
enum { RATE = 16000, FRAMES = 320, CHANNELS = 2 };
static const audio_codec_data_if_t *s_data;
static esp_codec_dev_handle_t s_spk, s_mic;
static bool s_attempted;
static bool s_mic_on;
static bool s_spk_on;

static esp_codec_dev_handle_t make_codec(esp_codec_dev_type_t type,
                                       esp_codec_dec_work_mode_t mode)
{
    audio_codec_i2c_cfg_t ic = {
        .port = I2C_NUM_0, .addr = ES8389_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus_get(),
    };
    es8389_codec_cfg_t cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&ic),
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = mode, .pa_pin = -1,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    if (!cfg.ctrl_if || !cfg.gpio_if) return NULL;
    const audio_codec_if_t *codec = es8389_codec_new(&cfg);
    if (!codec) return NULL;
    esp_codec_dev_cfg_t dev = { .dev_type = type, .codec_if = codec, .data_if = s_data };
    return esp_codec_dev_new(&dev);
}

static void pa_enable(bool on)
{
    gpio_set_level(PA_CTRL_GPIO, on ? PA_ON_LEVEL : !PA_ON_LEVEL);
}

esp_err_t paper_audio_init(void)
{
    if (s_spk && s_mic) return ESP_OK;
    // A failed partial initialization must not allocate a second I2S pair.
    if (s_attempted) return ESP_ERR_INVALID_STATE;
    s_attempted = true;
    // The board configures the PA pin once; audio only changes its level.
    pa_enable(false);
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "I2C bus");
    ESP_RETURN_ON_FALSE(i2c_bus_probe(ES8389_I2C_ADDR), ESP_ERR_NOT_FOUND, TAG, "ES8389 missing");
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &tx, &rx), TAG, "I2S channels");
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO, .bclk = I2S_BCK_GPIO, .ws = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO, .din = I2S_DIN_GPIO,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &sc), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &sc), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "Enable TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "Enable RX");
    audio_codec_i2s_cfg_t ic = { .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = rx };
    s_data = audio_codec_new_i2s_data(&ic);
    ESP_RETURN_ON_FALSE(s_data, ESP_ERR_NO_MEM, TAG, "I2S data interface");
    s_spk = make_codec(ESP_CODEC_DEV_TYPE_OUT, ESP_CODEC_DEV_WORK_MODE_DAC);
    s_mic = make_codec(ESP_CODEC_DEV_TYPE_IN, ESP_CODEC_DEV_WORK_MODE_ADC);
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "ES8389 initialization");
    return ESP_OK;
}

static esp_err_t speaker_open(void)
{
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = CHANNELS, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    if (esp_codec_dev_set_out_vol(s_spk, 65) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(s_spk);
        return ESP_FAIL;
    }
    pa_enable(true);
    return ESP_OK;
}

static void speaker_close(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    pa_enable(false);
    esp_codec_dev_close(s_spk);
}

esp_err_t paper_audio_speaker_start(void)
{
    if (s_spk_on) return ESP_OK;
    ESP_RETURN_ON_ERROR(paper_audio_init(), TAG, "Audio unavailable");
    esp_err_t result = speaker_open();
    s_spk_on = result == ESP_OK;
    return result;
}

esp_err_t paper_audio_speaker_stop(void)
{
    if (s_spk_on) speaker_close();
    s_spk_on = false;
    return ESP_OK;
}

esp_err_t paper_audio_write_mono(const int16_t *pcm, size_t samples)
{
    if (!s_spk_on) return ESP_ERR_INVALID_STATE;
    if (!pcm || samples > FRAMES) return ESP_ERR_INVALID_ARG;
    int16_t stereo[FRAMES * CHANNELS];
    for (size_t i = 0; i < samples; ++i) stereo[2 * i] = stereo[2 * i + 1] = pcm[i];
    return esp_codec_dev_write(s_spk, stereo, samples * CHANNELS * sizeof(int16_t)) == ESP_CODEC_DEV_OK
        ? ESP_OK : ESP_FAIL;
}

esp_err_t paper_audio_mic_start(void)
{
    if (s_mic_on) return ESP_OK;
    ESP_RETURN_ON_ERROR(paper_audio_init(), TAG, "Audio unavailable");
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = CHANNELS, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    if (esp_codec_dev_set_in_gain(s_mic, 30) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(s_mic);
        return ESP_FAIL;
    }
    s_mic_on = true;
    return ESP_OK;
}

esp_err_t paper_audio_mic_stop(void)
{
    if (s_mic_on) esp_codec_dev_close(s_mic);
    s_mic_on = false;
    return ESP_OK;
}

bool paper_audio_mic_active(void) { return s_mic_on; }

esp_err_t paper_audio_read_mono(int16_t *pcm, size_t capacity, size_t *samples)
{
    if (samples) *samples = 0;
    if (!s_mic_on) return ESP_ERR_INVALID_STATE;
    if (!pcm || !samples || !capacity || capacity > FRAMES) return ESP_ERR_INVALID_ARG;
    int16_t stereo[FRAMES * CHANNELS];
    if (esp_codec_dev_read(s_mic, stereo, capacity * CHANNELS * sizeof(int16_t)) != ESP_CODEC_DEV_OK)
        return ESP_FAIL;
    for (size_t i = 0; i < capacity; ++i)
        pcm[i] = ((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2;
    *samples = capacity;
    return ESP_OK;
}
