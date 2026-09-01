#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sashfold {

class Bitmap;

// Encodes an RGBA8 PNG. The deflate stream uses stored (uncompressed) blocks,
// which is valid zlib and keeps the encoder dependency-free; every PNG decoder
// accepts it. Compression can come later without changing this interface.
std::vector<std::uint8_t> encode_png(Bitmap const& bitmap);

bool write_png(std::string const& path, Bitmap const& bitmap);

}
