#pragma once
#include "application.h"
#include "config.h"
#include "amoled_hardware.h"
#include "amoled_ui.h"
#include "amoled_ui_controls.h"
#include "amoled_ui_language.h"
#include "amoled_system_menu.h"
#include "amoled_launcher_model.h"
#include "factory_view.h"
#include "factory_hardware_services.h"
#include "agent_system.h"
#include "agent_link_network.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using amoled_ui::Tr;
class AmoledFactoryApplication : public Application {
   public:
    AmoledFactoryApplication() {
        nvs_handle_t h;
        int8_t language = 0;
        if (nvs_open_from_partition(agent_system::SettingsPartition(), "amoled175", NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_i8(h, "language", &language);
            nvs_close(h);
        }
        amoled_ui::language.store(language == 1 ? amoled_ui::Language::kEnglish
                                                : amoled_ui::Language::kChinese);
        hardware_.InitDisplayAndTouch(32);
        menu_.SetDefaultVolume(AUDIO_DEFAULT_VOLUME);
        hardware_services_ = CreateFactoryHardwareServices(hardware_);
        if (hardware_services_) {
            menu_.SetExtraPage(hardware_services_->Menu());
            menu_.SetVolumeHandler(
                [](int value, void* context) {
                    return static_cast<FactoryHardwareServices*>(context)->SetVolume(value);
                },
                hardware_services_.get());
        }
        events_ = xQueueCreate(8, sizeof(amoled_ui::Input));
        ESP_ERROR_CHECK(events_ ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(amoled_ui::StartUi(&hardware_.panel_, &hardware_.touch_, OnInput, this,
                                           &amoled_factory::Renderer(),
                                           amoled_ui::DisplayPerformance::kLauncher));
        RefreshLocal();
        Home();
        Publish();
        ESP_ERROR_CHECK(xTaskCreate(CoverEntry, "cover_sync", 16384, this, 3, &cover_task_) ==
                                pdPASS
                            ? ESP_OK
                            : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(ControlEntry, "system_ctrl", 24576, this, 4, nullptr) == pdPASS
                            ? ESP_OK
                            : ESP_ERR_NO_MEM);
    }
    const char* Name() const override {
        return AMOLED_DEVICE_NAME;
    }
    const char* Model() const override {
        return AMOLED_FIRMWARE_MODEL;
    }
    // The board factory owns app management; Agent sessions belong to the installed voice app.
    const agent_platform_t* Platform() const override {
        return nullptr;
    }
    uint32_t Capabilities() const override {
        return (hardware_.panel_.Ready() ? AGENT_CAP_SCREEN : 0) |
               ((hardware_.touch_.Ready() || hardware_.button_ok_) ? AGENT_CAP_BUTTON : 0) |
               (hardware_.power_ok_ ? AGENT_CAP_BATTERY : 0);
    }
    void OnLinkStatus(const agent_link_status_t& status) override {
        amoled_ui::SetStatus(status);
    }
    int GetBatteryLevel() override {
        return battery_.load();
    }
    bool IsCharging() override {
        return charging_.load();
    }

   private:
    enum class Page { kHome, kCatalog, kLocalDetail, kCatalogDetail, kRemove, kServiceError };
    void Home() {
        page_ = Page::kHome;
        ++navigation_;
    }
    bool Check(esp_err_t error) {
        if (error == ESP_OK)
            return true;
        if (error == agent_system::kStoreError) {
            agent_system::LastStoreError(&store_error_);
            page_ = Page::kServiceError;
            return false;
        }
        if (error == agent_system::kRestartRequired) {
            amoled_ui::Toast(
                Tr("正在刷新应用布局，即将返回主页", "Refreshing app layout. Restarting..."), 1500);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            return false;
        }
        char text[128];
        snprintf(text, sizeof text, Tr("操作失败：%s", "Failed: %s"), esp_err_to_name(error));
        if (error == agent_system::kSlotsFull)
            snprintf(text, sizeof text, "%s",
                     Tr("应用槽已满，请先卸载旧应用", "Slots full. Remove an old app first."));
        if (error == agent_system::kAlreadyInstalled)
            snprintf(text, sizeof text, "%s",
                     Tr("此应用已安装，更新前请先卸载",
                        "App already installed. Remove it before updating."));
        if (error == agent_system::kNoAppSpace)
            snprintf(text, sizeof text, "%s",
                     Tr("连续应用空间不足，请先卸载其他应用",
                        "Not enough contiguous space. Remove another app."));
        if (error == agent_system::kNeedsFullPackage)
            snprintf(text, sizeof text, "%s",
                     Tr("请先通过 USB 更新完整启动器固件",
                        "Update the full launcher package over USB first."));
        if (error == agent_system::kLegacySlotsFull)
            snprintf(text, sizeof text, "%s",
                     Tr("旧版应用需要前两个槽，请先释放或重新编译",
                        "Legacy app needs slot 0/1. Free one or rebuild it."));
        if (error == agent_system::kUnsupportedPackage)
            snprintf(text, sizeof text, "%s",
                     Tr("此版本不是可安装的纯应用，请联系发布者",
                        "This release needs an installable application binary."));
        if (error == ESP_ERR_NOT_FOUND && page_ == Page::kCatalog)
            snprintf(text, sizeof text, "%s",
                     Tr("请先在系统菜单设置应用服务", "Configure App service in System first"));
        amoled_ui::Toast(text, 6000);
        return false;
    }
    void Busy(const char* text) {
        screen_ = {};
        screen_.view = ++view_;
        screen_.navigation = ++navigation_;
        screen_.kind = amoled_ui::View::kInfo;
        screen_.busy = true;
        screen_.launcher_shell = true;
        snprintf(screen_.title, sizeof screen_.title, "%s", text);
        snprintf(screen_.detail, sizeof screen_.detail, "%s",
                 Tr("请稍候，完成后可继续操作", "Please wait for completion"));
        if (hardware_services_)
            hardware_services_->Decorate(screen_);
        amoled_ui::SetScreen(screen_);
    }
    void RefreshLocal() {
        if (!Check(agent_system::List(apps_, agent_system::kAppSlotCount, &app_count_)))
            app_count_ = 0;
        carousel_.SetCount(app_count_);
        RefreshCover();
    }
    void RefreshCover() {
        cover_ = {};
        if (!carousel_.IsAdd())
            (void)agent_system::ReadCover(apps_[carousel_.Index()].slot, &cover_);
    }
    void Catalog(unsigned page = 1) {
        page_ = Page::kCatalog;
        ++navigation_;
        Busy(Tr("正在获取在线应用", "Loading online apps"));
        WaitForCoverSync();
        if (!Check(agent_system::FetchCatalogPage(catalog_, agent_system::kMaxApps, &catalog_count_,
                                                  page, &catalog_page_)))
            catalog_count_ = 0;
        retry_requested_ = true;
    }
    bool Installed(const agent_system::CatalogApp& app) const {
        for (size_t i = 0; i < app_count_; ++i)
            if (app.community_play ? apps_[i].community_play == app.community_play
                                   : !strcmp(apps_[i].descriptor.app_id, app.app_id))
                return true;
        return false;
    }
    void WaitForCoverSync() {
        // Only this controller starts the worker. Once it finishes, no new sync can
        // start while this explicit firmware operation is running.
        while (sync_busy_.load())
            vTaskDelay(pdMS_TO_TICKS(30));
    }
    void Select(int row) {
        if (row < 0)
            return;
        switch (page_) {
            case Page::kHome:
                if (row == 0) {
                    if (carousel_.IsAdd())
                        Catalog();
                    else {
                        selected_ = carousel_.Index();
                        page_ = Page::kLocalDetail;
                    }
                }
                break;
            case Page::kCatalog:
                if (static_cast<size_t>(row) < catalog_count_) {
                    selected_ = row;
                    page_ = Page::kCatalogDetail;
                } else if (catalog_page_.page > 1 && static_cast<size_t>(row) == catalog_count_) {
                    Catalog(catalog_page_.page - 1);
                } else if (catalog_page_.has_more &&
                           static_cast<size_t>(row) ==
                               catalog_count_ + (catalog_page_.page > 1 ? 1 : 0)) {
                    Catalog(catalog_page_.page + 1);
                }
                break;
            case Page::kServiceError:
                if (row == 0)
                    Catalog(catalog_page_.page);
                else
                    Home();
                break;
            case Page::kLocalDetail:
                if (row == 0)
                    Check(agent_system::Launch(apps_[selected_].slot));
                else if (row == 1)
                    page_ = Page::kRemove;
                break;
            case Page::kRemove:
                if (row == 0)
                    page_ = Page::kLocalDetail;
                else if (row == 1) {
                    Busy(Tr("正在卸载应用", "Removing application"));
                    WaitForCoverSync();
                    Check(agent_system::Remove(apps_[selected_].slot));
                    RefreshLocal();
                    Home();
                }
                break;
            case Page::kCatalogDetail:
                if (catalog_[selected_].package !=
                    agent_system::CatalogApp::Package::kApplication) {
                    page_ = Page::kCatalog;
                    break;
                }
                if (row == 0) {
                    if (Installed(catalog_[selected_])) {
                        Check(agent_system::kAlreadyInstalled);
                        break;
                    }
                    Busy(Tr("正在安装应用", "Installing application"));
                    WaitForCoverSync();
                    if (Check(agent_system::Install(catalog_[selected_], Progress, this))) {
                        amoled_ui::Toast(
                            Tr("安装完成，点击封面可启动", "Installed. Tap its cover to launch."),
                            5000);
                        RefreshLocal();
                        Home();
                        retry_requested_ = true;
                    }
                } else if (row == 1)
                    page_ = Page::kCatalog;
                break;
        }
        ++navigation_;
    }
    void Publish() {
        if (menu_.Active())
            menu_.Render(screen_, view_, navigation_, battery_.load(), charging_.load());
        else {
            screen_ = {};
            screen_.view = view_;
            screen_.navigation = navigation_;
            screen_.kind = amoled_ui::View::kList;
            screen_.activity = amoled_ui::Activity::kIdle;
            screen_.launcher_shell = true;
            snprintf(screen_.footer, sizeof screen_.footer, "%s",
                     Tr("顶部下滑系统菜单 · 右滑返回", "Top down: System - swipe right: back"));
            const auto title = [&](const char* s) {
                snprintf(screen_.title, sizeof screen_.title, "%s", s);
            };
            const auto row = [&](const char* s) {
                snprintf(screen_.rows[screen_.row_count++], sizeof screen_.rows[0], "%s", s);
            };
            if (page_ == Page::kHome) {
                screen_.kind = amoled_ui::View::kCarousel;
                screen_.card_index = carousel_.Index();
                screen_.card_count = carousel_.Pages();
                screen_.add_card = carousel_.IsAdd();
                title(screen_.add_card ? Tr("添加应用", "Add application")
                                       : apps_[carousel_.Index()].descriptor.name);
                if (!screen_.add_card) {
                    snprintf(screen_.app_id, sizeof screen_.app_id, "%s",
                             apps_[carousel_.Index()].descriptor.app_id);
                    screen_.cover_slot = cover_.slot;
                    screen_.cover_revision = cover_.revision;
                }
            } else if (page_ == Page::kCatalog) {
                char heading[64];
                snprintf(heading, sizeof heading,
                         Tr("在线应用 · 第 %u 页", "Online apps · page %u"), catalog_page_.page);
                title(heading);
                for (size_t i = 0; i < catalog_count_; ++i) {
                    char name[112];
                    snprintf(name, sizeof name, "%s%s", catalog_[i].name,
                             Installed(catalog_[i]) ? Tr(" · 已安装", " · Installed") : "");
                    row(name);
                }
                if (catalog_page_.page > 1)
                    row(Tr("上一页", "Previous page"));
                if (catalog_page_.has_more)
                    row(Tr("下一页", "Next page"));
                if (!catalog_count_)
                    snprintf(screen_.info, sizeof screen_.info, "%s",
                             Tr("暂无应用。检查网络和系统菜单中的应用服务地址。",
                                "No apps. Check network and App service URL in System."));
            } else if (page_ == Page::kCatalogDetail) {
                const auto& app = catalog_[selected_];
                title(app.name);
                snprintf(screen_.info, sizeof screen_.info, "%s\n%s\n%lu KB", app.app_id,
                         app.version, static_cast<unsigned long>(app.size / 1024));
                if (app.package != agent_system::CatalogApp::Package::kApplication) {
                    snprintf(screen_.info, sizeof screen_.info, "%s\n%s\n%s", app.version,
                             app.filename,
                             app.package == agent_system::CatalogApp::Package::kTooLarge
                                 ? Tr("应用超过当前设备容量，请联系发布者。",
                                      "Application exceeds this device's capacity.")
                                 : Tr("此版本为整机包或其他格式。启动器需要发布者提供纯应用 .bin。",
                                      "This release requires a pure application .bin for launcher "
                                      "installation."));
                    row(Tr("返回列表", "Back to list"));
                } else {
                    row(Installed(app)
                            ? Tr("已安装（更新前请先卸载）", "Installed (remove before updating)")
                            : Tr("下载安装", "Download and install"));
                    row(Tr("返回列表", "Back to list"));
                }
            } else if (page_ == Page::kServiceError) {
                title(Tr("应用服务暂不可用", "App service unavailable"));
                char header[192];
                snprintf(header, sizeof header, "HTTP %d %.47s\nrequestId: %.79s\n%s %u s\n",
                         store_error_.http_status, store_error_.code, store_error_.request_id,
                         Tr("建议等待", "Retry after"), store_error_.retry_after);
                std::string detail = std::string(header) + store_error_.message;
                size_t length = std::min(detail.size(), sizeof screen_.info - 1);
                while (length && (static_cast<unsigned char>(detail[length]) & 0xC0) == 0x80)
                    --length;
                memcpy(screen_.info, detail.data(), length);
                screen_.info[length] = 0;
                row(Tr("重新获取列表", "Reload list"));
                row(Tr("返回主页", "Home"));
            } else if (page_ == Page::kLocalDetail) {
                const auto& app = apps_[selected_];
                title(app.descriptor.name);
                screen_.kind = amoled_ui::View::kAppActions;
                screen_.card_index = carousel_.Index();
                screen_.card_count = carousel_.Pages();
                snprintf(screen_.app_id, sizeof screen_.app_id, "%s", app.descriptor.app_id);
                snprintf(screen_.detail, sizeof screen_.detail, "%s",
                         app.release_version[0] ? app.release_version : app.version);
                screen_.cover_slot = cover_.slot;
                screen_.cover_revision = cover_.revision;
            } else {
                const auto& app = apps_[selected_];
                title(page_ == Page::kRemove ? Tr("确认卸载？", "Remove this app?")
                                             : app.descriptor.name);
                snprintf(screen_.info, sizeof screen_.info, "%s\n%s  %s\n%lu KB",
                         app.descriptor.app_id, app.version, app.slot,
                         static_cast<unsigned long>(app.size / 1024));
                if (page_ == Page::kRemove) {
                    row(Tr("取消", "Cancel"));
                    row(Tr("确认卸载（保留设备设置）", "Remove (keep device settings)"));
                } else {
                    row(Tr("启动应用", "Launch application"));
                    row(Tr("卸载应用", "Remove application"));
                }
            }
        }
        if (hardware_services_)
            hardware_services_->Decorate(screen_);
        amoled_ui::SetScreen(screen_);
    }
    void ControlLoop() {
        amoled_ui::ButtonDebounce button;
        int64_t next_update = 0;
        while (true) {
            amoled_ui::Input input;
            const bool received = xQueueReceive(events_, &input, pdMS_TO_TICKS(30)) == pdTRUE;
            const bool fresh =
                received && amoled_ui::CurrentInput(input, view_, esp_timer_get_time());
            if (received && !fresh && input.view == view_) {
                ++view_;
                Publish();
            }
            if (fresh) {
                ++view_;
                ++navigation_;
                if (input.action == amoled_ui::TouchAction::kSystem)
                    menu_.Open();
                else if (menu_.Active())
                    menu_.Handle(input);
                else if (input.action == amoled_ui::TouchAction::kRow)
                    Select(input.row);
                else if (input.action == amoled_ui::TouchAction::kHome)
                    Home();
                else if (input.action == amoled_ui::TouchAction::kBack) {
                    if (page_ == Page::kRemove)
                        page_ = Page::kLocalDetail;
                    else if (page_ == Page::kLocalDetail)
                        Home();
                    else if (page_ == Page::kCatalogDetail)
                        page_ = Page::kCatalog;
                    else
                        Home();
                } else if (page_ == Page::kHome &&
                           (input.action == amoled_ui::TouchAction::kPreviousCard ||
                            input.action == amoled_ui::TouchAction::kNextCard)) {
                    if (carousel_.Move(input.action == amoled_ui::TouchAction::kNextCard ? 1 : -1))
                        RefreshCover();
                }
                if (menu_.TakeExit())
                    Check(agent_system::ReturnToLauncher());
                if (menu_.TakeCoverRetry())
                    retry_requested_ = true;
                Publish();
            }
            int value, saved;
            if (volume_.Take(value, saved)) {
                amoled_ui::Input volume{};
                volume.action = amoled_ui::TouchAction::kVolume;
                volume.row = saved >= 0 ? saved : value;
                volume.commit = saved >= 0;
                menu_.Handle(volume);
                Publish();
            }
            if (button.Update(hardware_.ButtonDown(), esp_timer_get_time())) {
                menu_.Close();
                Home();
                ++view_;
                ++navigation_;
                Publish();
            }
            if (hardware_services_) {
                if (hardware_services_->Button(hardware_.ButtonDown(), esp_timer_get_time())) {
                    menu_.OpenExtraPage();
                    ++view_;
                    ++navigation_;
                    Publish();
                }
                if (hardware_services_->Tick(!sync_busy_.load())) {
                    char info[384]{};
                    hardware_services_->DeviceInfo(info, sizeof info);
                    menu_.SetDeviceInfo(info);
                    Publish();
                }
            }
            if (esp_timer_get_time() >= next_update) {
                int battery = -1;
                bool charging = false;
                hardware_.Battery(&battery, &charging);
                battery_.store(battery);
                charging_.store(charging);
                amoled_ui::SetBattery(battery, charging);
                TickCoverSync();
                Publish();
                next_update = esp_timer_get_time() + 1000000;
            }
        }
    }
    void TickCoverSync() {
        const uint32_t app_revision = agent_system::Revision();
        if (app_revision != seen_app_revision_) {
            seen_app_revision_ = app_revision;
            RefreshLocal();
            Home();
        }
        if (!cover_task_)
            return;
        const int64_t now = esp_timer_get_time();
        const unsigned revision = sync_revision_.load();
        if (revision != seen_sync_revision_) {
            seen_sync_revision_ = revision;
            RefreshCover();
            next_sync_ = now + (sync_error_.load() == ESP_OK ? 300000000LL : 60000000LL);
        }
        agent_link_wifi_info_t wifi{};
        (void)agent_link_wifi_info(&wifi);
        char url[256]{};
        (void)agent_system::CatalogUrl(url, sizeof url);
        if (strcmp(url, sync_url_)) {
            snprintf(sync_url_, sizeof sync_url_, "%s", url);
            retry_requested_ = true;
        }
        if (wifi.connected && !was_connected_) {
            next_sync_ = now + 3000000;
            retry_requested_ = false;
        }
        was_connected_ = wifi.connected;
        if (!sync_busy_.load() && wifi.connected && url[0] &&
            (retry_requested_ || now >= next_sync_)) {
            retry_requested_ = false;
            sync_busy_.store(true);
            xTaskNotifyGive(cover_task_);
        }
        char summary[384]{};
        if (!wifi.connected)
            snprintf(summary, sizeof summary, "%s", Tr("等待 WiFi 连接", "Waiting for WiFi"));
        else if (!url[0])
            snprintf(summary, sizeof summary, "%s",
                     Tr("请先设置应用服务地址", "Configure App service first"));
        else if (sync_busy_.load())
            snprintf(summary, sizeof summary, "%s", Tr("正在后台同步", "Syncing in background"));
        else if (seen_sync_revision_ && sync_error_.load() != ESP_OK)
            snprintf(summary, sizeof summary,
                     Tr("同步失败：%s\n60 秒后重试", "Sync failed: %s\nRetry in 60 seconds"),
                     esp_err_to_name(sync_error_.load()));
        else
            snprintf(summary, sizeof summary, "%s",
                     Tr("联网后自动同步，可手动重试", "Automatic sync online; retry available"));
        for (size_t i = 0; i < app_count_; ++i) {
            agent_system::CoverStatus status{};
            (void)agent_system::GetCoverStatus(apps_[i].slot, &status);
            const char* state = CoverStateText(status.state);
            const size_t used = strlen(summary);
            snprintf(summary + used, sizeof summary - used, "\n%.28s: %s%s",
                     apps_[i].descriptor.name, state,
                     status.cached && status.state == agent_system::CoverState::kFailed
                         ? Tr("（保留旧图）", " (cached)")
                         : "");
            if (status.error != ESP_OK) {
                const size_t tail = strlen(summary);
                snprintf(summary + tail, sizeof summary - tail, " [%s,0x%x]",
                         CoverStageText(status.stage), static_cast<unsigned>(status.error));
            }
        }
        menu_.SetCoverSummary(summary);
    }
    static const char* CoverStateText(agent_system::CoverState state) {
        using S = agent_system::CoverState;
        switch (state) {
            case S::kDownloading:
                return Tr("下载中", "Downloading");
            case S::kReady:
                return Tr("已缓存", "Cached");
            case S::kNoImage:
                return Tr("未配置图片", "No image supplied");
            case S::kNotListed:
                return Tr("此版本不在目录中", "Version not in catalog");
            case S::kFailed:
                return Tr("失败", "Failed");
            default:
                return Tr("等待同步", "Pending");
        }
    }
    static const char* CoverStageText(agent_system::CoverStage stage) {
        using S = agent_system::CoverStage;
        switch (stage) {
            case S::kMount:
                return Tr("存储", "Storage");
            case S::kCatalog:
                return Tr("目录", "Catalog");
            case S::kMetadata:
                return Tr("图片信息", "Metadata");
            case S::kConnect:
                return Tr("连接", "Connection");
            case S::kHeaders:
                return "HTTP";
            case S::kReceive:
                return Tr("下载写入", "Download/write");
            case S::kValidate:
                return Tr("图片校验", "Validation");
            case S::kPublish:
                return Tr("文件保存", "Save");
            case S::kBinding:
                return Tr("关联记录", "Binding");
            case S::kRead:
                return Tr("本地读取", "Read");
            default:
                return "-";
        }
    }
    void CoverLoop() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            const esp_err_t error = agent_system::SyncStoreCovers();
            if (error == ESP_OK)
                ESP_LOGI("amoled.cover", "background sync complete");
            else
                ESP_LOGW("amoled.cover", "background sync failed: %s; retry scheduled",
                         esp_err_to_name(error));
            sync_error_.store(error);
            sync_revision_.fetch_add(1);
            sync_busy_.store(false);
        }
    }
    static void Progress(uint32_t received, uint32_t total, void* context) {
        auto* self = static_cast<AmoledFactoryApplication*>(context);
        const int64_t now = esp_timer_get_time();
        if (now < self->progress_at_ && received != total)
            return;
        self->progress_at_ = now + 250000;
        snprintf(self->screen_.info, sizeof self->screen_.info, "%lu%%  (%lu / %lu KB)",
                 static_cast<unsigned long>(static_cast<uint64_t>(received) * 100 / total),
                 static_cast<unsigned long>(received / 1024),
                 static_cast<unsigned long>(total / 1024));
        amoled_ui::SetScreen(self->screen_);
    }
    static bool OnInput(const amoled_ui::Input& input, void* context) {
        auto* self = static_cast<AmoledFactoryApplication*>(context);
        amoled_ui::Wake();
        if (input.action == amoled_ui::TouchAction::kVolume) {
            self->volume_.Submit(input.row, input.commit);
            return true;
        }
        return xQueueSend(self->events_, &input, 0) == pdTRUE;
    }
    static void ControlEntry(void* context) {
        static_cast<AmoledFactoryApplication*>(context)->ControlLoop();
    }
    static void CoverEntry(void* context) {
        static_cast<AmoledFactoryApplication*>(context)->CoverLoop();
    }
    AmoledHardware hardware_;
    std::unique_ptr<FactoryHardwareServices> hardware_services_;
    QueueHandle_t events_ = nullptr;
    AmoledSystemMenu menu_;
    AmoledLauncherModel carousel_;
    agent_system::Cover cover_;
    amoled_ui::VolumeMailbox volume_;
    amoled_ui::Screen screen_;
    agent_system::App apps_[agent_system::kAppSlotCount]{};
    uint32_t seen_app_revision_ = 0;
    agent_system::CatalogApp catalog_[agent_system::kMaxApps]{};
    agent_system::CatalogPage catalog_page_;
    agent_system::StoreError store_error_;
    size_t app_count_ = 0, catalog_count_ = 0, selected_ = 0;
    uint32_t view_ = 1, navigation_ = 1;
    Page page_ = Page::kHome;
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false};
    int64_t progress_at_ = 0;
    TaskHandle_t cover_task_ = nullptr;
    std::atomic<bool> sync_busy_{false};
    std::atomic<esp_err_t> sync_error_{ESP_OK};
    std::atomic<unsigned> sync_revision_{0};
    unsigned seen_sync_revision_ = 0;
    int64_t next_sync_ = 0;
    bool was_connected_ = false, retry_requested_ = false;
    char sync_url_[256]{};
};

