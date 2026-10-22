#pragma once

#include <stddef.h>
#include <stdint.h>

namespace agentlink::realtime {

// Reallocator follows realloc semantics, including freeing on size == 0.
// Grow only to a bounded message size; return idle storage after delivery.
class MessageBuffer {
public:
    using Reallocator = void* (*)(void*, size_t);
    MessageBuffer(size_t initial, size_t maximum, Reallocator resize)
        : initial_(initial), maximum_(maximum), resize_(resize) { Reserve(initial); }
    ~MessageBuffer() { if (data_) resize_(data_, 0); }
    MessageBuffer(const MessageBuffer&) = delete;
    MessageBuffer& operator=(const MessageBuffer&) = delete;
    bool Reserve(size_t bytes) {
        if (bytes > maximum_) return false;
        if (bytes <= capacity_) return true;
        size_t capacity = capacity_ ? capacity_ : initial_;
        while (capacity < bytes) capacity = capacity > maximum_ / 2 ? maximum_ : capacity * 2;
        auto* next = static_cast<uint8_t*>(resize_(data_, capacity));
        if (!next) return false;
        data_ = next;
        capacity_ = capacity;
        return true;
    }
    void Trim() {
        if (capacity_ <= initial_) return;
        auto* next = static_cast<uint8_t*>(resize_(data_, initial_));
        if (next) { data_ = next; capacity_ = initial_; }
    }
    uint8_t* Data() const { return data_; }
    size_t Capacity() const { return capacity_; }
private:
    const size_t initial_, maximum_;
    Reallocator resize_;
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0;
};

}  // namespace agentlink::realtime
