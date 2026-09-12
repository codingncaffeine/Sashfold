#include "svg/Svg.h"

#include "core/Bmp.h"
#include "core/Gif.h"
#include "core/Jpeg.h"
#include "core/Png.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "net/DataUrl.h"
#include "net/Url.h"
#include "text/FontManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sashfold::svg {

namespace {

// --- The microsyntaxes ----------------------------------------------------------

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// Reads SVG's number grammar: an optional sign, digits with an optional
// fraction, an optional exponent.
struct Scanner {
    std::string_view text;
    std::size_t pos = 0;

    bool at_end() const { return pos >= text.size(); }
    char peek() const { return at_end() ? '\0' : text[pos]; }

    void skip_space()
    {
        while (!at_end() && is_space(text[pos]))
            ++pos;
    }

    // Whitespace with at most one comma inside it.
    void skip_separator()
    {
        skip_space();
        if (!at_end() && text[pos] == ',') {
            ++pos;
            skip_space();
        }
    }

    bool number(float& out)
    {
        std::size_t i = pos;
        if (i < text.size() && (text[i] == '+' || text[i] == '-'))
            ++i;
        std::size_t const digits_start = i;
        while (i < text.size() && is_digit(text[i]))
            ++i;
        std::size_t fraction_digits = 0;
        if (i < text.size() && text[i] == '.') {
            std::size_t j = i + 1;
            while (j < text.size() && is_digit(text[j]))
                ++j;
            fraction_digits = j - i - 1;
            if (i == digits_start && fraction_digits == 0)
                return false;
            i = j;
        } else if (i == digits_start) {
            return false;
        }
        if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
            std::size_t j = i + 1;
            if (j < text.size() && (text[j] == '+' || text[j] == '-'))
                ++j;
            std::size_t const exponent_start = j;
            while (j < text.size() && is_digit(text[j]))
                ++j;
            if (j > exponent_start)
                i = j;
        }
        std::string const piece(text.substr(pos, i - pos));
        char* end = nullptr;
        double const value = std::strtod(piece.c_str(), &end);
        if (!std::isfinite(value))
            return false;
        out = static_cast<float>(value);
        pos = i;
        return true;
    }

    // An arc flag: a single 0 or 1, which may run straight into the next
    // number ("a1 1 0 00 10" is legal).
    bool flag(bool& out)
    {
        if (at_end() || (text[pos] != '0' && text[pos] != '1'))
            return false;
        out = text[pos] == '1';
        ++pos;
        return true;
    }
};

std::vector<float> parse_numbers(std::string_view text)
{
    std::vector<float> out;
    Scanner scanner { text };
    scanner.skip_space();
    while (!scanner.at_end()) {
        float value;
        if (!scanner.number(value))
            break;
        out.push_back(value);
        scanner.skip_separator();
    }
    return out;
}

// A length in SVG's units to px. `percent_base` is what a percentage is
// of; `em` the font size. A bare number is px (user units).
std::optional<float> parse_length(std::string_view text, float percent_base, float em)
{
    Scanner scanner { text };
    scanner.skip_space();
    float value;
    if (!scanner.number(value))
        return std::nullopt;
    std::string_view unit = text.substr(scanner.pos);
    while (!unit.empty() && is_space(unit.back()))
        unit.remove_suffix(1);
    if (unit.empty() || unit == "px")
        return value;
    if (unit == "%")
        return value / 100 * percent_base;
    if (unit == "em")
        return value * em;
    if (unit == "ex")
        return value * em / 2;
    if (unit == "pt")
        return value * 96 / 72;
    if (unit == "pc")
        return value * 16;
    if (unit == "in")
        return value * 96;
    if (unit == "cm")
        return value * 96 / 2.54f;
    if (unit == "mm")
        return value * 96 / 25.4f;
    if (unit == "rem")
        return value * 16;
    return std::nullopt;
}

bool ascii_ci_equals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y + 32);
        if (x != y)
            return false;
    }
    return true;
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    return text;
}

// The id a url(#id) or a bare #id names; empty when it is neither.
std::string reference_of(std::string_view text)
{
    text = trimmed(text);
    if (text.starts_with("url(")) {
        text.remove_prefix(4);
        std::size_t const close = text.find(')');
        if (close == std::string_view::npos)
            return {};
        text = trimmed(text.substr(0, close));
        if (text.size() >= 2 && (text.front() == '"' || text.front() == '\''))
            text = text.substr(1, text.size() - 2);
    }
    if (!text.starts_with('#'))
        return {};
    return std::string(text.substr(1));
}

} // namespace

Matrix parse_transform(std::string_view text)
{
    Matrix result;
    Scanner scanner { text };
    scanner.skip_space();
    while (!scanner.at_end()) {
        std::size_t const start = scanner.pos;
        while (!scanner.at_end() && ((scanner.peek() >= 'a' && scanner.peek() <= 'z') || (scanner.peek() >= 'A' && scanner.peek() <= 'Z')))
            ++scanner.pos;
        std::string_view const name = text.substr(start, scanner.pos - start);
        scanner.skip_space();
        if (scanner.peek() != '(')
            break;
        ++scanner.pos;
        std::vector<float> args;
        scanner.skip_space();
        while (!scanner.at_end() && scanner.peek() != ')') {
            float value;
            if (!scanner.number(value))
                return result; // a malformed list applies nothing after it
            args.push_back(value);
            scanner.skip_separator();
        }
        if (scanner.peek() != ')')
            break;
        ++scanner.pos;
        Matrix step;
        if (name == "matrix" && args.size() == 6) {
            step = Matrix { args[0], args[1], args[2], args[3], args[4], args[5] };
        } else if (name == "translate" && (args.size() == 1 || args.size() == 2)) {
            step = Matrix::translate(args[0], args.size() == 2 ? args[1] : 0);
        } else if (name == "scale" && (args.size() == 1 || args.size() == 2)) {
            step = Matrix::scale(args[0], args.size() == 2 ? args[1] : args[0]);
        } else if (name == "rotate" && (args.size() == 1 || args.size() == 3)) {
            step = Matrix::rotate(args[0]);
            if (args.size() == 3)
                step = Matrix::translate(-args[1], -args[2]).then(step).then(Matrix::translate(args[1], args[2]));
        } else if (name == "skewX" && args.size() == 1) {
            step = Matrix::skew_x(args[0]);
        } else if (name == "skewY" && args.size() == 1) {
            step = Matrix::skew_y(args[0]);
        } else {
            return result;
        }
        // Later items in the list apply first to a point: the list is a
        // product written left to right.
        result = step.then(result);
        scanner.skip_separator();
    }
    return result;
}

Path parse_path_data(std::string_view data)
{
    Path path;
    Scanner scanner { data };
    scanner.skip_space();
    char command = '\0';
    Point pen;
    Point start;
    // The last control point, for the smooth curve commands' reflection.
    std::optional<Point> last_cubic_control;
    std::optional<Point> last_quadratic_control;
    auto const number = [&](float& out) {
        scanner.skip_separator();
        return scanner.number(out);
    };
    auto const point = [&](Point& out, bool relative) {
        float x;
        float y;
        if (!number(x) || !number(y))
            return false;
        out = relative ? Point { pen.x + x, pen.y + y } : Point { x, y };
        return true;
    };
    while (!scanner.at_end()) {
        scanner.skip_space();
        if (scanner.at_end())
            break;
        char const c = scanner.peek();
        bool const letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (letter) {
            command = c;
            ++scanner.pos;
        } else if (command == '\0') {
            break; // numbers before any command
        } else if (command == 'M') {
            command = 'L'; // repeated moveto coordinates are linetos
        } else if (command == 'm') {
            command = 'l';
        } else if (command == 'Z' || command == 'z') {
            break; // nothing may follow a closepath but a command
        }
        bool const relative = command >= 'a' && command <= 'z';
        char const upper = relative ? static_cast<char>(command - 32) : command;
        std::optional<Point> cubic_control;
        std::optional<Point> quadratic_control;
        switch (upper) {
        case 'M': {
            Point p;
            if (!point(p, relative))
                return path;
            path.move_to(p);
            pen = start = p;
            break;
        }
        case 'L': {
            Point p;
            if (!point(p, relative))
                return path;
            path.line_to(p);
            pen = p;
            break;
        }
        case 'H': {
            float x;
            if (!number(x))
                return path;
            pen = Point { relative ? pen.x + x : x, pen.y };
            path.line_to(pen);
            break;
        }
        case 'V': {
            float y;
            if (!number(y))
                return path;
            pen = Point { pen.x, relative ? pen.y + y : y };
            path.line_to(pen);
            break;
        }
        case 'C': {
            Point c1;
            Point c2;
            Point p;
            if (!point(c1, relative) || !point(c2, relative) || !point(p, relative))
                return path;
            path.cubic_to(c1, c2, p);
            cubic_control = c2;
            pen = p;
            break;
        }
        case 'S': {
            Point c2;
            Point p;
            if (!point(c2, relative) || !point(p, relative))
                return path;
            Point const c1 = last_cubic_control
                ? Point { 2 * pen.x - last_cubic_control->x, 2 * pen.y - last_cubic_control->y }
                : pen;
            path.cubic_to(c1, c2, p);
            cubic_control = c2;
            pen = p;
            break;
        }
        case 'Q': {
            Point control;
            Point p;
            if (!point(control, relative) || !point(p, relative))
                return path;
            path.quadratic_to(control, p);
            quadratic_control = control;
            pen = p;
            break;
        }
        case 'T': {
            Point p;
            if (!point(p, relative))
                return path;
            Point const control = last_quadratic_control
                ? Point { 2 * pen.x - last_quadratic_control->x, 2 * pen.y - last_quadratic_control->y }
                : pen;
            path.quadratic_to(control, p);
            quadratic_control = control;
            pen = p;
            break;
        }
        case 'A': {
            float rx;
            float ry;
            float rotation;
            bool large;
            bool sweep;
            if (!number(rx) || !number(ry) || !number(rotation))
                return path;
            scanner.skip_separator();
            if (!scanner.flag(large))
                return path;
            scanner.skip_separator();
            if (!scanner.flag(sweep))
                return path;
            Point p;
            if (!point(p, relative))
                return path;
            path.arc_to(rx, ry, rotation, large, sweep, p);
            pen = p;
            break;
        }
        case 'Z':
            path.close();
            pen = start;
            break;
        default:
            return path;
        }
        last_cubic_control = cubic_control;
        last_quadratic_control = quadratic_control;
        if (path.segments.size() > 200000)
            return path; // enough of anything
    }
    return path;
}

namespace {

// --- The document ---------------------------------------------------------------

std::string_view attribute(dom::Element const& element, std::string_view name)
{
    dom::Attr const* const found = element.find_attribute(name);
    return found ? std::string_view(found->value) : std::string_view();
}

// href, or the older xlink:href.
std::string_view href_of(dom::Element const& element)
{
    for (dom::Attr const& attr : element.attributes()) {
        if (attr.local_name == "href")
            return attr.value;
    }
    return {};
}

struct ViewBox {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

std::optional<ViewBox> parse_view_box(std::string_view text)
{
    std::vector<float> const numbers = parse_numbers(text);
    if (numbers.size() != 4 || !(numbers[2] > 0) || !(numbers[3] > 0))
        return std::nullopt;
    return ViewBox { numbers[0], numbers[1], numbers[2], numbers[3] };
}

// A viewBox with a zero width or height disables the element's rendering
// (SVG 2 §8.8); as a picture, it is one with nothing in it.
bool view_box_disables(std::string_view text)
{
    std::vector<float> const numbers = parse_numbers(text);
    return numbers.size() == 4 && (numbers[2] <= 0 || numbers[3] <= 0);
}

// preserveAspectRatio: the alignment and whether the viewBox is fitted
// inside (meet) or made to cover (slice) the viewport, or stretched.
struct AspectRatio {
    bool none = false;
    float align_x = 0.5f; // 0 = min, 0.5 = mid, 1 = max
    float align_y = 0.5f;
    bool slice = false;
};

AspectRatio parse_aspect_ratio(std::string_view text)
{
    AspectRatio ratio;
    Scanner scanner { text };
    scanner.skip_space();
    std::size_t const start = scanner.pos;
    while (!scanner.at_end() && !is_space(scanner.peek()))
        ++scanner.pos;
    std::string_view const align = text.substr(start, scanner.pos - start);
    if (align == "none") {
        ratio.none = true;
    } else if (align.size() == 8 && align.starts_with('x')) {
        auto const part = [](std::string_view word) -> std::optional<float> {
            if (word == "Min")
                return 0.0f;
            if (word == "Mid")
                return 0.5f;
            if (word == "Max")
                return 1.0f;
            return std::nullopt;
        };
        std::optional<float> const x = part(align.substr(1, 3));
        std::optional<float> const y = align[4] == 'Y' ? part(align.substr(5, 3)) : std::nullopt;
        if (x && y) {
            ratio.align_x = *x;
            ratio.align_y = *y;
        }
    }
    scanner.skip_space();
    if (text.substr(scanner.pos).starts_with("slice"))
        ratio.slice = true;
    return ratio;
}

// The transform that puts a viewBox into a viewport (§8.8).
Matrix view_box_transform(std::optional<ViewBox> const& box, AspectRatio const& ratio, float x, float y,
    float width, float height)
{
    if (!box || width <= 0 || height <= 0)
        return Matrix::translate(x, y);
    float sx = width / box->width;
    float sy = height / box->height;
    if (!ratio.none) {
        float const s = ratio.slice ? std::max(sx, sy) : std::min(sx, sy);
        sx = sy = s;
    }
    float const tx = x - box->x * sx + (width - box->width * sx) * ratio.align_x;
    float const ty = y - box->y * sy + (height - box->height * sy) * ratio.align_y;
    return Matrix { sx, 0, 0, sy, tx, ty };
}

struct Context {
    dom::Element const& root;
    css::StyleMap const& styles;
    Bitmap& target;
    std::unordered_map<std::string, dom::Element const*> ids;
    // A budget on the work one document may ask for, so a hostile file
    // cannot spin the renderer: every shape and every text run spends one.
    int budget = 200000;
};

struct State {
    Matrix ctm;
    float opacity = 1;
    // The viewport a percentage is of, in user units.
    float viewport_width = 0;
    float viewport_height = 0;
    std::shared_ptr<Mask const> clip; // null: none
    int depth = 0;
};

css::ComputedStyle const* style_of(Context const& context, dom::Element const& element)
{
    auto const it = context.styles.find(&element);
    return it == context.styles.end() ? nullptr : &it->second;
}

void collect_ids(Context& context, dom::Node const& node)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        std::string_view const id = attribute(element, "id");
        if (!id.empty())
            context.ids.emplace(std::string(id), &element);
    }
    for (dom::Node const* child : node.children())
        collect_ids(context, *child);
}

dom::Element const* element_by_id(Context const& context, std::string const& id)
{
    auto const it = context.ids.find(id);
    return it == context.ids.end() ? nullptr : it->second;
}

float diagonal_base(State const& state)
{
    // A percentage length that is neither horizontal nor vertical — a
    // radius, a stroke width — is of the viewport's normalized diagonal.
    return std::sqrt((state.viewport_width * state.viewport_width + state.viewport_height * state.viewport_height) / 2);
}

float length_attribute(dom::Element const& element, std::string_view name, float percent_base, float em,
    float fallback = 0)
{
    std::string_view const text = attribute(element, name);
    if (text.empty())
        return fallback;
    return parse_length(text, percent_base, em).value_or(fallback);
}

float em_of(css::ComputedStyle const* style)
{
    return style ? style->font_size : 16.0f;
}

// --- Paint ----------------------------------------------------------------------

struct GradientStop {
    float offset;
    Color color;
};

struct Gradient {
    bool radial = false;
    Point p1; // linear: from
    Point p2; // linear: to
    Point centre; // radial
    Point focus;
    float radius = 0;
    Matrix to_gradient; // device px -> gradient space
    enum class Spread : std::uint8_t { Pad, Reflect, Repeat } spread = Spread::Pad;
    std::vector<GradientStop> stops;
};

struct ResolvedPaint {
    bool none = true;
    Color color;
    std::shared_ptr<Gradient const> gradient;
};

Color mix(Color a, Color b, float t)
{
    auto const channel = [&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(x + (y - x) * t), 0L, 255L));
    };
    return Color { channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a) };
}

Color gradient_color_at(Gradient const& gradient, float x, float y)
{
    if (gradient.stops.empty())
        return Color::rgba(0, 0, 0, 0);
    Point const p = gradient.to_gradient.apply(Point { x, y });
    float t;
    if (!gradient.radial) {
        float const dx = gradient.p2.x - gradient.p1.x;
        float const dy = gradient.p2.y - gradient.p1.y;
        float const len2 = dx * dx + dy * dy;
        t = len2 > 0 ? ((p.x - gradient.p1.x) * dx + (p.y - gradient.p1.y) * dy) / len2 : 0;
    } else {
        // Where the ray from the focus through the point meets the circle:
        // the fraction of the way there the point is (§14.2.3).
        float const fx = gradient.focus.x;
        float const fy = gradient.focus.y;
        float const cx = gradient.centre.x;
        float const cy = gradient.centre.y;
        float const r = gradient.radius;
        if (r <= 0) {
            t = 1;
        } else {
            float const dx = p.x - fx;
            float const dy = p.y - fy;
            float const ex = fx - cx;
            float const ey = fy - cy;
            float const a = dx * dx + dy * dy;
            if (a <= 1e-12f) {
                t = 0;
            } else {
                float const b = 2 * (dx * ex + dy * ey);
                float const c = ex * ex + ey * ey - r * r;
                float const disc = b * b - 4 * a * c;
                float const s = disc > 0 ? (-b + std::sqrt(disc)) / (2 * a) : 0;
                t = s > 0 ? 1 / s : 1;
            }
        }
    }
    switch (gradient.spread) {
    case Gradient::Spread::Pad:
        t = std::clamp(t, 0.0f, 1.0f);
        break;
    case Gradient::Spread::Repeat:
        t = t - std::floor(t);
        break;
    case Gradient::Spread::Reflect: {
        float const m = std::fmod(std::abs(t), 2.0f);
        t = m > 1 ? 2 - m : m;
        break;
    }
    }
    if (t <= gradient.stops.front().offset)
        return gradient.stops.front().color;
    for (std::size_t i = 1; i < gradient.stops.size(); ++i) {
        GradientStop const& a = gradient.stops[i - 1];
        GradientStop const& b = gradient.stops[i];
        if (t <= b.offset) {
            float const span = b.offset - a.offset;
            return span > 0 ? mix(a.color, b.color, (t - a.offset) / span) : b.color;
        }
    }
    return gradient.stops.back().color;
}

bool is_gradient(dom::Element const& element)
{
    return element.is_svg("linearGradient") || element.is_svg("radialGradient");
}

// A gradient element's attribute, following its href chain when it
// leaves one out (§14.2.2).
std::string_view gradient_attribute(Context const& context, dom::Element const& element, std::string_view name)
{
    dom::Element const* current = &element;
    for (int hops = 0; current && hops < 8; ++hops) {
        std::string_view const value = attribute(*current, name);
        if (!value.empty())
            return value;
        std::string const id = reference_of(href_of(*current));
        current = id.empty() ? nullptr : element_by_id(context, id);
        if (current && !is_gradient(*current))
            break;
    }
    return {};
}

// The stops: the element's own, else the first in its chain that has any.
void gradient_stops(Context const& context, dom::Element const& element, std::vector<GradientStop>& out)
{
    dom::Element const* current = &element;
    for (int hops = 0; current && hops < 8; ++hops) {
        for (dom::Node const* child : current->children()) {
            if (!child->is_element())
                continue;
            auto const& stop = static_cast<dom::Element const&>(*child);
            if (!stop.is_svg("stop"))
                continue;
            css::ComputedStyle const* style = style_of(context, stop);
            std::string_view const text = attribute(stop, "offset");
            float offset = 0;
            if (!text.empty()) {
                Scanner scanner { text };
                scanner.skip_space();
                if (scanner.number(offset) && text.substr(scanner.pos).starts_with('%'))
                    offset /= 100;
            }
            offset = std::clamp(offset, 0.0f, 1.0f);
            // Offsets never go backwards: each is at least the one before.
            if (!out.empty())
                offset = std::max(offset, out.back().offset);
            Color color = style ? style->stop_color : Color::rgb(0, 0, 0);
            float const opacity = style ? style->stop_opacity : 1.0f;
            color.a = static_cast<std::uint8_t>(std::lround(color.a * std::clamp(opacity, 0.0f, 1.0f)));
            out.push_back(GradientStop { offset, color });
        }
        if (!out.empty())
            return;
        std::string const id = reference_of(href_of(*current));
        current = id.empty() ? nullptr : element_by_id(context, id);
        if (current && !is_gradient(*current))
            break;
    }
}

// Builds the gradient a shape is painted with: its geometry in the
// shape's bounding box (the default) or in user space, with its own
// transform, mapped back from device pixels.
std::shared_ptr<Gradient const> build_gradient(Context const& context, State const& state,
    dom::Element const& element, css::ComputedStyle const* style, std::optional<Box> const& bbox)
{
    auto gradient = std::make_shared<Gradient>();
    gradient->radial = element.is_svg("radialGradient");
    bool const in_bbox = !ascii_ci_equals(trimmed(gradient_attribute(context, element, "gradientUnits")), "userSpaceOnUse");
    if (in_bbox && (!bbox || bbox->right - bbox->left <= 0 || bbox->bottom - bbox->top <= 0))
        return nullptr; // nothing to be a fraction of
    float const em = em_of(style);
    auto const coordinate = [&](std::string_view name, float fallback_fraction, bool horizontal) {
        std::string_view const text = gradient_attribute(context, element, name);
        if (in_bbox) {
            if (text.empty())
                return fallback_fraction;
            Scanner scanner { text };
            scanner.skip_space();
            float value;
            if (!scanner.number(value))
                return fallback_fraction;
            return text.substr(scanner.pos).starts_with('%') ? value / 100 : value;
        }
        float const base = horizontal ? state.viewport_width : state.viewport_height;
        if (text.empty())
            return fallback_fraction * base;
        return parse_length(text, base, em).value_or(fallback_fraction * base);
    };
    if (!gradient->radial) {
        gradient->p1 = Point { coordinate("x1", 0, true), coordinate("y1", 0, false) };
        gradient->p2 = Point { coordinate("x2", 1, true), coordinate("y2", 0, false) };
    } else {
        gradient->centre = Point { coordinate("cx", 0.5f, true), coordinate("cy", 0.5f, false) };
        std::string_view const r = gradient_attribute(context, element, "r");
        if (in_bbox) {
            float value = 0.5f;
            if (!r.empty()) {
                Scanner scanner { r };
                scanner.skip_space();
                if (scanner.number(value) && r.substr(scanner.pos).starts_with('%'))
                    value /= 100;
            }
            gradient->radius = value;
        } else {
            gradient->radius = r.empty() ? 0.5f * diagonal_base(state) : parse_length(r, diagonal_base(state), em).value_or(0);
        }
        std::string_view const fx = gradient_attribute(context, element, "fx");
        std::string_view const fy = gradient_attribute(context, element, "fy");
        gradient->focus = Point { fx.empty() ? gradient->centre.x : coordinate("fx", 0.5f, true),
            fy.empty() ? gradient->centre.y : coordinate("fy", 0.5f, false) };
        // A focus outside the circle is moved onto its edge.
        float const dx = gradient->focus.x - gradient->centre.x;
        float const dy = gradient->focus.y - gradient->centre.y;
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist > gradient->radius * 0.999f && dist > 0) {
            float const s = gradient->radius * 0.999f / dist;
            gradient->focus = Point { gradient->centre.x + dx * s, gradient->centre.y + dy * s };
        }
    }
    std::string_view const spread = trimmed(gradient_attribute(context, element, "spreadMethod"));
    if (spread == "reflect")
        gradient->spread = Gradient::Spread::Reflect;
    else if (spread == "repeat")
        gradient->spread = Gradient::Spread::Repeat;
    gradient_stops(context, element, gradient->stops);
    if (gradient->stops.empty())
        return nullptr;
    // Gradient space to user space, then to the device.
    Matrix to_user = parse_transform(gradient_attribute(context, element, "gradientTransform"));
    if (in_bbox)
        to_user = to_user.then(Matrix::scale(bbox->right - bbox->left, bbox->bottom - bbox->top))
                      .then(Matrix::translate(bbox->left, bbox->top));
    std::optional<Matrix> const inverse = to_user.then(state.ctm).inverse();
    if (!inverse)
        return nullptr;
    gradient->to_gradient = *inverse;
    return gradient;
}

ResolvedPaint resolve_paint(Context const& context, State const& state, css::SvgPaint const& paint,
    css::ComputedStyle const* style, std::optional<Box> const& bbox)
{
    ResolvedPaint out;
    switch (paint.kind) {
    case css::SvgPaint::Kind::None:
        return out;
    case css::SvgPaint::Kind::Color:
        out.none = false;
        out.color = paint.color;
        return out;
    case css::SvgPaint::Kind::CurrentColor:
        out.none = false;
        out.color = style ? style->color : Color::rgb(0, 0, 0);
        return out;
    case css::SvgPaint::Kind::Reference: {
        dom::Element const* const server = element_by_id(context, paint.reference);
        if (server && is_gradient(*server)) {
            std::shared_ptr<Gradient const> gradient = build_gradient(context, state, *server, style, bbox);
            if (gradient) {
                out.none = false;
                out.gradient = std::move(gradient);
                return out;
            }
            // A gradient with no stops paints nothing (§14.2.2).
            if (server->children().empty() && !paint.has_fallback)
                return out;
        }
        if (paint.has_fallback) {
            out.none = false;
            out.color = paint.color;
        }
        return out;
    }
    }
    return out;
}

// --- Drawing --------------------------------------------------------------------

void paint_mask(Context& context, State const& state, Mask const& mask, ResolvedPaint const& paint, float alpha)
{
    if (mask.empty() || paint.none || alpha <= 0)
        return;
    Bitmap& target = context.target;
    for (int row = 0; row < mask.height; ++row) {
        int const y = mask.top + row;
        if (y < 0 || y >= target.height())
            continue;
        for (int column = 0; column < mask.width; ++column) {
            int const x = mask.left + column;
            if (x < 0 || x >= target.width())
                continue;
            unsigned coverage = mask.alpha[static_cast<std::size_t>(row) * static_cast<std::size_t>(mask.width)
                + static_cast<std::size_t>(column)];
            if (coverage == 0)
                continue;
            if (state.clip) {
                coverage = coverage * state.clip->at(x, y) / 255;
                if (coverage == 0)
                    continue;
            }
            Color color = paint.gradient
                ? gradient_color_at(*paint.gradient, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f)
                : paint.color;
            float const a = static_cast<float>(color.a) * static_cast<float>(coverage) / 255.0f * alpha;
            color.a = static_cast<std::uint8_t>(std::clamp(std::lround(a), 0L, 255L));
            target.blend_pixel(x, y, color);
        }
    }
}

Mask rasterize_in(Context const& context, std::vector<Polygon> const& polygons, FillRule rule)
{
    return rasterize(polygons, rule, 0, 0, context.target.width(), context.target.height());
}

FillRule to_rule(css::FillRule rule)
{
    return rule == css::FillRule::EvenOdd ? FillRule::EvenOdd : FillRule::NonZero;
}

std::vector<Polygon> fill_polygons(std::vector<Polyline> const& lines)
{
    std::vector<Polygon> out;
    for (Polyline const& line : lines) {
        if (line.points.size() >= 3)
            out.push_back(line.points);
    }
    return out;
}

// Fills and strokes a path in user space, as the style says.
void draw_path(Context& context, State const& state, css::ComputedStyle const& style, Path const& path)
{
    if (context.budget-- <= 0)
        return;
    if (style.visibility == css::Visibility::Hidden)
        return;
    std::vector<Polyline> const user_lines = flatten(path, 0.1f / std::max(state.ctm.scale_factor(), 0.001f));
    std::vector<Polygon> const user_polygons = fill_polygons(user_lines);
    std::optional<Box> const bbox = bounds_of(user_polygons);
    // The fill.
    if (style.fill.kind != css::SvgPaint::Kind::None) {
        std::vector<Polygon> device;
        for (Polygon const& polygon : user_polygons) {
            Polygon moved;
            moved.reserve(polygon.size());
            for (Point const& p : polygon)
                moved.push_back(state.ctm.apply(p));
            device.push_back(std::move(moved));
        }
        ResolvedPaint const paint = resolve_paint(context, state, style.fill, &style, bbox);
        if (!paint.none)
            paint_mask(context, state, rasterize_in(context, device, to_rule(style.fill_rule)), paint,
                state.opacity * std::clamp(style.fill_opacity, 0.0f, 1.0f));
    }
    // The stroke, built in user space so a transform scales it as it
    // scales the shape, then moved to the device.
    if (style.stroke.kind != css::SvgPaint::Kind::None) {
        StrokeStyle stroke_style;
        float width = 1;
        if (style.stroke_width.kind == css::LengthPercent::Kind::Px)
            width = style.stroke_width.value;
        else if (style.stroke_width.kind == css::LengthPercent::Kind::Percent)
            width = style.stroke_width.value / 100 * diagonal_base(state);
        stroke_style.width = width;
        stroke_style.cap = style.stroke_linecap == css::StrokeLineCap::Round ? LineCap::Round
            : style.stroke_linecap == css::StrokeLineCap::Square             ? LineCap::Square
                                                                             : LineCap::Butt;
        stroke_style.join = style.stroke_linejoin == css::StrokeLineJoin::Round ? LineJoin::Round
            : style.stroke_linejoin == css::StrokeLineJoin::Bevel              ? LineJoin::Bevel
                                                                               : LineJoin::Miter;
        stroke_style.miter_limit = std::max(1.0f, style.stroke_miterlimit);
        if (style.stroke_dasharray)
            stroke_style.dashes = *style.stroke_dasharray;
        stroke_style.dash_offset = style.stroke_dashoffset;
        if (stroke_style.width > 0) {
            std::vector<Polygon> outline = stroke(user_lines, stroke_style);
            for (Polygon& polygon : outline) {
                for (Point& p : polygon)
                    p = state.ctm.apply(p);
            }
            ResolvedPaint const paint = resolve_paint(context, state, style.stroke, &style, bbox);
            if (!paint.none)
                paint_mask(context, state, rasterize_in(context, outline, FillRule::NonZero), paint,
                    state.opacity * std::clamp(style.stroke_opacity, 0.0f, 1.0f));
        }
    }
}

// --- Shapes ---------------------------------------------------------------------

Path rect_path(float x, float y, float width, float height, float rx, float ry)
{
    Path path;
    if (width <= 0 || height <= 0)
        return path;
    if (rx < 0 && ry < 0) {
        rx = ry = 0;
    } else {
        // One radius given stands for both (§10.2).
        if (rx < 0)
            rx = ry;
        if (ry < 0)
            ry = rx;
    }
    rx = std::min(rx, width / 2);
    ry = std::min(ry, height / 2);
    if (rx <= 0 || ry <= 0) {
        path.move_to(Point { x, y });
        path.line_to(Point { x + width, y });
        path.line_to(Point { x + width, y + height });
        path.line_to(Point { x, y + height });
        path.close();
        return path;
    }
    path.move_to(Point { x + rx, y });
    path.line_to(Point { x + width - rx, y });
    path.arc_to(rx, ry, 0, false, true, Point { x + width, y + ry });
    path.line_to(Point { x + width, y + height - ry });
    path.arc_to(rx, ry, 0, false, true, Point { x + width - rx, y + height });
    path.line_to(Point { x + rx, y + height });
    path.arc_to(rx, ry, 0, false, true, Point { x, y + height - ry });
    path.line_to(Point { x, y + ry });
    path.arc_to(rx, ry, 0, false, true, Point { x + rx, y });
    path.close();
    return path;
}

Path ellipse_path(float cx, float cy, float rx, float ry)
{
    Path path;
    if (rx <= 0 || ry <= 0)
        return path;
    path.move_to(Point { cx + rx, cy });
    path.arc_to(rx, ry, 0, false, true, Point { cx, cy + ry });
    path.arc_to(rx, ry, 0, false, true, Point { cx - rx, cy });
    path.arc_to(rx, ry, 0, false, true, Point { cx, cy - ry });
    path.arc_to(rx, ry, 0, false, true, Point { cx + rx, cy });
    path.close();
    return path;
}

Path points_path(std::string_view text, bool closed)
{
    Path path;
    std::vector<float> const numbers = parse_numbers(text);
    for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
        Point const p { numbers[i], numbers[i + 1] };
        if (i == 0)
            path.move_to(p);
        else
            path.line_to(p);
    }
    if (closed && !path.empty())
        path.close();
    return path;
}

// The path of one of the basic shapes, in user units; nullopt for an
// element that is not one.
std::optional<Path> shape_path(State const& state, dom::Element const& element, css::ComputedStyle const* style)
{
    float const em = em_of(style);
    float const vw = state.viewport_width;
    float const vh = state.viewport_height;
    std::string const& name = element.local_name();
    if (name == "path")
        return parse_path_data(attribute(element, "d"));
    if (name == "rect") {
        return rect_path(length_attribute(element, "x", vw, em), length_attribute(element, "y", vh, em),
            length_attribute(element, "width", vw, em), length_attribute(element, "height", vh, em),
            length_attribute(element, "rx", vw, em, -1), length_attribute(element, "ry", vh, em, -1));
    }
    if (name == "circle") {
        float const r = length_attribute(element, "r", diagonal_base(state), em);
        return ellipse_path(length_attribute(element, "cx", vw, em), length_attribute(element, "cy", vh, em), r, r);
    }
    if (name == "ellipse") {
        float rx = length_attribute(element, "rx", vw, em, -1);
        float ry = length_attribute(element, "ry", vh, em, -1);
        if (rx < 0)
            rx = ry;
        if (ry < 0)
            ry = rx;
        return ellipse_path(length_attribute(element, "cx", vw, em), length_attribute(element, "cy", vh, em), rx, ry);
    }
    if (name == "line") {
        Path path;
        path.move_to(Point { length_attribute(element, "x1", vw, em), length_attribute(element, "y1", vh, em) });
        path.line_to(Point { length_attribute(element, "x2", vw, em), length_attribute(element, "y2", vh, em) });
        return path;
    }
    if (name == "polyline")
        return points_path(attribute(element, "points"), false);
    if (name == "polygon")
        return points_path(attribute(element, "points"), true);
    return std::nullopt;
}

// --- Clip paths -----------------------------------------------------------------

void render_element(Context& context, State state, dom::Element const& element);

// Gathers the device-space fill of every shape inside a <clipPath> — the
// union is the clip — walking <use> and <g> but no further than one clip
// deep, since a clip path may name a clip path of its own.
void clip_polygons(Context& context, State const& state, dom::Element const& element,
    std::vector<std::pair<std::vector<Polygon>, FillRule>>& out, int depth)
{
    if (depth > 8)
        return;
    for (dom::Node const* child : element.children()) {
        if (!child->is_element())
            continue;
        auto const& shape = static_cast<dom::Element const&>(*child);
        css::ComputedStyle const* style = style_of(context, shape);
        if (style && style->display == css::Display::None)
            continue;
        State inner = state;
        inner.ctm = parse_transform(attribute(shape, "transform")).then(state.ctm);
        if (shape.is_svg("use")) {
            std::string const id = reference_of(href_of(shape));
            dom::Element const* const target = id.empty() ? nullptr : element_by_id(context, id);
            if (!target || target == &element)
                continue;
            float const em = em_of(style);
            inner.ctm = Matrix::translate(length_attribute(shape, "x", state.viewport_width, em),
                            length_attribute(shape, "y", state.viewport_height, em))
                            .then(inner.ctm);
            // A used shape: its path, at the use's place.
            if (std::optional<Path> path = shape_path(inner, *target, style_of(context, *target))) {
                State used = inner;
                used.ctm = parse_transform(attribute(*target, "transform")).then(inner.ctm);
                std::vector<Polygon> device;
                for (Polygon& polygon : fill_polygons(flatten(*path))) {
                    for (Point& p : polygon)
                        p = used.ctm.apply(p);
                    device.push_back(std::move(polygon));
                }
                css::ComputedStyle const* const used_style = style_of(context, *target);
                out.emplace_back(std::move(device), used_style ? to_rule(used_style->fill_rule) : FillRule::NonZero);
            }
            continue;
        }
        if (shape.is_svg("g")) {
            clip_polygons(context, inner, shape, out, depth + 1);
            continue;
        }
        if (std::optional<Path> path = shape_path(inner, shape, style)) {
            std::vector<Polygon> device;
            for (Polygon& polygon : fill_polygons(flatten(*path))) {
                for (Point& p : polygon)
                    p = inner.ctm.apply(p);
                device.push_back(std::move(polygon));
            }
            out.emplace_back(std::move(device), style ? to_rule(style->fill_rule) : FillRule::NonZero);
        }
    }
}

// The clip an element asks for, intersected with the one in force.
std::shared_ptr<Mask const> clip_for(Context& context, State const& state, dom::Element const& element,
    std::optional<Box> const& bbox)
{
    std::string_view text = attribute(element, "clip-path");
    std::string const id = reference_of(text);
    if (id.empty())
        return state.clip;
    dom::Element const* const clip = element_by_id(context, id);
    if (!clip || !clip->is_svg("clipPath")) {
        // A reference to nothing clips everything away (§14.3.5 — an
        // invalid reference is an error, and the element is not rendered).
        auto nothing = std::make_shared<Mask>();
        return nothing;
    }
    State clip_state = state;
    clip_state.ctm = parse_transform(attribute(*clip, "transform")).then(state.ctm);
    if (ascii_ci_equals(trimmed(attribute(*clip, "clipPathUnits")), "objectBoundingBox")) {
        if (!bbox || bbox->right - bbox->left <= 0 || bbox->bottom - bbox->top <= 0)
            return std::make_shared<Mask>();
        clip_state.ctm = Matrix::scale(bbox->right - bbox->left, bbox->bottom - bbox->top)
                             .then(Matrix::translate(bbox->left, bbox->top))
                             .then(clip_state.ctm);
    }
    std::vector<std::pair<std::vector<Polygon>, FillRule>> shapes;
    clip_polygons(context, clip_state, *clip, shapes, 0);
    // The union of the shapes, each by its own rule, over the whole target.
    auto mask = std::make_shared<Mask>();
    mask->left = 0;
    mask->top = 0;
    mask->width = context.target.width();
    mask->height = context.target.height();
    mask->alpha.assign(static_cast<std::size_t>(mask->width) * static_cast<std::size_t>(mask->height), 0);
    for (auto const& [polygons, rule] : shapes) {
        Mask const piece = rasterize_in(context, polygons, rule);
        for (int row = 0; row < piece.height; ++row) {
            for (int column = 0; column < piece.width; ++column) {
                int const x = piece.left + column;
                int const y = piece.top + row;
                if (x < 0 || y < 0 || x >= mask->width || y >= mask->height)
                    continue;
                std::uint8_t& cell = mask->alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(mask->width)
                    + static_cast<std::size_t>(x)];
                cell = std::max(cell, piece.alpha[static_cast<std::size_t>(row) * static_cast<std::size_t>(piece.width)
                    + static_cast<std::size_t>(column)]);
            }
        }
    }
    if (state.clip) {
        for (int y = 0; y < mask->height; ++y) {
            for (int x = 0; x < mask->width; ++x) {
                std::uint8_t& cell = mask->alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(mask->width)
                    + static_cast<std::size_t>(x)];
                cell = static_cast<std::uint8_t>(cell * state.clip->at(x, y) / 255);
            }
        }
    }
    return mask;
}

// A shape's bounding box in user space, for clips and gradients in
// bounding-box units.
std::optional<Box> bbox_of(Path const& path)
{
    return bounds_of(fill_polygons(flatten(path)));
}

// --- Text -----------------------------------------------------------------------

// The text-anchor in force: the nearest ancestor that writes one.
std::string_view text_anchor_of(Context const& context, dom::Element const& element)
{
    dom::Node const* node = &element;
    while (node && node->is_element()) {
        auto const& current = static_cast<dom::Element const&>(*node);
        std::string_view const anchor = trimmed(attribute(current, "text-anchor"));
        if (!anchor.empty())
            return anchor;
        if (&current == &context.root)
            break;
        node = node->parent();
    }
    return "start";
}

// A text node's characters with its whitespace collapsed, as xml:space
// default does: runs of it become one space, and none leads.
std::u32string collapsed_text(std::string const& raw)
{
    std::u32string out;
    bool pending_space = false;
    // UTF-8 to code points, leniently: the tree holds well-formed text.
    for (std::size_t i = 0; i < raw.size();) {
        unsigned char const c = static_cast<unsigned char>(raw[i]);
        char32_t code = c;
        std::size_t length = 1;
        if (c >= 0xF0) {
            length = 4;
            code = c & 0x07u;
        } else if (c >= 0xE0) {
            length = 3;
            code = c & 0x0Fu;
        } else if (c >= 0xC0) {
            length = 2;
            code = c & 0x1Fu;
        }
        for (std::size_t k = 1; k < length && i + k < raw.size(); ++k)
            code = (code << 6) | (static_cast<unsigned char>(raw[i + k]) & 0x3Fu);
        i += length;
        if (code == ' ' || code == '\t' || code == '\n' || code == '\r') {
            pending_space = true;
            continue;
        }
        if (pending_space && !out.empty())
            out.push_back(U' ');
        pending_space = false;
        out.push_back(code);
    }
    return out;
}

// A first cut of <text>: the string drawn from its anchor in the font its
// style names, the size and the position through the transform, upright
// whatever the transform turns — a rotated label is drawn level. <tspan>
// children follow on, taking a new position when they write one.
void render_text_element(Context& context, State const& state, dom::Element const& element,
    css::ComputedStyle const& style, Point& pen, bool& pen_set)
{
    if (context.budget-- <= 0)
        return;
    std::vector<float> const xs = parse_numbers(attribute(element, "x"));
    std::vector<float> const ys = parse_numbers(attribute(element, "y"));
    std::vector<float> const dxs = parse_numbers(attribute(element, "dx"));
    std::vector<float> const dys = parse_numbers(attribute(element, "dy"));
    if (!xs.empty())
        pen.x = xs.front();
    if (!ys.empty())
        pen.y = ys.front();
    if (!dxs.empty())
        pen.x += dxs.front();
    if (!dys.empty())
        pen.y += dys.front();
    pen_set = true;
    for (dom::Node const* child : element.children()) {
        if (child->is_element()) {
            auto const& span = static_cast<dom::Element const&>(*child);
            if (!span.is_svg("tspan"))
                continue;
            css::ComputedStyle const* span_style = style_of(context, span);
            if (!span_style || span_style->display == css::Display::None)
                continue;
            render_text_element(context, state, span, *span_style, pen, pen_set);
            continue;
        }
        if (!child->is_text())
            continue;
        // This run: the text node's own words.
        std::u32string const text = collapsed_text(static_cast<dom::Text const&>(*child).data);
        if (text.empty())
            continue;
        text::FontRequest request;
        if (style.font_family)
            request.families = *style.font_family;
        request.weight = style.font_weight;
        request.italic = style.font_style != css::FontStyle::Normal;
        text::FontStack const& fonts = text::FontManager::instance().resolve(request);
        float const scale = state.ctm.scale_factor();
        float const size = style.font_size * scale;
        if (size <= 0 || size > 2048)
            continue;
        float const width_device = fonts.measure(text, size);
        std::string_view const anchor = text_anchor_of(context, element);
        float shift = 0; // in user units, along the baseline
        if (anchor == "middle")
            shift = -width_device / scale / 2;
        else if (anchor == "end")
            shift = -width_device / scale;
        if (style.visibility != css::Visibility::Hidden && style.fill.kind != css::SvgPaint::Kind::None) {
            ResolvedPaint const paint = resolve_paint(context, state, style.fill, &style, std::nullopt);
            if (!paint.none) {
                Point const origin = state.ctm.apply(Point { pen.x + shift, pen.y });
                Color color = paint.gradient ? gradient_color_at(*paint.gradient, origin.x, origin.y) : paint.color;
                float const a = static_cast<float>(color.a) * state.opacity * std::clamp(style.fill_opacity, 0.0f, 1.0f);
                color.a = static_cast<std::uint8_t>(std::clamp(std::lround(a), 0L, 255L));
                float x = origin.x;
                bool const bold = style.bold();
                bool const italic = request.italic;
                for (char32_t const code : text) {
                    text::FontStack::Glyph const glyph = fonts.glyph_for(code);
                    glyph.face->draw_glyph(context.target, glyph.glyph, x, origin.y, size, color, bold, italic);
                    x += glyph.face->advance(glyph.glyph, size);
                }
            }
        }
        pen.x += width_device / scale;
    }
}

// --- Images ---------------------------------------------------------------------

std::optional<Bitmap> decode_embedded(std::vector<std::uint8_t> const& bytes)
{
    if (looks_like_png(bytes))
        return decode_png(bytes);
    if (looks_like_gif(bytes))
        return decode_gif(bytes);
    if (looks_like_jpeg(bytes))
        return decode_jpeg(bytes);
    if (looks_like_bmp(bytes))
        return decode_bmp(bytes);
    return std::nullopt;
}

// <image> with a data: URL: the picture fitted into its box, drawn where
// the transform puts the box's corners — level; a turned image is drawn
// in the box its corners span.
void render_image(Context& context, State const& state, dom::Element const& element, css::ComputedStyle const& style)
{
    if (context.budget-- <= 0 || style.visibility == css::Visibility::Hidden)
        return;
    std::string_view const href = href_of(element);
    if (!href.starts_with("data:"))
        return;
    std::optional<net::Url> const url = net::parse_url(std::string(href));
    if (!url)
        return;
    std::optional<net::DataUrlPayload> const payload = net::parse_data_url(*url);
    if (!payload)
        return;
    std::optional<Bitmap> picture;
    if (looks_like_svg(payload->bytes))
        picture = decode_svg(payload->bytes, 4u << 20);
    else
        picture = decode_embedded(payload->bytes);
    if (!picture || picture->width() <= 0 || picture->height() <= 0)
        return;
    float const em = style.font_size;
    float const x = length_attribute(element, "x", state.viewport_width, em);
    float const y = length_attribute(element, "y", state.viewport_height, em);
    float width = length_attribute(element, "width", state.viewport_width, em, -1);
    float height = length_attribute(element, "height", state.viewport_height, em, -1);
    if (width < 0 && height < 0) {
        width = static_cast<float>(picture->width());
        height = static_cast<float>(picture->height());
    } else if (width < 0) {
        width = height * static_cast<float>(picture->width()) / static_cast<float>(picture->height());
    } else if (height < 0) {
        height = width * static_cast<float>(picture->height()) / static_cast<float>(picture->width());
    }
    if (width <= 0 || height <= 0)
        return;
    // The picture keeps its ratio inside the box, centred (xMidYMid meet).
    AspectRatio const ratio = parse_aspect_ratio(attribute(element, "preserveAspectRatio"));
    ViewBox const natural { 0, 0, static_cast<float>(picture->width()), static_cast<float>(picture->height()) };
    Matrix const placement = view_box_transform(natural, ratio, x, y, width, height).then(state.ctm);
    Point const a = placement.apply(Point { 0, 0 });
    Point const b = placement.apply(Point { natural.width, natural.height });
    int const left = static_cast<int>(std::lround(std::min(a.x, b.x)));
    int const top = static_cast<int>(std::lround(std::min(a.y, b.y)));
    int const right = static_cast<int>(std::lround(std::max(a.x, b.x)));
    int const bottom = static_cast<int>(std::lround(std::max(a.y, b.y)));
    if (right <= left || bottom <= top)
        return;
    std::optional<Rect> const saved = context.target.clip();
    if (!ratio.none) {
        // Whatever the picture's box spills past the element's box is cut.
        Point const c0 = state.ctm.apply(Point { x, y });
        Point const c1 = state.ctm.apply(Point { x + width, y + height });
        Rect box { static_cast<int>(std::lround(std::min(c0.x, c1.x))), static_cast<int>(std::lround(std::min(c0.y, c1.y))), 0, 0 };
        box.width = static_cast<int>(std::lround(std::max(c0.x, c1.x))) - box.x;
        box.height = static_cast<int>(std::lround(std::max(c0.y, c1.y))) - box.y;
        if (saved) {
            int const l = std::max(box.x, saved->x);
            int const t = std::max(box.y, saved->y);
            int const r = std::min(box.right(), saved->right());
            int const bt = std::min(box.bottom(), saved->bottom());
            box = Rect { l, t, std::max(0, r - l), std::max(0, bt - t) };
        }
        context.target.set_clip(box);
    }
    if (state.opacity >= 1 && !state.clip) {
        context.target.draw_scaled(*picture, Rect { left, top, right - left, bottom - top });
    } else {
        // Faded or clipped: through a scratch the size of the box.
        Bitmap scratch(right - left, bottom - top, Color::rgba(0, 0, 0, 0));
        scratch.draw_scaled(*picture, Rect { 0, 0, right - left, bottom - top });
        for (int yy = 0; yy < scratch.height(); ++yy) {
            for (int xx = 0; xx < scratch.width(); ++xx) {
                Color color = scratch.pixel(xx, yy);
                float alpha = static_cast<float>(color.a) * state.opacity;
                if (state.clip)
                    alpha = alpha * static_cast<float>(state.clip->at(left + xx, top + yy)) / 255.0f;
                color.a = static_cast<std::uint8_t>(std::clamp(std::lround(alpha), 0L, 255L));
                context.target.blend_pixel(left + xx, top + yy, color);
            }
        }
    }
    context.target.set_clip(saved);
}

// --- The tree -------------------------------------------------------------------

void render_children(Context& context, State const& state, dom::Element const& element)
{
    for (dom::Node const* child : element.children()) {
        if (child->is_element())
            render_element(context, state, static_cast<dom::Element const&>(*child));
    }
}

// A nested viewport: <svg> inside the document, or a <symbol> or <svg>
// reached through <use>. Its content is fitted by its viewBox and clipped
// to the viewport's rectangle.
void render_viewport(Context& context, State const& state, dom::Element const& element,
    css::ComputedStyle const& style, float x, float y, float width, float height)
{
    if (width <= 0 || height <= 0)
        return;
    std::optional<ViewBox> const box = parse_view_box(attribute(element, "viewBox"));
    AspectRatio const ratio = parse_aspect_ratio(attribute(element, "preserveAspectRatio"));
    State inner = state;
    inner.ctm = view_box_transform(box, ratio, x, y, width, height).then(state.ctm);
    inner.viewport_width = box ? box->width : width;
    inner.viewport_height = box ? box->height : height;
    (void)style;
    // Clipped to the viewport's rectangle, as the user-agent rules for a
    // nested viewport say; the style's overflow is not consulted, since
    // no user-agent rule of ours sets it and the initial value is visible.
    {
        Path frame;
        frame.move_to(Point { x, y });
        frame.line_to(Point { x + width, y });
        frame.line_to(Point { x + width, y + height });
        frame.line_to(Point { x, y + height });
        frame.close();
        std::vector<Polygon> device;
        for (Polygon& polygon : fill_polygons(flatten(frame))) {
            for (Point& p : polygon)
                p = state.ctm.apply(p);
            device.push_back(std::move(polygon));
        }
        auto mask = std::make_shared<Mask>(rasterize_in(context, device, FillRule::NonZero));
        if (mask->empty())
            return;
        if (state.clip) {
            for (int row = 0; row < mask->height; ++row) {
                for (int column = 0; column < mask->width; ++column) {
                    std::uint8_t& cell = mask->alpha[static_cast<std::size_t>(row) * static_cast<std::size_t>(mask->width)
                        + static_cast<std::size_t>(column)];
                    cell = static_cast<std::uint8_t>(cell * state.clip->at(mask->left + column, mask->top + row) / 255);
                }
            }
        }
        inner.clip = std::move(mask);
    }
    render_children(context, inner, element);
}

void render_use(Context& context, State const& state, dom::Element const& element, css::ComputedStyle const& style)
{
    if (state.depth > 16)
        return;
    std::string const id = reference_of(href_of(element));
    if (id.empty())
        return;
    dom::Element const* const target = element_by_id(context, id);
    if (!target || target == &element)
        return;
    // A use may not reach an ancestor of itself: that is a cycle.
    for (dom::Node const* node = element.parent(); node; node = node->parent()) {
        if (node == target)
            return;
    }
    float const em = style.font_size;
    State inner = state;
    inner.depth = state.depth + 1;
    inner.ctm = Matrix::translate(length_attribute(element, "x", state.viewport_width, em),
                    length_attribute(element, "y", state.viewport_height, em))
                    .then(state.ctm);
    css::ComputedStyle const* target_style = style_of(context, *target);
    if (target->is_svg("symbol") || target->is_svg("svg")) {
        float const width = length_attribute(element, "width", state.viewport_width, em,
            length_attribute(*target, "width", state.viewport_width, em, state.viewport_width));
        float const height = length_attribute(element, "height", state.viewport_height, em,
            length_attribute(*target, "height", state.viewport_height, em, state.viewport_height));
        if (!target_style)
            return;
        // The symbol's own transform, then the use's.
        inner.ctm = parse_transform(attribute(*target, "transform")).then(inner.ctm);
        inner.opacity *= std::clamp(target_style->opacity, 0.0f, 1.0f);
        render_viewport(context, inner, *target, *target_style, 0, 0, width, height);
        return;
    }
    render_element(context, inner, *target);
}

void render_element(Context& context, State state, dom::Element const& element)
{
    if (element.namespace_uri() != dom::ns::svg)
        return;
    css::ComputedStyle const* const style = style_of(context, element);
    if (!style || style->display == css::Display::None)
        return;
    std::string const& name = element.local_name();
    // Never rendered directly: the definitions, the paint servers, the
    // clip paths, the descriptive elements.
    if (name == "defs" || name == "symbol" || name == "clipPath" || name == "mask" || name == "marker"
        || name == "pattern" || name == "linearGradient" || name == "radialGradient" || name == "title"
        || name == "desc" || name == "metadata" || name == "style" || name == "script"
        || name == "foreignObject" || name == "filter")
        return;
    state.ctm = parse_transform(attribute(element, "transform")).then(state.ctm);
    state.opacity *= std::clamp(style->opacity, 0.0f, 1.0f);
    if (state.opacity <= 0)
        return;
    if (name == "svg") {
        float const em = style->font_size;
        float const x = length_attribute(element, "x", state.viewport_width, em);
        float const y = length_attribute(element, "y", state.viewport_height, em);
        float const width = length_attribute(element, "width", state.viewport_width, em, state.viewport_width);
        float const height = length_attribute(element, "height", state.viewport_height, em, state.viewport_height);
        state.clip = clip_for(context, state, element, std::nullopt);
        render_viewport(context, state, element, *style, x, y, width, height);
        return;
    }
    if (name == "g" || name == "a" || name == "switch") {
        state.clip = clip_for(context, state, element, std::nullopt);
        render_children(context, state, element);
        return;
    }
    if (name == "use") {
        state.clip = clip_for(context, state, element, std::nullopt);
        render_use(context, state, element, *style);
        return;
    }
    if (name == "text") {
        state.clip = clip_for(context, state, element, std::nullopt);
        Point pen;
        bool pen_set = false;
        render_text_element(context, state, element, *style, pen, pen_set);
        return;
    }
    if (name == "image") {
        state.clip = clip_for(context, state, element, std::nullopt);
        render_image(context, state, element, *style);
        return;
    }
    if (std::optional<Path> path = shape_path(state, element, style)) {
        if (path->empty())
            return;
        state.clip = clip_for(context, state, element, bbox_of(*path));
        draw_path(context, state, *style, *path);
        return;
    }
    // An element this subset does not know: what it holds may still be
    // shapes (an unknown container), so they are drawn.
    render_children(context, state, element);
}

dom::Element const* find_svg_root(dom::Node const& node)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_svg("svg"))
            return &element;
    }
    for (dom::Node const* child : node.children()) {
        if (dom::Element const* found = find_svg_root(*child))
            return found;
    }
    return nullptr;
}

} // namespace

IntrinsicSize intrinsic_size(dom::Element const& svg)
{
    IntrinsicSize size;
    auto const absolute = [&](std::string_view name) -> std::optional<float> {
        std::string_view const text = trimmed(attribute(svg, name));
        if (text.empty() || text.ends_with('%') || text == "auto")
            return std::nullopt;
        std::optional<float> const value = parse_length(text, 0, 16);
        if (!value || !(*value >= 0))
            return std::nullopt;
        return value;
    };
    size.width = absolute("width");
    size.height = absolute("height");
    if (std::optional<ViewBox> const box = parse_view_box(attribute(svg, "viewBox")))
        size.ratio = box->width / box->height;
    else if (size.width && size.height && *size.height > 0)
        size.ratio = *size.width / *size.height;
    return size;
}

Bitmap render(dom::Element const& svg, css::StyleMap const& styles, float width, float height)
{
    int const w = std::max(1, static_cast<int>(std::lround(width)));
    int const h = std::max(1, static_cast<int>(std::lround(height)));
    Bitmap target(w, h, Color::rgba(0, 0, 0, 0));
    if (!svg.is_svg("svg"))
        return target;
    Context context { svg, styles, target, {}, 200000 };
    collect_ids(context, svg);
    css::ComputedStyle const* const style = style_of(context, svg);
    if (!style || style->display == css::Display::None || view_box_disables(attribute(svg, "viewBox")))
        return target;
    State state;
    std::optional<ViewBox> const box = parse_view_box(attribute(svg, "viewBox"));
    AspectRatio const ratio = parse_aspect_ratio(attribute(svg, "preserveAspectRatio"));
    state.ctm = view_box_transform(box, ratio, 0, 0, width, height);
    state.viewport_width = box ? box->width : width;
    state.viewport_height = box ? box->height : height;
    state.opacity = 1; // the element's own opacity is the box's, painted by the page
    render_children(context, state, svg);
    return target;
}

bool looks_like_svg(std::vector<std::uint8_t> const& bytes)
{
    std::string_view text(reinterpret_cast<char const*>(bytes.data()), std::min<std::size_t>(bytes.size(), 4096));
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    if (!text.starts_with('<'))
        return false;
    // The root element must be svg: the first tag that is not a
    // declaration, a comment or a processing instruction.
    std::size_t pos = 0;
    while (pos < text.size()) {
        pos = text.find('<', pos);
        if (pos == std::string_view::npos)
            return false;
        std::string_view const rest = text.substr(pos);
        if (rest.starts_with("<?") || rest.starts_with("<!")) {
            std::size_t const end = rest.starts_with("<!--") ? text.find("-->", pos) : text.find('>', pos);
            if (end == std::string_view::npos)
                return false;
            pos = end + 1;
            continue;
        }
        if (rest.size() >= 4 && rest.starts_with("<svg")
            && (rest.size() == 4 || is_space(rest[4]) || rest[4] == '>' || rest[4] == '/' || rest[4] == ':'))
            return true;
        // A namespace prefix: <s:svg.
        std::size_t const colon = rest.find(':');
        std::size_t const space = rest.find_first_of(" \t\n\r>/");
        if (colon != std::string_view::npos && space != std::string_view::npos && colon < space
            && rest.substr(colon + 1, space - colon - 1) == "svg")
            return true;
        return false;
    }
    return false;
}

std::optional<Bitmap> decode_svg(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels, float* scale)
{
    if (scale)
        *scale = 1;
    if (!looks_like_svg(bytes))
        return std::nullopt;
    std::unique_ptr<dom::Document> const document = html::parse_document_bytes(
        std::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
    if (!document)
        return std::nullopt;
    dom::Element const* const root = find_svg_root(*document);
    if (!root || view_box_disables(attribute(*root, "viewBox")))
        return std::nullopt;
    css::StyleMap const styles = css::resolve_styles(*document);
    IntrinsicSize const size = intrinsic_size(*root);
    float width = 300;
    float height = 150;
    std::optional<ViewBox> const box = parse_view_box(attribute(*root, "viewBox"));
    if (size.width && size.height) {
        width = *size.width;
        height = *size.height;
    } else if (size.width && size.ratio) {
        width = *size.width;
        height = width / *size.ratio;
    } else if (size.height && size.ratio) {
        height = *size.height;
        width = height * *size.ratio;
    } else if (box) {
        width = box->width;
        height = box->height;
    } else if (size.width) {
        width = *size.width;
    } else if (size.height) {
        height = *size.height;
    }
    if (!(width >= 1) || !(height >= 1))
        return std::nullopt;
    if (static_cast<double>(width) * static_cast<double>(height) > static_cast<double>(max_pixels))
        return std::nullopt;
    float factor = 1;
    if (scale) {
        float const shorter = std::min(width, height);
        factor = std::clamp(std::ceil(128.0f / shorter), 1.0f, 4.0f);
        while (factor > 1
            && static_cast<double>(width * factor) * static_cast<double>(height * factor) > static_cast<double>(max_pixels))
            factor -= 1;
        *scale = factor;
    }
    return render(*root, styles, width * factor, height * factor);
}

}
