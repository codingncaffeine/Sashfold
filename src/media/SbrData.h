#pragma once

// The tables of ISO/IEC 14496-3 §4.6.18 an SBR decoder reads, written by
// tools/gen-sbr-tables.py (SbrData.cpp): the envelope and noise floor
// codebooks, the start frequency tables, the QMF prototype filter, the
// noise generator's phases, and the limiter's and smoother's constants.

#include <cstddef>
#include <cstdint>

namespace sashfold::media::sbr_data {

struct HuffmanEntry {
    std::int8_t delta; // the decoded delta
    std::uint8_t length; // bits
    std::uint32_t code;
};

struct HuffmanBook {
    int lav; // the largest absolute value the book codes
    HuffmanEntry const* entries; // sorted by length, then code
    std::size_t count;
};

// 0 env level 1.5 dB time, 1 freq; 2 env balance 1.5 dB time, 3 freq;
// 4 env level 3 dB time, 5 freq; 6 env balance 3 dB time, 7 freq;
// 8 noise level time; 9 noise balance time. (Noise in the frequency
// direction uses books 5 and 7.)
extern HuffmanBook const books[10];

// Table 4.A.78 by the core rate's band, index = start_freq (0 to 15).
extern std::uint8_t const start_freq_16[16];
extern std::uint8_t const start_freq_22[16];
extern std::uint8_t const start_freq_24[16];
extern std::uint8_t const start_freq_32[16];
extern std::uint8_t const start_freq_44[16];
extern std::uint8_t const start_freq_48[16];

extern float const qmf_prototype[640];
extern float const random_phase[512][2];
extern float const limiter_gains[4];
extern float const limiter_bands_per_octave[4];
extern float const smooth_filter[4];

}
