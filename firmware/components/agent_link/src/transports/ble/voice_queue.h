#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace agentlink {

// The caller serializes access. Writes either fit completely or leave the queue unchanged.
template <size_t Capacity>
class VoiceQueue {
public:
    bool Push(const uint8_t* data, size_t bytes) {
        if (!data || !bytes || bytes > Capacity - size_) return false;
        const size_t tail = (head_ + size_) % Capacity;
        const size_t first = bytes < Capacity - tail ? bytes : Capacity - tail;
        std::memcpy(data_ + tail, data, first);
        std::memcpy(data_, data + first, bytes - first);
        size_ += bytes;
        return true;
    }

    size_t Pop(uint8_t* dest, size_t bytes) {
        if (!dest) return 0;
        if (bytes > size_) bytes = size_;
        const size_t first = bytes < Capacity - head_ ? bytes : Capacity - head_;
        std::memcpy(dest, data_ + head_, first);
        std::memcpy(dest + first, data_, bytes - first);
        head_ = (head_ + bytes) % Capacity;
        size_ -= bytes;
        return bytes;
    }

    void Clear() { head_ = size_ = 0; }
    size_t Size() const { return size_; }

private:
    uint8_t data_[Capacity] = {};
    size_t head_ = 0;
    size_t size_ = 0;
};

}  // namespace agentlink
