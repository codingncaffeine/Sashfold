#pragma once

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sashfold {

// Encodes an RGBA8 PNG. The deflate stream uses stored (uncompressed) blocks,
// which is valid zlib and keeps the encoder dependency-free; every PNG decoder
// accepts it. Compression can come later without changing this interface.
std::vector<std::uint8_t> encode_png(Bitmap const& bitmap);

bool write_png(std::string const& path, Bitmap const& bitmap);

// Decodes a PNG into an RGBA8 bitmap: every color type at every bit depth,
// all five filters, Adam7 interlace, palettes, and tRNS transparency. Critical
// chunks must carry a valid CRC; ancillary ones with a bad CRC are skipped;
// gamma, ICC and sRGB chunks are read past (samples are taken as sRGB).
// nullopt for anything malformed or larger than max_pixels.
std::optional<Bitmap> decode_png(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

// True when the bytes begin with the PNG signature: how an image is told
// apart from what its transport claims it is.
bool looks_like_png(std::vector<std::uint8_t> const& bytes);

}
