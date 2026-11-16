#include "boot_animation.h"
#include "boot_animation_format.h"
#include "agent_system.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "sdgoods_power.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "jpeg_decoder.h"
#include "nvs.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <new>

namespace {
constexpr const char* kTag = "factory.boot";
using Bytes = std::shared_ptr<uint8_t>;
Bytes Allocate(size_t size) {
    return Bytes(static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 heap_caps_free);
}
Bytes Load(size_t* size) {
    const auto* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "appdata");
    if (!partition || partition->address != 0x1000000 || partition->size < 4096) {
        ESP_LOGW(kTag, "appdata layout mismatch; skipping animation");
        return {};
    }
    esp_vfs_fat_mount_config_t config{};
    config.format_if_mount_failed = false;
    config.max_files = 2;
    config.allocation_unit_size = 4096;
    wl_handle_t wl = WL_INVALID_HANDLE;
    auto error = esp_vfs_fat_spiflash_mount_rw_wl("/appdata", "appdata", &config, &wl);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "appdata mount failed: %s; skipping animation", esp_err_to_name(error));
        return {};
    }
    Bytes data;
    FILE* file = fopen("/appdata/START.BVA", "rb");
    if (file) {
        if (fseek(file, 0, SEEK_END) == 0) {
            const long length = ftell(file);
            if (length >= long(sugarat_boot::kHeaderBytes) && length <= long(sugarat_boot::kMaxBytes) &&
                fseek(file, 0, SEEK_SET) == 0) {
                data = Allocate(length);
                if (data && fread(data.get(), 1, length, file) == size_t(length))
                    *size = length;
                else
                    data.reset();
            }
        }
        fclose(file);
    }
    error = esp_vfs_fat_spiflash_unmount_rw_wl("/appdata", wl);
    if (error != ESP_OK)
        ESP_LOGW(kTag, "appdata unmount failed: %s", esp_err_to_name(error));
    if (!data) ESP_LOGW(kTag, "START.BVA missing or unreadable; entering home");
    return data;
}
struct Sound {
    Bytes data;
    sdgoods::Audio* audio;
    size_t frames;
    std::atomic<bool> done{false};
};
void SoundEntry(void* argument) {
    {
        auto* owner = static_cast<std::shared_ptr<Sound>*>(argument);
        auto sound = std::move(*owner);
        delete owner;
        const auto* pcm = reinterpret_cast<const int16_t*>(sound->data.get() + sugarat_boot::kHeaderBytes);
        const auto error = sound->audio->PlayClip(pcm, sound->frames);
        if (error != ESP_OK) ESP_LOGW(kTag, "startup audio: %s", esp_err_to_name(error));
        sound->done.store(true);
    } // Drop the PCM owner before deleting this FreeRTOS task.
    vTaskDelete(nullptr);
}
esp_err_t Decode(const sugarat_boot::Movie& movie, unsigned index, uint8_t* pixels, void* work) {
    size_t length = 0;
    const auto* frame = movie.Frame(index, &length);
    esp_jpeg_image_cfg_t config{};
    config.indata = const_cast<uint8_t*>(frame);
    config.indata_size = length;
    config.outbuf = pixels;
    config.outbuf_size = 360 * 360 * 2;
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    config.out_scale = JPEG_IMAGE_SCALE_0;
    config.flags.swap_color_bytes = 1;
    config.advanced.working_buffer = work;
    config.advanced.working_buffer_size = 72 * 1024;
    esp_jpeg_image_output_t info{};
    auto error = esp_jpeg_get_image_info(&config, &info);
    if (error == ESP_OK && (info.width != 360 || info.height != 360))
        return ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK) error = esp_jpeg_decode(&config, &info);
    return error;
}
}  // namespace
void PlaySugaratBootAnimation(AmoledHardware& hardware) {
    if (sdgoods_power_take_sleep_resume()) {
        ESP_LOGI(kTag, "deep sleep resume; skipping startup movie");
        return;
    }
    if (!hardware.panel_.Ready()) return;
    size_t size = 0;
    auto data = Load(&size);
    sugarat_boot::Movie movie;
    if (!data) return;
    if (!movie.Open(data.get(), size) ||
        esp_rom_crc32_le(0, data.get() + sugarat_boot::kHeaderBytes,
                         size - sugarat_boot::kHeaderBytes) != sugarat_boot::U32(data.get() + 44)) {
        ESP_LOGW(kTag, "invalid startup animation; entering home");
        return;
    }
    auto pixels = Allocate(360 * 360 * 2), work = Allocate(72 * 1024);
    if (!pixels || !work || Decode(movie, 0, pixels.get(), work.get()) != ESP_OK) {
        ESP_LOGW(kTag, "cannot prepare first frame; entering home");
        return;
    }
    auto sound = std::make_shared<Sound>();
    sound->data = data; sound->audio = &hardware.audio_; sound->frames = movie.pcm_bytes / 2;
    int8_t volume = AUDIO_DEFAULT_VOLUME;
    nvs_handle_t handle;
    if (nvs_open_from_partition(agent_system::SettingsPartition(), "voice", NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_i8(handle, "vol", &volume);
        nvs_close(handle);
    }
    hardware.audio_.SetVolume(volume);
    auto* owner = new (std::nothrow) std::shared_ptr<Sound>(sound);
    bool audio = owner && movie.pcm_bytes &&
        xTaskCreate(SoundEntry, "boot_audio", 8192, owner, 4, nullptr) == pdPASS;
    if (!audio) delete owner;
    // Wait for speaker priming so picture and sound share their playback epoch.
    const auto preparing = esp_timer_get_time();
    while (audio && !sound->done.load() && hardware.audio_.State() != sdgoods::Audio::Stage::kPlaying &&
           esp_timer_get_time() - preparing < 500000)
        vTaskDelay(pdMS_TO_TICKS(2));
    (void)hardware.panel_.SetBrightness(DISPLAY_BRIGHTNESS);
    const auto started = esp_timer_get_time();
    unsigned shown = 0, displayed = 0;
    esp_err_t error = ESP_OK;
    do {
        if (displayed) {
            shown = movie.FrameAt(esp_timer_get_time() - started);
            error = Decode(movie, shown, pixels.get(), work.get());
        }
        if (error == ESP_OK) error = hardware.panel_.DrawBitmap(0, 0, 360, 360, pixels.get());
        if (error != ESP_OK) break;
        ++displayed;
        const uint64_t next = (uint64_t(shown + 1) * 1000000 + movie.fps - 1) / movie.fps;
        while (esp_timer_get_time() - started < int64_t(next))
            vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() - started < int64_t(movie.DurationUs()));
    if (audio) {
        const auto ending = esp_timer_get_time();
        while (!sound->done.load() && esp_timer_get_time() - ending < 500000)
            vTaskDelay(pdMS_TO_TICKS(5));
        if (!sound->done.load()) (void)hardware.audio_.StopAndWait();
    }
    ESP_LOGI(kTag, "startup finished frames=%u/%lu ms=%lld audio=%u result=0x%x", displayed,
             static_cast<unsigned long>(movie.frames),
             static_cast<long long>((esp_timer_get_time() - started) / 1000), audio, unsigned(error));
}
