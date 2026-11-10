#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "cJSON.h"

namespace paper {
inline bool Integer(const cJSON* object, const char* key, int lo, int hi) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(v) && v->valuedouble >= lo && v->valuedouble <= hi && v->valuedouble == v->valueint;
}
inline bool SafeTrack(const char* name) {
    if (!name || !*name || std::strlen(name) > 127 || std::strstr(name, "..") ||
        std::strchr(name, '/') || std::strchr(name, '\\')) return false;
    const size_t n = std::strlen(name);
    return n > 4 && (std::strcmp(name + n - 4, ".wav") == 0 || std::strcmp(name + n - 4, ".WAV") == 0);
}
inline bool ValidTool(const char* name, const cJSON* args) {
    if (!name || !cJSON_IsObject(args)) return false;
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(args, "value");
    if (std::strcmp(name, "motor0") == 0) return Integer(args, "value", 0, 2000);
    if (std::strcmp(name, "screen0") == 0) return cJSON_IsString(value) && std::strlen(value->valuestring) <= 2048;
    if (std::strcmp(name, "led0") == 0) {
        if (!cJSON_IsString(value) || value->valuestring[0] != '#' || std::strlen(value->valuestring) != 7) return false;
        return std::strspn(value->valuestring + 1, "0123456789abcdefABCDEF") == 6;
    }
    if (std::strcmp(name, "alarm0") == 0)
        return Integer(args, "hour", 0, 23) && Integer(args, "minute", 0, 59) &&
               cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(args, "enabled"));
    if (std::strcmp(name, "music0") == 0) {
        const char* action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "action"));
        if (!action) return false;
        if (std::strcmp(action, "stop") == 0 || std::strcmp(action, "list") == 0) return true;
        return std::strcmp(action, "play") == 0 && SafeTrack(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "file")));
    }
    return false;
}
// Validate the entire completed reply before any caller may execute its first action.
class ToolReply {
public:
    ToolReply() = default;
    ToolReply(const ToolReply&) = delete;
    ToolReply& operator=(const ToolReply&) = delete;
    ~ToolReply() { cJSON_Delete(root_); }
    bool Parse(const char* text) {
        cJSON_Delete(root_); root_ = nullptr;
        if (!text || std::strlen(text) > 32768) return false;
        const char* end = nullptr;
        root_ = cJSON_ParseWithOpts(text, &end, true);
        if (!cJSON_IsObject(root_) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root_, "text"))) return false;
        cJSON* tools = cJSON_GetObjectItemCaseSensitive(root_, "tools");
        if (!cJSON_IsArray(tools) || cJSON_GetArraySize(tools) > 8) return false;
        cJSON* tool;
        cJSON_ArrayForEach(tool, tools) {
            const char* name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(tool, "name"));
            if (!ValidTool(name, cJSON_GetObjectItemCaseSensitive(tool, "arguments"))) return false;
        }
        return true;
    }
    const char* Text() const { return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root_, "text")); }
    cJSON* Tools() const { return cJSON_GetObjectItemCaseSensitive(root_, "tools"); }
private:
    cJSON* root_ = nullptr;
};
inline unsigned U16(const uint8_t* p) { return p[0] | unsigned(p[1]) << 8; }
inline uint32_t U32(const uint8_t* p) { return U16(p) | uint32_t(U16(p + 2)) << 16; }
struct Wav {
    unsigned rate = 0, channels = 0;
    uint32_t bytes = 0;
};
inline bool ReadWav(FILE* file, Wav& wav) {
    uint8_t header[12];
    if (!file || std::fseek(file, 0, SEEK_END) != 0) return false;
    const long size = std::ftell(file);
    if (size < 44 || size > 512 * 1024 * 1024 || std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, 12, file) != 12 || std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4)) return false;
    const uint64_t limit = uint64_t(U32(header + 4)) + 8;
    if (limit > uint64_t(size) || limit < 44) return false;
    bool format = false;
    long data = -1;
    while (uint64_t(std::ftell(file)) + 8 <= limit) {
        uint8_t chunk[8];
        if (std::fread(chunk, 1, 8, file) != 8) return false;
        const uint32_t bytes = U32(chunk + 4);
        const long position = std::ftell(file);
        if (uint64_t(position) + bytes + (bytes & 1) > limit) return false;
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (bytes < 16 || std::fread(fmt, 1, 16, file) != 16 || U16(fmt) != 1 || U16(fmt + 14) != 16) return false;
            wav.channels = U16(fmt + 2); wav.rate = U32(fmt + 4);
            if ((wav.channels != 1 && wav.channels != 2) || wav.rate < 8000 || wav.rate > 48000 ||
                U16(fmt + 12) != wav.channels * 2 || U32(fmt + 8) != wav.rate * wav.channels * 2) return false;
            format = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) { data = position; wav.bytes = bytes; }
        if (format && data >= 0) break;
        if (std::fseek(file, position + bytes + (bytes & 1), SEEK_SET) != 0) return false;
    }
    return format && data >= 0 && wav.bytes > 0 && wav.bytes % (wav.channels * 2) == 0 &&
           std::fseek(file, data, SEEK_SET) == 0;
}
inline bool Leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
inline int MonthDays(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return month >= 1 && month <= 12 ? days[month - 1] + (month == 2 && Leap(year)) : 0;
}
}
