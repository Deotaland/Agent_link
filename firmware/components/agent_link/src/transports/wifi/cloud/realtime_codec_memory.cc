#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32C3 && CONFIG_AGENT_LINK_REALTIME_OPUS
#include "realtime_codec_memory.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
using agentlink::realtime::CodecWorkspace;
std::mutex routing_mutex;
CodecWorkspace* active = nullptr;
}

namespace agentlink::realtime {
bool CodecWorkspace::Reserve() {
    {
        std::lock_guard<std::mutex> lock(routing_mutex);
        if (data_) return Owner();
        if (active) return false;
    }
    auto* storage = static_cast<uint8_t*>(heap_caps_malloc(kBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!storage) return false;
    {
        std::lock_guard<std::mutex> lock(routing_mutex);
        if (!active) {
            data_ = storage;
            owner_ = xTaskGetCurrentTaskHandle();
            active = this;
            return true;
        }
    }
    std::free(storage);
    return false;
}

CodecWorkspace::~CodecWorkspace() {
    if (!data_) return;
    // All handles must be closed before their reservation is released.
    {
        // Foreign tasks may be passing through the libc allocator at teardown.
        // Protect the registry's lifetime, not the system heap allocator itself.
        std::lock_guard<std::mutex> lock(routing_mutex);
        assert(live_ == 0 && active == this);
        active = nullptr;
    }
    std::free(data_);
}

bool CodecWorkspace::Owner() const { return owner_ == xTaskGetCurrentTaskHandle(); }
bool CodecWorkspace::Contains(const void* ptr) const {
    const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t start = reinterpret_cast<uintptr_t>(data_);
    return data_ && value >= start && value - start < kBytes;
}

void* CodecWorkspace::Allocate(size_t bytes) {
    constexpr size_t alignment = 16;
    if (!bytes) bytes = 1;
    if (!data_ || !Owner() || bytes > kBytes || bytes > kBytes - used_) return nullptr;
    const size_t size = (bytes + alignment - 1) & ~(alignment - 1);
    if (size > kBytes - used_) return nullptr;
    for (auto& slot : slots_) {
        if (slot.live) continue;
        slot.ptr = data_ + used_;
        slot.live = true;
        used_ += size;
        ++live_;
        if (used_ > peak_) peak_ = used_;
        return slot.ptr;
    }
    return nullptr;
}

void CodecWorkspace::Rollback() {
    assert(data_ && Owner());
    for (auto& slot : slots_) slot.live = false;
    used_ = live_ = 0;
}

void CodecWorkspace::Free(void* ptr) {
    assert(Owner());
    for (auto& slot : slots_) {
        if (slot.ptr != ptr || !slot.live) continue;
        slot.live = false;
        if (!--live_) used_ = 0;
        return;
    }
    assert(false && "invalid codec workspace free");
}
}  // namespace agentlink::realtime

// esp_audio_codec 2.2.1 explicitly supplies these as weak allocator symbols.
// Override only the C3 build. Other tasks/codec consumers retain libc behavior;
// only the registered voice worker uses its fixed reservation. The workspace
// object is referenced by Call, so the linker pulls this translation unit in.
extern "C" void* media_lib_module_malloc(const char*, size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(routing_mutex);
        if (active && active->Owner()) return active->Allocate(bytes ? bytes : 1);
    }
    return std::malloc(bytes);
}
extern "C" void* media_lib_module_calloc(const char* module, size_t count, size_t bytes) {
    if (bytes && count > std::numeric_limits<size_t>::max() / bytes) return nullptr;
    const size_t size = count * bytes;
    void* data = media_lib_module_malloc(module, size);
    if (data) std::memset(data, 0, size);
    return data;
}
extern "C" void media_lib_free(void* data) {
    if (!data) return;
    {
        std::lock_guard<std::mutex> lock(routing_mutex);
        if (active && active->Contains(data)) { active->Free(data); return; }
    }
    std::free(data);
}
#endif
