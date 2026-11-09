#pragma once

// The tables of ITU-T H.264 the software decoder reads, generated from the
// Recommendation by tools/gen-h264-tables.py (H264Data.cpp).

#include <cstdint>

namespace sashfold::media::h264data {

struct CabacInit {
    std::int8_t m;
    std::int8_t n;
};

// A variable-length code: its bits, right-aligned, and how many; length 0
// is no code.
struct VlcCode {
    std::uint16_t bits;
    std::uint8_t length;
};

// [ctxIdx][0: I slices, 1 + cabac_init_idc]
extern CabacInit const cabac_init[1024][4];
extern std::uint8_t const range_tab_lps[64][4];
extern std::uint8_t const trans_idx_lps[64];
extern std::uint8_t const trans_idx_mps[64];

// [nC class][TrailingOnes][TotalCoeff]; the classes are 0 <= nC < 2,
// 2 <= nC < 4, 4 <= nC < 8, 8 <= nC, nC == -1 (chroma DC 4:2:0), nC == -2.
extern VlcCode const coeff_token[6][4][17];
extern VlcCode const total_zeros_4x4[15][16]; // [tzVlcIndex - 1][total_zeros]
extern VlcCode const total_zeros_2x2[3][4];
extern VlcCode const total_zeros_2x4[7][8];
extern VlcCode const run_before[7][15]; // [min(zerosLeft, 7) - 1][run_before]

extern std::uint8_t const cbp_intra[48];
extern std::uint8_t const cbp_inter[48];

extern std::uint8_t const significant_8x8_frame[63];
extern std::uint8_t const last_significant_8x8[63];

}
