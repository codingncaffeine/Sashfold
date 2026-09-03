#pragma once

// Computed style for the first property set: enough to lay out and paint the
// reader web's block/inline content. Lengths that layout must resolve against
// a containing block stay as LengthPercent; everything else is resolved here.

#include "core/Bitmap.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::css {

struct ComponentValue;

// Custom properties (--name) by name, each the component values it was
// written as, with any var() inside them already substituted.
using CustomProperties = std::unordered_map<std::string, std::vector<ComponentValue>>;

// How a list marker, and a counter() in the content property, writes a
// number. The glyph kinds ignore the number; the alphabetic and additive
// ones fall back to decimal outside the range they can spell.
enum class ListStyleType : std::uint8_t {
    Disc,
    Circle,
    Square,
    Decimal,
    DecimalLeadingZero,
    LowerRoman,
    UpperRoman,
    LowerAlpha, // lower-latin is the same list
    UpperAlpha,
    LowerGreek,
    Armenian,
    Georgian,
    None,
};

// Where a list item's marker sits (CSS 2.1 §12.5.1): outside the principal
// box, hanging in the margin, or inside it as the first thing on the item's
// first line, which is what a list with its padding zeroed needs if the
// marker is to be seen at all.
enum class ListStylePosition : std::uint8_t {
    Outside,
    Inside,
};

// A counter's value written out the way a list-style-type spells it: the
// decimal digits, roman numerals, letters of an alphabet, or one of the
// three marker glyphs (which say nothing about the number). Outside the
// range a system can spell — roman past 3999, an additive system past its
// largest numeral, anything below one where the alphabet starts — the
// value comes back in decimal, as css-counter-styles-3 §3 asks.
std::string format_counter(int value, ListStyleType style);

// One piece of the content property's value.
struct ContentItem {
    enum class Kind : std::uint8_t {
        String, // the text itself
        Attr, // the originating element's attribute named by text
        OpenQuote,
        CloseQuote,
        NoOpenQuote, // the depth changes, nothing is inserted
        NoCloseQuote,
        Counter, // counter(name, style): the innermost instance in scope
        Counters, // counters(name, separator, style): every instance, outermost first
    };
    Kind kind = Kind::String;
    std::string text; // String: the text; Attr, Counter, Counters: the name
    std::string fallback; // Attr: what stands in when the attribute is absent;
                          // Counters: what goes between the values
    ListStyleType style = ListStyleType::Decimal; // Counter, Counters: how a value is written
};

// One counter operation: the counter's name and the integer beside it.
struct CounterOp {
    std::string name;
    int value = 0;

    bool operator==(CounterOp const&) const = default;
};

// What counter-reset, counter-increment and counter-set name, in the order
// they were written. An empty list is `none`.
using CounterOps = std::vector<CounterOp>;

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
        Calc, // value holds the pixels, percent the percentage: calc(100% - 20px)
    };
    Kind kind = Kind::Px;
    float value = 0;
    float percent = 0; // Calc only

    static constexpr LengthPercent auto_value() { return { Kind::Auto, 0, 0 }; }
    static constexpr LengthPercent px(float value) { return { Kind::Px, value, 0 }; }
    static constexpr LengthPercent percent_of(float value) { return { Kind::Percent, value, 0 }; }
    static constexpr LengthPercent calc(float px, float percent)
    {
        if (percent == 0)
            return { Kind::Px, px, 0 };
        if (px == 0)
            return { Kind::Percent, percent, 0 };
        return { Kind::Calc, px, percent };
    }
    bool is_auto() const { return kind == Kind::Auto; }
};

enum class Display : std::uint8_t {
    Block,
    Inline,
    ListItem,
    FlowRoot, // a block whose contents form their own formatting context
    Flex, // a block-level flex container
    Grid, // a block-level grid container
    // The inline-level counterparts: one atomic box on its line, laid out
    // inside as the block-level kind is, sized to its contents, its
    // baseline the line's.
    InlineBlock,
    InlineFlex,
    InlineGrid,
    // Tables (CSS 2.1 §17): the table box (block-level, or inline-level as
    // inline-table) and the table-internal boxes; a table-internal box
    // outside a table gets an anonymous table around it, and a column or
    // column group renders nothing of its own but a background.
    Table,
    InlineTable,
    TableRowGroup,
    TableHeaderGroup,
    TableFooterGroup,
    TableRow,
    TableCell,
    TableCaption,
    TableColumnGroup,
    TableColumn,
    None,
};

// Whether a display is table-internal: a proper child of a table, or a
// cell, which lives in a row.
constexpr bool is_table_internal(Display display)
{
    switch (display) {
    case Display::TableRowGroup:
    case Display::TableHeaderGroup:
    case Display::TableFooterGroup:
    case Display::TableRow:
    case Display::TableCell:
    case Display::TableCaption:
    case Display::TableColumnGroup:
    case Display::TableColumn:
        return true;
    default:
        return false;
    }
}

constexpr bool is_table_display(Display display)
{
    return display == Display::Table || display == Display::InlineTable;
}

// Whether a display makes a block-level box: one that takes lines of its own
// among its siblings rather than sitting on one of theirs.
constexpr bool is_block_level_display(Display display)
{
    return display == Display::Block || display == Display::ListItem
        || display == Display::FlowRoot || display == Display::Flex || display == Display::Grid
        || display == Display::Table;
}

// Whether a display makes a block container: a box that holds either lines
// of text or block-level boxes of its own. A flex or grid container holds
// items instead, and a table box holds rows, so neither has a first line —
// which is why ::first-line and ::first-letter pass them by.
constexpr bool is_block_container_display(Display display)
{
    return display == Display::Block || display == Display::ListItem
        || display == Display::FlowRoot || display == Display::InlineBlock
        || display == Display::TableCell || display == Display::TableCaption;
}

enum class BorderCollapse : std::uint8_t {
    Separate,
    Collapse,
};

enum class CaptionSide : std::uint8_t {
    Top,
    Bottom,
};

enum class EmptyCells : std::uint8_t {
    Show,
    Hide,
};

enum class TableLayout : std::uint8_t {
    Auto,
    Fixed,
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

// How a line's free main-axis space is shared out — and, for a grid, how
// the columns share the container's free width. Normal is the initial
// value: flex-start on a flex line, stretch (the auto tracks grow) in a grid.
enum class JustifyContent : std::uint8_t {
    Normal,
    FlexStart,
    FlexEnd,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
    Stretch,
};

// How items sit across their line, or in their grid area's axis; Auto
// (align-self and justify-self only) defers to the container's align-items
// or justify-items. Normal is the initial value: stretch, except that a
// replaced grid item keeps its own size (start).
enum class AlignItems : std::uint8_t {
    Auto,
    Normal,
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

enum class Position : std::uint8_t {
    Static,
    Relative, // laid out in flow, then shifted by its offsets
    Absolute, // out of flow, placed in the nearest positioned ancestor's padding box
    Fixed, // out of flow, placed in the viewport
    Sticky, // in flow; treated as relative until scroll containers land
};

enum class Clear : std::uint8_t {
    None,
    Left,
    Right,
    Both,
};

// Which box a written width or height names. Layout sizes content boxes, so
// under border-box the padding and the borders come off the written value.
enum class BoxSizing : std::uint8_t {
    ContentBox,
    BorderBox,
};

enum class Overflow : std::uint8_t {
    Visible,
    // clip: the box clips what it paints to its padding box and nothing
    // scrolls. It is kept apart from the rest because it alone lets the
    // other axis stay visible — and a box that clips only one axis does
    // not round what it holds along its corners.
    Clip,
    // hidden, auto and scroll: the box contains its floats, keeps clear
    // of others, and clips what it paints to its padding box; scrolling
    // arrives with scroll containers.
    Hidden,
};

enum class Visibility : std::uint8_t {
    Visible,
    Hidden, // the box keeps its room and paints nothing (collapse counts as hidden)
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

// How the last line of a block, and any line right before a forced break,
// is aligned (css-text-3 §7.2). `Auto` takes the block's own text-align,
// except that justified text starts its last line at the start edge —
// which is what `justify-all` exists to say otherwise about. `start` and
// `end` are read as left and right: there is no `direction` yet.
enum class TextAlignLast : std::uint8_t {
    Auto,
    Left,
    Right,
    Center,
    Justify,
};

// What a justified line is allowed to stretch (css-text-3 §7.3). `none`
// turns justification off altogether; the rest are read, but the only
// method written is the one between words, so `inter-character` — and
// `distribute`, its old name — stretch the spaces like the others.
enum class TextJustify : std::uint8_t {
    Auto,
    None,
    InterWord,
    InterCharacter,
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
    // hidden: nothing is drawn, as with none — but in the collapsing
    // border model it beats every other border at its edge, and it is the
    // only way to take one away.
    Hidden,
    Solid, // every visible style draws solid for now
};

// css-text-3 §2.1: what the rendered text is turned into, without touching
// the document. `full-width` and `full-size-kana` are not written.
enum class TextTransform : std::uint8_t {
    None,
    Capitalize,
    Uppercase,
    Lowercase,
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

    bool operator==(LineHeight const&) const = default;
};

// How an inline-level box sits on its line: on the baseline; raised or
// lowered by a keyword or a length (a percentage of the box's own line
// height); or at the line box's top or bottom.
struct VerticalAlign {
    enum class Kind : std::uint8_t {
        Baseline,
        Sub,
        Super,
        TextTop,
        TextBottom,
        Middle,
        Top,
        Bottom,
        Length, // raised by `offset`; negative lowers
    };
    Kind kind = Kind::Baseline;
    LengthPercent offset = LengthPercent::px(0);
};

// --- Grid ---------------------------------------------------------------------

// One side of a grid track's sizing function (css-grid-1 §7.2.3): a
// length or percentage (of the container's content box in that axis; auto
// when that is indefinite), a flexible fr factor, or a content-based
// keyword.
struct TrackBreadth {
    enum class Kind : std::uint8_t {
        Length,
        Flex,
        Auto,
        MinContent,
        MaxContent,
    };
    Kind kind = Kind::Auto;
    LengthPercent length = LengthPercent::px(0); // Length
    float fr = 0; // Flex

    bool is_intrinsic() const
    {
        return kind == Kind::Auto || kind == Kind::MinContent || kind == Kind::MaxContent;
    }
    bool is_flexible() const { return kind == Kind::Flex; }
    bool is_fixed() const { return kind == Kind::Length; }
};

// A track's sizing function: a minimum and a maximum. One value stands for
// both; a flex factor alone has an auto minimum; fit-content(l) is
// minmax(auto, max-content) capped at l.
struct TrackSize {
    TrackBreadth min;
    TrackBreadth max;
    std::optional<LengthPercent> fit_content; // the cap, when the track is fit-content()
};

// grid-template-columns and -rows: the explicit tracks with their line
// names — the names before each track, and after the last — and the
// auto-repeat when there is one: repeat(auto-fill | auto-fit, ...) is
// expanded at layout to as many repetitions as fit the container.
struct GridTrackList {
    struct Track {
        std::vector<std::string> names; // the line names before this track
        TrackSize size;
    };
    std::vector<Track> tracks; // integer repeats already expanded
    std::vector<std::string> trailing_names; // after the last track
    enum class AutoRepeat : std::uint8_t {
        None,
        Fill,
        Fit, // like fill, but the empty repetitions collapse to nothing
    };
    AutoRepeat auto_repeat = AutoRepeat::None;
    std::size_t auto_repeat_at = 0; // the repetitions go before this track index
    std::vector<std::string> auto_repeat_leading_names; // before the first repetition
    std::vector<Track> auto_repeat_tracks; // one repetition, names as written
    std::vector<std::string> auto_repeat_trailing_names; // after each repetition

    bool empty() const { return tracks.empty() && auto_repeat == AutoRepeat::None; }
};

// grid-template-areas: the named areas, each a rectangle of cells in
// 1-based lines with the end exclusive, and the grid the strings span.
struct GridAreas {
    struct Area {
        std::string name;
        int row_start = 1;
        int row_end = 2;
        int column_start = 1;
        int column_end = 2;
    };
    std::vector<Area> areas;
    int rows = 0;
    int columns = 0;
};

// grid-row-start and its three siblings (css-grid-1 §8.3): auto; a line by
// number (negative counts back from the last explicit line), the nth line
// with a name, or a name alone (its area's edge, else the first line so
// named); or a span of so many tracks, or up to the nth line with a name.
struct GridLine {
    enum class Kind : std::uint8_t {
        Auto,
        Line, // number (nonzero), and the name when one was written
        Name, // a name alone
        Span, // number ≥ 1, and the name when one was written
    };
    Kind kind = Kind::Auto;
    int number = 0;
    std::string name;

    bool is_auto() const { return kind == Kind::Auto; }
};

enum class GridAutoFlow : std::uint8_t {
    Row,
    Column,
    RowDense,
    ColumnDense,
};

// --- Backgrounds ----------------------------------------------------------------

// A color stop of a gradient: its color and, when written, where it sits
// along the gradient line.
struct GradientStop {
    Color color;
    std::optional<LengthPercent> position;
};

// A gradient image (css-images-3): linear along an angle or toward a side
// or corner, or radial from a point; its color stops; repeating or not.
struct Gradient {
    enum class Kind : std::uint8_t {
        Linear,
        Radial,
    };
    Kind kind = Kind::Linear;
    bool repeating = false;
    // Linear: the angle in degrees (0 points up, 90 right; 180, down, is
    // the default), or the corner the line points at.
    float angle = 180;
    enum class Corner : std::uint8_t {
        None,
        TopLeft,
        TopRight,
        BottomRight,
        BottomLeft,
    };
    Corner corner = Corner::None;
    // Radial: the shape, how far it reaches, and its center.
    enum class Shape : std::uint8_t {
        Ellipse,
        Circle,
    };
    Shape shape = Shape::Ellipse;
    enum class Extent : std::uint8_t {
        FarthestCorner,
        ClosestSide,
        ClosestCorner,
        FarthestSide,
    };
    Extent extent = Extent::FarthestCorner;
    LengthPercent center_x = LengthPercent::percent_of(50);
    LengthPercent center_y = LengthPercent::percent_of(50);
    std::vector<GradientStop> stops;
};

// One background image: a picture by its resolved URL, or a gradient;
// neither for none.
struct BackgroundImage {
    std::string url;
    std::shared_ptr<Gradient const> gradient;

    bool none() const { return url.empty() && !gradient; }
};

enum class BackgroundRepeat : std::uint8_t {
    Repeat,
    // As many whole tiles as fit, the room left over shared out between
    // them so the first and last touch the edges.
    Space,
    // As many tiles as fit, the image stretched in that direction so a
    // whole number of them fills the area exactly.
    Round,
    NoRepeat,
};

struct BackgroundRepeatPair {
    BackgroundRepeat x = BackgroundRepeat::Repeat;
    BackgroundRepeat y = BackgroundRepeat::Repeat;
};

enum class BackgroundBox : std::uint8_t {
    BorderBox,
    PaddingBox,
    ContentBox,
};

struct BackgroundSize {
    enum class Kind : std::uint8_t {
        Auto,
        Cover,
        Contain,
        Lengths, // width and height, either auto
    };
    Kind kind = Kind::Auto;
    LengthPercent width = LengthPercent::auto_value();
    LengthPercent height = LengthPercent::auto_value();
};

// Where an image's box sits in the positioning area: percentages place
// the image's point at the area's same point.
struct BackgroundPosition {
    LengthPercent x = LengthPercent::percent_of(0);
    LengthPercent y = LengthPercent::percent_of(0);
};

struct BorderSide {
    float width = 0; // px, already zeroed when style is None
    BorderStyle style = BorderStyle::None;
    Color color; // defaults to currentColor at application time
    // The color was never written: it is currentColor, the element's own
    // color — and inherits as that keyword, not as the resolved color.
    bool current_color = true;
};

// One corner's two radii: the horizontal one measured along the border
// box's width, the vertical one along its height, so a percentage in
// each resolves against that side. Either at zero squares the corner.
struct CornerRadius {
    LengthPercent x = LengthPercent::px(0);
    LengthPercent y = LengthPercent::px(0);

    bool is_zero() const
    {
        auto const nothing = [](LengthPercent const& length) {
            return length.value == 0 && length.percent == 0;
        };
        return nothing(x) || nothing(y);
    }
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
    // Which box the six sizes above name.
    BoxSizing box_sizing = BoxSizing::ContentBox;
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
    // The four corners' radii, clockwise from the top left.
    CornerRadius border_top_left_radius;
    CornerRadius border_top_right_radius;
    CornerRadius border_bottom_right_radius;
    CornerRadius border_bottom_left_radius;
    Float floating = Float::None;
    Clear clear = Clear::None;
    // Whether the box clips at all — either axis is enough, and layout
    // asks only this much — and what each axis was left holding.
    Overflow overflow = Overflow::Visible;
    Overflow overflow_x = Overflow::Visible;
    Overflow overflow_y = Overflow::Visible;
    // overflow applies to block containers, not to table rows and row
    // groups (they lay out as blocks until tables land, but never clip).
    bool overflow_applies = true;

    // Tables. The border model, the gutters between cells in the separated
    // model, the caption's side and empty cells' painting inherit; the
    // layout algorithm does not.
    BorderCollapse border_collapse = BorderCollapse::Separate;
    LengthPercent border_spacing_horizontal = LengthPercent::px(0);
    LengthPercent border_spacing_vertical = LengthPercent::px(0);
    CaptionSide caption_side = CaptionSide::Top;
    EmptyCells empty_cells = EmptyCells::Show;
    TableLayout table_layout = TableLayout::Auto;

    // Positioning: the scheme, the four offsets (auto = not written), and
    // z-index (nullopt = auto).
    Position position = Position::Static;
    LengthPercent top = LengthPercent::auto_value();
    LengthPercent right = LengthPercent::auto_value();
    LengthPercent bottom = LengthPercent::auto_value();
    LengthPercent left = LengthPercent::auto_value();
    std::optional<int> z_index;
    // An absolutely positioned box that was inline-level before §9.7 made
    // it a block: its static position is where the inline box would have
    // begun.
    bool blockified = false;

    // Hiding: visibility is inherited and keeps the box's room; opacity is
    // not, and at zero hides the box and everything in it (between zero and
    // one the box paints as if opaque until group compositing lands).
    Visibility visibility = Visibility::Visible;
    float opacity = 1;

    // Transforms: the translation parts of `transform` and `translate`,
    // percentages of the box's own size, applied after layout without
    // touching the flow; any transform makes the box a stacking context.
    // Rotations, scales and skews are not drawn yet.
    LengthPercent translate_x = LengthPercent::px(0);
    LengthPercent translate_y = LengthPercent::px(0);
    bool transformed = false;

    bool positioned() const { return position != Position::Static; }
    bool out_of_flow() const { return position == Position::Absolute || position == Position::Fixed; }
    bool hidden() const { return visibility == Visibility::Hidden; }
    // Any corner curved: the box is painted as a rounded shape. A table
    // in the collapsing border model has square corners whatever its
    // radii say, and so does everything inside it — the borders it
    // shares with its neighbours have nowhere to curve to.
    bool rounded() const
    {
        if (border_collapse == BorderCollapse::Collapse
            && (is_table_display(display) || is_table_internal(display)))
            return false;
        return !border_top_left_radius.is_zero() || !border_top_right_radius.is_zero()
            || !border_bottom_right_radius.is_zero() || !border_bottom_left_radius.is_zero();
    }

    // Flex and grid containers and their items.
    FlexDirection flex_direction = FlexDirection::Row;
    FlexWrap flex_wrap = FlexWrap::NoWrap;
    JustifyContent justify_content = JustifyContent::Normal;
    AlignItems align_items = AlignItems::Normal;
    AlignItems align_self = AlignItems::Auto;
    AlignContent align_content = AlignContent::Stretch;
    // The inline-axis counterparts of align-items and align-self: how a
    // grid item sits across its area's width.
    AlignItems justify_items = AlignItems::Normal;
    AlignItems justify_self = AlignItems::Auto;
    float flex_grow = 0;
    float flex_shrink = 1;
    LengthPercent flex_basis = LengthPercent::auto_value();
    // The gutters; a percentage is of the container's content box in that
    // axis, zero while that is indefinite.
    LengthPercent row_gap = LengthPercent::px(0);
    LengthPercent column_gap = LengthPercent::px(0);
    int order = 0;

    // Grid containers: the explicit tracks in each axis (null: none), the
    // named areas (null: none), the sizes of implicit tracks (null: auto),
    // and how auto-placed items flow. Shared, since they are set on one
    // element and never inherited.
    std::shared_ptr<GridTrackList const> grid_template_columns;
    std::shared_ptr<GridTrackList const> grid_template_rows;
    std::shared_ptr<GridAreas const> grid_template_areas;
    std::shared_ptr<std::vector<TrackSize> const> grid_auto_columns;
    std::shared_ptr<std::vector<TrackSize> const> grid_auto_rows;
    GridAutoFlow grid_auto_flow = GridAutoFlow::Row;
    // Grid items: where the item goes.
    GridLine grid_row_start;
    GridLine grid_row_end;
    GridLine grid_column_start;
    GridLine grid_column_end;

    // Text and inheritance-carried properties.
    Color color = Color::rgb(0, 0, 0);
    Color background_color = Color::rgba(0, 0, 0, 0);
    // The background images (css-backgrounds-3), the first nearest the
    // viewer, and the lists the other background properties give them —
    // each repeated along the images when shorter. A null list is the
    // initial value: no image; repeat; 0% 0%; auto; padding-box; border-box.
    std::shared_ptr<std::vector<BackgroundImage> const> background_images;
    std::shared_ptr<std::vector<BackgroundRepeatPair> const> background_repeats;
    std::shared_ptr<std::vector<BackgroundPosition> const> background_positions;
    std::shared_ptr<std::vector<BackgroundSize> const> background_sizes;
    std::shared_ptr<std::vector<BackgroundBox> const> background_origins;
    std::shared_ptr<std::vector<BackgroundBox> const> background_clips;
    float font_size = 16;
    int font_weight = 400; // 700+ paints bold
    FontStyle font_style = FontStyle::Normal;
    // The font-family list as written, generic names included; null is the
    // initial value (the default serif face). Shared down the tree.
    std::shared_ptr<std::vector<std::string> const> font_family;
    LineHeight line_height;
    VerticalAlign vertical_align; // not inherited
    TextAlign text_align = TextAlign::Left;
    TextAlignLast text_align_last = TextAlignLast::Auto;
    TextJustify text_justify = TextJustify::Auto;
    WhiteSpace white_space = WhiteSpace::Normal;
    TextTransform text_transform = TextTransform::None;
    ListStyleType list_style_type = ListStyleType::Disc;
    ListStylePosition list_style_position = ListStylePosition::Outside;
    // What this list item's marker counts to: the `list-item` counter as it
    // stood once this box had its turn (css-lists-3 §4.2 increments it by one
    // on every `display: list-item` box). Meaningless on anything else.
    int list_item_value = 0;
    TextDecorationLine text_decoration = TextDecorationLine::None;
    // The spacing properties, all inherited: extra room after each character
    // and after each word separator (`normal` is none of either), and how far
    // the first line of a block starts in — a percentage of the containing
    // block's width, resolved at layout.
    float letter_spacing = 0;
    float word_spacing = 0;
    LengthPercent text_indent = LengthPercent::px(0);

    // Generated content: what a ::before or ::after box shows (meaningful
    // on those boxes' styles), the quotation marks in force (inherited),
    // and — on an element's own style — its generated boxes, when it has any.
    Content content;
    std::shared_ptr<QuotePairs const> quotes;
    std::shared_ptr<GeneratedContent const> generated;
    // The counter operations this box performs, in the order they were
    // written; null is none. A reset makes an instance of its own, an
    // increment adds to the innermost instance in scope, a set writes it.
    // Shared, because most boxes name no counter at all.
    std::shared_ptr<CounterOps const> counter_reset;
    std::shared_ptr<CounterOps const> counter_increment;
    std::shared_ptr<CounterOps const> counter_set;
    // What ::first-letter asks for, when a rule addresses it: the style the
    // first letter of this block's first line wears, cascaded from the
    // element. Null when nothing addresses it.
    std::shared_ptr<ComputedStyle const> first_letter;
    // The custom properties in force (--name → its component values, as
    // written, var() references already substituted): inherited, shared
    // with the parent until an element declares one of its own.
    std::shared_ptr<CustomProperties const> custom;

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
