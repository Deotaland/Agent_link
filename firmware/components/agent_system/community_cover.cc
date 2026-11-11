#include "community_cover.h"
#include "community_store.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "lodepng.h"
#include "src/webp/decode.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace agent_system::community {
namespace {
constexpr size_t kInputLimit = 2 * 1024 * 1024;
struct Free {
    void operator()(uint8_t* p) const {
        heap_caps_free(p);
    }
};
using Pixels = std::unique_ptr<uint8_t, Free>;
Pixels Allocate(size_t size) {
    return Pixels(
        static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
bool Dimensions(unsigned w, unsigned h) {
    return w && h && w <= 2048 && h <= 2048 && static_cast<uint64_t>(w) * h <= 1024 * 1024;
}
bool JpegHeader(const uint8_t* data, size_t size) {
    // Bound every marker before the decoder's metadata reader sees it.
    size_t at = 2;
    while (at < size) {
        if (data[at++] != 0xFF)
            return false;
        while (at < size && data[at] == 0xFF)
            ++at;
        if (at == size)
            return false;
        const uint8_t marker = data[at++];
        if (marker == 0xD9 || marker == 0xDA || marker == 0xC2)
            return false;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;
        if (size - at < 2)
            return false;
        const size_t length = (data[at] << 8) | data[at + 1];
        if (length < 2 || length > size - at)
            return false;
        if (marker == 0xC0) {
            if (length < 8 || data[at + 2] != 8)
                return false;
            const unsigned components = data[at + 7];
            return (components == 1 || components == 3) && length >= 8 + components * 3 &&
                   Dimensions((data[at + 5] << 8) | data[at + 6],
                              (data[at + 3] << 8) | data[at + 4]);
        }
        at += length;
    }
    return false;
}
uint32_t Crc(const uint8_t* p, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    }
    return crc ^ 0xFFFFFFFF;
}
// Center crop and bilinear sampling. Accumulate premultiplied color to keep
// transparent PNG/WebP edges clean when the launcher composites the A8 plane.
void Convert(const uint8_t* pixels, unsigned w, unsigned h, unsigned channels, uint8_t* output) {
    const unsigned edge = std::min(w, h), left = (w - edge) / 2, top = (h - edge) / 2;
    auto* rgb = output + kCoverHeaderBytes;
    auto* alpha = rgb + kCoverEdge * kCoverEdge * 2;
    for (unsigned y = 0; y < kCoverEdge; ++y) {
        const uint32_t fy = (edge > 1 ? y * (edge - 1) * 256 / (kCoverEdge - 1) : 0);
        for (unsigned x = 0; x < kCoverEdge; ++x) {
            const uint32_t fx = (edge > 1 ? x * (edge - 1) * 256 / (kCoverEdge - 1) : 0);
            const unsigned sx = fx / 256, sy = fy / 256, dx = fx % 256, dy = fy % 256;
            uint64_t a = 0, color[3]{};
            for (unsigned yy = 0; yy < 2; ++yy)
                for (unsigned xx = 0; xx < 2; ++xx) {
                    const auto* p = pixels + ((top + std::min(sy + yy, edge - 1)) * w + left +
                                              std::min(sx + xx, edge - 1)) *
                                                 channels;
                    const unsigned weight = (xx ? dx : 256 - dx) * (yy ? dy : 256 - dy);
                    const unsigned opacity = channels == 4 ? p[3] : 255;
                    a += static_cast<uint64_t>(opacity) * weight;
                    for (unsigned c = 0; c < 3; ++c)
                        color[c] += static_cast<uint64_t>(p[c]) * opacity * weight;
                }
            const uint16_t r = a ? (color[0] + a / 2) / a : 0;
            const uint16_t g = a ? (color[1] + a / 2) / a : 0;
            const uint16_t b = a ? (color[2] + a / 2) / a : 0;
            const uint16_t value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            const unsigned index = y * kCoverEdge + x;
            rgb[index * 2] = value;
            rgb[index * 2 + 1] = value >> 8;
            alpha[index] = (a + 32768) / 65536;
        }
    }
}
}  // namespace
esp_err_t DecodeCover(const uint8_t* data, size_t size, const char* app_id, uint8_t* output) {
    if (!data || !size || size > kInputLimit || !app_id || strlen(app_id) >= 40 || !output)
        return ESP_ERR_INVALID_ARG;
    Pixels decoded;
    unsigned w = 0, h = 0, channels = 4;
    if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) {
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config) ||
            WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK ||
            config.input.has_animation || !Dimensions(config.input.width, config.input.height))
            return ESP_ERR_INVALID_RESPONSE;
        const unsigned edge = std::min(config.input.width, config.input.height);
        config.options.use_cropping = 1;
        config.options.crop_left = (config.input.width - edge) / 2;
        config.options.crop_top = (config.input.height - edge) / 2;
        config.options.crop_width = config.options.crop_height = edge;
        config.options.use_scaling = 1;
        config.options.scaled_width = config.options.scaled_height = kCoverEdge;
        decoded = Allocate(kCoverEdge * kCoverEdge * 4);
        if (!decoded)
            return ESP_ERR_NO_MEM;
        config.output.colorspace = MODE_RGBA;
        config.output.is_external_memory = 1;
        config.output.u.RGBA.rgba = decoded.get();
        config.output.u.RGBA.stride = kCoverEdge * 4;
        config.output.u.RGBA.size = kCoverEdge * kCoverEdge * 4;
        const auto status = WebPDecode(data, size, &config);
        WebPFreeDecBuffer(&config.output);
        if (status != VP8_STATUS_OK)
            return ESP_ERR_INVALID_RESPONSE;
        w = h = kCoverEdge;
    } else if (size >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        LodePNGState state;
        lodepng_state_init(&state);
        state.decoder.read_text_chunks = 0;
        state.decoder.max_icc_size = 128 * 1024;
        auto error = lodepng_inspect(&w, &h, &state, data, size);
        if (error || !Dimensions(w, h)) {
            lodepng_state_cleanup(&state);
            return ESP_ERR_INVALID_SIZE;
        }
        state.decoder.zlibsettings.max_output_size = static_cast<size_t>(w) * h * 8 + h + 4096;
        uint8_t* raw = nullptr;
        error = lodepng_decode(&raw, &w, &h, &state, data, size);
        lodepng_state_cleanup(&state);
        decoded.reset(raw);
        if (error || !decoded)
            return ESP_ERR_INVALID_RESPONSE;
    } else if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        if (!JpegHeader(data, size))
            return ESP_ERR_INVALID_RESPONSE;
        esp_jpeg_image_cfg_t config{};
        config.indata = const_cast<uint8_t*>(data);
        config.indata_size = size;
        config.out_format = JPEG_IMAGE_FORMAT_RGB888;
        esp_jpeg_image_output_t info{};
        if (esp_jpeg_get_image_info(&config, &info) != ESP_OK ||
            !Dimensions(info.width, info.height))
            return ESP_ERR_INVALID_SIZE;
        while (info.output_len > 512 * 512 * 3 && config.out_scale < JPEG_IMAGE_SCALE_1_8) {
            config.out_scale = static_cast<esp_jpeg_image_scale_t>(config.out_scale + 1);
            if (esp_jpeg_get_image_info(&config, &info) != ESP_OK)
                return ESP_ERR_INVALID_RESPONSE;
        }
        decoded = Allocate(info.output_len);
        if (!decoded)
            return ESP_ERR_NO_MEM;
        config.outbuf = decoded.get();
        config.outbuf_size = info.output_len;
        auto scratch = Allocate(65536);
        if (!scratch)
            return ESP_ERR_NO_MEM;
        config.advanced.working_buffer = scratch.get();
        config.advanced.working_buffer_size = 65536;
        if (esp_jpeg_decode(&config, &info) != ESP_OK)
            return ESP_ERR_INVALID_RESPONSE;
        w = info.width;
        h = info.height;
        channels = 3;
    } else
        return ESP_ERR_NOT_SUPPORTED;
    memset(output, 0, kCoverBytes);
    Convert(decoded.get(), w, h, channels, output);
    CoverHeader header{};
    memcpy(header.magic, "ALCOVER1", 8);
    header.version = header.format = 1;
    header.width = header.height = kCoverEdge;
    header.data_len = kCoverPixelBytes;
    header.crc32 = Crc(output + kCoverHeaderBytes, kCoverPixelBytes);
    memcpy(header.app_id, app_id, strlen(app_id) + 1);
    memcpy(output, &header, sizeof header);
    return ESP_OK;
}
esp_err_t FetchCover(const CatalogApp& app, uint8_t* output) {
    File file;
    auto error = file.Open(app.cover_url);
    if (error != ESP_OK)
        return error;
    const auto length = esp_http_client_get_content_length(file.handle);
    if (length > static_cast<int64_t>(kInputLimit) || length == 0)
        return ESP_ERR_INVALID_SIZE;
    auto body = Allocate(length > 0 ? static_cast<size_t>(length) : kInputLimit);
    if (!body)
        return ESP_ERR_NO_MEM;
    size_t received = 0;
    const size_t capacity = length > 0 ? static_cast<size_t>(length) : kInputLimit;
    while (received < capacity) {
        const int bytes =
            esp_http_client_read(file.handle, reinterpret_cast<char*>(body.get() + received),
                                 std::min<size_t>(4096, capacity - received));
        if (bytes < 0)
            return ESP_ERR_INVALID_RESPONSE;
        if (!bytes)
            break;
        received += bytes;
    }
    if (!received || (length > 0 && received != static_cast<size_t>(length)) ||
        !esp_http_client_is_complete_data_received(file.handle))
        return ESP_ERR_INVALID_SIZE;
    return DecodeCover(body.get(), received, app.app_id, output);
}
}  // namespace agent_system::community
