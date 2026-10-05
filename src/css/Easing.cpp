#include "css/Easing.h"

#include "css/Parser.h"
#include "css/StyleResolver.h"
#include "core/Ascii.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace sashfold::css {

namespace {

std::vector<ComponentValue const*> significant_values(std::vector<ComponentValue> const& values)
{
    std::vector<ComponentValue const*> out;
    for (ComponentValue const& value : values) {
        if (!value.is_token(Token::Type::Whitespace))
            out.push_back(&value);
    }
    return out;
}

// The arguments of a function, split at its commas; empty when two commas
// meet or one ends the list.
std::optional<std::vector<std::vector<ComponentValue const*>>> comma_separated(std::vector<ComponentValue> const& values)
{
    std::vector<std::vector<ComponentValue const*>> parts(1);
    for (ComponentValue const* value : significant_values(values)) {
        if (value->is_token(Token::Type::Comma)) {
            if (parts.back().empty())
                return std::nullopt;
            parts.emplace_back();
            continue;
        }
        parts.back().push_back(value);
    }
    if (parts.back().empty())
        return std::nullopt;
    return parts;
}

bool is_calc_function(ComponentValue const& value)
{
    if (!value.is_function())
        return false;
    std::string_view const name = value.function().name;
    return ascii_ci_equals(name, "calc") || ascii_ci_equals(name, "min") || ascii_ci_equals(name, "max")
        || ascii_ci_equals(name, "clamp");
}

// The solver of a cubic Bézier through (0,0), (x1,y1), (x2,y2), (1,1) for
// its y at an x, extended past [0,1] along the tangent at the nearer end —
// what every engine does (Chromium's gfx::CubicBezier, whose gradients these
// are), so that an input outside the interval (a delay's before phase with
// fill, an iteration start past the end) still has an output.
struct Bezier {
    double ax, bx, cx, ay, by, cy;
    double start_gradient, end_gradient;
    double x1, y1, x2, y2;

    Bezier(double p1x, double p1y, double p2x, double p2y)
        : x1(p1x)
        , y1(p1y)
        , x2(p2x)
        , y2(p2y)
    {
        cx = 3.0 * p1x;
        bx = 3.0 * (p2x - p1x) - cx;
        ax = 1.0 - cx - bx;
        cy = 3.0 * p1y;
        by = 3.0 * (p2y - p1y) - cy;
        ay = 1.0 - cy - by;
        if (p1x > 0)
            start_gradient = p1y / p1x;
        else if (p1y == 0 && p2x > 0)
            start_gradient = p2y / p2x;
        else if (p1y == 0 && p2y == 0)
            start_gradient = 1;
        else
            start_gradient = 0;
        if (p2x < 1)
            end_gradient = (p2y - 1) / (p2x - 1);
        else if (p2y == 1 && p1x < 1)
            end_gradient = (p1y - 1) / (p1x - 1);
        else if (p2y == 1 && p1y == 1)
            end_gradient = 1;
        else
            end_gradient = 0;
    }

    double sample_x(double t) const { return ((ax * t + bx) * t + cx) * t; }
    double sample_y(double t) const { return ((ay * t + by) * t + cy) * t; }
    double sample_dx(double t) const { return (3.0 * ax * t + 2.0 * bx) * t + cx; }

    double solve_t(double x) const
    {
        constexpr double epsilon = 1e-9;
        // Newton's method from the line's guess, then bisection if it
        // wanders.
        double t = x;
        for (int i = 0; i < 8; ++i) {
            double const error = sample_x(t) - x;
            if (std::fabs(error) < epsilon)
                return t;
            double const slope = sample_dx(t);
            if (std::fabs(slope) < 1e-7)
                break;
            t -= error / slope;
        }
        double low = 0;
        double high = 1;
        t = x;
        if (t < low)
            return low;
        if (t > high)
            return high;
        while (low < high) {
            double const value = sample_x(t);
            if (std::fabs(value - x) < epsilon)
                return t;
            if (x > value)
                low = t;
            else
                high = t;
            t = (high - low) / 2.0 + low;
            if (high - low < epsilon)
                break;
        }
        return t;
    }

    double solve(double x) const
    {
        if (x < 0)
            return start_gradient * x;
        if (x > 1)
            return 1.0 + end_gradient * (x - 1.0);
        return sample_y(solve_t(x));
    }
};

double step_output(Easing const& easing, double input, bool before)
{
    // css-easing-1 §3.1.1, "step easing function".
    int const steps = easing.steps;
    double current = std::floor(input * steps);
    bool const jump_start = easing.position == Easing::StepPosition::JumpStart
        || easing.position == Easing::StepPosition::Start || easing.position == Easing::StepPosition::JumpBoth;
    if (jump_start)
        current += 1;
    if (before && std::fmod(input * steps, 1.0) == 0)
        current -= 1;
    if (input >= 0 && current < 0)
        current = 0;
    int jumps = steps;
    switch (easing.position) {
    case Easing::StepPosition::JumpNone: jumps = steps - 1; break;
    case Easing::StepPosition::JumpBoth: jumps = steps + 1; break;
    default: break;
    }
    if (input <= 1 && current > jumps)
        current = jumps;
    return current / jumps;
}

double linear_points_output(Easing const& easing, double input)
{
    // css-easing-2 §2.2.4, "calculate linear easing output progress".
    auto const& points = easing.points; // (output, input)
    if (points.empty())
        return input;
    if (points.size() == 1)
        return points.front().first;
    std::size_t a = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].second <= input)
            a = i;
    }
    if (a == points.size() - 1)
        --a;
    auto const& [a_output, a_input] = points[a];
    auto const& [b_output, b_input] = points[a + 1];
    if (a_input == b_input)
        return b_output;
    double const between = (input - a_input) / (b_input - a_input);
    return a_output + between * (b_output - a_output);
}

std::optional<Easing> keyword_easing(std::string_view name)
{
    Easing easing;
    if (ascii_ci_equals(name, "linear"))
        return easing;
    easing.kind = Easing::Kind::CubicBezier;
    if (ascii_ci_equals(name, "ease")) {
        easing.keyword = Easing::Keyword::Ease;
        easing.x1 = 0.25, easing.y1 = 0.1, easing.x2 = 0.25, easing.y2 = 1;
        return easing;
    }
    if (ascii_ci_equals(name, "ease-in")) {
        easing.keyword = Easing::Keyword::EaseIn;
        easing.x1 = 0.42, easing.y1 = 0, easing.x2 = 1, easing.y2 = 1;
        return easing;
    }
    if (ascii_ci_equals(name, "ease-out")) {
        easing.keyword = Easing::Keyword::EaseOut;
        easing.x1 = 0, easing.y1 = 0, easing.x2 = 0.58, easing.y2 = 1;
        return easing;
    }
    if (ascii_ci_equals(name, "ease-in-out")) {
        easing.keyword = Easing::Keyword::EaseInOut;
        easing.x1 = 0.42, easing.y1 = 0, easing.x2 = 0.58, easing.y2 = 1;
        return easing;
    }
    easing.kind = Easing::Kind::Steps;
    easing.steps = 1;
    if (ascii_ci_equals(name, "step-start")) {
        easing.keyword = Easing::Keyword::StepStart;
        easing.position = Easing::StepPosition::Start;
        return easing;
    }
    if (ascii_ci_equals(name, "step-end")) {
        easing.keyword = Easing::Keyword::StepEnd;
        easing.position = Easing::StepPosition::End;
        return easing;
    }
    return std::nullopt;
}

std::optional<Easing> cubic_bezier(std::vector<ComponentValue> const& arguments)
{
    auto const parts = comma_separated(arguments);
    if (!parts || parts->size() != 4)
        return std::nullopt;
    double numbers[4];
    for (std::size_t i = 0; i < 4; ++i) {
        if ((*parts)[i].size() != 1)
            return std::nullopt;
        ComponentValue const& value = *(*parts)[i][0];
        std::optional<double> const number = parse_number_value(value);
        if (!number)
            return std::nullopt;
        numbers[i] = *number;
        // The x coordinates are in [0, 1]: as written they must be, and a
        // calculation is clamped there (css-values-4 §10.12).
        if (i % 2 == 0) {
            if (is_calc_function(value))
                numbers[i] = std::clamp(numbers[i], 0.0, 1.0);
            else if (numbers[i] < 0 || numbers[i] > 1)
                return std::nullopt;
        }
    }
    Easing easing;
    easing.kind = Easing::Kind::CubicBezier;
    easing.x1 = numbers[0], easing.y1 = numbers[1], easing.x2 = numbers[2], easing.y2 = numbers[3];
    return easing;
}

std::optional<Easing> steps(std::vector<ComponentValue> const& arguments)
{
    auto const parts = comma_separated(arguments);
    if (!parts || parts->empty() || parts->size() > 2 || (*parts)[0].size() != 1)
        return std::nullopt;
    Easing easing;
    easing.kind = Easing::Kind::Steps;
    if (parts->size() == 2) {
        if ((*parts)[1].size() != 1 || !(*parts)[1][0]->is_token(Token::Type::Ident))
            return std::nullopt;
        std::string_view const name = (*parts)[1][0]->token().value;
        if (ascii_ci_equals(name, "jump-start"))
            easing.position = Easing::StepPosition::JumpStart;
        else if (ascii_ci_equals(name, "jump-end"))
            easing.position = Easing::StepPosition::JumpEnd;
        else if (ascii_ci_equals(name, "jump-none"))
            easing.position = Easing::StepPosition::JumpNone;
        else if (ascii_ci_equals(name, "jump-both"))
            easing.position = Easing::StepPosition::JumpBoth;
        else if (ascii_ci_equals(name, "start"))
            easing.position = Easing::StepPosition::Start;
        else if (ascii_ci_equals(name, "end"))
            easing.position = Easing::StepPosition::End;
        else
            return std::nullopt;
    }
    int const least = easing.position == Easing::StepPosition::JumpNone ? 2 : 1;
    ComponentValue const& count = *(*parts)[0][0];
    if (is_calc_function(count)) {
        // A calculation rounds to the nearest integer and is clamped to
        // what the position allows.
        std::optional<double> const number = parse_number_value(count);
        if (!number)
            return std::nullopt;
        double const rounded = std::floor(*number + 0.5);
        easing.steps = static_cast<int>(std::clamp(rounded, static_cast<double>(least), 1e9));
        return easing;
    }
    if (!count.is_token(Token::Type::Number) || count.token().numeric_type != Token::NumericType::Integer)
        return std::nullopt;
    double const number = count.token().numeric_value;
    if (number < least || number > 1e9)
        return std::nullopt;
    easing.steps = static_cast<int>(number);
    return easing;
}

std::optional<Easing> linear_function(std::vector<ComponentValue> const& arguments)
{
    auto const parts = comma_separated(arguments);
    if (!parts || parts->size() < 2)
        return std::nullopt;
    // css-easing-2 §2.2.3, "create a linear easing function".
    struct Point {
        double output;
        std::optional<double> input;
    };
    std::vector<Point> points;
    double largest = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < parts->size(); ++i) {
        auto const& stop = (*parts)[i];
        if (stop.empty() || stop.size() > 3)
            return std::nullopt;
        std::optional<double> output = parse_number_value(*stop[0]);
        if (!output)
            return std::nullopt;
        std::vector<double> lengths;
        for (std::size_t j = 1; j < stop.size(); ++j) {
            std::optional<double> const percent = parse_percentage_value(*stop[j]);
            if (!percent)
                return std::nullopt;
            lengths.push_back(*percent / 100.0);
        }
        points.push_back({ *output, std::nullopt });
        if (!lengths.empty()) {
            points.back().input = std::max(lengths[0], largest);
            largest = *points.back().input;
            if (lengths.size() > 1) {
                points.push_back({ *output, std::max(lengths[1], largest) });
                largest = *points.back().input;
            }
        } else if (i == 0) {
            points.back().input = 0;
            largest = 0;
        } else if (i + 1 == parts->size()) {
            points.back().input = std::max(1.0, largest);
        }
    }
    // The runs without an input share the room between their neighbours.
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].input)
            continue;
        std::size_t end = i;
        while (end < points.size() && !points[end].input)
            ++end;
        double const from = *points[i - 1].input;
        double const to = *points[end].input;
        std::size_t const gaps = end - i + 1;
        for (std::size_t k = i; k < end; ++k)
            points[k].input = from + (to - from) * static_cast<double>(k - i + 1) / static_cast<double>(gaps);
        i = end;
    }
    Easing easing;
    easing.kind = Easing::Kind::LinearPoints;
    for (Point const& point : points)
        easing.points.emplace_back(point.output, *point.input);
    return easing;
}

}

Easing Easing::ease() { return *keyword_easing("ease"); }

double Easing::apply(double input, bool before) const
{
    switch (kind) {
    case Kind::Linear: return input;
    case Kind::CubicBezier: {
        if (x1 == y1 && x2 == y2)
            return input;
        return Bezier(x1, y1, x2, y2).solve(input);
    }
    case Kind::Steps: return step_output(*this, input, before);
    case Kind::LinearPoints: return linear_points_output(*this, input);
    }
    return input;
}

std::string serialize_css_number(double value)
{
    if (std::isnan(value))
        return "NaN";
    if (std::isinf(value))
        return value > 0 ? "infinity" : "-infinity";
    // Six places after the point, the zeros after the last digit dropped,
    // as the engines write a computed number.
    double const rounded = std::round(value * 1e6) / 1e6;
    if (rounded == 0)
        return "0";
    char buffer[64];
    auto const [end, error] = std::to_chars(buffer, buffer + sizeof buffer, rounded, std::chars_format::fixed, 6);
    if (error != std::errc())
        return "0";
    std::string text(buffer, end);
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    return text;
}

std::string Easing::serialize() const
{
    switch (kind) {
    case Kind::Linear: return "linear";
    case Kind::CubicBezier:
        switch (keyword) {
        case Keyword::Ease: return "ease";
        case Keyword::EaseIn: return "ease-in";
        case Keyword::EaseOut: return "ease-out";
        case Keyword::EaseInOut: return "ease-in-out";
        default: break;
        }
        return "cubic-bezier(" + serialize_css_number(x1) + ", " + serialize_css_number(y1) + ", "
            + serialize_css_number(x2) + ", " + serialize_css_number(y2) + ")";
    case Kind::Steps: {
        std::string text = "steps(" + std::to_string(steps);
        switch (position) {
        case StepPosition::JumpStart: text += ", jump-start"; break;
        case StepPosition::Start: text += ", start"; break;
        case StepPosition::JumpNone: text += ", jump-none"; break;
        case StepPosition::JumpBoth: text += ", jump-both"; break;
        case StepPosition::JumpEnd:
        case StepPosition::End: break;
        }
        return text + ")";
    }
    case Kind::LinearPoints: {
        std::string text = "linear(";
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (i)
                text += ", ";
            text += serialize_css_number(points[i].first) + " " + serialize_css_number(points[i].second * 100) + "%";
        }
        return text + ")";
    }
    }
    return "linear";
}

std::optional<Easing> parse_easing(std::vector<ComponentValue> const& values)
{
    std::vector<ComponentValue const*> const items = significant_values(values);
    if (items.size() != 1)
        return std::nullopt;
    ComponentValue const& value = *items[0];
    if (value.is_token(Token::Type::Ident))
        return keyword_easing(value.token().value);
    if (!value.is_function())
        return std::nullopt;
    std::string_view const name = value.function().name;
    if (ascii_ci_equals(name, "cubic-bezier"))
        return cubic_bezier(value.function().values);
    if (ascii_ci_equals(name, "steps"))
        return steps(value.function().values);
    if (ascii_ci_equals(name, "linear"))
        return linear_function(value.function().values);
    return std::nullopt;
}

std::optional<Easing> parse_easing_text(std::string_view text)
{
    return parse_easing(parse_component_value_list(text));
}

}
