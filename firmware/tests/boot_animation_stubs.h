#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_SIZE=2, ESP_ERR_NO_MEM=3;
constexpr int ESP_PARTITION_TYPE_DATA=1, ESP_PARTITION_SUBTYPE_DATA_FAT=0x81;
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, NVS_READONLY=0;
constexpr int DISPLAY_BRIGHTNESS=190, AUDIO_DEFAULT_VOLUME=35, pdPASS=1;
constexpr int WL_INVALID_HANDLE=-1, JPEG_IMAGE_FORMAT_RGB565=1, JPEG_IMAGE_SCALE_0=0;
using wl_handle_t=int; using nvs_handle_t=int;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
inline const char* esp_err_to_name(int) {return "fixture";}
namespace boot_fixture {
inline std::vector<uint8_t> bytes;
inline std::vector<std::thread> workers;
inline std::atomic<int64_t> clock{0};
inline std::atomic<bool> audio_started{false};
inline int mounts=0, unmounts=0, allocations=0, frames=0;
inline bool mount_fails=false, file_missing=false, task_fails=false, decode_fails=false;
inline uint32_t partition_address=0x1000000;
inline int decode_us=5000;
inline FILE* Open(const char* path,const char* mode) {
    assert(!strcmp(path,"/appdata/START.BVA") && !strcmp(mode,"rb"));
    if(file_missing) return nullptr;
    FILE* out=tmpfile();assert(out);
    assert(fwrite(bytes.data(),1,bytes.size(),out)==bytes.size());rewind(out);return out;
}
inline void Join() {for(auto& t:workers)t.join();workers.clear();}
}
inline int64_t esp_timer_get_time(){return boot_fixture::clock.load();}
inline unsigned pdMS_TO_TICKS(unsigned value){return value;}
inline void vTaskDelay(unsigned value){boot_fixture::clock.fetch_add(value*1000);std::this_thread::yield();}
inline void vTaskDelete(void*){}
inline int xTaskCreate(void(*entry)(void*),const char*,int,void* arg,int,void*) {
    if(boot_fixture::task_fails)return 0;
    boot_fixture::audio_started.store(false);
    boot_fixture::workers.emplace_back([=]{entry(arg);});
    while(!boot_fixture::audio_started.load())std::this_thread::yield();
    return pdPASS;
}
inline void* heap_caps_malloc(size_t count,int){auto* p=malloc(count);if(p)++boot_fixture::allocations;return p;}
inline void heap_caps_free(void* p){if(p)--boot_fixture::allocations;free(p);}
inline uint32_t esp_rom_crc32_le(uint32_t seed,const uint8_t* bytes,size_t size){
    uint32_t crc=~seed;for(size_t i=0;i<size;++i){crc^=bytes[i];for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320:0);}return ~crc;
}
struct esp_partition_t{uint32_t address,size;};
inline const esp_partition_t* esp_partition_find_first(int,int,const char*){
    static esp_partition_t p;p={boot_fixture::partition_address,0x1000000};return &p;
}
struct esp_vfs_fat_mount_config_t{bool format_if_mount_failed;int max_files;size_t allocation_unit_size;};
inline int esp_vfs_fat_spiflash_mount_rw_wl(const char*,const char*,const esp_vfs_fat_mount_config_t* cfg,int* wl){
    assert(!cfg->format_if_mount_failed);++boot_fixture::mounts;*wl=0;return boot_fixture::mount_fails?ESP_FAIL:ESP_OK;
}
inline int esp_vfs_fat_spiflash_unmount_rw_wl(const char*,int){++boot_fixture::unmounts;return ESP_OK;}
inline int nvs_open_from_partition(const char*,const char*,int,int*){return ESP_FAIL;}
inline int nvs_get_i8(int,const char*,int8_t*){return ESP_FAIL;}
inline void nvs_close(int){}
namespace agent_system {inline const char* SettingsPartition(){return "agent_nvs";}}
struct esp_jpeg_image_cfg_t{
    uint8_t* indata;uint32_t indata_size;uint8_t* outbuf;uint32_t outbuf_size;int out_format,out_scale;
    struct{unsigned swap_color_bytes;}flags;
    struct{void* working_buffer;size_t working_buffer_size;}advanced;
};
struct esp_jpeg_image_output_t{uint16_t width,height;size_t output_len;};
inline int esp_jpeg_get_image_info(esp_jpeg_image_cfg_t*,esp_jpeg_image_output_t* out){*out={360,360,360*360*2};return boot_fixture::decode_fails?ESP_FAIL:ESP_OK;}
inline int esp_jpeg_decode(esp_jpeg_image_cfg_t* cfg,esp_jpeg_image_output_t*){
    assert(cfg->outbuf_size==360*360*2 && cfg->flags.swap_color_bytes);
    memset(cfg->outbuf,0,cfg->outbuf_size);boot_fixture::clock.fetch_add(boot_fixture::decode_us);return ESP_OK;
}
namespace sdgoods {
class Audio {
 public:
    enum class Stage{kIdle,kPlaying,kDone};
    void SetVolume(int value){volume=value;}
    Stage State()const{return stage.load();}
    esp_err_t PlayClip(const int16_t* data,size_t count){
        assert(data && count);samples=count;busy=true;stage=Stage::kPlaying;
        const auto end=esp_timer_get_time()+count*1000000/16000;
        boot_fixture::audio_started=true;
        while(!stop && esp_timer_get_time()<end)std::this_thread::yield();
        stage=Stage::kDone;busy=false;return ESP_OK;
    }
    bool StopAndWait(){stop=true;while(busy)std::this_thread::yield();return true;}
    int volume=0;size_t samples=0;std::atomic<Stage>stage{Stage::kIdle};
    std::atomic<bool>busy{false},stop{false};
};
}
class AmoledHardware{
 public:
    struct Panel{
        bool Ready(){return true;}
        int SetBrightness(int value){brightness=value;return ESP_OK;}
        int DrawBitmap(int x,int y,int w,int h,const void* pixels){
            assert(x==0 && y==0 && w==360 && h==360 && pixels);++boot_fixture::frames;
            boot_fixture::clock.fetch_add(2000);return ESP_OK;
        }
        int brightness=0;
    }panel_;
    sdgoods::Audio audio_;
};
