#include "paint/Painter.h"

#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

// A form control's look: the page's own background and borders when the
// author gave it any, else the built-in look — a white or gray fill with a
// gray border — and then the check mark, the radio dot, the select's arrow,
// the focus ring and the caret. The text inside is the fragment's runs.
void paint_control(Context& context, Fragment const& fragment)
{
    using layout::ControlKind;
    Fragment::ControlBox const& control = *fragment.control;
    ComputedStyle const& style = *fragment.style;
    float const x = control.x + context.dx;
    float const y = control.y + context.dy;
    Rect const rect = snap(x, y, control.width, control.height);
    if (rect.width <= 0 || rect.height <= 0)
        return;
    auto const solid = [](css::BorderSide const& side) {
        return side.style == BorderStyle::Solid && side.width > 0;
    };
    bool const author_look = style.background_color.a != 0 || solid(style.border_top)
        || solid(style.border_bottom) || solid(style.border_left) || solid(style.border_right);
    Color const border = control.disabled ? Color::rgb(0xc0, 0xc0, 0xc0) : Color::rgb(0x76, 0x76, 0x76);
    Color const white = Color::rgb(0xff, 0xff, 0xff);
    Color const gray = Color::rgb(0xef, 0xef, 0xef);
    Color const ink = control.disabled ? Color::rgb(0x8d, 0x8d, 0x8d) : Color::rgb(0x1a, 0x1a, 0x1a);
    auto const frame = [&](Rect const& r, Color color) {
        context.target.fill_rect(Rect { r.x, r.y, r.width, 1 }, color);
        context.target.fill_rect(Rect { r.x, r.y + r.height - 1, r.width, 1 }, color);
        context.target.fill_rect(Rect { r.x, r.y, 1, r.height }, color);
        context.target.fill_rect(Rect { r.x + r.width - 1, r.y, 1, r.height }, color);
    };
    auto const inset = [](Rect const& r, int by) {
        return Rect { r.x + by, r.y + by, r.width - 2 * by, r.height - 2 * by };
    };
    switch (control.kind) {
    case ControlKind::Checkbox:
        context.target.fill_round_rect(rect, 2, border);
        context.target.fill_round_rect(inset(rect, 1), 2, control.disabled ? gray : white);
        if (control.checked)
            context.target.fill_rect(inset(rect, 3), ink);
        break;
    case ControlKind::Radio: {
        int const radius = rect.width / 2;
        context.target.fill_round_rect(rect, radius, border);
        context.target.fill_round_rect(inset(rect, 1), radius - 1, control.disabled ? gray : white);
        if (control.checked)
            context.target.fill_round_rect(inset(rect, 4), radius - 4, ink);
        break;
    }
    default: {
        bool const button = control.kind == ControlKind::Submit || control.kind == ControlKind::Button
            || control.kind == ControlKind::File;
        if (!author_look) {
            context.target.fill_rect(rect, button || control.disabled ? gray : white);
            frame(rect, border);
        }
        if (control.kind == ControlKind::Select) {
            // A small triangle pointing down, near the right edge.
            int const tip_x = rect.x + rect.width - 12;
            int const top = rect.y + rect.height / 2 - 2;
            for (int row = 0; row < 4; ++row)
                context.target.fill_rect(Rect { tip_x - 3 + row, top + row, 7 - 2 * row, 1 }, ink);
        }
        break;
    }
    }
    if (control.focused) {
        Color const accent = Color::rgb(0x00, 0x60, 0xdf);
        frame(rect, accent);
        frame(inset(rect, 1), accent);
    }
    if (control.caret_x)
        context.target.fill_rect(snap(*control.caret_x + context.dx, y + 4, 1, control.height - 8), ink);
}

void paint_stacking_context(Context& context, Fragment const& root, bool is_canvas_background_owner);

// A box and what flows inside it: its background and borders, its picture
// or control, its in-flow children, the floats over them (a float paints
// above the blocks whose lines flow around it), and its own lines. The
// positioned descendants are not here: they belong to the stacking context
// and paint at their level in it.
void paint_flow(Context& context, Fragment const& fragment, bool is_canvas_background_owner)
{
    if (fragment.style)
        paint_background_and_borders(context, fragment, is_canvas_background_owner);
    if (fragment.control && fragment.style)
        paint_control(context, fragment);
    if (fragment.image && fragment.image->bitmap) {
        Fragment::ImageBox const& box = *fragment.image;
        context.target.draw_scaled(*box.bitmap,
            snap(box.x + context.dx, box.y + context.dy, box.width, box.height));
    }
    for (Fragment const& child : fragment.children) {
        if (!child.floating && !child.positioned)
            paint_flow(context, child, false);
    }
    for (Fragment const& child : fragment.children) {
        if (child.floating && !child.positioned)
            paint_flow(context, child, false);
    }
    for (TextRun const& run : fragment.runs)
        paint_run(context, run);
}

// The positioned boxes a stacking context paints: every positioned
// descendant reached without crossing another stacking context (a
// positioned box with z-index auto is walked through — its positioned
// descendants are the parent context's, per CSS 2.1 Appendix E).
void collect_positioned(Fragment const& fragment, std::vector<Fragment const*>& out)
{
    for (Fragment const& child : fragment.children) {
        if (child.positioned) {
            out.push_back(&child);
            if (child.stacking_context)
                continue;
        }
        collect_positioned(child, out);
    }
}

// CSS 2.1 Appendix E, the reader-web subset: the context's own flow first,
// then its positioned descendants by z-index — negative ones under the
// flow — in tree order within a level; a descendant that is itself a
// stacking context paints as one unit, one with z-index auto as its flow
// alone (its positioned descendants are already in this list).
void paint_stacking_context(Context& context, Fragment const& root, bool is_canvas_background_owner)
{
    std::vector<Fragment const*> positioned;
    collect_positioned(root, positioned);
    std::stable_sort(positioned.begin(), positioned.end(),
        [](Fragment const* a, Fragment const* b) { return a->z_index < b->z_index; });
    auto const paint_one = [&](Fragment const& box) {
        if (box.stacking_context)
            paint_stacking_context(context, box, false);
        else
            paint_flow(context, box, false);
    };
    // The root's own background and borders come before its negative
    // descendants, which come before the rest of its flow.
    if (root.style)
        paint_background_and_borders(context, root, is_canvas_background_owner);
    for (Fragment const* box : positioned) {
        if (box->z_index < 0)
            paint_one(*box);
    }
    {
        // The flow without the background already painted: children, floats, lines.
        if (root.control && root.style)
            paint_control(context, root);
        if (root.image && root.image->bitmap) {
            Fragment::ImageBox const& box = *root.image;
            context.target.draw_scaled(*box.bitmap,
                snap(box.x + context.dx, box.y + context.dy, box.width, box.height));
        }
        for (Fragment const& child : root.children) {
            if (!child.floating && !child.positioned)
                paint_flow(context, child, false);
        }
        for (Fragment const& child : root.children) {
            if (child.floating && !child.positioned)
                paint_flow(context, child, false);
        }
        for (TextRun const& run : root.runs)
            paint_run(context, run);
    }
    for (Fragment const* box : positioned) {
        if (box->z_index >= 0)
            paint_one(*box);
    }
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
    paint_stacking_context(context, page.root, true);
}

}
