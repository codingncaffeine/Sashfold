#include "layout/Layout.h"

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
    return style.display == Display::Block || style.display == Display::ListItem;
}

// One inline item: a word, a space, a hard break, or an image, carrying its style.
struct InlineItem {
    enum class Kind {
        Word,
        Space,
        HardBreak,
        Image,
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
    Bitmap const* image, float containing_width)
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
    float const intrinsic_width = image ? static_cast<float>(image->width()) : 0;
    float const intrinsic_height = image ? static_cast<float>(image->height()) : 0;
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

struct Layouter {
    css::StyleMap const& styles;
    // The faces each style resolved to, looked up once per style.
    mutable std::unordered_map<ComputedStyle const*, text::FontStack const*> fonts;
    ImageMap const* images = nullptr;

    std::shared_ptr<Bitmap const> image_for(dom::Element const& element) const
    {
        if (!images)
            return nullptr;
        auto const it = images->find(&element);
        return it == images->end() ? nullptr : it->second;
    }

    // An <img> becomes an image item when a picture or a size is known;
    // otherwise its alt text stands in.
    void append_image(dom::Element const& element, ComputedStyle const* style,
        std::vector<InlineItem>& items) const
    {
        std::shared_ptr<Bitmap const> image = image_for(element);
        bool const sized = !style->width.is_auto() || !style->height.is_auto()
            || attribute_length(element, "width") || attribute_length(element, "height");
        if (image || sized) {
            InlineItem item(InlineItem::Kind::Image, {}, style, &element);
            item.image = std::move(image);
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
            if (element.is_html("br")) {
                items.push_back(InlineItem { InlineItem::Kind::HardBreak, {}, inherited, &element });
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

    // Lays the items into lines; returns the total height used.
    float layout_lines(std::vector<InlineItem> const& items, ComputedStyle const& block_style,
        float content_x, float content_y, float content_width, Fragment& out) const
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
            float x = content_x;
            if (block_style.text_align == css::TextAlign::Center)
                x += (content_width - line_width) / 2.0f;
            else if (block_style.text_align == css::TextAlign::Right)
                x += content_width - line_width;
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
        };

        bool const allow_wrap = block_style.white_space != WhiteSpace::NoWrap
            && block_style.white_space != WhiteSpace::Pre;

        for (InlineItem const& item : items) {
            if (item.kind == InlineItem::Kind::HardBreak) {
                flush_line();
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
                std::optional<ReplacedSize> const size
                    = replaced_size(*item.element, *item.style, item.image.get(), content_width);
                if (!size)
                    continue;
                if (allow_wrap && !line.empty() && line_width + size->width > content_width)
                    flush_line();
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
            if (allow_wrap && !line.empty() && line_width + width > content_width)
                flush_line();
            if (allow_wrap && content_width > 0) {
                // Emergency break: slice a word that cannot fit a whole line,
                // at the last glyph that still fits (one at least).
                while (line.empty() && width > content_width) {
                    std::size_t fit = 0;
                    float fit_width = 0;
                    if (fonts_for(*item.style).faces().size() == 1) {
                        // Fixed pitch: the count is a division.
                        float const advance = measure(*item.style, U" ");
                        fit = std::max<std::size_t>(1, static_cast<std::size_t>(content_width / advance));
                        fit = std::min(fit, word.size());
                        fit_width = static_cast<float>(fit) * advance;
                    } else {
                        for (std::size_t i = 0; i < word.size(); ++i) {
                            float const glyph_width
                                = measure(*item.style, std::u32string_view(word).substr(i, 1));
                            if (fit > 0 && fit_width + glyph_width > content_width)
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
    Fragment layout_block(dom::Element const& element, ComputedStyle const& style, float x,
        float y, float containing_width, int list_depth) const
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
            border_box_width = containing_width - margin_left - margin_right;
        } else {
            border_box_width = resolve(style.width, containing_width) + padding_left
                + padding_right + border_left + border_right;
            // Both margins auto with a definite width: center.
            if (style.margin_left.is_auto() && style.margin_right.is_auto())
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
            std::shared_ptr<Bitmap const> image = image_for(element);
            std::optional<ReplacedSize> const size
                = replaced_size(element, style, image.get(), content_width);
            if (size) {
                if (style.width.is_auto())
                    fragment.width = size->width + border_left + border_right + padding_left + padding_right;
                fragment.height = size->height + border_top + border_bottom + padding_top + padding_bottom;
                fragment.image = Fragment::ImageBox { std::move(image), content_x, content_y, size->width,
                    size->height };
                return fragment;
            }
        }

        float const content_height = layout_children(element, style, content_x, content_y,
            content_width, fragment, list_depth);

        float used_height = content_height;
        if (!style.height.is_auto() && style.height.kind == LengthPercent::Kind::Px)
            used_height = style.height.value;
        fragment.height = used_height + border_top + border_bottom + padding_top + padding_bottom;
        return fragment;
    }

    // Lays out the children of a block container; returns the content height.
    float layout_children(dom::Element const& element, ComputedStyle const& style,
        float content_x, float content_y, float content_width, Fragment& fragment,
        int list_depth) const
    {
        // Does this element establish a block or an inline formatting context?
        bool has_block_child = false;
        for (dom::Node const* child : element.children()) {
            if (!child->is_element())
                continue;
            ComputedStyle const* child_style = style_of(static_cast<dom::Element const&>(*child));
            if (child_style && is_block_level(*child_style))
                has_block_child = true;
        }

        if (!has_block_child) {
            std::vector<InlineItem> items;
            collect_inline(element, &style, items);
            if (items.empty())
                return 0;
            return layout_lines(items, style, content_x, content_y, content_width, fragment);
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
                if (item.kind == InlineItem::Kind::Word)
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
                cursor + previous_bottom_margin, content_width, anonymous);
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
            Fragment child_fragment = layout_block(child_element, *child_style, content_x,
                cursor + gap, content_width, child_list_depth);

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
        if (element.is_html("br")) {
            items.push_back(InlineItem { InlineItem::Kind::HardBreak, {}, &style, &element });
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

    result.root = layouter.layout_block(*html, *html_style, 0, 0, viewport_width, 0);
    result.page_height = result.root.y + result.root.height
        + resolve(html_style->margin_bottom, viewport_width);
    return result;
}

}
