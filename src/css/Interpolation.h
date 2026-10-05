#pragma once

// Interpolation, addition and accumulation of computed values (css-values-4
// §3, web-animations-1 §5.3 "Animation types"), for the properties the
// engine computes. A property animates as a typed value read from a
// computed style, combined with another of its kind, and written back into
// a style: lengths and percentages (px and percent, interpolated part by
// part, so a length meets a percentage as calc()), numbers, integers,
// colors (premultiplied), lists of these, and visibility. What has no such
// form — a keyword that does not interpolate, a property with none — is
// discrete: the value flips at the halfway point, copied whole from the
// style one keyframe computes.

#include "css/ComputedStyle.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::css {

struct AnimatedValue {
    enum class Kind : std::uint8_t {
        Number,
        Length, // px (device) and a percentage
        Color, // unpremultiplied, each channel 0..1
        Lengths, // pairs and lists of lengths
        Numbers,
        Visibility,
    };
    Kind kind = Kind::Number;
    double number = 0;
    double px = 0;
    double percent = 0;
    double r = 0, g = 0, b = 0, a = 0;
    std::vector<std::pair<double, double>> lengths; // (px, percent)
    std::vector<double> numbers;
    bool visible = true;
};

// The longhands a style can animate, by name. Null for a name that is not
// one: a shorthand, a property the engine does not compute, one that is
// not animatable.
struct AnimatableProperty;
AnimatableProperty const* animatable_property(std::string_view longhand);
std::string_view property_name(AnimatableProperty const& property);
// Whether the property inherits (a child's value comes from the parent's).
bool property_inherits(AnimatableProperty const& property);

// The longhands a shorthand sets, for the shorthands keyframes may name;
// null for a name that is not one.
std::vector<std::string_view> const* shorthand_longhands(std::string_view name);

// Whether a name is one the animation APIs refuse outright: the animation
// and transition properties themselves, and the few the specifications
// call not animatable.
bool is_not_animatable(std::string_view name);

// The value as an animation sees it; nullopt when this value does not
// interpolate (a keyword such as auto): the property is discrete there.
std::optional<AnimatedValue> read_animated(AnimatableProperty const& property, ComputedStyle const& style);
// A keyframe's number as written, before the computed value clamps it:
// opacity and its kin (a number or a percentage) composite unclamped and
// are held to [0, 1] only when written (web-animations-1 §5.3.5). Nullopt
// for anything else, which reads its computed value.
std::optional<AnimatedValue> read_declared(AnimatableProperty const& property, std::vector<ComponentValue> const& value);

// Writes a value read (and combined) from the same property's styles.
// `border_colors` gets the bits of the border sides whose color it set (1
// top, 2 right, 4 bottom, 8 left), which the cascade keeps.
void write_animated(AnimatableProperty const& property, AnimatedValue const& value, ComputedStyle& style,
    unsigned& border_colors);

// `a` toward `b` by `p` (any real: an easing may overshoot); nullopt when
// the two do not interpolate (different list lengths that cannot be
// repeated to meet, a calc() a number cannot be): discrete.
std::optional<AnimatedValue> interpolate_animated(AnimatableProperty const& property, AnimatedValue const& a,
    AnimatedValue const& b, double p);
// `b` added to `a` (composite: add), and accumulated onto it `count` times
// (composite: accumulate, an iteration's accumulation); nullopt where the
// property is not additive.
std::optional<AnimatedValue> add_animated(AnimatableProperty const& property, AnimatedValue const& a,
    AnimatedValue const& b);
std::optional<AnimatedValue> accumulate_animated(AnimatableProperty const& property, AnimatedValue const& a,
    AnimatedValue const& b, double count);

// Every property with a typed value: what transition-property: all names.
std::vector<AnimatableProperty const*> const& animatable_properties();

// A value as CSS writes its computed value (a transition's keyframes say
// their ends this way): px for lengths, rgb() and rgba() for colors.
std::string serialize_animated(AnimatableProperty const& property, AnimatedValue const& value);

// Whether the value is currentcolor (a border or outline color written so,
// or a fill or stroke): a value that follows color, and so is the same
// value when only color changes (css-transitions-1 §3 compares computed
// values).
bool is_current_color(AnimatableProperty const& property, ComputedStyle const& style);

// Two values the same as an animation would draw them.
bool same_animated(AnimatedValue const& a, AnimatedValue const& b);

}
