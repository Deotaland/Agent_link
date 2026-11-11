#include "agent_system_dynamic_stubs.h"
#include "dynamic_layout_config.h"
#include "agent_system.h"
#include "cJSON.h"
#include <filesystem>
#include <fstream>

namespace {
using namespace agent_system;
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.is_open());
    return {std::istreambuf_iterator<char>(file), {}};
}
const char* Text(cJSON* object, const char* key) {
    auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    assert(cJSON_IsString(value));
    return value->valuestring;
}
void Put(uint32_t address, const std::string& bytes) {
    memcpy(dynamic_fixture::flash.data() + address, bytes.data(), bytes.size());
}
void Reboot() {
    uint8_t scratch[4096];
    auto io = dynamic_fixture::Io();
    assert(alpt_restore(&alpt_config, &io, scratch) >= 0);
    dynamic_fixture::Reload();
    assert(Init() == ESP_OK);
}
size_t Count() {
    App apps[16];
    size_t count = 0;
    assert(List(apps, 16, &count) == ESP_OK);
    return count;
}
std::string SlotFor(const char* id) {
    App apps[16];
    size_t count = 0;
    assert(List(apps, 16, &count) == ESP_OK);
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(apps[i].descriptor.app_id, id))
            return apps[i].slot;
    assert(false);
    return {};
}
void Empty() {
    alpt_entry slots[16];
    for (unsigned i = 0; i < 16; ++i)
        alpt_empty(&alpt_config, i, &slots[i]);
    uint8_t table[4096], scratch[4096];
    auto io = dynamic_fixture::Io();
    assert(alpt_build(&alpt_config, &io, slots, table) == 0 &&
           alpt_commit(&alpt_config, &io, table, scratch) == 0);
    for (auto i = fixture::store.begin(); i != fixture::store.end();) {
        if (i->first.find("app_nvs/al_apps/") == 0)
            i = fixture::store.erase(i);
        else
            ++i;
    }
    Reboot();
    assert(Count() == 0);
}
void InstallAndCheck(const CatalogApp& app) {
    const auto before = Count();
    assert(Install(app) == kRestartRequired);
    Reboot();
    assert(Count() == before + 1);
    const auto label = SlotFor(app.app_id);
    const auto* part = esp_partition_find_first(0, 255, label.c_str());
    assert(part);
    const auto& original = fixture::responses.at(app.url);
    assert(part->size == ((app.size + kCoverReserveBytes + 0xFFFF) & ~0xFFFFu));
    assert(
        !memcmp(dynamic_fixture::flash.data() + part->address, original.data(), original.size()));
    assert(Launch(label.c_str()) == ESP_OK && fixture::boot == part);
}
}  // namespace
int main(int argc, char** argv) {
    using namespace agent_system;
    assert(argc == 3);
    const std::filesystem::path artifacts = std::filesystem::u8path(argv[2]);
    const auto factory = Read(artifacts / "factory/agent_link.bin");
    Put(0, Read(artifacts / "factory/bootloader/bootloader.bin"));
    const auto seed_table = Read(artifacts / "factory/partition_table/partition-table.bin");
    Put(0x8000, seed_table);
    Put(0x10000, factory);
    dynamic_fixture::Reload();
    assert(Init() == kRestartRequired);
    Reboot();
    assert(Count() == 0);
    // A full USB package seeds ota_0 at the pool start. Shrink it without losing
    // the image or the cover previously stored at the bootstrap pool's tail.
    memset(dynamic_fixture::flash.data() + alpt_config.guard, 255, 0x4000);
    Put(0x8000, seed_table);
    Put(0x310000, Read(artifacts / "standard/agent_link.bin"));
    fixture::store["app_nvs/al_apps/ota_0"] = {2};
    memset(dynamic_fixture::flash.data() + alpt_config.pool_end - kCoverReserveBytes, 0xA6,
           kCoverReserveBytes);
    dynamic_fixture::Reload();
    assert(Init() == kRestartRequired);
    Reboot();
    assert(Count() == 1);
    const auto* seeded = esp_partition_find_first(0, 16, "ota_0");
    assert(seeded && seeded->size < 0xA00000);
    for (size_t i = 0; i < kCoverReserveBytes; ++i)
        assert(dynamic_fixture::flash[seeded->address + seeded->size - kCoverReserveBytes + i] ==
               0xA6);
    Empty();
    const auto input = Read(std::filesystem::u8path(argv[1]));
    auto* root = cJSON_Parse(input.c_str());
    assert(root);
    const std::string origin = Text(root, "sdk_origin");
    auto* files = cJSON_GetObjectItemCaseSensitive(root, "apps");
    cJSON* item;
    cJSON_ArrayForEach(item, files) fixture::responses[origin + Text(item, "url")] =
        Read(std::filesystem::u8path(Text(item, "download_path")));
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
    assert(apps.size() == static_cast<size_t>(cJSON_GetArraySize(files)));
    fixture::store["agent_nvs/al_wifi/ssid"] = {'h', 'o', 'm', 'e', 0};
    fixture::store["nvs/bmb_net/ssid"] = {'a', 'p', 'p', 0};
    // Sentinel bytes in every persistent data region, including the upper FAT area.
    std::vector<std::pair<uint32_t, uint8_t>> sentinels;
    for (const auto& part : dynamic_fixture::partitions)
        if (part.type == 1 && strcmp(part.label, "pt_guard") && strcmp(part.label, "otadata")) {
            for (uint32_t offset : {uint32_t(0), part.size - 1}) {
                const uint8_t value = static_cast<uint8_t>(sentinels.size() + 1);
                dynamic_fixture::flash[part.address + offset] = value;
                sentinels.push_back({part.address + offset, value});
            }
        }
    size_t transactions = 0;
    for (const auto& app : apps) {
        Empty();
        const auto original = fixture::responses.at(app.url);
        fixture::responses[app.url][512] ^= 1;
        assert(Install(app) == ESP_ERR_INVALID_CRC && Count() == 0);
        fixture::responses[app.url] = original;
        InstallAndCheck(app);
        ++transactions;
        assert(Install(app) == kAlreadyInstalled);
        assert(Remove(SlotFor(app.app_id).c_str()) == kRestartRequired);
        Reboot();
        assert(Count() == 0);
    }
    // Interrupted downloads never publish a candidate extent.
    Empty();
    fixture::fail_after = 1000;
    assert(Install(apps.front()) == ESP_ERR_INVALID_RESPONSE && Count() == 0);
    fixture::fail_after = static_cast<size_t>(-1);
    // Exercise the actual installer across a torn journal commit and torn
    // primary erase/write, in addition to the exhaustive shared-store checks.
    Empty();
    const auto saved_flash = dynamic_fixture::flash;
    const auto saved_nvs = fixture::store;
    dynamic_fixture::mutations = 0;
    assert(Install(apps.front()) == kRestartRequired);
    const int last_mutation = dynamic_fixture::mutations;
    for (int failure : {last_mutation - 2, last_mutation - 1, last_mutation}) {
        dynamic_fixture::flash = saved_flash;
        fixture::store = saved_nvs;
        Reboot();
        dynamic_fixture::mutations = 0;
        dynamic_fixture::cut = failure;
        dynamic_fixture::torn = 1;
        assert(Install(apps.front()) == kRestartRequired);
        assert(Install(apps.front()) == kNeedsFullPackage);
        dynamic_fixture::cut = -1;
        Reboot();
        assert(Count() == (failure == last_mutation - 2 ? 0 : 1));
    }
    // Actual downloaded apps coexist beyond the old two-slot limit.
    auto ordered = apps;
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.size < b.size; });
    Empty();
    size_t simultaneous = 0;
    uint32_t allocated = 0;
    for (const auto& app : ordered) {
        const uint32_t need = (app.size + kCoverReserveBytes + 0xFFFF) & ~0xFFFFu;
        if (allocated + need > 0xA00000)
            continue;
        InstallAndCheck(app);
        allocated += need;
        ++simultaneous;
        ++transactions;
        if (simultaneous == 5)
            break;
    }
    assert(simultaneous == 5 && Count() == 5);
    const auto freed = SlotFor(ordered[1].app_id);
    const auto* part = esp_partition_find_first(0, 255, freed.c_str());
    assert(part);
    const auto freed_address = part->address;
    assert(Remove(freed.c_str()) == kRestartRequired);
    Reboot();
    InstallAndCheck(ordered[1]);
    ++transactions;
    assert(esp_partition_find_first(0, 255, freed.c_str())->address == freed_address);
    // Legacy native code only recognizes subtype 0/1; do not strand it in ota_2.
    const auto legacy = std::find_if(apps.begin(), apps.end(), [](const auto& a) {
        return a.format == ImageFormat::kAgentLink;
    });
    assert(legacy != apps.end());
    Empty();
    unsigned installed = 0;
    for (const auto& app : ordered)
        if (app.format == ImageFormat::kSdgoods && installed < 2) {
            InstallAndCheck(app);
            ++installed;
            ++transactions;
        }
    assert(installed == 2 && Install(*legacy) == kLegacySlotsFull && Count() == 2);
    // The just-built native image advertises support for every dynamic subtype.
    CatalogApp native = *legacy;
    const auto bytes = Read(artifacts / "standard/agent_link.bin");
    fixture::responses[native.url] = bytes;
    native.size = static_cast<uint32_t>(bytes.size());
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    uint8_t digest[32];
    assert(mbedtls_sha256_starts(&hash, 0) == 0 &&
           mbedtls_sha256_update(&hash, (const uint8_t*)bytes.data(), bytes.size()) == 0 &&
           mbedtls_sha256_finish(&hash, digest) == 0);
    mbedtls_sha256_free(&hash);
    for (unsigned i = 0; i < 32; ++i)
        sprintf(native.sha256 + i * 2, "%02x", digest[i]);
    InstallAndCheck(native);
    assert(SlotFor(native.app_id) == "ota_2");
    ++transactions;
    for (const auto& item : sentinels)
        assert(dynamic_fixture::flash[item.first] == item.second);
    assert(fixture::store.at("agent_nvs/al_wifi/ssid") ==
           std::vector<uint8_t>({'h', 'o', 'm', 'e', 0}));
    assert(fixture::store.at("nvs/bmb_net/ssid") == std::vector<uint8_t>({'a', 'p', 'p', 0}));
    assert(!memcmp(dynamic_fixture::flash.data() + 0x10000, factory.data(), factory.size()));
    // A mismatched bootloader or encrypted flash cannot authorize table writes.
    dynamic_fixture::encrypted = true;
    assert(Init() == ESP_OK && Install(native) == kNeedsFullPackage);
    dynamic_fixture::encrypted = false;
    memset(dynamic_fixture::flash.data(), 255, 0x8000);
    assert(Init() == ESP_OK && Install(native) == kNeedsFullPackage);
    cJSON_Delete(root);
    printf(
        "Dynamic SDK: %zu real payloads, %zu verified installs, five apps together, "
        "deletion/reuse, legacy/new native compatibility and unchanged data. No hardware "
        "execution.\n",
        apps.size(), transactions);
}
