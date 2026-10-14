#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace kc_ui {

inline void CopyVoiceText(char* out, size_t capacity, const char* text) {
    if (!capacity) return;
    size_t size = 0;
    bool words = false, unsupported = false, unknown_run = false;
    const auto append = [&](char c) { if (size + 1 < capacity) out[size++] = c; };
    const auto* p = reinterpret_cast<const uint8_t*>(text ? text : "");
    while (*p && size + 1 < capacity) {
        if (*p < 128) {
            const char c = static_cast<char>(*p++);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) words = true;
            if (c >= 32 && c < 127) { append(c); unknown_run = false; }
            else if (c == '\n') { append(c); unknown_run = false; }
            else if (c == '\r' || c == '\t') append(' ');
            continue;
        }
        unsigned count = *p >= 0xf0 && *p <= 0xf4 ? 4 : *p >= 0xe0 && *p <= 0xef ? 3
                       : *p >= 0xc2 && *p <= 0xdf ? 2 : 1;
        uint32_t code = *p & (count == 4 ? 7 : count == 3 ? 15 : count == 2 ? 31 : 255);
        unsigned used = 1;
        while (used < count && p[used] && (p[used] & 0xc0) == 0x80) {
            code = (code << 6) | (p[used] & 0x3f);
            ++used;
        }
        p += used;
        char replacement = 0;
        if (used == count) {
            if (code == 0xa0) replacement = ' ';
            else if (code == 0x2018 || code == 0x2019) replacement = '\'';
            else if (code == 0x201c || code == 0x201d) replacement = '"';
            else if (code == 0x2013 || code == 0x2014) replacement = '-';
            else if (code == 0x2026) { append('.'); append('.'); replacement = '.'; }
        }
        if (replacement) { append(replacement); unknown_run = false; }
        else {
            unsupported = true;
            if (!unknown_run) append('?');
            unknown_run = true;
        }
    }
    out[size] = '\0';
    if (!words && unsupported) {
        constexpr char fallback[] = "Non-English transcript";
        const size_t bytes = sizeof fallback < capacity ? sizeof fallback : capacity;
        memcpy(out, fallback, bytes - 1);
        out[bytes - 1] = '\0';
    }
}

}  // namespace kc_ui
