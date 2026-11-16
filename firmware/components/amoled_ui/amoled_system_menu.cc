#include "amoled_system_menu.h"
#ifdef AGENT_SYSTEM_ENABLED
#include "agent_system.h"
#include "agent_link_network.h"
#include "amoled_ui_language.h"
#include "amoled_ui_guides.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

using amoled_ui::Tr;
void AmoledSystemMenu::SetDeviceInfo(const char* text) {
    snprintf(device_info_, sizeof device_info_, "%s", text);
}
void AmoledSystemMenu::SetCoverSummary(const char* text) {
    snprintf(cover_summary_, sizeof cover_summary_, "%s", text);
}
namespace {
void Error(esp_err_t error) {
    if (error == ESP_OK)
        return;
    char text[96];
    snprintf(text, sizeof text, Tr("操作失败：%s", "Failed: %s"), esp_err_to_name(error));
    amoled_ui::Toast(text, 5000);
}
}  // namespace
bool AmoledSystemMenu::Sugarat() const {
    return !strcmp(agent_system::Current().board, "SDGOODS-ESP32S3");
}
bool AmoledSystemMenu::TakeUpdateRequest() {
    nvs_handle_t handle;
    int8_t requested = 0;
    if (nvs_open_from_partition(agent_system::SettingsPartition(), "sugarat_ui", NVS_READWRITE,
                                &handle) == ESP_OK) {
        (void)nvs_get_i8(handle, "open_update", &requested);
        if (requested && nvs_set_i8(handle, "open_update", 0) == ESP_OK)
            (void)nvs_commit(handle);
        nvs_close(handle);
    }
    return requested != 0;
}
void AmoledSystemMenu::OpenUpdate() {
    page_ = Page::kUpdate;
    if (update_.handle) {
        amoled_ui::Input input{};
        input.action = amoled_ui::TouchAction::kMenu;
        update_.handle(input, update_.context);
    }
}
bool AmoledSystemMenu::HandleSugarat(const amoled_ui::Input& input) {
    using amoled_ui::TouchAction;
    if (input.action != TouchAction::kRow)
        return false;
    const int row = input.row;
    if (page_ == Page::kRoot) {
        quick_shortcut_ = row == 0 || row == 1;
        if (row == 0)
            page_ = Page::kNetwork;
        else if (row == 1)
            page_ = Page::kVolume;
        else if (row == 3)
            page_ = Page::kSettings;
        else if (row == 2) {
            if (agent_system::IsLauncher())
                amoled_ui::Toast(Tr("当前已在启动器", "Already in Launcher"));
            else
                page_ = Page::kExitConfirm;
        }
    } else if (page_ == Page::kSettings) {
        if (extra_.render && row == 2) {
            page_ = Page::kExtra;
            return true;
        }
        // Optional device tools sit directly above About; other actions retain
        // their semantic positions when no board-specific tools are installed.
        const int action = extra_.render && row > 2 ? row - 1 : row;
        switch (action) {
            case 0:
                if (extra_.open_brightness) {
                    extra_.open_brightness(extra_.context);
                    page_ = Page::kExtra;
                } else {
                    nvs_handle_t handle;
                    uint8_t value = 75;
                    if (nvs_open_from_partition(agent_system::SettingsPartition(), "button_hw",
                                                NVS_READONLY, &handle) == ESP_OK) {
                        (void)nvs_get_u8(handle, "bright", &value);
                        nvs_close(handle);
                    }
                    brightness_ = std::clamp<int>(value, 5, 100);
                    amoled_ui::SetBacklight(brightness_);
                    page_ = Page::kBrightness;
                }
                break;
            case 1:
                OpenUpdate();
                break;
            case 2:
                page_ = Page::kApp;
                break;
            case 3:
                page_ = Page::kDevice;
                break;
            case 4:
                page_ = Page::kLanguage;
                break;
            case 5:
                page_ = Page::kGuide;
                break;
            case 6:
                if (!agent_system::IsLauncher())
                    page_ = Page::kExitConfirm;
                break;
        }
    } else if (page_ == Page::kGuide) {
        if (row >= 0 && row < amoled_ui::kUserGuideTopicCount) {
            guide_topic_ = row;
            page_ = Page::kGuideTopic;
        }
    } else if (page_ == Page::kExitConfirm || page_ == Page::kUpdateConfirm) {
        if (row == 0)
            page_ = page_ == Page::kExitConfirm ? Page::kRoot : Page::kUpdate;
        else if (row == 1) {
            if (page_ == Page::kUpdateConfirm) {
                nvs_handle_t handle;
                esp_err_t err = nvs_open_from_partition(agent_system::SettingsPartition(),
                                                        "sugarat_ui", NVS_READWRITE, &handle);
                if (err == ESP_OK) {
                    err = nvs_set_i8(handle, "open_update", 1);
                    if (err == ESP_OK)
                        err = nvs_commit(handle);
                    nvs_close(handle);
                }
                if (err != ESP_OK) {
                    Error(err);
                    return true;
                }
            }
            exit_ = true;
        }
    } else if (page_ == Page::kUpdate && !update_.render) {
        if (row == 0)
            page_ = Page::kUpdateConfirm;
    } else
        return false;
    return true;
}
void AmoledSystemMenu::Open() {
    if (extra_.close)
        extra_.close(extra_.context);
    page_ = Page::kRoot;
    exit_ = false;
    quick_shortcut_ = false;
    nvs_handle_t h;
    int8_t value = default_volume_;
    if (nvs_open_from_partition(agent_system::SettingsPartition(), "voice", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i8(h, "vol", &value);
        nvs_close(h);
    }
    volume_ = value < 0 ? 0 : value > 100 ? 100 : value;
    if (volume_handler_)
        (void)volume_handler_(volume_, context_);
    amoled_ui::SetVolume(volume_);
}
bool AmoledSystemMenu::Handle(const amoled_ui::Input& input) {
    if (!Active())
        return false;
    using amoled_ui::TouchAction;
    if (input.action == TouchAction::kSystem) {
        if (extra_.close)
            extra_.close(extra_.context);
        page_ = Page::kRoot;
        quick_shortcut_ = false;
        return true;
    }
    if (input.action == TouchAction::kHome) {
        Close();
        return true;
    }
    if (input.action == TouchAction::kBack) {
        if (page_ == Page::kExtra && extra_.handle && extra_.handle(input, extra_.context))
            return true;
        if (extra_.close)
            extra_.close(extra_.context);
        if (Sugarat()) {
            if ((page_ == Page::kNetwork || page_ == Page::kVolume) && quick_shortcut_) {
                page_ = Page::kRoot;
                quick_shortcut_ = false;
            } else if (page_ == Page::kRoot)
                page_ = Page::kClosed;
            else if (page_ == Page::kSettings || page_ == Page::kExitConfirm)
                page_ = Page::kRoot;
            else if (page_ == Page::kUpdateConfirm)
                page_ = Page::kUpdate;
            else if (page_ == Page::kGuideTopic)
                page_ = Page::kGuide;
            else
                page_ = Page::kSettings;
        } else
            page_ = page_ == Page::kRoot ? Page::kClosed : Page::kRoot;
        return true;
    }
    if (page_ == Page::kExtra && extra_.handle) {
        extra_.handle(input, extra_.context);
        return true;
    }
    if (page_ == Page::kUpdate && update_.handle) {
        update_.handle(input, update_.context);
        return true;
    }
    if (input.action == TouchAction::kBrightness && page_ == Page::kBrightness) {
        brightness_ = std::clamp(input.row, 5, 100);
        amoled_ui::SetBacklight(brightness_);
        if (input.commit) {
            nvs_handle_t handle;
            auto err = nvs_open_from_partition(agent_system::SettingsPartition(), "button_hw",
                                               NVS_READWRITE, &handle);
            if (err == ESP_OK) {
                err = nvs_set_u8(handle, "bright", brightness_);
                if (err == ESP_OK)
                    err = nvs_commit(handle);
                nvs_close(handle);
            }
            Error(err);
        }
        return true;
    }
    if (input.action == TouchAction::kVolume && page_ == Page::kVolume) {
        const int value = input.row < 0 ? 0 : input.row > 100 ? 100 : input.row;
        if (volume_handler_ && !volume_handler_(value, context_)) {
            amoled_ui::SetVolume(volume_);
            return true;
        }
        volume_ = value;
        amoled_ui::SetVolume(value);
        if (input.commit) {
            nvs_handle_t h;
            esp_err_t err = nvs_open_from_partition(agent_system::SettingsPartition(), "voice", NVS_READWRITE, &h);
            if (err == ESP_OK) {
                err = nvs_set_i8(h, "vol", value);
                if (err == ESP_OK)
                    err = nvs_commit(h);
                nvs_close(h);
            }
            Error(err);
        }
        return true;
    }
    if (input.action != TouchAction::kRow)
        return true;
    if (Sugarat() && HandleSugarat(input))
        return true;
    if (page_ == Page::kRoot) {
        const int update_row =
            6 + (agent_system::IsLauncher() || agent_system::CanReturnToLauncher() ? 1 : 0) +
            (extra_.render ? 1 : 0) + (extra_.open_brightness ? 1 : 0);
        if (update_.render && input.row == update_row) {
            page_ = Page::kUpdate;
            return true;
        }
        switch (input.row) {
            case 0:
                page_ = Page::kNetwork;
                break;
            case 1:
                page_ = Page::kApp;
                break;
            case 2:
                page_ = Page::kDevice;
                break;
            case 3:
                page_ = Page::kService;
                break;
            case 4:
                page_ = Page::kVolume;
                break;
            case 5:
                page_ = Page::kLanguage;
                break;
            case 6:
                if (agent_system::IsLauncher())
                    page_ = Page::kCovers;
                else if (agent_system::CanReturnToLauncher())
                    exit_ = true;
                break;
            case 7:
                if (extra_.render)
                    page_ = Page::kExtra;
                break;
            case 8:
                if (extra_.open_brightness) {
                    extra_.open_brightness(extra_.context);
                    page_ = Page::kExtra;
                }
                break;
        }
    } else if (page_ == Page::kNetwork && input.row == 0)
        Error(agent_link_wifi_setup());
    else if (page_ == Page::kCovers && input.row == 0) {
        cover_retry_ = true;
        amoled_ui::Toast(Tr("已请求重新同步封面", "Cover sync requested"));
    } else if (page_ == Page::kLanguage && (input.row == 0 || input.row == 1)) {
        amoled_ui::language.store(input.row ? amoled_ui::Language::kEnglish
                                            : amoled_ui::Language::kChinese);
        nvs_handle_t h;
        esp_err_t err = nvs_open_from_partition(agent_system::SettingsPartition(), "amoled175", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_i8(h, "language", input.row);
            if (err == ESP_OK)
                err = nvs_commit(h);
            nvs_close(h);
        }
        Error(err);
        page_ = Sugarat() ? Page::kSettings : Page::kRoot;
    }
    return true;
}
void AmoledSystemMenu::Render(amoled_ui::Screen& s, uint32_t view, uint32_t navigation, int battery,
                              bool charging) {
    s = {};
    s.kind = amoled_ui::View::kList;
    s.view = view;
    s.navigation = navigation;
    s.activity = amoled_ui::Activity::kIdle;
    snprintf(s.footer, sizeof s.footer, "%s",
             Tr("右滑返回 · 顶部下滑系统菜单", "Swipe right: back - top down: system"));
    const auto row = [&](const char* text) {
        snprintf(s.rows[s.row_count++], sizeof s.rows[0], "%s", text);
    };
    agent_link_wifi_info_t wifi{};
    (void)agent_link_wifi_info(&wifi);
    if (Sugarat())
        s.launcher_shell = true;
    if (Sugarat() && page_ == Page::kRoot) {
        s.kind = amoled_ui::View::kQuickMenu;
        s.battery_percent = battery;
        row(Tr("无线网络", "Wi-Fi"));
        row(Tr("声音", "Sound"));
        row(Tr("退出", "Exit"));
        row(Tr("设置", "Settings"));
    } else if (page_ == Page::kSettings) {
        s.kind = amoled_ui::View::kSettings;
        snprintf(s.title, sizeof s.title, "%s", Tr("设置", "Settings"));
        row(Tr("亮度", "Brightness"));
        row(Tr("启动器更新", "Launcher update"));
        if (extra_.render)
            row(Tr("设备工具", "Device tools"));
        row(Tr("关于应用", "About"));
        row(Tr("设备状态", "Device status"));
        row(Tr("语言", "Language"));
        row(Tr("使用说明", "User guide"));
        if (!agent_system::IsLauncher())
            row(Tr("返回启动器", "Return to launcher"));
    } else if (page_ == Page::kGuide) {
        snprintf(s.title, sizeof s.title, "%s", Tr("使用说明", "User guide"));
        for (int i = 0; i < amoled_ui::kUserGuideTopicCount; ++i)
            row(amoled_ui::SugaratUserGuide(i).title);
    } else if (page_ == Page::kGuideTopic) {
        const auto topic = amoled_ui::SugaratUserGuide(guide_topic_);
        snprintf(s.title, sizeof s.title, "%s", topic.title);
        snprintf(s.info, sizeof s.info, "%s", topic.body);
    } else if (page_ == Page::kExitConfirm || page_ == Page::kUpdateConfirm) {
        snprintf(s.title, sizeof s.title, "%s", Tr("返回启动器？", "Return to Launcher?"));
        snprintf(s.info, sizeof s.info, "%s",
                 page_ == Page::kUpdateConfirm
                     ? Tr("确认后结束当前应用，重启到启动器检查 Factory 更新。",
                          "Close this app and restart into Launcher to check Factory updates.")
                     : Tr("确认后结束当前应用，并重启到启动器主页。",
                          "Close this app and restart into the Launcher home screen."));
        row(Tr("取消", "Cancel"));
        row(Tr("确认返回", "Confirm"));
    } else if (page_ == Page::kUpdate && !update_.render) {
        snprintf(s.title, sizeof s.title, "%s", Tr("Factory 固件", "Factory firmware"));
        snprintf(s.info, sizeof s.info, "%s",
                 Tr("Factory 版本和更新在启动器中检查。\n返回后自动检查，不会自动安装更新。",
                    "View the Factory version and check updates in Launcher. Updates are not "
                    "installed automatically."));
        row(Tr("返回启动器检查更新", "Check updates in Launcher"));
    } else if (page_ == Page::kBrightness) {
        s.kind = amoled_ui::View::kBrightnessControl;
        s.brightness_percent = brightness_;
        snprintf(s.title, sizeof s.title, "%s", Tr("亮度", "Brightness"));
    } else if (page_ == Page::kRoot) {
        snprintf(s.title, sizeof s.title, "%s", Tr("系统菜单", "System"));
        snprintf(s.detail, sizeof s.detail, "%s", Tr("上下滑动查看更多", "Scroll for more"));
        row(Tr("网络与配网", "Network and WiFi setup"));
        row(Tr("当前应用", "Current application"));
        row(Tr("设备状态", "Device status"));
        row(Tr("应用服务", "App service"));
        row(Tr("音量", "Volume"));
        row(Tr("语言", "Language"));
        if (agent_system::IsLauncher())
            row(Tr("应用封面与同步", "App covers and sync"));
        else if (agent_system::CanReturnToLauncher())
            row(Tr("退出到启动器", "Exit to launcher"));
        if (extra_.render)
            row(Tr("硬件与电源", "Hardware and power"));
        if (extra_.open_brightness)
            row(Tr("屏幕亮度", "Screen brightness"));
        if (update_.render)
            row(Tr("固件更新", "Firmware update"));
    } else if (page_ == Page::kUpdate && update_.render) {
        update_.render(s, update_.context);
    } else if (page_ == Page::kNetwork) {
        snprintf(s.title, sizeof s.title, "%s", Tr("网络与配网", "Network"));
        if (!wifi.connected && wifi.setup_active) {
            snprintf(s.info, sizeof s.info,
                     Tr("请先完成 WiFi 配网\n\n1. 手机连接设备热点：\n%s\n\n"
                        "2. 浏览器打开：\nhttp://192.168.4.1\n\n"
                        "3. 选择 WiFi 并输入密码\n联网后即可下载应用，已安装应用可离线使用。",
                        "Set up WiFi first\n\n1. Connect your phone to:\n%s\n\n"
                        "2. Open in your browser:\nhttp://192.168.4.1\n\n"
                        "3. Select WiFi and enter its password.\nInstalled apps remain available offline."),
                     wifi.setup_ssid[0] ? wifi.setup_ssid : Tr("热点启动中，请稍候", "Starting hotspot..."));
        } else if (!wifi.connected) {
            snprintf(s.info, sizeof s.info, "%s",
                     Tr("WiFi 尚未连接，请稍候或检查网络。\n\n"
                        "如需更换网络，请点击下方开始配网。\n已安装应用仍可离线使用。",
                        "WiFi is not connected. Please wait or check your network.\n\n"
                        "To change networks, start WiFi setup below. Installed apps remain available offline."));
        } else {
            snprintf(s.info, sizeof s.info,
                     Tr("WiFi 已连接\n网络：%s\n地址：%s\n信号：%d dBm\n可以返回主页下载应用。",
                        "WiFi connected\nSSID: %s\nIP: %s\nSignal: %d dBm\nReturn home to download apps."),
                     wifi.ssid, wifi.ip, wifi.rssi);
        }
        row(Tr("开始或重新配网（保留绑定）", "Set up WiFi"));
    } else if (page_ == Page::kApp) {
        snprintf(s.title, sizeof s.title, "%s", Tr("当前应用", "Current application"));
        const auto& d = agent_system::Current();
        snprintf(s.info, sizeof s.info, "%s\n%s\nVersion: %.31s\nSDK: %s\n%s", d.name, d.app_id,
                 esp_app_get_description()->version, d.sdk, d.board);
        if (Sugarat()) {
            snprintf(s.info, sizeof s.info,
                     Tr("当前应用：%s\n版本：%.31s\nWiFi：%s\n网络：%s\n地址：%s\n电量：%d%%",
                        "App: %s\nVersion: %.31s\nWiFi: %s\nSSID: %s\nIP: %s\nBattery: %d%%"),
                     !strcmp(d.app_id, "agent.status") ? "Agent_link" : d.name,
                     esp_app_get_description()->version,
                     wifi.connected      ? Tr("已连接", "Connected")
                     : wifi.setup_active ? Tr("配网中", "Setting up")
                                         : Tr("未连接", "Disconnected"),
                     wifi.connected ? wifi.ssid : "--", wifi.connected ? wifi.ip : "--", battery);
        }
    } else if (page_ == Page::kDevice) {
        snprintf(s.title, sizeof s.title, "%s", Tr("设备状态", "Device status"));
        if (device_info_[0]) {
            snprintf(s.info, sizeof s.info, "%s", device_info_);
            return;
        }
        snprintf(s.info, sizeof s.info,
                 Tr("电量：%d%%\n充电：%s\n可用内存：%lu KB\n应用编号：%d\n网络：%s",
                    "Battery: %d%%\nCharging: %s\nFree heap: %lu KB\nApp slots: %d\nNetwork: %s"),
                 battery, charging ? Tr("充电中", "Charging") : Tr("未充电", "Not charging"),
                 static_cast<unsigned long>(esp_get_free_heap_size() / 1024),
                 agent_system::kAppSlotCount,
                 wifi.connected ? wifi.ssid : Tr("未连接", "Disconnected"));
    } else if (page_ == Page::kService) {
        snprintf(s.title, sizeof s.title, "%s", Tr("应用服务", "App service"));
        char url[256]{};
        (void)agent_system::CatalogUrl(url, sizeof url);
        snprintf(
            s.info, sizeof s.info,
            Tr("手机或电脑打开：\nhttp://%s:8081\n设置应用列表地址。\n当前：\n%s",
               "Open on your phone or PC:\nhttp://%s:8081\nSet the app catalog URL.\nCurrent:\n%s"),
            wifi.connected ? wifi.ip : "192.168.4.1",
            url[0] ? url : Tr("尚未设置", "Not configured"));
    } else if (page_ == Page::kVolume) {
        snprintf(s.title, sizeof s.title, "%s", Tr("音量", "Volume"));
        s.volume_control = true;
        if (Sugarat())
            s.kind = amoled_ui::View::kVolumeControl;
        snprintf(s.detail, sizeof s.detail, Tr("共享音量 %d%%", "Shared volume %d%%"), volume_);
    } else if (page_ == Page::kLanguage) {
        snprintf(s.title, sizeof s.title, "%s", Tr("语言", "Language"));
        row(Tr("中文", "Chinese"));
        row("English");
    } else if (page_ == Page::kCovers) {
        snprintf(s.title, sizeof s.title, "%s", Tr("应用封面与同步", "App covers and sync"));
        snprintf(s.info, sizeof s.info, "%s",
                 cover_summary_[0] ? cover_summary_ : Tr("等待联网同步", "Waiting for network"));
        row(Tr("重新同步封面", "Retry cover sync"));
    } else if (page_ == Page::kExtra && extra_.render) {
        extra_.render(s, extra_.context);
    }
}
#endif
