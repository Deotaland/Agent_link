#pragma once

#include <cstring>
#include <string>
#include "cJSON.h"

namespace agentlink {
// An SSE line can cross arbitrary HTTP packets, including inside a UTF-8 codepoint.
class ChatResponse {
public:
    explicit ChatResponse(size_t capacity) : capacity_(capacity) {}
    bool Feed(const char* bytes, size_t size) {
        if (failed_ || total_ + size > 512 * 1024) return false;
        total_ += size;
        for (size_t i = 0; i < size; ++i) {
            if (bytes[i] == '\n') {
                if (!Line()) return failed_ = true, false;
                line_.clear();
            } else {
                if (line_.size() >= 65536) return failed_ = true, false;
                line_ += bytes[i];
            }
        }
        return true;
    }
    bool Finish() {
        if (!line_.empty() && !Line()) failed_ = true;
        return !failed_ && done_ && !text_.empty();
    }
    const std::string& Text() const { return text_; }
private:
    bool Line() {
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        if (line_.empty() || line_[0] == ':' || line_.compare(0, 6, "event:") == 0) return true;
        if (line_.compare(0, 5, "data:") != 0) return false;
        const char* value = line_.c_str() + 5;
        while (*value == ' ') ++value;
        if (std::strcmp(value, "[DONE]") == 0) { done_ = true; return true; }
        cJSON* json = cJSON_Parse(value);
        if (!json) return false;
        const char* type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "type"));
        bool ok = true;
        if (type && std::strcmp(type, "content") == 0) {
            const char* delta = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "delta"));
            if (!delta) delta = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "content"));
            if (!delta || done_ || text_.size() + std::strlen(delta) >= capacity_) ok = false;
            else text_ += delta;
        } else if (type && std::strcmp(type, "done") == 0) done_ = true;
        else if (type && std::strcmp(type, "error") == 0) ok = false;
        else if (!type) ok = false;
        cJSON_Delete(json);
        return ok;
    }
    size_t capacity_, total_ = 0;
    bool failed_ = false, done_ = false;
    std::string line_, text_;
};
}  // namespace agentlink
