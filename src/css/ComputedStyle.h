#pragma once

// Computed style for the first property set: enough to lay out and paint the
// reader web's block/inline content. Lengths that layout must resolve against
// a containing block stay as LengthPercent; everything else is resolved here.

#include "core/Bitmap.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sashfold::css {

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
    None,
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

}
