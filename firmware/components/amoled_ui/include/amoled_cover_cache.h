#pragma once
#include "agent_cover.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <algorithm>
#include <cstdint>

// Three recently used sizes in bounded PSRAM storage. Source snapshots remain
// immutable; a new downloaded revision naturally misses this cache.
class AmoledCoverCache {
   public:
    AmoledCoverCache() = default;
    explicit AmoledCoverCache(unsigned capacity) : capacity_(std::max(1u, std::min(3u, capacity))) {}
    AmoledCoverCache(const AmoledCoverCache&) = delete;
    AmoledCoverCache& operator=(const AmoledCoverCache&) = delete;
    ~AmoledCoverCache() {
        for (auto& entry : entries_) {
            lv_image_cache_drop(&entry.image);
            heap_caps_free(entry.pixels);
        }
    }
    const lv_image_dsc_t* Get(const std::shared_ptr<const agent_system::CoverPixels>& source,
                              uint16_t edge) {
        if (!source || (edge < 48 || edge > 300))
            return nullptr;
        Entry* oldest = &entries_[0];
        for (unsigned i = 0; i < capacity_; ++i) {
            auto& entry = entries_[i];
            if (entry.source == source && entry.image.header.w == edge) {
                entry.used = ++sequence_;
                return &entry.image;
            }
            if (entry.used < oldest->used)
                oldest = &entry;
        }
        const size_t size = static_cast<size_t>(edge) * edge * 3;
        auto* pixels =
            static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!pixels)
            return nullptr;  // Renderer can still scale the original.
        Scale(source->Data(), pixels, edge);
        lv_image_cache_drop(&oldest->image);
        heap_caps_free(oldest->pixels);
        oldest->pixels = pixels;
        oldest->source = source;
        oldest->used = ++sequence_;
        auto& image = oldest->image;
        image = {};
        image.header.magic = LV_IMAGE_HEADER_MAGIC;
        image.header.cf = LV_COLOR_FORMAT_RGB565A8;
        image.header.w = edge;
        image.header.h = edge;
        image.header.stride = edge * 2;
        image.data_size = size;
        image.data = pixels;
        return &image;
    }

   private:
    struct Entry {
        std::shared_ptr<const agent_system::CoverPixels> source;
        uint8_t* pixels = nullptr;
        lv_image_dsc_t image{};
        uint64_t used = 0;
    } entries_[3];
    uint64_t sequence_ = 0;
    unsigned capacity_ = 3;
    static void Scale(const uint8_t* source, uint8_t* target, int edge) {
        constexpr int n = agent_system::kCoverEdge;
        const auto* rgb = reinterpret_cast<const uint16_t*>(source);
        const auto* alpha = source + n * n * 2;
        auto* out_rgb = reinterpret_cast<uint16_t*>(target);
        auto* out_alpha = target + edge * edge * 2;
        for (int y = 0; y < edge; ++y) {
            const int sy =
                std::max(0, std::min(((2 * y + 1) * n * 128 / edge) - 128, (n - 1) * 256));
            const int y0 = sy >> 8, y1 = std::min(y0 + 1, n - 1), fy = sy & 255;
            for (int x = 0; x < edge; ++x) {
                const int sx =
                    std::max(0, std::min(((2 * x + 1) * n * 128 / edge) - 128, (n - 1) * 256));
                const int x0 = sx >> 8, x1 = std::min(x0 + 1, n - 1), fx = sx & 255;
                const int offsets[]{y0 * n + x0, y0 * n + x1, y1 * n + x0, y1 * n + x1};
                const uint32_t weights[]{static_cast<uint32_t>((256 - fx) * (256 - fy)),
                                         static_cast<uint32_t>(fx * (256 - fy)),
                                         static_cast<uint32_t>((256 - fx) * fy),
                                         static_cast<uint32_t>(fx * fy)};
                uint32_t a = 0, r = 0, g = 0, b = 0;
                for (int i = 0; i < 4; ++i) {
                    const uint32_t weight = weights[i] * alpha[offsets[i]];
                    const uint16_t color = rgb[offsets[i]];
                    a += weight;
                    r += weight * (color >> 11);
                    g += weight * ((color >> 5) & 63);
                    b += weight * (color & 31);
                }
                const int out = y * edge + x;
                // Interpolate premultiplied colors to avoid dark/color fringes
                // at the cover's existing antialiased circular alpha boundary.
                out_rgb[out] = a ? static_cast<uint16_t>(((r + a / 2) / a << 11) |
                                                         ((g + a / 2) / a << 5) | (b + a / 2) / a)
                                 : 0;
                out_alpha[out] = static_cast<uint8_t>((a + 32768) >> 16);
            }
        }
    }
};
