#include "layout/Layout.h"

#include "core/Unicode.h"
#include "dom/Dom.h"
#include "text/SashfoldMono.h"

#include <algorithm>
#include <string>

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

// One inline item: a word, a space, or a hard break, carrying its style.
struct InlineItem {
    enum class Kind {
        Word,
        Space,
        HardBreak,
    };
    Kind kind = Kind::Word;
    std::u32string text;
    ComputedStyle const* style = nullptr;
    dom::Element const* element = nullptr; // nearest element, for hit-testing
};

struct Layouter {
    css::StyleMap const& styles;

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
                // Images arrive in M4; the alt text stands in.
                if (dom::Attr const* alt = element.find_attribute("alt"); alt && !alt->value.empty())
                    append_text(decode_utf8("[" + alt->value + "]"), style, items, &element);
                continue;
            }
            collect_inline(element, style, items);
        }
    }

    // --- Inline layout: line building ----------------------------------------

    static float ascent_in_line(ComputedStyle const& style)
    {
        // The half-leading model: the em box centers in its line-height.
        float const leading = style.line_height_px() - style.font_size;
        return style.font_size * 25.0f / 32.0f + leading / 2.0f;
    }

    // Lays the items into lines; returns the total height used.
    float layout_lines(std::vector<InlineItem> const& items, ComputedStyle const& block_style,
        float content_x, float content_y, float content_width, Fragment& out) const
    {
        struct Placed {
            std::u32string text;
            ComputedStyle const* style;
            bool is_space;
            float width;
            dom::Element const* element;
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
            float x = content_x;
            if (block_style.text_align == css::TextAlign::Center)
                x += (content_width - line_width) / 2.0f;
            else if (block_style.text_align == css::TextAlign::Right)
                x += content_width - line_width;
            float const baseline = y + max_ascent;
            for (Placed& placed : line) {
                float const width = placed.width;
                if (!placed.text.empty())
                    out.runs.push_back(TextRun { x, baseline, std::move(placed.text), placed.style, placed.element });
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
                float const width = text::SashfoldMono::measure(item.text, item.style->font_size);
                line.push_back(Placed { item.text, item.style, true, width, item.element });
                line_width += width;
                continue;
            }
            std::u32string word = item.text;
            float width = text::SashfoldMono::measure(word, item.style->font_size);
            if (allow_wrap && !line.empty() && line_width + width > content_width)
                flush_line();
            if (allow_wrap && content_width > 0) {
                // Emergency break: slice a word that cannot fit a whole line.
                float const advance = text::SashfoldMono::advance(item.style->font_size);
                std::size_t const fit = std::max<std::size_t>(1,
                    static_cast<std::size_t>(content_width / advance));
                while (line.empty() && word.size() > fit) {
                    line.push_back(Placed { word.substr(0, fit), item.style, false,
                        static_cast<float>(fit) * advance, item.element });
                    line_width += line.back().width;
                    flush_line();
                    word = word.substr(fit);
                }
                width = text::SashfoldMono::measure(word, item.style->font_size);
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
            if (dom::Attr const* alt = element.find_attribute("alt"); alt && !alt->value.empty())
                append_text(decode_utf8("[" + alt->value + "]"), &style, items, &element);
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
        float const width = text::SashfoldMono::measure(marker, style.font_size);
        float const gap = style.font_size * 0.4f;
        float const baseline = item_fragment.y + style.border_top.width
            + resolve(style.padding_top, 0) + ascent_in_line(style);
        item_fragment.runs.insert(item_fragment.runs.begin(),
            TextRun { item_fragment.x - width - gap, baseline, std::move(marker), &style, item_fragment.element });
    }
};

} // namespace

LayoutResult layout_document(dom::Document const& document, css::StyleMap const& styles,
    float viewport_width)
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

    Layouter layouter { styles };
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
