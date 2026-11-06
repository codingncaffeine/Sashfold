#pragma once

// The chrome's text, for the surfaces the shell draws outside Browser.cpp
// (the file chooser of its own): measured and drawn in the chrome's face, as
// the tab strip and the menus are.

#include "core/Bitmap.h"

#include <string>
#include <string_view>

namespace sashfold::ui {

float chrome_text_width(std::u32string_view text, float size);
void chrome_draw_text(Bitmap& target, std::u32string_view text, float x, float baseline, float size, Color color, bool bold = false);
// The baseline that centers the face in a rect.
float chrome_centered_baseline(Rect const& rect, float size);
// The text cut with an ellipsis to fit `max_width`.
std::u32string chrome_ellipsize(std::u32string text, float max_width, float size);

}
