#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace kc_audio {
// Numeric aggregates only; never retain or change the audio waveform.
struct PcmLevels {
    uint32_t frames = 0, samples = 0;
    uint32_t zero_frames = 0, quiet_frames = 0, clipped_samples = 0;
    int32_t minimum = 0, maximum = 0;
    uint32_t peak = 0;
    int64_t sum = 0;
    uint64_t energy = 0;

    void Add(const int16_t* pcm, size_t count, size_t stride = 1) {
        if (!pcm || !count || !stride) return;
        uint32_t frame_peak = 0;
        for (size_t i = 0; i < count; ++i) {
            const int32_t value = pcm[i * stride];
            const uint32_t magnitude = value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
            if (!samples || value < minimum) minimum = value;
            if (!samples || value > maximum) maximum = value;
            if (magnitude > frame_peak) frame_peak = magnitude;
            sum += value;
            energy += static_cast<uint64_t>(value * value);
            if (magnitude >= 32760) ++clipped_samples;
            ++samples;
        }
        ++frames;
        if (!frame_peak) ++zero_frames;
        if (frame_peak <= 64) ++quiet_frames;
        if (frame_peak > peak) peak = frame_peak;
    }
    void Merge(const PcmLevels& other) {
        if (!other.samples) return;
        if (!samples || other.minimum < minimum) minimum = other.minimum;
        if (!samples || other.maximum > maximum) maximum = other.maximum;
        if (other.peak > peak) peak = other.peak;
        frames += other.frames; samples += other.samples;
        zero_frames += other.zero_frames; quiet_frames += other.quiet_frames;
        clipped_samples += other.clipped_samples; sum += other.sum; energy += other.energy;
    }
    uint32_t Rms() const { return samples ? static_cast<uint32_t>(std::sqrt(static_cast<double>(energy) / samples)) : 0; }
    int32_t Mean() const { return samples ? static_cast<int32_t>(sum / samples) : 0; }
};
}  // namespace kc_audio
