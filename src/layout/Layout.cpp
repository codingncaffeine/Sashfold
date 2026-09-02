#include "layout/Layout.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "dom/Dom.h"
#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace sashfold::layout {

namespace {

using css::ComputedStyle;
using css::Display;
using css::LengthPercent;
using css::WhiteSpace;

float resolve(LengthPercent const& length, float percent_base)
{
    switch (length.kind) {
    case LengthPercent::Kind::Auto: return 0;
    case LengthPercent::Kind::Px: return length.value;
    case LengthPercent::Kind::Percent: return percent_base * length.value / 100.0f;
    }
    return 0;
}

bool is_block_level(ComputedStyle const& style)
{
    return style.display == Display::Block || style.display == Display::ListItem
        || style.display == Display::FlowRoot;
}

bool is_floating(ComputedStyle const& style)
{
    return style.floating != css::Float::None;
}

// Whether a box's contents form their own block formatting context: its
// floats stay inside it, and its own box keeps clear of floats outside.
bool establishes_bfc(ComputedStyle const& style)
{
    return is_floating(style) || style.display == Display::FlowRoot
        || style.overflow != css::Overflow::Visible;
}

// The clear a <br> carries: its CSS, else its clear attribute (all = both).
css::Clear break_clear(dom::Element const& element, ComputedStyle const& style)
{
    if (style.clear != css::Clear::None)
        return style.clear;
    dom::Attr const* attribute = element.find_attribute("clear");
    if (!attribute)
        return css::Clear::None;
    std::string_view const value = attribute->value;
    if (ascii_ci_equals(value, "left"))
        return css::Clear::Left;
    if (ascii_ci_equals(value, "right"))
        return css::Clear::Right;
    if (ascii_ci_equals(value, "all") || ascii_ci_equals(value, "both"))
        return css::Clear::Both;
    return css::Clear::None;
}

// One inline item: a word, a space, a hard break, or an image, carrying its style.
struct InlineItem {
    enum class Kind {
        Word,
        Space,
        HardBreak,
        Image,
        Float, // an out-of-flow box met here; style and element are its own
    };
    InlineItem(Kind the_kind, std::u32string the_text, ComputedStyle const* the_style,
        dom::Element const* the_element)
        : kind(the_kind)
        , text(std::move(the_text))
        , style(the_style)
        , element(the_element)
    {
    }
    Kind kind = Kind::Word;
    std::u32string text;
    ComputedStyle const* style = nullptr;
    dom::Element const* element = nullptr; // nearest element, for hit-testing
    std::shared_ptr<Bitmap const> image; // Kind::Image: the picture, or null while it is missing
    float image_density = 1; // Kind::Image: picture pixels per CSS px
    css::Clear clear = css::Clear::None; // Kind::HardBreak: the floats the next line starts below
};

// A replaced element's used size, in px.
struct ReplacedSize {
    float width = 0;
    float height = 0;
};

// width="123" / width="50%": the presentational sizes an <img> carries.
std::optional<LengthPercent> attribute_length(dom::Element const& element, char const* name)
{
    dom::Attr const* attribute = element.find_attribute(name);
    if (!attribute)
        return std::nullopt;
    std::string_view text = attribute->value;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n'))
        text.remove_prefix(1);
    float value = 0;
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        value = value * 10 + static_cast<float>(text[digits] - '0');
        ++digits;
    }
    if (digits == 0)
        return std::nullopt;
    if (digits < text.size() && text[digits] == '%')
        return LengthPercent::percent(value);
    return LengthPercent::px(value);
}

// The used size of an image box from its CSS width and height, else its
// width and height attributes, else the picture's own size; one given
// dimension scales the other by the picture's ratio. A picture wider than
// its container shrinks to fit, ratio kept. nullopt when nothing sizes it.
std::optional<ReplacedSize> replaced_size(dom::Element const& element, ComputedStyle const& style,
    Bitmap const* image, float density, float containing_width)
{
    LengthPercent width = style.width;
    if (width.is_auto()) {
        if (std::optional<LengthPercent> const attribute = attribute_length(element, "width"))
            width = *attribute;
    }
    LengthPercent height = style.height;
    if (height.is_auto()) {
        if (std::optional<LengthPercent> const attribute = attribute_length(element, "height"))
            height = *attribute;
    }
    // The picture's pixels over the density its source was chosen at.
    float const px_per_pixel = density > 0 ? 1.0f / density : 1.0f;
    float const intrinsic_width = image ? static_cast<float>(image->width()) * px_per_pixel : 0;
    float const intrinsic_height = image ? static_cast<float>(image->height()) * px_per_pixel : 0;
    std::optional<float> used_width;
    std::optional<float> used_height;
    if (!width.is_auto())
        used_width = resolve(width, containing_width);
    if (!height.is_auto() && height.kind == LengthPercent::Kind::Px)
        used_height = height.value; // a percentage height has no definite base here
    if (!used_width && !used_height) {
        if (!image)
            return std::nullopt;
        used_width = intrinsic_width;
        used_height = intrinsic_height;
    } else if (!used_width) {
        used_width = image && intrinsic_height > 0 ? *used_height * intrinsic_width / intrinsic_height
                                                   : *used_height;
    } else if (!used_height) {
        used_height = image && intrinsic_width > 0 ? *used_width * intrinsic_height / intrinsic_width
                                                   : *used_width;
    }
    if (containing_width > 0 && *used_width > containing_width) {
        float const scale = containing_width / *used_width;
        used_width = containing_width;
        used_height = *used_height * scale;
    }
    return ReplacedSize { std::max(0.0f, *used_width), std::max(0.0f, *used_height) };
}

// A float's margin box in page coordinates.
struct FloatBox {
    float left = 0;
    float right = 0;
    float top = 0;
    float bottom = 0;
    bool is_left = true;
};

// The floats of one block formatting context, in placement order: line
// boxes and boxes with their own context are measured against them.
struct FloatContext {
    std::vector<FloatBox> floats;

    struct Band {
        float left;
        float right;
    };

    // The room at `y` between the floats there, within [left, right].
    Band band_at(float y, float left, float right) const
    {
        Band band { left, right };
        for (FloatBox const& box : floats) {
            if (box.top > y || box.bottom <= y)
                continue;
            if (box.is_left)
                band.left = std::max(band.left, box.right);
            else
                band.right = std::min(band.right, box.left);
        }
        return band;
    }

    // The nearest float bottom below `y`: where the band may next widen.
    std::optional<float> next_bottom(float y) const
    {
        std::optional<float> next;
        for (FloatBox const& box : floats) {
            if (box.bottom > y && (!next || box.bottom < *next))
                next = box.bottom;
        }
        return next;
    }

    // `y`, or the bottom of the lowest float a clear names, whichever is lower.
    float cleared_y(css::Clear clear, float y) const
    {
        for (FloatBox const& box : floats) {
            bool const named = clear == css::Clear::Both
                || (clear == css::Clear::Left && box.is_left)
                || (clear == css::Clear::Right && !box.is_left);
            if (named)
                y = std::max(y, box.bottom);
        }
        return y;
    }

    std::optional<float> lowest_bottom() const
    {
        std::optional<float> lowest;
        for (FloatBox const& box : floats) {
            if (!lowest || box.bottom > *lowest)
                lowest = box.bottom;
        }
        return lowest;
    }
};

struct Layouter {
    css::StyleMap const& styles;
    // The faces each style resolved to, looked up once per style.
    mutable std::unordered_map<ComputedStyle const*, text::FontStack const*> fonts;
    ImageMap const* images = nullptr;

    PageImage image_for(dom::Element const& element) const
    {
        if (!images)
            return {};
        auto const it = images->find(&element);
        return it == images->end() ? PageImage {} : it->second;
    }

    // An <img> becomes an image item when a picture or a size is known;
    // otherwise its alt text stands in.
    void append_image(dom::Element const& element, ComputedStyle const* style,
        std::vector<InlineItem>& items) const
    {
        PageImage image = image_for(element);
        bool const sized = !style->width.is_auto() || !style->height.is_auto()
            || attribute_length(element, "width") || attribute_length(element, "height");
        if (image.bitmap || sized) {
            InlineItem item(InlineItem::Kind::Image, {}, style, &element);
            item.image = std::move(image.bitmap);
            item.image_density = image.density;
            items.push_back(std::move(item));
            return;
        }
        if (dom::Attr const* alt = element.find_attribute("alt"); alt && !alt->value.empty())
            append_text(decode_utf8("[" + alt->value + "]"), style, items, &element);
    }

    text::FontStack const& fonts_for(ComputedStyle const& style) const
    {
        if (auto const it = fonts.find(&style); it != fonts.end())
            return *it->second;
        text::FontRequest request;
        if (style.font_family)
            request.families = *style.font_family;
        request.weight = style.font_weight;
        request.italic = style.font_style == css::FontStyle::Italic;
        text::FontStack const& stack = text::FontManager::instance().resolve(request);
        fonts.emplace(&style, &stack);
        return stack;
    }

    float measure(ComputedStyle const& style, std::u32string_view text) const
    {
        return fonts_for(style).measure(text, style.font_size);
    }

    ComputedStyle const* style_of(dom::Element const& element) const
    {
        auto const it = styles.find(&element);
        return it == styles.end() ? nullptr : &it->second;
    }

    // --- Inline collection ----------------------------------------------------

    // Collapses whitespace per the white-space mode and appends items.
    void append_text(std::u32string_view text, ComputedStyle const* style,
        std::vector<InlineItem>& items, dom::Element const* element) const
    {
        WhiteSpace const mode = style->white_space;
        bool const preserve_spaces = mode == WhiteSpace::Pre || mode == WhiteSpace::PreWrap;
        bool const preserve_newlines = preserve_spaces || mode == WhiteSpace::PreLine;

        std::u32string word;
        auto const flush_word = [&] {
            if (!word.empty()) {
                items.push_back(InlineItem { InlineItem::Kind::Word, std::move(word), style, element });
                word = {};
            }
        };
        for (char32_t const c : text) {
            if (is_default_ignorable(c))
                continue; // invisible by definition: no glyph, no advance, no break
            bool const is_newline = c == U'\n';
            bool const is_space = c == U' ' || c == U'\t' || c == U'\f' || c == U'\r';
            if (is_newline && preserve_newlines) {
                flush_word();
                items.push_back(InlineItem { InlineItem::Kind::HardBreak, {}, style, element });
                continue;
            }
            if (preserve_spaces) {
                // Spaces are content; tabs advance to the next 8-column stop
                // once monospace layout knows the position — approximated as
                // 8 spaces here.
                if (c == U'\t') {
                    word.append(8, U' ');
                    continue;
                }
                word.push_back(is_space || is_newline ? U' ' : c);
                continue;
            }
            if (is_space || is_newline) {
                flush_word();
                if (items.empty() || items.back().kind != InlineItem::Kind::Space)
                    items.push_back(InlineItem { InlineItem::Kind::Space, U" ", style, element });
                continue;
            }
            word.push_back(c);
        }
        flush_word();
    }

    void collect_inline(dom::Node const& node, ComputedStyle const* inherited,
        std::vector<InlineItem>& items) const
    {
        for (dom::Node const* child : node.children()) {
            if (child->is_text()) {
                std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                if (!text.empty())
                    append_text(text, inherited, items,
                        node.is_element() ? static_cast<dom::Element const*>(&node) : nullptr);
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* style = style_of(element);
            if (!style || style->display == Display::None)
                continue;
            if (is_floating(*style)) {
                items.push_back(InlineItem { InlineItem::Kind::Float, {}, style, &element });
                continue;
            }
            if (element.is_html("br")) {
                InlineItem item(InlineItem::Kind::HardBreak, {}, inherited, &element);
                item.clear = break_clear(element, *style);
                items.push_back(std::move(item));
                continue;
            }
            if (element.is_html("img")) {
                append_image(element, style, items);
                continue;
            }
            collect_inline(element, style, items);
        }
    }

    // --- Inline layout: line building ----------------------------------------

    float ascent_in_line(ComputedStyle const& style) const
    {
        // The half-leading model: the face's ascent, plus half of what the
        // line-height adds beyond the font size.
        float const leading = style.line_height_px() - style.font_size;
        return fonts_for(style).primary().metrics(style.font_size).ascent + leading / 2.0f;
    }

    // Lays the items into lines beside the context's floats; returns the
    // total height used. Floats met among the items are placed as they
    // come, and their boxes join `out`.
    float layout_lines(std::vector<InlineItem> const& items, ComputedStyle const& block_style,
        float content_x, float content_y, float content_width, Fragment& out,
        FloatContext& floats, int list_depth) const
    {
        struct Placed {
            Placed(std::u32string the_text, ComputedStyle const* the_style, bool space, float the_width,
                dom::Element const* the_element)
                : text(std::move(the_text))
                , style(the_style)
                , is_space(space)
                , width(the_width)
                , element(the_element)
            {
            }
            std::u32string text;
            ComputedStyle const* style;
            bool is_space;
            float width;
            dom::Element const* element;
            std::shared_ptr<Bitmap const> image; // an image item's picture (may be null)
            float image_height = 0;
            bool is_image = false;
        };
        float y = content_y;
        std::vector<Placed> line;
        float line_width = 0;
        // The room the current line has between floats, found where it starts.
        float line_left = content_x;
        float line_avail = content_width;
        auto const start_line = [&] {
            FloatContext::Band const band = floats.band_at(y, content_x, content_x + content_width);
            line_left = band.left;
            line_avail = std::max(0.0f, band.right - band.left);
        };
        start_line();

        auto const flush_line = [&] {
            while (!line.empty() && line.back().is_space) {
                line_width -= line.back().width;
                line.pop_back();
            }
            float line_height = block_style.line_height_px();
            float max_ascent = ascent_in_line(block_style);
            for (Placed const& placed : line) {
                line_height = std::max(line_height, placed.style->line_height_px());
                max_ascent = std::max(max_ascent, ascent_in_line(*placed.style));
            }
            // Images sit on the baseline: a tall one lifts it, keeping the
            // text's descent below.
            float const descent = line_height - max_ascent;
            for (Placed const& placed : line) {
                if (placed.is_image)
                    max_ascent = std::max(max_ascent, placed.image_height);
            }
            line_height = std::max(line_height, max_ascent + descent);
            float x = line_left;
            if (block_style.text_align == css::TextAlign::Center)
                x += (line_avail - line_width) / 2.0f;
            else if (block_style.text_align == css::TextAlign::Right)
                x += line_avail - line_width;
            float const baseline = y + max_ascent;
            for (Placed& placed : line) {
                float const width = placed.width;
                if (placed.is_image) {
                    Fragment box;
                    box.element = placed.element;
                    box.style = placed.style;
                    box.x = x;
                    box.y = baseline - placed.image_height;
                    box.width = width;
                    box.height = placed.image_height;
                    box.image = Fragment::ImageBox { placed.image, box.x, box.y, box.width, box.height };
                    out.children.push_back(std::move(box));
                } else if (!placed.text.empty()) {
                    out.runs.push_back(TextRun { x, baseline, std::move(placed.text), placed.style,
                        placed.element, &fonts_for(*placed.style), width });
                }
                x += width;
            }
            y += line_height;
            line.clear();
            line_width = 0;
            start_line();
        };

        // A line beside a float is short: content that would fit the full
        // width but not this line starts the line below the float instead.
        auto const widen_for = [&](float needed) {
            while (line.empty() && needed > line_avail && line_avail < content_width) {
                std::optional<float> const next = floats.next_bottom(y);
                if (!next)
                    break;
                y = *next;
                start_line();
            }
        };

        bool const allow_wrap = block_style.white_space != WhiteSpace::NoWrap
            && block_style.white_space != WhiteSpace::Pre;

        for (InlineItem const& item : items) {
            if (item.kind == InlineItem::Kind::HardBreak) {
                flush_line();
                if (item.clear != css::Clear::None) {
                    y = floats.cleared_y(item.clear, y);
                    start_line();
                }
                continue;
            }
            if (item.kind == InlineItem::Kind::Float) {
                // At the top of the current line when it fits beside what the
                // line holds, else below the line.
                float const outer = float_width(*item.element, *item.style, content_width).outer();
                if (!line.empty() && line_width + outer > line_avail)
                    flush_line();
                place_float(*item.element, *item.style, content_x, content_x + content_width, y,
                    list_depth, floats, out);
                start_line();
                continue;
            }
            if (item.kind == InlineItem::Kind::Space) {
                if (line.empty())
                    continue; // leading space on a line collapses away
                float const width = measure(*item.style, item.text);
                line.push_back(Placed { item.text, item.style, true, width, item.element });
                line_width += width;
                continue;
            }
            if (item.kind == InlineItem::Kind::Image) {
                std::optional<ReplacedSize> const size = replaced_size(*item.element, *item.style,
                    item.image.get(), item.image_density, content_width);
                if (!size)
                    continue;
                if (allow_wrap && !line.empty() && line_width + size->width > line_avail)
                    flush_line();
                if (allow_wrap)
                    widen_for(size->width);
                Placed placed({}, item.style, false, size->width, item.element);
                placed.image = item.image;
                placed.image_height = size->height;
                placed.is_image = true;
                line.push_back(std::move(placed));
                line_width += size->width;
                continue;
            }
            std::u32string word = item.text;
            float width = measure(*item.style, word);
            if (allow_wrap && !line.empty() && line_width + width > line_avail)
                flush_line();
            if (allow_wrap)
                widen_for(width);
            if (allow_wrap && line_avail > 0) {
                // Emergency break: slice a word that cannot fit a whole line,
                // at the last glyph that still fits (one at least).
                while (line.empty() && width > line_avail) {
                    std::size_t fit = 0;
                    float fit_width = 0;
                    if (fonts_for(*item.style).faces().size() == 1) {
                        // Fixed pitch: the count is a division.
                        float const advance = measure(*item.style, U" ");
                        fit = std::max<std::size_t>(1, static_cast<std::size_t>(line_avail / advance));
                        fit = std::min(fit, word.size());
                        fit_width = static_cast<float>(fit) * advance;
                    } else {
                        for (std::size_t i = 0; i < word.size(); ++i) {
                            float const glyph_width
                                = measure(*item.style, std::u32string_view(word).substr(i, 1));
                            if (fit > 0 && fit_width + glyph_width > line_avail)
                                break;
                            fit_width += glyph_width;
                            ++fit;
                        }
                    }
                    if (fit >= word.size())
                        break;
                    line.push_back(Placed { word.substr(0, fit), item.style, false, fit_width, item.element });
                    line_width += fit_width;
                    flush_line();
                    word = word.substr(fit);
                    width = measure(*item.style, word);
                }
            }
            line.push_back(Placed { std::move(word), item.style, false, width, item.element });
            line_width += width;
        }
        if (!line.empty())
            flush_line();
        return y - content_y;
    }

    // --- Block layout ---------------------------------------------------------

    // Lays out `element` with its border box starting at (x, y) given the
    // containing block's content width. Returns the fragment; the caller
    // advances by margins itself (margin collapsing lives there).
    // `floats` is the formatting context the box sits in; a box that forms
    // its own gives its children a fresh one. A float passes `shrink_width`
    // (its shrink-to-fit content width, when its width is auto) and
    // `as_float` (its auto margins are zero, never centering).
    Fragment layout_block(dom::Element const& element, ComputedStyle const& style, float x,
        float y, float containing_width, int list_depth, FloatContext& floats,
        std::optional<float> shrink_width = std::nullopt, bool as_float = false) const
    {
        Fragment fragment;
        fragment.element = &element;
        fragment.style = &style;

        float const margin_left = resolve(style.margin_left, containing_width);
        float const margin_right = resolve(style.margin_right, containing_width);
        float const padding_left = resolve(style.padding_left, containing_width);
        float const padding_right = resolve(style.padding_right, containing_width);
        float const padding_top = resolve(style.padding_top, containing_width);
        float const padding_bottom = resolve(style.padding_bottom, containing_width);
        float const border_left = style.border_left.width;
        float const border_right = style.border_right.width;
        float const border_top = style.border_top.width;
        float const border_bottom = style.border_bottom.width;

        float border_box_width;
        float extra_left = margin_left;
        if (style.width.is_auto()) {
            // A float's auto width shrinks to fit; in flow it fills the line.
            border_box_width = shrink_width
                ? *shrink_width + padding_left + padding_right + border_left + border_right
                : containing_width - margin_left - margin_right;
        } else {
            border_box_width = resolve(style.width, containing_width) + padding_left
                + padding_right + border_left + border_right;
            // Both margins auto with a definite width: center (a float's auto
            // margins are zero instead).
            if (!as_float && style.margin_left.is_auto() && style.margin_right.is_auto())
                extra_left = (containing_width - border_box_width) / 2.0f;
        }
        if (border_box_width < 0)
            border_box_width = 0;

        fragment.x = x + extra_left;
        fragment.y = y;
        fragment.width = border_box_width;

        float const content_x = fragment.x + border_left + padding_left;
        float const content_width = std::max(0.0f,
            border_box_width - border_left - border_right - padding_left - padding_right);
        float const content_y = fragment.y + border_top + padding_top;

        if (element.is_html("img")) {
            // A block-level picture: its own size, shrunk to fit, no children.
            PageImage image = image_for(element);
            std::optional<ReplacedSize> const size
                = replaced_size(element, style, image.bitmap.get(), image.density, content_width);
            if (size) {
                if (style.width.is_auto())
                    fragment.width = size->width + border_left + border_right + padding_left + padding_right;
                fragment.height = size->height + border_top + border_bottom + padding_top + padding_bottom;
                fragment.image = Fragment::ImageBox { std::move(image.bitmap), content_x, content_y,
                    size->width, size->height };
                return fragment;
            }
        }

        // A box that forms its own block formatting context keeps its floats
        // to itself, and its height reaches around them.
        FloatContext own_floats;
        bool const own_context = establishes_bfc(style);
        float const content_height = layout_children(element, style, content_x, content_y,
            content_width, fragment, list_depth, own_context ? own_floats : floats);

        float used_height = content_height;
        if (own_context) {
            if (std::optional<float> const bottom = own_floats.lowest_bottom())
                used_height = std::max(used_height, *bottom - content_y);
        }
        if (!style.height.is_auto() && style.height.kind == LengthPercent::Kind::Px)
            used_height = style.height.value;
        fragment.height = used_height + border_top + border_bottom + padding_top + padding_bottom;
        return fragment;
    }

    // Lays out the children of a block container; returns the content height.
    float layout_children(dom::Element const& element, ComputedStyle const& style,
        float content_x, float content_y, float content_width, Fragment& fragment,
        int list_depth, FloatContext& floats) const
    {
        // Does this element establish a block or an inline formatting context?
        bool has_block_child = false;
        for (dom::Node const* child : element.children()) {
            if (!child->is_element())
                continue;
            ComputedStyle const* child_style = style_of(static_cast<dom::Element const&>(*child));
            if (child_style && !is_floating(*child_style) && is_block_level(*child_style))
                has_block_child = true;
        }

        if (!has_block_child) {
            std::vector<InlineItem> items;
            collect_inline(element, &style, items);
            if (items.empty())
                return 0;
            return layout_lines(items, style, content_x, content_y, content_width, fragment,
                floats, list_depth);
        }

        // Block context: inline runs between blocks wrap in anonymous boxes.
        float cursor = content_y;
        float previous_bottom_margin = 0;
        bool first_in_flow = true;
        std::vector<InlineItem> pending_inline;
        int list_index = 0;

        auto const flush_inline = [&] {
            // Whitespace-only runs between blocks vanish.
            bool significant = false;
            for (InlineItem const& item : pending_inline) {
                if (item.kind == InlineItem::Kind::Word || item.kind == InlineItem::Kind::Image)
                    significant = true;
            }
            if (!significant) {
                pending_inline.clear();
                return;
            }
            Fragment anonymous;
            anonymous.x = content_x;
            anonymous.y = cursor + previous_bottom_margin;
            anonymous.width = content_width;
            float const height = layout_lines(pending_inline, style, content_x,
                cursor + previous_bottom_margin, content_width, anonymous, floats, list_depth);
            anonymous.height = height;
            cursor += previous_bottom_margin + height;
            previous_bottom_margin = 0;
            first_in_flow = false;
            fragment.children.push_back(std::move(anonymous));
            pending_inline.clear();
        };

        for (dom::Node const* child : element.children()) {
            if (child->is_text()) {
                std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                if (!text.empty())
                    append_text(text, &style, pending_inline, &element);
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None)
                continue;
            if (is_floating(*child_style)) {
                // A float among inline content rides with that content; one
                // between blocks is placed here, outside the flow (its
                // siblings' margins still collapse across it).
                bool content = false;
                for (InlineItem const& item : pending_inline) {
                    if (item.kind == InlineItem::Kind::Word || item.kind == InlineItem::Kind::Image)
                        content = true;
                }
                if (content)
                    pending_inline.push_back(
                        InlineItem { InlineItem::Kind::Float, {}, child_style, &child_element });
                else
                    place_float(child_element, *child_style, content_x, content_x + content_width,
                        cursor + previous_bottom_margin, list_depth, floats, fragment);
                continue;
            }
            if (!is_block_level(*child_style)) {
                collect_inline_element(child_element, *child_style, pending_inline);
                continue;
            }

            flush_inline();

            float const margin_top = resolve(child_style->margin_top, content_width);
            float const margin_bottom = resolve(child_style->margin_bottom, content_width);
            // Sibling margin collapsing: adjacent vertical margins overlap.
            float const gap = first_in_flow ? margin_top
                                            : std::max(previous_bottom_margin, margin_top);
            int const child_list_depth = child_element.is_html("ul") || child_element.is_html("ol")
                ? list_depth + 1
                : list_depth;
            float child_y = cursor + gap;
            float child_x = content_x;
            float child_width = content_width;
            // Clearance: the box starts below the floats its clear names.
            if (child_style->clear != css::Clear::None)
                child_y = floats.cleared_y(child_style->clear, child_y);
            // A box with its own formatting context keeps clear of the floats
            // beside it: narrower next to them, or below them when it cannot fit.
            if (establishes_bfc(*child_style)) {
                FloatContext::Band band = floats.band_at(child_y, content_x, content_x + content_width);
                if (!child_style->width.is_auto()) {
                    float const needed = resolve(child_style->width, content_width)
                        + resolve(child_style->padding_left, content_width)
                        + resolve(child_style->padding_right, content_width)
                        + child_style->border_left.width + child_style->border_right.width
                        + resolve(child_style->margin_left, content_width)
                        + resolve(child_style->margin_right, content_width);
                    while (needed > band.right - band.left) {
                        std::optional<float> const next = floats.next_bottom(child_y);
                        if (!next)
                            break;
                        child_y = *next;
                        band = floats.band_at(child_y, content_x, content_x + content_width);
                    }
                }
                child_x = band.left;
                child_width = std::max(0.0f, band.right - band.left);
            }
            Fragment child_fragment = layout_block(child_element, *child_style, child_x, child_y,
                child_width, child_list_depth, floats);

            if (child_style->display == Display::ListItem) {
                ++list_index;
                add_list_marker(child_fragment, *child_style, element, list_index);
            }

            cursor = child_fragment.y + child_fragment.height;
            previous_bottom_margin = margin_bottom;
            first_in_flow = false;
            fragment.children.push_back(std::move(child_fragment));
        }
        flush_inline();
        return cursor - content_y + previous_bottom_margin;
    }

    // A single inline ELEMENT child of a block container (helper so its own
    // style applies, unlike collect_inline which starts at the children).
    void collect_inline_element(dom::Element const& element, ComputedStyle const& style,
        std::vector<InlineItem>& items) const
    {
        if (is_floating(style)) {
            items.push_back(InlineItem { InlineItem::Kind::Float, {}, &style, &element });
            return;
        }
        if (element.is_html("br")) {
            InlineItem item(InlineItem::Kind::HardBreak, {}, &style, &element);
            item.clear = break_clear(element, style);
            items.push_back(std::move(item));
            return;
        }
        if (element.is_html("img")) {
            append_image(element, &style, items);
            return;
        }
        collect_inline(element, &style, items);
    }

    void add_list_marker(Fragment& item_fragment, ComputedStyle const& style,
        dom::Element const& list_parent, int index) const
    {
        (void)list_parent;
        std::u32string marker;
        switch (style.list_style_type) {
        case css::ListStyleType::None:
            return;
        case css::ListStyleType::Disc:
            marker = U"•";
            break;
        case css::ListStyleType::Circle:
            marker = U"◦";
            break; // falls back to the replacement box until the glyph lands
        case css::ListStyleType::Square:
            marker = U"▪";
            break;
        case css::ListStyleType::Decimal: {
            std::string const number = std::to_string(index) + ".";
            marker = decode_utf8(number);
            break;
        }
        }
        if (marker.empty())
            return;
        float const width = measure(style, marker);
        float const gap = style.font_size * 0.4f;
        float const baseline = item_fragment.y + style.border_top.width
            + resolve(style.padding_top, 0) + ascent_in_line(style);
        item_fragment.runs.insert(item_fragment.runs.begin(),
            TextRun { item_fragment.x - width - gap, baseline, std::move(marker), &style,
                item_fragment.element, &fonts_for(style), width });
    }

    // --- Floats -----------------------------------------------------------------

    struct Intrinsic {
        float min = 0; // the widest thing that cannot break: a word, a picture
        float max = 0; // everything on one line
    };

    // The min-content and max-content widths of a run of inline items.
    Intrinsic inline_intrinsic(std::vector<InlineItem> const& items) const
    {
        Intrinsic result;
        float line = 0;
        float pending_space = 0;
        auto const add = [&](float width) {
            if (line > 0)
                line += pending_space;
            pending_space = 0;
            line += width;
        };
        for (InlineItem const& item : items) {
            switch (item.kind) {
            case InlineItem::Kind::Word: {
                float const width = measure(*item.style, item.text);
                result.min = std::max(result.min, width);
                add(width);
                break;
            }
            case InlineItem::Kind::Space:
                pending_space += measure(*item.style, item.text);
                break;
            case InlineItem::Kind::HardBreak:
                result.max = std::max(result.max, line);
                line = 0;
                pending_space = 0;
                break;
            case InlineItem::Kind::Image: {
                std::optional<ReplacedSize> const size = replaced_size(*item.element, *item.style,
                    item.image.get(), item.image_density, 0);
                float const width = size ? size->width : 0;
                result.min = std::max(result.min, width);
                add(width);
                break;
            }
            case InlineItem::Kind::Float: {
                Intrinsic const box = block_intrinsic(*item.element, *item.style);
                result.min = std::max(result.min, box.min);
                add(box.max);
                break;
            }
            }
        }
        result.max = std::max(result.max, line);
        return result;
    }

    // The intrinsic widths of an element's contents, inside its edges.
    Intrinsic intrinsic_widths(dom::Element const& element, ComputedStyle const& style) const
    {
        if (element.is_html("img")) {
            PageImage const image = image_for(element);
            std::optional<ReplacedSize> const size
                = replaced_size(element, style, image.bitmap.get(), image.density, 0);
            float const width = size ? size->width : 0;
            return { width, width };
        }
        bool has_block_child = false;
        for (dom::Node const* child : element.children()) {
            if (!child->is_element())
                continue;
            ComputedStyle const* child_style = style_of(static_cast<dom::Element const&>(*child));
            if (child_style && !is_floating(*child_style) && is_block_level(*child_style))
                has_block_child = true;
        }
        std::vector<InlineItem> pending;
        if (!has_block_child) {
            collect_inline(element, &style, pending);
            return inline_intrinsic(pending);
        }
        Intrinsic result;
        auto const flush = [&] {
            Intrinsic const run = inline_intrinsic(pending);
            result.min = std::max(result.min, run.min);
            result.max = std::max(result.max, run.max);
            pending.clear();
        };
        for (dom::Node const* child : element.children()) {
            if (child->is_text()) {
                std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                if (!text.empty())
                    append_text(text, &style, pending, &element);
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None)
                continue;
            if (is_floating(*child_style) || !is_block_level(*child_style)) {
                collect_inline_element(child_element, *child_style, pending);
                continue;
            }
            flush();
            Intrinsic const box = block_intrinsic(child_element, *child_style);
            result.min = std::max(result.min, box.min);
            result.max = std::max(result.max, box.max);
        }
        flush();
        return result;
    }

    // A block's intrinsic widths seen from outside: its width when written
    // in px, else its contents', plus its horizontal edges (percentages
    // count as zero here).
    Intrinsic block_intrinsic(dom::Element const& element, ComputedStyle const& style) const
    {
        float const edges = resolve(style.margin_left, 0) + resolve(style.margin_right, 0)
            + resolve(style.padding_left, 0) + resolve(style.padding_right, 0)
            + style.border_left.width + style.border_right.width;
        if (!style.width.is_auto() && style.width.kind == LengthPercent::Kind::Px)
            return { style.width.value + edges, style.width.value + edges };
        Intrinsic const inner = intrinsic_widths(element, style);
        return { inner.min + edges, inner.max + edges };
    }

    // A float's horizontal measure in a containing block this wide: its
    // width as written, else shrink-to-fit — the larger of its narrowest
    // content and the room available, but no wider than its content wants.
    struct FloatWidth {
        float margin_left = 0;
        float margin_right = 0;
        float border_box = 0;
        std::optional<float> shrink; // the content width, when auto
        float outer() const { return margin_left + border_box + margin_right; }
    };

    FloatWidth float_width(dom::Element const& element, ComputedStyle const& style,
        float containing_width) const
    {
        FloatWidth result;
        result.margin_left = resolve(style.margin_left, containing_width);
        result.margin_right = resolve(style.margin_right, containing_width);
        float const edges = resolve(style.padding_left, containing_width)
            + resolve(style.padding_right, containing_width) + style.border_left.width
            + style.border_right.width;
        if (style.width.is_auto()) {
            Intrinsic const intrinsic = intrinsic_widths(element, style);
            float const available = std::max(0.0f,
                containing_width - result.margin_left - result.margin_right - edges);
            result.shrink = std::min(std::max(intrinsic.min, available), intrinsic.max);
            result.border_box = *result.shrink + edges;
        } else {
            result.border_box = resolve(style.width, containing_width) + edges;
        }
        return result;
    }

    // Lays out a floated box and places it at or below `y`, against the
    // containing block's content edges [x0, x1] and beside the floats already
    // there; its box joins `parent`.
    void place_float(dom::Element const& element, ComputedStyle const& style, float x0, float x1,
        float y, int list_depth, FloatContext& floats, Fragment& parent) const
    {
        float const containing_width = x1 - x0;
        FloatWidth const width = float_width(element, style, containing_width);
        float const margin_top = resolve(style.margin_top, containing_width);
        float const margin_bottom = resolve(style.margin_bottom, containing_width);

        // No higher than any earlier float, below what its own clear names,
        // then down until it fits beside the floats at that height.
        for (FloatBox const& box : floats.floats)
            y = std::max(y, box.top);
        y = floats.cleared_y(style.clear, y);
        FloatContext::Band band = floats.band_at(y, x0, x1);
        while (width.outer() > band.right - band.left) {
            std::optional<float> const next = floats.next_bottom(y);
            if (!next)
                break; // nothing to wait for: it overflows here
            y = *next;
            band = floats.band_at(y, x0, x1);
        }
        bool const is_left = style.floating == css::Float::Left;
        float const outer_left = is_left ? band.left : band.right - width.outer();
        Fragment box = layout_block(element, style, outer_left, y + margin_top, containing_width,
            list_depth, floats, width.shrink, true);
        floats.floats.push_back(FloatBox { outer_left, outer_left + width.outer(), y,
            box.y + box.height + margin_bottom, is_left });
        box.floating = true;
        parent.children.push_back(std::move(box));
    }
};

} // namespace

LayoutResult layout_document(dom::Document const& document, css::StyleMap const& styles,
    float viewport_width, ImageMap const* images)
{
    LayoutResult result;
    result.canvas_background = Color::rgb(255, 255, 255);

    dom::Element const* html = nullptr;
    for (dom::Node const* child : document.children()) {
        if (child->is_element() && static_cast<dom::Element const*>(child)->is_html("html"))
            html = static_cast<dom::Element const*>(child);
    }
    if (!html)
        return result;

    Layouter layouter { styles, {}, images };
    ComputedStyle const* html_style = layouter.style_of(*html);
    if (!html_style || html_style->display == Display::None)
        return result;

    // Background propagation: html's background paints the canvas; when html
    // is transparent, body's does (and body then skips its own).
    if (html_style->background_color.a != 0) {
        result.canvas_background = html_style->background_color;
    } else {
        for (dom::Node const* child : html->children()) {
            if (child->is_element() && static_cast<dom::Element const*>(child)->is_html("body")) {
                if (ComputedStyle const* body = layouter.style_of(
                        *static_cast<dom::Element const*>(child));
                    body && body->background_color.a != 0)
                    result.canvas_background = body->background_color;
            }
        }
    }

    // The root's formatting context holds the page's floats, and the page
    // reaches around them.
    FloatContext root_floats;
    result.root = layouter.layout_block(*html, *html_style, 0, 0, viewport_width, 0, root_floats);
    if (std::optional<float> const bottom = root_floats.lowest_bottom())
        result.root.height = std::max(result.root.height, *bottom - result.root.y);
    result.page_height = result.root.y + result.root.height
        + resolve(html_style->margin_bottom, viewport_width);
    return result;
}

}
