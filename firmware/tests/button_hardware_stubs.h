#pragma once
// Host adapters for real board drivers and Factory page logic; no device I/O.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../components/amoled_ui/include/amoled_hardware_test.h"
using esp_err_t = int;
constexpr int ESP_ERR_INVALID_SIZE = 0x104;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_NO_MEM = 0x101,
              ESP_ERR_INVALID_ARG = 0x102,
              ESP_ERR_TIMEOUT = 0x107;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
inline const char* esp_err_to_name(int) {
    return "injected-error";
}
using gpio_num_t = int;
#define GPIO_NUM_NC -1
constexpr int GPIO_NUM_2 = 2, GPIO_NUM_3 = 3, GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_6 = 6,
              GPIO_NUM_7 = 7, GPIO_NUM_8 = 8, GPIO_NUM_9 = 9, GPIO_NUM_10 = 10, GPIO_NUM_11 = 11,
              GPIO_NUM_12 = 12, GPIO_NUM_13 = 13, GPIO_NUM_15 = 15, GPIO_NUM_16 = 16,
              GPIO_NUM_17 = 17, GPIO_NUM_18 = 18, GPIO_NUM_21 = 21, GPIO_NUM_38 = 38,
              GPIO_NUM_40 = 40, GPIO_NUM_41 = 41, GPIO_NUM_42 = 42, GPIO_NUM_45 = 45,
              GPIO_NUM_46 = 46, GPIO_NUM_47 = 47, GPIO_NUM_48 = 48;
struct TaskExit {};
struct SleepSignal {
    bool off;
};
struct RestartSignal {};
struct TestTask {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    unsigned notifications = 0;
};
namespace button_fixture {
inline bool adc_fail = false, adc_read_fail = false, calibration_fail = false, imu_fail = false,
            imu_read_fail = false;
inline int adc_creations = 0, adc_samples = 0, adc_mv = 1300;
inline uint8_t identity = 5, registers[256]{};
inline int16_t imu_axes[]{8192, -8192, 16384, 512, -1024, 32767};
inline std::atomic<bool> amplifier{false}, amplifier_used{false};
inline std::atomic<int> channels{0}, peak{0}, pcm_allocations{0};
inline int audio_read_error = 0, audio_tx_error = 0;
inline size_t audio_write_limit = 256;
inline std::atomic<bool> quit{false};
inline std::vector<TestTask*> tasks;
inline thread_local TestTask* current = nullptr;
inline std::map<std::string, int> nvs;
inline std::map<int, std::string> handles;
inline int next_handle = 1, brightness = 0;
inline int64_t now = 1000000;
inline bool ui_ready = true, button = false;
inline std::string toast;
inline std::vector<std::string> power_events;
inline void Finish() {
    quit.store(true);
    for (auto* task : tasks) {
        std::lock_guard<std::mutex> guard(task->mutex);
        task->cv.notify_all();
    }
    for (auto* task : tasks) {
        task->thread.join();
        delete task;
    }
    tasks.clear();
}
}  // namespace button_fixture
inline int gpio_set_level(int pin, int level) {
    assert(pin == 3);
    button_fixture::amplifier.store(level != 0);
    if (level)
        button_fixture::amplifier_used.store(true);
    return ESP_OK;
}
using TaskHandle_t = TestTask*;
constexpr int pdPASS = 1, pdTRUE = 1;
constexpr uint32_t portMAX_DELAY = 0xFFFFFFFF;
inline uint32_t pdMS_TO_TICKS(uint32_t value) {
    return value;
}
inline void vTaskDelay(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
inline int xTaskCreate(void (*entry)(void*), const char*, unsigned, void* context, unsigned,
                       TaskHandle_t* handle) {
    auto* task = new TestTask;
    *handle = task;
    button_fixture::tasks.push_back(task);
    task->thread = std::thread([=] {
        button_fixture::current = task;
        try {
            entry(context);
        } catch (const TaskExit&) {
        }
    });
    return pdPASS;
}
inline void xTaskNotifyGive(TestTask* task) {
    std::lock_guard<std::mutex> lock(task->mutex);
    ++task->notifications;
    task->cv.notify_one();
}
inline unsigned ulTaskNotifyTake(int, uint32_t) {
    auto* task = button_fixture::current;
    assert(task);
    std::unique_lock<std::mutex> lock(task->mutex);
    task->cv.wait(lock, [&] { return task->notifications || button_fixture::quit.load(); });
    if (button_fixture::quit.load())
        throw TaskExit{};
    const auto value = task->notifications;
    task->notifications = 0;
    return value;
}
using adc_oneshot_unit_handle_t = void*;
using adc_cali_handle_t = void*;
constexpr int ADC_UNIT_1 = 1, ADC_CHANNEL_7 = 7, ADC_ATTEN_DB_12 = 12, ADC_BITWIDTH_DEFAULT = 12;
struct adc_oneshot_unit_init_cfg_t {
    int unit_id;
};
struct adc_oneshot_chan_cfg_t {
    int atten, bitwidth;
};
struct adc_cali_curve_fitting_config_t {
    int unit_id, chan, atten, bitwidth;
};
inline int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t* c, void** handle) {
    assert(c->unit_id == 1);
    ++button_fixture::adc_creations;
    *handle = button_fixture::adc_fail ? nullptr : (void*)1;
    return *handle ? ESP_OK : ESP_FAIL;
}
inline int adc_oneshot_config_channel(void*, int channel, const adc_oneshot_chan_cfg_t*) {
    assert(channel == 7);
    return ESP_OK;
}
inline int adc_oneshot_del_unit(void*) {
    return ESP_OK;
}
inline int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*,
                                                void** handle) {
    *handle = button_fixture::calibration_fail ? nullptr : (void*)2;
    return *handle ? ESP_OK : ESP_FAIL;
}
inline int adc_oneshot_read(void*, int, int* raw) {
    // One gross outlier in each nine-sample window must not alter the median.
    *raw = (++button_fixture::adc_samples % 9 == 1) ? 4000 : button_fixture::adc_mv;
    return button_fixture::adc_read_fail ? ESP_FAIL : ESP_OK;
}
inline int adc_cali_raw_to_voltage(void*, int raw, int* mv) {
    *mv = raw;
    return ESP_OK;
}
using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;
constexpr int I2C_ADDR_BIT_LEN_7 = 7;
struct i2c_device_config_t {
    int dev_addr_length, device_address, scl_speed_hz;
};
inline int i2c_master_probe(void*, int address, int) {
    assert(address == 0x6B);
    return button_fixture::imu_fail ? ESP_FAIL : ESP_OK;
}
inline int i2c_master_bus_add_device(void*, const i2c_device_config_t*, void** device) {
    *device = (void*)3;
    return ESP_OK;
}
inline int i2c_master_bus_rm_device(void*) {
    return ESP_OK;
}
inline int i2c_master_transmit(void*, const uint8_t* bytes, size_t size, int) {
    assert(size == 2);
    button_fixture::registers[bytes[0]] = bytes[1];
    return ESP_OK;
}
inline int i2c_master_transmit_receive(void*, const uint8_t* reg, size_t, void* out, size_t size,
                                       int) {
    if (button_fixture::imu_read_fail)
        return ESP_FAIL;
    if (*reg == 0 && size == 1) {
        *(uint8_t*)out = button_fixture::identity;
        return ESP_OK;
    }
    assert(*reg == 0x35 && size == 12);
    memcpy(out, button_fixture::imu_axes, 12);
    return ESP_OK;
}
constexpr int I2S_NUM_0 = 0, I2S_NUM_1 = 1, I2S_ROLE_MASTER = 0, I2S_DATA_BIT_WIDTH_32BIT = 32,
              I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2,
              I2S_STD_SLOT_LEFT = 1, I2S_MCLK_MULTIPLE_384 = 384;
struct Channel {
    int port;
    bool tx, enabled = false;
};
using i2s_chan_handle_t = Channel*;
struct i2s_chan_config_t {
    int port;
    bool auto_clear;
    int dma_desc_num, dma_frame_num;
};
struct i2s_std_clk_config_t {
    int rate, mclk_multiple;
};
struct i2s_std_slot_config_t {
    int bits, mode, slot_mask;
};
struct i2s_std_config_t {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    struct {
        int mclk, bclk, ws, dout, din;
    } gpio_cfg;
};
#define I2S_CHANNEL_DEFAULT_CONFIG(port, role) i2s_chan_config_t{port, false, 0, 0}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) i2s_std_clk_config_t{int(rate), 256}
#define I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(bits, mode) i2s_std_slot_config_t{bits, mode, 0}
inline int i2s_new_channel(const i2s_chan_config_t* config, Channel** tx, Channel** rx) {
    auto* channel = new Channel{config->port, tx != nullptr};
    ++button_fixture::channels;
    if (tx)
        *tx = channel;
    else
        *rx = channel;
    return ESP_OK;
}
inline int i2s_channel_init_std_mode(Channel* c, const i2s_std_config_t* config) {
    assert(config->clk_cfg.rate == 16000);
    if (c->tx) {
        assert(c->port == 0 && config->gpio_cfg.bclk == 48 && config->gpio_cfg.ws == 38 &&
               config->gpio_cfg.dout == 47 && config->gpio_cfg.mclk == 15 &&
               config->clk_cfg.mclk_multiple == 384);
        return button_fixture::audio_tx_error;
    }
    assert(c->port == 1 && config->gpio_cfg.bclk == 16 && config->gpio_cfg.ws == 2 &&
           config->gpio_cfg.din == 17 && config->slot_cfg.bits == 32);
    return ESP_OK;
}
inline int i2s_channel_enable(Channel* c) {
    c->enabled = true;
    return ESP_OK;
}
inline int i2s_channel_disable(Channel* c) {
    c->enabled = false;
    return ESP_OK;
}
inline int i2s_del_channel(Channel* c) {
    --button_fixture::channels;
    delete c;
    return ESP_OK;
}
inline int i2s_channel_read(Channel* c, void* out, size_t size, size_t* count, unsigned timeout) {
    assert(c->enabled && !c->tx && timeout == 100);
    vTaskDelay(1);
    if (button_fixture::audio_read_error)
        return button_fixture::audio_read_error;
    *count = size;
    auto* samples = (int32_t*)out;
    for (size_t i = 0; i < size / 4; ++i)
        samples[i] = (i % 2 ? 1000 : -1000) * 16384;
    return ESP_OK;
}
inline int i2s_channel_write(Channel* c, const void* data, size_t size, size_t* written,
                             unsigned timeout) {
    assert(c->enabled && c->tx && timeout == 100);
    vTaskDelay(1);
    *written = std::min(size, button_fixture::audio_write_limit);
    auto* samples = (const int16_t*)data;
    for (size_t i = 0; i < *written / 2; ++i)
        button_fixture::peak.store(
            std::max(button_fixture::peak.load(), std::abs(int(samples[i]))));
    return ESP_OK;
}
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
inline void* heap_caps_malloc(size_t bytes, int caps) {
    assert(bytes == 96000 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++button_fixture::pcm_allocations;
    return malloc(bytes);
}
inline void heap_caps_free(void* p) {
    if (p)
        --button_fixture::pcm_allocations;
    free(p);
}
inline size_t heap_caps_get_free_size(int) {
    return 500000;
}
inline int64_t esp_timer_get_time() {
    return button_fixture::now;
}
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
inline int nvs_open_from_partition(const char* partition, const char* ns, int, int* h) {
    *h = button_fixture::next_handle++;
    button_fixture::handles[*h] = std::string(partition) + "/" + ns + "/";
    return ESP_OK;
}
inline int nvs_get_u8(int h, const char* key, uint8_t* out) {
    auto i = button_fixture::nvs.find(button_fixture::handles[h] + key);
    if (i == button_fixture::nvs.end())
        return ESP_FAIL;
    *out = (uint8_t)i->second;
    return ESP_OK;
}
inline int nvs_get_i8(int h, const char* key, int8_t* out) {
    uint8_t value = 0;
    const int error = nvs_get_u8(h, key, &value);
    *out = (int8_t)value;
    return error;
}
inline int nvs_set_u8(int h, const char* key, uint8_t value) {
    button_fixture::nvs[button_fixture::handles[h] + key] = value;
    return ESP_OK;
}
inline int nvs_commit(int) {
    return ESP_OK;
}
inline void nvs_close(int) {}
constexpr int ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_SUBTYPE_APP_FACTORY = 0;
struct esp_partition_t {
    int subtype;
};
inline const esp_partition_t* esp_partition_find_first(int, int, const char*) {
    static esp_partition_t p{};
    return &p;
}
inline int esp_ota_set_boot_partition(const esp_partition_t*) {
    button_fixture::power_events.push_back("factory");
    return ESP_OK;
}
[[noreturn]] inline void esp_restart() {
    throw RestartSignal{};
}
inline int esp_wifi_stop() {
    button_fixture::power_events.push_back("wifi-stop");
    return ESP_OK;
}
struct agent_link_wifi_info_t {
    bool connected = true;
    bool setup_active = false;
    char setup_ssid[33]{};
};
inline agent_link_wifi_info_t test_wifi{};
inline int agent_link_wifi_info(agent_link_wifi_info_t* out) {
    *out = test_wifi;
    return ESP_OK;
}
namespace agent_system {
constexpr int kAppSlotCount = 16;
inline const char* SettingsPartition() {
    return "agent_nvs";
}
}  // namespace agent_system
namespace amoled_ui {
enum class TouchAction { kBack, kHome, kRow, kTouchPoint, kBrightness };
enum class View { kList, kHardwareTest, kBrightnessControl };
struct Input {
    TouchAction action;
    int row;
    int x = -1, y = -1;
    bool commit = false;
};
struct Screen {
    char rows[12][112]{}, title[96]{}, info[384]{}, hardware_status[64]{};
    int row_count = 0;
    int brightness_percent = 75;
    View kind = View::kList;
    bool local_mode = false;
    HardwareTest hardware_test;
    char detail[192]{};
    int battery_percent = -1;
    bool wifi_connected = false, wifi_setup_active = false;
    char wifi_setup_ssid[33]{};
};
inline const char* Tr(const char*, const char* english) {
    return english;
}
inline void Toast(const char* text, unsigned = 2500) {
    button_fixture::toast = text;
}
inline void SetBacklight(int value) {
    button_fixture::brightness = value;
}
inline bool PrepareForSleep() {
    button_fixture::power_events.push_back("display-drained");
    return button_fixture::ui_ready;
}
inline void CancelSleep() {
    button_fixture::power_events.push_back("display-resumed");
}
}  // namespace amoled_ui
class AmoledSystemMenu {
   public:
    struct ExtraPage {
        void* context;
        bool (*handle)(const amoled_ui::Input&, void*);
        void (*render)(amoled_ui::Screen&, void*);
        void (*close)(void*);
        void (*open_brightness)(void*);
    };
};
