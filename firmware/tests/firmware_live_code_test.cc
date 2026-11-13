#include "agent_system_dynamic_stubs.h"
#include "dynamic_layout_config.h"
#include "agent_system.h"
#include "../components/agent_system/community_store.h"
#include "cJSON.h"
#include <filesystem>
#include <fstream>

namespace agent_system::community {
esp_err_t FetchCover(const CatalogApp&, uint8_t*) {
    return ESP_ERR_NOT_SUPPORTED;
}
}  // namespace agent_system::community
namespace {
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
const char* Text(cJSON* value, const char* key) {
    auto* field = cJSON_GetObjectItemCaseSensitive(value, key);
    assert(cJSON_IsString(field));
    return field->valuestring;
}
void Put(uint32_t offset, const std::string& bytes) {
    assert(offset + bytes.size() <= dynamic_fixture::flash.size());
    memcpy(dynamic_fixture::flash.data() + offset, bytes.data(), bytes.size());
}
void Reboot() {
    uint8_t scratch[4096];
    auto io = dynamic_fixture::Io();
    assert(alpt_restore(&alpt_config, &io, scratch) >= 0);
    dynamic_fixture::Reload();
    assert(agent_system::Init() == ESP_OK);
}
size_t Count() {
    agent_system::App apps[14];
    size_t count = 0;
    assert(agent_system::List(apps, 14, &count) == ESP_OK);
    return count;
}
}  // namespace
int main(int argc, char** argv) {
    using namespace agent_system;
    assert(argc == 3);
    const auto artifacts = std::filesystem::u8path(argv[1]) / "factory";
    const auto snapshot = std::filesystem::u8path(argv[2]);
    const auto detail = Read(snapshot);
    auto* root = cJSON_Parse(detail.c_str());
    assert(root);
    auto* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    auto* firmware = cJSON_GetObjectItemCaseSensitive(data, "firmware");
    const std::string code = Text(data, "download_code");
    const std::string lookup =
        "https://bot.deotaland.cn/api/firmware/code/" + code + "/download-url";
    const auto binary = Read(snapshot.parent_path() / "application.bin");
    const std::string url = Text(data, "download_url");
    fixture::responses[lookup] = detail;
    fixture::responses[url] = binary;
    Put(0, Read(artifacts / "bootloader/bootloader.bin"));
    Put(0x8000, Read(artifacts / "partition_table/partition-table.bin"));
    Put(0x10000, Read(artifacts / "recovery/SDGOODS_LAUNCHER.bin"));
    Put(ALBOOT_A_OFFSET, Read(artifacts / "agent_link.bin"));
    dynamic_fixture::Reload();
    assert(Init() == kRestartRequired);
    Reboot();
    const auto before = dynamic_fixture::flash;
    CatalogApp app;
    assert(FetchDownloadCode(code.c_str(), &app) == ESP_OK);
    assert(app.package == CatalogApp::Package::kApplication && app.size == binary.size());
    assert(app.firmware_id == 19 && app.firmware_version_code == 1 && !app.sha256[0]);
    fixture::header_results[lookup] = {-ESP_ERR_HTTP_EAGAIN, -ESP_ERR_HTTP_EAGAIN,
                                       -ESP_ERR_HTTP_EAGAIN};
    InstallStatus status;
    assert(Install(app, nullptr, nullptr, &status) == ESP_ERR_TIMEOUT);
    assert(status.stage == InstallStage::kTicket && dynamic_fixture::flash == before);
    fixture::header_results.clear();
    fixture::open_errors[url] = ESP_ERR_HTTP_CONNECT;
    fixture::tls_errors[url] = {MBEDTLS_ERR_X509_FATAL_ERROR, 8};
    const auto requests = fixture::requests.size();
    assert(Install(app, nullptr, nullptr, &status) == kCertificateError);
    assert(status.stage == InstallStage::kConnect && status.received == 0 &&
           dynamic_fixture::flash == before);
    assert(fixture::requests.size() == requests + 2);  // one refresh, one file attempt
    StoreError certificate_error;
    LastStoreError(&certificate_error);
    assert(!strcmp(certificate_error.code, "TLS_CERT_VERIFY_FAILED"));
    fixture::open_errors.clear();
    fixture::responses[url][700] ^= 1;
    assert(Install(app) == ESP_ERR_INVALID_CRC && Count() == 0);
    fixture::responses[url] = binary;
    const auto result = Install(app, nullptr, nullptr, &status);
    if (result != kRestartRequired) {
        StoreError error;
        LastStoreError(&error);
        printf("Install error=%d stage=%d HTTP=%d message=%s\n", result, int(status.stage),
               error.http_status, error.message);
    }
    assert(result == kRestartRequired);
    Reboot();
    App installed[14];
    size_t count = 0;
    assert(List(installed, 14, &count) == ESP_OK && count == 1);
    assert(installed[0].firmware_id == 19 && installed[0].firmware_version_code == 1);
    assert(!strcmp(installed[0].download_code, code.c_str()));
    assert(!strcmp(installed[0].release_version, Text(firmware, "version")));
    assert(!strcmp(installed[0].descriptor.app_id, "agent.status"));
    assert(!strcmp(installed[0].version, "2.0.0"));
    const auto* part = esp_partition_find_first(0, 16, "app_00");
    assert(part);
    assert(part->address == alpt_config.pool_start);
    assert(!memcmp(dynamic_fixture::flash.data() + part->address, binary.data(), binary.size()));
    for (const auto& fixed : dynamic_fixture::partitions) {
        if (!strcmp(fixed.label, "otadata") || !strcmp(fixed.label, "app_meta") ||
            !strcmp(fixed.label, "layout_journal") ||
            (fixed.type == 0 && fixed.subtype >= 16 && fixed.subtype < 30))
            continue;
        assert(!memcmp(before.data() + fixed.address, dynamic_fixture::flash.data() + fixed.address,
                       fixed.size));
    }
    assert(Launch("app_00") == ESP_OK && fixture::boot == part);
    assert(Install(app) == kAlreadyInstalled);
    assert(Remove("app_00") == kRestartRequired);
    Reboot();
    assert(Count() == 0);
    for (const auto& request : fixture::requests) {
        assert(request.method == HTTP_METHOD_GET);
        assert(!request.headers.count("Authorization") && !request.headers.count("X-Token") &&
               !request.headers.count("Cookie") && !request.headers.count("X-User-Info"));
    }
    cJSON_Delete(root);
    puts(
        "Real firmware code 100018: production V5 lookup/refresh, MD5 corruption rejection, "
        "timeout before writes, exact binary installation, persistent firmware/release identity, "
        "launch selection and removal passed. Launcher and appdata preserved. Simulated flash "
        "only.");
}
