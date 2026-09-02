#pragma once

// JPEG decoding, baseline and extended-sequential Huffman: quantization and
// Huffman tables, the entropy-coded scans with byte stuffing and restart
// markers, an integer inverse DCT, chroma upsampling, and YCbCr, grayscale
// and Adobe RGB color. Progressive and arithmetic-coded files are
// recognized and declined for now. Every table index and every size is
// bounded; a stream that ends early leaves the rest of the picture gray.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

std::optional<Bitmap> decode_jpeg(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

bool looks_like_jpeg(std::vector<std::uint8_t> const& bytes);

}
