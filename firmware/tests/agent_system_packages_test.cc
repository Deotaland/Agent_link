#include "agent_system_stubs.h"
#include "agent_system.h"
#include "cJSON.h"
#include <cassert>
#include <filesystem>
#include <fstream>

namespace {
std::string Read(const char* path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    assert(file.is_open());
    return std::string(std::istreambuf_iterator<char>(file), {});
}
const char* Text(cJSON* object, const char* key) {
    auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    assert(cJSON_IsString(value));
    return value->valuestring;
}
void ClearSlots() {
    for (int i = 1; i <= 2; ++i) {
        const auto& part = fixture::partitions[i];
        fixture::flash[part.address].assign(part.size, 0xFF);
        fixture::valid[part.address] = false;
        fixture::image_lengths[part.address] = 0;
        fixture::store.erase(std::string("app_nvs/al_apps/") + part.label);
    }
    fixture::boot = nullptr;
}
void Seed(const agent_system::CatalogApp& app) {
    const auto& part = fixture::partitions[1];
    const auto& bytes = fixture::responses.at(app.url);
    memcpy(fixture::flash[part.address].data(), bytes.data(), bytes.size());
    fixture::valid[part.address] = true;
    fixture::image_lengths[part.address] = static_cast<uint32_t>(bytes.size());
}
}

int main(int argc, char** argv) {
    using namespace agent_system;
    assert(argc == 2);
    fixture::partitions[1].size = 0x500000;
    fixture::partitions[2].address = 0x810000;
    fixture::partitions[2].size = 0x500000;
    assert(Init() == ESP_OK);
    const auto input = Read(argv[1]);
    cJSON* root = cJSON_Parse(input.c_str());
    assert(root);
    const std::string origin = Text(root, "sdk_origin");
    auto* files = cJSON_GetObjectItemCaseSensitive(root, "apps");
    cJSON* item;
    cJSON_ArrayForEach(item, files) {
        fixture::responses[origin + Text(item, "url")] = Read(Text(item, "download_path"));
    }
    std::vector<CatalogApp> apps;
    auto* pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    cJSON_ArrayForEach(item, pages) {
        const std::string url = origin + Text(item, "route");
        assert(SetCatalogUrl(url.c_str()) == ESP_OK);
        char* json = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(item, "catalog"));
        assert(json);
        fixture::responses[url] = json;
        cJSON_free(json);
        CatalogApp page[kMaxApps];
        size_t count = 0;
        assert(FetchCatalog(page, kMaxApps, &count) == ESP_OK);
        apps.insert(apps.end(), page, page + count);
    }
    assert(apps.size() == static_cast<size_t>(cJSON_GetArraySize(files)) && apps.size() >= 2);
    fixture::store["agent_nvs/al_dev/auth_key"] = {'k','e','y',0};
    fixture::store["agent_nvs/al_wifi/ssid"] = {'h','o','m','e',0};
    bool tampered_native = false, tampered_vendor = false;
    size_t transactions = 0;
    for (size_t n = 0; n < apps.size(); ++n) {
        const auto& app = apps[n];
        printf("Checking %s (%s), %u bytes\n", app.app_id,
               app.format == ImageFormat::kSdgoods ? "sdgoods" : "agent-link", app.size);
        const auto expected = fixture::responses.at(app.url);
        bool& tampered = app.format == ImageFormat::kSdgoods ? tampered_vendor : tampered_native;
        if (!tampered) {
            ClearSlots();
            fixture::responses[app.url][512] ^= 1;
            assert(Install(app) == ESP_ERR_INVALID_CRC);
            App installed[2];size_t count = 0;
            assert(List(installed, 2, &count) == ESP_OK && count == 0);
            fixture::responses[app.url] = expected;
            tampered = true;
        }
        for (int slot = 0; slot < 2; ++slot) {
            ClearSlots();
            if (slot == 1) Seed(apps[(n + 1) % apps.size()]);
            assert(Install(app) == ESP_OK);
            const auto& part = fixture::partitions[slot + 1];
            assert(!memcmp(fixture::flash[part.address].data(), expected.data(), expected.size()));
            assert(fixture::store.at(std::string("app_nvs/al_apps/") + part.label) == std::vector<uint8_t>{1});
            App installed[2];size_t count = 0;
            assert(List(installed, 2, &count) == ESP_OK && count == static_cast<size_t>(slot + 1));
            bool found = false;
            for (size_t i = 0; i < count; ++i) {
                if (strcmp(installed[i].descriptor.app_id, app.app_id)) continue;
                found = true;
                assert(installed[i].format == app.format && !strcmp(installed[i].version, app.version));
                assert(!strcmp(installed[i].slot, part.label));
            }
            assert(found);
            const int opens = fixture::opens;
            assert(Install(app) == kAlreadyInstalled && fixture::opens == opens);
            assert(Launch(part.label) == ESP_OK && fixture::boot == &part);
            assert(Remove(part.label) == ESP_OK);
            assert(List(installed, 2, &count) == ESP_OK && count == static_cast<size_t>(slot));
            ++transactions;
        }
    }
    assert(tampered_native && tampered_vendor);
    assert(fixture::store.at("agent_nvs/al_dev/auth_key") == (std::vector<uint8_t>{'k','e','y',0}));
    assert(fixture::store.at("agent_nvs/al_wifi/ssid") == (std::vector<uint8_t>{'h','o','m','e',0}));
    printf("Real payload checks passed: %zu apps, %zu simulated OTA installs, real SHA-256; "
           "identity, versions, flash bytes, duplicates, slot selection, launch selection and removal. "
           "No hardware execution.\n", apps.size(), transactions);
    cJSON_Delete(root);
}
