#pragma once
// IMA-ADPCM decoder for the App -> device audio downlink (WAV fmt_tag 0x0011, 4:1).
//
// Block layout (block_align = 256 bytes = 505 samples ~= 31.5ms @16kHz):
//   [0..1]   predictor  int16 LE  - sample 0 of the block, verbatim
//   [2]      step_index uint8     - 0..88
//   [3]      reserved   = 0
//   [4..255] 252 bytes = 504 nibbles, LOW nibble first, samples 1..504
//
// The property that matters: every block carries its own predictor + step_index, so decoder
// state never crosses a block boundary. Losing a whole block costs 31.5ms and the next block
// recovers on its own - whereas losing an odd number of raw-PCM bytes swaps the high/low byte
// of every following sample and turns the rest of the stream into white noise permanently.
#include <cstddef>
#include <cstdint>

namespace agentlink {
namespace adpcm {

inline constexpr size_t kBlockBytes       = 256;   // block_align
inline constexpr size_t kSamplesPerBlock  = 505;   // ((256-4)*2) + 1
inline constexpr size_t kPcmBytesPerBlock = kSamplesPerBlock * sizeof(int16_t);  // 1010

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

// Decode one nibble, advancing predictor / step_index.
inline int16_t DecodeSample(uint8_t nibble, int16_t& predictor, int8_t& step_index) {
    const int32_t step = kStepTable[step_index];

    int32_t diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;

    int32_t pred = predictor;
    if (nibble & 8) pred -= diff;
    else            pred += diff;
    if (pred >  32767) pred =  32767;
    if (pred < -32768) pred = -32768;
    predictor = static_cast<int16_t>(pred);

    int new_idx = step_index + kIndexTable[nibble];
    if (new_idx <  0) new_idx =  0;
    if (new_idx > 88) new_idx = 88;
    step_index = static_cast<int8_t>(new_idx);

    return predictor;
}

// Decode one complete 256B block into 505 int16 samples (out needs kPcmBytesPerBlock bytes).
// Returns the sample count (always kSamplesPerBlock). The block must be a full 256 bytes.
inline size_t DecodeBlock(const uint8_t* block, int16_t* out) {
    int16_t predictor = static_cast<int16_t>(
        static_cast<uint16_t>(block[0]) | (static_cast<uint16_t>(block[1]) << 8));
    int8_t step_index = static_cast<int8_t>(block[2]);
    if (step_index < 0)  step_index = 0;
    if (step_index > 88) step_index = 88;

    out[0] = predictor;                       // sample 0 verbatim
    const uint8_t* in = block + 4;
    size_t n = 1;
    for (size_t i = 0; i < (kBlockBytes - 4); ++i) {
        const uint8_t byte = in[i];
        out[n++] = DecodeSample(static_cast<uint8_t>(byte & 0x0F), predictor, step_index);
        out[n++] = DecodeSample(static_cast<uint8_t>(byte >> 4),   predictor, step_index);
    }
    return n;                                 // = kSamplesPerBlock
}

}  // namespace adpcm
}  // namespace agentlink
