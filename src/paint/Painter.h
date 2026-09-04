#pragma once

// Paints a fragment tree into a Bitmap: backgrounds, solid borders, then
// text runs with their decorations. Consumes only fragments, never boxes.

#include "core/Bitmap.h"
#include "layout/Layout.h"

namespace sashfold::paint {

// Renders the laid-out page, translated by the offset — a scrolled viewport
// paints with offset_y = -scroll. The bitmap must already be sized; the
// canvas background comes from the layout result and fills the whole target.
// `backgrounds` holds the decoded background pictures by URL; without it
// the styles' background images that are pictures paint nothing (gradients
// need no fetch and paint regardless).
// `scrolls` is how far each box that scrolls has had its content moved —
// the shell's live state, and the only thing the painter needs it for is
// where to draw each thumb: the content itself has already been moved on
// the fragments (`layout::apply_scroll`), so the painter draws it where it
// finds it. Without the map no scrollbar is drawn at all, which is what a
// single render of a page is: nothing has been scrolled, and nothing can
// be.
void paint_page(Bitmap& target, layout::LayoutResult const& page, float offset_x = 0,
    float offset_y = 0, layout::BackgroundImages const* backgrounds = nullptr,
    layout::ScrollOffsets const* scrolls = nullptr);

// The room a scrollbar takes inside a scrollport, in CSS px — one number
// for both axes, as a desktop engine's default is.
inline constexpr float scrollbar_width = 12;

// Where a box's scrollbars are, in page coordinates: the vertical one down
// the scrollport's right edge, the horizontal one across its bottom. Absent
// when the axis shows no bar — `overflow: auto` with nothing out of sight,
// or `hidden`, which gives a reader no way to reach it. `thumb` is the part
// that moves and that a reader drags; `track` is the whole bar. The shell
// draws these and hit-tests them, and the painter draws them when it is
// given the offsets, so both read the geometry from one place.
struct ScrollbarGeometry {
    Rect track;
    Rect thumb;
};
std::optional<ScrollbarGeometry> vertical_scrollbar(
    layout::Fragment const& box, layout::ScrollOffset offset);
std::optional<ScrollbarGeometry> horizontal_scrollbar(
    layout::Fragment const& box, layout::ScrollOffset offset);

}
