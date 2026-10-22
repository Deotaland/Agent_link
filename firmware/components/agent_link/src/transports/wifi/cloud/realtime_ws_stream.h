#pragma once

#include <stddef.h>
#include <stdint.h>

namespace agentlink::realtime {

// Keep a partially received server frame across nonblocking TLS reads. Control
// frames may occur between fragments of a data message; the message owner keeps
// that separate state. Servers must not mask frames or use unnegotiated RSV bits.
class FrameReader {
public:
    explicit FrameReader(size_t maximum) : maximum_(maximum) {}
    template <typename Chunk>
    bool Consume(const uint8_t* data, size_t bytes, Chunk chunk) {
        while (bytes || (header_size_ == header_need_ && remaining_ == 0)) {
            if (!payload_) {
                while (bytes && header_size_ < header_need_) {
                    header_[header_size_++] = *data++; --bytes;
                    if (header_size_ == 2) {
                        const unsigned length = header_[1] & 0x7f;
                        header_need_ = length == 126 ? 4 : length == 127 ? 10 : 2;
                    }
                }
                if (header_size_ != header_need_) return true;
                if ((header_[0] & 0x70) || (header_[1] & 0x80)) return false;
                opcode_ = header_[0] & 15;
                fin_ = (header_[0] & 0x80) != 0;
                if (opcode_ != 0 && opcode_ != 1 && opcode_ != 2 &&
                    opcode_ != 8 && opcode_ != 9 && opcode_ != 10) return false;
                uint64_t length = header_[1] & 0x7f;
                if (header_need_ > 2) {
                    length = 0;
                    for (size_t i = 2; i < header_need_; ++i) length = (length << 8) | header_[i];
                    if ((header_need_ == 4 && length < 126) ||
                        (header_need_ == 10 && (length < 65536 || (header_[2] & 0x80)))) return false;
                }
                if (length > maximum_ || (opcode_ >= 8 && (!fin_ || length > 125))) return false;
                remaining_ = static_cast<size_t>(length);
                first_ = payload_ = true;
            }
            const size_t take = bytes < remaining_ ? bytes : remaining_;
            if (!take && remaining_) return true;
            const bool end = take == remaining_;
            if (!chunk(opcode_, first_, end, fin_, data, take)) return false;
            first_ = false;
            data += take; bytes -= take; remaining_ -= take;
            if (end) { payload_ = false; header_size_ = 0; header_need_ = 2; }
        }
        return true;
    }
private:
    size_t maximum_, header_size_ = 0, header_need_ = 2, remaining_ = 0;
    uint8_t header_[10]{};
    unsigned opcode_ = 0;
    bool fin_ = false, first_ = false, payload_ = false;
};

// The caller owns an immutable masked frame until all plaintext is accepted by
// TLS. WANT_READ/WANT_WRITE retries use precisely the same suffix; never remask,
// overwrite, duplicate or interleave another frame. A stall has one deadline.
class PendingWrite {
public:
    bool Begin(size_t bytes, int64_t now_ms) {
        if (Busy() || !bytes) return false;
        size_ = bytes; offset_ = 0; started_ = now_ms;
        return true;
    }
    bool Advance(int result, bool would_block, int64_t now_ms) {
        if (!Busy()) return false;
        if (result > 0) {
            if (static_cast<size_t>(result) > Remaining()) return false;
            offset_ += result;
            if (!Busy()) return true;
        } else if (!would_block) return false;
        return now_ms - started_ < 3000;
    }
    bool Busy() const { return offset_ < size_; }
    bool Expired(int64_t now_ms) const { return Busy() && now_ms - started_ >= 3000; }
    size_t Offset() const { return offset_; }
    size_t Remaining() const { return size_ - offset_; }
    size_t Size() const { return size_; }
private:
    size_t size_ = 0, offset_ = 0;
    int64_t started_ = 0;
};

}  // namespace agentlink::realtime
