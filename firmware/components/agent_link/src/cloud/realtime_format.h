#pragma once

#include <cstring>
#include <initializer_list>
#include "cJSON.h"

namespace agentlink::realtime {

struct AudioFormat {
    bool input_opus = false;
    bool output_opus = false;
};

inline constexpr char kOpusAudioConfig[] =
    R"({"input":{"encoding":"opus","sampleRate":16000,"channels":1,"packetDurationMs":20},)"
    R"("output":{"encoding":"opus","sampleRate":24000,"channels":1,"packetDurationMs":20,"bitrate":24000}})";

inline const cJSON* AudioSettings(const cJSON* body, unsigned depth = 0) {
    if (!cJSON_IsObject(body) || depth > 3) return nullptr;
    for (const char* key : {"audio_config", "audio"}) {
        const cJSON* audio = cJSON_GetObjectItemCaseSensitive(body, key);
        if (cJSON_IsObject(audio)) return audio;
    }
    for (const char* key : {"realtimeConnection", "grant", "data"}) {
        const cJSON* audio = AudioSettings(cJSON_GetObjectItemCaseSensitive(body, key), depth + 1);
        if (audio) return audio;
    }
    return nullptr;
}

inline const cJSON* AudioField(const cJSON* body, const char* snake, const char* camel) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(body, snake);
    return value ? value : cJSON_GetObjectItemCaseSensitive(body, camel);
}

inline bool AudioDirection(const cJSON* direction, int rate, bool* opus) {
    const cJSON* encoding = cJSON_GetObjectItemCaseSensitive(direction, "encoding");
    const cJSON* sample_rate = AudioField(direction, "sample_rate", "sampleRate");
    const cJSON* channels = cJSON_GetObjectItemCaseSensitive(direction, "channels");
    if (!cJSON_IsString(encoding) || !cJSON_IsNumber(sample_rate) || sample_rate->valuedouble != rate ||
        !cJSON_IsNumber(channels) || channels->valuedouble != 1) return false;
    if (std::strcmp(encoding->valuestring, "pcm16") == 0) {
        *opus = false;
        return true;
    }
    if (std::strcmp(encoding->valuestring, "opus") != 0) return false;
    const cJSON* duration = AudioField(direction, "packet_duration_ms", "packetDurationMs");
    if (duration && (!cJSON_IsNumber(duration) || duration->valuedouble != 20)) return false;
    *opus = true;
    return true;
}

// Grants describe both directions. session.ready may repeat only the output settings.
inline bool ReadAudioFormat(const cJSON* body, AudioFormat* format, bool require_input) {
    const cJSON* audio = AudioSettings(body);
    const cJSON* input = cJSON_GetObjectItemCaseSensitive(audio, "input");
    const cJSON* output = cJSON_GetObjectItemCaseSensitive(audio, "output");
    AudioFormat parsed = *format;
    if ((input || require_input) && !AudioDirection(input, 16000, &parsed.input_opus)) return false;
    if (!AudioDirection(output, 24000, &parsed.output_opus)) return false;
    *format = parsed;
    return true;
}

} // namespace agentlink::realtime
