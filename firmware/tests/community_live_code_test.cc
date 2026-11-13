#include "agent_system_dynamic_stubs.h"
#include "dynamic_layout_config.h"
#include "agent_system.h"
#include "../components/agent_system/community_store.h"
#include "cJSON.h"
#include <filesystem>
#include <fstream>

namespace agent_system::community {
esp_err_t FetchCover(const CatalogApp&, uint8_t*) { return ESP_ERR_NOT_SUPPORTED; }
}
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
std::string Json(cJSON* value) {
    char* encoded = cJSON_PrintUnformatted(value);
    assert(encoded);
    std::string result(encoded); cJSON_free(encoded); return result;
}
void Put(uint32_t offset, const std::string& bytes) {
    assert(offset + bytes.size() <= dynamic_fixture::flash.size());
    memcpy(dynamic_fixture::flash.data() + offset, bytes.data(), bytes.size());
}
void Reboot() {
    uint8_t scratch[4096]; auto io = dynamic_fixture::Io();
    assert(alpt_restore(&alpt_config, &io, scratch) >= 0);
    dynamic_fixture::Reload();
    assert(agent_system::Init() == ESP_OK);
}
}
int main(int argc, char** argv) {
    using namespace agent_system;
    assert(argc == 3);
    const auto artifacts = std::filesystem::u8path(argv[1]) / "factory";
    const auto snapshot = Read(std::filesystem::u8path(argv[2]));
    auto* root = cJSON_Parse(snapshot.c_str()); assert(root);
    auto* detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
    auto* data = cJSON_GetObjectItemCaseSensitive(detail, "data");
    auto* published = cJSON_GetObjectItemCaseSensitive(data, "publishedVersion");
    const std::string code = Text(data, "downloadCode");
    assert(code == "261112");
    const std::string lookup = "https://sugapi.deotaland.cn/api/v1/community/download-codes/" + code;
    const auto binary = Read(std::filesystem::u8path(Text(root, "file")));
    fixture::responses[lookup] = Json(detail);
    fixture::responses[lookup + "/downloads"] = Json(cJSON_GetObjectItemCaseSensitive(root, "ticket"));
    fixture::responses[Text(root, "download_url")] = binary;
    Put(0, Read(artifacts / "bootloader/bootloader.bin"));
    Put(0x8000, Read(artifacts / "partition_table/partition-table.bin"));
    Put(0x10000, Read(artifacts / "recovery/SDGOODS_LAUNCHER.bin"));
    Put(ALBOOT_A_OFFSET, Read(artifacts / "agent_link.bin"));
    dynamic_fixture::Reload();
    assert(Init() == kRestartRequired); Reboot();
    const auto before = dynamic_fixture::flash;
    CatalogApp app;
    assert(FetchDownloadCode(code.c_str(), &app) == ESP_OK);
    assert(app.package == CatalogApp::Package::kApplication && app.size == binary.size());
    InstallStatus status;
    const auto error = Install(app, nullptr, nullptr, &status);
    if (error != kRestartRequired) {
        StoreError store; LastStoreError(&store);
        printf("Install failed error=%d stage=%d http=%d message=%s\n", error, int(status.stage), store.http_status, store.message);
    }
    assert(error == kRestartRequired); Reboot();
    App installed[14]; size_t count = 0;
    assert(List(installed, 14, &count) == ESP_OK && count == 1);
    assert(!strcmp(installed[0].slot, "app_00"));
    assert(installed[0].format == ImageFormat::kSdgoods);
    assert(!strcmp(installed[0].release_version, Text(published, "version")));
    const auto* part = esp_partition_find_first(0, 16, "app_00"); assert(part);
    assert(part->address == alpt_config.pool_start && part->size == ((binary.size() + kCoverReserveBytes + 65535) & ~65535u));
    assert(!memcmp(dynamic_fixture::flash.data() + part->address, binary.data(), binary.size()));
    for (const auto& fixed : dynamic_fixture::partitions) {
        if (fixed.address == alpt_config.table || !strcmp(fixed.label, "otadata") ||
            !strcmp(fixed.label, "app_meta") || !strcmp(fixed.label, "layout_journal") || fixed.type == 0 && fixed.subtype >= 16 && fixed.subtype < 30)
            continue;
        assert(!memcmp(before.data() + fixed.address, dynamic_fixture::flash.data() + fixed.address, fixed.size));
    }
    assert(Launch("app_00") == ESP_OK && fixture::boot == part);
    assert(Install(app) == kAlreadyInstalled);
    assert(Remove("app_00") == kRestartRequired); Reboot();
    assert(List(installed, 14, &count) == ESP_OK && count == 0);
    printf("Download code 261112: real API metadata and firmware accepted by production V5 parser/installer; app_00 allocated, image unchanged, launch selection/removal passed, appdata and Launcher regions preserved. Simulated flash only.\n");
    cJSON_Delete(root);
}
