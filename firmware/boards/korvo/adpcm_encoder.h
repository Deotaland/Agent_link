#pragma once
// IMA-ADPCM encoder — the exact inverse of the decoder in agent_link's src/adpcm.h, so anything
// written here decodes with that and with any standard WAV player (fmt_tag 0x0011, 4:1).
//
// That decoder is private to the component (src/, not include/), so its tables are reproduced
// here rather than shared. They are the standard IMA tables, not something either side invented.
//
// Block layout, matching src/adpcm.h byte for byte:
//   [0..1]   predictor  int16 LE  - sample 0 of the block, verbatim
//   [2]      step_index uint8     - 0..88
//   [3]      reserved   = 0
//   [4..255] 252 bytes = 504 nibbles, LOW nibble first, samples 1..504
//
// Each block restates its own predictor and step_index, so a lost block costs 31.5ms and the next
// one recovers on its own. That is the whole reason for the format: with raw PCM, losing an odd
// number of bytes swaps every following sample's halves and ruins the rest of the file.

#include <cstddef>
#include <cstdint>

namespace adpcm_enc {

inline constexpr size_t kBlockBytes      = 256;   // block_align
inline constexpr size_t kSamplesPerBlock = 505;   // ((256-4)*2) + 1

inline constexpr int16_t kStepTable[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

inline constexpr int8_t kIndexTable[16] = {
    -1, -1, -1, -1,  2,  4,  6,  8,
    -1, -1, -1, -1,  2,  4,  6,  8,
};

// Quantise one sample against the running predictor, returning its 4-bit code and advancing the
// predictor / step_index exactly the way the decoder will when it sees that code.
inline uint8_t EncodeSample(int16_t sample, int16_t& predictor, int8_t& step_index) {
    const int32_t step = kStepTable[step_index];

    int32_t diff = static_cast<int32_t>(sample) - predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }   // sign bit

    // Three magnitude bits, each worth half the previous one. vpdiff accumulates exactly what the
    // decoder will reconstruct, so both sides stay on the same predictor.
    int32_t vpdiff = step >> 3;
    if (diff >= step)      { code |= 4; diff -= step;      vpdiff += step; }
    if (diff >= (step >> 1)) { code |= 2; diff -= step >> 1; vpdiff += step >> 1; }
    if (diff >= (step >> 2)) { code |= 1;                    vpdiff += step >> 2; }

    int32_t pred = predictor;
    if (code & 8) pred -= vpdiff;
    else          pred += vpdiff;
    if (pred >  32767) pred =  32767;
    if (pred < -32768) pred = -32768;
    predictor = static_cast<int16_t>(pred);

    int idx = step_index + kIndexTable[code];
    if (idx <  0) idx =  0;
    if (idx > 88) idx = 88;
    step_index = static_cast<int8_t>(idx);

    return code;
}

// Streaming encoder: feed samples, take a 256-byte block every 505 of them.
//
// step_index carries across blocks (the predictor does not — each block restates it), which is
// what keeps the quantiser converged at a block boundary instead of restarting coarse.
class Encoder {
public:
    void Reset() { count_ = 0; step_index_ = 0; }

    // Add one sample. Returns true when `block` has been filled with a complete 256-byte block.
    bool Push(int16_t sample, uint8_t* block) {
        pcm_[count_++] = sample;
        if (count_ < kSamplesPerBlock) return false;
        EncodeBlock(block);
        count_ = 0;
        return true;
    }

    // Flush a partial block, padding the tail. Returns 0 if nothing is pending. A file should end
    // on one of these or its last few hundred ms are simply lost; the padding is why the WAV's
    // fact chunk has to carry the true sample count, so a player stops at the real end.
    size_t Flush(uint8_t* block) {
        if (count_ == 0) return 0;
        // Pad by holding the last real sample rather than jumping to zero, which would click.
        // Hoisted out of the loop on purpose: reading pcm_[count_ - 1] in the same expression that
        // increments count_ is unsequenced, and GCC rejects it (-Werror=sequence-point).
        const int16_t hold = pcm_[count_ - 1];
        while (count_ < kSamplesPerBlock) pcm_[count_++] = hold;
        EncodeBlock(block);
        count_ = 0;
        return kBlockBytes;
    }

    size_t Pending() const { return count_; }

private:
    void EncodeBlock(uint8_t* out) {
        int16_t predictor = pcm_[0];            // sample 0 goes in the header verbatim
        out[0] = static_cast<uint8_t>(predictor & 0xFF);
        out[1] = static_cast<uint8_t>((predictor >> 8) & 0xFF);
        out[2] = static_cast<uint8_t>(step_index_);
        out[3] = 0;

        size_t n = 1;
        for (size_t i = 0; i < (kBlockBytes - 4); ++i) {
            const uint8_t lo = EncodeSample(pcm_[n++], predictor, step_index_);   // low nibble first
            const uint8_t hi = EncodeSample(pcm_[n++], predictor, step_index_);
            out[4 + i] = static_cast<uint8_t>((hi << 4) | lo);
        }
    }

    int16_t pcm_[kSamplesPerBlock] = {};
    size_t  count_      = 0;
    int8_t  step_index_ = 0;
};

}  // namespace adpcm_enc
