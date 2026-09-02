#include "paint/Painter.h"

#include "text/Face.h"
#include "text/FontManager.h"

#include <cmath>

namespace sashfold::paint {

namespace {

using css::BorderStyle;
using css::ComputedStyle;
using layout::Fragment;
using layout::TextRun;

struct Context {
    Bitmap& target;
    float dx;
    float dy;
};

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

void paint_background_and_borders(Context& context, Fragment const& fragment,
    bool skip_background)
{
    ComputedStyle const& style = *fragment.style;
    float const x = fragment.x + context.dx;
    float const y = fragment.y + context.dy;
    if (!skip_background && style.background_color.a != 0)
        context.target.fill_rect(snap(x, y, fragment.width, fragment.height),
            style.background_color);

    if (style.border_top.style == BorderStyle::Solid && style.border_top.width > 0)
        context.target.fill_rect(snap(x, y, fragment.width, style.border_top.width),
            style.border_top.color);
    if (style.border_bottom.style == BorderStyle::Solid && style.border_bottom.width > 0)
        context.target.fill_rect(snap(x, y + fragment.height - style.border_bottom.width,
                                     fragment.width, style.border_bottom.width),
            style.border_bottom.color);
    if (style.border_left.style == BorderStyle::Solid && style.border_left.width > 0)
        context.target.fill_rect(snap(x, y, style.border_left.width, fragment.height),
            style.border_left.color);
    if (style.border_right.style == BorderStyle::Solid && style.border_right.width > 0)
        context.target.fill_rect(snap(x + fragment.width - style.border_right.width, y,
                                     style.border_right.width, fragment.height),
            style.border_right.color);
}

void paint_run(Context& context, TextRun const& run)
{
    ComputedStyle const& style = *run.style;
    float const baseline = run.baseline_y + context.dy;
    // A run entirely above or below the target draws nothing: skip it before
    // rasterizing — a scrolled page has thousands of such runs. One em of
    // margin covers descenders, decorations, and synthesized styles.
    if (baseline + style.font_size < 0
        || baseline - style.font_size > static_cast<float>(context.target.height()))
        return;

    if (!run.fonts)
        return;
    bool const italic = style.font_style == css::FontStyle::Italic;
    float const start_x = run.x + context.dx;
    float x = start_x;
    for (char32_t const c : run.text) {
        text::FontStack::Glyph const glyph = run.fonts->glyph_for(c);
        glyph.face->draw_glyph(context.target, glyph.glyph, x, baseline, style.font_size,
            style.color, style.bold(), italic);
        x += glyph.face->advance(glyph.glyph, style.font_size);
    }

    if (style.text_decoration == css::TextDecorationLine::None || run.text.empty())
        return;
    float const width = x - start_x;
    float const thickness = std::max(1.0f, style.font_size / 14.0f);
    float line_y;
    if (style.text_decoration == css::TextDecorationLine::Underline)
        line_y = baseline + style.font_size * 2.0f / 32.0f + 1.0f;
    else
        line_y = baseline - style.font_size * 8.0f / 32.0f;
    context.target.fill_rect(snap(start_x, line_y, width, thickness), style.color);
}

void paint_fragment(Context& context, Fragment const& fragment, bool is_canvas_background_owner)
{
    if (fragment.style)
        paint_background_and_borders(context, fragment, is_canvas_background_owner);
    if (fragment.image && fragment.image->bitmap) {
        Fragment::ImageBox const& box = *fragment.image;
        context.target.draw_scaled(*box.bitmap,
            snap(box.x + context.dx, box.y + context.dy, box.width, box.height));
    }
    for (Fragment const& child : fragment.children) {
        // The element whose background became the canvas skips its own.
        paint_fragment(context, child, false);
    }
    for (TextRun const& run : fragment.runs)
        paint_run(context, run);
}

} // namespace

void paint_page(Bitmap& target, layout::LayoutResult const& page, float offset_x, float offset_y)
{
    target.fill_rect(Rect { 0, 0, target.width(), target.height() }, page.canvas_background);
    if (!page.root.style)
        return;
    // The html fragment's own background became the canvas, so it skips
    // itself. A promoted opaque body background repaints the same color over
    // its own box, which is invisible; translucent body backgrounds are the
    // one known double-composite, noted for the reftest era.
    Context context { target, offset_x, offset_y };
    paint_fragment(context, page.root, true);
}

}
