#include "factory_store_job_stubs.h"
#define AGENT_SYSTEM_LAYOUT 3
#define AGENT_SYSTEM_METADATA_PARTITION "app_meta"
#define AGENT_SYSTEM_BOARD_MODEL "SDGOODS-ESP32S3"
#include "../factory/common/factory_store_job.h"
#include <cassert>
#include <functional>

std::atomic<bool> hold_cover{false}, entered_cover{false}, hold_install{false},
    entered_publish{false};
std::atomic<int> install_calls{0};
esp_err_t installed_result = agent_system::kCancelled;
agent_system::InstallStage failed_stage = agent_system::InstallStage::kDownload;
void Until(const std::function<bool()>& predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
namespace agent_system {
esp_err_t FetchDownloadCode(const char* code, CatalogApp* app) {
    assert(!strcmp(code, "000001"));
    *app = {};
    snprintf(app->download_code, sizeof app->download_code, "%s", code);
    snprintf(app->name, sizeof app->name, "Code app");
    return ESP_OK;
}
void LastStoreError(StoreError* out) {
    out->http_status = 403;
    snprintf(out->message, sizeof out->message, "Download link expired");
    snprintf(out->code, sizeof out->code, "FILE_EXPIRED");
}
esp_err_t FetchCatalogPage(CatalogApp* apps, size_t, size_t* count, unsigned page,
                           CatalogPage* info) {
    *count = 1;
    apps[0] = {};
    snprintf(apps[0].name, sizeof apps[0].name, "Catalog app");
    *info = {page, 1, false};
    return ESP_OK;
}
esp_err_t Install(const CatalogApp& app, Progress, void* context, InstallStatus* out,
                  InstallObserver observer, InstallCancel cancel) {
    ++install_calls;
    InstallStatus status{};
    status.total = app.size;
    status.stage = failed_stage;
    status.received = 512;
    status.http_status = 200;
    snprintf(status.app_id, sizeof status.app_id, "vendor.test");
    snprintf(status.slot, sizeof status.slot, "ota_2");
    observer(status, context);
    if (status.stage == InstallStage::kPublish)
        entered_publish.store(true);
    Until([&] {
        return !hold_install.load() || (status.stage != InstallStage::kPublish && cancel(context));
    });
    status.error =
        cancel(context) && status.stage != InstallStage::kPublish ? kCancelled : installed_result;
    if (status.error == kStoreError)
        status.http_status = 403;
    *out = status;
    observer(status, context);
    return status.error;
}
}  // namespace agent_system
int main() {
    using Job = FactoryStoreJob;
    using namespace agent_system;
    Job job;
    assert(job.Init(
               [](void*) {
                   entered_cover.store(true);
                   Until([] { return !hold_cover.load(); });
               },
               nullptr) == ESP_OK);
    CatalogApp app{};
    app.size = 1024;
    app.community_play = 7;
    app.community_version = 9;
    snprintf(app.name, sizeof app.name, "Background app");
    snprintf(app.version, sizeof app.version, "1.0");
    hold_cover.store(true);
    assert(job.StartInstall(app) == ESP_OK);
    Until([] { return entered_cover.load(); });
    assert(job.Read().running && job.StartCatalog(1) == ESP_ERR_INVALID_STATE);
    job.Cancel();
    hold_cover.store(false);
    Until([&] { return !job.Read().running; });
    assert(install_calls.load() == 0 && job.Read().result.state == Job::State::kCancelled);
    Job restored;
    assert(restored.Restore(nullptr, 0) && restored.Read().result.state == Job::State::kCancelled);
    job.Acknowledge();
    Job cleared;
    assert(!cleared.Restore(nullptr, 0));

    hold_install.store(true);
    failed_stage = InstallStage::kDownload;
    assert(job.StartInstall(app) == ESP_OK);
    Until([&] { return job.Read().result.status.received == 512; });
    assert(job.Read().running);
    job.Cancel();
    Until([&] { return !job.Read().running; });
    assert(job.Read().result.state == Job::State::kCancelled &&
           job.Read().result.status.received == 512);
    hold_install.store(false);
    job.Acknowledge();

    installed_result = kStoreError;
    failed_stage = InstallStage::kConnect;
    assert(job.StartInstall(app) == ESP_OK);
    Until([&] { return !job.Read().running; });
    Job failed;
    assert(failed.Restore(nullptr, 0));
    assert(failed.Read().result.state == Job::State::kFailed &&
           failed.Read().result.status.stage == InstallStage::kConnect &&
           failed.Read().result.store.http_status == 403 &&
           !strcmp(failed.Read().result.store.code, "FILE_EXPIRED"));
    job.Acknowledge();

    // The durable publication checkpoint exists before the atomic SDK commit.
    hold_install.store(true);
    installed_result = kRestartRequired;
    failed_stage = InstallStage::kPublish;
    assert(job.StartInstall(app) == ESP_OK);
    Until([] { return entered_publish.load(); });
    Job interrupted;
    assert(interrupted.Restore(nullptr, 0));
    assert(interrupted.Read().result.state == Job::State::kFailed);
    job.Cancel();
    assert(job.Read().running);
    hold_install.store(false);
    Until([&] { return !job.Read().running; });
    assert(job.Read().result.state == Job::State::kRestart);
    App installed{};
    installed.community_play = 7;
    installed.community_version = 9;
    Job confirmed;
    assert(confirmed.Restore(&installed, 1));
    assert(confirmed.Read().result.state == Job::State::kSuccess);
    job.Acknowledge();

    assert(job.StartCatalog(2) == ESP_OK);
    Until([&] { return !job.Read().running; });
    CatalogApp apps[kMaxApps];
    size_t count = 0;
    CatalogPage page;
    job.CopyCatalog(apps, &count, &page);
    assert(count == 1 && page.page == 2 && !strcmp(apps[0].name, "Catalog app"));
    job.Acknowledge();
    assert(job.StartLookupCode("12345") == ESP_ERR_INVALID_ARG);
    assert(job.StartLookupCode("12/456") == ESP_ERR_INVALID_ARG);
    hold_cover.store(true);
    assert(job.StartLookupCode("000001") == ESP_OK);
    assert(job.StartLookupCode("000001") == ESP_ERR_INVALID_STATE);
    hold_cover.store(false);
    Until([&] { return !job.Read().running; });
    job.CopyCatalog(apps, &count, &page);
    assert(count == 1 && !strcmp(apps[0].download_code, "000001") &&
           job.Read().result.kind == Job::Kind::kLookupCode);
    StopFactoryTasks();
    puts(
        "Factory worker: nonblocking navigation, busy rejection, safe cancellation, durable "
        "errors, interrupted publication and post-reboot verification passed.");
}
