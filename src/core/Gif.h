#pragma once

// GIF decoding (87a and 89a): the logical screen, global and local color
// tables, LZW image data, interlaced rows, and the graphic control
// extension's transparent index. The first frame becomes the picture;
// animation waits. Every table walk and every size is bounded, so hostile
// input cannot escape.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

// The first frame composed onto a transparent logical screen; nullopt for
// anything malformed, or larger than max_pixels.
std::optional<Bitmap> decode_gif(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

bool looks_like_gif(std::vector<std::uint8_t> const& bytes);

}
