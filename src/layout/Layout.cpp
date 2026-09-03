#include "layout/Layout.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "dom/Dom.h"
#include "layout/GridAlgorithm.h"
#include "layout/TableBorders.h"
#include "layout/TableStructure.h"
#include "layout/TableWidths.h"
#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <tuple>
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
    case LengthPercent::Kind::Calc: return length.value + percent_base * length.percent / 100.0f;
    // A content keyword has no length in it: the sites that can size a box
    // from its own content read it before they ever get here, and the rest
    // treat it as auto (see LengthPercent::is_auto).
    case LengthPercent::Kind::MinContent:
    case LengthPercent::Kind::MaxContent:
    case LengthPercent::Kind::FitContent: return 0;
    }
    return 0;
}

bool is_block_level(ComputedStyle const& style)
{
    return css::is_block_level_display(style.display);
}

bool is_floating(ComputedStyle const& style)
{
    return style.floating != css::Float::None;
}

// An inline-level box laid out inside as a block would be — inline-block,
// inline-flex, inline-grid: one atomic box on its line, shrink-to-fit
// wide, its baseline the line's.
bool is_atomic_inline(ComputedStyle const& style)
{
    return style.display == Display::InlineBlock || style.display == Display::InlineFlex
        || style.display == Display::InlineGrid || style.display == Display::InlineTable;
}

bool is_flex_container(ComputedStyle const& style)
{
    return style.display == Display::Flex || style.display == Display::InlineFlex;
}

bool is_grid_container(ComputedStyle const& style)
{
    return style.display == Display::Grid || style.display == Display::InlineGrid;
}

// Whether a box's contents form their own block formatting context: its
// floats stay inside it, and its own box keeps clear of floats outside.
bool establishes_bfc(ComputedStyle const& style)
{
    return is_floating(style) || style.display == Display::FlowRoot
        || style.display == Display::Flex || style.display == Display::Grid
        || style.display == Display::Table || is_atomic_inline(style)
        || style.overflow != css::Overflow::Visible;
}

// CSS 2.1 §8.3.1: two adjoining vertical margins collapse into one — the
// larger of two positive margins, the more negative of two negative ones,
// the sum of a positive and a negative.
float collapse_margins(float a, float b)
{
    if (a >= 0 && b >= 0)
        return std::max(a, b);
    if (a < 0 && b < 0)
        return std::min(a, b);
    return a + b;
}

// Text that lays out as nothing between blocks.
bool is_blank(std::string const& text)
{
    for (char const c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
            return false;
    }
    return true;
}

// The document's element: its margins collapse with nothing.
bool is_root(dom::Element const& element)
{
    return element.parent() != nullptr && !element.parent()->is_element();
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
        Control, // a form control: an atomic box sized by its kind
        SoftBreak, // ends the line when it holds anything: a block inside inline content
        Absolute, // an absolutely positioned box met here: records its static position, takes no room
        Block, // an inline-block (or inline-flex, inline-grid, inline-table): an atomic box laid out as a block inside
        Table, // an anonymous inline-table around a run of table parts met in inline content (`nodes`)
        // Where an inline box opens and closes. Its margin, border and
        // padding on that side take room on the line (CSS 2.1 §9.4.2 and
        // §8.4); everything between the two is inside it.
        BoxStart,
        BoxEnd,
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
    // The nearest inline box around this item (itself included) whose
    // vertical-align is not baseline: the item sits on the line by it.
    // Null for the block's own text, which has no inline box.
    ComputedStyle const* aligned = nullptr;
    std::vector<dom::Node const*> nodes; // Kind::Table: the table parts the anonymous table wraps
};

// Gives the items appended since `first` the alignment of the inline box
// they were collected in, when they have none nearer and the box has one.
void mark_aligned(std::vector<InlineItem>& items, std::size_t first, ComputedStyle const& box)
{
    if (box.vertical_align.kind == css::VerticalAlign::Kind::Baseline)
        return;
    for (std::size_t i = first; i < items.size(); ++i) {
        if (!items[i].aligned)
            items[i].aligned = &box;
    }
}

// Whether an item puts something on a line: a word, a picture, a control
// or an inline-block (spaces and breaks alone make no line, and a float
// rides with content).
bool is_inline_content(InlineItem const& item)
{
    return item.kind == InlineItem::Kind::Word || item.kind == InlineItem::Kind::Image
        || item.kind == InlineItem::Kind::Control || item.kind == InlineItem::Kind::Block
        || item.kind == InlineItem::Kind::Table;
}

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
        return LengthPercent::percent_of(value);
    return LengthPercent::px(value);
}

// A non-negative integer attribute (size, cols, rows), else the fallback.
int attribute_int(dom::Element const& element, char const* name, int fallback)
{
    dom::Attr const* attribute = element.find_attribute(name);
    if (!attribute)
        return fallback;
    std::string_view text = attribute->value;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n'))
        text.remove_prefix(1);
    int value = 0;
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9' && value < 100000) {
        value = value * 10 + (text[digits] - '0');
        ++digits;
    }
    return digits == 0 ? fallback : value;
}

// Whether a written size names the border box rather than the content box.
bool sizes_border_box(ComputedStyle const& style)
{
    return style.box_sizing == css::BoxSizing::BorderBox;
}

// A written size as a content size. Under border-box the padding and the
// borders are counted inside the written value, so they come off it, and
// what is left is floored at zero.
float as_content_size(ComputedStyle const& style, float written, float edges)
{
    return sizes_border_box(style) ? std::max(0.0f, written - edges) : written;
}

// A size written as one of css-sizing-3 §5's content keywords, given the
// box's own min-content and max-content sizes and the room it has to grow
// into: its narrowest, its widest, or — for fit-content — the narrowest
// held up to the room but never past the widest, which is the same formula
// shrink-to-fit has always used. Callers ask `is_content_size()` first.
float content_size_of(LengthPercent const& size, float min_content, float max_content, float available)
{
    if (size.kind == LengthPercent::Kind::MinContent)
        return min_content;
    if (size.kind == LengthPercent::Kind::MaxContent)
        return max_content;
    return std::min(std::max(min_content, available), max_content);
}

// What a box carries either side of its content, and above and below it.
// A percentage padding is of the containing width in both axes, as CSS
// says; without a containing width to go on it reads as zero.
float horizontal_edges_of(ComputedStyle const& style, float containing_width)
{
    return resolve(style.padding_left, containing_width) + resolve(style.padding_right, containing_width)
        + style.border_left.width + style.border_right.width;
}

float vertical_edges_of(ComputedStyle const& style, float containing_width)
{
    return resolve(style.padding_top, containing_width) + resolve(style.padding_bottom, containing_width)
        + style.border_top.width + style.border_bottom.width;
}

// A width held within the style's min-width and max-width, percentages
// against the containing width; an auto minimum is zero, an auto maximum
// is none. `horizontal_edges` is what the box carries either side, needed
// only to read a border-box bound as a content one — left out, it is taken
// from the style, which is right whenever the padding is not a percentage
// of a width this call was not told.
float clamp_width(ComputedStyle const& style, float width, float containing_width,
    std::optional<float> horizontal_edges = std::nullopt)
{
    // A percentage bound needs a containing width to resolve against;
    // without one (intrinsic sizing) it reads as none.
    auto const applies = [&](LengthPercent const& bound) {
        return !bound.is_auto() && (bound.kind == LengthPercent::Kind::Px || containing_width > 0);
    };
    float const edges = horizontal_edges.value_or(horizontal_edges_of(style, containing_width));
    auto const bound = [&](LengthPercent const& length) {
        return as_content_size(style, resolve(length, containing_width), edges);
    };
    if (applies(style.max_width))
        width = std::min(width, bound(style.max_width));
    if (applies(style.min_width))
        width = std::max(width, bound(style.min_width));
    return std::max(0.0f, width);
}

// The same for a height: a length always, a percentage only against a
// definite containing height — without one it reads as none, as the
// specification says.
float clamp_height(ComputedStyle const& style, float height, std::optional<float> containing_height = std::nullopt,
    std::optional<float> vertical_edges = std::nullopt)
{
    auto const applies = [&](LengthPercent const& bound) {
        return !bound.is_auto() && (bound.kind == LengthPercent::Kind::Px || containing_height.has_value());
    };
    // A vertical percentage padding is of the containing WIDTH, which this
    // call does not know; a caller with padding written that way passes the
    // edges in rather than leaving them to be read off the style here.
    float const edges = vertical_edges.value_or(vertical_edges_of(style, 0));
    auto const bound = [&](LengthPercent const& length) {
        return as_content_size(style, resolve(length, containing_height.value_or(0)), edges);
    };
    if (applies(style.max_height))
        height = std::min(height, bound(style.max_height));
    if (applies(style.min_height))
        height = std::max(height, bound(style.min_height));
    return std::max(0.0f, height);
}

// A written height: a length, or a percentage of a definite containing
// height; nullopt when it is auto or has no base.
std::optional<float> definite_height_of(ComputedStyle const& style, std::optional<float> containing_height,
    std::optional<float> vertical_edges = std::nullopt)
{
    float const edges = vertical_edges.value_or(vertical_edges_of(style, 0));
    if (style.height.is_auto())
        return std::nullopt;
    if (style.height.kind == LengthPercent::Kind::Px)
        return as_content_size(style, style.height.value, edges);
    if (containing_height)
        return as_content_size(style, resolve(style.height, *containing_height), edges);
    return std::nullopt;
}

// The used size of a replaced box from its CSS width and height, else its
// width and height attributes, else its intrinsic size. With `keep_ratio`
// one given dimension scales the other by the intrinsic ratio (a picture);
// without it the other stays intrinsic (a control). A box wider than its
// container shrinks to fit, and min/max bounds hold it, the ratio kept.
// nullopt when nothing sizes it.
// `edges_inside` says the size returned already covers the box's border and
// padding — true of a form control, whose own edges live inside the size it
// reports — so a border-box width names it directly and nothing comes off.
std::optional<ReplacedSize> sized_box(dom::Element const& element, ComputedStyle const& style,
    std::optional<ReplacedSize> const& intrinsic, float containing_width, bool keep_ratio,
    std::optional<float> containing_height = std::nullopt, bool edges_inside = false)
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
    float const intrinsic_width = intrinsic ? intrinsic->width : 0;
    float const intrinsic_height = intrinsic ? intrinsic->height : 0;
    // What the box carries either side of its picture. Under border-box the
    // written width and height hold these, so they come off the used size.
    // Vertical padding percentages are of the containing width, as CSS says.
    float const horizontal_edges = edges_inside ? 0.0f : horizontal_edges_of(style, containing_width);
    float const vertical_edges = edges_inside ? 0.0f : vertical_edges_of(style, containing_width);
    std::optional<float> used_width;
    std::optional<float> used_height;
    if (!width.is_auto())
        used_width = as_content_size(style, resolve(width, containing_width), horizontal_edges);
    if (!height.is_auto()) {
        // A percentage height needs a definite containing height; without one it is auto.
        if (height.kind == LengthPercent::Kind::Px)
            used_height = as_content_size(style, height.value, vertical_edges);
        else if (containing_height)
            used_height = as_content_size(style, resolve(height, *containing_height), vertical_edges);
    }
    if (!used_width && !used_height) {
        if (!intrinsic)
            return std::nullopt;
        used_width = intrinsic_width;
        used_height = intrinsic_height;
    } else if (!used_width) {
        if (!keep_ratio && intrinsic)
            used_width = intrinsic_width;
        else
            used_width = intrinsic && intrinsic_height > 0
                ? *used_height * intrinsic_width / intrinsic_height
                : *used_height;
    } else if (!used_height) {
        if (!keep_ratio && intrinsic)
            used_height = intrinsic_height;
        else
            used_height = intrinsic && intrinsic_width > 0
                ? *used_width * intrinsic_height / intrinsic_width
                : *used_width;
    }
    if (containing_width > 0 && *used_width > containing_width) {
        float const scale = containing_width / *used_width;
        used_width = containing_width;
        if (keep_ratio)
            used_height = *used_height * scale;
    }
    // The bounds: a width held by its bounds scales the height with it
    // when the ratio is kept, then the height's own bounds hold that.
    if (float const bounded = clamp_width(style, *used_width, containing_width, horizontal_edges);
        bounded != *used_width) {
        if (keep_ratio && *used_width > 0)
            used_height = *used_height * bounded / *used_width;
        used_width = bounded;
    }
    if (float const bounded = clamp_height(style, *used_height, containing_height, vertical_edges);
        bounded != *used_height) {
        if (keep_ratio && *used_height > 0)
            used_width = *used_width * bounded / *used_height;
        used_height = bounded;
    }
    return ReplacedSize { std::max(0.0f, *used_width), std::max(0.0f, *used_height) };
}

// A replaced element: a picture, or one of the embedded kinds that lay out
// as a box of their own and show none of their children (an iframe, a
// canvas, a video, an embed or an object).
bool is_replaced(dom::Element const& element)
{
    return element.is_html("img") || element.is_html("iframe") || element.is_html("canvas")
        || element.is_html("video") || element.is_html("embed") || element.is_html("object");
}

// A replaced element with an intrinsic ratio: a picture, or a canvas
// (whose size is its own). An iframe, an embed, an object or a video
// without a picture has a default size but no ratio: one written
// dimension leaves the other at its default.
bool keeps_ratio(dom::Element const& element)
{
    return element.is_html("img") || element.is_html("canvas");
}

// An image box: the picture's pixels over the density its source was
// chosen at give the intrinsic size. An embedded element with no picture
// of its own has none, and CSS 2.1 §10.3.2 gives it 300 by 150.
std::optional<ReplacedSize> replaced_size(dom::Element const& element, ComputedStyle const& style,
    Bitmap const* image, float density, float containing_width,
    std::optional<float> containing_height = std::nullopt)
{
    std::optional<ReplacedSize> intrinsic;
    if (image) {
        float const px_per_pixel = density > 0 ? 1.0f / density : 1.0f;
        intrinsic = ReplacedSize { static_cast<float>(image->width()) * px_per_pixel,
            static_cast<float>(image->height()) * px_per_pixel };
    } else if (!element.is_html("img") && is_replaced(element)) {
        intrinsic = ReplacedSize { 300, 150 };
    }
    return sized_box(element, style, intrinsic, containing_width, keeps_ratio(element), containing_height);
}

// The edges an inline-level replaced box carries on its line: margins
// outside its border box, border and padding around its content.
struct InlineEdges {
    float margin_left = 0;
    float margin_right = 0;
    float margin_top = 0;
    float margin_bottom = 0;
    float left = 0; // border and padding
    float right = 0;
    float top = 0;
    float bottom = 0;
};

InlineEdges inline_edges(ComputedStyle const& style, float containing_width)
{
    InlineEdges edges;
    edges.margin_left = resolve(style.margin_left, containing_width);
    edges.margin_right = resolve(style.margin_right, containing_width);
    edges.margin_top = resolve(style.margin_top, containing_width);
    edges.margin_bottom = resolve(style.margin_bottom, containing_width);
    edges.left = style.border_left.width + resolve(style.padding_left, containing_width);
    edges.right = style.border_right.width + resolve(style.padding_right, containing_width);
    edges.top = style.border_top.width + resolve(style.padding_top, containing_width);
    edges.bottom = style.border_bottom.width + resolve(style.padding_bottom, containing_width);
    return edges;
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

    // The room between the floats anywhere within [y0, y1): what a line
    // box or a box with its own formatting context, that tall, may use
    // without overlapping a float along its height (CSS 2.1 §9.5).
    Band band_over(float y0, float y1, float left, float right) const
    {
        Band band { left, right };
        for (FloatBox const& box : floats) {
            if (box.top >= y1 || box.bottom <= y0)
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

// How a block is laid out beyond what its style says: a float or a flex
// item arrives with its size settled by its formatting context.
struct BlockOptions {
    std::optional<float> content_width; // the used content width, whatever the style says
    std::optional<float> content_height; // the used content height, likewise
    // A table cell: CSS 2.1 §17.5.3 makes its written height a floor for the
    // row, not a size for the box — the row settles first and the cell's
    // content is then aligned inside it. The box is laid out to its content
    // and the caller carries the floor, so `vertical-align` has room to work.
    bool height_is_minimum = false;
    // A list item whose marker goes INSIDE its box: the marker's text, to be
    // put at the head of the item's own inline content so the first line
    // begins with it and everything else moves along. Empty for a marker
    // that hangs outside, which is placed after the box is laid out.
    std::u32string inside_marker;
    bool zero_auto_margins = false; // floats and flex items: auto margins are zero, never centering
    bool own_context = false; // the box forms its own formatting context regardless of style
    // The containing block's content height when it is definite: the base
    // for percentage heights (and min/max-height) of this box.
    std::optional<float> containing_height;
};

struct Layouter {
    css::StyleMap const& styles;
    // The faces each style resolved to, looked up once per style.
    mutable std::unordered_map<ComputedStyle const*, text::FontStack const*> fonts;
    ImageMap const* images = nullptr;
    ControlStates const* controls = nullptr;
    // A flex item's content height at a width, remembered: an item is
    // measured before it is placed, and an item that is itself a flex
    // container measures its own items each time, so without this a chain
    // of nested containers costs two to the power of its depth.
    mutable std::map<std::tuple<dom::Element const*, float, float, int>, float> measured_heights;

    // Absolutely positioned boxes wait for their containing block — the
    // nearest positioned ancestor, else the initial one — to finish, then
    // are placed in its padding box: one list per positioned ancestor being
    // laid out (the root's first), and the fixed ones for the viewport.
    struct OutOfFlow {
        dom::Element const* element = nullptr;
        ComputedStyle const* style = nullptr;
        bool static_known = false; // where the box would have been in flow
        float static_x = 0;
        float static_y = 0;
        unsigned serial = 0; // the record's order among all records made
        // A grid container that is this box's containing block gives it the
        // grid area its placement names instead of the padding box; an edge
        // the placement leaves auto keeps the padding box's (css-grid-2 §9).
        std::optional<float> area_left;
        std::optional<float> area_right;
        std::optional<float> area_top;
        std::optional<float> area_bottom;
    };
    mutable std::vector<std::vector<OutOfFlow>> absolute_stack;
    mutable std::vector<OutOfFlow> fixed_boxes;
    mutable unsigned next_serial = 0;
    // The styles of the anonymous boxes the specification generates around
    // misplaced content (a table around loose cells): made here, handed to
    // the LayoutResult at the end, so the fragments' pointers outlive this
    // layouter.
    mutable std::vector<std::shared_ptr<ComputedStyle const>> owned_styles;
    // The style a first letter wears when it was written inside an inline
    // box, from the pseudo-element's style and that box's: built once per
    // pair. Declared last: the layouter is initialised as an aggregate.
    mutable std::map<std::pair<ComputedStyle const*, ComputedStyle const*>,
        std::shared_ptr<ComputedStyle const>>
        first_letter_styles;

    ComputedStyle const& anonymous_style(ComputedStyle const& parent, Display display) const
    {
        auto style = std::make_shared<ComputedStyle>(css::inherited_style(parent));
        style->display = display;
        owned_styles.push_back(style);
        return *style;
    }

    // A copy of a settled style this layouter owns, for a box whose used
    // values are not what the cascade wrote — a collapsed table's borders.
    std::shared_ptr<ComputedStyle> owned_copy(ComputedStyle const& from) const
    {
        auto style = std::make_shared<ComputedStyle>(from);
        owned_styles.push_back(style);
        return style;
    }

    // Remembers an out-of-flow child for its containing block. A scratch
    // layout may record the same element again; the last record wins.
    void record_out_of_flow(dom::Element const& element, ComputedStyle const& style,
        std::optional<std::pair<float, float>> static_position) const
    {
        std::vector<OutOfFlow>& list = style.position == css::Position::Fixed || absolute_stack.empty()
            ? fixed_boxes
            : absolute_stack.back();
        OutOfFlow entry;
        entry.element = &element;
        entry.style = &style;
        if (static_position) {
            entry.static_known = true;
            entry.static_x = static_position->first;
            entry.static_y = static_position->second;
        }
        entry.serial = next_serial++;
        for (OutOfFlow& existing : list) {
            if (existing.element == &element) {
                existing = entry;
                return;
            }
        }
        list.push_back(entry);
    }

    // Moves the static positions recorded in the serial range [since,
    // until): a box laid out at a scratch origin and then moved (an
    // inline-block, placed once its line's baseline is known) carries the
    // static positions of the out-of-flow boxes met inside it along.
    void shift_recorded(unsigned since, unsigned until, float dx, float dy) const
    {
        auto const shift = [&](std::vector<OutOfFlow>& list) {
            for (OutOfFlow& entry : list) {
                if (entry.serial >= since && entry.serial < until && entry.static_known) {
                    entry.static_x += dx;
                    entry.static_y += dy;
                }
            }
        };
        for (std::vector<OutOfFlow>& list : absolute_stack)
            shift(list);
        shift(fixed_boxes);
    }

    // Moves a fragment and everything in it.
    static void shift_fragment(Fragment& fragment, float dx, float dy)
    {
        fragment.x += dx;
        fragment.y += dy;
        if (fragment.last_baseline)
            *fragment.last_baseline += dy;
        if (fragment.first_baseline)
            *fragment.first_baseline += dy;
        for (TextRun& run : fragment.runs) {
            run.x += dx;
            run.baseline_y += dy;
        }
        if (fragment.image) {
            fragment.image->x += dx;
            fragment.image->y += dy;
        }
        if (fragment.control) {
            fragment.control->x += dx;
            fragment.control->y += dy;
            if (fragment.control->caret_x)
                *fragment.control->caret_x += dx;
        }
        for (Fragment& child : fragment.children)
            shift_fragment(child, dx, dy);
    }

    // Marks a laid-out box as positioned when its style says so — it paints
    // in the positioned layer, is a stacking context with a z-index or an
    // opacity below one, and a relative one is shifted by its offsets. Called
    // once the flow has read the box's bottom, since the shift is not flow.
    static void mark_positioned(Fragment& box, ComputedStyle const& style, float containing_width)
    {
        if (style.transformed) {
            // A translation moves the box after layout, percentages of its
            // own size; any transform makes it a stacking context.
            shift_fragment(box, resolve(style.translate_x, box.width), resolve(style.translate_y, box.height));
            box.stacking_context = true;
        }
        if (!style.positioned())
            return;
        box.positioned = true;
        box.z_index = style.z_index.value_or(0);
        box.stacking_context = box.stacking_context || style.z_index.has_value() || style.opacity < 1;
        if (style.position == css::Position::Relative)
            shift_fragment(box, relative_dx(style, containing_width), relative_dy(style));
    }

    // A relatively positioned box's shift: left, else the negative of
    // right; top, else the negative of bottom (percentages of the
    // containing block's width horizontally; vertical percentages wait for
    // a definite height and count as zero).
    static float relative_dx(ComputedStyle const& style, float containing_width)
    {
        if (!style.left.is_auto())
            return resolve(style.left, containing_width);
        if (!style.right.is_auto())
            return -resolve(style.right, containing_width);
        return 0;
    }

    static float relative_dy(ComputedStyle const& style)
    {
        auto const px = [](LengthPercent const& length) {
            return length.kind == LengthPercent::Kind::Px ? length.value : 0.0f;
        };
        if (!style.top.is_auto())
            return px(style.top);
        if (!style.bottom.is_auto())
            return -px(style.bottom);
        return 0;
    }

    // Lays the collected out-of-flow boxes out against their containing
    // block's padding box and adds them to `parent` as positioned children.
    void place_out_of_flow(std::vector<OutOfFlow> const& boxes, float outer_x, float outer_y, float outer_width,
        float outer_height, Fragment& parent, int list_depth) const
    {
        for (OutOfFlow const& box : boxes) {
            ComputedStyle const& s = *box.style;
            if (s.display == Display::TableColumn)
                continue; // a column box renders nothing, positioned or not
            // The padding box of the box that contains it — or, inside a
            // grid, the area its placement named, edge by edge.
            float const cb_x = box.area_left.value_or(outer_x);
            float const cb_y = box.area_top.value_or(outer_y);
            float const cb_width = std::max(0.0f, box.area_right.value_or(outer_x + outer_width) - cb_x);
            float const cb_height = std::max(0.0f, box.area_bottom.value_or(outer_y + outer_height) - cb_y);
            float const margin_left = resolve(s.margin_left, cb_width);
            float const margin_right = resolve(s.margin_right, cb_width);
            float const margin_top = resolve(s.margin_top, cb_width);
            float const margin_bottom = resolve(s.margin_bottom, cb_width);
            float const horizontal_edges = resolve(s.padding_left, cb_width) + resolve(s.padding_right, cb_width)
                + s.border_left.width + s.border_right.width;
            bool const has_left = !s.left.is_auto();
            bool const has_right = !s.right.is_auto();
            bool const has_top = !s.top.is_auto();
            bool const has_bottom = !s.bottom.is_auto();
            float const left = has_left ? resolve(s.left, cb_width) : 0;
            float const right = has_right ? resolve(s.right, cb_width) : 0;
            float const top = has_top ? resolve(s.top, cb_height) : 0;
            float const bottom = has_bottom ? resolve(s.bottom, cb_height) : 0;

            // The content width: as written; stretched between two offsets;
            // else shrink-to-fit within what the offsets leave. A replaced
            // box keeps its own size either way (§10.3.8, §10.6.5): the
            // offsets place it, they do not stretch it.
            BlockOptions options;
            options.own_context = true;
            options.zero_auto_margins = true;
            options.containing_height = cb_height; // definite for an absolutely positioned box
            bool const replaced = is_replaced(*box.element);
            if (s.width.is_auto() && !replaced) {
                // A content keyword is a definite width, so two offsets do
                // not stretch the box: it takes its content's size and the
                // over-constrained side gives way (§10.3.7).
                if (has_left && has_right && !s.width.is_content_size()) {
                    options.content_width = std::max(0.0f,
                        cb_width - left - right - margin_left - margin_right - horizontal_edges);
                } else {
                    float const available = std::max(0.0f, cb_width - left - right - margin_left - margin_right);
                    FloatWidth const measure = float_width(*box.element, s, available);
                    if (measure.shrink)
                        options.content_width = *measure.shrink;
                }
            }
            float const vertical_edges = resolve(s.padding_top, cb_width) + resolve(s.padding_bottom, cb_width)
                + s.border_top.width + s.border_bottom.width;
            if (!s.height.is_auto() && s.height.kind == LengthPercent::Kind::Percent && cb_height > 0)
                options.content_height = clamp_height(s,
                    as_content_size(s, s.height.value / 100.0f * cb_height, vertical_edges), cb_height,
                    vertical_edges);
            else if (s.height.is_auto() && has_top && has_bottom && !replaced)
                options.content_height
                    = std::max(0.0f, cb_height - top - bottom - margin_top - margin_bottom - vertical_edges);

            // Where the box would have stood in flow — but a box laid out
            // in a grid area starts at that area's corner, wherever its
            // flow position would have been.
            bool const in_area_across = box.area_left.has_value() || box.area_right.has_value();
            bool const in_area_down = box.area_top.has_value() || box.area_bottom.has_value();
            float const static_x = box.static_known && !in_area_across ? box.static_x : cb_x;
            float const static_y = box.static_known && !in_area_down ? box.static_y : cb_y;
            // Laid out at x = 0 first, then moved: a right-anchored box needs its width.
            FloatContext own_floats;
            Fragment fragment = layout_block(*box.element, s, 0, 0, cb_width, list_depth, own_floats, options);
            if (std::optional<float> const lowest = own_floats.lowest_bottom())
                fragment.height = std::max(fragment.height, *lowest - fragment.y);
            float x;
            if (has_left && has_right && !s.width.is_auto()) {
                // Over-constrained (§10.3.7): auto margins take what is
                // left, split when both are auto; with neither auto, right
                // gives way.
                float const free = cb_width - left - right - fragment.width;
                float left_margin = margin_left;
                if (s.margin_left.is_auto() && s.margin_right.is_auto())
                    left_margin = free >= 0 ? free / 2.0f : 0.0f;
                else if (s.margin_left.is_auto())
                    left_margin = free - margin_right;
                x = cb_x + left + left_margin;
            } else if (has_left) {
                x = cb_x + left + margin_left;
            } else if (has_right) {
                x = cb_x + cb_width - right - margin_right - fragment.width;
            } else {
                x = static_x + margin_left;
            }
            float y;
            if (has_top && has_bottom && !s.height.is_auto()) {
                // Over-constrained vertically (§10.6.4): auto margins take
                // what is left, split when both are auto; else bottom gives way.
                float const free = cb_height - top - bottom - fragment.height;
                float top_margin = margin_top;
                if (s.margin_top.is_auto() && s.margin_bottom.is_auto())
                    top_margin = free >= 0 ? free / 2.0f : 0.0f;
                else if (s.margin_top.is_auto())
                    top_margin = free - margin_bottom;
                y = cb_y + top + top_margin;
            } else if (has_top) {
                y = cb_y + top + margin_top;
            } else if (has_bottom) {
                y = cb_y + cb_height - bottom - margin_bottom - fragment.height;
            } else {
                y = static_y + margin_top;
            }
            shift_fragment(fragment, x - fragment.x, y - fragment.y);
            fragment.positioned = true;
            fragment.out_of_flow = true;
            fragment.z_index = s.z_index.value_or(0);
            fragment.stacking_context = s.z_index.has_value() || s.opacity < 1 || s.transformed;
            if (s.transformed)
                shift_fragment(fragment, resolve(s.translate_x, fragment.width), resolve(s.translate_y, fragment.height));
            parent.children.push_back(std::move(fragment));
        }
    }

    PageImage image_for(dom::Element const& element) const
    {
        if (!images)
            return {};
        auto const it = images->find(&element);
        return it == images->end() ? PageImage {} : it->second;
    }

    // An <img> becomes an image item when a picture or a size is known;
    // otherwise its alt text stands in. The other replaced elements always
    // have a size (their own, or 300 by 150).
    void append_image(dom::Element const& element, ComputedStyle const* style,
        std::vector<InlineItem>& items) const
    {
        PageImage image = image_for(element);
        bool const sized = !style->width.is_auto() || !style->height.is_auto()
            || attribute_length(element, "width") || attribute_length(element, "height")
            || !element.is_html("img");
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
        return fonts_for(style).measure(text, style.font_size) + extra_spacing(style, text);
    }

    // What letter-spacing and word-spacing add to a run: one letter's worth
    // after every character, including the last, and one word's worth after
    // every word separator. The painter steps the same amounts, so a run's
    // measured width and its drawn width stay the same number.
    static float extra_spacing(ComputedStyle const& style, std::u32string_view text)
    {
        if (style.letter_spacing == 0 && style.word_spacing == 0)
            return 0;
        float extra = style.letter_spacing * static_cast<float>(text.size());
        if (style.word_spacing != 0) {
            for (char32_t const c : text) {
                if (c == U' ')
                    extra += style.word_spacing;
            }
        }
        return extra;
    }

    ComputedStyle const* style_of(dom::Element const& element) const
    {
        auto const it = styles.find(&element);
        return it == styles.end() ? nullptr : &it->second;
    }

    // --- Inline collection ----------------------------------------------------

    // Collapses whitespace per the white-space mode and appends items.
    // css-text-3 §2.1: `text-transform` changes the text that is measured and
    // drawn, never the document. `capitalize` titlecases the first letter of
    // each word; a word carries on through an apostrophe, so "don't" keeps
    // its lowercase t. ⛔ Word starts are found within one text node, so a
    // word split across an inline box (`he<b>llo</b>`) is capitalized twice.
    static std::u32string transformed(std::u32string_view text, css::TextTransform transform)
    {
        std::u32string out;
        out.reserve(text.size());
        bool at_word_start = true;
        for (char32_t const c : text) {
            switch (transform) {
            case css::TextTransform::None:
                out.push_back(c);
                break;
            case css::TextTransform::Uppercase:
                out.push_back(to_uppercase(c));
                break;
            case css::TextTransform::Lowercase:
                out.push_back(to_lowercase(c));
                break;
            case css::TextTransform::Capitalize: {
                bool const cased = to_uppercase(c) != c || to_lowercase(c) != c;
                bool const in_word = cased || (c >= U'0' && c <= U'9') || is_combining_mark(c)
                    || c == U'\'' || c == U'’';
                out.push_back(at_word_start && cased ? to_titlecase(c) : c);
                at_word_start = !in_word;
                break;
            }
            }
        }
        return out;
    }

    void append_text(std::u32string_view text, ComputedStyle const* style,
        std::vector<InlineItem>& items, dom::Element const* element) const
    {
        std::u32string const changed = style->text_transform == css::TextTransform::None
            ? std::u32string()
            : transformed(text, style->text_transform);
        if (style->text_transform != css::TextTransform::None)
            text = changed;
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
                // A space after a space collapses, and neither a float
                // between them — out of the flow — nor the edge of an inline
                // box keeps them apart.
                std::size_t back = items.size();
                while (back > 0
                    && (items[back - 1].kind == InlineItem::Kind::Float
                        || items[back - 1].kind == InlineItem::Kind::BoxStart
                        || items[back - 1].kind == InlineItem::Kind::BoxEnd))
                    --back;
                if (back == 0 || items[back - 1].kind != InlineItem::Kind::Space)
                    items.push_back(InlineItem { InlineItem::Kind::Space, U" ", style, element });
                continue;
            }
            word.push_back(c);
        }
        flush_word();
    }

    // How much of a word ::first-letter wears (CSS 2.1 §5.12.2): the
    // punctuation in front of the first letter, the letter itself with the
    // marks that hang off it, and the punctuation right behind it. Zero when
    // there is nothing to dress — a word of punctuation with no letter after
    // it, or one that begins with a space the line kept (a no-break space,
    // an en or em space, a thin one), which is not selected and leaves the
    // letter behind it unselected too.
    static std::size_t first_letter_length(std::u32string_view word)
    {
        if (!word.empty() && is_first_letter_skipped(word[0]))
            return 0;
        std::size_t n = 0;
        while (n < word.size() && is_first_letter_punctuation(word[n]))
            ++n;
        if (n == word.size())
            return 0; // punctuation alone: the letter is not here
        ++n; // the letter
        while (n < word.size() && is_combining_mark(word[n]))
            ++n;
        while (n < word.size() && is_first_letter_punctuation(word[n]))
            ++n;
        return n;
    }

    // Whether a box would draw anything of its own: a colour behind it, a
    // picture, or a side with a visible line. An inline box that draws
    // nothing needs no fragment — which is most of them, on most pages.
    static bool paints_anything(ComputedStyle const& style)
    {
        auto const draws = [](css::BorderSide const& border) {
            return border.width > 0 && border.style != css::BorderStyle::None
                && border.style != css::BorderStyle::Hidden;
        };
        return style.background_color.a != 0 || style.background_images || draws(style.border_top)
            || draws(style.border_right) || draws(style.border_bottom) || draws(style.border_left);
    }

    // What an inline box's edge takes on the line where it opens or closes:
    // its margin, border and padding on that side (CSS 2.1 §8.4 — the
    // vertical ones take no room, the horizontal ones do). A box broken over
    // several lines has these at its two ends only.
    float inline_edge(ComputedStyle const& style, bool opening, float containing_width) const
    {
        return opening ? resolve(style.margin_left, containing_width) + style.border_left.width
                + resolve(style.padding_left, containing_width)
                       : resolve(style.margin_right, containing_width) + style.border_right.width
                + resolve(style.padding_right, containing_width);
    }

    // A word that is punctuation from end to end: it leads into the letter
    // the next word holds, and comes along with it.
    static bool is_all_punctuation(std::u32string_view word)
    {
        if (word.empty())
            return false;
        for (char32_t const c : word) {
            if (!is_first_letter_punctuation(c))
                return false;
        }
        return true;
    }

    // The style the first letter actually wears. The pseudo-element's own
    // style cascaded from the block, but the spec's fictional start tag goes
    // inside the inline boxes around the letter — so an inherited property
    // the ::first-letter rules did not set comes from the box the letter was
    // written in, not from the block. A property the rules did set differs
    // from the block's own value, which is what the cascade started from.
    ComputedStyle const* first_letter_style(ComputedStyle const& letter, ComputedStyle const& block,
        ComputedStyle const* written_in) const
    {
        if (!written_in || written_in == &block)
            return &letter;
        auto const key = std::make_pair(&letter, written_in);
        if (auto const it = first_letter_styles.find(key); it != first_letter_styles.end())
            return it->second.get();
        std::shared_ptr<ComputedStyle> merged = owned_copy(letter);
        auto const take = [&](auto member) {
            if (letter.*member == block.*member)
                merged.get()->*member = written_in->*member;
        };
        take(&ComputedStyle::color);
        take(&ComputedStyle::font_size);
        take(&ComputedStyle::font_weight);
        take(&ComputedStyle::font_style);
        take(&ComputedStyle::font_family);
        take(&ComputedStyle::line_height);
        take(&ComputedStyle::white_space);
        take(&ComputedStyle::visibility);
        first_letter_styles.emplace(key, merged);
        return merged.get();
    }

    // Dresses the first letter of a block's first line in what
    // ::first-letter asked for: the run is cut out of the word that holds it
    // and becomes an item of its own, carrying the pseudo-element's style so
    // that line layout treats it as the inline box the spec's fictional tag
    // sequence describes. Leading spaces and out-of-flow boxes are stepped
    // over; anything else that puts something on a line — a picture, a
    // control, an atomic box — means the block has no first letter to dress.
    // Returns whether this run settled the question: false only when it held
    // nothing but spaces and out-of-flow boxes, so the first line is still to
    // come and a later run gets the offer.
    bool apply_first_letter(std::vector<InlineItem>& items, ComputedStyle const& block) const
    {
        ComputedStyle const* const asked = block.first_letter.get();
        if (!asked)
            return true;
        std::size_t first = 0;
        for (; first < items.size(); ++first) {
            InlineItem::Kind const kind = items[first].kind;
            if (kind != InlineItem::Kind::Space && kind != InlineItem::Kind::Float
                && kind != InlineItem::Kind::Absolute && kind != InlineItem::Kind::BoxStart
                && kind != InlineItem::Kind::BoxEnd)
                break;
        }
        if (first == items.size())
            return false;
        // A word of nothing but punctuation — an opening quotation mark a
        // ::before put there, say — leads into the letter, which the next
        // word holds: the run takes those words whole and carries on, past
        // the edges of any inline box written between them.
        std::vector<std::size_t> leading;
        std::size_t last = first;
        while (last < items.size()) {
            InlineItem::Kind const kind = items[last].kind;
            if (kind == InlineItem::Kind::BoxStart || kind == InlineItem::Kind::BoxEnd) {
                ++last;
                continue;
            }
            if (kind != InlineItem::Kind::Word || !is_all_punctuation(items[last].text))
                break;
            leading.push_back(last);
            ++last;
        }
        if (last == items.size() || items[last].kind != InlineItem::Kind::Word)
            return true; // no letter to dress: nothing but punctuation, or a box
        std::size_t const cut = first_letter_length(items[last].text);
        if (cut == 0)
            return true; // a space the line kept starts it: nothing is dressed
        ComputedStyle const* const letter = first_letter_style(*asked, block, items[last].style);
        for (std::size_t const i : leading)
            items[i].style = first_letter_style(*asked, block, items[i].style);
        InlineItem head = items[last];
        head.style = letter;
        head.text = items[last].text.substr(0, cut);
        if (cut == items[last].text.size()) {
            items[last] = std::move(head);
            return true;
        }
        items[last].text.erase(0, cut);
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(last), std::move(head));
        return true;
    }

    static css::GeneratedBox const* before_of(ComputedStyle const* style)
    {
        return style && style->generated && style->generated->before ? &*style->generated->before
                                                                       : nullptr;
    }

    static css::GeneratedBox const* after_of(ComputedStyle const* style)
    {
        return style && style->generated && style->generated->after ? &*style->generated->after
                                                                      : nullptr;
    }

    // The generated box a (element, style) pair names, when the style is
    // one of the element's own ::before or ::after styles: the block and
    // float machinery then lays the box out from its text rather than from
    // the element's children.
    css::GeneratedBox const* generated_box_of(dom::Element const& element,
        ComputedStyle const& style) const
    {
        ComputedStyle const* const own = style_of(element);
        if (!own || !own->generated)
            return nullptr;
        if (own->generated->before && &own->generated->before->style == &style)
            return &*own->generated->before;
        if (own->generated->after && &own->generated->after->style == &style)
            return &*own->generated->after;
        return nullptr;
    }

    // A ::before or ::after box among inline items: its text in its own
    // style; a floated one rides as a float; a block-level one takes lines
    // of its own, as a block inside inline content does.
    void append_generated(css::GeneratedBox const& box, dom::Element const& element,
        std::vector<InlineItem>& items) const
    {
        if (is_floating(box.style)) {
            items.push_back(InlineItem { InlineItem::Kind::Float, {}, &box.style, &element });
            return;
        }
        bool const block = is_block_level(box.style);
        if (block)
            items.push_back(InlineItem { InlineItem::Kind::SoftBreak, {}, &box.style, &element });
        std::size_t const first = items.size();
        // An inline generated box is an inline box like any other: its own
        // margin, border and padding are its, not the element's.
        if (!block)
            items.push_back(InlineItem { InlineItem::Kind::BoxStart, {}, &box.style, &element });
        std::u32string const text = decode_utf8(box.text);
        if (!text.empty())
            append_text(text, &box.style, items, &element);
        if (!block)
            items.push_back(InlineItem { InlineItem::Kind::BoxEnd, {}, &box.style, &element });
        mark_aligned(items, first, box.style);
        if (block)
            items.push_back(InlineItem { InlineItem::Kind::SoftBreak, {}, &box.style, &element });
    }

    // Collects the inline items of `node`'s content: its generated boxes
    // around its children (`inherited` is the node's own style).
    void collect_inline(dom::Node const& node, ComputedStyle const* inherited,
        std::vector<InlineItem>& items) const
    {
        dom::Element const* const owner
            = node.is_element() ? static_cast<dom::Element const*>(&node) : nullptr;
        std::vector<dom::Node const*> const children(node.children().begin(), node.children().end());
        collect_inline_nodes(children, owner, inherited, items, false);
    }

    // The same over a run of nodes: an element's children, or what an
    // anonymous box holds (`anonymous`: no generated boxes of its own).
    void collect_inline_nodes(std::vector<dom::Node const*> const& children, dom::Element const* owner,
        ComputedStyle const* inherited, std::vector<InlineItem>& items, bool anonymous) const
    {
        if (owner && !anonymous) {
            if (css::GeneratedBox const* const before = before_of(inherited))
                append_generated(*before, *owner, items);
        }
        for (std::size_t i = 0; i < children.size(); ++i) {
            dom::Node const* child = children[i];
            if (child->is_text()) {
                std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                if (!text.empty())
                    append_text(text, inherited, items, owner);
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* style = style_of(element);
            if (!style || style->display == Display::None)
                continue;
            if (is_table_internal(style->display) && !style->out_of_flow() && !is_floating(*style)) {
                // A run of table parts in inline content: an anonymous
                // inline-table around them, one atomic box on the line.
                InlineItem item(InlineItem::Kind::Table, {}, &anonymous_style(*inherited, Display::InlineTable),
                    owner);
                item.nodes = table_run(children, i);
                items.push_back(std::move(item));
                continue;
            }
            if (style->out_of_flow()) {
                // Out of the line; the line layout records where it would
                // have stood.
                items.push_back(InlineItem { InlineItem::Kind::Absolute, {}, style, &element });
                continue;
            }
            if (is_floating(*style)) {
                items.push_back(InlineItem { InlineItem::Kind::Float, {}, style, &element });
                continue;
            }
            std::size_t const first = items.size();
            if (is_control(element)) {
                items.push_back(InlineItem { InlineItem::Kind::Control, {}, style, &element });
            } else if (is_block_level(*style)) {
                // A block inside inline content takes lines of its own; a
                // block-level picture (an image link's <img> is one, as a
                // rule) is a line of its own rather than nothing, since it
                // has no children to collect.
                items.push_back(InlineItem { InlineItem::Kind::SoftBreak, {}, style, &element });
                if (is_replaced(element))
                    append_image(element, style, items);
                else
                    collect_inline(element, style, items);
                items.push_back(InlineItem { InlineItem::Kind::SoftBreak, {}, style, &element });
                continue; // a block has no inline box to align
            } else if (element.is_html("br")) {
                InlineItem item(InlineItem::Kind::HardBreak, {}, inherited, &element);
                item.clear = break_clear(element, *style);
                items.push_back(std::move(item));
            } else if (is_replaced(element)) {
                append_image(element, style, items);
            } else if (is_atomic_inline(*style)) {
                // One box on the line, laid out as a block inside.
                items.push_back(InlineItem { InlineItem::Kind::Block, {}, style, &element });
            } else {
                items.push_back(InlineItem { InlineItem::Kind::BoxStart, {}, style, &element });
                collect_inline(element, style, items);
                items.push_back(InlineItem { InlineItem::Kind::BoxEnd, {}, style, &element });
            }
            mark_aligned(items, first, *style);
        }
        if (owner && !anonymous) {
            if (css::GeneratedBox const* const after = after_of(inherited))
                append_generated(*after, *owner, items);
        }
    }

    // --- Form controls ----------------------------------------------------------

    // A control's box and what it shows, before it is placed.
    struct ControlSpec {
        ReplacedSize size; // the border box
        Fragment::ControlBox box; // kind and state; the position is filled at placement
        std::u32string shown; // the text drawn inside, cut to what fits
        std::size_t caret = 0; // an index into `shown`
        bool caret_visible = false;
        bool centered = false; // buttons center their caption
    };

    ControlSpec control_spec(dom::Element const& element, ComputedStyle const& style,
        float containing_width) const
    {
        ControlSpec spec;
        ControlKind const kind = control_kind(element);
        spec.box.kind = kind;
        spec.box.disabled = element.has_attribute("disabled") || kind == ControlKind::File;
        spec.box.focused = controls && controls->focused == &element;
        spec.box.checked = control_checked(element, controls);
        ControlState const* state = controls ? controls->find(element) : nullptr;
        float const glyph = measure(style, U"0");
        float const line = line_height_of(style);
        float const edges = 6; // a 1px border and 2px of padding each side
        ReplacedSize intrinsic { 0, line + edges };
        std::u32string text = decode_utf8(control_caption(element, controls));
        switch (kind) {
        case ControlKind::Text:
        case ControlKind::Password: {
            int const size = std::max(1, attribute_int(element, "size", 20));
            intrinsic.width = static_cast<float>(size) * glyph + edges;
            if (kind == ControlKind::Password)
                text.assign(text.size(), U'•');
            break;
        }
        case ControlKind::TextArea: {
            int const cols = std::max(1, attribute_int(element, "cols", 20));
            int const rows = std::max(1, attribute_int(element, "rows", 2));
            intrinsic.width = static_cast<float>(cols) * glyph + edges;
            intrinsic.height = static_cast<float>(rows) * line + edges;
            break;
        }
        case ControlKind::Submit:
        case ControlKind::Button:
        case ControlKind::File:
            intrinsic.width = measure(style, text) + 18; // 8px of padding and the border each side
            spec.centered = true;
            break;
        case ControlKind::Checkbox:
        case ControlKind::Radio:
            intrinsic = ReplacedSize { 13, 13 };
            break;
        case ControlKind::Select: {
            float widest = 0;
            for (std::string const& label : select_options(element, controls).labels)
                widest = std::max(widest, measure(style, decode_utf8(label)));
            intrinsic.width = widest + 20 + edges; // room for the arrow
            break;
        }
        case ControlKind::Hidden:
            break;
        }
        // A control's own border and padding are inside the size reported
        // here, so a written size names that whole box either way.
        spec.size
            = sized_box(element, style, intrinsic, containing_width, false, std::nullopt, true).value_or(intrinsic);

        // The text is cut to what fits: a caption loses its tail, a field
        // being edited loses its head so the caret stays in view.
        // The room for text: the edges, plus the arrow of a select or the
        // caret's own pixel or two in a field.
        float const inner = std::max(0.0f,
            spec.size.width - (kind == ControlKind::Select ? 26.0f : spec.centered ? 2.0f : 8.0f));
        std::size_t caret = state ? std::min(state->caret, text.size()) : text.size();
        std::size_t dropped = 0;
        if (kind != ControlKind::TextArea) {
            if (spec.centered) {
                while (!text.empty() && measure(style, text) > inner)
                    text.pop_back();
            } else {
                while (!text.empty() && measure(style, text) > inner) {
                    text.erase(0, 1);
                    ++dropped;
                }
            }
        }
        spec.shown = std::move(text);
        spec.caret = std::min(caret > dropped ? caret - dropped : 0, spec.shown.size());
        spec.caret_visible = spec.box.focused && is_text_kind(kind) && !spec.box.disabled;
        return spec;
    }

    // Gives a placed fragment a control's box and the runs of its text.
    void fill_control(Fragment& box, ControlSpec const& spec, ComputedStyle const& style) const
    {
        Fragment::ControlBox control = spec.box;
        control.x = box.x;
        control.y = box.y;
        control.width = box.width;
        control.height = box.height;
        float const line = line_height_of(style);
        float const ascent = ascent_in_line(style);
        text::FontStack const* const stack = &fonts_for(style);
        if (spec.box.kind == ControlKind::TextArea) {
            // One run per line, as many as fit; the caret sits at the end.
            float const text_x = box.x + 4;
            float baseline = box.y + 3 + ascent;
            std::size_t start = 0;
            while (baseline - ascent + line <= box.y + box.height - 2) {
                std::size_t const end = spec.shown.find(U'\n', start);
                std::u32string const text
                    = spec.shown.substr(start, end == std::u32string::npos ? std::u32string::npos : end - start);
                if (!text.empty())
                    box.runs.push_back(TextRun { text_x, baseline, text, &style, box.element, stack,
                        measure(style, text) });
                if (end == std::u32string::npos) {
                    if (spec.caret_visible)
                        control.caret_x = text_x + measure(style, text);
                    break;
                }
                start = end + 1;
                baseline += line;
            }
        } else {
            float const text_x = spec.centered
                ? box.x + (box.width - measure(style, spec.shown)) / 2.0f
                : box.x + 4;
            float const baseline = box.y + (box.height - line) / 2.0f + ascent;
            if (!spec.shown.empty())
                box.runs.push_back(TextRun { text_x, baseline, spec.shown, &style, box.element, stack,
                    measure(style, spec.shown) });
            if (spec.caret_visible)
                control.caret_x = text_x
                    + measure(style, std::u32string_view(spec.shown).substr(0, spec.caret));
        }
        box.control = control;
    }

    // --- Inline layout: line building ----------------------------------------

    // The used line-height: as written, or for `normal` what the primary
    // face asks for — its ascent, descent and line gap at this size (Ahem's
    // is exactly one em; a typical text face's a little over).
    float line_height_of(ComputedStyle const& style) const
    {
        if (style.line_height.kind != css::LineHeight::Kind::Normal)
            return style.line_height_px();
        text::FaceMetrics const metrics = fonts_for(style).primary().metrics(style.font_size);
        return metrics.ascent + metrics.descent + metrics.line_gap;
    }

    float ascent_in_line(ComputedStyle const& style) const
    {
        // The half-leading model: the face's ascent, plus half of what the
        // line-height adds beyond the face's own height.
        text::FaceMetrics const metrics = fonts_for(style).primary().metrics(style.font_size);
        float const leading = line_height_of(style) - (metrics.ascent + metrics.descent);
        return metrics.ascent + leading / 2.0f;
    }

    // Lays the items into lines beside the context's floats; returns the
    // total height used. Floats met among the items are placed as they
    // come, and their boxes join `out`.
    // `opens_block` says these items start the block's own first formatted
    // line, which is the only line text-indent moves: a block whose content
    // is broken into several anonymous runs indents the first of them alone.
    float layout_lines(std::vector<InlineItem> const& items, ComputedStyle const& block_style,
        float content_x, float content_y, float content_width, Fragment& out,
        FloatContext& floats, int list_depth, std::optional<float> containing_height = std::nullopt,
        bool opens_block = true) const
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
            float image_height = 0; // a picture's or a control's margin box height: its reach above the baseline
            bool is_image = false; // a picture or a control: an atomic box on the baseline
            std::optional<ControlSpec> control;
            // A picture's or a control's edges (a control's border and
            // padding are inside its own size, so only its margins count)
            // and the content box within them; `width` is the margin box.
            InlineEdges edges;
            float content_width = 0;
            float content_height = 0;
            // An inline-block: laid out at the origin, moved onto the line
            // at flush; its margin box reaches `ascent` above the baseline
            // and `descent` below it, and [since, until) names the
            // out-of-flow records made during its layout, moved with it.
            std::optional<Fragment> block;
            float ascent = 0;
            float descent = 0;
            unsigned since = 0;
            unsigned until = 0;
            // The inline box whose vertical-align places this on the line
            // (see InlineItem::aligned); null sits on the baseline.
            ComputedStyle const* aligned = nullptr;
            // Where an inline box opens or closes: the entry takes the room
            // that side's margin, border and padding ask for, draws nothing
            // itself, and does not save a space in front of it at the line's
            // end.
            bool box_edge = false;
        };

        // An inline box's run along one line: what to paint it as, and where
        // it starts and stops among the line's entries. A box broken over
        // several lines has one of these per line.
        struct BoxRun {
            ComputedStyle const* style;
            dom::Element const* element;
            std::size_t from; // the first entry inside it, or its opening edge
            std::size_t to; // one past the last entry, its closing edge included
            bool opened_here; // its left edge is on this line
            bool closed_here; // its right edge is on this line
        };
        float y = content_y;
        std::vector<Placed> line;
        float line_width = 0;
        // The room the current line has between floats, found where it starts.
        float line_left = content_x;
        float line_avail = content_width;
        // text-indent: how far the first line of this block starts in, and
        // nothing after it. A percentage is of the block's own content width.
        float const indent = opens_block ? resolve(block_style.text_indent, content_width) : 0.0f;
        bool first_line = true;
        // The inline boxes open where the line stands, outermost first, and
        // where each began on this line; a box still open when the line ends
        // starts the next one at its left edge.
        struct OpenBox {
            ComputedStyle const* style;
            dom::Element const* element;
            std::size_t from;
            bool opened_here;
        };
        std::vector<OpenBox> open_boxes;
        std::vector<BoxRun> box_runs; // finished on the line being built
        auto const indent_now = [&] { return first_line ? indent : 0.0f; };
        auto const start_line = [&] {
            FloatContext::Band const band = floats.band_at(y, content_x, content_x + content_width);
            line_left = band.left + indent_now();
            line_avail = std::max(0.0f, band.right - band.left - indent_now());
        };
        start_line();

        // A float met mid-line that does not fit beside what the line holds
        // goes under the line, once the line is complete: the line itself
        // runs on as if the float were not there.
        std::vector<InlineItem const*> pending_floats;
        auto const place_pending = [&] {
            if (pending_floats.empty())
                return;
            for (InlineItem const* pending : pending_floats) {
                place_float(*pending->element, *pending->style, content_x, content_x + content_width, y,
                    list_depth, floats, out);
            }
            pending_floats.clear();
            start_line();
        };

        // `last_line` is set where the line ends the block or ends at a
        // forced break: those keep their start alignment under
        // text-align: justify (CSS 2.1 §16.2), the rest are stretched.
        auto const flush_line = [&](bool last_line = false) {
            // A space at the end of a line is dropped, and an inline box
            // closing after it does not save it: the edges are stepped over
            // on the way back, and the entries they index shift with them.
            for (std::size_t i = line.size(); i-- > 0;) {
                if (line[i].box_edge)
                    continue;
                if (!line[i].is_space)
                    break;
                line_width -= line[i].width;
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(i));
                for (BoxRun& run : box_runs) {
                    run.from -= run.from > i ? 1 : 0;
                    run.to -= run.to > i ? 1 : 0;
                }
                for (OpenBox& box : open_boxes)
                    box.from -= box.from > i ? 1 : 0;
            }
            // The line box (CSS 2.1 §10.8): every box on the line reaches
            // some way above and below the baseline it is aligned to — the
            // strut of the block's own font, text by its ascent and descent
            // within its line height, a picture by its height, an
            // inline-block by its ascent and descent — raised or lowered by
            // its vertical-align; the line runs from the uppermost top to
            // the lowermost bottom. A box aligned top or bottom sits at the
            // line's edge once the rest is placed, and a taller one grows
            // the line.
            using Kind = css::VerticalAlign::Kind;
            struct Extent {
                float above;
                float below;
            };
            auto const extent_of = [&](Placed const& placed) -> Extent {
                if (placed.block)
                    return { placed.ascent, placed.descent };
                if (placed.is_image)
                    return { placed.image_height, 0 };
                float const ascent = ascent_in_line(*placed.style);
                return { ascent, line_height_of(*placed.style) - ascent };
            };
            // The parent's metrics for text-top, text-bottom and middle:
            // the block's face stands in for the nearest inline ancestor.
            text::FaceMetrics const parent
                = fonts_for(block_style).primary().metrics(block_style.font_size);
            // The alignment of a box on the line: that of the nearest
            // inline box around it (an atomic box's own included) with a
            // vertical-align; the block's direct text has no inline box, so
            // the block's own vertical-align (which applies to it as a flex
            // item or a table cell, not to its lines) never reaches it.
            auto const alignment_of = [&](Placed const& placed) -> css::VerticalAlign const& {
                static constexpr css::VerticalAlign baseline {};
                return placed.aligned ? placed.aligned->vertical_align : baseline;
            };
            auto const shift_of = [&](Placed const& placed, Extent const& extent) -> float {
                css::VerticalAlign const& align = alignment_of(placed);
                switch (align.kind) {
                case Kind::Baseline:
                case Kind::Top:
                case Kind::Bottom:
                    return 0;
                case Kind::Sub:
                    return -block_style.font_size / 5.0f;
                case Kind::Super:
                    return block_style.font_size / 3.0f;
                case Kind::TextTop:
                    return parent.ascent - extent.above;
                case Kind::TextBottom:
                    return extent.below - parent.descent;
                case Kind::Middle:
                    // The box's midpoint half the parent's x-height above
                    // the baseline, the x-height taken as half an em.
                    return block_style.font_size / 4.0f - (extent.above - extent.below) / 2.0f;
                case Kind::Length:
                    return resolve(align.offset, line_height_of(*placed.style));
                }
                return 0;
            };
            float above = ascent_in_line(block_style);
            float below = line_height_of(block_style) - above;
            std::vector<float> shifts(line.size(), 0.0f);
            for (std::size_t i = 0; i < line.size(); ++i) {
                Kind const kind = alignment_of(line[i]).kind;
                if (kind == Kind::Top || kind == Kind::Bottom)
                    continue;
                Extent const extent = extent_of(line[i]);
                shifts[i] = shift_of(line[i], extent);
                above = std::max(above, extent.above + shifts[i]);
                below = std::max(below, extent.below - shifts[i]);
            }
            float line_top = 0; // both relative to the top the baseline boxes set
            float line_bottom = above + below;
            for (Placed const& placed : line) {
                if (alignment_of(placed).kind == Kind::Top) {
                    Extent const extent = extent_of(placed);
                    line_bottom = std::max(line_bottom, extent.above + extent.below);
                }
            }
            for (Placed const& placed : line) {
                if (alignment_of(placed).kind == Kind::Bottom) {
                    Extent const extent = extent_of(placed);
                    line_top = std::min(line_top, line_bottom - (extent.above + extent.below));
                }
            }
            float const line_height = line_bottom - line_top;
            // The line must not overlap a float anywhere along its height
            // (CSS 2.1 §9.5): where a float starting lower narrows it, the
            // line takes the narrower room when its content fits, else it
            // moves below the next float and reads its room again.
            for (int round = 0; round < 16; ++round) {
                FloatContext::Band const over
                    = floats.band_over(y, y + line_height, content_x, content_x + content_width);
                float const room = std::max(0.0f, over.right - over.left);
                if (room >= line_avail - 0.01f && over.left <= line_left + 0.01f)
                    break;
                std::optional<float> const next = floats.next_bottom(y);
                if (line_width <= room || line.empty() || !next || *next <= y) {
                    line_left = over.left + indent_now();
                    line_avail = std::max(0.0f, room - indent_now());
                    break;
                }
                y = *next;
                start_line();
            }
            float const baseline = y - line_top + above;
            // Which alignment this line takes (css-text-3 §7.2): the block's,
            // except that the last line — and any line ended by a forced
            // break — answers to text-align-last. Its `auto` starts a
            // justified block's last line at the start edge and otherwise
            // leaves the block's own alignment alone, which is why
            // `text-align: justify-all` exists to ask for the other thing.
            // `start` and `end` are sides only once the block says which way
            // its content runs (css-writing-modes-4 §2.1).
            bool const rtl = block_style.direction == css::Direction::Rtl;
            css::TextAlign const start_side = rtl ? css::TextAlign::Right : css::TextAlign::Left;
            css::TextAlign const end_side = rtl ? css::TextAlign::Left : css::TextAlign::Right;
            auto const sided = [&](css::TextAlign written) {
                if (written == css::TextAlign::Start || written == css::TextAlign::MatchParent)
                    return start_side; // match-parent was settled while the style was computed
                return written == css::TextAlign::End ? end_side : written;
            };
            css::TextAlign align = sided(block_style.text_align);
            if (last_line) {
                switch (block_style.text_align_last) {
                case css::TextAlignLast::Auto:
                    if (align == css::TextAlign::Justify)
                        align = start_side;
                    break;
                case css::TextAlignLast::Start:
                case css::TextAlignLast::MatchParent:
                    align = start_side;
                    break;
                case css::TextAlignLast::End:
                    align = end_side;
                    break;
                case css::TextAlignLast::Left:
                    align = css::TextAlign::Left;
                    break;
                case css::TextAlignLast::Right:
                    align = css::TextAlign::Right;
                    break;
                case css::TextAlignLast::Center:
                    align = css::TextAlign::Center;
                    break;
                case css::TextAlignLast::Justify:
                    align = css::TextAlign::Justify;
                    break;
                }
            }
            // text-align: justify (CSS 2.1 §16.2): the room the line did not
            // use is shared out among the spaces between its words, so both
            // its edges come out flush. Only the spaces stretch — the words
            // keep the widths their font gave them — and a line with none of
            // them (one long word, or a row of pictures) stays where it is.
            // A space is one entry of its own here, which is what makes this
            // a single pass; text kept whole by `white-space: pre-wrap` holds
            // its spaces inside a word and so has nothing to stretch yet.
            if (align == css::TextAlign::Justify && line_avail > line_width
                && block_style.text_justify != css::TextJustify::None) {
                std::size_t spaces = 0;
                for (Placed const& placed : line)
                    spaces += placed.is_space ? 1 : 0;
                if (spaces > 0) {
                    float const share = (line_avail - line_width) / static_cast<float>(spaces);
                    for (Placed& placed : line) {
                        if (!placed.is_space)
                            continue;
                        placed.width += share;
                        line_width += share;
                    }
                }
            }
            float x = line_left;
            if (align == css::TextAlign::Center)
                x += (line_avail - line_width) / 2.0f;
            else if (align == css::TextAlign::Right)
                x += line_avail - line_width;
            // Where each entry begins, so the inline boxes around them can be
            // drawn once the line is laid out; the last is where it ends.
            std::vector<float> starts(line.size() + 1, x);
            // And what each entry left behind, so a relatively positioned
            // inline box can take its content with it (§9.4.3): the runs and
            // the fragments an entry adds all sit at or after these marks.
            std::vector<std::size_t> run_at(line.size() + 1, out.runs.size());
            std::vector<std::size_t> child_at(line.size() + 1, out.children.size());
            for (std::size_t i = 0; i < line.size(); ++i) {
                run_at[i] = out.runs.size();
                child_at[i] = out.children.size();
                Placed& placed = line[i];
                float const width = placed.width;
                // Where this box's own baseline lands: the line's, raised by
                // its shift; a top-aligned box's top at the line's top, a
                // bottom-aligned box's bottom at its bottom.
                Kind const kind = alignment_of(placed).kind;
                float own_baseline = baseline - shifts[i];
                if (kind == Kind::Top)
                    own_baseline = y + extent_of(placed).above;
                else if (kind == Kind::Bottom)
                    own_baseline = y + line_height - extent_of(placed).below;
                if (placed.block) {
                    // Laid out with its margin box's left edge and its
                    // border box's top at the origin: moved so the margin
                    // box's top-left lands at (x, own baseline − ascent),
                    // with the static positions recorded inside it.
                    float const dx = x;
                    float const dy = own_baseline - placed.ascent
                        + resolve(placed.style->margin_top, content_width);
                    shift_fragment(*placed.block, dx, dy);
                    shift_recorded(placed.since, placed.until, dx, dy);
                    mark_positioned(*placed.block, *placed.style, content_width);
                    out.children.push_back(std::move(*placed.block));
                } else if (placed.is_image) {
                    // The border box inside the margin box, the content
                    // (the picture) inside the border and padding.
                    InlineEdges const& e = placed.edges;
                    Fragment box;
                    box.element = placed.element;
                    box.style = placed.style;
                    box.x = x + e.margin_left;
                    box.y = own_baseline - placed.image_height + e.margin_top;
                    box.width = e.left + placed.content_width + e.right;
                    box.height = e.top + placed.content_height + e.bottom;
                    if (placed.control)
                        fill_control(box, *placed.control, *placed.style);
                    else
                        box.image = Fragment::ImageBox { placed.image, box.x + e.left, box.y + e.top,
                            placed.content_width, placed.content_height };
                    // An atomic inline box is positioned like any other: its
                    // own offsets move it off the line it was placed on, and
                    // a z-index or an opacity makes it a stacking context.
                    mark_positioned(box, *placed.style, content_width);
                    out.children.push_back(std::move(box));
                } else if (!placed.text.empty()) {
                    out.runs.push_back(TextRun { x, own_baseline, std::move(placed.text), placed.style,
                        placed.element, &fonts_for(*placed.style), width });
                }
                x += width;
                starts[i + 1] = x;
            }
            run_at[line.size()] = out.runs.size();
            child_at[line.size()] = out.children.size();
            // The inline boxes that ran along this line: a box still open at
            // its end stops here and opens again on the next, as CSS 2.1
            // §8.4 breaks it. Each is drawn around the content area its own
            // font gives it, its padding and border outside that.
            for (OpenBox const& box : open_boxes)
                box_runs.push_back(BoxRun { box.style, box.element, box.from, line.size(),
                    box.opened_here, false });
            // Which fragment each box run left in `out.children`, so a
            // relatively positioned one can be shifted with its content once
            // every run on the line has its box. Absent when the box draws
            // nothing — it is still shifted, through its content.
            std::vector<std::optional<std::size_t>> box_fragment(box_runs.size());
            for (std::size_t r = 0; r < box_runs.size(); ++r) {
                BoxRun const& run = box_runs[r];
                ComputedStyle const& s = *run.style;
                if (!paints_anything(s))
                    continue; // nothing to draw: no box, and no room taken by one
                float const left = starts[std::min(run.from, line.size())]
                    + (run.opened_here ? resolve(s.margin_left, content_width) : 0.0f);
                float const right = starts[std::min(run.to, line.size())]
                    - (run.closed_here ? resolve(s.margin_right, content_width) : 0.0f);
                text::FaceMetrics const face = fonts_for(s).primary().metrics(s.font_size);
                float const top = baseline - face.ascent - resolve(s.padding_top, content_width)
                    - s.border_top.width;
                float const bottom = baseline + face.descent + resolve(s.padding_bottom, content_width)
                    + s.border_bottom.width;
                // Where the box was broken it has no edge: the line it
                // carries on to draws neither the border it never reached
                // nor the one it has not come to yet.
                ComputedStyle const* drawn = run.style;
                if (!run.opened_here || !run.closed_here) {
                    std::shared_ptr<ComputedStyle> const copy = owned_copy(s);
                    if (!run.opened_here)
                        copy->border_left.width = 0;
                    if (!run.closed_here)
                        copy->border_right.width = 0;
                    drawn = copy.get();
                }
                Fragment box;
                box.element = run.element;
                box.style = drawn;
                box.x = left;
                box.y = top;
                box.width = std::max(0.0f, right - left);
                box.height = std::max(0.0f, bottom - top);
                box_fragment[r] = out.children.size();
                out.children.push_back(std::move(box));
            }
            // CSS 2.1 §9.4.3: a relatively positioned inline box moves with
            // everything inside it and leaves the line where it was — the
            // content after it does not close the gap. The runs are walked
            // innermost first (a box closes before the one around it), so an
            // outer shift lands on top of an inner one; a box nested inside
            // this one rides along, since its entries are inside this one's.
            for (std::size_t r = 0; r < box_runs.size(); ++r) {
                BoxRun const& run = box_runs[r];
                ComputedStyle const& s = *run.style;
                if (s.position != css::Position::Relative)
                    continue;
                float const dx = relative_dx(s, content_width);
                float const dy = relative_dy(s);
                if (dx == 0 && dy == 0)
                    continue;
                std::size_t const from = std::min(run.from, line.size());
                std::size_t const to = std::min(run.to, line.size());
                // ⚠ The box is NOT flagged positioned. Its own fragment is
                // only the paint behind the line — the text is in the
                // block's runs — so moving the fragment into the positioned
                // layer would draw the background over the words it belongs
                // to. Promoting an inline box properly means carrying its
                // runs into the layer with it; until then the shift is what
                // §9.4.3 asks for and the layer stays as it was.
                if (std::optional<std::size_t> const own = box_fragment[r])
                    shift_fragment(out.children[*own], dx, dy);
                for (std::size_t i = run_at[from]; i < run_at[to]; ++i) {
                    out.runs[i].x += dx;
                    out.runs[i].baseline_y += dy;
                }
                for (std::size_t i = child_at[from]; i < child_at[to]; ++i)
                    shift_fragment(out.children[i], dx, dy);
                // The boxes that closed inside this one have their fragments
                // past the line's own entries, so they are shifted by name.
                for (std::size_t inner = 0; inner < r; ++inner) {
                    if (box_runs[inner].from < from || box_runs[inner].to > to)
                        continue;
                    if (std::optional<std::size_t> const nested = box_fragment[inner])
                        shift_fragment(out.children[*nested], dx, dy);
                }
            }
            box_runs.clear();
            for (OpenBox& box : open_boxes) {
                box.from = 0; // it starts the next line at its left edge
                box.opened_here = false;
            }
            out.last_baseline = baseline;
            if (!out.first_baseline)
                out.first_baseline = baseline;
            y += line_height;
            line.clear();
            line_width = 0;
            first_line = false; // the indent was this line's alone
            start_line();
            place_pending();
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
            if (item.kind == InlineItem::Kind::Absolute) {
                // Its static position: an inline-level box would have begun
                // where the line stands; a block-level one on the line
                // below (taken as one line tall when this line holds
                // anything).
                bool const inline_level = item.style->blockified;
                float const static_x = inline_level ? line_left + line_width : line_left;
                float const static_y = inline_level || line.empty() ? y : y + line_height_of(block_style);
                record_out_of_flow(*item.element, *item.style, std::make_pair(static_x, static_y));
                continue;
            }
            if (item.kind == InlineItem::Kind::BoxStart || item.kind == InlineItem::Kind::BoxEnd) {
                // The box's edge — its margin, border and padding on that
                // side — takes room on the line where it opens or closes. A
                // side that asks for none leaves no entry behind, so a box
                // that neither spaces nor draws is invisible to the line; the
                // run's ends still bracket what is inside it either way.
                bool const opening = item.kind == InlineItem::Kind::BoxStart;
                float const edge = inline_edge(*item.style, opening, content_width);
                if (allow_wrap && edge > 0 && !line.empty() && line_width + edge > line_avail)
                    flush_line();
                if (opening)
                    open_boxes.push_back(OpenBox { item.style, item.element, line.size(), true });
                if (edge > 0) {
                    Placed placed({}, item.style, false, edge, item.element);
                    placed.box_edge = true;
                    placed.aligned = item.aligned;
                    line.push_back(std::move(placed));
                    line_width += edge;
                }
                if (!opening) {
                    if (!open_boxes.empty()) {
                        OpenBox const box = open_boxes.back();
                        open_boxes.pop_back();
                        box_runs.push_back(BoxRun { box.style, box.element, box.from, line.size(),
                            box.opened_here, true });
                    } else {
                        // A box that opened before these items: CSS 2.1
                        // §9.2.1.1 broke it around a block, and this run is
                        // what comes after. It closes here without opening.
                        box_runs.push_back(
                            BoxRun { item.style, item.element, 0, line.size(), false, true });
                    }
                }
                continue;
            }
            if (item.kind == InlineItem::Kind::SoftBreak) {
                // A block inside inline content: what came before it is the
                // last line of an anonymous block of its own.
                if (!line.empty())
                    flush_line(true);
                continue;
            }
            if (item.kind == InlineItem::Kind::HardBreak) {
                flush_line(true);
                if (item.clear != css::Clear::None) {
                    y = floats.cleared_y(item.clear, y);
                    start_line();
                }
                continue;
            }
            if (item.kind == InlineItem::Kind::Float) {
                // At the top of the current line when it fits beside what the
                // line holds, else under the line once it is complete.
                float const outer = float_width(*item.element, *item.style, content_width).outer();
                // Once one float waits for the line's end, the floats after
                // it on the line wait too: none may sit higher than an
                // earlier one.
                if (!pending_floats.empty() || (!line.empty() && line_width + outer > line_avail)) {
                    pending_floats.push_back(&item);
                    continue;
                }
                place_float(*item.element, *item.style, content_x, content_x + content_width, y,
                    list_depth, floats, out);
                start_line();
                continue;
            }
            if (item.kind == InlineItem::Kind::Block || item.kind == InlineItem::Kind::Table) {
                // An inline-block: shrink-to-fit wide like a float, laid
                // out at the origin now and moved onto the line at flush;
                // its margin box is what the line holds, and it sits on the
                // baseline of its last line box (CSS 2.2 §10.8.1) — its
                // bottom margin edge when it has no line; when it clips its
                // overflow, the higher of the two. An anonymous inline-table
                // is the same box, shrink-to-fit from its own measures.
                bool const anonymous_table = item.kind == InlineItem::Kind::Table;
                FloatWidth measure;
                if (anonymous_table) {
                    Intrinsic const intrinsic = table_intrinsic_widths(item.nodes, *item.style, item.element);
                    measure.border_box = std::min(std::max(intrinsic.min, content_width), intrinsic.max);
                } else {
                    measure = float_width(*item.element, *item.style, content_width);
                }
                float const outer = measure.outer();
                if (allow_wrap && !line.empty() && line_width + outer > line_avail)
                    flush_line();
                if (allow_wrap)
                    widen_for(outer);
                Placed placed({}, item.style, false, outer, item.element);
                placed.aligned = item.aligned;
                placed.since = next_serial;
                BlockOptions options;
                options.content_width = measure.shrink;
                options.zero_auto_margins = true;
                options.own_context = true;
                FloatContext own_floats;
                Fragment box = anonymous_table
                    ? layout_table(nullptr, item.element, item.nodes, *item.style, 0, 0, content_width, list_depth,
                          own_floats, options)
                    : layout_block(*item.element, *item.style, 0, 0, content_width, list_depth, own_floats,
                          options);
                placed.until = next_serial;
                if (std::optional<float> const lowest = own_floats.lowest_bottom())
                    box.height = std::max(box.height, *lowest - box.y);
                float const margin_top = resolve(item.style->margin_top, content_width);
                float const margin_bottom = resolve(item.style->margin_bottom, content_width);
                float const margin_height = margin_top + box.height + margin_bottom;
                placed.ascent = margin_height;
                if (box.last_baseline) {
                    float const line_ascent = margin_top + (*box.last_baseline - box.y);
                    bool const clips = item.style->overflow != css::Overflow::Visible
                        && item.style->overflow_applies;
                    placed.ascent = clips ? std::min(line_ascent, margin_height) : line_ascent;
                }
                placed.descent = margin_height - placed.ascent;
                placed.block = std::move(box);
                line.push_back(std::move(placed));
                line_width += outer;
                continue;
            }
            if (item.kind == InlineItem::Kind::Control) {
                // An atomic box on the baseline, like a picture; its own
                // size holds its border and padding, its margins go around.
                ControlSpec spec = control_spec(*item.element, *item.style, content_width);
                InlineEdges edges = inline_edges(*item.style, content_width);
                edges.left = edges.right = edges.top = edges.bottom = 0;
                float const width = edges.margin_left + spec.size.width + edges.margin_right;
                if (allow_wrap && !line.empty() && line_width + width > line_avail)
                    flush_line();
                if (allow_wrap)
                    widen_for(width);
                Placed placed({}, item.style, false, width, item.element);
                placed.aligned = item.aligned;
                placed.edges = edges;
                placed.content_width = spec.size.width;
                placed.content_height = spec.size.height;
                placed.image_height = edges.margin_top + spec.size.height + edges.margin_bottom;
                placed.is_image = true;
                placed.control = std::move(spec);
                line.push_back(std::move(placed));
                line_width += width;
                continue;
            }
            if (item.kind == InlineItem::Kind::Space) {
                // A leading space on a line collapses away, and an inline box
                // opening in front of it does not make it an inner one.
                bool only_edges = true;
                for (Placed const& placed : line) {
                    if (!placed.box_edge)
                        only_edges = false;
                }
                if (only_edges)
                    continue;
                float const width = measure(*item.style, item.text);
                Placed placed(item.text, item.style, true, width, item.element);
                placed.aligned = item.aligned;
                line.push_back(std::move(placed));
                line_width += width;
                continue;
            }
            if (item.kind == InlineItem::Kind::Image) {
                std::optional<ReplacedSize> const size = replaced_size(*item.element, *item.style,
                    item.image.get(), item.image_density, content_width, containing_height);
                if (!size)
                    continue;
                // The margin box is what the line holds; the bottom margin
                // edge sits on the baseline (CSS 2.1 §10.8.1).
                InlineEdges const edges = inline_edges(*item.style, content_width);
                float const width = edges.margin_left + edges.left + size->width + edges.right
                    + edges.margin_right;
                if (allow_wrap && !line.empty() && line_width + width > line_avail)
                    flush_line();
                if (allow_wrap)
                    widen_for(width);
                Placed placed({}, item.style, false, width, item.element);
                placed.aligned = item.aligned;
                placed.image = item.image;
                placed.edges = edges;
                placed.content_width = size->width;
                placed.content_height = size->height;
                placed.image_height = edges.margin_top + edges.top + size->height + edges.bottom
                    + edges.margin_bottom;
                placed.is_image = true;
                line.push_back(std::move(placed));
                line_width += width;
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
                    Placed slice(word.substr(0, fit), item.style, false, fit_width, item.element);
                    slice.aligned = item.aligned;
                    line.push_back(std::move(slice));
                    line_width += fit_width;
                    flush_line();
                    word = word.substr(fit);
                    width = measure(*item.style, word);
                }
            }
            Placed placed(std::move(word), item.style, false, width, item.element);
            placed.aligned = item.aligned;
            line.push_back(std::move(placed));
            line_width += width;
        }
        if (!line.empty())
            flush_line(true);
        place_pending();
        return y - content_y;
    }

    // --- Margin collapsing through a box (CSS 2.1 §8.3.1) ----------------------

    // Whether a box's top edge lets its first in-flow child's top margin
    // through: no top border or padding, no formatting context of its own,
    // not replaced, not the root.
    // A grid container's items are formatting context roots of their own.
    bool is_grid_item(dom::Element const& element) const
    {
        dom::Node const* parent = element.parent();
        if (!parent || !parent->is_element())
            return false;
        ComputedStyle const* parent_style = style_of(static_cast<dom::Element const&>(*parent));
        return parent_style
            && (parent_style->display == Display::Grid || parent_style->display == Display::InlineGrid);
    }

    bool top_edge_collapses(dom::Element const& element, ComputedStyle const& style,
        float containing_width, BlockOptions const& options) const
    {
        return !options.own_context && !establishes_bfc(style) && style.border_top.width == 0
            && resolve(style.padding_top, containing_width) == 0 && !is_root(element)
            && !is_replaced(element) && !is_control(element) && !is_grid_item(element);
    }

    // The same for the bottom edge, which also needs an auto height.
    bool bottom_edge_collapses(dom::Element const& element, ComputedStyle const& style,
        float containing_width, BlockOptions const& options) const
    {
        return !options.own_context && !options.content_height && !establishes_bfc(style)
            && style.border_bottom.width == 0 && resolve(style.padding_bottom, containing_width) == 0
            && style.height.is_auto() && !is_root(element) && !is_replaced(element)
            && !is_control(element) && !is_grid_item(element);
    }

    // The content width a block child will be given, before float bands.
    float content_width_of(ComputedStyle const& style, float containing_width) const
    {
        float const edges = resolve(style.padding_left, containing_width)
            + resolve(style.padding_right, containing_width) + style.border_left.width
            + style.border_right.width;
        if (style.width.is_auto())
            return std::max(0.0f, containing_width - resolve(style.margin_left, containing_width)
                    - resolve(style.margin_right, containing_width) - edges);
        return std::max(0.0f, resolve(style.width, containing_width));
    }

    // A block with both edges open and nothing in flow inside it: its top
    // and bottom margins meet, and it takes no room of its own.
    bool is_empty_block(dom::Element const& element, ComputedStyle const& style,
        float containing_width) const
    {
        // A cleared box stands where its clearance puts it: not collapsed through.
        if (style.clear != css::Clear::None)
            return false;
        // A generated box is content. So is a marker that goes inside: it
        // makes a line of its own even when the item holds nothing else.
        if (before_of(&style) || after_of(&style))
            return false;
        if (style.display == Display::ListItem
            && style.list_style_position == css::ListStylePosition::Inside
            && style.list_style_type != css::ListStyleType::None)
            return false;
        if (!top_edge_collapses(element, style, containing_width, {})
            || !bottom_edge_collapses(element, style, containing_width, {}))
            return false;
        float const inner_width = content_width_of(style, containing_width);
        for (dom::Node const* child : element.children()) {
            if (child->is_text()) {
                if (!is_blank(static_cast<dom::Text const*>(child)->data))
                    return false;
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None || is_floating(*child_style)
                || child_style->out_of_flow())
                continue;
            if (!is_block_level(*child_style) || !is_empty_block(child_element, *child_style, inner_width))
                return false;
        }
        return true;
    }

    // Whether an in-flow block-level box lives inside the element, as a
    // child or inside inline boxes (CSS 2.1 §9.2.1.1: an inline box holding
    // a block is broken around it, and the block becomes a block child of
    // the containing block). Floats, out-of-flow boxes and atomic inline
    // boxes (controls, pictures, inline-blocks) hold their contents in.
    bool contains_block_descendant(dom::Element const& element) const
    {
        for (dom::Node const* child : element.children()) {
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None || is_floating(*child_style)
                || child_style->out_of_flow())
                continue;
            // A table part outside a table gets a block-level anonymous
            // table around it here.
            if (is_block_level(*child_style) || is_table_internal(child_style->display))
                return true;
            if (splits_around_blocks(child_element, *child_style))
                return true;
        }
        return false;
    }

    // The same over a run of nodes an anonymous box holds.
    bool contains_block_in(std::vector<dom::Node const*> const& nodes) const
    {
        for (dom::Node const* child : nodes) {
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None || is_floating(*child_style)
                || child_style->out_of_flow())
                continue;
            if (is_block_level(*child_style) || is_table_internal(child_style->display))
                return true;
            if (splits_around_blocks(child_element, *child_style))
                return true;
        }
        return false;
    }

    // An inline box that a block inside it splits: a plain inline element
    // (not a control, a replaced box, an inline-block or a br) holding a
    // block-level descendant.
    bool splits_around_blocks(dom::Element const& element, ComputedStyle const& style) const
    {
        return !is_block_level(style) && !is_control(element) && !is_replaced(element)
            && !is_atomic_inline(style) && !element.is_html("br") && contains_block_descendant(element);
    }

    // Whether a float lives anywhere inside the element: an empty box that
    // holds one still places it, so the margins after that box must not
    // reach past it and carry the float along.
    bool contains_float(dom::Element const& element) const
    {
        for (dom::Node const* child : element.children()) {
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None)
                continue;
            if (is_floating(*child_style) || contains_float(child_element))
                return true;
        }
        return false;
    }

    // The margins that reach through a box's top edge from inside it: its
    // first in-flow block child's top margin joined with what reaches
    // through that child, and, while a child is empty and collapses
    // through, its bottom margin and the next child's margins in turn.
    // Inline content first means nothing reaches through; a cleared child
    // keeps its margin inside, and an empty box holding a float ends the
    // walk after its top margin.
    float collapsed_through_top(dom::Element const& element, ComputedStyle const& style,
        float containing_width) const
    {
        if (!top_edge_collapses(element, style, containing_width, {}))
            return 0;
        float const inner_width = content_width_of(style, containing_width);
        float margin = 0;
        for (dom::Node const* child : element.children()) {
            if (child->is_text()) {
                if (!is_blank(static_cast<dom::Text const*>(child)->data))
                    break;
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None || is_floating(*child_style))
                continue;
            if (!is_block_level(*child_style) || child_style->clear != css::Clear::None)
                break;
            margin = collapse_margins(margin,
                collapse_margins(resolve(child_style->margin_top, inner_width),
                    collapsed_through_top(child_element, *child_style, inner_width)));
            if (!is_empty_block(child_element, *child_style, inner_width) || contains_float(child_element))
                break;
            margin = collapse_margins(margin, resolve(child_style->margin_bottom, inner_width));
        }
        return margin;
    }

    // --- Block layout ---------------------------------------------------------

    // Lays out `element` with its border box starting at (x, y) given the
    // containing block's content width. Returns the fragment; the caller
    // advances by margins itself (margin collapsing lives there).
    // `floats` is the formatting context the box sits in; a box that forms
    // its own gives its children a fresh one. `options` carry what a float
    // or a flex item has settled already (see BlockOptions).
    Fragment layout_block(dom::Element const& element, ComputedStyle const& style, float x,
        float y, float containing_width, int list_depth, FloatContext& floats,
        BlockOptions const& options = {}) const
    {
        // A generated box named by its style: laid out from its text.
        if (css::GeneratedBox const* const box = generated_box_of(element, style))
            return layout_generated_block(*box, element, x, y, containing_width, floats,
                options.content_width, options.content_height);
        if (is_table_display(style.display)) {
            std::vector<dom::Node const*> const children(element.children().begin(), element.children().end());
            return layout_table(&element, &element, children, style, x, y, containing_width, list_depth, floats,
                options);
        }

        Fragment fragment;
        fragment.element = &element;
        fragment.style = &style;
        // A box with opacity below one paints as one unit: a stacking
        // context whose positioned descendants stay inside it.
        fragment.stacking_context = style.opacity < 1;

        // A positioned box is the containing block of the absolutely
        // positioned boxes inside it: they collect here and are placed
        // once this box's padding box is known.
        bool const containing_block = style.positioned();
        if (containing_block)
            absolute_stack.emplace_back();
        struct PopOnExit {
            std::vector<std::vector<OutOfFlow>>& stack;
            bool active;
            std::vector<OutOfFlow> taken;
            ~PopOnExit()
            {
                if (active && !stack.empty())
                    stack.pop_back();
            }
        } pop { absolute_stack, containing_block, {} };

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

        float const horizontal_edges = padding_left + padding_right + border_left + border_right;
        float border_box_width;
        float extra_left = margin_left;
        if (options.content_width) {
            // Settled by a float's shrink-to-fit or a flex line.
            border_box_width = *options.content_width + horizontal_edges;
        } else if (style.width.is_content_size()) {
            // A block sized from its own content: it no longer fills the
            // room it is given, so auto margins centre it as a definite
            // width does (css-sizing-3 §5.1).
            Intrinsic const intrinsic = intrinsic_widths(element, style);
            float const available = std::max(0.0f,
                containing_width - margin_left - margin_right - horizontal_edges);
            border_box_width = clamp_content_bounds(style,
                                   clamp_width(style,
                                       content_size_of(style.width, intrinsic.min, intrinsic.max, available),
                                       containing_width, horizontal_edges),
                                   intrinsic, available)
                + horizontal_edges;
            if (!options.zero_auto_margins && style.margin_left.is_auto()
                && style.margin_right.is_auto())
                extra_left = (containing_width - border_box_width) / 2.0f;
        } else if (style.width.is_auto()) {
            float const available = containing_width - margin_left - margin_right - horizontal_edges;
            border_box_width
                = clamp_width(style, available, containing_width, horizontal_edges) + horizontal_edges;
            // A bound written as a content keyword needs the content
            // measured, which is why it is only measured when one is.
            if (has_content_bound(style)) {
                Intrinsic const intrinsic = intrinsic_widths(element, style);
                border_box_width = clamp_content_bounds(style, border_box_width - horizontal_edges,
                                       intrinsic, std::max(0.0f, available))
                    + horizontal_edges;
            }
        } else {
            border_box_width = clamp_width(style,
                                   as_content_size(style, resolve(style.width, containing_width), horizontal_edges),
                                   containing_width, horizontal_edges)
                + horizontal_edges;
            // Both margins auto with a definite width: center (a float's or a
            // flex item's auto margins are zero instead).
            if (!options.zero_auto_margins && style.margin_left.is_auto()
                && style.margin_right.is_auto())
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

        if (is_replaced(element)) {
            // A block-level picture (or embedded box): its own size, shrunk
            // to fit, no children. A box without a ratio of its own takes
            // the size a formatting context settled for it (a flex line's
            // item); a picture keeps its ratio and its own size.
            PageImage image = image_for(element);
            // A percentage width is of the containing block; an auto width
            // shrinks to the room this box has.
            std::optional<ReplacedSize> const size = replaced_size(element, style, image.bitmap.get(),
                image.density, style.width.is_auto() ? content_width : containing_width,
                options.containing_height);
            if (size) {
                bool const settled = !keeps_ratio(element);
                float width = settled ? options.content_width.value_or(size->width) : size->width;
                float height = settled ? options.content_height.value_or(size->height) : size->height;
                // A picture whose written width the caller already resolved
                // against the right base takes it, the ratio following.
                if (!settled && options.content_width && !style.width.is_auto()) {
                    if (size->width > 0 && style.height.is_auto())
                        height = *options.content_width * size->height / size->width;
                    width = *options.content_width;
                }
                if (!settled && options.content_height && !style.height.is_auto())
                    height = *options.content_height;
                if (style.width.is_auto() || (settled && options.content_width))
                    fragment.width = width + border_left + border_right + padding_left + padding_right;
                fragment.height = height + border_top + border_bottom + padding_top + padding_bottom;
                fragment.image = Fragment::ImageBox { std::move(image.bitmap), content_x, content_y, width,
                    height };
                return fragment;
            }
        }
        if (is_control(element)) {
            // A block-level control: its own box is the whole of it — unless
            // a formatting context settled its size (a control stretched
            // between left and right, or a flex line's item).
            ControlSpec spec = control_spec(element, style, content_width);
            if (options.content_width)
                spec.size.width = *options.content_width + horizontal_edges;
            if (options.content_height)
                spec.size.height = *options.content_height + padding_top + padding_bottom + border_top + border_bottom;
            fragment.width = spec.size.width;
            fragment.height = spec.size.height;
            fill_control(fragment, spec, style);
            return fragment;
        }

        // A box that forms its own block formatting context keeps its floats
        // to itself, and its height reaches around them.
        FloatContext own_floats;
        bool const own_context = options.own_context || establishes_bfc(style);
        bool const flex = is_flex_container(style);
        bool const grid = is_grid_container(style);
        // This box's definite content height, when it has one — settled by
        // its formatting context, written as a length, or a percentage of a
        // definite containing height: the base for its children's percentages.
        float const vertical_edges = padding_top + padding_bottom + border_top + border_bottom;
        std::optional<float> const own_height = options.content_height
            ? options.content_height
            : definite_height_of(style, options.containing_height, vertical_edges);
        std::optional<float> const children_base = own_height
            ? std::optional<float>(clamp_height(style, *own_height, options.containing_height, vertical_edges))
            : std::nullopt;
        float content_height;
        if (flex) {
            content_height = layout_flex(element, style, content_x, content_y, content_width, fragment,
                list_depth, children_base);
        } else if (grid) {
            content_height = layout_grid(element, style, content_x, content_y, content_width, fragment,
                list_depth, children_base);
        } else {
            content_height = layout_children(element, style, content_x, content_y, content_width, fragment,
                list_depth, own_context ? own_floats : floats,
                top_edge_collapses(element, style, containing_width, options),
                bottom_edge_collapses(element, style, containing_width, options), children_base, nullptr,
                false, options.inside_marker);
        }
        // The box's baseline, for an inline-block's sake: its own lines set
        // it; with block children it is the last in-flow child's (a flex or
        // grid container's the first item's).
        if (!fragment.last_baseline) {
            auto const in_flow = [](Fragment const& child) { return !child.floating && !child.out_of_flow; };
            if (flex || grid) {
                for (Fragment const& child : fragment.children) {
                    if (in_flow(child) && child.last_baseline) {
                        fragment.last_baseline = child.last_baseline;
                        break;
                    }
                }
            } else {
                for (auto it = fragment.children.rbegin(); it != fragment.children.rend(); ++it) {
                    if (in_flow(*it) && it->last_baseline) {
                        fragment.last_baseline = it->last_baseline;
                        break;
                    }
                }
            }
        }
        // And its first baseline, for a table cell's sake: the first
        // in-flow child's.
        if (!fragment.first_baseline) {
            for (Fragment const& child : fragment.children) {
                if (!child.floating && !child.out_of_flow && child.first_baseline) {
                    fragment.first_baseline = child.first_baseline;
                    break;
                }
            }
        }

        float used_height = content_height;
        if (own_context) {
            if (std::optional<float> const bottom = own_floats.lowest_bottom())
                used_height = std::max(used_height, *bottom - content_y);
        }
        if (options.content_height) {
            used_height = *options.content_height;
        } else if (options.height_is_minimum) {
            // The written height and minimum are the caller's floor, so the
            // box keeps its content's height; only a maximum still binds.
            if (!style.max_height.is_auto()
                && (style.max_height.kind == LengthPercent::Kind::Px || options.containing_height))
                used_height = std::min(used_height,
                    as_content_size(style,
                        resolve(style.max_height, options.containing_height.value_or(0)), vertical_edges));
            used_height = std::max(0.0f, used_height);
        } else {
            if (std::optional<float> const written
                = definite_height_of(style, options.containing_height, vertical_edges))
                used_height = *written;
            used_height = clamp_height(style, used_height, options.containing_height, vertical_edges);
        }
        fragment.height = used_height + border_top + border_bottom + padding_top + padding_bottom;
        if (containing_block && !absolute_stack.empty() && !absolute_stack.back().empty()) {
            // The padding box is settled: place what collected inside.
            std::vector<OutOfFlow> const boxes = std::move(absolute_stack.back());
            absolute_stack.back().clear();
            place_out_of_flow(boxes, fragment.x + border_left, fragment.y + border_top,
                std::max(0.0f, fragment.width - border_left - border_right),
                std::max(0.0f, fragment.height - border_top - border_bottom), fragment, list_depth);
        }
        return fragment;
    }

    // Lays out the children of a block container; returns the content height.
    // With `collapse_top` the first child's top margin was applied by the
    // caller, outside this box; with `collapse_bottom` the last child's
    // bottom margin is left out of the height and reported through
    // fragment.collapsed_bottom for the caller to apply.
    // `nodes`, when given, are the children laid out in place of the
    // element's own — an anonymous box's run — and `anonymous` leaves the
    // element's generated boxes out.
    float layout_children(dom::Element const& element, ComputedStyle const& style,
        float content_x, float content_y, float content_width, Fragment& fragment,
        int list_depth, FloatContext& floats, bool collapse_top = false,
        bool collapse_bottom = false, std::optional<float> containing_height = std::nullopt,
        std::vector<dom::Node const*> const* nodes = nullptr, bool no_generated = false,
        std::u32string_view inside_marker = {}) const
    {
        std::vector<dom::Node const*> const own_children(element.children().begin(), element.children().end());
        std::vector<dom::Node const*> const& children = nodes ? *nodes : own_children;
        // Does this element establish a block or an inline formatting context?
        css::GeneratedBox const* const before = no_generated ? nullptr : before_of(&style);
        css::GeneratedBox const* const after = no_generated ? nullptr : after_of(&style);
        bool const has_block_child = (before && is_block_level(before->style))
            || (after && is_block_level(after->style)) || contains_block_in(children);

        if (!has_block_child) {
            std::vector<InlineItem> items;
            // An inside marker leads the item's own content: it is put on the
            // line before anything else, so the first line begins with it and
            // an empty item still makes a line to hold it.
            if (!inside_marker.empty()) {
                items.push_back(
                    InlineItem { InlineItem::Kind::Word, std::u32string(inside_marker), &style, &element });
                items.push_back(InlineItem { InlineItem::Kind::Space, U" ", &style, &element });
            }
            collect_inline_nodes(children, &element, &style, items, no_generated); // the generated boxes ride along
            if (items.empty())
                return 0;
            apply_first_letter(items, style);
            return layout_lines(items, style, content_x, content_y, content_width, fragment,
                floats, list_depth, containing_height);
        }

        // Block context: inline runs between blocks wrap in anonymous boxes.
        float cursor = content_y;
        float previous_bottom_margin = 0;
        bool first_in_flow = true;
        std::vector<InlineItem> pending_inline;
        int list_index = 0;
        // Only the first line of the block wears ::first-letter, so only the
        // first anonymous run that puts anything on a line is offered it.
        bool first_letter_owed = style.first_letter != nullptr;

        auto const flush_inline = [&] {
            // Whitespace-only runs between blocks vanish.
            bool significant = false;
            for (InlineItem const& item : pending_inline) {
                if (is_inline_content(item))
                    significant = true;
            }
            if (!significant) {
                pending_inline.clear();
                return;
            }
            if (first_letter_owed && apply_first_letter(pending_inline, style))
                first_letter_owed = false;
            Fragment anonymous;
            anonymous.x = content_x;
            anonymous.y = cursor + previous_bottom_margin;
            anonymous.width = content_width;
            float const height = layout_lines(pending_inline, style, content_x,
                cursor + previous_bottom_margin, content_width, anonymous, floats, list_depth,
                containing_height, first_in_flow);
            anonymous.height = height;
            cursor += previous_bottom_margin + height;
            previous_bottom_margin = 0;
            first_in_flow = false;
            fragment.children.push_back(std::move(anonymous));
            pending_inline.clear();
        };

        // A block-level generated box is a block child like any other: its
        // margins join the running one, its clearance puts it below the
        // floats it names (the clearfix idiom), its lines are its text.
        auto const place_generated = [&](css::GeneratedBox const& box) {
            flush_inline();
            float const margin_top = resolve(box.style.margin_top, content_width);
            float const margin_bottom = resolve(box.style.margin_bottom, content_width);
            bool const held_by_caller = first_in_flow && collapse_top
                && box.style.clear == css::Clear::None;
            float y = cursor + (held_by_caller ? 0.0f : collapse_margins(previous_bottom_margin, margin_top));
            if (box.style.clear != css::Clear::None)
                y = floats.cleared_y(box.style.clear, y);
            Fragment child = layout_generated_block(box, element, content_x, y, content_width, floats);
            cursor = child.y + child.height;
            previous_bottom_margin = margin_bottom;
            first_in_flow = false;
            fragment.children.push_back(std::move(child));
        };
        auto const place_or_append = [&](css::GeneratedBox const& box) {
            if (is_floating(box.style)) {
                // As a float child: with the inline content when there is
                // some, else placed here between the blocks.
                bool content = false;
                for (InlineItem const& item : pending_inline) {
                    if (is_inline_content(item))
                        content = true;
                }
                if (content)
                    pending_inline.push_back(InlineItem { InlineItem::Kind::Float, {}, &box.style, &element });
                else
                    place_float(element, box.style, content_x, content_x + content_width,
                        cursor + previous_bottom_margin, list_depth, floats, fragment);
            } else if (is_block_level(box.style)) {
                place_generated(box);
            } else {
                append_generated(box, element, pending_inline);
            }
        };
        if (before)
            place_or_append(*before);

        // The children in a block context: text and inline-level boxes
        // gather into the pending inline content; a block-level box is a
        // block child — at any depth through inline boxes, which CSS 2.1
        // §9.2.1.1 breaks around it, their content before and after it
        // staying inline (their generated boxes at either end).
        std::function<void(std::vector<dom::Node const*> const&, dom::Element const*, ComputedStyle const&)> walk;
        walk = [&](std::vector<dom::Node const*> const& siblings, dom::Element const* parent_element,
                   ComputedStyle const& parent_style) {
        for (std::size_t i = 0; i < siblings.size(); ++i) {
            dom::Node const* child = siblings[i];
            if (child->is_text()) {
                std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                if (!text.empty())
                    append_text(text, &parent_style, pending_inline, parent_element);
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& child_element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(child_element);
            if (!child_style || child_style->display == Display::None)
                continue;
            if (is_table_internal(child_style->display) && !child_style->out_of_flow()
                && !is_floating(*child_style)) {
                // A run of table parts outside a table: an anonymous table
                // around them (CSS 2.1 §17.2.1), a block child of this box
                // with no margins, beside the floats like any formatting
                // context root.
                std::vector<dom::Node const*> const run = table_run(siblings, i);
                flush_inline();
                float const table_y = cursor + previous_bottom_margin;
                FloatContext::Band const band = floats.band_at(table_y, content_x, content_x + content_width);
                ComputedStyle const& anonymous_table = anonymous_style(parent_style, Display::Table);
                Fragment box = layout_table(nullptr, parent_element, run, anonymous_table, band.left, table_y,
                    std::max(0.0f, band.right - band.left), list_depth, floats, BlockOptions {});
                cursor = box.y + box.height;
                previous_bottom_margin = 0;
                first_in_flow = false;
                fragment.children.push_back(std::move(box));
                continue;
            }
            if (child_style->out_of_flow()) {
                // Out of the flow; it remembers where it would have been:
                // below the inline content gathered so far, taken as one
                // line (the common menu-under-a-link case; a longer run
                // before it lands short).
                bool pending_content = false;
                for (InlineItem const& item : pending_inline) {
                    if (is_inline_content(item))
                        pending_content = true;
                }
                record_out_of_flow(child_element, *child_style,
                    std::make_pair(content_x,
                        cursor + previous_bottom_margin + (pending_content ? line_height_of(style) : 0.0f)));
                continue;
            }
            if (is_floating(*child_style)) {
                // A float among inline content rides with that content; one
                // between blocks is placed here, outside the flow (its
                // siblings' margins still collapse across it).
                bool content = false;
                for (InlineItem const& item : pending_inline) {
                    if (is_inline_content(item))
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
                if (splits_around_blocks(child_element, *child_style)) {
                    // The box opens before the block breaks it and closes
                    // after: the run before the block draws no right edge,
                    // the run after it no left one (CSS 2.1 §9.2.1.1).
                    std::size_t const first = pending_inline.size();
                    pending_inline.push_back(
                        InlineItem { InlineItem::Kind::BoxStart, {}, child_style, &child_element });
                    if (css::GeneratedBox const* const own_before = before_of(child_style))
                        append_generated(*own_before, child_element, pending_inline);
                    std::vector<dom::Node const*> const grandchildren(child_element.children().begin(),
                        child_element.children().end());
                    walk(grandchildren, &child_element, *child_style);
                    if (css::GeneratedBox const* const own_after = after_of(child_style))
                        append_generated(*own_after, child_element, pending_inline);
                    pending_inline.push_back(
                        InlineItem { InlineItem::Kind::BoxEnd, {}, child_style, &child_element });
                    mark_aligned(pending_inline, first, *child_style);
                    continue;
                }
                collect_inline_element(child_element, *child_style, pending_inline);
                continue;
            }

            flush_inline();

            float const margin_top = resolve(child_style->margin_top, content_width);
            float const margin_bottom = resolve(child_style->margin_bottom, content_width);
            // Adjoining margins collapse: the previous sibling's bottom, this
            // child's top, and whatever reaches through the child's own top
            // edge from inside it. A first child whose margin the caller
            // already applied through this box's top edge starts flush.
            bool const empty = is_empty_block(child_element, *child_style, content_width);
            float const effective_top = collapse_margins(margin_top,
                collapsed_through_top(child_element, *child_style, content_width));
            // A cleared child's margin never went through this box's top edge.
            bool const held_by_caller = first_in_flow && collapse_top
                && child_style->clear == css::Clear::None;
            float const gap = held_by_caller ? 0.0f : collapse_margins(previous_bottom_margin, effective_top);
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
            BlockOptions child_options;
            child_options.containing_height = containing_height;
            // A list item counts here, before it is laid out, because an
            // inside marker is content the item lays out around (§12.5.1);
            // an outside one is hung on the box afterwards.
            bool const list_item = child_style->display == Display::ListItem;
            bool const marker_inside
                = list_item && child_style->list_style_position == css::ListStylePosition::Inside;
            if (list_item) {
                // The number is the `list-item` counter's, settled in the
                // cascade, so `start`, `value` and an author's own reset all
                // reach the marker; layout only reads it.
                ++list_index;
                if (marker_inside)
                    child_options.inside_marker
                        = marker_text(*child_style, child_style->list_item_value);
            }
            Fragment child_fragment = layout_block(child_element, *child_style, child_x, child_y,
                child_width, child_list_depth, floats, child_options);
            if (establishes_bfc(*child_style) && !floats.floats.empty()) {
                // Its border box must not overlap a float anywhere along its
                // height (§9.5): a float starting lower narrows it (an auto
                // width is laid out again in the narrower room) or, when a
                // written width no longer fits, sends it below the next float.
                for (int round = 0; round < 6; ++round) {
                    FloatContext::Band const over = floats.band_over(child_y,
                        child_y + child_fragment.height, content_x, content_x + content_width);
                    float const room = std::max(0.0f, over.right - over.left);
                    float const needed = child_style->width.is_auto()
                        ? 0.0f
                        : child_fragment.width + resolve(child_style->margin_left, content_width)
                            + resolve(child_style->margin_right, content_width);
                    if (needed > room + 0.01f) {
                        std::optional<float> const next = floats.next_bottom(child_y);
                        if (!next || *next <= child_y)
                            break;
                        child_y = *next;
                        FloatContext::Band const band = floats.band_at(child_y, content_x, content_x + content_width);
                        child_x = band.left;
                        child_width = std::max(0.0f, band.right - band.left);
                    } else if (room >= child_width - 0.01f && over.left <= child_x + 0.01f) {
                        break; // laid out in a room no wider than what its whole height allows
                    } else {
                        child_x = over.left;
                        child_width = room;
                    }
                    child_fragment = layout_block(child_element, *child_style, child_x, child_y,
                        child_width, child_list_depth, floats, child_options);
                }
            }
            // Relative: laid out in flow, then shifted once the flow has read
            // its bottom; sticky stays put until scroll containers land.
            // Either paints in the positioned layer.
            auto const hand_over = [&](Fragment& box) {
                mark_positioned(box, *child_style, content_width);
                fragment.children.push_back(std::move(box));
            };

            if (list_item && !marker_inside)
                add_list_marker(child_fragment, *child_style, element, child_style->list_item_value);

            float const effective_bottom = collapse_margins(margin_bottom, child_fragment.collapsed_bottom);
            if (empty) {
                // The box takes no room and its margins meet: they join the
                // running margin — unless the caller holds them already,
                // having applied them through this box's top edge. A box
                // holding a float ended the caller's walk after its top
                // margin: its bottom margin starts the running margin here.
                bool const holds_float = contains_float(child_element);
                if (!held_by_caller)
                    previous_bottom_margin = collapse_margins(
                        collapse_margins(previous_bottom_margin, effective_top), effective_bottom);
                else if (holds_float)
                    previous_bottom_margin = effective_bottom;
                if (holds_float)
                    first_in_flow = false;
                hand_over(child_fragment);
                continue;
            }
            cursor = child_fragment.y + child_fragment.height;
            previous_bottom_margin = effective_bottom;
            first_in_flow = false;
            hand_over(child_fragment);
        }
        };
        walk(children, &element, style);
        if (after)
            place_or_append(*after);
        flush_inline();
        if (collapse_bottom) {
            fragment.collapsed_bottom = previous_bottom_margin;
            return cursor - content_y;
        }
        return cursor - content_y + previous_bottom_margin;
    }

    // A block-level ::before or ::after box: its own edges and width from
    // its style, its text laid out in lines inside, no children of its own.
    // A flex line hands it settled content sizes.
    Fragment layout_generated_block(css::GeneratedBox const& box, dom::Element const& element,
        float x, float y, float containing_width, FloatContext& floats,
        std::optional<float> settled_width = std::nullopt,
        std::optional<float> settled_height = std::nullopt) const
    {
        ComputedStyle const& style = box.style;
        Fragment fragment;
        fragment.element = &element;
        fragment.style = &style;
        float const margin_left = resolve(style.margin_left, containing_width);
        float const margin_right = resolve(style.margin_right, containing_width);
        float const padding_left = resolve(style.padding_left, containing_width);
        float const padding_right = resolve(style.padding_right, containing_width);
        float const padding_top = resolve(style.padding_top, containing_width);
        float const padding_bottom = resolve(style.padding_bottom, containing_width);
        float const horizontal_edges = padding_left + padding_right + style.border_left.width
            + style.border_right.width;
        float border_box_width;
        if (settled_width)
            border_box_width = *settled_width + horizontal_edges;
        else if (style.width.is_auto())
            border_box_width = clamp_width(style,
                                   containing_width - margin_left - margin_right - horizontal_edges,
                                   containing_width, horizontal_edges)
                + horizontal_edges;
        else
            border_box_width = clamp_width(style,
                                   as_content_size(style, resolve(style.width, containing_width), horizontal_edges),
                                   containing_width, horizontal_edges)
                + horizontal_edges;
        if (border_box_width < 0)
            border_box_width = 0;
        fragment.x = x + margin_left;
        fragment.y = y;
        fragment.width = border_box_width;
        float const content_x = fragment.x + style.border_left.width + padding_left;
        float const content_width = std::max(0.0f, border_box_width - horizontal_edges);
        float const content_y = fragment.y + style.border_top.width + padding_top;
        std::vector<InlineItem> items;
        std::u32string const text = decode_utf8(box.text);
        if (!text.empty())
            append_text(text, &style, items, &element);
        float content_height = items.empty()
            ? 0.0f
            : layout_lines(items, style, content_x, content_y, content_width, fragment, floats, 0);
        float const vertical_edges
            = padding_top + padding_bottom + style.border_top.width + style.border_bottom.width;
        if (settled_height)
            content_height = *settled_height;
        else if (!style.height.is_auto() && style.height.kind == LengthPercent::Kind::Px)
            content_height = clamp_height(style, as_content_size(style, style.height.value, vertical_edges),
                std::nullopt, vertical_edges);
        else
            content_height = clamp_height(style, content_height, std::nullopt, vertical_edges);
        fragment.height = content_height + style.border_top.width + style.border_bottom.width
            + padding_top + padding_bottom;
        return fragment;
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
        std::size_t const first = items.size();
        if (is_control(element)) {
            items.push_back(InlineItem { InlineItem::Kind::Control, {}, &style, &element });
        } else if (element.is_html("br")) {
            InlineItem item(InlineItem::Kind::HardBreak, {}, &style, &element);
            item.clear = break_clear(element, style);
            items.push_back(std::move(item));
        } else if (is_replaced(element)) {
            append_image(element, &style, items);
        } else if (is_atomic_inline(style)) {
            items.push_back(InlineItem { InlineItem::Kind::Block, {}, &style, &element });
        } else {
            items.push_back(InlineItem { InlineItem::Kind::BoxStart, {}, &style, &element });
            collect_inline(element, &style, items);
            items.push_back(InlineItem { InlineItem::Kind::BoxEnd, {}, &style, &element });
        }
        mark_aligned(items, first, style);
    }

    // The text of a list item's marker: every counter style spells one the
    // way a counter() is spelled, only the three glyphs stand for
    // themselves, and the numbering systems take the full stop a marker
    // wears after them. Empty for `list-style-type: none`.
    static std::u32string marker_text(ComputedStyle const& style, int index)
    {
        std::string written = css::format_counter(index, style.list_style_type);
        if (written.empty())
            return {};
        bool const is_glyph = style.list_style_type == css::ListStyleType::Disc
            || style.list_style_type == css::ListStyleType::Circle
            || style.list_style_type == css::ListStyleType::Square;
        if (!is_glyph)
            written += ".";
        return decode_utf8(written);
    }

    void add_list_marker(Fragment& item_fragment, ComputedStyle const& style,
        dom::Element const& list_parent, int index) const
    {
        (void)list_parent;
        std::u32string marker = marker_text(style, index);
        if (marker.empty())
            return;
        float const width = measure(style, marker);
        float const gap = style.font_size * 0.4f;
        float const baseline = item_fragment.y + style.border_top.width
            + resolve(style.padding_top, 0) + ascent_in_line(style);
        // The marker hangs off the start edge, which is the right one in a
        // right-to-left item (css-lists-3 §3.2).
        float const x = style.direction == css::Direction::Rtl
            ? item_fragment.x + item_fragment.width + gap
            : item_fragment.x - width - gap;
        item_fragment.runs.insert(item_fragment.runs.begin(),
            TextRun { x, baseline, std::move(marker), &style, item_fragment.element, &fonts_for(style),
                width });
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
            case InlineItem::Kind::BoxStart:
            case InlineItem::Kind::BoxEnd: {
                // The edge sticks to what it opens or closes, so it widens
                // the narrowest line as well as the widest. A percentage
                // margin or padding measures nothing here, as everywhere an
                // intrinsic width is asked for.
                float const edge
                    = inline_edge(*item.style, item.kind == InlineItem::Kind::BoxStart, 0);
                if (edge != 0) {
                    result.min = std::max(result.min, edge);
                    add(edge);
                }
                break;
            }
            case InlineItem::Kind::Absolute:
                break; // takes no room
            case InlineItem::Kind::HardBreak:
            case InlineItem::Kind::SoftBreak:
                result.max = std::max(result.max, line);
                line = 0;
                pending_space = 0;
                break;
            case InlineItem::Kind::Image: {
                std::optional<ReplacedSize> const size = replaced_size(*item.element, *item.style,
                    item.image.get(), item.image_density, 0);
                InlineEdges const edges = inline_edges(*item.style, 0);
                float const width = size
                    ? edges.margin_left + edges.left + size->width + edges.right + edges.margin_right
                    : 0;
                result.min = std::max(result.min, width);
                add(width);
                break;
            }
            case InlineItem::Kind::Float:
            case InlineItem::Kind::Block: {
                Intrinsic const box = block_intrinsic(*item.element, *item.style);
                result.min = std::max(result.min, box.min);
                add(box.max);
                break;
            }
            case InlineItem::Kind::Table: {
                Intrinsic const box = table_intrinsic_widths(item.nodes, *item.style, item.element);
                result.min = std::max(result.min, box.min);
                add(box.max);
                break;
            }
            case InlineItem::Kind::Control: {
                InlineEdges const edges = inline_edges(*item.style, 0);
                float const width = edges.margin_left + control_spec(*item.element, *item.style, 0).size.width
                    + edges.margin_right;
                result.min = std::max(result.min, width);
                add(width);
                break;
            }
            }
        }
        result.max = std::max(result.max, line);
        return result;
    }

    // The intrinsic widths of an element's contents, inside its edges, held
    // by its min-width and max-width when those are lengths (a percentage
    // has no base in intrinsic sizing).
    // The min-width and max-width bounds when they are written as content
    // keywords. `clamp_width` cannot read those — it has no measure of the
    // content — so the callers that do hold the box's intrinsics apply them
    // afterwards, here. The keywords name a content size, so box-sizing
    // does not come into it.
    float clamp_content_bounds(ComputedStyle const& style, float width, Intrinsic const& intrinsic,
        float available) const
    {
        if (style.max_width.is_content_size())
            width = std::min(width,
                content_size_of(style.max_width, intrinsic.min, intrinsic.max, available));
        if (style.min_width.is_content_size())
            width = std::max(width,
                content_size_of(style.min_width, intrinsic.min, intrinsic.max, available));
        return std::max(0.0f, width);
    }

    // Whether either width bound is written as a content keyword, which is
    // what makes measuring the content worth its cost on a box whose own
    // width did not already ask for it.
    static bool has_content_bound(ComputedStyle const& style)
    {
        return style.min_width.is_content_size() || style.max_width.is_content_size();
    }

    Intrinsic intrinsic_widths(dom::Element const& element, ComputedStyle const& style) const
    {
        Intrinsic result;
        if (css::GeneratedBox const* const box = generated_box_of(element, style)) {
            std::vector<InlineItem> items;
            std::u32string const text = decode_utf8(box->text);
            if (!text.empty())
                append_text(text, &style, items, &element);
            result = inline_intrinsic(items);
        } else {
            result = content_intrinsic_widths(element, style);
        }
        // These are content widths, so a border-box bound gives up its edges
        // (a percentage padding has no base here either, and reads as zero).
        float const edges = horizontal_edges_of(style, 0);
        if (style.max_width.kind == LengthPercent::Kind::Px) {
            float const bound = as_content_size(style, style.max_width.value, edges);
            result.max = std::min(result.max, bound);
            result.min = std::min(result.min, bound);
        }
        if (style.min_width.kind == LengthPercent::Kind::Px) {
            float const bound = as_content_size(style, style.min_width.value, edges);
            result.min = std::max(result.min, bound);
            result.max = std::max(result.max, bound);
        }
        return result;
    }

    Intrinsic content_intrinsic_widths(dom::Element const& element, ComputedStyle const& style) const
    {
        if (is_replaced(element)) {
            PageImage const image = image_for(element);
            std::optional<ReplacedSize> const size
                = replaced_size(element, style, image.bitmap.get(), image.density, 0);
            float const width = size ? size->width : 0;
            return { width, width };
        }
        if (is_control(element)) {
            float const width = control_spec(element, style, 0).size.width;
            return { width, width };
        }
        if (is_flex_container(style))
            return flex_intrinsic_widths(element, style);
        if (is_grid_container(style))
            return grid_intrinsic_widths(element, style);
        if (is_table_display(style.display)) {
            // The table's border box, less the edges block_intrinsic adds.
            std::vector<dom::Node const*> const children(element.children().begin(), element.children().end());
            Intrinsic const box = table_intrinsic_widths(children, style, &element);
            float const edges = resolve(style.padding_left, 0) + resolve(style.padding_right, 0)
                + style.border_left.width + style.border_right.width;
            return { std::max(0.0f, box.min - edges), std::max(0.0f, box.max - edges) };
        }
        std::vector<dom::Node const*> const children(element.children().begin(), element.children().end());
        return nodes_intrinsic_widths(children, style, &element, false);
    }

    // The intrinsic widths of a run of nodes laid out as a block container
    // — an element's children, or what an anonymous box holds — under
    // `style` (the container's; `owner` is it, when it is an element).
    Intrinsic nodes_intrinsic_widths(std::vector<dom::Node const*> const& children, ComputedStyle const& style,
        dom::Element const* owner, bool anonymous) const
    {
        std::vector<InlineItem> pending;
        if (!contains_block_in(children)) {
            collect_inline_nodes(children, owner, &style, pending, anonymous);
            apply_first_letter(pending, style);
            return inline_intrinsic(pending);
        }
        Intrinsic result;
        bool first_letter_owed = style.first_letter != nullptr;
        auto const flush = [&] {
            if (first_letter_owed && apply_first_letter(pending, style))
                first_letter_owed = false;
            Intrinsic const run = inline_intrinsic(pending);
            result.min = std::max(result.min, run.min);
            result.max = std::max(result.max, run.max);
            pending.clear();
        };
        // The same walk as layout: inline content gathers, a block at any
        // depth through inline boxes counts on its own, and a run of table
        // parts counts as the anonymous table around it.
        std::function<void(std::vector<dom::Node const*> const&, dom::Element const*, ComputedStyle const&)> walk;
        walk = [&](std::vector<dom::Node const*> const& nodes, dom::Element const* parent_element,
                   ComputedStyle const& parent_style) {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                dom::Node const* child = nodes[i];
                if (child->is_text()) {
                    std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                    if (!text.empty())
                        append_text(text, &parent_style, pending, parent_element);
                    continue;
                }
                if (!child->is_element())
                    continue;
                auto const& child_element = static_cast<dom::Element const&>(*child);
                ComputedStyle const* child_style = style_of(child_element);
                if (!child_style || child_style->display == Display::None)
                    continue;
                if (child_style->out_of_flow())
                    continue; // takes no room in the flow
                if (is_table_internal(child_style->display)) {
                    std::vector<dom::Node const*> const run = table_run(nodes, i);
                    flush();
                    ComputedStyle const& anonymous_table = anonymous_style(parent_style, Display::Table);
                    Intrinsic const box = table_intrinsic_widths(run, anonymous_table, parent_element);
                    result.min = std::max(result.min, box.min);
                    result.max = std::max(result.max, box.max);
                    continue;
                }
                if (is_floating(*child_style) || !is_block_level(*child_style)) {
                    if (splits_around_blocks(child_element, *child_style)) {
                        std::vector<dom::Node const*> const grandchildren(child_element.children().begin(),
                            child_element.children().end());
                        walk(grandchildren, &child_element, *child_style);
                    } else {
                        collect_inline_element(child_element, *child_style, pending);
                    }
                    continue;
                }
                flush();
                Intrinsic const box = block_intrinsic(child_element, *child_style);
                result.min = std::max(result.min, box.min);
                result.max = std::max(result.max, box.max);
            }
        };
        walk(children, owner, style);
        flush();
        return result;
    }

    // A run of table parts starting at `from`: consecutive siblings that
    // are table-internal boxes, with the blank text between them; `from`
    // moves to the run's last node.
    std::vector<dom::Node const*> table_run(std::vector<dom::Node const*> const& nodes, std::size_t& from) const
    {
        std::vector<dom::Node const*> run;
        std::size_t last = from;
        for (std::size_t j = from; j < nodes.size(); ++j) {
            dom::Node const* node = nodes[j];
            if (node->is_text()) {
                bool blank = true;
                for (char const c : static_cast<dom::Text const*>(node)->data)
                    blank = blank && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f');
                if (!blank)
                    break;
                run.push_back(node);
                continue;
            }
            if (!node->is_element())
                continue;
            ComputedStyle const* style = style_of(static_cast<dom::Element const&>(*node));
            if (!style || style->display == Display::None) {
                run.push_back(node);
                continue;
            }
            if (!is_table_internal(style->display))
                break;
            run.push_back(node);
            last = j;
        }
        while (!run.empty() && run.back()->is_text())
            run.pop_back();
        from = last;
        return run;
    }

    // A block's intrinsic widths seen from outside: its width when written
    // in px, else its contents', plus its horizontal edges (percentages
    // count as zero here).
    Intrinsic block_intrinsic(dom::Element const& element, ComputedStyle const& style) const
    {
        float const margins = resolve(style.margin_left, 0) + resolve(style.margin_right, 0);
        float const inner_edges = resolve(style.padding_left, 0) + resolve(style.padding_right, 0)
            + style.border_left.width + style.border_right.width;
        float const edges = margins + inner_edges;
        if (!style.width.is_auto() && style.width.kind == LengthPercent::Kind::Px) {
            // Under border-box the written width already holds the padding and
            // the borders; only the margins go outside it.
            float const border_box = sizes_border_box(style)
                ? std::max(style.width.value, inner_edges)
                : style.width.value + inner_edges;
            return { border_box + margins, border_box + margins };
        }
        Intrinsic const inner = intrinsic_widths(element, style);
        // A box told to be its own narrowest or widest contributes that one
        // size both ways: it will not be any other width whatever room its
        // parent has. fit-content contributes the pair unchanged.
        if (style.width.kind == LengthPercent::Kind::MinContent)
            return { inner.min + edges, inner.min + edges };
        if (style.width.kind == LengthPercent::Kind::MaxContent)
            return { inner.max + edges, inner.max + edges };
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
            // auto here IS fit-content, so a written keyword only changes
            // the answer when it asks for one of the two extremes.
            float const preferred = style.width.is_content_size()
                ? content_size_of(style.width, intrinsic.min, intrinsic.max, available)
                : std::min(std::max(intrinsic.min, available), intrinsic.max);
            result.shrink = clamp_content_bounds(style,
                clamp_width(style, preferred, containing_width, edges), intrinsic, available);
            result.border_box = *result.shrink + edges;
        } else {
            result.border_box = clamp_width(style,
                                    as_content_size(style, resolve(style.width, containing_width), edges),
                                    containing_width, edges)
                + edges;
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
        BlockOptions options;
        options.content_width = width.shrink;
        options.zero_auto_margins = true;
        Fragment box = layout_block(element, style, outer_left, y + margin_top, containing_width,
            list_depth, floats, options);
        floats.floats.push_back(FloatBox { outer_left, outer_left + width.outer(), y,
            box.y + box.height + margin_bottom, is_left });
        box.floating = true;
        mark_positioned(box, style, containing_width); // a relative float paints in the positioned layer
        parent.children.push_back(std::move(box));
    }

    // --- Flexbox ----------------------------------------------------------------

    struct FlexItem {
        dom::Element const* element = nullptr; // null: an anonymous item of the container's own text
        ComputedStyle const* style = nullptr; // the element's; the container's for an anonymous item
        std::vector<InlineItem> inline_items; // an anonymous or generated item's content
        // A ::before or ::after box of the container (element is then the
        // container, style the box's own): its edges and sizes are its own,
        // its content the text.
        css::GeneratedBox const* generated = nullptr;
        float margin_start = 0; // along the main axis
        float margin_end = 0;
        float margin_cross_start = 0;
        float margin_cross_end = 0;
        float edges_main = 0; // padding and border along the main axis
        float edges_cross = 0;
        float grow = 0;
        float shrink = 1;
        float base = 0; // the flex base size: the content main size to start from
        float minimum = 0; // the minimum main size: min-width/height, else the content's narrowest
        std::optional<float> maximum; // the maximum main size, when max-width/height says
        float hypothetical = 0; // the base size, clamped
        float main = 0; // the content main size, once resolved
        float cross = 0; // the content cross size, once known
        bool cross_is_auto = true;
        bool frozen = false;
        int order = 0;

        float outer_main() const { return margin_start + edges_main + main + margin_end; }
        float outer_cross() const { return margin_cross_start + edges_cross + cross + margin_cross_end; }
    };

    // How an item sits across its line: its own align-self, else the
    // container's align-items.
    static css::AlignItems item_alignment(FlexItem const& item, ComputedStyle const& container)
    {
        css::AlignItems const alignment = !item.element || item.style->align_self == css::AlignItems::Auto
            ? container.align_items
            : item.style->align_self;
        // normal is stretch on a flex line.
        return alignment == css::AlignItems::Normal ? css::AlignItems::Stretch : alignment;
    }

    // A flex or grid item's content height when laid out `width` wide,
    // from a scratch layout: an anonymous item (no element) is its lines, a
    // generated box (element = the container, style = the box's) its text.
    float measure_item_height(dom::Element const* element, ComputedStyle const& s,
        std::vector<InlineItem> const& inline_items, css::GeneratedBox const* generated, float width,
        float containing_width, int list_depth) const
    {
        FloatContext scratch_floats;
        if (!element) {
            Fragment scratch;
            return layout_lines(inline_items, s, 0, 0, width, scratch, scratch_floats, list_depth);
        }
        if (generated) {
            Fragment const box = layout_generated_block(*generated, *element, 0, 0, containing_width,
                scratch_floats, width);
            float const vertical_edges = resolve(s.padding_top, containing_width)
                + resolve(s.padding_bottom, containing_width) + s.border_top.width + s.border_bottom.width;
            return std::max(0.0f, box.height - vertical_edges);
        }
        auto const key = std::make_tuple(element, width, containing_width, list_depth);
        if (auto const it = measured_heights.find(key); it != measured_heights.end())
            return it->second;
        BlockOptions options;
        options.content_width = width;
        options.zero_auto_margins = true;
        options.own_context = true;
        Fragment const box = layout_block(*element, s, 0, 0, containing_width, list_depth, scratch_floats,
            options);
        float const vertical_edges = resolve(s.padding_top, containing_width)
            + resolve(s.padding_bottom, containing_width) + s.border_top.width + s.border_bottom.width;
        float const height = std::max(0.0f, box.height - vertical_edges);
        measured_heights.emplace(key, height);
        return height;
    }

    float measure_item_height(FlexItem const& item, float width, float containing_width,
        int list_depth) const
    {
        return measure_item_height(item.element, *item.style, item.inline_items, item.generated, width,
            containing_width, list_depth);
    }

    // A flex or grid item with a z-index paints in that order among its
    // container's positioned boxes, positioned itself or not.
    static void mark_item_layer(Fragment& box, ComputedStyle const& style)
    {
        if (style.positioned() || !style.z_index)
            return;
        box.positioned = true;
        box.z_index = *style.z_index;
        box.stacking_context = true;
    }

    // Lays an item out at its settled sizes, its margin box at (x, y).
    void place_flex_item(FlexItem const& item, bool horizontal, float x, float y,
        float containing_width, int list_depth, Fragment& parent) const
    {
        float const content_w = horizontal ? item.main : item.cross;
        float const content_h = horizontal ? item.cross : item.main;
        FloatContext scratch_floats;
        if (!item.element) {
            Fragment box;
            box.x = x;
            box.y = y;
            box.width = content_w;
            box.height = content_h;
            (void)layout_lines(item.inline_items, *item.style, x, y, content_w, box, scratch_floats,
                list_depth);
            parent.children.push_back(std::move(box));
            return;
        }
        float const margin_top = horizontal ? item.margin_cross_start : item.margin_start;
        if (item.generated) {
            parent.children.push_back(layout_generated_block(*item.generated, *item.element, x,
                y + margin_top, containing_width, scratch_floats, content_w, content_h));
            return;
        }
        BlockOptions options;
        options.content_width = content_w;
        options.content_height = content_h;
        options.zero_auto_margins = true;
        options.own_context = true;
        Fragment box = layout_block(*item.element, *item.style, x, y + margin_top,
            containing_width, list_depth, scratch_floats, options);
        mark_positioned(box, *item.style, containing_width);
        mark_item_layer(box, *item.style);
        parent.children.push_back(std::move(box));
    }

    // A flex container's intrinsic widths: a row adds its items up, a
    // column takes the widest; a wrapping row can break between items.
    Intrinsic flex_intrinsic_widths(dom::Element const& element, ComputedStyle const& style) const
    {
        bool const row = style.flex_direction == css::FlexDirection::Row
            || style.flex_direction == css::FlexDirection::RowReverse;
        bool const wrap = style.flex_wrap != css::FlexWrap::NoWrap;
        Intrinsic result;
        std::size_t count = 0;
        auto const take = [&](Intrinsic const& box) {
            if (row) {
                float const gap = count > 0 ? resolve(style.column_gap, 0) : 0.0f;
                result.max += gap + box.max;
                if (wrap)
                    result.min = std::max(result.min, box.min);
                else
                    result.min += gap + box.min;
            } else {
                result.max = std::max(result.max, box.max);
                result.min = std::max(result.min, box.min);
            }
            ++count;
        };
        std::vector<InlineItem> pending;
        auto const flush = [&] {
            for (InlineItem const& item : pending) {
                if (is_inline_content(item)) {
                    take(inline_intrinsic(pending));
                    break;
                }
            }
            pending.clear();
        };
        // The generated boxes count as text at either end (their own edges aside).
        if (css::GeneratedBox const* const before = before_of(&style))
            append_generated(*before, element, pending);
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
            flush();
            take(block_intrinsic(child_element, *child_style));
        }
        if (css::GeneratedBox const* const after = after_of(&style))
            append_generated(*after, element, pending);
        flush();
        return result;
    }

    // --- Tables -------------------------------------------------------------------

    // What the width algorithm needs from a table: its structure, the
    // gutters and edges, and the cells' measures. Shared by the layout and
    // the intrinsic widths.
    struct TableSetup {
        table::Structure structure;
        table::WidthInput input;
        float spacing_h = 0;
        float spacing_v = 0;
        float edges_left = 0; // the table box's border and padding, each side
        float edges_right = 0;
        float edges_top = 0;
        float edges_bottom = 0;
        // The collapsing border model: what won at every segment of every
        // grid line, and the half of it each cell takes at each of its
        // four edges. Both are empty in the separated model.
        bool collapse = false;
        table::CollapsedBorders collapsed;
        struct CellEdges {
            float left = 0;
            float top = 0;
            float right = 0;
            float bottom = 0;
        };
        std::vector<CellEdges> cell_edges;
        // The table box's own style, its outer borders halved, once the
        // boxes have been handed what they draw.
        ComputedStyle const* box_style = nullptr;
    };

    static float cell_horizontal_edges(ComputedStyle const& s, float base)
    {
        return resolve(s.padding_left, base) + resolve(s.padding_right, base) + s.border_left.width
            + s.border_right.width;
    }

    static float cell_vertical_edges(ComputedStyle const& s, float base)
    {
        return resolve(s.padding_top, base) + resolve(s.padding_bottom, base) + s.border_top.width
            + s.border_bottom.width;
    }

    // The collapsing model: every box's borders at a grid line collapse
    // into one (§17.6.2.1 picks it), the line runs down the middle of the
    // winner, and each box on either side keeps half. A cell that spans
    // takes the widest segment along each of its edges. The table's own
    // edge is the outer half of the border on its outermost line, and it
    // has no padding.
    void collapse_table_borders(TableSetup& setup, ComputedStyle const& style) const
    {
        table::Structure const& structure = setup.structure;
        setup.collapsed = table::collapse_borders(structure, style);
        table::CollapsedBorders const& borders = setup.collapsed;
        int const rows = borders.rows;
        int const columns = borders.columns;
        setup.cell_edges.assign(structure.cells.size(), TableSetup::CellEdges {});
        for (std::size_t i = 0; i < structure.cells.size(); ++i) {
            table::Cell const& cell = structure.cells[i];
            if (cell.anonymous())
                continue;
            int const first_row = cell.row;
            int const last_row = std::min(cell.row + cell.row_span, rows);
            int const first_column = cell.column;
            int const last_column = std::min(cell.column + cell.column_span, columns);
            TableSetup::CellEdges& edges = setup.cell_edges[i];
            edges.top = borders.widest_horizontal(first_row, first_column, last_column) / 2;
            edges.bottom = borders.widest_horizontal(last_row, first_column, last_column) / 2;
            edges.left = borders.widest_vertical(first_column, first_row, last_row) / 2;
            edges.right = borders.widest_vertical(last_column, first_row, last_row) / 2;
        }
        setup.edges_left = borders.widest_vertical(0, 0, rows) / 2;
        setup.edges_right = borders.widest_vertical(columns, 0, rows) / 2;
        setup.edges_top = borders.widest_horizontal(0, 0, columns) / 2;
        setup.edges_bottom = borders.widest_horizontal(rows, 0, columns) / 2;
    }

    static float collapsed_horizontal_edges(TableSetup const& setup, table::Cell const& cell,
        ComputedStyle const& s)
    {
        auto const at = static_cast<std::size_t>(&cell - setup.structure.cells.data());
        TableSetup::CellEdges const& edges = setup.cell_edges[at];
        return resolve(s.padding_left, 0) + resolve(s.padding_right, 0) + edges.left + edges.right;
    }

    // Hands every box of a collapsed table the border it actually draws:
    // each cell half of the winner at each of its edges, the table box
    // the outer half of its outermost lines, and the rows, groups and
    // columns nothing at all — what was theirs the cells now draw.
    void rewrite_collapsed_styles(TableSetup& setup, ComputedStyle const& style) const
    {
        table::Structure& structure = setup.structure;
        table::CollapsedBorders const& borders = setup.collapsed;
        int const rows = borders.rows;
        int const columns = borders.columns;
        auto const set = [](css::BorderSide& side, float width, Color color) {
            side.width = width;
            side.color = color;
            side.current_color = false;
            side.style = width > 0 ? css::BorderStyle::Solid : css::BorderStyle::None;
        };
        for (std::size_t i = 0; i < structure.cells.size(); ++i) {
            table::Cell& cell = structure.cells[i];
            if (cell.anonymous())
                continue;
            int const first_row = cell.row;
            int const last_row = std::min(cell.row + cell.row_span, rows);
            int const first_column = cell.column;
            int const last_column = std::min(cell.column + cell.column_span, columns);
            TableSetup::CellEdges const& edges = setup.cell_edges[i];
            std::shared_ptr<ComputedStyle> const copy = owned_copy(*cell.style);
            set(copy->border_top, edges.top, borders.color_horizontal(first_row, first_column, last_column));
            set(copy->border_bottom, edges.bottom, borders.color_horizontal(last_row, first_column, last_column));
            set(copy->border_left, edges.left, borders.color_vertical(first_column, first_row, last_row));
            set(copy->border_right, edges.right, borders.color_vertical(last_column, first_row, last_row));
            cell.style = copy.get();
        }
        auto const strip = [&](ComputedStyle const*& target, dom::Element const* element) {
            if (!element || !target)
                return;
            std::shared_ptr<ComputedStyle> const copy = owned_copy(*target);
            for (css::BorderSide* side : { &copy->border_top, &copy->border_right, &copy->border_bottom,
                     &copy->border_left })
                set(*side, 0, side->color);
            target = copy.get();
        };
        for (table::Row& row : structure.rows)
            strip(row.style, row.element);
        for (table::RowGroup& group : structure.groups)
            strip(group.style, group.element);
        for (table::Column& column : structure.columns)
            strip(column.style, column.element);
        for (table::ColumnGroup& group : structure.column_groups)
            strip(group.style, group.element);
        std::shared_ptr<ComputedStyle> const box = owned_copy(style);
        set(box->border_top, setup.edges_top, borders.color_horizontal(0, 0, columns));
        set(box->border_bottom, setup.edges_bottom, borders.color_horizontal(rows, 0, columns));
        set(box->border_left, setup.edges_left, borders.color_vertical(0, 0, rows));
        set(box->border_right, setup.edges_right, borders.color_vertical(columns, 0, rows));
        setup.box_style = box.get();
    }

    TableSetup table_setup(std::vector<dom::Node const*> const& children, ComputedStyle const& style,
        dom::Element const* owner, float containing_width) const
    {
        TableSetup setup;
        setup.structure = table::build_structure(children, style,
            [this](dom::Element const& element) { return style_of(element); });
        // In the collapsing model the gutters are gone and the table has no padding.
        bool const collapse = style.border_collapse == css::BorderCollapse::Collapse;
        setup.collapse = collapse;
        setup.spacing_h = collapse ? 0.0f : resolve(style.border_spacing_horizontal, 0);
        setup.spacing_v = collapse ? 0.0f : resolve(style.border_spacing_vertical, 0);
        if (collapse) {
            // Every box's borders at a grid line collapse into one, and
            // each box takes half of the winner at each edge it touches:
            // the two halves meet on the line and make the border whole
            // (§17.6.2). The table's own edge is the outer half.
            collapse_table_borders(setup, style);
        } else {
            setup.edges_left = style.border_left.width + resolve(style.padding_left, containing_width);
            setup.edges_right = style.border_right.width + resolve(style.padding_right, containing_width);
            setup.edges_top = style.border_top.width + resolve(style.padding_top, containing_width);
            setup.edges_bottom = style.border_bottom.width + resolve(style.padding_bottom, containing_width);
        }
        table::Structure const& structure = setup.structure;
        auto const n = static_cast<std::size_t>(structure.column_count);
        setup.input.columns.resize(n);
        setup.input.spacing = setup.spacing_h;
        setup.input.edges = setup.edges_left + setup.edges_right;
        for (table::Cell const& cell : structure.cells) {
            Intrinsic inner;
            float edges = 0;
            std::optional<float> fixed;
            std::optional<float> percent;
            if (cell.anonymous()) {
                inner = nodes_intrinsic_widths(cell.nodes, *cell.style, owner, true);
            } else {
                inner = intrinsic_widths(*cell.element, *cell.style);
                edges = collapse ? collapsed_horizontal_edges(setup, cell, *cell.style)
                                 : cell_horizontal_edges(*cell.style, 0);
                LengthPercent const& width = cell.style->width;
                if (width.kind == LengthPercent::Kind::Px)
                    fixed = width.value + edges;
                else if (width.kind == LengthPercent::Kind::Percent)
                    percent = width.value;
            }
            float const min = inner.min + edges;
            float const max = std::max(inner.max + edges, min);
            if (cell.column_span <= 1) {
                table::ColumnInput& column = setup.input.columns[static_cast<std::size_t>(cell.column)];
                column.min = std::max(column.min, min);
                column.max = std::max(column.max, max);
                if (fixed)
                    column.fixed = std::max(column.fixed.value_or(0.0f), *fixed);
                if (percent)
                    column.percent = std::max(column.percent.value_or(0.0f), *percent);
            } else {
                setup.input.spans.push_back({ cell.column, cell.column_span, min, max, fixed, percent });
            }
        }
        for (std::size_t c = 0; c < n; ++c) {
            table::Column const& column = structure.columns[c];
            if (!column.style)
                continue;
            LengthPercent const& width = column.style->width;
            if (width.kind == LengthPercent::Kind::Px)
                setup.input.columns[c].fixed = std::max(setup.input.columns[c].fixed.value_or(0.0f), width.value);
            else if (width.kind == LengthPercent::Kind::Percent)
                setup.input.columns[c].percent
                    = std::max(setup.input.columns[c].percent.value_or(0.0f), width.value);
        }
        setup.input.fixed_layout = style.table_layout == css::TableLayout::Fixed;
        return setup;
    }

    // A table's intrinsic widths: its border box at its narrowest and at
    // its widest — or as written, when its width is a length.
    Intrinsic table_intrinsic_widths(std::vector<dom::Node const*> const& children, ComputedStyle const& style,
        dom::Element const* owner) const
    {
        TableSetup setup = table_setup(children, style, owner, 0);
        if (style.width.kind == LengthPercent::Kind::Px) {
            bool const border_box = owner && owner->is_html("table") && is_table_display(style.display);
            setup.input.width = style.width.value + (border_box ? 0.0f : setup.input.edges);
        }
        table::WidthResult const widths = table::compute_widths(setup.input);
        if (setup.input.width)
            return { widths.width, widths.width };
        return { widths.min, widths.max };
    }

    // The table (CSS 2.1 §17): the wrapper box holds the captions and the
    // table box; the table box holds the row groups, rows and cells, with
    // the column and row backgrounds under the cells. `element` is the
    // table element (null for an anonymous table around `children`);
    // `owner` is the element the boxes answer to.
    Fragment layout_table(dom::Element const* element, dom::Element const* owner,
        std::vector<dom::Node const*> const& children, ComputedStyle const& style, float x, float y,
        float containing_width, int list_depth, FloatContext& floats, BlockOptions const& options) const
    {
        (void)floats;
        using css::VerticalAlign;
        TableSetup setup = table_setup(children, style, owner, containing_width);
        if (setup.collapse)
            rewrite_collapsed_styles(setup, style);
        table::Structure const& structure = setup.structure;
        int const n = structure.column_count;
        auto const m = static_cast<int>(structure.rows.size());
        auto const at = [](int i) { return static_cast<std::size_t>(i); };
        float const hs = setup.spacing_h;
        float const vs = setup.spacing_v;

        // The width: settled by a formatting context, written, else
        // shrink-to-fit in the room the margins leave.
        float const margin_left = resolve(style.margin_left, containing_width);
        float const margin_right = resolve(style.margin_right, containing_width);
        // An HTML table's width and height are its border box; another
        // element's with display: table are its content box — unless it
        // writes box-sizing: border-box, which says the same thing.
        bool const border_box = (element && element->is_html("table")) || sizes_border_box(style);
        if (options.content_width)
            setup.input.width = *options.content_width + setup.input.edges;
        else if (!style.width.is_auto())
            // The width and its bounds name the same box here, so nothing
            // comes off the bounds: zero edges, whichever box that is.
            setup.input.width = clamp_width(style, resolve(style.width, containing_width), containing_width, 0.0f)
                + (border_box ? 0.0f : setup.input.edges);
        setup.input.available = std::max(0.0f, containing_width - margin_left - margin_right);
        table::WidthResult const widths = table::compute_widths(setup.input);
        float const table_width = widths.width;

        // Auto margins center the table, whatever its width.
        float wrapper_x = x + margin_left;
        if (!options.zero_auto_margins && !options.content_width) {
            bool const auto_left = style.margin_left.is_auto();
            bool const auto_right = style.margin_right.is_auto();
            float const free = containing_width - table_width - margin_left - margin_right;
            if (auto_left && auto_right)
                wrapper_x = x + std::max(0.0f, free / 2);
            else if (auto_left)
                wrapper_x = x + std::max(0.0f, free);
        }

        Fragment wrapper;
        wrapper.element = owner;
        wrapper.x = wrapper_x;
        wrapper.y = y;
        wrapper.width = table_width;
        float cursor = y;
        auto const lay_caption = [&](table::Caption const& caption) {
            FloatContext scratch;
            float const top = resolve(caption.style->margin_top, table_width);
            float const bottom = resolve(caption.style->margin_bottom, table_width);
            Fragment box = layout_block(*caption.element, *caption.style, wrapper_x, cursor + top, table_width,
                list_depth, scratch, BlockOptions {});
            cursor = box.y + box.height + bottom;
            mark_positioned(box, *caption.style, table_width);
            wrapper.children.push_back(std::move(box));
        };
        for (table::Caption const& caption : structure.captions) {
            if (caption.style->caption_side == css::CaptionSide::Top)
                lay_caption(caption);
        }

        Fragment box;
        box.element = element ? element : owner;
        box.style = setup.box_style ? setup.box_style : &style;
        box.x = wrapper_x;
        box.y = cursor;
        box.width = table_width;
        float const grid_x = wrapper_x + setup.edges_left;
        float const grid_y = cursor + setup.edges_top;
        std::vector<float> column_x(at(n) + 1, grid_x + hs);
        {
            float cx = grid_x + hs;
            for (int c = 0; c < n; ++c) {
                column_x[at(c)] = cx;
                cx += widths.columns[at(c)] + hs;
            }
            column_x[at(n)] = cx; // past the last gutter
        }

        // The cells, laid out at the origin at their widths.
        struct CellBox {
            Fragment fragment;
            float height = 0; // as laid out
            float min_height = 0; // a written height
            float baseline = 0; // the first baseline's distance from the top
            float width = 0;
            VerticalAlign::Kind align = VerticalAlign::Kind::Baseline;
        };
        std::vector<CellBox> boxes(structure.cells.size());
        for (std::size_t i = 0; i < structure.cells.size(); ++i) {
            table::Cell const& cell = structure.cells[i];
            CellBox& out = boxes[i];
            float width = hs * static_cast<float>(cell.column_span - 1);
            for (int c = cell.column; c < cell.column + cell.column_span && c < n; ++c)
                width += widths.columns[at(c)];
            out.width = width;
            float const cell_x = n > 0 ? column_x[at(std::min(cell.column, n - 1))] : grid_x;
            FloatContext scratch;
            if (cell.anonymous()) {
                Fragment anon;
                table::Row const& row = structure.rows[at(cell.row)];
                dom::Element const* const holder = row.element ? row.element : owner;
                anon.element = holder;
                anon.x = cell_x;
                anon.width = width;
                if (holder) {
                    float const height = layout_children(*holder, *cell.style, cell_x, 0, width, anon, list_depth,
                        scratch, false, false, std::nullopt, &cell.nodes, true);
                    anon.height = std::max(height, scratch.lowest_bottom().value_or(0.0f));
                }
                out.fragment = std::move(anon);
            } else {
                ComputedStyle const& s = *cell.style;
                float const edges = cell_horizontal_edges(s, width);
                BlockOptions cell_options;
                cell_options.content_width = std::max(0.0f, width - edges);
                cell_options.own_context = true;
                cell_options.zero_auto_margins = true;
                cell_options.height_is_minimum = true;
                // A cell has no margins: its box starts at its column.
                out.fragment = layout_block(*cell.element, s, cell_x - resolve(s.margin_left, width), 0, width,
                    list_depth, scratch, cell_options);
                // §17.5.3: a written height (and minimum) raises the row, and
                // the content is aligned in what the row settles on.
                float const cell_edges = cell_vertical_edges(s, width);
                if (s.height.kind == LengthPercent::Kind::Px)
                    out.min_height = s.height.value + cell_edges;
                if (s.min_height.kind == LengthPercent::Kind::Px)
                    out.min_height = std::max(out.min_height, s.min_height.value + cell_edges);
            }
            out.height = out.fragment.height;
            VerticalAlign::Kind const kind = cell.style->vertical_align.kind;
            out.align = kind == VerticalAlign::Kind::Top || kind == VerticalAlign::Kind::Middle
                    || kind == VerticalAlign::Kind::Bottom
                ? kind
                : VerticalAlign::Kind::Baseline;
            // The cell's baseline: its first line's, else the bottom of its content.
            if (out.fragment.first_baseline) {
                out.baseline = *out.fragment.first_baseline - out.fragment.y;
            } else {
                float const bottom_edges = cell.anonymous()
                    ? 0.0f
                    : resolve(cell.style->padding_bottom, width) + cell.style->border_bottom.width;
                out.baseline = std::max(0.0f, out.height - bottom_edges);
            }
        }

        // Row heights: the tallest cell of one row, the baseline-aligned
        // ones lined up first; a spanning cell that needs more gets it from
        // the last row it spans; a written row height is a floor.
        std::vector<float> row_height(at(m), 0.0f);
        std::vector<float> row_baseline(at(m), 0.0f);
        for (int r = 0; r < m; ++r) {
            table::Row const& row = structure.rows[at(r)];
            if (row.element && row.style && row.style->height.kind == LengthPercent::Kind::Px)
                row_height[at(r)] = row.style->height.value;
        }
        for (std::size_t i = 0; i < structure.cells.size(); ++i) {
            table::Cell const& cell = structure.cells[i];
            if (cell.row_span == 1 && boxes[i].align == VerticalAlign::Kind::Baseline)
                row_baseline[at(cell.row)] = std::max(row_baseline[at(cell.row)], boxes[i].baseline);
        }
        for (std::size_t i = 0; i < structure.cells.size(); ++i) {
            table::Cell const& cell = structure.cells[i];
            if (cell.row_span != 1)
                continue;
            float needed = std::max(boxes[i].height, boxes[i].min_height);
            if (boxes[i].align == VerticalAlign::Kind::Baseline)
                needed = std::max(needed, boxes[i].height + (row_baseline[at(cell.row)] - boxes[i].baseline));
            row_height[at(cell.row)] = std::max(row_height[at(cell.row)], needed);
        }
        for (std::size_t i = 0; i < structure.cells.size(); ++i) {
            table::Cell const& cell = structure.cells[i];
            if (cell.row_span <= 1)
                continue;
            int const last = std::min(cell.row + cell.row_span, m) - 1;
            float spanned = vs * static_cast<float>(last - cell.row);
            for (int r = cell.row; r <= last; ++r)
                spanned += row_height[at(r)];
            float const needed = std::max(boxes[i].height, boxes[i].min_height);
            if (needed > spanned)
                row_height[at(last)] += needed - spanned;
        }
        // A written height on the table stretches the rows, in proportion;
        // a table with no rows is that tall regardless.
        float rows_total = m > 0 ? vs * static_cast<float>(m + 1) : 0.0f;
        for (float const height : row_height)
            rows_total += height;
        {
            std::optional<float> target;
            float const vertical_edges = setup.edges_top + setup.edges_bottom;
            if (options.content_height)
                target = *options.content_height;
            else if (style.height.kind == LengthPercent::Kind::Px)
                target = style.height.value - (border_box ? vertical_edges : 0.0f);
            else if (style.height.kind == LengthPercent::Kind::Percent && options.containing_height)
                target = resolve(style.height, *options.containing_height) - (border_box ? vertical_edges : 0.0f);
            if (target && *target > rows_total) {
                float const extra = *target - rows_total;
                float sum = 0;
                for (float const height : row_height)
                    sum += height;
                for (float& height : row_height)
                    height += sum > 0 ? extra * height / sum : extra / static_cast<float>(std::max(m, 1));
                rows_total = *target;
            }
        }
        std::vector<float> row_y(at(m) + 1, grid_y);
        {
            float cy = grid_y + (m > 0 ? vs : 0.0f);
            for (int r = 0; r < m; ++r) {
                row_y[at(r)] = cy;
                cy += row_height[at(r)] + vs;
            }
            row_y[at(m)] = cy;
        }
        float const grid_bottom = m > 0 ? row_y[at(m)] : grid_y + rows_total;
        box.height = grid_bottom - grid_y + setup.edges_top + setup.edges_bottom;
        float const rows_top = m > 0 ? row_y[0] : grid_y;
        float const rows_bottom = m > 0 ? grid_bottom - vs : grid_y;

        // Column groups and columns: backgrounds under the rows.
        auto const background_of = [](ComputedStyle const* s) -> std::optional<Color> {
            if (!s || s->background_color.a == 0)
                return std::nullopt;
            return s->background_color;
        };
        for (table::ColumnGroup const& group : structure.column_groups) {
            std::optional<Color> const background = background_of(group.style);
            if (!background || group.count <= 0 || group.first >= n)
                continue;
            Fragment f;
            f.element = group.element;
            f.background = background;
            int const last = std::min(group.first + group.count, n);
            f.x = column_x[at(group.first)];
            f.width = column_x[at(last)] - hs - f.x;
            f.y = rows_top;
            f.height = std::max(0.0f, rows_bottom - rows_top);
            box.children.push_back(std::move(f));
        }
        for (int c = 0; c < n; ++c) {
            table::Column const& column = structure.columns[at(c)];
            std::optional<Color> const background = background_of(column.style);
            if (!background)
                continue;
            Fragment f;
            f.element = column.element;
            f.background = background;
            f.x = column_x[at(c)];
            f.width = widths.columns[at(c)];
            f.y = rows_top;
            f.height = std::max(0.0f, rows_bottom - rows_top);
            box.children.push_back(std::move(f));
        }

        // Row groups, rows, and the cells in them, each cell's content
        // placed in its box by vertical-align.
        std::optional<float> table_baseline;
        for (table::RowGroup const& group : structure.groups) {
            Fragment gf;
            gf.element = group.element;
            gf.background = group.element ? background_of(group.style) : std::nullopt;
            gf.x = n > 0 ? column_x[0] : grid_x;
            gf.width = n > 0 ? column_x[at(n)] - hs - gf.x : 0.0f;
            if (!group.rows.empty()) {
                std::size_t const first = group.rows.front();
                std::size_t const last = group.rows.back();
                gf.y = row_y[first];
                gf.height = row_y[last] + row_height[last] - gf.y;
            } else {
                gf.y = grid_y;
            }
            for (std::size_t const r : group.rows) {
                table::Row const& row = structure.rows[r];
                Fragment rf;
                rf.element = row.element;
                rf.background = row.element ? background_of(row.style) : std::nullopt;
                rf.x = gf.x;
                rf.width = gf.width;
                rf.y = row_y[r];
                rf.height = row_height[r];
                for (std::size_t const ci : row.cells) {
                    table::Cell const& cell = structure.cells[ci];
                    CellBox& cb = boxes[ci];
                    int const last = std::min(cell.row + cell.row_span, m) - 1;
                    float cell_height = vs * static_cast<float>(last - cell.row);
                    for (int rr = cell.row; rr <= last; ++rr)
                        cell_height += row_height[at(rr)];
                    float shift = 0;
                    switch (cb.align) {
                    case VerticalAlign::Kind::Middle:
                        shift = (cell_height - cb.height) / 2;
                        break;
                    case VerticalAlign::Kind::Bottom:
                        shift = cell_height - cb.height;
                        break;
                    case VerticalAlign::Kind::Baseline:
                        shift = row_baseline[at(cell.row)] - cb.baseline;
                        break;
                    default:
                        break;
                    }
                    shift = std::max(0.0f, shift);
                    shift_fragment(cb.fragment, 0, row_y[at(cell.row)] + shift);
                    cb.fragment.y = row_y[at(cell.row)];
                    cb.fragment.height = cell_height;
                    if (!cell.anonymous())
                        mark_positioned(cb.fragment, *cell.style, cb.width);
                    if (!table_baseline && cell.row == 0 && cb.fragment.first_baseline)
                        table_baseline = cb.fragment.first_baseline;
                    rf.children.push_back(std::move(cb.fragment));
                }
                gf.children.push_back(std::move(rf));
            }
            box.children.push_back(std::move(gf));
        }
        // The table's baseline is its first row's.
        box.first_baseline = table_baseline;
        box.last_baseline = table_baseline;
        cursor = box.y + box.height;
        wrapper.children.push_back(std::move(box));
        for (table::Caption const& caption : structure.captions) {
            if (caption.style->caption_side == css::CaptionSide::Bottom)
                lay_caption(caption);
        }
        wrapper.height = cursor - y;
        wrapper.first_baseline = table_baseline;
        wrapper.last_baseline = table_baseline;
        return wrapper;
    }

    // --- Grid ---------------------------------------------------------------------

    // A grid item: an element child, the container's own text wrapped in
    // an anonymous block, or one of its generated boxes (element = the
    // container, style = the box's own).
    struct GridItem {
        dom::Element const* element = nullptr;
        ComputedStyle const* style = nullptr;
        std::vector<InlineItem> inline_items;
        css::GeneratedBox const* generated = nullptr;
        int order = 0;
        grid::Area area;
        // Settled by the column pass: the content width, and where the
        // margin box starts across its area; then the content height at
        // that width, for the row pass.
        float content_width = 0;
        float x_offset = 0;
        float measured_height = 0;
    };

    // The items in order-modified document order. Out-of-flow children are
    // recorded against the container.
    std::vector<GridItem> collect_grid_items(dom::Element const& container, ComputedStyle const& style,
        float content_x, float content_y) const
    {
        std::vector<GridItem> items;
        std::vector<InlineItem> pending;
        auto const flush_text = [&] {
            bool content = false;
            for (InlineItem const& item : pending)
                content = content || is_inline_content(item);
            if (content) {
                GridItem item;
                item.style = &style;
                item.inline_items = std::move(pending);
                items.push_back(std::move(item));
            }
            pending.clear();
        };
        auto const add_generated = [&](css::GeneratedBox const& box) {
            GridItem item;
            item.element = &container;
            item.style = &box.style;
            item.generated = &box;
            item.order = box.style.order;
            std::u32string const text = decode_utf8(box.text);
            if (!text.empty())
                append_text(text, &box.style, item.inline_items, &container);
            items.push_back(std::move(item));
        };
        if (css::GeneratedBox const* const before = before_of(&style))
            add_generated(*before);
        for (dom::Node const* child : container.children()) {
            if (child->is_text()) {
                std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                if (!text.empty())
                    append_text(text, &style, pending, &container);
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(element);
            if (!child_style || child_style->display == Display::None)
                continue;
            if (child_style->out_of_flow()) {
                record_out_of_flow(element, *child_style, std::make_pair(content_x, content_y));
                continue;
            }
            flush_text();
            GridItem item;
            item.element = &element;
            item.style = child_style;
            item.order = child_style->order;
            items.push_back(std::move(item));
        }
        flush_text();
        if (css::GeneratedBox const* const after = after_of(&style))
            add_generated(*after);
        std::stable_sort(items.begin(), items.end(),
            [](GridItem const& a, GridItem const& b) { return a.order < b.order; });
        return items;
    }

    // A track sizing function's side with its length resolved: a percentage
    // is of the container's content size in that axis, auto while that is
    // indefinite.
    static grid::Breadth breadth_of(css::TrackBreadth const& breadth, std::optional<float> percent_base)
    {
        grid::Breadth out;
        switch (breadth.kind) {
        case css::TrackBreadth::Kind::Length:
            if (breadth.length.kind == LengthPercent::Kind::Px) {
                out.kind = grid::Breadth::Kind::Fixed;
                out.value = breadth.length.value;
            } else if (percent_base) {
                out.kind = grid::Breadth::Kind::Fixed;
                out.value = resolve(breadth.length, *percent_base);
            } else {
                out.kind = grid::Breadth::Kind::Auto;
            }
            break;
        case css::TrackBreadth::Kind::Flex:
            out.kind = grid::Breadth::Kind::Flex;
            out.value = breadth.fr;
            break;
        case css::TrackBreadth::Kind::Auto:
            out.kind = grid::Breadth::Kind::Auto;
            break;
        case css::TrackBreadth::Kind::MinContent:
            out.kind = grid::Breadth::Kind::MinContent;
            break;
        case css::TrackBreadth::Kind::MaxContent:
            out.kind = grid::Breadth::Kind::MaxContent;
            break;
        }
        return out;
    }

    static grid::Track track_of(css::TrackSize const& size, std::optional<float> percent_base)
    {
        grid::Track track;
        track.min = breadth_of(size.min, percent_base);
        track.max = breadth_of(size.max, percent_base);
        if (track.min.kind == grid::Breadth::Kind::Flex)
            track.min.kind = grid::Breadth::Kind::Auto;
        if (size.fit_content) {
            if (size.fit_content->kind == LengthPercent::Kind::Px)
                track.fit_content = size.fit_content->value;
            else if (percent_base)
                track.fit_content = resolve(*size.fit_content, *percent_base);
        }
        return track;
    }

    // The explicit tracks of one axis: the template's list with its
    // auto-repeat expanded to what fits, padded to the areas' extent, and
    // the line names — the template's own and the areas' -start and -end.
    struct AxisTracks {
        std::vector<grid::Track> tracks;
        grid::AxisLines lines;
        // The auto-repeat's tracks, for auto-fit's collapsing of empty ones.
        std::size_t repeat_first = 0;
        std::size_t repeat_count = 0;
        bool fit = false;
    };

    // `available` is the container's definite content size in this axis;
    // while it is indefinite an auto-repeat is counted against the maximum
    // size, else the minimum size, else it repeats once.
    static AxisTracks explicit_tracks(css::GridTrackList const* list, css::GridAreas const* areas, bool columns,
        std::optional<float> available, std::optional<float> max_size, std::optional<float> min_size, float gap,
        std::vector<css::TrackSize> const* auto_sizes)
    {
        AxisTracks axis;
        std::vector<std::vector<std::string>> line_names;
        std::vector<std::string> carried; // names for the next line
        auto const append = [&](css::GridTrackList::Track const& track) {
            std::vector<std::string> names = carried;
            names.insert(names.end(), track.names.begin(), track.names.end());
            carried.clear();
            line_names.push_back(std::move(names));
            axis.tracks.push_back(track_of(track.size, available));
        };
        auto const expand_repeat = [&] {
            int repetitions = 1;
            {
                auto const size_for_count = [&](css::TrackSize const& size) {
                    grid::Track const track = track_of(size, available);
                    if (track.max.kind == grid::Breadth::Kind::Fixed)
                        return track.max.value;
                    if (track.min.kind == grid::Breadth::Kind::Fixed)
                        return track.min.value;
                    return 0.0f;
                };
                float fixed = 0;
                for (css::GridTrackList::Track const& track : list->tracks)
                    fixed += size_for_count(track.size);
                float repeat = 0;
                for (css::GridTrackList::Track const& track : list->auto_repeat_tracks)
                    repeat += size_for_count(track.size);
                auto const fixed_count = static_cast<int>(list->tracks.size());
                auto const repeat_count = static_cast<int>(list->auto_repeat_tracks.size());
                if (available)
                    repetitions = grid::repetitions_that_fit(*available, false, fixed, fixed_count, repeat,
                        repeat_count, gap);
                else if (max_size)
                    repetitions = grid::repetitions_that_fit(*max_size, false, fixed, fixed_count, repeat,
                        repeat_count, gap);
                else if (min_size)
                    repetitions = grid::repetitions_that_fit(*min_size, true, fixed, fixed_count, repeat,
                        repeat_count, gap);
            }
            axis.repeat_first = axis.tracks.size();
            axis.fit = list->auto_repeat == css::GridTrackList::AutoRepeat::Fit;
            carried.insert(carried.end(), list->auto_repeat_leading_names.begin(),
                list->auto_repeat_leading_names.end());
            for (int r = 0; r < repetitions; ++r) {
                for (css::GridTrackList::Track const& track : list->auto_repeat_tracks)
                    append(track);
                carried = list->auto_repeat_trailing_names;
            }
            axis.repeat_count = axis.tracks.size() - axis.repeat_first;
            // The names after the last repetition are already on the next
            // track (or the list's end) — the parser put them there.
            carried.clear();
        };
        if (list) {
            bool const repeats = list->auto_repeat != css::GridTrackList::AutoRepeat::None;
            for (std::size_t i = 0; i < list->tracks.size(); ++i) {
                if (repeats && i == list->auto_repeat_at)
                    expand_repeat();
                append(list->tracks[i]);
            }
            if (repeats && list->auto_repeat_at >= list->tracks.size())
                expand_repeat();
            carried.insert(carried.end(), list->trailing_names.begin(), list->trailing_names.end());
        }
        // The areas may reach past the template; the tracks added take
        // their sizes from grid-auto-columns or -rows, from its start.
        int const from_areas = areas ? (columns ? areas->columns : areas->rows) : 0;
        for (int k = 0; static_cast<int>(axis.tracks.size()) < from_areas; ++k) {
            css::GridTrackList::Track track;
            if (auto_sizes && !auto_sizes->empty())
                track.size = (*auto_sizes)[static_cast<std::size_t>(k) % auto_sizes->size()];
            append(track);
        }
        line_names.push_back(std::move(carried));
        if (areas) {
            for (css::GridAreas::Area const& area : areas->areas) {
                auto const start = static_cast<std::size_t>((columns ? area.column_start : area.row_start) - 1);
                auto const end = static_cast<std::size_t>((columns ? area.column_end : area.row_end) - 1);
                if (start < line_names.size())
                    line_names[start].push_back(area.name + "-start");
                if (end < line_names.size())
                    line_names[end].push_back(area.name + "-end");
            }
        }
        axis.lines.names = std::move(line_names);
        return axis;
    }

    // The implicit grid's tracks in one axis: the explicit ones with the
    // implicit ones before and after them, sized by grid-auto-columns or
    // -rows — the pattern runs forwards after the explicit grid and
    // backwards before it.
    static std::vector<grid::Track> implicit_tracks(AxisTracks const& axis, int count, int offset,
        std::vector<css::TrackSize> const* auto_sizes, std::optional<float> percent_base)
    {
        std::vector<grid::Track> tracks;
        auto const explicit_count = static_cast<int>(axis.tracks.size());
        auto const pattern = [&](int k) {
            if (!auto_sizes || auto_sizes->empty())
                return grid::Track {};
            auto const n = static_cast<int>(auto_sizes->size());
            int const i = ((k % n) + n) % n;
            return track_of((*auto_sizes)[static_cast<std::size_t>(i)], percent_base);
        };
        for (int i = 0; i < count; ++i) {
            if (i < offset)
                tracks.push_back(pattern(i - offset)); // -1 is the one just before the grid
            else if (i - offset < explicit_count)
                tracks.push_back(axis.tracks[static_cast<std::size_t>(i - offset)]);
            else
                tracks.push_back(pattern(i - offset - explicit_count));
        }
        return tracks;
    }

    // auto-fit: a repetition's track that no item spans collapses.
    static void collapse_empty(std::vector<grid::Track>& tracks, AxisTracks const& axis, int offset,
        std::vector<GridItem> const& items, bool columns)
    {
        if (!axis.fit || axis.repeat_count == 0)
            return;
        for (std::size_t k = 0; k < axis.repeat_count; ++k) {
            int const t = offset + static_cast<int>(axis.repeat_first + k);
            if (t < 0 || t >= static_cast<int>(tracks.size()))
                continue;
            bool used = false;
            for (GridItem const& item : items) {
                int const start = columns ? item.area.column_start : item.area.row_start;
                int const end = columns ? item.area.column_end : item.area.row_end;
                if (start <= t && t < end) {
                    used = true;
                    break;
                }
            }
            if (!used)
                tracks[static_cast<std::size_t>(t)].collapsed = true;
        }
    }

    // The tracks' free space goes where the content-distribution says.
    static grid::Distribution distribution_of(css::JustifyContent justify)
    {
        using css::JustifyContent;
        switch (justify) {
        case JustifyContent::Normal:
        case JustifyContent::Stretch: return grid::Distribution::Stretch;
        case JustifyContent::FlexStart: return grid::Distribution::Start;
        case JustifyContent::FlexEnd: return grid::Distribution::End;
        case JustifyContent::Center: return grid::Distribution::Center;
        case JustifyContent::SpaceBetween: return grid::Distribution::SpaceBetween;
        case JustifyContent::SpaceAround: return grid::Distribution::SpaceAround;
        case JustifyContent::SpaceEvenly: return grid::Distribution::SpaceEvenly;
        }
        return grid::Distribution::Start;
    }

    static grid::Distribution distribution_of(css::AlignContent align)
    {
        using css::AlignContent;
        switch (align) {
        case AlignContent::Stretch: return grid::Distribution::Stretch;
        case AlignContent::FlexStart: return grid::Distribution::Start;
        case AlignContent::FlexEnd: return grid::Distribution::End;
        case AlignContent::Center: return grid::Distribution::Center;
        case AlignContent::SpaceBetween: return grid::Distribution::SpaceBetween;
        case AlignContent::SpaceAround: return grid::Distribution::SpaceAround;
        case AlignContent::SpaceEvenly: return grid::Distribution::SpaceEvenly;
        }
        return grid::Distribution::Start;
    }

    // Where the items go: their lines resolved and the rest auto-placed.
    static grid::PlacedGrid place_grid(std::vector<GridItem>& items, AxisTracks const& columns,
        AxisTracks const& rows, css::GridAutoFlow flow)
    {
        std::vector<grid::ItemPlacement> placements;
        placements.reserve(items.size());
        for (GridItem const& item : items) {
            grid::ItemPlacement placement;
            if (item.element) {
                placement.rows = grid::resolve_lines(item.style->grid_row_start, item.style->grid_row_end, rows.lines);
                placement.columns = grid::resolve_lines(item.style->grid_column_start,
                    item.style->grid_column_end, columns.lines);
            }
            placements.push_back(placement);
        }
        grid::PlacedGrid placed = grid::place_items(placements, rows.lines.tracks(), columns.lines.tracks(), flow);
        for (std::size_t i = 0; i < items.size(); ++i)
            items[i].area = placed.areas[i];
        return placed;
    }

    // An item's contributions along the columns: its outer min-content,
    // max-content and minimum sizes (percentages of the area count as
    // zero until the area is known).
    grid::Contribution column_contribution(GridItem const& item) const
    {
        grid::Contribution contribution;
        contribution.start = item.area.column_start;
        contribution.end = item.area.column_end;
        Intrinsic const intrinsic
            = item.element ? block_intrinsic(*item.element, *item.style) : inline_intrinsic(item.inline_items);
        contribution.min_content = intrinsic.min;
        contribution.max_content = intrinsic.max;
        contribution.minimum = intrinsic.min;
        if (item.element) {
            // The automatic minimum is the min-content size (a written
            // width stands in for it), unless the box clips its overflow
            // or writes a minimum.
            ComputedStyle const& s = *item.style;
            float const margins = resolve(s.margin_left, 0) + resolve(s.margin_right, 0);
            float const inner_edges = resolve(s.padding_left, 0) + resolve(s.padding_right, 0)
                + s.border_left.width + s.border_right.width;
            float const edges = margins + inner_edges;
            if (s.min_width.kind == LengthPercent::Kind::Px)
                contribution.minimum
                    = as_content_size(s, s.min_width.value, inner_edges) + edges;
            else if (s.overflow != css::Overflow::Visible)
                contribution.minimum = edges;
        }
        return contribution;
    }

    // Whether an item keeps its own size under a normal alignment: a
    // picture, an embedded box or a control does; a block stretches.
    static bool keeps_own_size(GridItem const& item)
    {
        return item.element && !item.generated && (is_replaced(*item.element) || is_control(*item.element));
    }

    // Settles an item's content width and where its margin box starts
    // across an area this wide: stretched to it, sized as written, or
    // shrink-to-fit and placed by justify-self (auto margins take the
    // free space first).
    void size_grid_item_width(GridItem& item, float area_width, ComputedStyle const& container) const
    {
        if (!item.element) {
            item.content_width = area_width;
            item.x_offset = 0;
            return;
        }
        ComputedStyle const& s = *item.style;
        bool const auto_left = s.margin_left.is_auto();
        bool const auto_right = s.margin_right.is_auto();
        float const margin_left = resolve(s.margin_left, area_width);
        float const margin_right = resolve(s.margin_right, area_width);
        float const edges = resolve(s.padding_left, area_width) + resolve(s.padding_right, area_width)
            + s.border_left.width + s.border_right.width;
        css::AlignItems justify = s.justify_self == css::AlignItems::Auto ? container.justify_items : s.justify_self;
        bool const own_size = keeps_own_size(item);
        if (justify == css::AlignItems::Normal || justify == css::AlignItems::Auto)
            justify = own_size ? css::AlignItems::FlexStart : css::AlignItems::Stretch;
        if (justify == css::AlignItems::Baseline)
            justify = css::AlignItems::FlexStart;
        float const available = std::max(0.0f, area_width - margin_left - margin_right - edges);
        float content;
        if (!s.width.is_auto()) {
            content = clamp_width(s, as_content_size(s, resolve(s.width, area_width), edges), area_width, edges);
        } else if (justify == css::AlignItems::Stretch && !auto_left && !auto_right && !own_size) {
            content = clamp_width(s, available, area_width, edges);
        } else {
            Intrinsic const intrinsic = intrinsic_widths(*item.element, s);
            content
                = clamp_width(s, std::min(std::max(intrinsic.min, available), intrinsic.max), area_width, edges);
        }
        float const free = area_width - (margin_left + edges + content + margin_right);
        float offset = 0;
        if (auto_left && auto_right)
            offset = std::max(0.0f, free) / 2;
        else if (auto_left)
            offset = std::max(0.0f, free);
        else if (!auto_right && justify == css::AlignItems::FlexEnd)
            offset = free;
        else if (!auto_right && justify == css::AlignItems::Center)
            offset = free / 2;
        item.content_width = content;
        item.x_offset = offset;
    }

    // An item's content height at its settled width.
    float grid_item_height(GridItem const& item, float area_width, int list_depth) const
    {
        return measure_item_height(item.element, *item.style, item.inline_items, item.generated,
            item.content_width, area_width, list_depth);
    }

    // An item's contribution along the rows: its outer height at its width.
    grid::Contribution row_contribution(GridItem const& item, float area_width) const
    {
        grid::Contribution contribution;
        contribution.start = item.area.row_start;
        contribution.end = item.area.row_end;
        float outer = item.measured_height;
        float minimum = outer;
        if (item.element) {
            ComputedStyle const& s = *item.style;
            float const edges = resolve(s.margin_top, area_width) + resolve(s.margin_bottom, area_width)
                + resolve(s.padding_top, area_width) + resolve(s.padding_bottom, area_width)
                + s.border_top.width + s.border_bottom.width;
            outer += edges;
            minimum = outer;
            if (s.min_height.kind == LengthPercent::Kind::Px)
                minimum = s.min_height.value + edges;
            else if (s.overflow != css::Overflow::Visible)
                minimum = edges;
        }
        contribution.minimum = minimum;
        contribution.min_content = outer;
        contribution.max_content = outer;
        return contribution;
    }

    // Lays an item out in its area: across it as the column pass settled,
    // down it by align-self — stretched to the area's height, sized as
    // written, or its own height placed (auto margins first).
    void place_grid_item(GridItem const& item, float area_x, float area_y, float area_width,
        float area_height, ComputedStyle const& container, int list_depth, Fragment& parent) const
    {
        FloatContext scratch_floats;
        float const x = area_x + item.x_offset;
        if (!item.element) {
            Fragment box;
            box.x = x;
            box.y = area_y;
            box.width = item.content_width;
            box.height = layout_lines(item.inline_items, *item.style, x, area_y, item.content_width, box,
                scratch_floats, list_depth);
            parent.children.push_back(std::move(box));
            return;
        }
        ComputedStyle const& s = *item.style;
        bool const auto_top = s.margin_top.is_auto();
        bool const auto_bottom = s.margin_bottom.is_auto();
        float const margin_top = resolve(s.margin_top, area_width);
        float const margin_bottom = resolve(s.margin_bottom, area_width);
        float const edges = resolve(s.padding_top, area_width) + resolve(s.padding_bottom, area_width)
            + s.border_top.width + s.border_bottom.width;
        css::AlignItems align = s.align_self == css::AlignItems::Auto ? container.align_items : s.align_self;
        bool const own_size = keeps_own_size(item);
        if (align == css::AlignItems::Normal || align == css::AlignItems::Auto)
            align = own_size ? css::AlignItems::FlexStart : css::AlignItems::Stretch;
        if (align == css::AlignItems::Baseline)
            align = css::AlignItems::FlexStart;
        std::optional<float> content_height;
        if (std::optional<float> const written = definite_height_of(s, area_height, edges))
            content_height = clamp_height(s, *written, area_height, edges);
        else if (align == css::AlignItems::Stretch && !auto_top && !auto_bottom && !own_size)
            content_height = clamp_height(s, std::max(0.0f, area_height - margin_top - margin_bottom - edges),
                area_height, edges);
        float const content = content_height.value_or(item.measured_height);
        float const free = area_height - (margin_top + edges + content + margin_bottom);
        float offset = 0;
        if (auto_top && auto_bottom)
            offset = std::max(0.0f, free) / 2;
        else if (auto_top)
            offset = std::max(0.0f, free);
        else if (!auto_bottom && align == css::AlignItems::FlexEnd)
            offset = free;
        else if (!auto_bottom && align == css::AlignItems::Center)
            offset = free / 2;
        float const y = area_y + offset + margin_top;
        if (item.generated) {
            parent.children.push_back(layout_generated_block(*item.generated, *item.element, x, y, area_width,
                scratch_floats, item.content_width, content_height));
            return;
        }
        BlockOptions options;
        options.content_width = item.content_width;
        options.content_height = content_height;
        options.zero_auto_margins = true;
        options.own_context = true;
        options.containing_height = area_height;
        Fragment box = layout_block(*item.element, s, x, y, area_width, list_depth, scratch_floats, options);
        mark_positioned(box, s, area_width);
        mark_item_layer(box, s);
        parent.children.push_back(std::move(box));
    }

    // The offset and size of the tracks an item spans, collapsed ones aside.
    static std::pair<float, float> span_extent(std::vector<float> const& sizes,
        grid::TrackPositions const& positions, std::vector<grid::Track> const& tracks, int start, int end)
    {
        auto const n = static_cast<int>(sizes.size());
        start = std::clamp(start, 0, std::max(0, n - 1));
        end = std::clamp(end, start + 1, n);
        if (n == 0)
            return { 0.0f, 0.0f };
        float const from = positions.offsets[static_cast<std::size_t>(start)];
        float to = from;
        for (int t = end - 1; t >= start; --t) {
            auto const i = static_cast<std::size_t>(t);
            if (!tracks[i].collapsed) {
                to = positions.offsets[i] + sizes[i];
                break;
            }
        }
        return { from, std::max(0.0f, to - from) };
    }

    static std::optional<float> px_of(LengthPercent const& length)
    {
        if (length.kind == LengthPercent::Kind::Px)
            return length.value;
        return std::nullopt;
    }

    // Whether a track size has a percentage in it.
    static bool uses_percentage(css::TrackSize const& size)
    {
        auto const percent = [](css::TrackBreadth const& breadth) {
            return breadth.kind == css::TrackBreadth::Kind::Length
                && breadth.length.kind != LengthPercent::Kind::Px;
        };
        return percent(size.min) || percent(size.max)
            || (size.fit_content && size.fit_content->kind != LengthPercent::Kind::Px);
    }

    // Whether the rows or the row gap ask for a percentage of the height.
    static bool rows_use_percentages(ComputedStyle const& style)
    {
        if (style.row_gap.kind != LengthPercent::Kind::Px)
            return true;
        if (css::GridTrackList const* const list = style.grid_template_rows.get()) {
            for (css::GridTrackList::Track const& track : list->tracks) {
                if (uses_percentage(track.size))
                    return true;
            }
            for (css::GridTrackList::Track const& track : list->auto_repeat_tracks) {
                if (uses_percentage(track.size))
                    return true;
            }
        }
        if (style.grid_auto_rows) {
            for (css::TrackSize const& size : *style.grid_auto_rows) {
                if (uses_percentage(size))
                    return true;
            }
        }
        return false;
    }

    // The grid layout algorithm: the items and the explicit grid, their
    // placement, the column sizes, then the row sizes from the items'
    // heights at their widths, then each item in its area. Returns the
    // content height.
    // Where an explicit grid line falls in the axis, measured from the
    // container's content edge: the offset of the track that starts there,
    // or the far edge of the last track that has a size.
    static float line_position(std::vector<float> const& sizes, grid::TrackPositions const& positions,
        std::vector<grid::Track> const& tracks, int line)
    {
        auto const n = static_cast<int>(sizes.size());
        if (n == 0)
            return 0;
        if (line <= 0)
            return positions.offsets[0];
        if (line < n)
            return positions.offsets[static_cast<std::size_t>(line)];
        for (int t = n - 1; t >= 0; --t) {
            auto const i = static_cast<std::size_t>(t);
            if (!tracks[i].collapsed)
                return positions.offsets[i] + sizes[i];
        }
        return positions.offsets[0];
    }

    // css-grid-2 §9: an absolutely positioned child of a grid container is
    // not a grid item, but when the container is also its containing block
    // its placement still names a grid area, and that area is what it is
    // laid out in. A property that names a line the grid does not have —
    // outside the implicit grid the in-flow items made — counts as auto,
    // and an auto edge stays on the container's padding edge.
    struct AbsEdges {
        std::optional<int> start; // an index into the implicit grid's lines
        std::optional<int> end;
    };

    static AbsEdges abspos_edges(css::GridLine const& start, css::GridLine const& end,
        grid::AxisLines const& lines, int offset, std::size_t track_count)
    {
        AbsEdges edges;
        bool const start_auto = start.is_auto();
        bool const end_auto = end.is_auto();
        if (start_auto && end_auto)
            return edges;
        grid::AxisPlacement const placement = grid::resolve_lines(start, end, lines);
        auto const line_in_grid = [&](std::optional<int> line) -> std::optional<int> {
            if (!line)
                return std::nullopt; // a lone span has nothing to span from
            int const index = offset + *line;
            if (index < 0 || index > static_cast<int>(track_count))
                return std::nullopt; // no such line: the property counts as auto
            return index;
        };
        if (!start_auto)
            edges.start = line_in_grid(placement.start);
        if (!end_auto)
            edges.end = line_in_grid(placement.end);
        return edges;
    }

    // Hands each absolutely positioned child of a grid container that this
    // container contains the grid area its placement names.
    void give_grid_areas(dom::Element const& container, float content_x, float content_y,
        AxisTracks const& columns, std::vector<float> const& column_sizes,
        grid::TrackPositions const& column_positions, std::vector<grid::Track> const& column_tracks,
        int column_offset, AxisTracks const& rows, std::vector<float> const& row_sizes,
        grid::TrackPositions const& row_positions, std::vector<grid::Track> const& row_tracks,
        int row_offset) const
    {
        if (absolute_stack.empty() || absolute_stack.back().empty())
            return;
        for (OutOfFlow& box : absolute_stack.back()) {
            if (!box.element || box.element->parent() != &container)
                continue;
            ComputedStyle const& s = *box.style;
            AbsEdges const across = abspos_edges(s.grid_column_start, s.grid_column_end, columns.lines,
                column_offset, column_sizes.size());
            AbsEdges const down
                = abspos_edges(s.grid_row_start, s.grid_row_end, rows.lines, row_offset, row_sizes.size());
            if (across.start)
                box.area_left = content_x
                    + line_position(column_sizes, column_positions, column_tracks, *across.start);
            if (across.end)
                box.area_right
                    = content_x + line_position(column_sizes, column_positions, column_tracks, *across.end);
            if (down.start)
                box.area_top = content_y + line_position(row_sizes, row_positions, row_tracks, *down.start);
            if (down.end)
                box.area_bottom = content_y + line_position(row_sizes, row_positions, row_tracks, *down.end);
        }
    }

    float layout_grid(dom::Element const& container, ComputedStyle const& style, float content_x,
        float content_y, float content_width, Fragment& fragment, int list_depth,
        std::optional<float> own_height) const
    {
        std::vector<GridItem> items = collect_grid_items(container, style, content_x, content_y);
        float const column_gap = resolve(style.column_gap, content_width);
        float const row_gap = resolve(style.row_gap, own_height.value_or(0.0f));
        css::GridAreas const* const areas = style.grid_template_areas.get();
        AxisTracks const columns = explicit_tracks(style.grid_template_columns.get(), areas, true, content_width,
            std::nullopt, std::nullopt, column_gap, style.grid_auto_columns.get());
        AxisTracks const rows = explicit_tracks(style.grid_template_rows.get(), areas, false, own_height,
            own_height ? std::nullopt : px_of(style.max_height), own_height ? std::nullopt : px_of(style.min_height),
            row_gap, style.grid_auto_rows.get());
        grid::PlacedGrid const placed = place_grid(items, columns, rows, style.grid_auto_flow);

        std::vector<grid::Track> column_tracks = implicit_tracks(columns, placed.columns, placed.column_offset,
            style.grid_auto_columns.get(), content_width);
        collapse_empty(column_tracks, columns, placed.column_offset, items, true);

        // Columns.
        grid::SizingInput column_input;
        column_input.tracks = column_tracks;
        for (GridItem const& item : items)
            column_input.items.push_back(column_contribution(item));
        column_input.available = content_width;
        column_input.gap = column_gap;
        column_input.stretch = style.justify_content == css::JustifyContent::Normal
            || style.justify_content == css::JustifyContent::Stretch;
        std::vector<float> const column_sizes = grid::size_tracks(column_input);
        std::vector<bool> column_collapsed;
        for (grid::Track const& track : column_tracks)
            column_collapsed.push_back(track.collapsed);
        grid::TrackPositions const column_positions = grid::distribute_tracks(column_sizes, column_collapsed,
            column_gap, content_width, distribution_of(style.justify_content));

        // The items' widths, and their heights at those widths.
        std::vector<float> widths(items.size(), 0.0f);
        for (std::size_t i = 0; i < items.size(); ++i) {
            GridItem& item = items[i];
            auto const [x, width]
                = span_extent(column_sizes, column_positions, column_tracks, item.area.column_start, item.area.column_end);
            size_grid_item_width(item, width, style);
            item.measured_height = grid_item_height(item, width, list_depth);
            widths[i] = width;
        }

        // Rows: sized within the container's definite height, else to their
        // content. A percentage row or row gap under an indefinite height
        // counts as auto (and zero) while the content height is found, then
        // resolves against that height and the rows are sized again in it.
        struct RowPass {
            float gap = 0;
            std::vector<grid::Track> tracks;
            std::vector<float> sizes;
            grid::TrackPositions positions;
        };
        auto const size_rows = [&](std::optional<float> height) {
            RowPass pass;
            pass.gap = resolve(style.row_gap, height.value_or(0.0f));
            AxisTracks const axis = explicit_tracks(style.grid_template_rows.get(), areas, false, height,
                height ? std::nullopt : px_of(style.max_height), height ? std::nullopt : px_of(style.min_height),
                pass.gap, style.grid_auto_rows.get());
            pass.tracks = implicit_tracks(axis, placed.rows, placed.row_offset, style.grid_auto_rows.get(), height);
            collapse_empty(pass.tracks, axis, placed.row_offset, items, false);
            grid::SizingInput input;
            input.tracks = pass.tracks;
            for (std::size_t i = 0; i < items.size(); ++i)
                input.items.push_back(row_contribution(items[i], widths[i]));
            input.available = height;
            input.gap = pass.gap;
            input.stretch = style.align_content == css::AlignContent::Stretch;
            pass.sizes = grid::size_tracks(input);
            std::vector<bool> collapsed;
            for (grid::Track const& track : pass.tracks)
                collapsed.push_back(track.collapsed);
            pass.positions = grid::distribute_tracks(pass.sizes, collapsed, pass.gap, height,
                distribution_of(style.align_content));
            return pass;
        };
        std::optional<float> row_height = own_height;
        RowPass row_pass = size_rows(row_height);
        if (!own_height && rows_use_percentages(style)) {
            row_height = clamp_height(style, row_pass.positions.extent);
            row_pass = size_rows(row_height);
        }

        // Each item in its area.
        for (std::size_t i = 0; i < items.size(); ++i) {
            GridItem const& item = items[i];
            auto const [x, width]
                = span_extent(column_sizes, column_positions, column_tracks, item.area.column_start, item.area.column_end);
            auto const [y, height]
                = span_extent(row_pass.sizes, row_pass.positions, row_pass.tracks, item.area.row_start, item.area.row_end);
            place_grid_item(item, content_x + x, content_y + y, width, height, style, list_depth, fragment);
        }
        // An absolutely positioned child this container contains is laid
        // out in the area its placement named, not in the padding box.
        if (style.positioned()) {
            give_grid_areas(container, content_x, content_y, columns, column_sizes, column_positions,
                column_tracks, placed.column_offset, rows, row_pass.sizes, row_pass.positions,
                row_pass.tracks, placed.row_offset);
        }
        if (row_height)
            return *row_height;
        return row_pass.positions.extent;
    }

    // A grid container's intrinsic widths: its columns sized under a
    // min-content, then a max-content constraint, gaps included.
    Intrinsic grid_intrinsic_widths(dom::Element const& container, ComputedStyle const& style) const
    {
        std::vector<GridItem> items = collect_grid_items(container, style, 0, 0);
        float const column_gap = resolve(style.column_gap, 0);
        css::GridAreas const* const areas = style.grid_template_areas.get();
        AxisTracks const columns = explicit_tracks(style.grid_template_columns.get(), areas, true, std::nullopt,
            px_of(style.max_width), px_of(style.min_width), column_gap, style.grid_auto_columns.get());
        AxisTracks const rows = explicit_tracks(style.grid_template_rows.get(), areas, false, std::nullopt,
            px_of(style.max_height), px_of(style.min_height), 0, style.grid_auto_rows.get());
        grid::PlacedGrid const placed = place_grid(items, columns, rows, style.grid_auto_flow);
        std::vector<grid::Track> column_tracks = implicit_tracks(columns, placed.columns, placed.column_offset,
            style.grid_auto_columns.get(), std::nullopt);
        collapse_empty(column_tracks, columns, placed.column_offset, items, true);
        grid::SizingInput input;
        input.tracks = column_tracks;
        for (GridItem const& item : items)
            input.items.push_back(column_contribution(item));
        input.gap = column_gap;
        input.stretch = false;
        auto const total = [&](grid::Constraint constraint) {
            input.constraint = constraint;
            std::vector<float> const sizes = grid::size_tracks(input);
            float sum = 0;
            int count = 0;
            for (std::size_t t = 0; t < sizes.size(); ++t) {
                if (column_tracks[t].collapsed)
                    continue;
                sum += sizes[t];
                ++count;
            }
            return sum + (count > 1 ? column_gap * static_cast<float>(count - 1) : 0.0f);
        };
        Intrinsic result;
        result.min = total(grid::Constraint::MinContent);
        result.max = total(grid::Constraint::MaxContent);
        return result;
    }

    // The flexbox algorithm in one pass: the items, their base sizes, the
    // lines, flexible lengths, cross sizes and alignment, then placement.
    // Returns the content height.
    float layout_flex(dom::Element const& container, ComputedStyle const& style, float content_x,
        float content_y, float content_width, Fragment& fragment, int list_depth,
        std::optional<float> own_height = std::nullopt) const
    {
        using css::AlignContent;
        using css::AlignItems;
        using css::FlexDirection;
        using css::JustifyContent;
        bool const horizontal = style.flex_direction == FlexDirection::Row
            || style.flex_direction == FlexDirection::RowReverse;
        bool const reversed = style.flex_direction == FlexDirection::RowReverse
            || style.flex_direction == FlexDirection::ColumnReverse;
        bool const wrap = style.flex_wrap != css::FlexWrap::NoWrap;
        bool const wrap_reversed = style.flex_wrap == css::FlexWrap::WrapReverse;
        // The inner sizes: the width is definite here, the height when
        // written as a length, a percentage of a definite containing
        // height, or settled by the container's own formatting context.
        std::optional<float> const definite_height = own_height;
        std::optional<float> const main_size
            = horizontal ? std::optional<float>(content_width) : definite_height;
        std::optional<float> const cross_size
            = horizontal ? definite_height : std::optional<float>(content_width);
        // A percentage gap is of the container's content box in that axis,
        // zero while the height is indefinite.
        float const column_gap = resolve(style.column_gap, content_width);
        float const row_gap = resolve(style.row_gap, definite_height.value_or(0.0f));
        float const main_gap = horizontal ? column_gap : row_gap;
        float const cross_gap = horizontal ? row_gap : column_gap;

        // 1. The items: element children in order, and the container's own
        // text wrapped in anonymous items; a ::before or ::after box is an
        // item at its end with its own style and its text as content.
        std::vector<FlexItem> items;
        std::vector<InlineItem> pending;
        auto const flush_text = [&] {
            bool content = false;
            for (InlineItem const& item : pending) {
                if (is_inline_content(item))
                    content = true;
            }
            if (content) {
                FlexItem item;
                item.style = &style;
                item.inline_items = std::move(pending);
                items.push_back(std::move(item));
            }
            pending.clear();
        };
        auto const add_generated = [&](css::GeneratedBox const& box) {
            FlexItem item;
            item.element = &container;
            item.style = &box.style;
            item.generated = &box;
            item.order = box.style.order;
            item.grow = box.style.flex_grow;
            item.shrink = box.style.flex_shrink;
            std::u32string const text = decode_utf8(box.text);
            if (!text.empty())
                append_text(text, &box.style, item.inline_items, &container);
            items.push_back(std::move(item));
        };
        if (css::GeneratedBox const* const before = before_of(&style))
            add_generated(*before);
        for (dom::Node const* child : container.children()) {
            if (child->is_text()) {
                std::u32string const text = decode_utf8(static_cast<dom::Text const*>(child)->data);
                if (!text.empty())
                    append_text(text, &style, pending, &container);
                continue;
            }
            if (!child->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*child);
            ComputedStyle const* child_style = style_of(element);
            if (!child_style || child_style->display == Display::None)
                continue;
            if (child_style->out_of_flow()) {
                // Not an item: placed against the container once it is done.
                record_out_of_flow(element, *child_style, std::make_pair(content_x, content_y));
                continue;
            }
            flush_text();
            FlexItem item;
            item.element = &element;
            item.style = child_style;
            item.order = child_style->order;
            item.grow = child_style->flex_grow;
            item.shrink = child_style->flex_shrink;
            items.push_back(std::move(item));
        }
        flush_text();
        if (css::GeneratedBox const* const after = after_of(&style))
            add_generated(*after);
        if (items.empty())
            return definite_height.value_or(0.0f);
        std::stable_sort(items.begin(), items.end(),
            [](FlexItem const& a, FlexItem const& b) { return a.order < b.order; });

        // 2. Each item's edges, flex base size and automatic minimum.
        for (FlexItem& item : items) {
            ComputedStyle const& s = *item.style;
            bool const anonymous = !item.element;
            float const margin_left = anonymous ? 0 : resolve(s.margin_left, content_width);
            float const margin_right = anonymous ? 0 : resolve(s.margin_right, content_width);
            float const margin_top = anonymous ? 0 : resolve(s.margin_top, content_width);
            float const margin_bottom = anonymous ? 0 : resolve(s.margin_bottom, content_width);
            float const horizontal_edges = anonymous ? 0
                : resolve(s.padding_left, content_width) + resolve(s.padding_right, content_width)
                    + s.border_left.width + s.border_right.width;
            float const vertical_edges = anonymous ? 0
                : resolve(s.padding_top, content_width) + resolve(s.padding_bottom, content_width)
                    + s.border_top.width + s.border_bottom.width;
            if (horizontal) {
                item.margin_start = margin_left;
                item.margin_end = margin_right;
                item.margin_cross_start = margin_top;
                item.margin_cross_end = margin_bottom;
                item.edges_main = horizontal_edges;
                item.edges_cross = vertical_edges;
            } else {
                item.margin_start = margin_top;
                item.margin_end = margin_bottom;
                item.margin_cross_start = margin_left;
                item.margin_cross_end = margin_right;
                item.edges_main = vertical_edges;
                item.edges_cross = horizontal_edges;
            }

            // The flex base size: flex-basis, else the main size property,
            // else the content's size.
            LengthPercent basis = anonymous ? LengthPercent::auto_value() : s.flex_basis;
            // Only a basis that is really `auto` defers to the main size
            // property: a content keyword is a basis of its own.
            if (basis.is_auto() && !basis.is_content_size() && !anonymous)
                basis = horizontal ? s.width : s.height;
            bool const definite_basis = !basis.is_auto()
                && (basis.kind == LengthPercent::Kind::Px || main_size.has_value());
            Intrinsic const intrinsic = anonymous || item.generated
                ? inline_intrinsic(item.inline_items)
                : intrinsic_widths(*item.element, s);
            if (horizontal) {
                // A written base, minimum or maximum names the border box
                // under border-box; the item's sizes here are content sizes.
                auto const content = [&](float written, float edges) {
                    return anonymous ? written : as_content_size(s, written, edges);
                };
                // The room a fit-content basis or bound would fit into: what
                // the line offers along the main axis, less this item's own
                // margins and edges. With no definite main size there is no
                // room to fit into and it reads as the content's widest.
                float const available_main = main_size
                    ? std::max(0.0f,
                          *main_size - item.margin_start - item.margin_end - item.edges_main)
                    : intrinsic.max;
                item.base = definite_basis
                    ? content(resolve(basis, main_size.value_or(0)), item.edges_main)
                    : intrinsic.max;
                if (!definite_basis && basis.is_content_size())
                    item.base = content_size_of(basis, intrinsic.min, intrinsic.max, available_main);
                // The minimum: min-width as written, else the automatic one —
                // the content's narrowest, no more than a written width or a
                // maximum.
                if (!anonymous && s.min_width.is_content_size()) {
                    item.minimum
                        = content_size_of(s.min_width, intrinsic.min, intrinsic.max, available_main);
                } else if (!anonymous && !s.min_width.is_auto()) {
                    item.minimum = content(resolve(s.min_width, content_width), item.edges_main);
                } else {
                    item.minimum = intrinsic.min;
                    if (!anonymous && s.width.is_content_size())
                        item.minimum = std::min(item.minimum,
                            content_size_of(s.width, intrinsic.min, intrinsic.max, available_main));
                    else if (!anonymous && !s.width.is_auto())
                        item.minimum = std::min(item.minimum,
                            content(resolve(s.width, content_width), item.edges_main));
                }
                if (!anonymous && s.max_width.is_content_size()) {
                    item.maximum
                        = content_size_of(s.max_width, intrinsic.min, intrinsic.max, available_main);
                    item.minimum = std::min(item.minimum, *item.maximum);
                } else if (!anonymous && !s.max_width.is_auto()) {
                    item.maximum = content(resolve(s.max_width, content_width), item.edges_main);
                    item.minimum = std::min(item.minimum, *item.maximum);
                }
                std::optional<float> const written_cross
                    = anonymous ? std::nullopt : definite_height_of(s, cross_size, item.edges_cross);
                item.cross_is_auto = !written_cross;
                if (written_cross)
                    item.cross = clamp_height(s, *written_cross, cross_size, item.edges_cross);
            } else {
                // A column's cross size is the width — stretched to the line,
                // as written, or shrink-to-fit — and the base is the height at
                // that width.
                float const available = std::max(0.0f,
                    content_width - item.margin_cross_start - item.margin_cross_end - item.edges_cross);
                auto const content = [&](float written, float edges) {
                    return anonymous ? written : as_content_size(s, written, edges);
                };
                if (!anonymous && s.width.is_content_size()) {
                    item.cross = content_size_of(s.width, intrinsic.min, intrinsic.max, available);
                    item.cross_is_auto = false;
                } else if (!anonymous && !s.width.is_auto()) {
                    item.cross = content(resolve(s.width, content_width), item.edges_cross);
                    item.cross_is_auto = false;
                } else {
                    bool const stretch = item_alignment(item, style) == AlignItems::Stretch;
                    item.cross = stretch ? available
                                         : std::min(std::max(intrinsic.min, available), intrinsic.max);
                    item.cross_is_auto = true;
                }
                if (!anonymous)
                    item.cross = clamp_width(s, item.cross, content_width, item.edges_cross);
                float const content_height
                    = measure_item_height(item, item.cross, content_width, list_depth);
                item.base = definite_basis
                    ? content(resolve(basis, main_size.value_or(0)), item.edges_main)
                    : content_height;
                if (!anonymous && s.min_height.kind == LengthPercent::Kind::Px)
                    item.minimum = content(s.min_height.value, item.edges_main);
                else
                    item.minimum = std::min(content_height, item.base);
                if (!anonymous && s.max_height.kind == LengthPercent::Kind::Px) {
                    item.maximum = content(s.max_height.value, item.edges_main);
                    item.minimum = std::min(item.minimum, *item.maximum);
                }
            }
            item.base = std::max(0.0f, item.base);
            item.hypothetical = std::max(item.base, item.minimum);
            if (item.maximum)
                item.hypothetical = std::min(item.hypothetical, *item.maximum);
        }

        // 3. Lines: everything on one, or as many as fit when wrapping.
        std::vector<std::vector<std::size_t>> lines;
        {
            std::vector<std::size_t> line;
            float used = 0;
            for (std::size_t i = 0; i < items.size(); ++i) {
                FlexItem const& item = items[i];
                float const outer
                    = item.margin_start + item.edges_main + item.hypothetical + item.margin_end;
                if (wrap && main_size && !line.empty() && used + main_gap + outer > *main_size + 0.01f) {
                    lines.push_back(std::move(line));
                    line.clear();
                    used = 0;
                }
                used += (line.empty() ? 0 : main_gap) + outer;
                line.push_back(i);
            }
            lines.push_back(std::move(line));
        }

        // 4. Flexible lengths: items grow into a line's free space or shrink
        // out of its overflow, each weighted by its factor (and its base
        // size when shrinking), never below its minimum — one that hits its
        // minimum is frozen there and the rest share what remains.
        for (std::vector<std::size_t> const& line : lines) {
            for (std::size_t const i : line) {
                items[i].main = items[i].hypothetical;
                items[i].frozen = false;
            }
            if (!main_size)
                continue;
            float const available = *main_size - main_gap * static_cast<float>(line.size() - 1);
            float sum_hypothetical = 0;
            for (std::size_t const i : line) {
                FlexItem const& item = items[i];
                sum_hypothetical
                    += item.margin_start + item.edges_main + item.hypothetical + item.margin_end;
            }
            bool const growing = sum_hypothetical < available;
            for (std::size_t const i : line) {
                FlexItem& item = items[i];
                float const factor = growing ? item.grow : item.shrink;
                if (factor <= 0 || (!growing && item.base < item.hypothetical))
                    item.frozen = true;
            }
            for (int round = 0; round < 64; ++round) {
                float free = available;
                float sum_factor = 0;
                float sum_scaled = 0;
                std::size_t open = 0;
                for (std::size_t const i : line) {
                    FlexItem const& item = items[i];
                    if (item.frozen) {
                        free -= item.outer_main();
                    } else {
                        free -= item.margin_start + item.edges_main + item.base + item.margin_end;
                        sum_factor += growing ? item.grow : item.shrink;
                        sum_scaled += item.shrink * item.base;
                        ++open;
                    }
                }
                if (open == 0)
                    break;
                std::vector<std::size_t> clamped;
                for (std::size_t const i : line) {
                    FlexItem& item = items[i];
                    if (item.frozen)
                        continue;
                    float target = item.base;
                    if (growing && free > 0 && sum_factor > 0)
                        target = item.base + free * item.grow / sum_factor;
                    else if (!growing && free < 0 && sum_scaled > 0)
                        target = item.base + free * (item.shrink * item.base) / sum_scaled;
                    float size = std::max(target, item.minimum);
                    if (item.maximum)
                        size = std::min(size, *item.maximum);
                    if (size > target + 0.001f || size < target - 0.001f)
                        clamped.push_back(i);
                    item.main = size;
                }
                if (clamped.empty())
                    break;
                for (std::size_t const i : clamped)
                    items[i].frozen = true;
            }
        }

        // 5. Cross sizes: each item's, then each line's.
        for (FlexItem& item : items) {
            if (horizontal && item.cross_is_auto)
                item.cross = measure_item_height(item, item.main, content_width, list_depth);
        }
        std::vector<float> line_cross(lines.size(), 0.0f);
        for (std::size_t l = 0; l < lines.size(); ++l) {
            for (std::size_t const i : lines[l])
                line_cross[l] = std::max(line_cross[l], items[i].outer_cross());
        }
        // A single-line container (flex-wrap: nowrap) with a definite cross
        // size gives its one line that size; a wrapping container whose
        // items happened to fit one line keeps that line its items' size.
        if (!wrap && cross_size)
            line_cross[0] = *cross_size;

        // 6. The lines across the container: align-content shares the free
        // cross space among them.
        float cross_cursor = 0;
        float line_gap = cross_gap;
        {
            float total = cross_gap * static_cast<float>(lines.size() - 1);
            for (float const size : line_cross)
                total += size;
            float const free = cross_size ? *cross_size - total : 0.0f;
            auto const n = static_cast<float>(lines.size());
            AlignContent align = style.align_content;
            if (free < 0
                && (align == AlignContent::SpaceBetween || align == AlignContent::SpaceAround
                    || align == AlignContent::SpaceEvenly))
                align = AlignContent::FlexStart;
            switch (align) {
            case AlignContent::Stretch:
                if (free > 0) {
                    for (float& size : line_cross)
                        size += free / n;
                }
                break;
            case AlignContent::FlexStart:
                break;
            case AlignContent::FlexEnd:
                cross_cursor = free;
                break;
            case AlignContent::Center:
                cross_cursor = free / 2;
                break;
            case AlignContent::SpaceBetween:
                if (lines.size() > 1)
                    line_gap += free / (n - 1);
                break;
            case AlignContent::SpaceAround:
                cross_cursor = free / (2 * n);
                line_gap += free / n;
                break;
            case AlignContent::SpaceEvenly:
                cross_cursor = free / (n + 1);
                line_gap += free / (n + 1);
                break;
            }
        }

        // 7. Placement: each line's items along the main axis as
        // justify-content says, each item across its line as its alignment says.
        float extent_main = 0; // what the items take along an indefinite main axis
        for (std::size_t step = 0; step < lines.size(); ++step) {
            std::size_t const l = wrap_reversed ? lines.size() - 1 - step : step;
            std::vector<std::size_t> const& line = lines[l];
            float const height_of_line = line_cross[l];
            auto const n = static_cast<float>(line.size());
            float used = main_gap * (n - 1);
            for (std::size_t const i : line)
                used += items[i].outer_main();
            extent_main = std::max(extent_main, used);
            float const free = main_size ? *main_size - used : 0.0f;
            JustifyContent justify = style.justify_content;
            // normal and stretch are flex-start on a flex line.
            if (justify == JustifyContent::Normal || justify == JustifyContent::Stretch)
                justify = JustifyContent::FlexStart;
            if (reversed) {
                if (justify == JustifyContent::FlexStart)
                    justify = JustifyContent::FlexEnd;
                else if (justify == JustifyContent::FlexEnd)
                    justify = JustifyContent::FlexStart;
            }
            if (free < 0
                && (justify == JustifyContent::SpaceBetween || justify == JustifyContent::SpaceAround
                    || justify == JustifyContent::SpaceEvenly))
                justify = JustifyContent::FlexStart;
            float main_cursor = 0;
            float between = main_gap;
            switch (justify) {
            case JustifyContent::Normal:
            case JustifyContent::Stretch:
            case JustifyContent::FlexStart:
                break;
            case JustifyContent::FlexEnd:
                main_cursor = free;
                break;
            case JustifyContent::Center:
                main_cursor = free / 2;
                break;
            case JustifyContent::SpaceBetween:
                if (line.size() > 1)
                    between += free / (n - 1);
                break;
            case JustifyContent::SpaceAround:
                main_cursor = free / (2 * n);
                between += free / n;
                break;
            case JustifyContent::SpaceEvenly:
                main_cursor = free / (n + 1);
                between += free / (n + 1);
                break;
            }
            for (std::size_t k = 0; k < line.size(); ++k) {
                FlexItem& item = items[reversed ? line[line.size() - 1 - k] : line[k]];
                float cross_pos = 0;
                switch (item_alignment(item, style)) {
                case AlignItems::Stretch:
                    if (item.cross_is_auto)
                        item.cross = std::max(0.0f,
                            height_of_line - item.margin_cross_start - item.margin_cross_end
                                - item.edges_cross);
                    break;
                case AlignItems::FlexEnd:
                    cross_pos = height_of_line - item.outer_cross();
                    break;
                case AlignItems::Center:
                    cross_pos = (height_of_line - item.outer_cross()) / 2;
                    break;
                case AlignItems::Auto:
                case AlignItems::Normal: // never here: item_alignment made it stretch
                case AlignItems::FlexStart:
                case AlignItems::Baseline: // as flex-start until baselines are gathered
                    break;
                }
                float const x = horizontal ? content_x + main_cursor
                                           : content_x + cross_cursor + cross_pos;
                float const y = horizontal ? content_y + cross_cursor + cross_pos
                                           : content_y + main_cursor;
                place_flex_item(item, horizontal, x, y, content_width, list_depth, fragment);
                main_cursor += item.outer_main() + between;
            }
            cross_cursor += height_of_line + line_gap;
        }

        // The content height: written, else the lines' extent for a row and
        // the items' for a column.
        if (definite_height)
            return *definite_height;
        if (horizontal) {
            float total = cross_gap * static_cast<float>(lines.size() - 1);
            for (float const size : line_cross)
                total += size;
            return total;
        }
        return extent_main;
    }
};

} // namespace

LayoutResult layout_document(dom::Document const& document, css::StyleMap const& styles,
    float viewport_width, ImageMap const* images, ControlStates const* controls, float viewport_height)
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

    Layouter layouter { styles, {}, images, controls, {}, {}, {}, 0, {}, {} };
    ComputedStyle const* html_style = layouter.style_of(*html);
    if (!html_style || html_style->display == Display::None)
        return result;

    // Background propagation (css-backgrounds-3 §2.11.2): html's whole
    // background paints the canvas, color and pictures alike. Only when
    // html has none at all — transparent and no image — does body's go
    // there instead (and body then skips its own color).
    auto const has_background = [](ComputedStyle const& s) {
        if (s.background_color.a != 0)
            return true;
        if (!s.background_images)
            return false;
        for (css::BackgroundImage const& image : *s.background_images) {
            if (!image.none())
                return true;
        }
        return false;
    };
    // A box with a picture but no color of its own leaves the canvas the
    // white it started as, and lays its picture over that.
    if (has_background(*html_style)) {
        if (html_style->background_color.a != 0)
            result.canvas_background = html_style->background_color;
    } else {
        for (dom::Node const* child : html->children()) {
            if (child->is_element() && static_cast<dom::Element const*>(child)->is_html("body")) {
                if (ComputedStyle const* body = layouter.style_of(
                        *static_cast<dom::Element const*>(child));
                    body && has_background(*body)) {
                    if (body->background_color.a != 0)
                        result.canvas_background = body->background_color;
                    result.canvas_background_from_body = true;
                }
            }
        }
    }

    // The root's formatting context holds the page's floats, and the page
    // reaches around them.
    FloatContext root_floats;
    // The initial containing block collects the absolutely positioned boxes
    // with no positioned ancestor; the fixed ones join them. It has the
    // viewport's size, or the page's height when no viewport height is known.
    layouter.absolute_stack.emplace_back();
    // The initial containing block has the viewport's height: the root's
    // percentage height resolves against it.
    BlockOptions root_options;
    if (viewport_height > 0)
        root_options.containing_height = viewport_height;
    result.root = layouter.layout_block(*html, *html_style, 0, 0, viewport_width, 0, root_floats, root_options);
    if (std::optional<float> const bottom = root_floats.lowest_bottom())
        result.root.height = std::max(result.root.height, *bottom - result.root.y);
    result.page_height = result.root.y + result.root.height
        + resolve(html_style->margin_bottom, viewport_width);
    {
        std::vector<Layouter::OutOfFlow> boxes = std::move(layouter.absolute_stack.back());
        layouter.absolute_stack.pop_back();
        boxes.insert(boxes.end(), layouter.fixed_boxes.begin(), layouter.fixed_boxes.end());
        layouter.fixed_boxes.clear();
        if (!boxes.empty()) {
            float const icb_height = viewport_height > 0 ? viewport_height : result.page_height;
            layouter.place_out_of_flow(boxes, 0, 0, viewport_width, icb_height, result.root, 0);
            for (Fragment const& child : result.root.children) {
                if (child.positioned)
                    result.page_height = std::max(result.page_height, child.y + child.height);
            }
        }
    }
    // The anonymous boxes' styles go with the fragments that point at them.
    result.owned_styles = std::move(layouter.owned_styles);
    return result;
}

}
