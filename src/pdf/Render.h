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

#include <atomic>
#include <cstddef>
#include <optional>

namespace sashfold::pdf {

struct RenderOptions {
    double scale = 1; // device pixels per point: 1 is 72 dpi
    bool annotations = true; // annotations' appearances, as viewers show them
    std::atomic<bool> const* cancel = nullptr; // set to stop between operators
    int extra_rotation = 0; // 0, 90, 180 or 270, clockwise, on top of the page's
};

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
