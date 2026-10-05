#pragma once

// Easing functions (css-easing-1, and linear() of css-easing-2): what turns an
// animation's input progress into its output progress. Shared by script
// animations (an effect's and a keyframe's `easing`), CSS animations
// (animation-timing-function) and CSS transitions
// (transition-timing-function).

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::css {

struct ComponentValue;

struct Easing {
    enum class Kind : unsigned char {
        Linear, // the keyword: output is input
        CubicBezier,
        Steps,
        LinearPoints, // linear(...)
    };
    // How a step easing places its jumps (§3.1).
    enum class StepPosition : unsigned char {
        JumpStart,
        JumpEnd,
        JumpNone,
        JumpBoth,
        Start, // spelled `start`: jump-start, serialized as written
        End, // spelled `end`: jump-end
    };
    // The keyword a cubic Bézier was written as, for serializing it back.
    enum class Keyword : unsigned char {
        None,
        Ease,
        EaseIn,
        EaseOut,
        EaseInOut,
        StepStart,
        StepEnd,
    };

    Kind kind = Kind::Linear;
    Keyword keyword = Keyword::None;
    double x1 = 0, y1 = 0, x2 = 1, y2 = 1; // CubicBezier
    int steps = 1; // Steps
    StepPosition position = StepPosition::End;
    // LinearPoints: (output, input) pairs, inputs settled (§2.2.3 "canonicalize").
    std::vector<std::pair<double, double>> points;

    static Easing linear() { return {}; }
    static Easing ease();

    // The output progress for `input`; `before` is the before flag a step
    // easing reads (web-animations-1 §4.10.1).
    double apply(double input, bool before = false) const;
    // As getTiming(), getComputedStyle() and the CSSOM write it.
    std::string serialize() const;
    bool is_linear() const { return kind == Kind::Linear; }
    bool operator==(Easing const& other) const = default;
};

// <easing-function> from one component value list (whitespace allowed
// around it, nothing else); nullopt when it is not one.
std::optional<Easing> parse_easing(std::vector<ComponentValue> const& values);
// The same from text, as the Web Animations API takes it.
std::optional<Easing> parse_easing_text(std::string_view text);
// A number as CSS writes one: shortest form, no exponent for ordinary sizes.
std::string serialize_css_number(double value);

}
