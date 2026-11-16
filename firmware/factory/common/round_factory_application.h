#pragma once
#include "application.h"
#include "config.h"
#include "amoled_hardware.h"
#include "amoled_ui.h"
#include "amoled_ui_controls.h"
#include "amoled_ui_language.h"
#include "amoled_ui_guides.h"
#include "amoled_system_menu.h"
#include "amoled_launcher_model.h"
#include "factory_view.h"
#include "factory_hardware_services.h"
#include "factory_store_job.h"
#include "agent_system.h"
#ifdef AGENT_LAUNCHER_AB
#include "launcher_update.h"
#endif
#include "agent_link_network.h"
#include "esp_log.h"
#include "esp_partition.h"
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
    using Startup = void (*)(AmoledHardware&);
    explicit AmoledFactoryApplication(bool tile_home = false, Startup startup = nullptr)
        : tile_home_(tile_home) {
        nvs_handle_t h;
        int8_t language = static_cast<int8_t>(amoled_ui::kDefaultLanguage);
        if (nvs_open_from_partition(agent_system::SettingsPartition(), "amoled175", NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_i8(h, "language", &language);
            nvs_close(h);
        }
        amoled_ui::language.store(amoled_ui::SavedLanguage(language));
        hardware_.InitDisplayAndTouch(32);
        if (startup) startup(hardware_);
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
#ifdef AGENT_LAUNCHER_AB
        ESP_ERROR_CHECK(agent_system::launcher::Init());
        menu_.SetUpdatePage({this,
                             [](const amoled_ui::Input& input, void* ctx) {
                                 return static_cast<AmoledFactoryApplication*>(ctx)->HandleUpdate(
                                     input);
                             },
                             [](amoled_ui::Screen& screen, void* ctx) {
                                 static_cast<AmoledFactoryApplication*>(ctx)->RenderUpdate(screen);
                             }});
#endif
        events_ = xQueueCreate(8, sizeof(amoled_ui::Input));
        ESP_ERROR_CHECK(events_ ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(amoled_ui::StartUi(&hardware_.panel_, &hardware_.touch_, OnInput, this,
                                           &amoled_factory::Renderer(),
                                           amoled_ui::DisplayPerformance::kLauncher));
        RefreshLocal();
        Home();
#ifdef AGENT_LAUNCHER_AB
        if (tile_home_ && AmoledSystemMenu::TakeUpdateRequest()) {
            // Defer the UI-requested check until the existing WiFi connection is ready.
            pending_update_check_ = true;
        }
#endif
        if (tile_home_) {
            store_job_ = std::make_unique<FactoryStoreJob>();
            if (store_job_->Restore(apps_, app_count_)) {
                const auto restored = store_job_->Read().result;
                if (restored.kind == FactoryStoreJob::Kind::kInstall &&
                    restored.state == FactoryStoreJob::State::kSuccess)
                    store_job_->Acknowledge();
                else if (!startup)
                    page_ = Page::kJobResult;
            }
        }
        Publish();
        ESP_ERROR_CHECK(xTaskCreate(CoverEntry, "cover_sync", 16384, this, 3, &cover_task_) ==
                                pdPASS
                            ? ESP_OK
                            : ESP_ERR_NO_MEM);
        if (store_job_)
            ESP_ERROR_CHECK(store_job_->Init(
                [](void* context) {
                    static_cast<AmoledFactoryApplication*>(context)->WaitForCoverSync();
                },
                this));
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
    enum class Page {
        kHome,
        kDashboard,
        kApps,
        kCatalog,
        kDownloadCode,
        kDownloadHelp,
        kLocalDetail,
        kCatalogDetail,
        kRemove,
        kServiceError,
        kJobProgress,
        kJobResult,
        kInterrupt
    };
    bool StoreRunning() {
        return store_job_ && store_job_->Running();
    }
#ifdef AGENT_LAUNCHER_AB
    bool StoreInstalling() {
        if (!store_job_)
            return false;
        const auto job = store_job_->Read();
        return job.running && job.result.kind == FactoryStoreJob::Kind::kInstall;
    }
    bool StartsHighLoad(const amoled_ui::Input& input) {
        if (menu_.Active())
            return menu_.ExtraActive() && hardware_services_ &&
                   hardware_services_->StartsHighLoad(input);
        return input.action == amoled_ui::TouchAction::kRow &&
               ((page_ == Page::kRemove && input.row == 1) ||
                (page_ == Page::kCatalogDetail && input.row == 0));
    }
#endif
    bool NeedsRestart() {
        return store_job_ && store_job_->Read().result.state == FactoryStoreJob::State::kRestart;
    }
    void Home() {
        page_ = Page::kHome;
        ++navigation_;
    }
    void Back() {
        if (page_ == Page::kRemove || page_ == Page::kInterrupt)
            page_ = Page::kLocalDetail;
        else if (page_ == Page::kCatalogDetail)
            page_ = tile_home_ ? Page::kDownloadCode : Page::kCatalog;
        else if (page_ == Page::kDownloadHelp)
            page_ = Page::kDownloadCode;
        else if (!tile_home_ && page_ == Page::kJobResult && catalog_count_)
            page_ = Page::kCatalog;
        else
            Home();
    }
    void ShortPowerPress() {
        if (pending_launch_[0])
            return;
        if (menu_.Active())
            menu_.Close();
        else if (page_ == Page::kHome || page_ == Page::kDashboard)
            hardware_services_->RequestSleep();
        else
            Back();
        ++view_;
        ++navigation_;
        Publish();
    }
    void RequestLaunch(const char* slot) {
        if (!tile_home_) {
            Check(agent_system::Launch(slot));
            return;
        }
        snprintf(pending_launch_, sizeof pending_launch_, "%s", slot);
        launch_dispatched_ = false;
        launch_started_ = esp_timer_get_time();
        page_ = Page::kLocalDetail;
        launch_view_ = ++view_;
        ++navigation_;
        Publish();
    }
    void PollLaunch() {
        if (!pending_launch_[0])
            return;
        const auto elapsed = esp_timer_get_time() - launch_started_;
        if (elapsed > 30000000) {
            pending_launch_[0] = 0;
            launch_dispatched_ = false;
            Check(ESP_ERR_TIMEOUT);
            ++view_;
            Publish();
            return;
        }
        if (launch_dispatched_) {
#ifdef AGENT_LAUNCHER_AB
            const auto update = agent_system::launcher::Read();
            if (!agent_system::launcher::Busy() && update.state == agent_system::launcher::State::kError) {
                pending_launch_[0] = 0;
                launch_dispatched_ = false;
                Check(update.error);
                ++view_;
                Publish();
            }
#endif
            return;
        }
        if (StoreRunning() || sync_busy_.load() || elapsed < 250000 ||
            !amoled_ui::FramePresented(launch_view_))
            return;
#ifdef AGENT_LAUNCHER_AB
        if (agent_system::launcher::Background() && !agent_system::launcher::YieldToForeground())
            return;
#endif
        const auto error = agent_system::Launch(pending_launch_);
        if (error == ESP_OK) {
            launch_dispatched_ = true;
        } else {
            pending_launch_[0] = 0;
            Check(error);
            ++view_;
            Publish();
        }
    }
    void Apps() {
        page_ = Page::kApps;
        apps_page_ = std::min(apps_page_, app_count_ ? (app_count_ - 1) / 4 : size_t(0));
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
        seen_app_revision_ = agent_system::Revision();
    }
    void RefreshCover() {
        cover_ = {};
        if (!carousel_.IsAdd())
            (void)agent_system::ReadCover(apps_[carousel_.Index()].slot, &cover_);
    }
    bool ReadyToDownload() {
#ifdef AGENT_LAUNCHER_AB
        if (UpdateBlocksApps())
            return false;
#endif
        if (tile_home_) {
            agent_link_wifi_info_t wifi{};
            if (agent_link_wifi_info(&wifi) != ESP_OK || !wifi.connected) {
                menu_.OpenNetwork();
                ++navigation_;
                amoled_ui::Toast(wifi.setup_active
                    ? Tr("请先完成 WiFi 配网，再下载应用", "Set up WiFi before downloading apps")
                    : Tr("WiFi 未连接，请先检查网络", "Connect WiFi before downloading apps"));
                return false;
            }
        }
        return true;
    }
    void Catalog(unsigned page = 1) {
        if (store_job_) {
            const auto state = store_job_->Read();
            if (state.running) {
                page_ = Page::kJobProgress;
                ++navigation_;
                return;
            }
            if (state.result.state != FactoryStoreJob::State::kNone) {
                page_ = Page::kJobResult;
                ++navigation_;
                return;
            }
            page_ = Page::kDownloadCode;
            ++navigation_;
            return;
        }
        if (!ReadyToDownload())
            return;
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
            if (app.firmware_id      ? apps_[i].firmware_id == app.firmware_id
                : app.community_play ? apps_[i].community_play == app.community_play
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
            case Page::kDownloadCode: {
                const size_t length = strlen(download_code_);
                if (row <= 9 && length < 6) {
                    download_code_[length] = static_cast<char>('0' + row);
                    download_code_[length + 1] = 0;
                } else if (row == 10 && length)
                    download_code_[length - 1] = 0;
                else if (row == 11 && length == 6 && ReadyToDownload()) {
                    if (Check(store_job_->StartLookupCode(download_code_))) {
                        catalog_count_ = 0;
                        page_ = Page::kJobProgress;
                    }
                } else if (row == 12)
                    page_ = Page::kDownloadHelp;
                break;
            }
            case Page::kDownloadHelp:
                page_ = Page::kDownloadCode;
                break;
            case Page::kDashboard:
                if (row == 0) Apps();
                else if (row == 1) Catalog();
                else if (row == 2) menu_.Open();
                else if (row == 3) menu_.OpenNetwork();
                break;
            case Page::kApps:
                if (!app_count_ && row == 0) Catalog();
                else if (row < 4 && apps_page_ * 4 + row < app_count_) {
                    selected_ = apps_page_ * 4 + row;
                    page_ = Page::kLocalDetail;
                } else if (row == 4 && apps_page_) { --apps_page_; Apps(); }
                else if (row == 5 && (apps_page_ + 1) * 4 < app_count_) { ++apps_page_; Apps(); }
                break;
            case Page::kHome:
                if (row == 3 && tile_home_)
                    menu_.OpenNetwork();
                else if (row == 0) {
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
                if (row == 0) {
#ifdef AGENT_LAUNCHER_AB
                    if (UpdateBlocksApps())
                        break;
#endif
                    if (StoreRunning())
                        page_ = Page::kInterrupt;
                    else
                        RequestLaunch(apps_[selected_].slot);
                } else if (row == 1) {
#ifdef AGENT_LAUNCHER_AB
                    if (UpdateBlocksApps())
                        break;
#endif
                    if (StoreRunning() || NeedsRestart())
                        amoled_ui::Toast(
                            Tr("请先完成当前安装", "Finish the current installation first"));
                    else
                        page_ = Page::kRemove;
                }
                break;
            case Page::kInterrupt:
                if (row == 0)
                    page_ = Page::kLocalDetail;
                else if (row == 1) {
                    store_job_->Cancel();
                    RequestLaunch(apps_[selected_].slot);
                }
                break;
            case Page::kJobProgress:
                break;
            case Page::kJobResult: {
                const auto result = store_job_->Read().result;
                if (result.state != FactoryStoreJob::State::kRestart &&
                    result.state != FactoryStoreJob::State::kSuccess) {
                    store_job_->Acknowledge();
                    if (row == 0)
                        Catalog(result.page);
                    else
                        Home();
                }
                break;
            }
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
                    page_ = tile_home_ ? Page::kDownloadCode : Page::kCatalog;
                    break;
                }
                if (row == 0) {
                    if (Installed(catalog_[selected_])) {
                        Check(agent_system::kAlreadyInstalled);
                        break;
                    }
                    if (NeedsRestart()) {
                        page_ = Page::kJobResult;
                        break;
                    }
                    if (!ReadyToDownload())
                        break;
                    if (store_job_) {
                        if (StoreRunning()) {
                            page_ = Page::kJobProgress;
                            break;
                        }
                        if (Check(store_job_->StartInstall(catalog_[selected_])))
                            page_ = Page::kJobProgress;
                        else
                            page_ = Page::kJobResult;
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
                    page_ = tile_home_ ? Page::kDownloadCode : Page::kCatalog;
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
            if (page_ == Page::kJobProgress || page_ == Page::kJobResult) {
                RenderJob(row, title);
            } else if (page_ == Page::kInterrupt) {
                title(Tr("中断下载并启动？", "Interrupt download?"));
                snprintf(screen_.info, sizeof screen_.info, "%s",
                         Tr("启动其他应用会中断本次下载，下次需要重新下载。\n\n"
                            "确认后会先安全结束当前操作，再启动应用。",
                            "Opening another app interrupts this download. It must restart from "
                            "the beginning next time.\n\n"
                            "The current operation will stop safely before the app opens."));
                row(Tr("取消，继续下载", "Cancel, keep downloading"));
                row(Tr("确认中断并启动", "Interrupt and open"));
            } else if (page_ == Page::kDashboard) {
                screen_.kind = amoled_ui::View::kFactoryHome;
                screen_.card_count = app_count_;
                title("Sugarat Launcher");
                if (store_job_) {
                    const auto job = store_job_->Read();
                    if (job.running)
                        snprintf(screen_.job_status, sizeof screen_.job_status, "%s",
                                 Tr("后台下载中 · 点击下载查看", "Downloading - tap Download"));
                    else if (job.result.state != FactoryStoreJob::State::kNone)
                        snprintf(screen_.job_status, sizeof screen_.job_status, "%s",
                                 Tr("下载结果待查看 · 点击下载", "Result ready - tap Download"));
                }
#ifdef AGENT_LAUNCHER_AB
                const auto launcher_update = agent_system::launcher::Read();
                if (!screen_.job_status[0] && launcher_update.state == agent_system::launcher::State::kReady)
                    snprintf(screen_.job_status, sizeof screen_.job_status, "%s", launcher_update.forced
                        ? Tr("必需更新已就绪 · 下次重启应用", "Required update ready - next restart")
                        : Tr("启动器更新已就绪 · 设置中更新", "Launcher update ready - open Settings"));
#endif
                screen_.storage_total = agent_system::kAppImageMaxBytes + agent_system::kCoverReserveBytes;
                for (size_t i = 0; i < app_count_; ++i) {
                    const auto* slot = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                        ESP_PARTITION_SUBTYPE_ANY, apps_[i].slot);
                    if (slot) screen_.storage_used += slot->size;
                }
            } else if (page_ == Page::kApps) {
                screen_.kind = amoled_ui::View::kAppGrid;
                screen_.card_index = apps_page_;
                screen_.card_count = std::max<size_t>(1, (app_count_ + 3) / 4);
                title(Tr("我的应用", "My apps"));
                const size_t first = apps_page_ * 4;
                for (size_t i = first; i < app_count_ && i < first + 4; ++i) {
                    row(apps_[i].descriptor.name);
                    agent_system::Cover cover;
                    if (agent_system::ReadCover(apps_[i].slot, &cover) == ESP_OK)
                        screen_.grid[i - first] = {cover.slot, cover.revision};
                }
            } else if (page_ == Page::kHome) {
                screen_.kind = amoled_ui::View::kCarousel;
                screen_.card_index = carousel_.Index();
                screen_.card_count = carousel_.Pages();
                screen_.add_card = carousel_.IsAdd();
                const auto& app = apps_[carousel_.IsAdd() ? 0 : carousel_.Index()];
                title(screen_.add_card ? Tr("添加应用", "Add application")
                      : tile_home_ && !strcmp(app.descriptor.app_id, "agent.status")
                          ? "Agent_link"
                          : app.descriptor.name);
                if (!screen_.add_card) {
                    snprintf(screen_.app_id, sizeof screen_.app_id, "%s",
                             apps_[carousel_.Index()].descriptor.app_id);
                    screen_.cover_slot = cover_.slot;
                    screen_.cover_revision = cover_.revision;
                }
            } else if (page_ == Page::kDownloadCode) {
                screen_.kind = amoled_ui::View::kDownloadCode;
                snprintf(screen_.download_code, sizeof screen_.download_code, "%s", download_code_);
            } else if (page_ == Page::kDownloadHelp) {
                screen_.kind = amoled_ui::View::kDownloadHelp;
                title(Tr("下载码在哪里？", "Where is the code?"));
                snprintf(screen_.info, sizeof screen_.info, "%s", amoled_ui::DownloadCodeGuide());
                row(Tr("返回输入下载码", "Enter download code"));
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
                if (tile_home_) {
                    screen_.kind = amoled_ui::View::kDownloadDetail;
                    snprintf(screen_.detail, sizeof screen_.detail, "%s · %.2f MB\n%s %s",
                             app.version, app.size / 1048576.0,
                             Tr("下载码", "Code"), app.download_code);
                }
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
                    if (tile_home_)
                        snprintf(screen_.detail, sizeof screen_.detail, "%s",
                                 Tr("需要纯应用 .bin 或文件超过容量", "Requires an app .bin within capacity"));
                    row(tile_home_ ? Tr("不可安装", "Unavailable") : Tr("返回列表", "Back to list"));
                } else {
                    row(Installed(app)
                            ? Tr("已安装（更新前请先卸载）", "Installed (remove before updating)")
                            : Tr("下载安装", "Download and install"));
                    row(tile_home_ ? Tr("返回输入下载码", "Enter another code") : Tr("返回列表", "Back to list"));
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
                title(tile_home_ && !strcmp(app.descriptor.app_id, "agent.status")
                          ? "Agent_link"
                          : app.descriptor.name);
                screen_.kind = amoled_ui::View::kAppActions;
                screen_.card_index = carousel_.Index();
                screen_.card_count = carousel_.Pages();
                snprintf(screen_.app_id, sizeof screen_.app_id, "%s", app.descriptor.app_id);
                snprintf(screen_.detail, sizeof screen_.detail, "%s",
                         app.release_version[0] ? app.release_version : app.version);
                agent_system::Cover detail_cover;
                (void)agent_system::ReadCover(app.slot, &detail_cover);
                screen_.cover_slot = detail_cover.slot;
                screen_.cover_revision = detail_cover.revision;
            } else {
                const auto& app = apps_[selected_];
                title(page_ == Page::kRemove ? Tr("确认卸载？", "Remove this app?")
                                             : app.descriptor.name);
                snprintf(screen_.info, sizeof screen_.info, "%s\n%s  %s\n%lu KB",
                         app.descriptor.app_id, app.version, app.slot,
                         static_cast<unsigned long>(app.size / 1024));
                if (page_ == Page::kRemove) {
                    if (tile_home_) {
                        screen_.kind = amoled_ui::View::kAppRemove;
                        snprintf(screen_.detail, sizeof screen_.detail, "%s",
                                 !strcmp(app.descriptor.app_id, "agent.status")
                                     ? "Agent_link"
                                     : app.descriptor.name);
                        snprintf(screen_.app_id, sizeof screen_.app_id, "%s",
                                 app.descriptor.app_id);
                        agent_system::Cover cover;
                        (void)agent_system::ReadCover(app.slot, &cover);
                        screen_.cover_slot = cover.slot;
                        screen_.cover_revision = cover.revision;
                    }
                    row(Tr("取消", "Cancel"));
                    row(Tr("确认卸载（保留设备设置）", "Remove (keep device settings)"));
                } else {
                    row(Tr("启动应用", "Launch application"));
                    row(Tr("卸载应用", "Remove application"));
                }
            }
        }
        if (pending_launch_[0] && tile_home_) {
            screen_.kind = amoled_ui::View::kAppActions;
            screen_.launching = screen_.busy = true;
            screen_.cover_slot = cover_.slot;
            screen_.cover_revision = cover_.revision;
            if (selected_ < app_count_) {
                snprintf(screen_.title, sizeof screen_.title, "%s", apps_[selected_].descriptor.name);
                snprintf(screen_.app_id, sizeof screen_.app_id, "%s", apps_[selected_].descriptor.app_id);
            }
        }
        if (tile_home_) screen_.launcher_shell = true;
        if (hardware_services_)
            hardware_services_->Decorate(screen_);
        amoled_ui::SetScreen(screen_);
    }
    static const char* InstallStageName(agent_system::InstallStage stage) {
        using S = agent_system::InstallStage;
        switch (stage) {
            case S::kWaitCover:
                return Tr("等待封面任务结束", "Waiting for cover sync");
            case S::kInspect:
                return Tr("检查应用与空间", "Checking apps and space");
            case S::kTicket:
                return Tr("获取下载信息", "Getting download details");
            case S::kConnect:
                return Tr("连接下载服务器", "Connecting to server");
            case S::kHeader:
                return Tr("检查固件格式", "Checking image identity");
            case S::kAllocate:
                return Tr("分配应用空间", "Allocating app space");
            case S::kErase:
                return Tr("准备设备存储", "Preparing storage");
            case S::kDownload:
                return Tr("下载并写入", "Downloading and writing");
            case S::kVerify:
                return Tr("校验固件", "Verifying firmware");
            case S::kSave:
                return Tr("保存安装记录", "Saving app record");
            case S::kPublish:
                return Tr("完成应用布局", "Publishing app layout");
            case S::kDone:
                return Tr("安装完成", "Installed");
        }
        return "-";
    }
    static const char* InstallReason(esp_err_t error, agent_system::InstallStage stage) {
        if (error == agent_system::kCertificateError)
            return Tr("下载服务器证书校验失败，请更新启动器或检查设备时间后重试。",
                      "Server certificate verification failed. Update the Launcher or check the "
                      "device time.");
        if (error == agent_system::kCancelled)
            return Tr("下载已中断，下次需从头下载。",
                      "Download interrupted. Next attempt starts over.");
        if (error == ESP_ERR_INVALID_VERSION)
            return Tr("固件与设备不兼容，或文件是启动器／整机包。",
                      "Incompatible image, launcher or full-device package.");
        if (error == ESP_ERR_INVALID_SIZE)
            return Tr("文件大小与目录不一致，或超过可用空间。",
                      "File size differs from the catalog or available space.");
        if (error == ESP_ERR_INVALID_CRC)
            return Tr("文件不完整或校验失败，请重新下载。",
                      "Incomplete file or checksum mismatch. Download again.");
        if (error == ESP_ERR_TIMEOUT) {
            if (stage == agent_system::InstallStage::kTicket)
                return Tr("查询详情或申请下载链接超时，请稍后重试。",
                          "Timed out checking details or requesting a download link. Try later.");
            if (stage == agent_system::InstallStage::kConnect)
                return Tr("等待固件服务器响应超时，请稍后重试。",
                          "Timed out waiting for the firmware server. Try again later.");
            return Tr("读取服务器数据超时，请检查网络后重试。",
                      "Timed out reading server data. Check the network and retry.");
        }
        if (error == ESP_ERR_INVALID_RESPONSE)
            return Tr("网络连接中断、超时或服务器响应不完整。",
                      "Connection interrupted, timed out or response incomplete.");
        if (error == ESP_ERR_NO_MEM)
            return Tr("可用内存不足，请稍后重试。", "Not enough memory. Try again later.");
        if (error == agent_system::kNoAppSpace || error == agent_system::kSlotsFull ||
            error == agent_system::kLegacySlotsFull)
            return Tr("应用空间不足，请先卸载其他应用。",
                      "Not enough app space. Remove another app first.");
        if (error == agent_system::kNeedsFullPackage)
            return Tr("需要先通过 USB 更新完整启动器固件。",
                      "Update the full launcher package over USB first.");
        if (error == agent_system::kAlreadyInstalled)
            return Tr("此应用已经安装，更新前请先卸载。",
                      "App already installed. Remove it before updating.");
        if (error == agent_system::kUnsupportedPackage)
            return Tr("需要纯应用 .bin，不能安装整机包。", "A pure application .bin is required.");
        if (error == ESP_ERR_INVALID_STATE)
            return Tr("操作未完成，可能已被重启中断，请重新选择应用。",
                      "Operation incomplete or interrupted by restart. Select the app again.");
        if (stage == agent_system::InstallStage::kErase)
            return Tr("无法擦除目标应用分区，请检查设备存储。",
                      "Unable to erase the target app partition.");
        if (stage == agent_system::InstallStage::kSave)
            return Tr("无法保存安装记录，请检查设备存储。", "Unable to save installation records.");
        if (stage == agent_system::InstallStage::kDownload)
            return Tr("下载或写入存储失败，请重新下载。",
                      "Download or flash write failed. Download again.");
        if (stage == agent_system::InstallStage::kVerify)
            return Tr("固件校验失败，文件损坏或格式不受支持。",
                      "Image verification failed: damaged or unsupported image.");
        return Tr("操作失败，请根据下方阶段和错误码重试。",
                  "Operation failed. Check the stage and error code below.");
    }
    template <class Row, class Title>
    void RenderJob(const Row& row, const Title& title) {
        using J = FactoryStoreJob;
        const auto job = store_job_->Read();
        const auto& result = job.result;
        const auto& status = result.status;
        if (job.running) {
            screen_.kind = amoled_ui::View::kInfo;
            screen_.busy = true;
            screen_.progress =
                status.total && status.received
                    ? static_cast<int>(uint64_t(status.received) * 100 / status.total)
                    : -1;
            title(pending_launch_[0]                 ? Tr("正在结束下载…", "Stopping download...")
                  : result.kind == J::Kind::kLookupCode ? Tr("查询下载码", "Looking up code")
                  : result.kind == J::Kind::kCatalog ? Tr("获取在线应用", "Loading online apps")
                                                     : InstallStageName(status.stage));
            snprintf(screen_.detail, sizeof screen_.detail, "%s", result.name);
            const auto rate =
                status.elapsed_ms ? uint64_t(status.received) * 1000 / status.elapsed_ms / 1024 : 0;
            snprintf(screen_.info, sizeof screen_.info, "%s\n%lu / %lu KB · %llu KB/s", result.name,
                     static_cast<unsigned long>(status.received / 1024),
                     static_cast<unsigned long>(status.total / 1024),
                     static_cast<unsigned long long>(rate));
            return;
        }
        title(result.state == J::State::kRestart     ? Tr("应用已写入", "App written")
              : result.state == J::State::kSuccess   ? Tr("安装完成", "Installed")
              : result.state == J::State::kCancelled ? Tr("下载已中断", "Download interrupted")
              : result.kind == J::Kind::kLookupCode  ? Tr("下载码查询失败", "Code lookup failed")
              : result.kind == J::Kind::kCatalog     ? Tr("应用列表加载失败", "Unable to load apps")
                                                 : Tr("下载安装失败", "Download/install failed"));
        if (result.state == J::State::kRestart || result.state == J::State::kSuccess) {
            screen_.kind = amoled_ui::View::kInfo;
            screen_.busy = true;
            snprintf(screen_.info, sizeof screen_.info,
                     Tr("%s\n安装完成，正在自动重启回启动器。",
                        "%s\nInstalled. Restarting into the Launcher."),
                     result.name);
        } else {
            char header[320];
            snprintf(header, sizeof header, "%s\n%s\n%s: %s\n%s (0x%x)\n%lu / %lu KB\n",
                     result.name, InstallReason(status.error, status.stage), Tr("阶段", "Stage"),
                     InstallStageName(status.stage), esp_err_to_name(status.error),
                     unsigned(status.error), static_cast<unsigned long>(status.received / 1024),
                     static_cast<unsigned long>(status.total / 1024));
            std::string text = header;
            if (status.error == agent_system::kStoreError) {
                char server[192];
                snprintf(server, sizeof server, "HTTP %d %.47s\nrequestId: %.79s\n",
                         result.store.http_status, result.store.code, result.store.request_id);
                text = std::string(result.name) + "\n" + InstallStageName(status.stage) + "\n" +
                       server + result.store.message + "\n" + esp_err_to_name(status.error);
            } else if (status.http_status) {
                text += "HTTP " + std::to_string(status.http_status);
            }
            if (status.network_errno)
                text += " errno=" + std::to_string(status.network_errno);
            size_t length = std::min(text.size(), sizeof screen_.info - 1);
            while (length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
                --length;
            memcpy(screen_.info, text.data(), length);
            screen_.info[length] = 0;
            row(tile_home_ ? Tr("返回输入下载码", "Enter download code") : Tr("返回下载列表", "Back to downloads"));
            row(Tr("知道了，返回主页", "Done, go home"));
        }
    }
    void PollStoreJob() {
        if (!store_job_)
            return;
        if (store_job_->TakeRestartRequest()) {
            pending_launch_[0] = 0;
            ESP_LOGI("factory.store", "Installation complete; restarting into Launcher");
            Busy(Tr("安装完成，正在返回启动器", "Installed. Restarting Launcher"));
            vTaskDelay(pdMS_TO_TICKS(150));
            esp_restart();
            return;
        }
        const auto job = store_job_->Read();
        if (job.revision == seen_job_revision_)
            return;
        seen_job_revision_ = job.revision;
        if (!job.running && job.result.state != FactoryStoreJob::State::kNone) {
            if (job.result.kind != FactoryStoreJob::Kind::kInstall &&
                job.result.state == FactoryStoreJob::State::kSuccess) {
                store_job_->CopyCatalog(catalog_, &catalog_count_, &catalog_page_);
                store_job_->Acknowledge();
                if (page_ == Page::kJobProgress) {
                    selected_ = 0;
                    page_ = job.result.kind == FactoryStoreJob::Kind::kLookupCode
                                ? Page::kCatalogDetail : Page::kCatalog;
                }
                retry_requested_ = true;
            } else {
                if (job.result.state == FactoryStoreJob::State::kSuccess)
                    RefreshLocal();
                if (page_ == Page::kJobProgress)
                    page_ = Page::kJobResult;
            }
            ++view_;
            ++navigation_;
        }
        Publish();
    }
    void ControlLoop() {
        amoled_ui::ButtonDebounce button;
        int64_t next_update = 0;
#ifdef AGENT_LAUNCHER_AB
        bool deferred = false;
        amoled_ui::Input deferred_input{};
        uint32_t deferred_navigation = 0;
#endif
        while (true) {
#ifdef AGENT_LAUNCHER_AB
            if (!launcher_confirmed_) {
                if (!amoled_ui::UiResponsive() || !hardware_.panel_.Ready() ||
                    !hardware_.touch_.Ready())
                    healthy_since_ = 0;
                else if (!healthy_since_)
                    healthy_since_ = esp_timer_get_time();
                else if (esp_timer_get_time() - healthy_since_ > 10000000 &&
                         agent_system::launcher::ConfirmHealthy() == ESP_OK)
                    launcher_confirmed_ = true;
            }
            agent_link_wifi_info_t update_wifi{};
            (void)agent_link_wifi_info(&update_wifi);
            agent_system::launcher::Tick(
                update_wifi.connected,
                launcher_confirmed_ && !deferred && !StoreInstalling() && !NeedsRestart() &&
                    !sync_busy_.load() && !pending_launch_[0] &&
                    (!hardware_services_ || !hardware_services_->HighLoad()));
            const auto update = agent_system::launcher::Read();
            if (update.revision != update_revision_) {
                update_revision_ = update.revision;
                if (!update.background)
                    ++view_;
                Publish();
            }
            if (launcher_confirmed_ && !update.forced &&
                update.state == agent_system::launcher::State::kReady &&
                announced_update_ != update.version_code) {
                announced_update_ = update.version_code;
                amoled_ui::Toast(
                    Tr("启动器更新包已下载，请到设置 > 固件更新确认重启更新",
                       "Launcher update downloaded. Open Settings > Firmware update to restart."),
                    8000);
            }
            if (pending_update_check_) {
                agent_link_wifi_info_t wifi{};
                if (agent_link_wifi_info(&wifi) == ESP_OK && wifi.connected) {
                    pending_update_check_ = false;
                    menu_.OpenUpdate();
                    ++view_;
                    ++navigation_;
                    Publish();
                }
            }
#endif
            PollStoreJob();
            PollLaunch();
            amoled_ui::Input input;
            bool received = xQueueReceive(events_, &input, pdMS_TO_TICKS(30)) == pdTRUE;
#ifdef AGENT_LAUNCHER_AB
            if (deferred) {
                // The prior event already passed view/timestamp validation. Keep
                // its target only while navigation has stayed on the same page.
                if (received)
                    (void)xQueueSend(events_, &input, 0);
                input = deferred_input;
                input.at = esp_timer_get_time();
                input.view = view_;
                received = deferred_navigation == navigation_;
                deferred = false;
            }
#endif
            bool fresh = received && amoled_ui::CurrentInput(input, view_, esp_timer_get_time());
#ifdef AGENT_LAUNCHER_AB
            if (fresh && StartsHighLoad(input) && agent_system::launcher::Background() &&
                !agent_system::launcher::YieldToForeground()) {
                deferred_input = input;
                deferred_navigation = navigation_;
                deferred = true;
                received = fresh = false;
            }
#endif
            // Drain the slider before navigation so a release is saved even
            // when Back/Home arrives in the same controller iteration.
            int brightness, saved_brightness;
            if (brightness_requests_.Take(brightness, saved_brightness)) {
                amoled_ui::Input level{};
                level.action = amoled_ui::TouchAction::kBrightness;
                level.row = saved_brightness >= 0 ? saved_brightness : brightness;
                level.commit = saved_brightness >= 0;
                menu_.Handle(level);
                Publish();
            }
            if (received && !fresh && input.view == view_) {
                ++view_;
                Publish();
            }
            if (pending_launch_[0]) received = fresh = false;
            if (fresh) {
                ++view_;
                ++navigation_;
                if (input.action == amoled_ui::TouchAction::kHome) {
                    menu_.Close();
                    Home();
                } else if (input.action == amoled_ui::TouchAction::kSystem)
                    menu_.Open();
                else if (menu_.Active())
                    menu_.Handle(input);
                else if (input.action == amoled_ui::TouchAction::kRow)
                    Select(input.row);
                else if (input.action == amoled_ui::TouchAction::kHome)
                    Home();
                else if (input.action == amoled_ui::TouchAction::kBack) {
                    Back();
                } else if (page_ == Page::kDashboard && input.action == amoled_ui::TouchAction::kNextCard) {
                    Apps();
                } else if (page_ == Page::kHome &&
                           (input.action == amoled_ui::TouchAction::kPreviousCard ||
                            input.action == amoled_ui::TouchAction::kNextCard)) {
                    if (carousel_.Move(input.action == amoled_ui::TouchAction::kNextCard ? 1 : -1,
                                       tile_home_))
                        RefreshCover();
                }
                if (menu_.TakeExit()) {
                    amoled_ui::Toast(Tr("当前已在启动器", "Already in Launcher"));
                }
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
            if (!hardware_services_ && button.Update(hardware_.ButtonDown(), esp_timer_get_time())) {
                menu_.Close();
                Home();
                ++view_;
                ++navigation_;
                Publish();
            }
            if (hardware_services_) {
                if (hardware_services_->Button(hardware_.ButtonDown(), esp_timer_get_time())) {
                    ShortPowerPress();
                }
                const bool hardware_available =
#ifdef AGENT_LAUNCHER_AB
                    !hardware_services_->HighLoad() || !agent_system::launcher::Background() ||
                    agent_system::launcher::YieldToForeground();
#else
                    true;
#endif
                if (hardware_available && hardware_services_->Tick(
                        !pending_launch_[0] && !sync_busy_.load() && !StoreRunning()
#ifdef AGENT_LAUNCHER_AB
                        && (!agent_system::launcher::Busy() || agent_system::launcher::Background())
#endif
                            )) {
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
#ifdef AGENT_LAUNCHER_AB
        if (!launcher_confirmed_ ||
            (agent_system::launcher::Busy() && !agent_system::launcher::Background()))
            return;
#endif
        if (pending_launch_[0]) return;
        // No new cover work or cached-partition scans while an install owns flash,
        // or after layout publication until IDF reloads the partition table.
        if (StoreRunning() || NeedsRestart())
            return;
        const uint32_t app_revision = agent_system::Revision();
        if (app_revision != seen_app_revision_) {
            seen_app_revision_ = app_revision;
            RefreshLocal();
            if (page_ != Page::kJobResult && page_ != Page::kInterrupt)
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
#ifdef AGENT_LAUNCHER_AB
            if (agent_system::launcher::Background() &&
                !agent_system::launcher::YieldToForeground())
                return;
#endif
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
        self->screen_.progress = total ? static_cast<int>(static_cast<uint64_t>(received) * 100 / total) : -1;
        snprintf(self->screen_.info, sizeof self->screen_.info, "%lu%%  (%lu / %lu KB)",
                 static_cast<unsigned long>(std::max(0, self->screen_.progress)),
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
        if (input.action == amoled_ui::TouchAction::kBrightness) {
            self->brightness_requests_.Submit(input.row, input.commit);
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
    const bool tile_home_;
#ifdef AGENT_LAUNCHER_AB
    bool UpdateBlocksApps() {
        if (launcher_confirmed_ &&
            (!agent_system::launcher::Busy() || agent_system::launcher::Background()))
            return false;
        amoled_ui::Toast(Tr("启动器正在验证或更新，请完成更新或取消后再操作应用",
                            "Finish or cancel the Launcher update before using apps"),
                         5000);
        return true;
    }
    bool HandleUpdate(const amoled_ui::Input& input) {
        using namespace agent_system::launcher;
        const bool check_only = input.action == amoled_ui::TouchAction::kMenu;
        if (input.action != amoled_ui::TouchAction::kRow && !check_only)
            return true;
        if (!check_only && input.row == 0) {
            Check(SetAutoDownload(!AutoDownloadEnabled()));
            return true;
        }
        const auto value = Read();
        if (agent_system::launcher::Busy()) {
            if (!check_only) {
                if (Background() && !value.forced)
                    Check(SetAutoDownload(false));
                else {
                    Check(SetAutoDownload(false));
                    Cancel();
                }
            }
            return true;
        }
        if (value.state == State::kReady) {
            if (!check_only) {
                if (StoreRunning() || sync_busy_.load() || NeedsRestart()) {
                    amoled_ui::Toast(Tr("请等待当前任务完成后再更新",
                                        "Wait for the current task before updating"));
                } else
                    Check(Restart());
            }
            return true;
        }
        if (!check_only && (value.state == State::kAvailable || value.state == State::kPaused) &&
            input.row == 1) {
            if (StoreRunning() || NeedsRestart() || sync_busy_.load() || !launcher_confirmed_) {
                amoled_ui::Toast(Tr("请等待当前任务完成后再更新启动器",
                                    "Wait for the current task before updating"));
                return true;
            }
            Check(Install());
            return true;
        }
        agent_link_wifi_info_t wifi{};
        if (agent_link_wifi_info(&wifi) != ESP_OK || !wifi.connected) {
            menu_.OpenNetwork();
            amoled_ui::Toast(
                Tr("请先联网，再检查更新", "Connect WiFi before checking for updates"));
        } else
            Check(agent_system::launcher::Check());
        return true;
    }
    void RenderUpdate(amoled_ui::Screen& screen) {
        using namespace agent_system::launcher;
        const auto value = Read();
        screen.row_count = 0;
        screen.toggle_rows = 1;
        screen.checked_rows = AutoDownloadEnabled() ? 1 : 0;
        screen.kind = amoled_ui::View::kList;
        snprintf(screen.title, sizeof screen.title, "%s", Tr("固件更新", "Firmware update"));
        snprintf(screen.detail, sizeof screen.detail, "%s (%lu)", value.current,
                 (unsigned long)value.current_code);
        snprintf(screen.footer, sizeof screen.footer, "%s",
                 Tr("底部上滑回主页", "Swipe up from bottom: home"));
        if (value.state == State::kAvailable)
            snprintf(
                screen.info, sizeof screen.info,
                Tr("新版本：%s\n大小：%lu KB%s\n\n%.220s", "New: %s\nSize: %lu KB%s\n\n%.220s"),
                value.version, (unsigned long)((value.size + 1023) / 1024),
                value.forced ? Tr(" · 必需更新", " - Required update") : "", value.changelog);
        else if (agent_system::launcher::Busy() || value.state == State::kPaused)
            snprintf(screen.info, sizeof screen.info, "%.260s\n\n%lu / %lu KB · %lu%%\n%lu KB/s",
                     value.message, (unsigned long)(value.received / 1024),
                     (unsigned long)(value.size / 1024),
                     (unsigned long)(value.size ? uint64_t(value.received) * 100 / value.size : 0),
                     (unsigned long)(value.elapsed_ms
                                         ? uint64_t(value.received) * 1000 / value.elapsed_ms / 1024
                                         : 0));
        else
            snprintf(screen.info, sizeof screen.info, "%.290s%s", value.message,
                     value.state == State::kError
                         ? Tr("\n\n旧启动器保持可用，可重新检查更新。",
                              "\n\nThe previous Launcher is available. You can retry.")
                         : "");
        const auto row = [&](const char* label) {
            snprintf(screen.rows[screen.row_count++], sizeof screen.rows[0], "%s", label);
        };
        row(Tr("后台自动下载", "Auto download"));
        if (agent_system::launcher::Busy())
            row(Background() ? Tr("暂停后台下载", "Pause background download")
                             : Tr("取消更新", "Cancel update"));
        else if (value.state == State::kReady)
            row(Tr("重启并试运行新版", "Restart into new version"));
        else {
            if (value.state == State::kAvailable || value.state == State::kPaused)
                row(value.resumable ? Tr("继续下载更新", "Resume download")
                                    : Tr("下载更新包", "Download update"));
            row(Tr("检查更新", "Check for updates"));
        }
    }
    bool launcher_confirmed_ = false;
    bool pending_update_check_ = false;
    int64_t healthy_since_ = 0;
    uint32_t update_revision_ = 0, announced_update_ = 0;
#endif
    std::unique_ptr<FactoryStoreJob> store_job_;
    uint32_t seen_job_revision_ = 0;
    char pending_launch_[17]{};
    bool launch_dispatched_ = false;
    int64_t launch_started_ = 0;
    uint32_t launch_view_ = 0;
    char download_code_[7]{};
    size_t apps_page_ = 0;
    AmoledHardware hardware_;
    std::unique_ptr<FactoryHardwareServices> hardware_services_;
    QueueHandle_t events_ = nullptr;
    AmoledSystemMenu menu_;
    AmoledLauncherModel carousel_;
    agent_system::Cover cover_;
    amoled_ui::VolumeMailbox volume_;
    amoled_ui::VolumeMailbox brightness_requests_;
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

