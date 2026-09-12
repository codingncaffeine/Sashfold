#pragma once

// BMP and ICO decoding: the one DIB family behind both. A BMP is a file
// header over a DIB — a BITMAPCOREHEADER or one of the BITMAPINFOHEADER
// line (V2 through V5) — with 1, 4, 8, 16, 24 or 32 bits per pixel, a
// palette for the small depths, BI_RGB, BI_BITFIELDS with the masks the
// header carries, and the RLE4 and RLE8 run lengths, bottom-up or
// top-down. An ICO is a directory of pictures, each a DIB with an AND mask
// stacked under its pixels or, since Vista, a PNG. Every size and every
// offset is checked against the bytes in hand, so hostile input cannot
// escape.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

// The picture as RGBA8; nullopt for anything malformed, truncated, or
// larger than max_pixels.
std::optional<Bitmap> decode_bmp(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

// The directory entry nearest the size wanted — the one of that width, else
// the smallest wider one, else the largest of all; a `wanted` of zero takes
// the largest — decoded, its AND mask applied where it has no alpha of
// its own.
std::optional<Bitmap> decode_ico(std::vector<std::uint8_t> const& bytes, int wanted = 0,
    std::size_t max_pixels = 32u * 1024u * 1024u);

// The signatures: "BM", and the ICO directory's reserved zero, type and a
// plausible count.
bool looks_like_bmp(std::vector<std::uint8_t> const& bytes);
bool looks_like_ico(std::vector<std::uint8_t> const& bytes);

}
