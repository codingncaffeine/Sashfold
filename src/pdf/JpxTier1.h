#pragma once

// JPEG 2000's code-block coder, tier 1 (ITU-T T.800 Annex D): a block of
// a subband's quantized coefficients coded bit-plane by bit-plane, most
// significant first, in three passes a plane — significance propagation,
// magnitude refinement and cleanup — each decision by the MQ coder in one
// of 19 contexts, or (with the bypass option) the first two passes of
// the lower planes as raw bits.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sashfold::pdf::jpx {

// The code-block style of COD/COC (Table A.19).
struct BlockStyle {
    bool bypass = false; // selective arithmetic coding bypass
    bool reset = false; // contexts reset after each pass
    bool terminate_all = false; // every pass terminated
    bool causal = false; // vertically causal contexts
    bool predictable = false; // predictable termination (nothing to do when decoding)
    bool segmentation = false; // segmentation symbols after each cleanup
};

enum class Band { LL, HL, LH, HH };

// A run of passes coded as one codeword segment, and its bytes.
struct Segment {
    std::span<std::uint8_t const> data;
    int passes = 0;
};

// How many passes, from pass `first` (counted from the block's first, a
// cleanup), the segment that starts there may hold: one with every pass
// terminated; with bypass the first ten (the four top planes) together,
// then the raw pairs and the cleanups apart; otherwise all of them.
int segment_pass_limit(BlockStyle const& style, int first);

// Decodes a block's passes into `coefficients` (width * height, row by
// row): each the coefficient's signed magnitude times two, with the half
// step added past the last plane decoded (T.800 E.1.1.2, r = 1/2), so the
// caller divides by two. `planes` is the number of bit-planes coded (Mb
// minus the zero planes the packet header said). False when the data
// cannot be what it says; what was decoded stands.
bool decode_code_block(std::span<Segment const> segments, int width, int height, int planes, Band band,
    BlockStyle const& style, std::vector<std::int32_t>& coefficients);

}
