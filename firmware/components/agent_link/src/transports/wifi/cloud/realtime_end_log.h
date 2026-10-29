#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "cJSON.h"

namespace agentlink::realtime {

struct EndDiagnostic {
    char event[24]{};
    char code[64]{};
    char reason[160]{};
};

inline bool DiagnosticWordEqual(const char* word, size_t size, const char* expected) {
    if (std::strlen(expected) != size) return false;
    for (size_t i = 0; i < size; ++i)
        if (std::tolower(static_cast<unsigned char>(word[i])) != expected[i]) return false;
    return true;
}

// Only selected terminal fields reach this formatter. Never serialize a server
// object. Redact known credentials before truncation, URL/identifier-like words,
// and values following credential labels; normalize controls to one log line.
inline void SafeEndField(char* out, size_t capacity, const char* text,
                         const char* auth_key, const char* connect_token) {
    if (!out || !capacity) return;
    size_t used = 0;
    bool full = false;
    auto append = [&](const char* word, size_t bytes) {
        for (size_t i = 0; i < bytes && used + 1 < capacity;) {
            const unsigned char c = static_cast<unsigned char>(word[i]);
            const size_t width = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
            if (i + width > bytes || used + width >= capacity) { full = true; break; }
            std::memcpy(out + used, word + i, width); used += width; i += width;
        }
    };
    bool credential_value = false;
    const char* p = text ? text : "unspecified";
    while (*p && used + 1 < capacity && !full) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isspace(c) || c < 0x20 || c == 0x7f) {
            if (used && out[used - 1] != ' ') out[used++] = ' ';
            ++p; continue;
        }
        if (*p == '"' || *p == '\'' || *p == '{' || *p == '}' || *p == '[' || *p == ']' || *p == ',') {
            out[used++] = ' '; ++p; continue;
        }
        const char* begin = p;
        while (*p && !std::isspace(static_cast<unsigned char>(*p)) &&
               static_cast<unsigned char>(*p) >= 0x20 && static_cast<unsigned char>(*p) != 0x7f &&
               *p != '"' && *p != '\'' && *p != '{' && *p != '}' && *p != '[' && *p != ']' && *p != ',') ++p;
        const size_t length = static_cast<size_t>(p - begin);
        bool secret = false, label = false, assigned = false;
        for (const char* key : {auth_key, connect_token}) {
            if (!key || !key[0]) continue;
            const size_t bytes = std::strlen(key);
            for (size_t i = 0; i < length; ++i)
                if (std::strncmp(begin + i, key, bytes) == 0) { secret = true; break; }
        }
        size_t name = 0;
        while (name < length && begin[name] != ':' && begin[name] != '=') ++name;
        for (const char* key : {"token", "connect_token", "access_token", "refresh_token", "auth_key",
                               "api_key", "key", "authorization", "bearer", "secret", "password",
                               "credential", "session_id", "device_sn", "device_id", "request_id", "id",
                               "client_secret", "access-token", "connect-token", "auth-key", "api-key", "x-api-key"}) {
            if (DiagnosticWordEqual(begin, name, key)) {
                label = true; assigned = name + 1 < length; break;
            }
        }
        bool safe_status = false;
        for (const char* status : {"expired", "invalid", "missing", "denied", "failed", "revoked",
                                  "required", "unavailable", "timeout"})
            safe_status = safe_status || DiagnosticWordEqual(begin, length, status);
        bool public_code = false;
        for (const char* code : {"session_duration_exceeded", "session_max_duration_exceeded",
                                "session_duration_limit_exceeded", "max_session_duration_exceeded",
                                "maximum_session_duration_exceeded", "authentication_failed"})
            public_code = public_code || DiagnosticWordEqual(begin, length, code);
        size_t ascii_run = 0;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char ch = static_cast<unsigned char>(begin[i]);
            ascii_run = ch < 0x80 && (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') ? ascii_run + 1 : 0;
            if (ascii_run >= 24 && !public_code) secret = true;
            if (i + 2 < length && begin[i] == ':' && begin[i + 1] == '/' && begin[i + 2] == '/') secret = true;
        }
        if (secret || assigned || (credential_value && !label && !safe_status))
            append("[redacted]", 10);
        else append(begin, length);
        // A bare '=' or ':' between a label and its value does not consume it.
        if (label) credential_value = !assigned;
        else if (!DiagnosticWordEqual(begin, length, "=") && !DiagnosticWordEqual(begin, length, ":") &&
                 !DiagnosticWordEqual(begin, length, "is") && !DiagnosticWordEqual(begin, length, "was") &&
                 !DiagnosticWordEqual(begin, length, "equals"))
            credential_value = false;
    }
    out[used] = 0;
}

inline const cJSON* EndField(const cJSON* root, const char* first, const char* second) {
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* session = cJSON_GetObjectItemCaseSensitive(root, "session");
    const cJSON* objects[] = {cJSON_GetObjectItemCaseSensitive(root, "error"),
                              cJSON_GetObjectItemCaseSensitive(data, "error"), root, data, session};
    for (const cJSON* object : objects) {
        for (const char* key : {first, second}) {
            const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
            if (cJSON_IsObject(value)) {
                const cJSON* message = cJSON_GetObjectItemCaseSensitive(value, "message");
                value = message ? message : cJSON_GetObjectItemCaseSensitive(value, "code");
            }
            if (cJSON_IsString(value) || cJSON_IsNumber(value)) return value;
        }
    }
    return nullptr;
}

inline bool DescribeServerEnd(const cJSON* root, const char* auth_key, const char* connect_token,
                               EndDiagnostic* out) {
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!out || !cJSON_IsString(type)) return false;
    const char* event = type->valuestring;
    if (std::strcmp(event, "error") && std::strcmp(event, "session.error") && std::strcmp(event, "session.ended"))
        return false;
    *out = {};
    std::snprintf(out->event, sizeof out->event, "%s", event);
    auto field = [&](char* target, size_t capacity, const cJSON* value) {
        if (cJSON_IsNumber(value)) std::snprintf(target, capacity, "%.16g", value->valuedouble);
        else SafeEndField(target, capacity, cJSON_IsString(value) ? value->valuestring : nullptr,
                          auth_key, connect_token);
    };
    field(out->code, sizeof out->code, EndField(root, "code", "error_code"));
    const cJSON* reason = EndField(root, "reason", "message");
    if (!reason) reason = cJSON_GetObjectItemCaseSensitive(root, "error");
    field(out->reason, sizeof out->reason, reason);
    return true;
}

}  // namespace agentlink::realtime
