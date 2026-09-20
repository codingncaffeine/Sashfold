#pragma once

// GIF decoding (87a and 89a): the logical screen, global and local color
// tables, LZW image data, interlaced rows, and the graphic control
// extension's transparent index, delay and disposal. A file is scanned for
// its frames without decoding a pixel, and a frame is drawn when it is
// wanted: a picture that moves is held as its bytes and one canvas, not as
// every frame it has. Every table walk and every size is bounded, so hostile
// input cannot escape.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

// One image of a GIF: where it lies on the logical screen, how long it is
// shown, what becomes of it when the next is due, and where in the file its
// tables and data begin.
struct GifFrame {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::uint32_t delay_ms = 0; // as written: hundredths of a second, in ms
    // 0 and 1 leave the frame where it is; 2 clears its area to the
    // background, which is nothing; 3 puts back what was there before it.
    int disposal = 0;
    int transparent = -1; // the index that paints nothing; -1 for none
    std::size_t descriptor = 0; // the offset of the image descriptor's packed byte
};

struct GifAnimation {
    int width = 0;
    int height = 0;
    // How many times the frames are played through: 0 is without end. A
    // file that does not say plays once.
    std::uint32_t loops = 1;
    std::vector<Color> global_table;
    std::vector<GifFrame> frames;
};

// The file's frames, found without decoding them; nullopt for anything
// malformed before the first frame, for a file with none, or for a screen
// or a frame larger than max_pixels. A file that stops short keeps the
// frames that came whole.
std::optional<GifAnimation> scan_gif(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

// Draws one frame over the canvas — the logical screen — leaving its
// transparent pixels as they were. False when the frame's data is malformed.
bool draw_gif_frame(std::vector<std::uint8_t> const& bytes, GifAnimation const& animation, std::size_t index,
    Bitmap& canvas);

// The first frame composed onto a transparent logical screen; nullopt for
// anything malformed, or larger than max_pixels.
std::optional<Bitmap> decode_gif(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

bool looks_like_gif(std::vector<std::uint8_t> const& bytes);

}
