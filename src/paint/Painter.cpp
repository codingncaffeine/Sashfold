#include "paint/Painter.h"

#include "text/SashfoldMono.h"

#include <cmath>

namespace sashfold::paint {

namespace {

using css::BorderStyle;
using css::ComputedStyle;
using layout::Fragment;
using layout::TextRun;

int round_px(float value)
{
    return static_cast<int>(value >= 0 ? value + 0.5f : value - 0.5f);
}

Rect snap(float x, float y, float width, float height)
{
    int const left = round_px(x);
    int const top = round_px(y);
    return Rect { left, top, round_px(x + width) - left, round_px(y + height) - top };
}

void paint_background_and_borders(Bitmap& target, Fragment const& fragment, bool skip_background)
{
    ComputedStyle const& style = *fragment.style;
    if (!skip_background && style.background_color.a != 0)
        target.fill_rect(snap(fragment.x, fragment.y, fragment.width, fragment.height),
            style.background_color);

    if (style.border_top.style == BorderStyle::Solid && style.border_top.width > 0)
        target.fill_rect(snap(fragment.x, fragment.y, fragment.width, style.border_top.width),
            style.border_top.color);
    if (style.border_bottom.style == BorderStyle::Solid && style.border_bottom.width > 0)
        target.fill_rect(snap(fragment.x, fragment.y + fragment.height - style.border_bottom.width,
                             fragment.width, style.border_bottom.width),
            style.border_bottom.color);
    if (style.border_left.style == BorderStyle::Solid && style.border_left.width > 0)
        target.fill_rect(snap(fragment.x, fragment.y, style.border_left.width, fragment.height),
            style.border_left.color);
    if (style.border_right.style == BorderStyle::Solid && style.border_right.width > 0)
        target.fill_rect(snap(fragment.x + fragment.width - style.border_right.width, fragment.y,
                             style.border_right.width, fragment.height),
            style.border_right.color);
}

void paint_run(Bitmap& target, TextRun const& run)
{
    ComputedStyle const& style = *run.style;
    text::SashfoldMono const& font = text::SashfoldMono::instance();
    float const advance = text::SashfoldMono::advance(style.font_size);
    float x = run.x;
    for (char32_t const c : run.text) {
        font.draw_glyph(target, c, x, run.baseline_y, style.font_size, style.color, style.bold(),
            style.font_style == css::FontStyle::Italic);
        x += advance;
    }

    if (style.text_decoration == css::TextDecorationLine::None || run.text.empty())
        return;
    float const width = x - run.x;
    float const thickness = std::max(1.0f, style.font_size / 14.0f);
    float line_y;
    if (style.text_decoration == css::TextDecorationLine::Underline)
        line_y = run.baseline_y + style.font_size * 2.0f / 32.0f + 1.0f;
    else
        line_y = run.baseline_y - style.font_size * 8.0f / 32.0f;
    target.fill_rect(snap(run.x, line_y, width, thickness), style.color);
}

void paint_fragment(Bitmap& target, Fragment const& fragment, bool is_canvas_background_owner)
{
    if (fragment.style)
        paint_background_and_borders(target, fragment, is_canvas_background_owner);
    for (Fragment const& child : fragment.children) {
        // The element whose background became the canvas skips its own.
        paint_fragment(target, child, false);
    }
    for (TextRun const& run : fragment.runs)
        paint_run(target, run);
}

} // namespace

void paint_page(Bitmap& target, layout::LayoutResult const& page)
{
    target.fill_rect(Rect { 0, 0, target.width(), target.height() }, page.canvas_background);
    if (!page.root.style)
        return;
    // The html fragment's own background became the canvas, so it skips
    // itself. A promoted opaque body background repaints the same color over
    // its own box, which is invisible; translucent body backgrounds are the
    // one known double-composite, noted for the reftest era.
    paint_fragment(target, page.root, true);
}

}
