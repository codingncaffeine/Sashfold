#pragma once

// The chrome's icons: line drawings on a grid of sixteen units, stroked a
// unit and a half wide with round ends and round corners, drawn at whatever
// size a button asks for in whatever color its theme gives — a browser
// theme's icons color lands on every one of them.
//
// They are drawn by a rasterizer of their own, and on purpose not by the
// SVG one. The chrome's pictures are compared byte for byte on three
// operating systems; an arc, a round cap or a miter drawn through a sine or
// an arc cosine is the same picture only as far as three math libraries
// agree in their last bit. Here an icon is a few segments and ring arcs
// held as whole numbers, a pixel's coverage is how many of its sixty-four
// sub-samples lie within the stroke, and whether one does is decided by
// comparing products of whole numbers: no floating point is involved, so
// every machine draws the same bytes.

#include "core/Bitmap.h"

namespace sashfold::ui {

enum class Icon {
    Back,
    Forward,
    Reload,
    Reader,
    Menu,
    Plus,
    Close,
    Minimize,
    Maximize,
};

// The icon `size` device px square, centered in `box`, in `color`.
void draw_icon(Bitmap& target, Icon icon, Rect const& box, int size, Color color);

// How much of the pixel (x, y) of the icon drawn `size` px square its
// strokes cover, 0 to 255: what draw_icon composites through. For tests.
std::uint8_t icon_coverage(Icon icon, int size, int x, int y);

}
