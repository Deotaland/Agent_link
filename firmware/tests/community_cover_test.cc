#include "../components/agent_system/community_cover.h"
#include "../components/agent_system/community_store.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
namespace agent_system::community {
File::~File() {
    delete handle;
}
esp_err_t File::Open(const char* url) {
    std::ifstream stream(std::filesystem::u8path(url), std::ios::binary);
    if (!stream)
        return ESP_FAIL;
    handle = new Http;
    handle->body = {std::istreambuf_iterator<char>(stream), {}};
    return ESP_OK;
}
}  // namespace agent_system::community
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), {}};
}
uint32_t Crc(const uint8_t* p, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return crc ^ 0xFFFFFFFF;
}
int main(int argc, char** argv) {
    using namespace agent_system;
    assert(argc >= 6);
    std::vector<uint8_t> output(kCoverBytes);
    for (int i = 2; i < argc; ++i) {
        auto input = Read(std::filesystem::u8path(argv[i]));
        const auto result =
            community::DecodeCover(input.data(), input.size(), "actual.image", output.data());
        if (result != ESP_OK)
            fprintf(stderr, "Fixture %s failed: %d\n", argv[i], result);
        assert(result == ESP_OK);
        const auto* h = reinterpret_cast<const CoverHeader*>(output.data());
        assert(!memcmp(h->magic, "ALCOVER1", 8) && !strcmp(h->app_id, "actual.image") &&
               h->width == 132 && h->height == 132);
        assert(h->crc32 == Crc(output.data() + 64, kCoverPixelBytes));
        if (i < 6) {
            const size_t n = 66 * 132 + 66;
            const unsigned rgb = output[64 + n * 2] | (output[65 + n * 2] << 8);
            assert(abs(int((rgb >> 11) * 255 / 31) - 200) <= 12);
            assert(abs(int(((rgb >> 5) & 63) * 255 / 63) - 80) <= 12);
            assert(abs(int((rgb & 31) * 255 / 31) - 32) <= 12);
            assert(output[64 + 132 * 132 * 2 + n] == (i == 5 ? 255 : 128));
        }
        assert(community::DecodeCover(input.data(), 8, "actual.image", output.data()) != ESP_OK);
        CatalogApp app;
        strcpy(app.app_id, "actual.image");
        strncpy(app.cover_url, argv[i], sizeof app.cover_url - 1);
        assert(community::FetchCover(app, output.data()) == ESP_OK);
    }
    const auto animated = Read(std::filesystem::u8path(argv[1]) / "animated.webp");
    assert(community::DecodeCover(animated.data(), animated.size(), "app", output.data()) !=
           ESP_OK);
    auto png = Read(std::filesystem::u8path(argv[1]) / "cover.png");
    png[16] = 0x7F;  // Oversized dimensions rejected before decompression/allocation.
    assert(community::DecodeCover(png.data(), png.size(), "app", output.data()) != ESP_OK);
    const uint8_t zip[] = {'P', 'K', 3, 4};
    assert(community::DecodeCover(zip, sizeof zip, "app", output.data()) != ESP_OK);
    const auto jpeg = Read(std::filesystem::u8path(argv[1]) / "cover.jpg");
    for (size_t length = 2; length < std::min<size_t>(256, jpeg.size()); ++length)
        assert(community::DecodeCover(jpeg.data(), length, "app", output.data()) != ESP_OK);
}
