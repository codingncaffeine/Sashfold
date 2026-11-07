#pragma once

// A PDF page drawn (ISO 32000-2 §8 and §11): its content streams run
// through the graphics state, paths filled, stroked and clipped, colors in
// every space, pictures, shadings and patterns, form XObjects, transparency
// groups and soft masks, optional content hidden as the document's default
// configuration says, on white paper, at a scale and under the page's own
// rotation. Called from any thread: one Document renders on several at once.

#include "core/Bitmap.h"
#include "paint/Canvas2D.h"
#include "pdf/Document.h"
#include "pdf/Form.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace sashfold::pdf {

// One glyph shown on a page, for a text layer: its box in device pixels
// (bottom left, bottom right, top right, top left: the baseline first),
// the text it stands for, and whether it is a word space.
struct TextGlyph {
    std::array<svg::Point, 4> quad {};
    std::u32string text;
    double font_size = 0; // in device pixels
    bool word_space = false;
};

struct RenderOptions {
    double scale = 1; // device pixels per point: 1 is 72 dpi
    bool annotations = true; // annotations' appearances, as viewers show them
    std::atomic<bool> const* cancel = nullptr; // set to stop between operators
    int extra_rotation = 0; // 0, 90, 180 or 270, clockwise, on top of the page's
    // Every glyph shown is said here; with `draw` false nothing is painted
    // at all, and the page's text is all that comes of it.
    std::function<void(TextGlyph const&)> text_sink;
    bool draw = true;
    // Form fields: the values a viewer has given them (over the file's),
    // and whether the viewer shows text and choice fields itself (as HTML
    // controls over the page), in which case the page leaves them out.
    FormValues const* form_values = nullptr;
    bool fields_shown_by_viewer = false;
};

// The glyphs of a page in the order its content shows them, in device
// pixels at the scale given (the page's rotation applied).
std::vector<TextGlyph> page_glyphs(Document const& doc, std::size_t index, double scale = 1);

// The page's picture size at a scale (rotation included), and the
// transform from its user space to the picture's pixels.
struct PixelSize {
    int width = 0;
    int height = 0;
};
PixelSize page_pixel_size(Page const& page, double scale, int extra_rotation = 0);
canvas::Transform page_to_device(Page const& page, double scale, int extra_rotation = 0);

// Nullopt when the page does not exist, its picture would be too large, or
// the render was cancelled.
std::optional<Bitmap> render_page(Document const& doc, std::size_t index, RenderOptions const& options = {});

inline constexpr std::size_t k_max_render_pixels = 100u * 1024u * 1024u;

}
