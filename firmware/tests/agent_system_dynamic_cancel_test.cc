#include "agent_system_dynamic_stubs.h"
#include "dynamic_layout_config.h"
#include "agent_system.h"
#include "../components/agent_system/image_identity.h"
#include "../components/agent_link/src/ota/sha256.h"
#include <filesystem>
#include <fstream>

std::string ReadImage(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void PutImage(uint32_t address, const std::string& image) {
    memcpy(dynamic_fixture::flash.data() + address, image.data(), image.size());
}
void RebootFactory() {
    uint8_t scratch[4096];
    auto io = dynamic_fixture::Io();
    assert(alpt_restore(&alpt_config, &io, scratch) >= 0);
    dynamic_fixture::Reload();
    assert(agent_system::Init() == ESP_OK);
}
int main(int argc, char** argv) {
    using namespace agent_system;
    assert(argc == 3);
    const auto artifacts = std::filesystem::u8path(argv[1]) / "factory";
    PutImage(0, ReadImage(artifacts / "bootloader/bootloader.bin"));
    PutImage(0x8000, ReadImage(artifacts / "partition_table/partition-table.bin"));
    PutImage(0x10000, ReadImage(artifacts / "agent_link.bin"));
    dynamic_fixture::Reload();
    const auto init = Init();
    assert(init == ESP_OK || init == kRestartRequired);
    if (init == kRestartRequired)
        RebootFactory();
    const auto table =
        std::string(reinterpret_cast<const char*>(dynamic_fixture::flash.data() + 0x8000), 0x1000);
    const auto image = ReadImage(std::filesystem::u8path(argv[2]));
    assert(image.size() > 4096);
    CatalogApp app{};
    Descriptor identity{}, raw{};
    esp_app_desc_t idf{};
    memcpy(&idf, image.data() + 32, sizeof idf);
    memcpy(&raw, image.data() + 0x120, sizeof raw);
    assert(DecodeIdentity(idf, raw, &identity, &app.format));
    snprintf(app.app_id, sizeof app.app_id, "%s", identity.app_id);
    snprintf(app.version, sizeof app.version, "%s", idf.version);
    snprintf(app.url, sizeof app.url, "https://example.test/download.bin");
    app.size = static_cast<uint32_t>(image.size());
    fixture::responses[app.url] = image;
    agentlink::ota::Sha256 hash;
    uint8_t digest[32];
    assert(hash.Start() &&
           hash.Update(reinterpret_cast<const uint8_t*>(image.data()), image.size()) &&
           hash.Finish(digest));
    for (unsigned i = 0; i < 32; ++i)
        snprintf(app.sha256 + i * 2, 3, "%02x", digest[i]);
    struct Cancellation {
        InstallStage stage;
        bool requested = false;
    };
    const auto observe = +[](const InstallStatus& status, void* context) {
        auto& c = *static_cast<Cancellation*>(context);
        if (status.stage == c.stage &&
            (c.stage != InstallStage::kDownload || status.received > 512))
            c.requested = true;
    };
    const auto cancel =
        +[](void* context) { return static_cast<Cancellation*>(context)->requested; };
    for (auto phase : {InstallStage::kErase, InstallStage::kDownload, InstallStage::kVerify,
                       InstallStage::kPublish}) {
        Cancellation cancellation{phase};
        InstallStatus result;
        assert(Install(app, nullptr, &cancellation, &result, observe, cancel) == kCancelled);
        assert(result.error == kCancelled && result.stage == phase);
        assert(!memcmp(dynamic_fixture::flash.data() + 0x8000, table.data(), table.size()));
        App apps[16];
        size_t count = 0;
        assert(List(apps, 16, &count) == ESP_OK && count == 0);
    }
    InstallStatus result;
    const auto yields_before_retry = fixture::erase_yields;
    assert(Install(app, nullptr, nullptr, &result) == kRestartRequired);
    assert(fixture::erase_yields - yields_before_retry >=
           (app.size + kCoverReserveBytes + 0xFFFF) / 0x10000);
    assert(result.received == app.size && result.stage == InstallStage::kPublish);
    RebootFactory();
    App apps[16];
    size_t count = 0;
    assert(List(apps, 16, &count) == ESP_OK && count == 1 &&
           !strcmp(apps[0].descriptor.app_id, app.app_id));
    puts(
        "Dynamic installer: actual vendor binary, cancellation before erase/during transfer/before "
        "verify/before publication, unchanged table, hidden partial image and full retry passed.");
}
