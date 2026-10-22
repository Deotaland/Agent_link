#pragma once

#include <stddef.h>
#include <stdint.h>

namespace agentlink::realtime {

// One C3 voice worker owns the vendor codec allocator while a call is active.
// Encoder/decoder wrappers and core state share this reservation sequentially.
// No codec allocation falls back to the fragmented system heap for that worker.
class CodecWorkspace {
public:
    static constexpr size_t kBytes = 25 * 1024;
    ~CodecWorkspace();
    CodecWorkspace() = default;
    CodecWorkspace(const CodecWorkspace&) = delete;
    CodecWorkspace& operator=(const CodecWorkspace&) = delete;
    bool Reserve();
    void* Allocate(size_t bytes);
    bool Contains(const void* ptr) const;
    void Free(void* ptr);
    // Discard unpublished allocations after vendor open fails with no handle.
    void Rollback();
    bool Owner() const;
    bool Reserved() const { return data_ != nullptr; }
    size_t Live() const { return live_; }
    size_t Peak() const { return peak_; }
private:
    struct Slot { void* ptr = nullptr; bool live = false; };
    Slot slots_[8]{};
    uint8_t* data_ = nullptr;
    void* owner_ = nullptr;
    size_t used_ = 0, live_ = 0, peak_ = 0;
};

}  // namespace agentlink::realtime
