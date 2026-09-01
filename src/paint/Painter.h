#pragma once

// Paints a fragment tree into a Bitmap: backgrounds, solid borders, then
// text runs with their decorations. Consumes only fragments, never boxes.

#include "core/Bitmap.h"
#include "layout/Layout.h"

namespace sashfold::paint {

// Renders the whole laid-out page. The bitmap must already be sized;
// the canvas background comes from the layout result.
void paint_page(Bitmap& target, layout::LayoutResult const& page);

}
