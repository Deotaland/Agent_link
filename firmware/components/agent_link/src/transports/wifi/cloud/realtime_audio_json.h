#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace agentlink::realtime {
namespace audio_json {

struct String {
    uint8_t* data = nullptr;
    size_t bytes = 0;
    bool Equals(const char* value) const {
        return data && strlen(value) == bytes && memcmp(data, value, bytes) == 0;
    }
};

// A conservative, allocation-free fast path for audio deltas only. Validate
// the whole object before borrowing a string. Other events, escaped property
// names/type, excessive nesting and unsupported strings use the normal cJSON
// path unchanged. Nested metadata cannot masquerade as top-level audio/type.
class Reader {
public:
    Reader(uint8_t* data, size_t bytes) : next_(data), end_(data + bytes) {}
    bool Read(String* audio) {
        Skip();
        if (!Object(0)) return false;
        Skip();
        if (next_ != end_ || !type_.Equals("response.audio.delta") || !audio_.data) return false;
        // Check every escape before making any changes. The fallback parser
        // must receive the original bytes if this is not an ASCII payload.
        for (size_t i = 0; i < audio_.bytes;) {
            uint8_t c = 0;
            if (!Ascii(audio_, &i, &c)) return false;
        }
        size_t written = 0;
        for (size_t i = 0; i < audio_.bytes;) {
            uint8_t c = 0;
            (void)Ascii(audio_, &i, &c);
            audio_.data[written++] = c;
        }
        *audio = {audio_.data, written};
        return true;
    }
private:
    void Skip() { while (next_ != end_ && (*next_ == ' ' || *next_ == '\r' || *next_ == '\n' || *next_ == '\t')) ++next_; }
    bool Take(uint8_t c) { if (next_ == end_ || *next_ != c) return false; ++next_; return true; }
    static int Hex(uint8_t c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool Quoted(String* out) {
        if (!Take('"')) return false;
        uint8_t* begin = next_;
        while (next_ != end_) {
            const uint8_t c = *next_++;
            if (c == '"') { *out = {begin, size_t(next_ - begin - 1)}; return true; }
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (next_ == end_) return false;
            const uint8_t escape = *next_++;
            if (escape == 'u') {
                unsigned code = 0;
                if (!Code(&code)) return false;
                if (code >= 0xdc00 && code <= 0xdfff) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (!Take('\\') || !Take('u') || !Code(&code) || code < 0xdc00 || code > 0xdfff) return false;
                }
            } else if (!strchr("\"\\/bfnrt", escape) || !escape) return false;
        }
        return false;
    }
    bool Code(unsigned* value) {
        *value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (next_ == end_) return false;
            const int digit = Hex(*next_++);
            if (digit < 0) return false;
            *value = (*value << 4) | unsigned(digit);
        }
        return true;
    }
    static bool Ascii(const String& text, size_t* offset, uint8_t* out) {
        uint8_t c = text.data[(*offset)++];
        if (c == '\\') {
            c = text.data[(*offset)++]; // Quoted() has checked the escape bounds.
            if (c == 'u') {
                unsigned value = 0;
                for (unsigned i = 0; i < 4; ++i) value = (value << 4) | unsigned(Hex(text.data[(*offset)++]));
                if (!value || value > 0x7f) return false;
                c = uint8_t(value);
            } else {
                switch (c) {
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: break;
                }
            }
        }
        if (c > 0x7f || !c) return false;
        *out = c;
        return true;
    }
    bool Literal(const char* text) {
        const size_t bytes = strlen(text);
        if (size_t(end_ - next_) < bytes || memcmp(next_, text, bytes)) return false;
        next_ += bytes; return true;
    }
    bool Digits() {
        uint8_t* begin = next_;
        while (next_ != end_ && *next_ >= '0' && *next_ <= '9') ++next_;
        return next_ != begin;
    }
    bool Number() {
        (void)Take('-');
        if (!Take('0') && !Digits()) return false;
        if (Take('.') && !Digits()) return false;
        if (Take('e') || Take('E')) {
            if (!Take('+')) (void)Take('-');
            if (!Digits()) return false;
        }
        return true;
    }
    bool Value(unsigned depth, String* text) {
        Skip();
        if (next_ == end_ || depth > 16) return false;
        switch (*next_) {
        case '"': return Quoted(text);
        case '{': return Object(depth);
        case '[':
            ++next_; Skip();
            if (Take(']')) return true;
            do {
                String ignored;
                if (!Value(depth + 1, &ignored)) return false;
                Skip();
                if (Take(']')) return true;
                if (!Take(',')) return false;
            } while (true);
        case 't': return Literal("true");
        case 'f': return Literal("false");
        case 'n': return Literal("null");
        default: return Number();
        }
    }
    bool Object(unsigned depth) {
        if (depth > 16 || !Take('{')) return false;
        Skip();
        if (Take('}')) return true;
        do {
            String key, text;
            Skip();
            if (!Quoted(&key)) return false;
            // Escaped root keys could be a duplicate of type/audio. Leave
            // those to cJSON, which preserves its established first-key rule.
            if (!depth && memchr(key.data, '\\', key.bytes)) return false;
            Skip();
            if (!Take(':') || !Value(depth + 1, &text)) return false;
            if (!depth && key.Equals("type") && !seen_type_) { type_ = text; seen_type_ = true; }
            if (!depth && key.Equals("audio") && !seen_audio_) { audio_ = text; seen_audio_ = true; }
            Skip();
            if (Take('}')) return true;
            if (!Take(',')) return false;
        } while (true);
    }
    uint8_t* next_;
    uint8_t* end_;
    String type_, audio_;
    bool seen_type_ = false, seen_audio_ = false;
};
}  // namespace audio_json

// On success the audio points inside the caller-owned writable JSON message.
// Only that string's bytes may change. The view expires when delivery returns.
inline bool BorrowAudioDelta(uint8_t* data, size_t bytes, uint8_t** audio, size_t* audio_bytes) {
    audio_json::String value;
    if (!audio_json::Reader(data, bytes).Read(&value)) return false;
    *audio = value.data; *audio_bytes = value.bytes;
    return true;
}
}  // namespace agentlink::realtime
