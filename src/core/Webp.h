#pragma once

// WebP decoding (RFC 9649): the RIFF container, simple and extended; lossy
// images, a VP8 key frame (RFC 6386) — the boolean entropy decoder, the
// segment, quantizer and loop-filter headers, intra prediction in 16x16,
// 4x4 and chroma blocks, the token tree, the inverse DCT and Walsh-Hadamard
// transforms and the normal and simple loop filters — turned into RGB with
// the chroma upsampled as libwebp upsamples it, so a picture is the same
// pixels every engine shows; lossless images (VP8L) with their transforms,
// colour cache and meta prefix codes; and the alpha chunk of a lossy image,
// raw or lossless-coded, with its line filters. An animation shows its
// first frame on its canvas. Every size and every code is bounded; a stream
// that ends early is refused.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

std::optional<Bitmap> decode_webp(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);
bool looks_like_webp(std::vector<std::uint8_t> const& bytes);

}
