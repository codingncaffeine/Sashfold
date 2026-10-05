#include "css/Interpolation.h"

#include "css/Parser.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace sashfold::css {

namespace {

using S = ComputedStyle;
using V = AnimatedValue;

// What a written value is held to: the range the property's grammar has
// (an easing may overshoot past either end), and whether it is a whole
// number.
enum Range : unsigned char {
    Any,
    NonNegative,
    Unit, // [0, 1]
    AtLeastOne,
    Weight, // [1, 1000]
};

}

struct AnimatableProperty {
    std::string_view name;
    bool inherited;
    V::Kind kind;
    Range range;
    bool integer;
    std::optional<V> (*read)(S const&);
    void (*write)(V const&, S&, unsigned&);
};

namespace {

double clamp_to(Range range, double value)
{
    switch (range) {
    case Any: return value;
    case NonNegative: return std::max(0.0, value);
    case Unit: return std::clamp(value, 0.0, 1.0);
    case AtLeastOne: return std::max(1.0, value);
    case Weight: return std::clamp(value, 1.0, 1000.0);
    }
    return value;
}

double round_half_up(double value) { return std::floor(value + 0.5); }

V number(double n)
{
    V value;
    value.kind = V::Kind::Number;
    value.number = n;
    return value;
}

std::optional<V> length(LengthPercent const& l)
{
    V value;
    value.kind = V::Kind::Length;
    switch (l.kind) {
    case LengthPercent::Kind::Px: value.px = static_cast<double>(l.value); return value;
    case LengthPercent::Kind::Percent: value.percent = static_cast<double>(l.value); return value;
    case LengthPercent::Kind::Calc:
        value.px = static_cast<double>(l.value);
        value.percent = static_cast<double>(l.percent);
        return value;
    default: return std::nullopt; // auto and the content sizes: discrete
    }
}

LengthPercent to_length(double px, double percent, Range range)
{
    // A length alone or a percentage alone is held to the range; a mix is
    // a calc() whose sign layout settles.
    if (percent == 0)
        px = clamp_to(range == Unit ? Any : range, px);
    if (px == 0)
        percent = clamp_to(range == Unit ? Any : range, percent);
    return LengthPercent::calc(static_cast<float>(px), static_cast<float>(percent));
}

std::optional<V> pair(LengthPercent const& x, LengthPercent const& y)
{
    std::optional<V> const a = length(x);
    std::optional<V> const b = length(y);
    if (!a || !b)
        return std::nullopt;
    V value;
    value.kind = V::Kind::Lengths;
    value.lengths = { { a->px, a->percent }, { b->px, b->percent } };
    return value;
}

std::optional<V> color(Color const& c)
{
    V value;
    value.kind = V::Kind::Color;
    value.r = c.r / 255.0;
    value.g = c.g / 255.0;
    value.b = c.b / 255.0;
    value.a = c.a / 255.0;
    return value;
}

Color to_color(V const& v)
{
    auto const channel = [](double c) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
    };
    return Color { channel(v.r), channel(v.g), channel(v.b), channel(v.a) };
}

// A shadow list as it animates: currentcolor taken as the element's color.
std::optional<V> shadows_of(std::shared_ptr<Shadows const> const& list, Color current)
{
    V value;
    value.kind = V::Kind::ShadowList;
    if (!list)
        return value;
    for (Shadow const& shadow : *list) {
        Color const c = shadow.current_color ? current : shadow.color;
        value.shadows.push_back(AnimatedShadow { static_cast<double>(shadow.x), static_cast<double>(shadow.y),
            static_cast<double>(shadow.blur), static_cast<double>(shadow.spread), c.r / 255.0, c.g / 255.0,
            c.b / 255.0, c.a / 255.0, shadow.inset });
    }
    return value;
}

// A shadow list written back: blur held at zero and up, colors to the gamut.
void write_shadows(V const& v, std::shared_ptr<Shadows const>& to)
{
    if (v.shadows.empty()) {
        to.reset();
        return;
    }
    Shadows list;
    for (AnimatedShadow const& from : v.shadows) {
        Shadow shadow;
        shadow.x = static_cast<float>(from.x);
        shadow.y = static_cast<float>(from.y);
        shadow.blur = static_cast<float>(std::max(0.0, from.blur));
        shadow.spread = static_cast<float>(from.spread);
        V color_value;
        color_value.r = from.r;
        color_value.g = from.g;
        color_value.b = from.b;
        color_value.a = from.a;
        shadow.color = to_color(color_value);
        shadow.current_color = false;
        shadow.inset = from.inset;
        list.push_back(shadow);
    }
    to = std::make_shared<Shadows const>(std::move(list));
}

#define LENGTH(css, member, range)                                                                                    \
    AnimatableProperty                                                                                                \
    {                                                                                                                 \
        css, false, V::Kind::Length, range, false, [](S const& s) { return length(s.member); },                      \
            [](V const& v, S& s, unsigned&) { s.member = to_length(v.px, v.percent, range); }                          \
    }

#define PX(css, inherited, member, range)                                                                             \
    AnimatableProperty                                                                                                \
    {                                                                                                                 \
        css, inherited, V::Kind::Length, range, false,                                                                \
            [](S const& s) -> std::optional<V> {                                                                      \
                V value;                                                                                              \
                value.kind = V::Kind::Length;                                                                         \
                value.px = static_cast<double>(s.member);                                                                                \
                return value;                                                                                         \
            },                                                                                                        \
            [](V const& v, S& s, unsigned&) { s.member = static_cast<float>(clamp_to(range, v.px)); }                  \
    }

#define NUMBER(css, inherited, member, range, integer)                                                                \
    AnimatableProperty                                                                                                \
    {                                                                                                                 \
        css, inherited, V::Kind::Number, range, integer,                                                              \
            [](S const& s) -> std::optional<V> { return number(static_cast<double>(s.member)); },                    \
            [](V const& v, S& s, unsigned&) {                                                                         \
                double const n = clamp_to(range, integer ? round_half_up(v.number) : v.number);                       \
                s.member = static_cast<decltype(s.member)>(n);                                                        \
            }                                                                                                         \
    }

#define COLOR(css, inherited, member)                                                                                 \
    AnimatableProperty                                                                                                \
    {                                                                                                                 \
        css, inherited, V::Kind::Color, Any, false, [](S const& s) { return color(s.member); },                       \
            [](V const& v, S& s, unsigned&) { s.member = to_color(v); }                                                \
    }

#define BORDER_COLOR(css, side, bit)                                                                                  \
    AnimatableProperty                                                                                                \
    {                                                                                                                 \
        css, false, V::Kind::Color, Any, false, [](S const& s) { return color(s.side.color); },                       \
            [](V const& v, S& s, unsigned& set) {                                                                     \
                s.side.color = to_color(v);                                                                           \
                s.side.current_color = false;                                                                         \
                set |= bit;                                                                                           \
            }                                                                                                         \
    }

#define RADIUS(css, member)                                                                                           \
    AnimatableProperty                                                                                                \
    {                                                                                                                 \
        css, false, V::Kind::Lengths, NonNegative, false, [](S const& s) { return pair(s.member.x, s.member.y); },     \
            [](V const& v, S& s, unsigned&) {                                                                         \
                if (v.lengths.size() != 2)                                                                            \
                    return;                                                                                           \
                s.member.x = to_length(v.lengths[0].first, v.lengths[0].second, NonNegative);                         \
                s.member.y = to_length(v.lengths[1].first, v.lengths[1].second, NonNegative);                         \
            }                                                                                                         \
    }

std::optional<V> svg_paint_color(SvgPaint const& paint)
{
    if (paint.kind != SvgPaint::Kind::Color)
        return std::nullopt;
    return color(paint.color);
}

void write_svg_paint(SvgPaint& paint, V const& v)
{
    paint = SvgPaint {};
    paint.kind = SvgPaint::Kind::Color;
    paint.color = to_color(v);
}

std::vector<AnimatableProperty> const& property_table()
{
    static std::vector<AnimatableProperty> const table = {
        // Box sizes and edges: <length-percentage> with keywords that do
        // not interpolate.
        LENGTH("width", width, NonNegative),
        LENGTH("height", height, NonNegative),
        LENGTH("min-width", min_width, NonNegative),
        LENGTH("min-height", min_height, NonNegative),
        LENGTH("max-width", max_width, NonNegative),
        LENGTH("max-height", max_height, NonNegative),
        LENGTH("margin-top", margin_top, Any),
        LENGTH("margin-right", margin_right, Any),
        LENGTH("margin-bottom", margin_bottom, Any),
        LENGTH("margin-left", margin_left, Any),
        LENGTH("padding-top", padding_top, NonNegative),
        LENGTH("padding-right", padding_right, NonNegative),
        LENGTH("padding-bottom", padding_bottom, NonNegative),
        LENGTH("padding-left", padding_left, NonNegative),
        LENGTH("top", top, Any),
        LENGTH("right", right, Any),
        LENGTH("bottom", bottom, Any),
        LENGTH("left", left, Any),
        LENGTH("flex-basis", flex_basis, NonNegative),
        LENGTH("row-gap", row_gap, NonNegative),
        LENGTH("column-gap", column_gap, NonNegative),
        AnimatableProperty { "text-indent", true, V::Kind::Length, Any, false, [](S const& s) { return length(s.text_indent); },
            [](V const& v, S& s, unsigned&) { s.text_indent = to_length(v.px, v.percent, Any); } },
        AnimatableProperty { "stroke-width", true, V::Kind::Length, NonNegative, false,
            [](S const& s) { return length(s.stroke_width); },
            [](V const& v, S& s, unsigned&) { s.stroke_width = to_length(v.px, v.percent, NonNegative); } },
        // Lengths held as px.
        PX("font-size", true, font_size, NonNegative),
        PX("border-top-width", false, border_top.width, NonNegative),
        PX("border-right-width", false, border_right.width, NonNegative),
        PX("border-bottom-width", false, border_bottom.width, NonNegative),
        PX("border-left-width", false, border_left.width, NonNegative),
        PX("outline-width", false, outline.width, NonNegative),
        PX("outline-offset", false, outline.offset, Any),
        PX("letter-spacing", true, letter_spacing, Any),
        PX("word-spacing", true, word_spacing, Any),
        // Numbers.
        NUMBER("opacity", false, opacity, Unit, false),
        NUMBER("fill-opacity", true, fill_opacity, Unit, false),
        NUMBER("stroke-opacity", true, stroke_opacity, Unit, false),
        NUMBER("stop-opacity", false, stop_opacity, Unit, false),
        NUMBER("flex-grow", false, flex_grow, NonNegative, false),
        NUMBER("flex-shrink", false, flex_shrink, NonNegative, false),
        NUMBER("stroke-miterlimit", true, stroke_miterlimit, AtLeastOne, false),
        NUMBER("stroke-dashoffset", true, stroke_dashoffset, Any, false),
        NUMBER("font-weight", true, font_weight, Weight, true),
        NUMBER("font-stretch", true, font_stretch, NonNegative, true),
        NUMBER("order", false, order, Any, true),
        AnimatableProperty { "z-index", false, V::Kind::Number, Any, true,
            [](S const& s) -> std::optional<V> {
                if (!s.z_index)
                    return std::nullopt;
                return number(*s.z_index);
            },
            [](V const& v, S& s, unsigned&) { s.z_index = static_cast<int>(round_half_up(v.number)); } },
        // Colors.
        COLOR("color", true, color),
        AnimatableProperty { "background-color", false, V::Kind::Color, Any, false,
            [](S const& s) { return color(s.background_color); },
            [](V const& v, S& s, unsigned&) {
                s.background_color = to_color(v);
                s.background_color_current = false;
            } },
        COLOR("stop-color", false, stop_color),
        BORDER_COLOR("border-top-color", border_top, 1u),
        BORDER_COLOR("border-right-color", border_right, 2u),
        BORDER_COLOR("border-bottom-color", border_bottom, 4u),
        BORDER_COLOR("border-left-color", border_left, 8u),
        AnimatableProperty { "outline-color", false, V::Kind::Color, Any, false,
            [](S const& s) { return color(s.outline.color); },
            [](V const& v, S& s, unsigned&) {
                s.outline.color = to_color(v);
                s.outline.current_color = false;
            } },
        AnimatableProperty { "box-shadow", false, V::Kind::ShadowList, Any, false,
            [](S const& s) { return shadows_of(s.box_shadow, s.color); },
            [](V const& v, S& s, unsigned&) { write_shadows(v, s.box_shadow); } },
        AnimatableProperty { "text-shadow", true, V::Kind::ShadowList, Any, false,
            [](S const& s) { return shadows_of(s.text_shadow, s.color); },
            [](V const& v, S& s, unsigned&) { write_shadows(v, s.text_shadow); } },
        AnimatableProperty { "fill", true, V::Kind::Color, Any, false, [](S const& s) { return svg_paint_color(s.fill); },
            [](V const& v, S& s, unsigned&) { write_svg_paint(s.fill, v); } },
        AnimatableProperty { "stroke", true, V::Kind::Color, Any, false, [](S const& s) { return svg_paint_color(s.stroke); },
            [](V const& v, S& s, unsigned&) { write_svg_paint(s.stroke, v); } },
        // Pairs of lengths.
        RADIUS("border-top-left-radius", border_top_left_radius),
        RADIUS("border-top-right-radius", border_top_right_radius),
        RADIUS("border-bottom-right-radius", border_bottom_right_radius),
        RADIUS("border-bottom-left-radius", border_bottom_left_radius),
        AnimatableProperty { "object-position", false, V::Kind::Lengths, Any, false,
            [](S const& s) { return pair(s.object_position_x, s.object_position_y); },
            [](V const& v, S& s, unsigned&) {
                if (v.lengths.size() != 2)
                    return;
                s.object_position_x = to_length(v.lengths[0].first, v.lengths[0].second, Any);
                s.object_position_y = to_length(v.lengths[1].first, v.lengths[1].second, Any);
            } },
        AnimatableProperty { "border-spacing", true, V::Kind::Lengths, NonNegative, false,
            [](S const& s) { return pair(s.border_spacing_horizontal, s.border_spacing_vertical); },
            [](V const& v, S& s, unsigned&) {
                if (v.lengths.size() != 2)
                    return;
                s.border_spacing_horizontal = to_length(v.lengths[0].first, v.lengths[0].second, NonNegative);
                s.border_spacing_vertical = to_length(v.lengths[1].first, v.lengths[1].second, NonNegative);
            } },
        // The translations: the engine draws only these of a transform,
        // and `transform` and `translate` keep them in the same place. A
        // box with a transform animating is a stacking context throughout
        // (css-transforms-1 §6), whatever the value of the moment.
        AnimatableProperty { "translate", false, V::Kind::Lengths, Any, false,
            [](S const& s) { return pair(s.translate_x, s.translate_y); },
            [](V const& v, S& s, unsigned&) {
                if (v.lengths.size() != 2)
                    return;
                s.translate_x = to_length(v.lengths[0].first, v.lengths[0].second, Any);
                s.translate_y = to_length(v.lengths[1].first, v.lengths[1].second, Any);
                s.transformed = true;
            } },
        AnimatableProperty { "transform", false, V::Kind::Lengths, Any, false,
            [](S const& s) { return pair(s.translate_x, s.translate_y); },
            [](V const& v, S& s, unsigned&) {
                if (v.lengths.size() != 2)
                    return;
                s.translate_x = to_length(v.lengths[0].first, v.lengths[0].second, Any);
                s.translate_y = to_length(v.lengths[1].first, v.lengths[1].second, Any);
                s.transformed = true;
            } },
        // The background layers' positions, two lengths a layer.
        AnimatableProperty { "background-position", false, V::Kind::Lengths, Any, false,
            [](S const& s) -> std::optional<V> {
                V value;
                value.kind = V::Kind::Lengths;
                std::vector<BackgroundPosition> const fallback { BackgroundPosition {} };
                auto const& positions = s.background_positions ? *s.background_positions : fallback;
                for (BackgroundPosition const& position : positions) {
                    std::optional<V> const x = length(position.x);
                    std::optional<V> const y = length(position.y);
                    if (!x || !y)
                        return std::nullopt;
                    value.lengths.emplace_back(x->px, x->percent);
                    value.lengths.emplace_back(y->px, y->percent);
                }
                return value;
            },
            [](V const& v, S& s, unsigned&) {
                std::vector<BackgroundPosition> positions;
                for (std::size_t i = 0; i + 1 < v.lengths.size(); i += 2)
                    positions.push_back({ to_length(v.lengths[i].first, v.lengths[i].second, Any),
                        to_length(v.lengths[i + 1].first, v.lengths[i + 1].second, Any) });
                s.background_positions = std::make_shared<std::vector<BackgroundPosition> const>(std::move(positions));
            } },
        // Lines and alignment: the length forms interpolate, the keywords
        // do not.
        AnimatableProperty { "line-height", true, V::Kind::Number, NonNegative, false,
            [](S const& s) -> std::optional<V> {
                if (s.line_height.kind == LineHeight::Kind::Number)
                    return number(static_cast<double>(s.line_height.value));
                if (s.line_height.kind == LineHeight::Kind::Px) {
                    V value;
                    value.kind = V::Kind::Length;
                    value.px = static_cast<double>(s.line_height.value);
                    return value;
                }
                return std::nullopt;
            },
            [](V const& v, S& s, unsigned&) {
                if (v.kind == V::Kind::Number)
                    s.line_height = { LineHeight::Kind::Number, static_cast<float>(std::max(0.0, v.number)) };
                else
                    s.line_height = { LineHeight::Kind::Px, static_cast<float>(std::max(0.0, v.px)) };
            } },
        AnimatableProperty { "vertical-align", false, V::Kind::Length, Any, false,
            [](S const& s) -> std::optional<V> {
                if (s.vertical_align.kind != VerticalAlign::Kind::Length)
                    return std::nullopt;
                return length(s.vertical_align.offset);
            },
            [](V const& v, S& s, unsigned&) {
                s.vertical_align.kind = VerticalAlign::Kind::Length;
                s.vertical_align.offset = to_length(v.px, v.percent, Any);
            } },
        // The dashes of a stroke: a list of numbers, repeated to meet.
        AnimatableProperty { "stroke-dasharray", true, V::Kind::Numbers, NonNegative, false,
            [](S const& s) -> std::optional<V> {
                if (!s.stroke_dasharray)
                    return std::nullopt;
                V value;
                value.kind = V::Kind::Numbers;
                for (float const dash : *s.stroke_dasharray)
                    value.numbers.push_back(static_cast<double>(dash));
                return value;
            },
            [](V const& v, S& s, unsigned&) {
                std::vector<float> dashes;
                for (double const dash : v.numbers)
                    dashes.push_back(static_cast<float>(std::max(0.0, dash)));
                s.stroke_dasharray = std::make_shared<std::vector<float> const>(std::move(dashes));
            } },
        AnimatableProperty { "visibility", true, V::Kind::Visibility, Any, false,
            [](S const& s) -> std::optional<V> {
                V value;
                value.kind = V::Kind::Visibility;
                value.visible = s.visibility == Visibility::Visible;
                return value;
            },
            [](V const& v, S& s, unsigned&) {
                s.visibility = v.visible ? Visibility::Visible : Visibility::Hidden;
            } },
    };
    return table;
}

std::unordered_map<std::string_view, AnimatableProperty const*> const& property_index()
{
    static std::unordered_map<std::string_view, AnimatableProperty const*> const index = [] {
        std::unordered_map<std::string_view, AnimatableProperty const*> made;
        for (AnimatableProperty const& property : property_table())
            made.emplace(property.name, &property);
        return made;
    }();
    return index;
}

// Two lists repeated to their least common multiple (css-values-4 §3.3,
// "repeatable list"); nullopt when either is empty or the multiple is
// out of reason.
template<typename T>
std::optional<std::pair<std::vector<T>, std::vector<T>>> repeated(std::vector<T> const& a, std::vector<T> const& b,
    std::size_t unit)
{
    if (a.empty() || b.empty() || a.size() % unit || b.size() % unit)
        return std::nullopt;
    std::size_t const na = a.size() / unit;
    std::size_t const nb = b.size() / unit;
    std::size_t const n = std::lcm(na, nb);
    if (n > 1024)
        return std::nullopt;
    std::pair<std::vector<T>, std::vector<T>> out;
    for (std::size_t i = 0; i < n * unit; ++i) {
        out.first.push_back(a[i % a.size()]);
        out.second.push_back(b[i % b.size()]);
    }
    return out;
}

double mix(double a, double b, double p) { return a + (b - a) * p; }

}

AnimatableProperty const* animatable_property(std::string_view longhand)
{
    auto const& index = property_index();
    auto const found = index.find(longhand);
    return found == index.end() ? nullptr : found->second;
}

std::string_view property_name(AnimatableProperty const& property) { return property.name; }

bool property_inherits(AnimatableProperty const& property) { return property.inherited; }

std::vector<std::string_view> const* shorthand_longhands(std::string_view name)
{
    static std::unordered_map<std::string_view, std::vector<std::string_view>> const shorthands = {
        { "margin", { "margin-top", "margin-right", "margin-bottom", "margin-left" } },
        { "padding", { "padding-top", "padding-right", "padding-bottom", "padding-left" } },
        { "inset", { "top", "right", "bottom", "left" } },
        { "border-width", { "border-top-width", "border-right-width", "border-bottom-width", "border-left-width" } },
        { "border-color", { "border-top-color", "border-right-color", "border-bottom-color", "border-left-color" } },
        { "border-style", { "border-top-style", "border-right-style", "border-bottom-style", "border-left-style" } },
        { "border-top", { "border-top-width", "border-top-style", "border-top-color" } },
        { "border-right", { "border-right-width", "border-right-style", "border-right-color" } },
        { "border-bottom", { "border-bottom-width", "border-bottom-style", "border-bottom-color" } },
        { "border-left", { "border-left-width", "border-left-style", "border-left-color" } },
        { "border",
            { "border-top-width", "border-right-width", "border-bottom-width", "border-left-width", "border-top-style",
                "border-right-style", "border-bottom-style", "border-left-style", "border-top-color", "border-right-color",
                "border-bottom-color", "border-left-color" } },
        { "border-radius",
            { "border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
                "border-bottom-left-radius" } },
        { "outline", { "outline-color", "outline-style", "outline-width" } },
        { "flex", { "flex-grow", "flex-shrink", "flex-basis" } },
        { "flex-flow", { "flex-direction", "flex-wrap" } },
        { "gap", { "row-gap", "column-gap" } },
        { "grid-gap", { "row-gap", "column-gap" } },
        { "overflow", { "overflow-x", "overflow-y" } },
        { "background",
            { "background-color", "background-image", "background-position", "background-size", "background-repeat",
                "background-origin", "background-clip" } },
        { "font", { "font-style", "font-weight", "font-stretch", "font-size", "line-height", "font-family" } },
        { "list-style", { "list-style-type", "list-style-position" } },
    };
    auto const found = shorthands.find(name);
    return found == shorthands.end() ? nullptr : &found->second;
}

bool is_not_animatable(std::string_view name)
{
    return name.starts_with("animation") || name.starts_with("transition") || name == "direction"
        || name == "unicode-bidi" || name == "writing-mode" || name == "text-orientation" || name == "will-change"
        || name == "all" || name == "contain" || name == "container"
        || name == "container-name" || name == "container-type";
}

std::optional<AnimatedValue> read_animated(AnimatableProperty const& property, ComputedStyle const& style)
{
    return property.read(style);
}

std::optional<AnimatedValue> read_declared(AnimatableProperty const& property, std::vector<ComponentValue> const& value)
{
    if (property.kind != V::Kind::Number || property.range != Unit)
        return std::nullopt;
    ComponentValue const* only = nullptr;
    for (ComponentValue const& item : value) {
        if (item.is_token(Token::Type::Whitespace))
            continue;
        if (only)
            return std::nullopt;
        only = &item;
    }
    if (!only || !only->is_token())
        return std::nullopt;
    Token const& token = only->token();
    if (token.type == Token::Type::Number)
        return number(token.numeric_value);
    if (token.type == Token::Type::Percentage)
        return number(token.numeric_value / 100.0);
    return std::nullopt;
}

void write_animated(AnimatableProperty const& property, AnimatedValue const& value, ComputedStyle& style,
    unsigned& border_colors)
{
    property.write(value, style, border_colors);
}

namespace {

// Two shadow lists made the same length (css-backgrounds-3 §7.1, "as shadow
// list"): the shorter padded with transparent zero shadows, each inset as
// its partner is; nullopt when a pair differs in inset.
std::optional<std::pair<std::vector<AnimatedShadow>, std::vector<AnimatedShadow>>> matched_shadows(
    std::vector<AnimatedShadow> a, std::vector<AnimatedShadow> b)
{
    while (a.size() < b.size()) {
        AnimatedShadow blank;
        blank.inset = b[a.size()].inset;
        a.push_back(blank);
    }
    while (b.size() < a.size()) {
        AnimatedShadow blank;
        blank.inset = a[b.size()].inset;
        b.push_back(blank);
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].inset != b[i].inset)
            return std::nullopt;
    }
    return std::pair { std::move(a), std::move(b) };
}

// `b` scaled by `t` added to `a` (an interpolation is a's (1 - p) plus b's p),
// lengths as they are and colors premultiplied.
AnimatedShadow weighted_shadow(AnimatedShadow const& a, double s, AnimatedShadow const& b, double t)
{
    AnimatedShadow out;
    out.x = a.x * s + b.x * t;
    out.y = a.y * s + b.y * t;
    out.blur = a.blur * s + b.blur * t;
    out.spread = a.spread * s + b.spread * t;
    double const alpha = std::clamp(a.a * s + b.a * t, 0.0, 1.0);
    out.a = alpha;
    if (alpha > 0) {
        out.r = (a.r * a.a * s + b.r * b.a * t) / alpha;
        out.g = (a.g * a.a * s + b.g * b.a * t) / alpha;
        out.b = (a.b * a.a * s + b.b * b.a * t) / alpha;
    }
    out.inset = a.inset;
    return out;
}

}

std::optional<AnimatedValue> interpolate_animated(AnimatableProperty const& property, AnimatedValue const& a,
    AnimatedValue const& b, double p)
{
    if (a.kind != b.kind)
        return std::nullopt;
    V out;
    out.kind = a.kind;
    switch (a.kind) {
    case V::Kind::Number:
        out.number = mix(a.number, b.number, p);
        if (property.integer)
            out.number = round_half_up(out.number);
        return out;
    case V::Kind::Length:
        out.px = mix(a.px, b.px, p);
        out.percent = mix(a.percent, b.percent, p);
        return out;
    case V::Kind::Color: {
        // Premultiplied, so a transparent end lends no color of its own.
        double const alpha = std::clamp(mix(a.a, b.a, p), 0.0, 1.0);
        out.a = alpha;
        if (alpha > 0) {
            out.r = mix(a.r * a.a, b.r * b.a, p) / alpha;
            out.g = mix(a.g * a.a, b.g * b.a, p) / alpha;
            out.b = mix(a.b * a.a, b.b * b.a, p) / alpha;
        }
        return out;
    }
    case V::Kind::Lengths: {
        if (a.lengths.size() != b.lengths.size()) {
            auto const lists = repeated(a.lengths, b.lengths, property.name == "background-position" ? 2 : 1);
            if (!lists)
                return std::nullopt;
            V left;
            left.kind = V::Kind::Lengths;
            left.lengths = lists->first;
            V right;
            right.kind = V::Kind::Lengths;
            right.lengths = lists->second;
            return interpolate_animated(property, left, right, p);
        }
        for (std::size_t i = 0; i < a.lengths.size(); ++i)
            out.lengths.emplace_back(mix(a.lengths[i].first, b.lengths[i].first, p),
                mix(a.lengths[i].second, b.lengths[i].second, p));
        return out;
    }
    case V::Kind::Numbers: {
        auto const lists = repeated(a.numbers, b.numbers, 1);
        if (!lists)
            return std::nullopt;
        for (std::size_t i = 0; i < lists->first.size(); ++i)
            out.numbers.push_back(mix(lists->first[i], lists->second[i], p));
        return out;
    }
    case V::Kind::ShadowList: {
        auto const lists = matched_shadows(a.shadows, b.shadows);
        if (!lists)
            return std::nullopt;
        for (std::size_t i = 0; i < lists->first.size(); ++i)
            out.shadows.push_back(weighted_shadow(lists->first[i], 1 - p, lists->second[i], p));
        return out;
    }
    case V::Kind::Visibility:
        // css-values-4 §3 for visibility: visible throughout (0, 1) when
        // either end is; the ends are themselves.
        if (!a.visible && !b.visible)
            return std::nullopt;
        if (p <= 0)
            return a;
        if (p >= 1)
            return b;
        out.visible = true;
        return out;
    }
    return std::nullopt;
}

std::optional<AnimatedValue> accumulate_animated(AnimatableProperty const& property, AnimatedValue const& a,
    AnimatedValue const& b, double count)
{
    if (a.kind != b.kind)
        return std::nullopt;
    V out;
    out.kind = a.kind;
    switch (a.kind) {
    case V::Kind::Number:
        out.number = a.number + b.number * count;
        if (property.integer)
            out.number = round_half_up(out.number);
        return out;
    case V::Kind::Length:
        out.px = a.px + b.px * count;
        out.percent = a.percent + b.percent * count;
        return out;
    case V::Kind::Color: {
        // Added premultiplied, the alpha held to [0, 1] and the channels
        // left past the gamut until the value is written, so that a sum
        // still interpolates as itself (the engines all do).
        double const alpha = std::clamp(a.a + b.a * count, 0.0, 1.0);
        out.a = alpha;
        if (alpha > 0) {
            out.r = (a.r * a.a + b.r * b.a * count) / alpha;
            out.g = (a.g * a.a + b.g * b.a * count) / alpha;
            out.b = (a.b * a.a + b.b * b.a * count) / alpha;
        }
        return out;
    }
    case V::Kind::Lengths: {
        if (a.lengths.size() != b.lengths.size())
            return std::nullopt;
        for (std::size_t i = 0; i < a.lengths.size(); ++i)
            out.lengths.emplace_back(a.lengths[i].first + b.lengths[i].first * count,
                a.lengths[i].second + b.lengths[i].second * count);
        return out;
    }
    case V::Kind::Numbers:
        // A dash list is not additive (SVG 2 §13.5.7): the value replaces.
        return std::nullopt;
    case V::Kind::ShadowList: {
        auto const lists = matched_shadows(a.shadows, b.shadows);
        if (!lists)
            return std::nullopt;
        for (std::size_t i = 0; i < lists->first.size(); ++i)
            out.shadows.push_back(weighted_shadow(lists->first[i], 1, lists->second[i], count));
        return out;
    }
    case V::Kind::Visibility: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<AnimatedValue> add_animated(AnimatableProperty const& property, AnimatedValue const& a,
    AnimatedValue const& b)
{
    // Shadow lists add by putting one after the other (css-backgrounds-3
    // §7.1): the shadows under, then the ones added.
    if (a.kind == V::Kind::ShadowList && b.kind == V::Kind::ShadowList) {
        V out = a;
        out.shadows.insert(out.shadows.end(), b.shadows.begin(), b.shadows.end());
        return out;
    }
    return accumulate_animated(property, a, b, 1);
}

bool same_animated(AnimatedValue const& a, AnimatedValue const& b)
{
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
    case V::Kind::Number: return a.number == b.number;
    case V::Kind::Length: return a.px == b.px && a.percent == b.percent;
    case V::Kind::Color: return to_color(a) == to_color(b);
    case V::Kind::Lengths: return a.lengths == b.lengths;
    case V::Kind::Numbers: return a.numbers == b.numbers;
    case V::Kind::Visibility: return a.visible == b.visible;
    case V::Kind::ShadowList: return a.shadows == b.shadows;
    }
    return false;
}

}

namespace sashfold::css {

std::vector<AnimatableProperty const*> const& animatable_properties()
{
    static std::vector<AnimatableProperty const*> const all = [] {
        std::vector<AnimatableProperty const*> made;
        for (AnimatableProperty const& property : property_table())
            made.push_back(&property);
        return made;
    }();
    return all;
}

namespace {

std::string number_text(double value)
{
    double const rounded = std::round(value * 1e6) / 1e6;
    if (rounded == 0)
        return "0";
    std::string text = std::to_string(rounded);
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0')
            text.pop_back();
        if (text.back() == '.')
            text.pop_back();
    }
    return text;
}

std::string length_text(double px, double percent)
{
    if (percent == 0)
        return number_text(px) + "px";
    if (px == 0)
        return number_text(percent) + "%";
    return "calc(" + number_text(percent) + "% + " + number_text(px) + "px)";
}

}

std::string serialize_animated(AnimatableProperty const& property, AnimatedValue const& value)
{
    switch (value.kind) {
    case V::Kind::Number:
        return number_text(property.integer ? round_half_up(value.number) : value.number);
    case V::Kind::Length:
        return length_text(value.px, value.percent);
    case V::Kind::Color: {
        Color const color = to_color(value);
        if (color.a == 255)
            return "rgb(" + std::to_string(color.r) + ", " + std::to_string(color.g) + ", " + std::to_string(color.b) + ")";
        return "rgba(" + std::to_string(color.r) + ", " + std::to_string(color.g) + ", " + std::to_string(color.b) + ", "
            + number_text(std::round(color.a / 255.0 * 1000) / 1000) + ")";
    }
    case V::Kind::Lengths: {
        std::string text;
        for (auto const& [px, percent] : value.lengths)
            text += (text.empty() ? "" : " ") + length_text(px, percent);
        return text;
    }
    case V::Kind::Numbers: {
        std::string text;
        for (double const number : value.numbers)
            text += (text.empty() ? "" : ", ") + number_text(number);
        return text;
    }
    case V::Kind::Visibility: return value.visible ? "visible" : "hidden";
    case V::Kind::ShadowList: {
        if (value.shadows.empty())
            return "none";
        bool const box = property.name == "box-shadow";
        std::string text;
        for (AnimatedShadow const& shadow : value.shadows) {
            V color_value;
            color_value.kind = V::Kind::Color;
            color_value.r = shadow.r;
            color_value.g = shadow.g;
            color_value.b = shadow.b;
            color_value.a = shadow.a;
            text += (text.empty() ? "" : ", ") + serialize_animated(property, color_value) + " " + length_text(shadow.x, 0) + " "
                + length_text(shadow.y, 0) + " " + length_text(std::max(0.0, shadow.blur), 0);
            if (box) {
                text += " " + length_text(shadow.spread, 0);
                if (shadow.inset)
                    text += " inset";
            }
        }
        return text;
    }
    }
    return {};
}

}

namespace sashfold::css {

bool is_current_color(AnimatableProperty const& property, ComputedStyle const& style)
{
    std::string_view const name = property.name;
    if (name == "border-top-color")
        return style.border_top.current_color;
    if (name == "border-right-color")
        return style.border_right.current_color;
    if (name == "border-bottom-color")
        return style.border_bottom.current_color;
    if (name == "border-left-color")
        return style.border_left.current_color;
    if (name == "outline-color")
        return style.outline.current_color;
    if (name == "background-color")
        return style.background_color_current;
    if (name == "fill")
        return style.fill.kind == SvgPaint::Kind::CurrentColor;
    if (name == "stroke")
        return style.stroke.kind == SvgPaint::Kind::CurrentColor;
    return false;
}

}
