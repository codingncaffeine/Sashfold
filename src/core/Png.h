#pragma once

#include "core/Bitmap.h"

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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

// What a PNG says of its samples, apart from how many of them there are:
// the same for every frame of an animated one.
struct PngSamples {
    int depth = 0;
    int color_type = 0;
    bool interlaced = false;
    std::vector<Color> palette;
    bool has_key = false;
    std::array<std::uint32_t, 3> key { 0, 0, 0 };
};

// One frame of an animated PNG (APNG 1.0): where it lies in the picture,
// how long it is shown, what becomes of its area when the next is due (0
// nothing, 1 cleared to transparent, 2 what was there before it), whether it
// replaces what it covers (0) or is laid over it (1), and where in the file
// its data is - the pieces of one zlib stream, as offsets and lengths.
struct ApngFrame {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::uint32_t delay_ms = 0; // as written
    int dispose = 0;
    int blend = 0;
    std::vector<std::pair<std::size_t, std::size_t>> data;
};

struct Apng {
    int width = 0;
    int height = 0;
    std::uint32_t loops = 0; // how many times it plays; 0 is without end
    PngSamples samples;
    std::vector<ApngFrame> frames;
};

// An animated PNG's frames, found without decoding them: nullopt for a PNG
// that is not animated (no acTL before its data, or no frame that can be
// shown), and for anything decode_png refuses. The picture a still decoder
// shows is the first frame only when the file says so.
std::optional<Apng> scan_apng(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

// One frame's own picture, at its own size; nullopt when its data is
// malformed.
std::optional<Bitmap> decode_apng_frame(std::vector<std::uint8_t> const& bytes, Apng const& animation,
    std::size_t index);

// True when the bytes begin with the PNG signature: how an image is told
// apart from what its transport claims it is.
bool looks_like_png(std::vector<std::uint8_t> const& bytes);

}
