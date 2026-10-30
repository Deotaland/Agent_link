#pragma once
#include <array>
#include "pcm_levels.h"
#include <cstddef>
#include <cstdint>

namespace amoled_voice {
using kc_audio::PcmLevels;

struct MicDiagnosticWindow {
    PcmLevels raw, uplink;
    std::array<PcmLevels, 2> channels;  // 16 kHz standard I2S: MIC1, MIC2
    uint32_t sequence = 0, capture = 0;
    int64_t started_us = 0, ended_us = 0;
    uint32_t voice_frames = 0, asr_frames = 0, echo_muted_frames = 0;
    uint32_t voice_drops = 0, write_errors = 0, read_errors = 0;
    int32_t last_read_error = 0, last_write_error = 0;
    uint32_t mic_stack_min = 0, play_stack_min = 0;
    bool final = false;
};
}  // namespace amoled_voice
