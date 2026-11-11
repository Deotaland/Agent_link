#include "../common/factory_hardware_services.h"
#include "amoled_hardware.h"
#include "amoled_ui_language.h"
#include "agent_system.h"
#include "agent_link_network.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include <algorithm>
#include <cstdio>

namespace {
using amoled_ui::Tr;
class ButtonFactoryServices final : public FactoryHardwareServices {
   public:
    explicit ButtonFactoryServices(AmoledHardware& hardware) : hardware_(hardware) {
        nvs_handle_t handle;
        if (nvs_open_from_partition(agent_system::SettingsPartition(), "button_hw", NVS_READONLY,
                                    &handle) == ESP_OK) {
            uint8_t value = 75;
            if (nvs_get_u8(handle, "bright", &value) == ESP_OK)
                brightness_ = std::clamp<int>(value, 5, 100);
            nvs_close(handle);
        }
        if (nvs_open_from_partition(agent_system::SettingsPartition(), "voice", NVS_READONLY,
                                    &handle) == ESP_OK) {
            int8_t value = AUDIO_DEFAULT_VOLUME;
            if (nvs_get_i8(handle, "vol", &value) == ESP_OK)
                SetVolume(value);
            nvs_close(handle);
        }
        amoled_ui::SetBacklight(brightness_);
    }
    AmoledSystemMenu::ExtraPage Menu() override {
        return {this,
                [](const amoled_ui::Input& input, void* context) {
                    return static_cast<ButtonFactoryServices*>(context)->Handle(input);
                },
                [](amoled_ui::Screen& screen, void* context) {
                    static_cast<ButtonFactoryServices*>(context)->Render(screen);
                },
                [](void* context) { static_cast<ButtonFactoryServices*>(context)->Close(); }};
    }
    bool SetVolume(int value) override {
        hardware_.audio_.SetVolume(value);
        return true;
    }
    bool Button(bool down, int64_t now) override {
        if (!down) {
            button_armed_ = true;
            button_at_ = 0;
            long_handled_ = false;
            return false;
        }
        if (!button_armed_)
            return false;
        if (!button_at_)
            button_at_ = now;
        if (!long_handled_ && now - button_at_ >= 1200000) {
            long_handled_ = true;
            page_ = Page::kPower;
            return true;
        }
        return false;
    }
    bool Tick(bool can_power_down) override {
        if (pending_ != Action::kNone) {
            const Action action = pending_;
            pending_ = Action::kNone;
            if (!can_power_down || !hardware_.audio_.StopAndWait() || hardware_.ButtonDown()) {
                amoled_ui::Toast(Tr("请等待当前任务完成并松开按键",
                                    "Wait for work to finish and release the key"));
                return true;
            }
            const auto* factory = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
            if (!factory || esp_ota_set_boot_partition(factory) != ESP_OK ||
                !amoled_ui::PrepareForSleep()) {
                amoled_ui::CancelSleep();
                amoled_ui::Toast(
                    Tr("电源操作准备失败，请重试", "Power action could not be prepared"));
                return true;
            }
            if (action == Action::kRestart)
                esp_restart();
            (void)esp_wifi_stop();
            // The UI has drained its transfer queue. A successful deep sleep or
            // latch release does not return; reinitialize cleanly on any failure.
            (void)hardware_.EnterSleep(action == Action::kOff);
            esp_restart();
        }
        const int64_t now = esp_timer_get_time();
        if (now < next_sample_)
            return false;
        hardware_.sensors_.Poll(page_ == Page::kMotion);
        next_sample_ = now + (page_ == Page::kMotion ? 100000 : 1000000);
        return true;
    }
    void DeviceInfo(char* out, size_t size) override {
        const auto& state = hardware_.sensors_.Read();
        char battery[64];
        if (state.percent >= 0)
            snprintf(battery, sizeof battery, Tr("电量约 %d%% · %.2f V", "Battery ~%d%% · %.2f V"),
                     state.percent, state.battery_mv / 1000.0);
        else
            snprintf(battery, sizeof battery, "%s",
                     Tr("电量与电压暂不可用", "Battery reading unavailable"));
        snprintf(out, size,
                 Tr("%s\n充电状态：未知\n内部可用：%u KB\nPSRAM 可用：%u KB\n应用编号：最多 %d "
                    "个\nQMI8658：%s",
                    "%s\nCharging: unknown\nFree internal: %u KB\nFree PSRAM: %u KB\nApp indices: "
                    "up to %d\nQMI8658: %s"),
                 battery,
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024),
                 agent_system::kAppSlotCount,
                 hardware_.sensors_.ImuReady() ? Tr("已连接", "Connected")
                                               : Tr("不可用", "Unavailable"));
    }
    void Decorate(amoled_ui::Screen& screen) override {
        agent_link_wifi_info_t wifi{};
        (void)agent_link_wifi_info(&wifi);
        const auto& state = hardware_.sensors_.Read();
        if (state.percent >= 0)
            snprintf(screen.hardware_status, sizeof screen.hardware_status, "%d%%  ·  %s",
                     state.percent, wifi.connected ? "WiFi" : Tr("离线", "Offline"));
        else
            snprintf(screen.hardware_status, sizeof screen.hardware_status, "--%%  ·  %s",
                     wifi.connected ? "WiFi" : Tr("离线", "Offline"));
    }

   private:
    enum class Page { kRoot, kBattery, kMotion, kAudio, kBrightness, kPower, kConfirm, kDisplay };
    enum class Action { kNone, kSleep, kOff, kRestart };
    void Close() {
        hardware_.audio_.Stop();
        hardware_.sensors_.Suspend();
        page_ = Page::kRoot;
        next_sample_ = 0;
    }
    void Error(esp_err_t error) {
        if (error == ESP_OK)
            return;
        char message[128];
        snprintf(message, sizeof message, Tr("操作失败：%s", "Failed: %s"), esp_err_to_name(error));
        amoled_ui::Toast(message, 5000);
    }
    bool Handle(const amoled_ui::Input& input) {
        using amoled_ui::TouchAction;
        if (input.action == TouchAction::kBack || input.action == TouchAction::kHome) {
            if (page_ == Page::kRoot)
                return false;
            Close();
            return true;
        }
        if (input.action != TouchAction::kRow)
            return true;
        if (page_ == Page::kRoot) {
            switch (input.row) {
                case 0:
                    page_ = Page::kBattery;
                    break;
                case 1:
                    page_ = Page::kMotion;
                    next_sample_ = 0;
                    break;
                case 2:
                case 3:
                    Error(hardware_.audio_.Start(input.row == 2
                                                     ? sdgoods::Audio::Test::kSpeaker
                                                     : sdgoods::Audio::Test::kRecordPlayback));
                    page_ = Page::kAudio;
                    break;
                case 4:
                    page_ = Page::kBrightness;
                    break;
                case 5:
                    page_ = Page::kPower;
                    break;
                case 6:
                    page_ = Page::kDisplay;
                    break;
            }
        } else if (page_ == Page::kAudio && input.row == 0)
            hardware_.audio_.Stop();
        else if (page_ == Page::kBrightness) {
            switch (input.row) {
                case 0:
                    brightness_ -= 10;
                    break;
                case 1:
                    brightness_ += 10;
                    break;
                case 2:
                    brightness_ = 25;
                    break;
                case 3:
                    brightness_ = 75;
                    break;
                case 4:
                    brightness_ = 100;
                    break;
                default:
                    return true;
            }
            brightness_ = std::clamp(brightness_, 5, 100);
            amoled_ui::SetBacklight(brightness_);
            nvs_handle_t handle;
            esp_err_t error = nvs_open_from_partition(agent_system::SettingsPartition(),
                                                      "button_hw", NVS_READWRITE, &handle);
            if (error == ESP_OK) {
                error = nvs_set_u8(handle, "bright", brightness_);
                if (error == ESP_OK)
                    error = nvs_commit(handle);
                nvs_close(handle);
            }
            Error(error);
        } else if (page_ == Page::kPower && input.row >= 0 && input.row < 3) {
            selected_ = input.row == 0   ? Action::kSleep
                        : input.row == 1 ? Action::kOff
                                         : Action::kRestart;
            page_ = Page::kConfirm;
        } else if (page_ == Page::kConfirm) {
            if (input.row == 0)
                pending_ = selected_;
            if (input.row == 0 || input.row == 1)
                page_ = Page::kPower;
        }
        return true;
    }
    void Render(amoled_ui::Screen& screen) {
        const auto row = [&](const char* text) {
            snprintf(screen.rows[screen.row_count++], sizeof screen.rows[0], "%s", text);
        };
        const auto title = [&](const char* text) {
            snprintf(screen.title, sizeof screen.title, "%s", text);
        };
        const auto& state = hardware_.sensors_.Read();
        if (page_ == Page::kRoot) {
            title(Tr("硬件与电源", "Hardware and power"));
            row(Tr("电量与电压", "Battery and voltage"));
            row(Tr("六轴传感器", "Motion sensors"));
            row(Tr("扬声器测试", "Test speaker"));
            row(Tr("录音 3 秒并回放", "Record 3 seconds and play"));
            row(Tr("屏幕亮度", "Screen brightness"));
            row(Tr("休眠、关机与重启", "Sleep, power off, restart"));
            row(Tr("显示与触摸状态", "Display and touch status"));
        } else if (page_ == Page::kBattery) {
            title(Tr("电量与电压", "Battery and voltage"));
            if (state.percent >= 0)
                snprintf(screen.info, sizeof screen.info,
                         Tr("估算电量：%d%%\n电池电压：%.3f "
                            "V\nADC：%s\n充电状态：未知\n百分比采用官方电压估算。",
                            "Estimated level: %d%%\nVoltage: %.3f V\nADC: %s\nCharging: "
                            "unknown\nVendor voltage estimate."),
                         state.percent, state.battery_mv / 1000.0,
                         state.calibrated ? Tr("已校准", "calibrated")
                                          : Tr("未校准估算", "uncalibrated estimate"));
            else
                snprintf(screen.info, sizeof screen.info, "%s",
                         Tr("电池采样暂不可用。\n充电状态：未知",
                            "Battery reading unavailable.\nCharging: unknown"));
        } else if (page_ == Page::kMotion) {
            title(Tr("六轴传感器", "Motion sensors"));
            if (state.imu_valid)
                snprintf(screen.info, sizeof screen.info,
                         "QMI8658\nAccel (g)\nX %+.2f  Y %+.2f  Z %+.2f\nGyro (deg/s)\nX %+.1f  Y "
                         "%+.1f  Z %+.1f",
                         state.acceleration[0], state.acceleration[1], state.acceleration[2],
                         state.angular_velocity[0], state.angular_velocity[1],
                         state.angular_velocity[2]);
            else
                snprintf(screen.info, sizeof screen.info, "%s",
                         Tr("传感器未就绪或读取失败", "Sensor not ready or read failed"));
        } else if (page_ == Page::kAudio) {
            title(Tr("音频自检", "Audio self-test"));
            const char* text = Tr("测试就绪", "Ready");
            switch (hardware_.audio_.State()) {
                case sdgoods::Audio::Stage::kPreparing:
                    text = Tr("正在准备音频", "Preparing audio");
                    break;
                case sdgoods::Audio::Stage::kRecording:
                    text = Tr("正在录音，请说话…", "Recording. Please speak...");
                    break;
                case sdgoods::Audio::Stage::kPlaying:
                    text = Tr("正在播放", "Playing");
                    break;
                case sdgoods::Audio::Stage::kDone:
                    text = Tr("测试完成", "Test complete");
                    break;
                case sdgoods::Audio::Stage::kStopped:
                    text = Tr("已停止", "Stopped");
                    break;
                case sdgoods::Audio::Stage::kFailed:
                    text = Tr("测试失败", "Test failed");
                    break;
                default:
                    break;
            }
            snprintf(screen.info, sizeof screen.info, "%s\n%s", text,
                     hardware_.audio_.State() == sdgoods::Audio::Stage::kFailed
                         ? esp_err_to_name(hardware_.audio_.Error())
                         : Tr("录音仅在设备内存临时保存。", "Recording stays in device RAM."));
            row(Tr("停止音频测试", "Stop audio test"));
        } else if (page_ == Page::kBrightness) {
            title(Tr("屏幕亮度", "Screen brightness"));
            snprintf(screen.info, sizeof screen.info, Tr("当前亮度：%d%%", "Brightness: %d%%"),
                     brightness_);
            row(Tr("降低 10%", "Decrease 10%"));
            row(Tr("提高 10%", "Increase 10%"));
            row("25%");
            row("75%");
            row("100%");
        } else if (page_ == Page::kPower) {
            title(Tr("电源", "Power"));
            snprintf(screen.info, sizeof screen.info, "%s",
                     Tr("休眠后按电源键唤醒。\n长按电源键可打开此页。",
                        "Press the power key to wake.\nHold it to open this page."));
            row(Tr("深度休眠", "Deep sleep"));
            row(Tr("关机", "Power off"));
            row(Tr("重启启动器", "Restart launcher"));
        } else if (page_ == Page::kConfirm) {
            title(Tr("确认电源操作", "Confirm power action"));
            snprintf(screen.info, sizeof screen.info, "%s",
                     selected_ == Action::kSleep ? Tr("进入深度休眠？", "Enter deep sleep?")
                     : selected_ == Action::kOff
                         ? Tr("关闭设备？\nUSB 供电时进入休眠。",
                              "Power off?\nWith USB power, enter sleep.")
                         : Tr("重新启动 Agent_link Launcher？", "Restart Agent_link Launcher?"));
            row(Tr("确认", "Confirm"));
            row(Tr("取消", "Cancel"));
        } else if (page_ == Page::kDisplay) {
            title(Tr("显示与触摸", "Display and touch"));
            snprintf(screen.info, sizeof screen.info,
                     "ST77916 360 x 360\nLCD: %s\nCST816: %s\nTE GPIO18: %u edges\nTE timeouts: %u",
                     hardware_.panel_.Ready() ? "OK" : "unavailable",
                     hardware_.touch_.Ready() ? "OK" : "unavailable", hardware_.panel_.TeEdges(),
                     hardware_.panel_.TeTimeouts());
        }
    }
    AmoledHardware& hardware_;
    Page page_ = Page::kRoot;
    Action selected_ = Action::kNone, pending_ = Action::kNone;
    int brightness_ = 75;
    int64_t next_sample_ = 0, button_at_ = 0;
    bool button_armed_ = false, long_handled_ = false;
};
}  // namespace
std::unique_ptr<FactoryHardwareServices> CreateFactoryHardwareServices(AmoledHardware& hardware) {
    return std::make_unique<ButtonFactoryServices>(hardware);
}
