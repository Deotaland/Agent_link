#pragma once

#include <cstddef>
#include <cstring>

// Log the top-level business message only, never a request body or response data.
// Some validation messages echo a submitted value; redact both the stored and submitted key.
inline void cloud_log_safe_message(char* out, size_t capacity, const char* message,
                                   const char* stored_key, const char* submitted_key = nullptr) {
    if (!out || !capacity) return;
    const char* text = message ? message : "";
    const char* keys[] = {stored_key, submitted_key};
    size_t used = 0;
    while (*text && used + 1 < capacity) {
        size_t match = 0;
        for (const char* key : keys) {
            if (key && key[0]) {
                const size_t length = std::strlen(key);
                if (length > match && std::strncmp(text, key, length) == 0) match = length;
            }
        }
        if (match) {
            constexpr const char replacement[] = "[redacted]";
            for (const char* p = replacement; *p && used + 1 < capacity; ++p) out[used++] = *p;
            text += match;
        } else {
            const unsigned char c = static_cast<unsigned char>(*text++);
            out[used++] = c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
        }
    }
    out[used] = '\0';
}
