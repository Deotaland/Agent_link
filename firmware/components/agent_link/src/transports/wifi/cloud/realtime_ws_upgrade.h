#pragma once

#include <cctype>
#include <cstring>

namespace agentlink::realtime {

inline bool EqualHeader(const char* a, size_t bytes, const char* b) {
    if (bytes != std::strlen(b)) return false;
    for (size_t i = 0; i < bytes; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

inline bool HeaderToken(const char* value, size_t bytes, const char* token) {
    while (bytes) {
        const char* end = static_cast<const char*>(std::memchr(value, ',', bytes));
        size_t length = end ? static_cast<size_t>(end - value) : bytes;
        size_t trimmed = length;
        while (trimmed && (value[trimmed - 1] == ' ' || value[trimmed - 1] == '\t')) --trimmed;
        size_t start = 0;
        while (start < trimmed && (value[start] == ' ' || value[start] == '\t')) ++start;
        if (EqualHeader(value + start, trimmed - start, token)) return true;
        if (!end) break;
        value += length + 1; bytes -= length + 1;
    }
    return false;
}

// Validate a complete, bounded HTTP upgrade. Accept/protocol are case-sensitive
// values, unlike header names and the Upgrade/Connection tokens.
inline bool ValidUpgrade(const char* header, const char* accept, int* status) {
    *status = 0;
    if (std::strncmp(header, "HTTP/1.1 ", 9) != 0) return false;
    for (unsigned i = 9; i < 12; ++i) {
        if (header[i] < '0' || header[i] > '9') return false;
        *status = *status * 10 + header[i] - '0';
    }
    if (*status != 101 || (header[12] != ' ' && header[12] != '\r')) return false;
    const char* line = std::strstr(header, "\r\n");
    if (!line) return false;
    unsigned seen = 0;
    for (line += 2; *line; ) {
        const char* end = std::strstr(line, "\r\n");
        if (!end) return false;
        if (line == end) return seen == 15;
        const char* colon = static_cast<const char*>(std::memchr(line, ':', end - line));
        if (!colon) return false;
        const char* value = colon + 1;
        while (value < end && (*value == ' ' || *value == '\t')) ++value;
        const char* value_end = end;
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
        const size_t bytes = static_cast<size_t>(value_end - value);
        unsigned bit = 0;
        bool valid = true;
        if (EqualHeader(line, colon - line, "Upgrade")) {
            bit = 1; valid = EqualHeader(value, bytes, "websocket");
        } else if (EqualHeader(line, colon - line, "Connection")) {
            bit = 2; valid = HeaderToken(value, bytes, "upgrade");
        } else if (EqualHeader(line, colon - line, "Sec-WebSocket-Accept")) {
            bit = 4; valid = bytes == std::strlen(accept) && std::memcmp(value, accept, bytes) == 0;
        } else if (EqualHeader(line, colon - line, "Sec-WebSocket-Protocol")) {
            bit = 8; valid = bytes == sizeof("tidb-link.realtime.v1") - 1 &&
                            std::memcmp(value, "tidb-link.realtime.v1", bytes) == 0;
        } else if (EqualHeader(line, colon - line, "Sec-WebSocket-Extensions")) return false;
        if (!valid || (seen & bit)) return false;
        seen |= bit;
        line = end + 2;
    }
    return false;
}

}  // namespace agentlink::realtime
