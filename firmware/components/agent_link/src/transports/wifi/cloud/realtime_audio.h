#pragma once

#include <stddef.h>
#include <stdint.h>

namespace agentlink::realtime {

// Keep PCM frame boundaries independent of the sizes passed to stream_write().
template <size_t FrameBytes>
class PcmFrames {
public:
    void Reset() { size_ = 0; }
    template <typename Emit>
    bool Append(const uint8_t* data, size_t bytes, Emit emit) {
        while (bytes) {
            const size_t take = bytes < FrameBytes - size_ ? bytes : FrameBytes - size_;
            for (size_t i = 0; i < take; ++i) data_[size_ + i] = data[i];
            size_ += take;
            data += take;
            bytes -= take;
            if (size_ == FrameBytes) {
                size_ = 0;
                if (!emit(data_)) return false;
            }
        }
        return true;
    }
    template <typename Emit>
    bool Flush(Emit emit) {
        if (!size_) return true;
        for (size_t i = size_; i < FrameBytes; ++i) data_[i] = 0;
        size_ = 0;
        return emit(data_);
    }
private:
    uint8_t data_[FrameBytes];
    size_t size_ = 0;
};

// The WebSocket writer masks the payload in place, even when its API takes const char*.
template <typename Writer>
bool SendControl(const char* text, Writer write) {
    if (!text) return false;
    char frame[128];
    size_t bytes = 0;
    while (text[bytes]) {
        if (bytes == sizeof frame) return false;
        frame[bytes] = text[bytes];
        ++bytes;
    }
    return write(frame, bytes);
}

// PCM16LE at 24 kHz to 16 kHz, with byte and sample carry across packets.
class Pcm24To16 {
public:
    void Reset() { count_ = 0; have_low_ = false; }
    size_t Convert(const uint8_t* data, size_t bytes, int16_t* out) {
        size_t written = 0;
        for (size_t i = 0; i < bytes; ++i) {
            if (!have_low_) {
                low_ = data[i];
                have_low_ = true;
                continue;
            }
            samples_[count_++] = static_cast<int16_t>(low_ | (uint16_t(data[i]) << 8));
            have_low_ = false;
            if (count_ == 3) {
                // Average over each output sample's 1.5 input samples.
                out[written++] = static_cast<int16_t>((2 * int32_t(samples_[0]) + samples_[1]) / 3);
                out[written++] = static_cast<int16_t>((int32_t(samples_[1]) + 2 * int32_t(samples_[2])) / 3);
                count_ = 0;
            }
        }
        return written;
    }

private:
    int16_t samples_[3] = {};
    unsigned count_ = 0;
    uint8_t low_ = 0;
    bool have_low_ = false;
};

// The transport may split a frame across reads and a message across frames.
class Message {
public:
    enum Result { kError, kMore, kComplete };
    Message(uint8_t* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}
    Result Append(unsigned opcode, bool frame_start, bool frame_end, bool fin,
                  const uint8_t* data, size_t bytes) {
        if (frame_start) {
            if (opcode == 1 || opcode == 2) {
                if (active_) return kError;
                opcode_ = opcode;
                size_ = 0;
                active_ = true;
            } else if (opcode != 0 || !active_) {
                return kError;
            }
        }
        if (!active_ || bytes > capacity_ - size_) return kError;
        for (size_t i = 0; i < bytes; ++i) buffer_[size_++] = data[i];
        if (frame_end && fin) {
            active_ = false;
            return kComplete;
        }
        return kMore;
    }
    size_t Size() const { return size_; }
    unsigned Opcode() const { return opcode_; }

private:
    uint8_t* buffer_;
    size_t capacity_;
    size_t size_ = 0;
    unsigned opcode_ = 0;
    bool active_ = false;
};

}  // namespace agentlink::realtime
