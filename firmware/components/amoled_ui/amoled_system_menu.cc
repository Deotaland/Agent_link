#include "amoled_system_menu.h"
#ifdef AGENT_SYSTEM_ENABLED
#include "agent_system.h"
#include "agent_link_network.h"
#include "amoled_ui_language.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"
#include <cstdio>

using amoled_ui::Tr;
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
void AmoledSystemMenu::Open() {
    page_ = Page::kRoot;
    exit_ = false;
    nvs_handle_t h;
    int8_t value = 70;
    if (nvs_open_from_partition(agent_system::SettingsPartition(), "voice", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i8(h, "vol", &value);
        nvs_close(h);
    }
    volume_ = value < 0 ? 0 : value > 100 ? 100 : value;
    amoled_ui::SetVolume(volume_);
}
bool AmoledSystemMenu::Handle(const amoled_ui::Input& input) {
    if (!Active())
        return false;
    using amoled_ui::TouchAction;
    if (input.action == TouchAction::kSystem) {
        page_ = Page::kRoot;
        return true;
    }
    if (input.action == TouchAction::kBack || input.action == TouchAction::kHome) {
        page_ = page_ == Page::kRoot ? Page::kClosed : Page::kRoot;
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
    if (page_ == Page::kRoot) {
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
        page_ = Page::kRoot;
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
    if (page_ == Page::kRoot) {
        snprintf(s.title, sizeof s.title, "%s", Tr("系统菜单", "System"));
        snprintf(s.detail, sizeof s.detail, "%s", Tr("上下滑动查看更多", "Scroll for more"));
        row(Tr("网络与配网", "Network and WiFi setup"));
        row(Tr("当前应用", "Current application"));
        row(Tr("设备状态", "Device status"));
        row(Tr("应用服务", "App service"));
        row(Tr("音量", "Volume"));
        row("语言 / Language");
        if (agent_system::IsLauncher())
            row(Tr("应用封面与同步", "App covers and sync"));
        else if (agent_system::CanReturnToLauncher())
            row(Tr("返回应用选择器", "Return to launcher"));
    } else if (page_ == Page::kNetwork) {
        snprintf(s.title, sizeof s.title, "%s", Tr("网络与配网", "Network"));
        snprintf(s.info, sizeof s.info,
                 Tr("网络：%s\n地址：%s\n信号：%d dBm\n配网热点：%s\n配网页：192.168.4.1",
                    "SSID: %s\nIP: %s\nSignal: %d dBm\nSetup AP: %s\nSetup: 192.168.4.1"),
                 wifi.connected ? wifi.ssid : Tr("未连接", "Disconnected"), wifi.ip, wifi.rssi,
                 wifi.setup_ssid);
        row(Tr("开始或重新配网（保留绑定）", "Set up WiFi (keep binding)"));
    } else if (page_ == Page::kApp) {
        snprintf(s.title, sizeof s.title, "%s", Tr("当前应用", "Current application"));
        const auto& d = agent_system::Current();
        snprintf(s.info, sizeof s.info, "%s\n%s\nVersion: %.31s\nSDK: %s\n%s", d.name, d.app_id,
                 esp_app_get_description()->version, d.sdk, d.board);
    } else if (page_ == Page::kDevice) {
        snprintf(s.title, sizeof s.title, "%s", Tr("设备状态", "Device status"));
        snprintf(
            s.info, sizeof s.info,
            Tr("电量：%d%%\n充电：%s\n可用内存：%lu KB\n应用槽：3 × 3 MB\n网络：%s",
               "Battery: %d%%\nCharging: %s\nFree heap: %lu KB\nApp slots: 3 x 3 MB\nNetwork: %s"),
            battery, charging ? Tr("充电中", "Charging") : Tr("未充电", "Not charging"),
            static_cast<unsigned long>(esp_get_free_heap_size() / 1024),
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
        snprintf(s.detail, sizeof s.detail, Tr("共享音量 %d%%", "Shared volume %d%%"), volume_);
    } else if (page_ == Page::kLanguage) {
        snprintf(s.title, sizeof s.title, "%s", "语言 / Language");
        row("中文");
        row("English");
    } else if (page_ == Page::kCovers) {
        snprintf(s.title, sizeof s.title, "%s", Tr("应用封面与同步", "App covers and sync"));
        snprintf(s.info, sizeof s.info, "%s",
                 cover_summary_[0] ? cover_summary_ : Tr("等待联网同步", "Waiting for network"));
        row(Tr("重新同步封面", "Retry cover sync"));
    }
}
#endif
