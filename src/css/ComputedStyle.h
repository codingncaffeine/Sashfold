#pragma once

// Computed style for the first property set: enough to lay out and paint the
// reader web's block/inline content. Lengths that layout must resolve against
// a containing block stay as LengthPercent; everything else is resolved here.

#include "core/Bitmap.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::css {

// One piece of the content property's value.
struct ContentItem {
    enum class Kind : std::uint8_t {
        String, // the text itself
        Attr, // the originating element's attribute named by text
        OpenQuote,
        CloseQuote,
        NoOpenQuote, // the depth changes, nothing is inserted
        NoCloseQuote,
    };
    Kind kind = Kind::String;
    std::string text;
    std::string fallback; // Attr: what stands in when the attribute is absent
};

// The content property: normal (a ::before or ::after generates no box),
// none (the same), or the items a generated box shows.
struct Content {
    enum class Kind : std::uint8_t {
        Normal,
        None,
        Items,
    };
    Kind kind = Kind::Normal;
    std::vector<ContentItem> items;
};

// The pairs of quotation marks the quotes property names, outermost first;
// null is auto (the language's marks), an empty list is none.
using QuotePairs = std::vector<std::pair<std::string, std::string>>;

struct GeneratedContent;

struct LengthPercent {
    enum class Kind : std::uint8_t {
        Auto,
        Px,
        Percent, // value holds 0-100
    };
    Kind kind = Kind::Px;
    float value = 0;

    static constexpr LengthPercent auto_value() { return { Kind::Auto, 0 }; }
    static constexpr LengthPercent px(float value) { return { Kind::Px, value }; }
    static constexpr LengthPercent percent(float value) { return { Kind::Percent, value }; }
    bool is_auto() const { return kind == Kind::Auto; }
};

enum class Display : std::uint8_t {
    Block,
    Inline,
    ListItem,
    FlowRoot, // a block whose contents form their own formatting context
    Flex, // a block-level flex container
    Grid, // a grid container: a block-level formatting context root until grid lands
    // table-column and table-column-group: a column box renders none of
    // its own margins, padding, backgrounds or content in any engine; until
    // tables land it is an inline-level box that paints nothing, and a
    // generated box with this display is not generated at all.
    TableColumn,
    None,
};

enum class FlexDirection : std::uint8_t {
    Row,
    RowReverse,
    Column,
    ColumnReverse,
};

enum class FlexWrap : std::uint8_t {
    NoWrap,
    Wrap,
    WrapReverse,
};

// How a line's free main-axis space is shared out.
enum class JustifyContent : std::uint8_t {
    FlexStart,
    FlexEnd,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

// How items sit across their line; Auto (align-self only) defers to the
// container's align-items.
enum class AlignItems : std::uint8_t {
    Auto,
    Stretch,
    FlexStart,
    FlexEnd,
    Center,
    Baseline,
};

// How a multi-line container's lines share its free cross-axis space.
enum class AlignContent : std::uint8_t {
    Stretch,
    FlexStart,
    FlexEnd,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

enum class Float : std::uint8_t {
    None,
    Left,
    Right,
};

enum class Clear : std::uint8_t {
    None,
    Left,
    Right,
    Both,
};

enum class Overflow : std::uint8_t {
    Visible,
    // hidden, clip, auto and scroll: the box contains its floats and keeps
    // clear of others; clipping and scrolling arrive with scroll containers.
    Hidden,
};

enum class FontStyle : std::uint8_t {
    Normal,
    Italic,
};

enum class TextAlign : std::uint8_t {
    Left,
    Right,
    Center,
    Justify,
};

enum class WhiteSpace : std::uint8_t {
    Normal,
    Pre,
    NoWrap,
    PreWrap,
    PreLine,
};

enum class BorderStyle : std::uint8_t {
    None,
    Solid, // every visible style draws solid for now
};

enum class ListStyleType : std::uint8_t {
    Disc,
    Circle,
    Square,
    Decimal,
    None,
};

enum class TextDecorationLine : std::uint8_t {
    None,
    Underline,
    LineThrough,
};

struct LineHeight {
    enum class Kind : std::uint8_t {
        Normal,
        Number, // multiplies font-size
        Px,
    };
    Kind kind = Kind::Normal;
    float value = 0;
};

struct BorderSide {
    float width = 0; // px, already zeroed when style is None
    BorderStyle style = BorderStyle::None;
    Color color; // defaults to currentColor at application time
};

struct ComputedStyle {
    // Box model.
    Display display = Display::Inline;
    LengthPercent width = LengthPercent::auto_value();
    LengthPercent height = LengthPercent::auto_value();
    // The bounds on the used size: auto means none for a maximum, and the
    // automatic minimum (zero, or a flex item's content) for a minimum.
    LengthPercent min_width = LengthPercent::auto_value();
    LengthPercent max_width = LengthPercent::auto_value();
    LengthPercent min_height = LengthPercent::auto_value();
    LengthPercent max_height = LengthPercent::auto_value();
    LengthPercent margin_top = LengthPercent::px(0);
    LengthPercent margin_right = LengthPercent::px(0);
    LengthPercent margin_bottom = LengthPercent::px(0);
    LengthPercent margin_left = LengthPercent::px(0);
    LengthPercent padding_top = LengthPercent::px(0);
    LengthPercent padding_right = LengthPercent::px(0);
    LengthPercent padding_bottom = LengthPercent::px(0);
    LengthPercent padding_left = LengthPercent::px(0);
    BorderSide border_top;
    BorderSide border_right;
    BorderSide border_bottom;
    BorderSide border_left;
    Float floating = Float::None;
    Clear clear = Clear::None;
    Overflow overflow = Overflow::Visible;

    // Flex containers and their items.
    FlexDirection flex_direction = FlexDirection::Row;
    FlexWrap flex_wrap = FlexWrap::NoWrap;
    JustifyContent justify_content = JustifyContent::FlexStart;
    AlignItems align_items = AlignItems::Stretch;
    AlignItems align_self = AlignItems::Auto;
    AlignContent align_content = AlignContent::Stretch;
    float flex_grow = 0;
    float flex_shrink = 1;
    LengthPercent flex_basis = LengthPercent::auto_value();
    float row_gap = 0; // px; percentages wait for their base
    float column_gap = 0;
    int order = 0;

    // Text and inheritance-carried properties.
    Color color = Color::rgb(0, 0, 0);
    Color background_color = Color::rgba(0, 0, 0, 0);
    float font_size = 16;
    int font_weight = 400; // 700+ paints bold
    FontStyle font_style = FontStyle::Normal;
    // The font-family list as written, generic names included; null is the
    // initial value (the default serif face). Shared down the tree.
    std::shared_ptr<std::vector<std::string> const> font_family;
    LineHeight line_height;
    TextAlign text_align = TextAlign::Left;
    WhiteSpace white_space = WhiteSpace::Normal;
    ListStyleType list_style_type = ListStyleType::Disc;
    TextDecorationLine text_decoration = TextDecorationLine::None;

    // Generated content: what a ::before or ::after box shows (meaningful
    // on those boxes' styles), the quotation marks in force (inherited),
    // and — on an element's own style — its generated boxes, when it has any.
    Content content;
    std::shared_ptr<QuotePairs const> quotes;
    std::shared_ptr<GeneratedContent const> generated;

    bool bold() const { return font_weight >= 600; }
    float line_height_px() const
    {
        switch (line_height.kind) {
        case LineHeight::Kind::Normal: return font_size * 1.2f;
        case LineHeight::Kind::Number: return font_size * line_height.value;
        case LineHeight::Kind::Px: return line_height.value;
        }
        return font_size * 1.2f;
    }
};

// A ::before or ::after box: its computed style (inheriting from the
// originating element) and the text it shows, resolved in tree order —
// attributes read, quotation marks chosen by the nesting depth.
struct GeneratedBox {
    ComputedStyle style;
    std::string text; // UTF-8
};

struct GeneratedContent {
    std::optional<GeneratedBox> before;
    std::optional<GeneratedBox> after;
};

}
