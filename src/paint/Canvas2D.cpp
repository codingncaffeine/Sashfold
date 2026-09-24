#include "paint/Canvas2D.h"

#include "core/Bidi.h"
#include "text/FontManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

namespace sashfold::canvas {

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double two_pi = 2 * pi;

// Pixels that may be worked on at once: a shape past this many is drawn as
// nothing rather than exhausting memory.
constexpr std::size_t max_layer_pixels = 64u * 1024u * 1024u;

float to_unit(std::uint8_t v) { return static_cast<float>(v) / 255.0f; }

std::uint8_t to_byte(float v)
{
    if (!(v > 0))
        return 0;
    if (v >= 1)
        return 255;
    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

// atan2 without the C library: a polynomial for the arctangent on [0, 1]
// (error under 2e-6 radians) and the octants unfolded by hand, so that a
// conic gradient paints the same pixels everywhere.
double arctangent2(double y, double x)
{
    double const ax = std::abs(x);
    double const ay = std::abs(y);
    if (ax == 0 && ay == 0)
        return 0;
    double const small = std::min(ax, ay);
    double const large = std::max(ax, ay);
    double const z = small / large;
    double const z2 = z * z;
    double angle = z
        * (0.99997726 + z2 * (-0.33262347 + z2 * (0.19354346 + z2 * (-0.11643287 + z2 * (0.05265332 + z2 * -0.01172120)))));
    if (ay > ax)
        angle = pi / 2 - angle;
    if (x < 0)
        angle = pi - angle;
    if (y < 0)
        angle = -angle;
    return angle;
}

// --- Reducing an angle by pi / 2 -----------------------------------------------
//
// sin and cos are taken of the angle less the nearest multiple of pi / 2,
// which is exact only if pi / 2 is known to far more bits than a double
// holds: a double near 1e300 is some 2^1000 quarter turns, and the one double
// that comes closest to a multiple of pi / 2 (6381956970095103 * 2^797) lies
// within 2^-61 of it. So pi / 2 is worked out here once, to 1,280 bits, by
// Machin's formula pi / 4 = 4 atan(1/5) - atan(1/239) in fixed point.

// Word 0 is the integer part, then 32-bit words of fraction, most
// significant first. The last word is a guard for the truncations of the
// series (about a thousand, each under one unit of it).
constexpr std::size_t fraction_words = 40;
constexpr std::size_t fixed_words = 1 + fraction_words + 1;
using Fixed = std::array<std::uint32_t, fixed_words>;

void divide(Fixed& v, std::uint32_t d)
{
    std::uint64_t rest = 0;
    for (std::uint32_t& word : v) {
        std::uint64_t const current = (rest << 32) | word;
        word = static_cast<std::uint32_t>(current / d);
        rest = current % d;
    }
}

void add(Fixed& a, Fixed const& b)
{
    std::uint64_t carry = 0;
    for (std::size_t i = fixed_words; i-- > 0;) {
        std::uint64_t const sum = std::uint64_t { a[i] } + b[i] + carry;
        a[i] = static_cast<std::uint32_t>(sum);
        carry = sum >> 32;
    }
}

void subtract(Fixed& a, Fixed const& b)
{
    std::uint64_t borrow = 0;
    for (std::size_t i = fixed_words; i-- > 0;) {
        std::uint64_t const take = std::uint64_t { b[i] } + borrow;
        borrow = a[i] < take ? 1 : 0;
        a[i] = static_cast<std::uint32_t>((std::uint64_t { a[i] } + (borrow << 32)) - take);
    }
}

// sum += multiple * atan(1 / n), the series 1/n - 1/(3 n^3) + 1/(5 n^5) ...
void add_arctangent(Fixed& sum, std::uint32_t multiple, std::uint32_t n)
{
    Fixed power {};
    power[0] = multiple;
    divide(power, n);
    for (std::uint32_t k = 0; std::any_of(power.begin(), power.end(), [](std::uint32_t w) { return w != 0; }); ++k) {
        Fixed term = power;
        divide(term, 2 * k + 1);
        if (k % 2 == 0)
            add(sum, term);
        else
            subtract(sum, term);
        divide(power, n * n);
    }
}

struct QuarterTurn {
    // pi / 2 times 2^1280, least significant word first, for the long
    // division of a large angle.
    std::array<std::uint32_t, 1 + fraction_words> scaled {};
    // pi / 2 cut into pieces of 24 bits each: a multiple of pi / 2 under
    // 2^27 turns is taken off piece by piece, each product exact.
    std::array<double, 7> pieces {};
};

QuarterTurn const& quarter_turn()
{
    static QuarterTurn const turn = [] {
        Fixed half_pi {};
        add_arctangent(half_pi, 8, 5);
        Fixed smaller {};
        add_arctangent(smaller, 2, 239);
        subtract(half_pi, smaller);
        QuarterTurn made;
        for (std::size_t i = 0; i <= fraction_words; ++i)
            made.scaled[i] = half_pi[fraction_words - i];
        // Bit k of the fraction (k = 0 the units) weighs 2^-k.
        auto bit = [&](std::size_t k) -> std::uint32_t {
            if (k == 0)
                return half_pi[0] & 1u;
            std::size_t const word = 1 + (k - 1) / 32;
            return (half_pi[word] >> (31 - (k - 1) % 32)) & 1u;
        };
        for (std::size_t piece = 0; piece < made.pieces.size(); ++piece) {
            std::uint32_t bits = 0;
            for (std::size_t k = piece * 24; k < piece * 24 + 24; ++k)
                bits = (bits << 1) | bit(k);
            made.pieces[piece] = std::ldexp(static_cast<double>(bits), -static_cast<int>(piece * 24 + 23));
        }
        return made;
    }();
    return turn;
}

struct Reduced {
    // The remainder as the sum of two doubles, `low` under half an ulp of
    // `high`, which is in [-pi / 4, pi / 4] give or take a rounding.
    double high = 0;
    double low = 0;
    unsigned quadrant = 0; // the multiple of pi / 2 taken off, mod 4
};

// a + b as a rounded sum and its exact rounding error (Knuth's two-sum).
std::pair<double, double> two_sum(double a, double b)
{
    double const sum = a + b;
    double const b_part = sum - a;
    double const error = (a - (sum - b_part)) + (b - b_part);
    return { sum, error };
}

// x (finite, not negative) less the nearest multiple of pi / 2.
Reduced reduce(double x)
{
    QuarterTurn const& turn = quarter_turn();
    if (x < 134217728.0) { // 2^27
        // Cody and Waite: the nearest multiple n, then n (pi / 2) taken off
        // a piece at a time. n times a piece fits 51 bits, so each product
        // is exact, and each difference keeps its rounding error.
        double const n = std::floor(x * (2 / pi) + 0.5);
        double high = x;
        double low = 0;
        for (double const piece : turn.pieces) {
            auto const [sum, error] = two_sum(high, -(n * piece));
            high = sum;
            low += error;
        }
        auto const [sum, error] = two_sum(high, low);
        return { sum, error, static_cast<unsigned>(n) & 3u };
    }
    // Past that, the long division of x by pi / 2, both scaled by 2^1280:
    // x = m 2^(e - 53) with m an integer of 53 bits, so x 2^1280 is m shifted
    // left by e - 53 + 1280 (at least 1,254 for these angles).
    int exponent = 0;
    double const fraction = std::frexp(x, &exponent);
    auto const mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 53));
    int const shift = exponent - 53 + static_cast<int>(fraction_words * 32);
    constexpr std::size_t words = 1 + fraction_words + 1;
    std::array<std::uint32_t, words> rest {};
    unsigned quotient = 0;
    auto greater_or_equal = [&]() {
        for (std::size_t i = words; i-- > 0;) {
            std::uint32_t const other = i < turn.scaled.size() ? turn.scaled[i] : 0;
            if (rest[i] != other)
                return rest[i] > other;
        }
        return true;
    };
    for (int position = 52 + shift; position >= 0; --position) {
        std::uint32_t incoming = (position >= shift && ((mantissa >> (position - shift)) & 1u)) ? 1u : 0u;
        for (std::uint32_t& word : rest) {
            std::uint32_t const out = word >> 31;
            word = (word << 1) | incoming;
            incoming = out;
        }
        quotient = (quotient << 1) & 3u;
        if (greater_or_equal()) {
            std::uint64_t borrow = 0;
            for (std::size_t i = 0; i < words; ++i) {
                std::uint64_t const take = std::uint64_t { i < turn.scaled.size() ? turn.scaled[i] : 0u } + borrow;
                borrow = rest[i] < take ? 1 : 0;
                rest[i] = static_cast<std::uint32_t>((std::uint64_t { rest[i] } + (borrow << 32)) - take);
            }
            quotient |= 1u;
        }
    }
    // The remainder is in [0, pi / 2); past pi / 4 the next multiple is
    // nearer, and the remainder from it is exact as well.
    std::array<std::uint32_t, words> twice = rest;
    std::uint32_t carry = 0;
    for (std::uint32_t& word : twice) {
        std::uint32_t const out = word >> 31;
        word = (word << 1) | carry;
        carry = out;
    }
    bool const past_half = [&] {
        for (std::size_t i = words; i-- > 0;) {
            std::uint32_t const other = i < turn.scaled.size() ? turn.scaled[i] : 0;
            if (twice[i] != other)
                return twice[i] > other;
        }
        return false;
    }();
    if (past_half) {
        std::array<std::uint32_t, words> from_next {};
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < words; ++i) {
            std::uint64_t const take = std::uint64_t { rest[i] } + borrow;
            std::uint32_t const whole = i < turn.scaled.size() ? turn.scaled[i] : 0u;
            borrow = whole < take ? 1 : 0;
            from_next[i] = static_cast<std::uint32_t>((std::uint64_t { whole } + (borrow << 32)) - take);
        }
        rest = from_next;
        quotient = (quotient + 1) & 3u;
    }
    // The top three words hold at least 65 significant bits of any
    // remainder not under 2^-1150, which none is: each word is a double
    // exactly, and they are summed keeping the rounding error.
    double high = 0;
    double low = 0;
    for (std::size_t i = words; i-- > 0;) {
        if (rest[i] == 0)
            continue;
        for (std::size_t j = i + 1; j-- > 0 && j + 3 > i;) {
            double const part = std::ldexp(static_cast<double>(rest[j]), static_cast<int>(j * 32) - static_cast<int>(fraction_words * 32));
            auto const [sum, error] = two_sum(high, part);
            high = sum;
            low += error;
        }
        break;
    }
    auto const [sum, error] = two_sum(high, low);
    if (past_half)
        return { -sum, -error, quotient };
    return { sum, error, quotient };
}

} // namespace

// The angle less its nearest multiple of pi / 2 (exact for every double, see
// reduce()), then Taylor series in double on [-pi / 4, pi / 4] that stop
// past the last term that changes a double: only additions,
// multiplications and divisions, which round the same everywhere
// (contraction is off for the build). The leading terms are added last, and
// the remainder's low part and the rounding of its square are carried, so
// the results are within an ulp of the true sine and cosine for any finite
// angle, and mostly the nearest double to them.
SineCosine sine_cosine(double radians)
{
    if (!std::isfinite(radians))
        return { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN() };
    Reduced const reduced = reduce(std::abs(radians));
    double const r = reduced.high;
    double const x2 = r * r;
    // sin(r + low) = sin r + low cos r, near enough, and
    // sin r = r - r (r^2 / 3! - r^4 / 5! + ...).
    double const odd = x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 * (1.0 - x2 / 110.0 * (1.0 - x2 / 156.0 * (1.0 - x2 / 210.0 * (1.0 - x2 / 272.0)))))));
    double const half = x2 / 2.0;
    double const s = r + (reduced.low * (1.0 - half) - r * odd);
    // cos(r + low) = cos r - low sin r, and cos r = 1 - r^2 / 2 + (r^4 / 4! - ...),
    // where 1 - r^2 / 2 keeps both its own rounding error and that of r^2
    // (Dekker's exact product, r split into halves of 26 bits).
    double const even = x2 * x2 / 24.0 * (1.0 - x2 / 30.0 * (1.0 - x2 / 56.0 * (1.0 - x2 / 90.0 * (1.0 - x2 / 132.0 * (1.0 - x2 / 182.0 * (1.0 - x2 / 240.0))))));
    double const scaled = 134217729.0 * r; // 2^27 + 1
    double const r_high = scaled - (scaled - r);
    double const r_low = r - r_high;
    double const square_error = ((r_high * r_high - x2) + 2.0 * r_high * r_low) + r_low * r_low;
    double const w = 1.0 - half;
    double const c = w + ((((1.0 - w) - half) - square_error / 2.0) + (even - r * reduced.low));
    SineCosine turned;
    switch (reduced.quadrant) {
    case 1:
        turned = { c, -s };
        break;
    case 2:
        turned = { -s, -c };
        break;
    case 3:
        turned = { -c, s };
        break;
    default:
        turned = { s, c };
        break;
    }
    if (std::signbit(radians))
        turned.sine = -turned.sine;
    return turned;
}

namespace {

// A float image layer, premultiplied, over a rectangle of the surface.
struct Layer {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<float> rgba;

    bool empty() const { return width <= 0 || height <= 0; }
    float* at(int x, int y)
    {
        return rgba.data() + (static_cast<std::size_t>(y - top) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x - left)) * 4u;
    }
    float const* at(int x, int y) const
    {
        return rgba.data() + (static_cast<std::size_t>(y - top) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x - left)) * 4u;
    }
    bool contains(int x, int y) const { return x >= left && y >= top && x < left + width && y < top + height; }
};

void premultiplied(Color color, float out[4])
{
    float const a = to_unit(color.a);
    out[0] = to_unit(color.r) * a;
    out[1] = to_unit(color.g) * a;
    out[2] = to_unit(color.b) * a;
    out[3] = a;
}

// The color a style gives each pixel of the surface.
class Shader {
public:
    Shader(Style const& style, Transform const& transform, bool smoothing, bool clamp_edges = false)
        : m_style(style)
        , m_smoothing(smoothing)
        , m_clamp(clamp_edges)
    {
        premultiplied(style.color, m_solid);
        if (style.gradient || style.pattern) {
            Transform space = transform;
            if (style.pattern)
                space = transform.multiply(style.pattern->transform);
            m_inverse = space.inverse();
        }
        if (style.gradient)
            prepare_stops(*style.gradient);
    }

    void at(int px, int py, float out[4]) const
    {
        if (!m_style.gradient && !m_style.pattern) {
            std::copy(m_solid, m_solid + 4, out);
            return;
        }
        out[0] = out[1] = out[2] = out[3] = 0;
        if (!m_inverse)
            return;
        svg::Point const p = m_inverse->apply(px + 0.5, py + 0.5);
        if (m_style.gradient)
            gradient_at(*m_style.gradient, static_cast<double>(p.x), static_cast<double>(p.y), out);
        else
            pattern_at(*m_style.pattern, static_cast<double>(p.x), static_cast<double>(p.y), out);
    }

private:
    void prepare_stops(Gradient const& gradient)
    {
        m_stops = gradient.stops;
    }

    // The color at an offset along the stops: the first before the first
    // stop, the last after the last, and between two the straight colors
    // mixed, then premultiplied.
    void color_at(double t, float out[4]) const
    {
        if (m_stops.empty()) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        }
        if (!(t > m_stops.front().offset)) {
            premultiplied(m_stops.front().color, out);
            return;
        }
        if (t >= m_stops.back().offset) {
            premultiplied(m_stops.back().color, out);
            return;
        }
        std::size_t i = 1;
        while (i < m_stops.size() && m_stops[i].offset <= t)
            ++i;
        ColorStop const& a = m_stops[i - 1];
        ColorStop const& b = m_stops[i];
        double const span = b.offset - a.offset;
        float const f = span > 0 ? static_cast<float>((t - a.offset) / span) : 1.0f;
        float const ra = to_unit(a.color.r) + (to_unit(b.color.r) - to_unit(a.color.r)) * f;
        float const ga = to_unit(a.color.g) + (to_unit(b.color.g) - to_unit(a.color.g)) * f;
        float const ba = to_unit(a.color.b) + (to_unit(b.color.b) - to_unit(a.color.b)) * f;
        float const aa = to_unit(a.color.a) + (to_unit(b.color.a) - to_unit(a.color.a)) * f;
        out[0] = ra * aa;
        out[1] = ga * aa;
        out[2] = ba * aa;
        out[3] = aa;
    }

    void gradient_at(Gradient const& g, double x, double y, float out[4]) const
    {
        switch (g.kind) {
        case Gradient::Kind::Linear: {
            double const dx = g.x1 - g.x0;
            double const dy = g.y1 - g.y0;
            double const length2 = dx * dx + dy * dy;
            if (length2 == 0)
                return;
            color_at(((x - g.x0) * dx + (y - g.y0) * dy) / length2, out);
            return;
        }
        case Gradient::Kind::Radial: {
            if (g.x0 == g.x1 && g.y0 == g.y1 && g.r0 == g.r1)
                return;
            // The largest w for which the circle at w, of radius r(w) >= 0,
            // passes through the point (HTML section4.12.5.1.8).
            double const cdx = g.x1 - g.x0;
            double const cdy = g.y1 - g.y0;
            double const dr = g.r1 - g.r0;
            double const pdx = x - g.x0;
            double const pdy = y - g.y0;
            double const a = cdx * cdx + cdy * cdy - dr * dr;
            double const b = pdx * cdx + pdy * cdy + g.r0 * dr;
            double const c = pdx * pdx + pdy * pdy - g.r0 * g.r0;
            std::optional<double> w;
            auto const consider = [&](double candidate) {
                if (std::isfinite(candidate) && g.r0 + candidate * dr >= 0 && (!w || candidate > *w))
                    w = candidate;
            };
            if (std::abs(a) < 1e-12) {
                if (b != 0)
                    consider(c / (2 * b));
            } else {
                double const discriminant = b * b - a * c;
                if (discriminant < 0)
                    return;
                double const root = std::sqrt(discriminant);
                consider((b + root) / a);
                consider((b - root) / a);
            }
            if (w)
                color_at(*w, out);
            return;
        }
        case Gradient::Kind::Conic: {
            double const dx = x - g.x0;
            double const dy = y - g.y0;
            double turn = (arctangent2(dy, dx) - g.angle) / two_pi;
            turn -= std::floor(turn);
            color_at(turn, out);
            return;
        }
        }
    }

    // One texel of the pattern's picture, transparent outside it unless
    // the axis repeats (or, for a picture drawn once, the edge is held).
    void texel(Surface const& image, bool repeat_x, bool repeat_y, int tx, int ty, float out[4]) const
    {
        if (repeat_x) {
            tx %= image.width;
            if (tx < 0)
                tx += image.width;
        } else if (m_clamp) {
            tx = std::clamp(tx, 0, image.width - 1);
        } else if (tx < 0 || tx >= image.width) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        }
        if (repeat_y) {
            ty %= image.height;
            if (ty < 0)
                ty += image.height;
        } else if (m_clamp) {
            ty = std::clamp(ty, 0, image.height - 1);
        } else if (ty < 0 || ty >= image.height) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        }
        std::uint8_t const* p = image.pixels.data() + image.offset(tx, ty);
        out[0] = to_unit(p[0]);
        out[1] = to_unit(p[1]);
        out[2] = to_unit(p[2]);
        out[3] = to_unit(p[3]);
    }

    void pattern_at(Pattern const& pattern, double x, double y, float out[4]) const
    {
        Surface const* image = pattern.image.get();
        if (!image || image->width <= 0 || image->height <= 0)
            return;
        if (!m_smoothing) {
            texel(*image, pattern.repeat_x, pattern.repeat_y, static_cast<int>(std::floor(x)),
                static_cast<int>(std::floor(y)), out);
            return;
        }
        double const u = x - 0.5;
        double const v = y - 0.5;
        double const fu = std::floor(u);
        double const fv = std::floor(v);
        int const x0 = static_cast<int>(fu);
        int const y0 = static_cast<int>(fv);
        float const wx = static_cast<float>(u - fu);
        float const wy = static_cast<float>(v - fv);
        float t00[4], t10[4], t01[4], t11[4];
        texel(*image, pattern.repeat_x, pattern.repeat_y, x0, y0, t00);
        texel(*image, pattern.repeat_x, pattern.repeat_y, x0 + 1, y0, t10);
        texel(*image, pattern.repeat_x, pattern.repeat_y, x0, y0 + 1, t01);
        texel(*image, pattern.repeat_x, pattern.repeat_y, x0 + 1, y0 + 1, t11);
        for (int k = 0; k < 4; ++k) {
            float const top = t00[k] + (t10[k] - t00[k]) * wx;
            float const bottom = t01[k] + (t11[k] - t01[k]) * wx;
            out[k] = top + (bottom - top) * wy;
        }
    }

    Style const& m_style;
    bool m_smoothing;
    bool m_clamp;
    float m_solid[4] {};
    std::optional<Transform> m_inverse;
    std::vector<ColorStop> m_stops;
};

// --- Blending (Compositing and Blending section10) ---------------------------------

float blend_separable(Composite op, float cb, float cs)
{
    switch (op) {
    case Composite::Multiply:
        return cb * cs;
    case Composite::Screen:
        return cb + cs - cb * cs;
    case Composite::Overlay:
        return blend_separable(Composite::HardLight, cs, cb);
    case Composite::Darken:
        return std::min(cb, cs);
    case Composite::Lighten:
        return std::max(cb, cs);
    case Composite::ColorDodge:
        if (cb == 0)
            return 0;
        if (cs >= 1)
            return 1;
        return std::min(1.0f, cb / (1 - cs));
    case Composite::ColorBurn:
        if (cb >= 1)
            return 1;
        if (cs <= 0)
            return 0;
        return 1 - std::min(1.0f, (1 - cb) / cs);
    case Composite::HardLight:
        if (cs <= 0.5f)
            return blend_separable(Composite::Multiply, cb, 2 * cs);
        return blend_separable(Composite::Screen, cb, 2 * cs - 1);
    case Composite::SoftLight: {
        if (cs <= 0.5f)
            return cb - (1 - 2 * cs) * cb * (1 - cb);
        float const d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    case Composite::Difference:
        return std::abs(cb - cs);
    case Composite::Exclusion:
        return cb + cs - 2 * cb * cs;
    default:
        return cs;
    }
}

float luminosity_of(float const c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

void clip_color(float c[3])
{
    float const l = luminosity_of(c);
    float const n = std::min({ c[0], c[1], c[2] });
    float const x = std::max({ c[0], c[1], c[2] });
    for (int k = 0; k < 3; ++k) {
        if (n < 0 && l - n != 0)
            c[k] = l + (c[k] - l) * l / (l - n);
        if (x > 1 && x - l != 0)
            c[k] = l + (c[k] - l) * (1 - l) / (x - l);
    }
}

void set_luminosity(float c[3], float l)
{
    float const d = l - luminosity_of(c);
    for (int k = 0; k < 3; ++k)
        c[k] += d;
    clip_color(c);
}

float saturation_of(float const c[3]) { return std::max({ c[0], c[1], c[2] }) - std::min({ c[0], c[1], c[2] }); }

void set_saturation(float c[3], float s)
{
    int max_i = 0;
    int min_i = 0;
    for (int k = 1; k < 3; ++k) {
        if (c[k] > c[max_i])
            max_i = k;
        if (c[k] < c[min_i])
            min_i = k;
    }
    if (max_i == min_i) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    int const mid_i = 3 - max_i - min_i;
    float const span = c[max_i] - c[min_i];
    c[mid_i] = (c[mid_i] - c[min_i]) * s / span;
    c[max_i] = s;
    c[min_i] = 0;
}

void blend_non_separable(Composite op, float const cb[3], float const cs[3], float out[3])
{
    float t[3];
    switch (op) {
    case Composite::Hue:
        std::copy(cs, cs + 3, t);
        set_saturation(t, saturation_of(cb));
        set_luminosity(t, luminosity_of(cb));
        break;
    case Composite::Saturation:
        std::copy(cb, cb + 3, t);
        set_saturation(t, saturation_of(cs));
        set_luminosity(t, luminosity_of(cb));
        break;
    case Composite::Color:
        std::copy(cs, cs + 3, t);
        set_luminosity(t, luminosity_of(cb));
        break;
    default: // Luminosity
        std::copy(cb, cb + 3, t);
        set_luminosity(t, luminosity_of(cs));
        break;
    }
    std::copy(t, t + 3, out);
}

// Whether the operator leaves the destination as it is where the source
// is transparent: those draw only where the shape is.
bool bounded(Composite op)
{
    switch (op) {
    case Composite::SourceIn:
    case Composite::SourceOut:
    case Composite::DestinationIn:
    case Composite::DestinationAtop:
    case Composite::Copy:
        return false;
    default:
        return true;
    }
}

// One pixel: source over destination by the operator, both premultiplied.
void composite_pixel(Composite op, float const s[4], float const d[4], float out[4])
{
    float const as = s[3];
    float const ad = d[3];
    float fa = 1;
    float fb = 1;
    switch (op) {
    case Composite::SourceOver: fa = 1; fb = 1 - as; break;
    case Composite::SourceIn: fa = ad; fb = 0; break;
    case Composite::SourceOut: fa = 1 - ad; fb = 0; break;
    case Composite::SourceAtop: fa = ad; fb = 1 - as; break;
    case Composite::DestinationOver: fa = 1 - ad; fb = 1; break;
    case Composite::DestinationIn: fa = 0; fb = as; break;
    case Composite::DestinationOut: fa = 0; fb = 1 - as; break;
    case Composite::DestinationAtop: fa = 1 - ad; fb = as; break;
    case Composite::Copy: fa = 1; fb = 0; break;
    case Composite::Xor: fa = 1 - ad; fb = 1 - as; break;
    case Composite::Clear: fa = 0; fb = 0; break;
    case Composite::Lighter:
        for (int k = 0; k < 4; ++k)
            out[k] = std::min(1.0f, s[k] + d[k]);
        return;
    default: {
        // A blend mode, composited source-over.
        float cs[3] = { 0, 0, 0 };
        float cb[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; ++k) {
            cs[k] = as > 0 ? std::min(1.0f, s[k] / as) : 0;
            cb[k] = ad > 0 ? std::min(1.0f, d[k] / ad) : 0;
        }
        float mixed[3];
        if (op == Composite::Hue || op == Composite::Saturation || op == Composite::Color || op == Composite::Luminosity) {
            blend_non_separable(op, cb, cs, mixed);
        } else {
            for (int k = 0; k < 3; ++k)
                mixed[k] = blend_separable(op, cb[k], cs[k]);
        }
        for (int k = 0; k < 3; ++k)
            out[k] = s[k] * (1 - ad) + d[k] * (1 - as) + as * ad * mixed[k];
        out[3] = as + ad - as * ad;
        return;
    }
    }
    for (int k = 0; k < 4; ++k)
        out[k] = s[k] * fa + d[k] * fb;
}

// The layer composited over the surface, within the clip: the shape's
// rectangle for an operator that leaves the rest be, the whole surface for
// one that clears what the source does not cover.
void composite_layer(Surface& surface, Layer const& layer, DrawState const& state)
{
    Composite const op = state.composite;
    int left = 0;
    int top = 0;
    int right = surface.width;
    int bottom = surface.height;
    if (bounded(op)) {
        if (layer.empty())
            return;
        left = std::max(left, layer.left);
        top = std::max(top, layer.top);
        right = std::min(right, layer.left + layer.width);
        bottom = std::min(bottom, layer.top + layer.height);
    }
    ClipMask const* clip = state.clip.get();
    float const zero[4] = { 0, 0, 0, 0 };
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            std::size_t const at = surface.offset(x, y);
            float c = 1;
            if (clip) {
                std::uint8_t const covered = (*clip)[at / 4];
                if (covered == 0)
                    continue;
                c = to_unit(covered);
            }
            float const* s = layer.contains(x, y) ? layer.at(x, y) : zero;
            if (s[3] <= 0 && s[0] <= 0 && s[1] <= 0 && s[2] <= 0 && bounded(op))
                continue;
            std::uint8_t* p = surface.pixels.data() + at;
            float const d[4] = { to_unit(p[0]), to_unit(p[1]), to_unit(p[2]), to_unit(p[3]) };
            float r[4];
            composite_pixel(op, s, d, r);
            for (int k = 0; k < 4; ++k) {
                float const v = d[k] + (r[k] - d[k]) * c;
                p[k] = to_byte(v);
            }
            // A color never exceeds its alpha once premultiplied.
            p[0] = std::min(p[0], p[3]);
            p[1] = std::min(p[1], p[3]);
            p[2] = std::min(p[2], p[3]);
        }
    }
}

// Three box blurs one after another along an axis, as SVG's feGaussianBlur
// approximates a Gaussian of deviation sigma (Filter Effects section9.3).
void box_blur_line(std::vector<float>& line, std::vector<float>& scratch, int size, int offset)
{
    int const n = static_cast<int>(line.size());
    if (size <= 1 || n == 0)
        return;
    scratch.assign(line.size(), 0);
    // Pixel i averages [i - offset, i - offset + size).
    double sum = 0;
    int const first = -offset;
    for (int j = first; j < first + size; ++j)
        sum += (j >= 0 && j < n) ? static_cast<double>(line[static_cast<std::size_t>(j)]) : 0.0;
    for (int i = 0; i < n; ++i) {
        scratch[static_cast<std::size_t>(i)] = static_cast<float>(sum / size);
        int const leaving = i - offset;
        int const entering = i - offset + size;
        if (leaving >= 0 && leaving < n)
            sum -= static_cast<double>(line[static_cast<std::size_t>(leaving)]);
        if (entering >= 0 && entering < n)
            sum += static_cast<double>(line[static_cast<std::size_t>(entering)]);
    }
    line.swap(scratch);
}

void gaussian_line(std::vector<float>& line, std::vector<float>& scratch, double sigma)
{
    int const d = static_cast<int>(std::floor(sigma * 3 * 2.5066282746310002 / 4 + 0.5));
    if (d <= 1)
        return;
    if (d % 2 == 1) {
        for (int pass = 0; pass < 3; ++pass)
            box_blur_line(line, scratch, d, d / 2);
    } else {
        box_blur_line(line, scratch, d, d / 2);
        box_blur_line(line, scratch, d, d / 2 - 1);
        box_blur_line(line, scratch, d + 1, d / 2);
    }
}

// The shadow a layer casts: its alpha moved by the offset, blurred, and
// colored in the shadow color.
Layer shadow_of(Layer const& layer, DrawState const& state)
{
    Layer shadow;
    double const sigma = state.shadow_blur / 2;
    int const spread = sigma > 0 ? static_cast<int>(std::ceil(sigma * 3)) + 2 : 0;
    int const ox = static_cast<int>(std::lround(state.shadow_offset_x));
    int const oy = static_cast<int>(std::lround(state.shadow_offset_y));
    shadow.left = layer.left + ox - spread;
    shadow.top = layer.top + oy - spread;
    shadow.width = layer.width + 2 * spread;
    shadow.height = layer.height + 2 * spread;
    if (static_cast<std::size_t>(shadow.width) * static_cast<std::size_t>(shadow.height) > max_layer_pixels)
        return Layer {};
    std::vector<float> alpha(static_cast<std::size_t>(shadow.width) * static_cast<std::size_t>(shadow.height), 0);
    for (int y = 0; y < layer.height; ++y) {
        for (int x = 0; x < layer.width; ++x)
            alpha[static_cast<std::size_t>(y + spread) * static_cast<std::size_t>(shadow.width) + static_cast<std::size_t>(x + spread)]
                = layer.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(layer.width) + static_cast<std::size_t>(x)) * 4u + 3u];
    }
    if (sigma > 0) {
        std::vector<float> line;
        std::vector<float> scratch;
        for (int y = 0; y < shadow.height; ++y) {
            auto const row = alpha.begin() + static_cast<std::ptrdiff_t>(y) * shadow.width;
            line.assign(row, row + shadow.width);
            gaussian_line(line, scratch, sigma);
            std::copy(line.begin(), line.end(), row);
        }
        for (int x = 0; x < shadow.width; ++x) {
            line.resize(static_cast<std::size_t>(shadow.height));
            for (int y = 0; y < shadow.height; ++y)
                line[static_cast<std::size_t>(y)] = alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(shadow.width) + static_cast<std::size_t>(x)];
            gaussian_line(line, scratch, sigma);
            for (int y = 0; y < shadow.height; ++y)
                alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(shadow.width) + static_cast<std::size_t>(x)] = line[static_cast<std::size_t>(y)];
        }
    }
    float color[4];
    premultiplied(state.shadow_color, color);
    shadow.rgba.resize(alpha.size() * 4u);
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        for (int k = 0; k < 4; ++k)
            shadow.rgba[i * 4u + static_cast<std::size_t>(k)] = color[k] * alpha[i];
    }
    return shadow;
}

bool draws_shadow(DrawState const& state)
{
    return state.shadow_color.a != 0
        && (state.shadow_blur > 0 || state.shadow_offset_x != 0 || state.shadow_offset_y != 0);
}

// The layer and its shadow onto the surface.
void composite_with_shadow(Surface& surface, Layer const& layer, DrawState const& state)
{
    if (draws_shadow(state) && !layer.empty())
        composite_layer(surface, shadow_of(layer, state), state);
    composite_layer(surface, layer, state);
}

std::vector<svg::Polygon> polygons_of(std::vector<svg::Polyline> lines)
{
    std::vector<svg::Polygon> polygons;
    polygons.reserve(lines.size());
    for (svg::Polyline& line : lines)
        polygons.push_back(std::move(line.points));
    return polygons;
}

bool is_similarity(Transform const& t)
{
    double const tolerance = 1e-9 * std::max({ 1.0, std::abs(t.a), std::abs(t.b), std::abs(t.c), std::abs(t.d) });
    return (std::abs(t.a - t.d) <= tolerance && std::abs(t.b + t.c) <= tolerance)
        || (std::abs(t.a + t.d) <= tolerance && std::abs(t.b - t.c) <= tolerance);
}

// The canvas's rules for what a stroke traces (HTML section4.12.5.1.5, "trace a
// path"): zero-length segments are pruned, and a subpath left with a single
// point is not stroked at all. A closed subpath of two points is a line
// traced out and back: joins at both ends, never caps.
std::vector<svg::Polygon> stroke_lines(std::vector<svg::Polyline> const& lines, svg::StrokeStyle const& pen)
{
    struct Kept {
        svg::Polyline line;
        bool out_and_back = false;
    };
    std::vector<Kept> kept;
    std::vector<svg::Point> turns; // the ends of the out-and-back lines
    for (svg::Polyline const& line : lines) {
        svg::Polyline pruned;
        pruned.closed = line.closed;
        for (svg::Point const& p : line.points) {
            if (pruned.points.empty() || pruned.points.back().x != p.x || pruned.points.back().y != p.y)
                pruned.points.push_back(p);
        }
        if (pruned.closed && pruned.points.size() > 1 && pruned.points.front().x == pruned.points.back().x
            && pruned.points.front().y == pruned.points.back().y)
            pruned.points.pop_back();
        if (pruned.points.size() < 2)
            continue;
        bool const out_and_back = pruned.closed && pruned.points.size() == 2;
        if (out_and_back) {
            // Stroked open with butt ends; a round join is added at each end below.
            pruned.closed = false;
            turns.push_back(pruned.points[0]);
            turns.push_back(pruned.points[1]);
        }
        kept.push_back(Kept { std::move(pruned), out_and_back });
    }
    std::vector<svg::Polygon> polygons;
    for (Kept const& entry : kept) {
        svg::StrokeStyle style = pen;
        if (entry.out_and_back)
            style.cap = svg::LineCap::Butt;
        std::vector<svg::Polygon> part = svg::stroke({ entry.line }, style);
        polygons.insert(polygons.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    if (pen.join == svg::LineJoin::Round) {
        svg::StrokeStyle dot = pen;
        dot.cap = svg::LineCap::Round;
        dot.dashes.clear();
        for (svg::Point const& p : turns) {
            std::vector<svg::Polygon> part = svg::stroke({ svg::Polyline { { p }, false } }, dot);
            polygons.insert(polygons.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        }
    }
    return polygons;
}

// How closely a curve is followed before it is stroked with a pen this wide:
// a chord's error grows with the pen's reach from it, so a wide pen follows
// the curve more closely, and its ends square to the curve's own tangent.
float pen_tolerance(float width)
{
    float const half = width / 2;
    return half > 4 ? 0.1f * 4 / half : 0.1f;
}

// A stroke's outline polygons in device pixels.
std::vector<svg::Polygon> stroke_polygons(svg::Path const& device_path, svg::StrokeStyle const& pen, Transform const& transform)
{
    double const scale = transform.scale_factor();
    if (!(scale > 0) || !std::isfinite(scale))
        return {};
    if (is_similarity(transform)) {
        // A pen that stays round under the transform: stroked where it lands,
        // every length scaled with it.
        svg::StrokeStyle scaled = pen;
        auto const s = static_cast<float>(scale);
        scaled.width *= s;
        for (float& dash : scaled.dashes)
            dash *= s;
        scaled.dash_offset *= s;
        return stroke_lines(svg::flatten(device_path, pen_tolerance(scaled.width)), scaled);
    }
    std::optional<Transform> const inverse = transform.inverse();
    if (!inverse)
        return {};
    svg::Path const user = device_path.transformed(inverse->to_matrix());
    std::vector<svg::Polygon> polygons
        = stroke_lines(svg::flatten(user, static_cast<float>(static_cast<double>(pen_tolerance(pen.width * static_cast<float>(scale))) / scale)), pen);
    svg::Matrix const m = transform.to_matrix();
    for (svg::Polygon& polygon : polygons) {
        for (svg::Point& point : polygon)
            point = m.apply(point);
    }
    return polygons;
}

// Whether a point is inside polygons by a fill rule, or on one of their edges.
bool inside_polygons(std::vector<svg::Polygon> const& polygons, svg::FillRule rule, double x, double y)
{
    int winding = 0;
    int crossings = 0;
    for (svg::Polygon const& polygon : polygons) {
        std::size_t const n = polygon.size();
        if (n < 2)
            continue;
        for (std::size_t i = 0; i < n; ++i) {
            svg::Point const a = polygon[i];
            svg::Point const b = polygon[(i + 1) % n];
            double const ax = static_cast<double>(a.x);
            double const ay = static_cast<double>(a.y);
            double const bx = static_cast<double>(b.x);
            double const by = static_cast<double>(b.y);
            // On the edge counts as inside.
            double const ex = bx - ax;
            double const ey = by - ay;
            double const length2 = ex * ex + ey * ey;
            if (length2 > 0) {
                double t = ((x - ax) * ex + (y - ay) * ey) / length2;
                t = std::clamp(t, 0.0, 1.0);
                double const qx = ax + t * ex - x;
                double const qy = ay + t * ey - y;
                if (qx * qx + qy * qy < 1e-10)
                    return true;
            } else if (ax == x && ay == y) {
                return true;
            }
            if ((ay <= y) != (by <= y)) {
                double const cross_x = ax + (y - ay) / (by - ay) * (bx - ax);
                if (cross_x > x) {
                    ++crossings;
                    winding += by > ay ? 1 : -1;
                }
            }
        }
    }
    return rule == svg::FillRule::NonZero ? winding != 0 : (crossings % 2) == 1;
}

} // namespace

// --- Transform --------------------------------------------------------------------

Transform Transform::multiply(Transform const& m) const
{
    return Transform {
        a * m.a + c * m.b,
        b * m.a + d * m.b,
        a * m.c + c * m.d,
        b * m.c + d * m.d,
        a * m.e + c * m.f + e,
        b * m.e + d * m.f + f,
    };
}

svg::Point Transform::apply(double x, double y) const
{
    return svg::Point { static_cast<float>(a * x + c * y + e), static_cast<float>(b * x + d * y + f) };
}

std::optional<Transform> Transform::inverse() const
{
    double const det = a * d - b * c;
    if (!std::isfinite(det) || det == 0)
        return std::nullopt;
    Transform m;
    m.a = d / det;
    m.b = -b / det;
    m.c = -c / det;
    m.d = a / det;
    m.e = (c * f - d * e) / det;
    m.f = (b * e - a * f) / det;
    return m;
}

bool Transform::is_finite() const
{
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d) && std::isfinite(e)
        && std::isfinite(f);
}

svg::Matrix Transform::to_matrix() const
{
    return svg::Matrix { static_cast<float>(a), static_cast<float>(b), static_cast<float>(c), static_cast<float>(d),
        static_cast<float>(e), static_cast<float>(f) };
}

double Transform::scale_factor() const { return std::sqrt(std::abs(a * d - b * c)); }

// --- Surface ----------------------------------------------------------------------

Surface::Surface(int w, int h)
    : width(std::max(0, w))
    , height(std::max(0, h))
    , pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0)
{
}

Surface Surface::from_bitmap(Bitmap const& bitmap)
{
    Surface out(bitmap.width(), bitmap.height());
    std::vector<std::uint8_t> const& in = bitmap.pixels();
    for (std::size_t i = 0; i + 3 < in.size(); i += 4) {
        unsigned const a = in[i + 3];
        out.pixels[i] = static_cast<std::uint8_t>((in[i] * a + 127) / 255);
        out.pixels[i + 1] = static_cast<std::uint8_t>((in[i + 1] * a + 127) / 255);
        out.pixels[i + 2] = static_cast<std::uint8_t>((in[i + 2] * a + 127) / 255);
        out.pixels[i + 3] = static_cast<std::uint8_t>(a);
    }
    return out;
}

Bitmap Surface::to_bitmap() const
{
    Bitmap out(std::max(1, width), std::max(1, height), Color { 0, 0, 0, 0 });
    std::vector<std::uint8_t>& store = out.writable_pixels();
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        unsigned const a = pixels[i + 3];
        if (a == 0) {
            store[i] = store[i + 1] = store[i + 2] = store[i + 3] = 0;
            continue;
        }
        store[i] = static_cast<std::uint8_t>(std::min(255u, (pixels[i] * 255u + a / 2) / a));
        store[i + 1] = static_cast<std::uint8_t>(std::min(255u, (pixels[i + 1] * 255u + a / 2) / a));
        store[i + 2] = static_cast<std::uint8_t>(std::min(255u, (pixels[i + 2] * 255u + a / 2) / a));
        store[i + 3] = static_cast<std::uint8_t>(a);
    }
    return out;
}

Surface Surface::crop(int x, int y, int w, int h) const
{
    Surface out(w, h);
    for (int row = 0; row < out.height; ++row) {
        int const sy = y + row;
        if (sy < 0 || sy >= height)
            continue;
        for (int col = 0; col < out.width; ++col) {
            int const sx = x + col;
            if (sx < 0 || sx >= width)
                continue;
            std::copy_n(pixels.data() + offset(sx, sy), 4, out.pixels.data() + out.offset(col, row));
        }
    }
    return out;
}

// --- Names ------------------------------------------------------------------------

namespace {

constexpr std::pair<std::string_view, Composite> composite_names[] = {
    { "source-over", Composite::SourceOver },
    { "source-in", Composite::SourceIn },
    { "source-out", Composite::SourceOut },
    { "source-atop", Composite::SourceAtop },
    { "destination-over", Composite::DestinationOver },
    { "destination-in", Composite::DestinationIn },
    { "destination-out", Composite::DestinationOut },
    { "destination-atop", Composite::DestinationAtop },
    { "lighter", Composite::Lighter },
    { "copy", Composite::Copy },
    { "xor", Composite::Xor },
    { "clear", Composite::Clear },
    { "multiply", Composite::Multiply },
    { "screen", Composite::Screen },
    { "overlay", Composite::Overlay },
    { "darken", Composite::Darken },
    { "lighten", Composite::Lighten },
    { "color-dodge", Composite::ColorDodge },
    { "color-burn", Composite::ColorBurn },
    { "hard-light", Composite::HardLight },
    { "soft-light", Composite::SoftLight },
    { "difference", Composite::Difference },
    { "exclusion", Composite::Exclusion },
    { "hue", Composite::Hue },
    { "saturation", Composite::Saturation },
    { "color", Composite::Color },
    { "luminosity", Composite::Luminosity },
};

} // namespace

std::optional<Composite> composite_by_name(std::string_view name)
{
    for (auto const& [text, op] : composite_names) {
        if (text == name)
            return op;
    }
    return std::nullopt;
}

std::string_view composite_name(Composite op)
{
    for (auto const& [text, value] : composite_names) {
        if (value == op)
            return text;
    }
    return "source-over";
}

void Gradient::add_stop(double offset, Color color)
{
    auto const at = std::upper_bound(stops.begin(), stops.end(), offset,
        [](double value, ColorStop const& stop) { return value < stop.offset; });
    stops.insert(at, ColorStop { offset, color });
}

// --- Drawing ----------------------------------------------------------------------

namespace {

// The layer a coverage mask gives with a shader and the global alpha: over
// the bitmap, or over the whole mask when a shadow is cast from it.
Layer layer_of(Surface const& surface, svg::Mask const& coverage, Shader const& shader, DrawState const& state)
{
    Layer layer;
    if (coverage.empty())
        return layer;
    bool const whole = draws_shadow(state);
    layer.left = whole ? coverage.left : std::max(0, coverage.left);
    layer.top = whole ? coverage.top : std::max(0, coverage.top);
    int const right = whole ? coverage.left + coverage.width : std::min(surface.width, coverage.left + coverage.width);
    int const bottom = whole ? coverage.top + coverage.height : std::min(surface.height, coverage.top + coverage.height);
    double const global_alpha = state.global_alpha;
    layer.width = right - layer.left;
    layer.height = bottom - layer.top;
    if (layer.width <= 0 || layer.height <= 0
        || static_cast<std::size_t>(layer.width) * static_cast<std::size_t>(layer.height) > max_layer_pixels)
        return Layer {};
    layer.rgba.assign(static_cast<std::size_t>(layer.width) * static_cast<std::size_t>(layer.height) * 4u, 0);
    auto const alpha = static_cast<float>(global_alpha);
    for (int y = layer.top; y < bottom; ++y) {
        for (int x = layer.left; x < right; ++x) {
            std::uint8_t const covered = coverage.at(x, y);
            if (covered == 0)
                continue;
            float color[4];
            shader.at(x, y, color);
            float const k = to_unit(covered) * alpha;
            float* out = layer.at(x, y);
            for (int i = 0; i < 4; ++i)
                out[i] = color[i] * k;
        }
    }
    return layer;
}

} // namespace

void paint_mask(Surface& surface, svg::Mask const& coverage, Style const& style, DrawState const& state)
{
    if (surface.width <= 0 || surface.height <= 0)
        return;
    Shader const shader(style, state.transform, state.smoothing);
    Layer const layer = layer_of(surface, coverage, shader, state);
    composite_with_shadow(surface, layer, state);
}

namespace {

// The pixels a shape is scan-converted over: the bitmap, and with a shadow
// cast, as far beyond it as the shadow could land back on it from.
struct Region {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

Region raster_region(Surface const& surface, DrawState const* state)
{
    Region region { 0, 0, surface.width, surface.height };
    if (!state || !draws_shadow(*state))
        return region;
    double const reach = state->shadow_blur / 2 * 3 + 4;
    double const ox = std::clamp(state->shadow_offset_x, -1e5, 1e5);
    double const oy = std::clamp(state->shadow_offset_y, -1e5, 1e5);
    region.left = std::min(0, static_cast<int>(std::floor(-ox - reach)));
    region.top = std::min(0, static_cast<int>(std::floor(-oy - reach)));
    region.right = std::max(surface.width, static_cast<int>(std::ceil(surface.width - ox + reach)));
    region.bottom = std::max(surface.height, static_cast<int>(std::ceil(surface.height - oy + reach)));
    return region;
}

} // namespace

svg::Mask fill_coverage(Surface const& surface, svg::Path const& device_path, svg::FillRule rule, DrawState const* state)
{
    Region const r = raster_region(surface, state);
    return svg::rasterize(polygons_of(svg::flatten(device_path, 0.1f)), rule, r.left, r.top, r.right, r.bottom);
}

svg::Mask stroke_coverage(Surface const& surface, svg::Path const& device_path, svg::StrokeStyle const& pen,
    DrawState const& state)
{
    Region const r = raster_region(surface, &state);
    return svg::rasterize(stroke_polygons(device_path, pen, state.transform), svg::FillRule::NonZero, r.left, r.top,
        r.right, r.bottom);
}

void draw_image(Surface& surface, Surface const& source, double sx, double sy, double sw, double sh, double dx,
    double dy, double dw, double dh, DrawState const& state)
{
    if (surface.width <= 0 || surface.height <= 0 || sw <= 0 || sh <= 0 || dw == 0 || dh == 0)
        return;
    // The picture as a pattern drawn once, its source rectangle placed over
    // the destination rectangle; its edges held rather than faded.
    auto pattern = std::make_shared<Pattern>();
    int const left = static_cast<int>(std::floor(sx));
    int const top = static_cast<int>(std::floor(sy));
    int const right = static_cast<int>(std::ceil(sx + sw));
    int const bottom = static_cast<int>(std::ceil(sy + sh));
    pattern->image = std::make_shared<Surface const>(source.crop(left, top, right - left, bottom - top));
    pattern->repeat_x = false;
    pattern->repeat_y = false;
    double const scale_x = dw / sw;
    double const scale_y = dh / sh;
    pattern->transform = Transform { scale_x, 0, 0, scale_y, dx - (sx - left) * scale_x, dy - (sy - top) * scale_y };
    Style style;
    style.pattern = pattern;
    PathBuilder rectangle;
    rectangle.rect(state.transform, dx, dy, dw, dh);
    svg::Mask const coverage = fill_coverage(surface, rectangle.path, svg::FillRule::NonZero, &state);
    Shader const shader(style, state.transform, state.smoothing, true);
    Layer const layer = layer_of(surface, coverage, shader, state);
    composite_with_shadow(surface, layer, state);
}

void clear_rect(Surface& surface, double x, double y, double w, double h, DrawState const& state)
{
    PathBuilder rectangle;
    rectangle.rect(state.transform, x, y, w, h);
    svg::Mask const coverage = fill_coverage(surface, rectangle.path, svg::FillRule::NonZero);
    if (coverage.empty())
        return;
    for (int py = std::max(0, coverage.top); py < std::min(surface.height, coverage.top + coverage.height); ++py) {
        for (int px = std::max(0, coverage.left); px < std::min(surface.width, coverage.left + coverage.width); ++px) {
            std::size_t const at = surface.offset(px, py);
            float k = to_unit(coverage.at(px, py));
            if (state.clip)
                k *= to_unit((*state.clip)[at / 4]);
            if (k <= 0)
                continue;
            for (int i = 0; i < 4; ++i)
                surface.pixels[at + static_cast<std::size_t>(i)]
                    = to_byte(to_unit(surface.pixels[at + static_cast<std::size_t>(i)]) * (1 - k));
        }
    }
}

std::shared_ptr<ClipMask const> intersect_clip(Surface const& surface, std::shared_ptr<ClipMask const> const& clip,
    svg::Path const& device_path, svg::FillRule rule)
{
    auto out = std::make_shared<ClipMask>(static_cast<std::size_t>(surface.width) * static_cast<std::size_t>(surface.height), 0);
    svg::Mask const coverage = fill_coverage(surface, device_path, rule);
    if (!coverage.empty()) {
        for (int y = std::max(0, coverage.top); y < std::min(surface.height, coverage.top + coverage.height); ++y) {
            for (int x = std::max(0, coverage.left); x < std::min(surface.width, coverage.left + coverage.width); ++x) {
                std::size_t const i = static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width) + static_cast<std::size_t>(x);
                unsigned value = coverage.at(x, y);
                if (clip)
                    value = (value * (*clip)[i] + 127) / 255;
                (*out)[i] = static_cast<std::uint8_t>(value);
            }
        }
    }
    return out;
}

bool point_in_path(svg::Path const& device_path, svg::FillRule rule, double x, double y)
{
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    return inside_polygons(polygons_of(svg::flatten(device_path, 0.05f)), rule, x, y);
}

bool point_in_stroke(svg::Path const& device_path, svg::StrokeStyle const& pen, Transform const& transform, double x,
    double y)
{
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    return inside_polygons(stroke_polygons(device_path, pen, transform), svg::FillRule::NonZero, x, y);
}

// --- Paths ------------------------------------------------------------------------

void PathBuilder::move_to(Transform const& t, double x, double y) { path.move_to(t.apply(x, y)); }

void PathBuilder::line_to(Transform const& t, double x, double y)
{
    if (!has_subpath()) {
        move_to(t, x, y);
        return;
    }
    path.line_to(t.apply(x, y));
}

void PathBuilder::quadratic_to(Transform const& t, double cx, double cy, double x, double y)
{
    if (!has_subpath())
        move_to(t, cx, cy);
    path.quadratic_to(t.apply(cx, cy), t.apply(x, y));
}

void PathBuilder::bezier_to(Transform const& t, double c1x, double c1y, double c2x, double c2y, double x, double y)
{
    if (!has_subpath())
        move_to(t, c1x, c1y);
    path.cubic_to(t.apply(c1x, c1y), t.apply(c2x, c2y), t.apply(x, y));
}

namespace {

struct Vec {
    double x = 0;
    double y = 0;
};

double length_of(Vec v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// A circular arc of less than a half turn about `centre`, from a to b (both
// radius away from it), as one or two cubics, points through t.
void add_short_arc(svg::Path& path, Transform const& t, Vec centre, Vec a, Vec b, double radius)
{
    Vec const va { a.x - centre.x, a.y - centre.y };
    Vec const vb { b.x - centre.x, b.y - centre.y };
    double const r2 = radius * radius;
    double const cosine = std::clamp((va.x * vb.x + va.y * vb.y) / r2, -1.0, 1.0);
    auto const piece = [&](Vec from, Vec to, double cos_theta) {
        // k = 4/3 tan(theta / 4), from the cosine by the half-angle formulas.
        double const tan_half = std::sqrt(std::max(0.0, (1 - cos_theta) / (1 + cos_theta)));
        double const tan_quarter = tan_half / (1 + std::sqrt(1 + tan_half * tan_half));
        double const k = 4.0 / 3.0 * tan_quarter;
        Vec const vf { from.x - centre.x, from.y - centre.y };
        Vec const vt { to.x - centre.x, to.y - centre.y };
        double const dot = (vf.x * vt.x + vf.y * vt.y) / r2;
        Vec tf { vt.x - dot * vf.x, vt.y - dot * vf.y };
        Vec tt { vf.x - dot * vt.x, vf.y - dot * vt.y };
        double const lf = length_of(tf);
        double const lt = length_of(tt);
        if (lf > 0) {
            tf.x *= radius / lf;
            tf.y *= radius / lf;
        }
        if (lt > 0) {
            tt.x *= radius / lt;
            tt.y *= radius / lt;
        }
        path.cubic_to(t.apply(from.x + k * tf.x, from.y + k * tf.y), t.apply(to.x + k * tt.x, to.y + k * tt.y),
            t.apply(to.x, to.y));
    };
    if (cosine >= 0) {
        piece(a, b, cosine);
        return;
    }
    // Wider than a quarter turn: split at the middle.
    Vec mid { va.x + vb.x, va.y + vb.y };
    double const lm = length_of(mid);
    if (lm == 0)
        return;
    mid = Vec { centre.x + mid.x / lm * radius, centre.y + mid.y / lm * radius };
    double const half_cos = std::sqrt((1 + cosine) / 2);
    piece(a, mid, half_cos);
    piece(mid, b, half_cos);
}

} // namespace

void PathBuilder::arc_to(Transform const& t, double x1, double y1, double x2, double y2, double radius)
{
    if (!has_subpath())
        move_to(t, x1, y1);
    std::optional<Transform> const inverse = t.inverse();
    if (!inverse)
        return;
    svg::Point const last = path.current();
    svg::Point const p0f = inverse->apply(static_cast<double>(last.x), static_cast<double>(last.y));
    Vec const p0 { static_cast<double>(p0f.x), static_cast<double>(p0f.y) };
    Vec const p1 { x1, y1 };
    Vec const p2 { x2, y2 };
    if ((p0.x == p1.x && p0.y == p1.y) || (p1.x == p2.x && p1.y == p2.y) || radius == 0) {
        line_to(t, x1, y1);
        return;
    }
    Vec const v1 { p0.x - p1.x, p0.y - p1.y };
    Vec const v2 { p2.x - p1.x, p2.y - p1.y };
    double const l1 = length_of(v1);
    double const l2 = length_of(v2);
    double const cross = v1.x * v2.y - v1.y * v2.x;
    if (std::abs(cross) <= 1e-12 * l1 * l2) {
        line_to(t, x1, y1);
        return;
    }
    Vec const u1 { v1.x / l1, v1.y / l1 };
    Vec const u2 { v2.x / l2, v2.y / l2 };
    double const cosine = std::clamp(u1.x * u2.x + u1.y * u2.y, -1.0, 1.0);
    double const tan_half = std::sqrt((1 - cosine) / (1 + cosine));
    double const sin_half = std::sqrt((1 - cosine) / 2);
    double const along = radius / tan_half;
    Vec const t1 { p1.x + u1.x * along, p1.y + u1.y * along };
    Vec const t2 { p1.x + u2.x * along, p1.y + u2.y * along };
    Vec bisector { u1.x + u2.x, u1.y + u2.y };
    double const lb = length_of(bisector);
    bisector.x /= lb;
    bisector.y /= lb;
    double const to_centre = radius / sin_half;
    Vec const centre { p1.x + bisector.x * to_centre, p1.y + bisector.y * to_centre };
    line_to(t, t1.x, t1.y);
    add_short_arc(path, t, centre, t1, t2, radius);
}

void PathBuilder::ellipse(Transform const& t, double x, double y, double rx, double ry, double rotation, double start,
    double end, bool anticlockwise)
{
    // The sweep from the start angle to the end, in the direction asked:
    // a whole turn at most, and a whole turn when the end is a whole number
    // of turns the other way round (as every engine draws arc(0, 2 pi) in
    // either direction).
    double sweep;
    if (!anticlockwise) {
        if (end - start >= two_pi)
            sweep = two_pi;
        else if (start > end)
            sweep = two_pi - std::fmod(start - end, two_pi);
        else
            sweep = end - start;
    } else {
        if (start - end >= two_pi)
            sweep = -two_pi;
        else if (start < end)
            sweep = -(two_pi - std::fmod(end - start, two_pi));
        else
            sweep = end - start;
    }
    SineCosine const turned = sine_cosine(rotation);
    double const cos_r = turned.cosine;
    double const sin_r = turned.sine;
    auto const point = [&](double cos_a, double sin_a) {
        double const ex = rx * cos_a;
        double const ey = ry * sin_a;
        return Vec { x + ex * cos_r - ey * sin_r, y + ex * sin_r + ey * cos_r };
    };
    auto const derivative = [&](double cos_a, double sin_a) {
        double const ex = -rx * sin_a;
        double const ey = ry * cos_a;
        return Vec { ex * cos_r - ey * sin_r, ex * sin_r + ey * cos_r };
    };
    SineCosine const at_start = sine_cosine(start);
    double cos_a = at_start.cosine;
    double sin_a = at_start.sine;
    Vec const first = point(cos_a, sin_a);
    if (has_subpath())
        line_to(t, first.x, first.y);
    else
        move_to(t, first.x, first.y);
    if (sweep == 0)
        return;
    int const pieces = std::max(1, static_cast<int>(std::ceil(std::abs(sweep) / (pi / 2) - 1e-9)));
    double const step = sweep / pieces;
    SineCosine const quarter = sine_cosine(step / 4);
    double const k = 4.0 / 3.0 * quarter.sine / quarter.cosine;
    double angle = start;
    for (int i = 0; i < pieces; ++i) {
        double const next = i + 1 == pieces ? start + sweep : angle + step;
        SineCosine const at_next = sine_cosine(next);
        double const cos_b = at_next.cosine;
        double const sin_b = at_next.sine;
        Vec const a = point(cos_a, sin_a);
        Vec const b = point(cos_b, sin_b);
        Vec const da = derivative(cos_a, sin_a);
        Vec const db = derivative(cos_b, sin_b);
        path.cubic_to(t.apply(a.x + k * da.x, a.y + k * da.y), t.apply(b.x - k * db.x, b.y - k * db.y), t.apply(b.x, b.y));
        angle = next;
        cos_a = cos_b;
        sin_a = sin_b;
    }
}

void PathBuilder::rect(Transform const& t, double x, double y, double w, double h)
{
    move_to(t, x, y);
    path.line_to(t.apply(x + w, y));
    path.line_to(t.apply(x + w, y + h));
    path.line_to(t.apply(x, y + h));
    path.close();
    move_to(t, x, y);
}

void PathBuilder::round_rect(Transform const& outer, double x, double y, double w, double h, std::array<svg::Point, 4> r)
{
    // A flipped rectangle mirrors its corners (HTML section4.12.5.1.14, roundRect
    // steps 9 and 10): it is drawn from (x, y) in a frame whose axes point
    // the way w and h do, so the first radius stays at the corner (x, y) and
    // the path is traced in the mirrored direction.
    double const flip_x = w < 0 ? -1 : 1;
    double const flip_y = h < 0 ? -1 : 1;
    w = std::abs(w);
    h = std::abs(h);
    Transform const t = outer.multiply(Transform { flip_x, 0, 0, flip_y, x, y });
    double const start_x = x;
    double const start_y = y;
    x = 0;
    y = 0;
    // Corners that overrun an edge shrink together.
    auto const d = [](float value) { return static_cast<double>(value); };
    double const top = d(r[0].x) + d(r[1].x);
    double const right = d(r[1].y) + d(r[2].y);
    double const bottom = d(r[2].x) + d(r[3].x);
    double const left = d(r[0].y) + d(r[3].y);
    double scale = 1;
    if (top > w)
        scale = std::min(scale, w / top);
    if (right > h)
        scale = std::min(scale, h / right);
    if (bottom > w)
        scale = std::min(scale, w / bottom);
    if (left > h)
        scale = std::min(scale, h / left);
    if (scale < 1) {
        for (svg::Point& corner : r) {
            corner.x = static_cast<float>(static_cast<double>(corner.x) * scale);
            corner.y = static_cast<float>(static_cast<double>(corner.y) * scale);
        }
    }
    // Each corner a quarter ellipse as one cubic, from where the edge before
    // it stops to where the edge after it starts.
    constexpr double kappa = 0.5522847498307936;
    auto const corner = [&](double c1x, double c1y, double c2x, double c2y, double ex, double ey) {
        path.cubic_to(t.apply(c1x, c1y), t.apply(c2x, c2y), t.apply(ex, ey));
    };
    double const ulx = d(r[0].x), uly = d(r[0].y), urx = d(r[1].x), ury = d(r[1].y);
    double const lrx = d(r[2].x), lry = d(r[2].y), llx = d(r[3].x), lly = d(r[3].y);
    move_to(t, x + ulx, y);
    path.line_to(t.apply(x + w - urx, y));
    corner(x + w - urx + urx * kappa, y, x + w, y + ury - ury * kappa, x + w, y + ury);
    path.line_to(t.apply(x + w, y + h - lry));
    corner(x + w, y + h - lry + lry * kappa, x + w - lrx + lrx * kappa, y + h, x + w - lrx, y + h);
    path.line_to(t.apply(x + llx, y + h));
    corner(x + llx - llx * kappa, y + h, x, y + h - lly + lly * kappa, x, y + h - lly);
    path.line_to(t.apply(x, y + uly));
    corner(x, y + uly - uly * kappa, x + ulx - ulx * kappa, y, x + ulx, y);
    path.close();
    move_to(outer, start_x, start_y);
}

void PathBuilder::close()
{
    if (has_subpath())
        path.close();
}

void PathBuilder::add_path(svg::Path const& other, Transform const& t)
{
    svg::Path const moved = other.transformed(t.to_matrix());
    path.segments.insert(path.segments.end(), moved.segments.begin(), moved.segments.end());
}

// --- Text -------------------------------------------------------------------------

namespace {

text::FontStack const& stack_for(Font const& font)
{
    text::FontRequest request;
    request.families = font.families;
    request.weight = font.weight;
    request.italic = font.italic;
    request.stretch = font.stretch;
    return text::FontManager::instance().resolve(request);
}

struct PlacedGlyph {
    text::FontStack::Glyph glyph;
    double x = 0; // pen position, text units
};

// The glyphs of a run and their pen positions, and the advance of the whole.
std::vector<PlacedGlyph> place(std::u32string_view text, Font const& font, TextLayout const& layout,
    text::FontStack const& stack, double& advance)
{
    std::vector<PlacedGlyph> placed;
    double x = 0;
    text::FontStack::Glyph previous { nullptr, 0 };
    // The run in the order it is drawn (UAX #9, one paragraph whose base
    // direction is the context's), a right-to-left character mirrored.
    std::u32string visual;
    bool reorder = layout.rtl;
    for (char32_t const c : text)
        reorder = reorder || strong_direction(c) == StrongDirection::Rtl;
    if (reorder) {
        BidiParagraph const paragraph = bidi_resolve(text, static_cast<std::uint8_t>(layout.rtl ? 1 : 0));
        for (std::size_t const index : bidi_visual_order(paragraph))
            visual += paragraph.levels[index] % 2 == 1 ? bidi_mirrored(text[index]) : text[index];
        text = visual;
    }
    for (char32_t const c : text) {
        // A control character takes no room and draws nothing.
        if (c < 0x20 || (c >= 0x7f && c < 0xa0))
            continue;
        text::FontStack::Glyph const glyph = stack.glyph_for(c);
        if (layout.kerning && previous.face == glyph.face && previous.face)
            x += static_cast<double>(glyph.face->kerning(previous.glyph, glyph.glyph, font.size));
        placed.push_back(PlacedGlyph { glyph, x });
        x += static_cast<double>(glyph.face->advance(glyph.glyph, font.size) + layout.letter_spacing);
        if (c == U' ')
            x += static_cast<double>(layout.word_spacing);
        previous = glyph;
    }
    advance = x;
    return placed;
}

struct VerticalMetrics {
    double ascent = 0;
    double descent = 0;
    double em_ascent = 0;
    double em_descent = 0;
};

VerticalMetrics vertical_metrics(text::FontStack const& stack, Font const& font)
{
    text::FaceMetrics const m = stack.primary().metrics(font.size);
    VerticalMetrics v;
    v.ascent = static_cast<double>(m.ascent);
    v.descent = static_cast<double>(m.descent);
    double const size = static_cast<double>(font.size);
    double const total = v.ascent + v.descent;
    v.em_ascent = total > 0 ? size * v.ascent / total : size * 0.8;
    v.em_descent = size - v.em_ascent;
    return v;
}

// Where the textBaseline's line lies, measured up from the alphabetic
// baseline.
double baseline_height(TextLayout::Baseline baseline, VerticalMetrics const& v)
{
    switch (baseline) {
    case TextLayout::Baseline::Top:
        return v.em_ascent;
    case TextLayout::Baseline::Hanging:
        return v.em_ascent * 0.8;
    case TextLayout::Baseline::Middle:
        return (v.em_ascent - v.em_descent) / 2;
    case TextLayout::Baseline::Alphabetic:
        return 0;
    case TextLayout::Baseline::Ideographic:
    case TextLayout::Baseline::Bottom:
        return -v.em_descent;
    }
    return 0;
}

// How far left of the point given the run begins, by the alignment.
double align_offset(TextLayout const& layout, double width)
{
    TextLayout::Align align = layout.align;
    if (align == TextLayout::Align::Start)
        align = layout.rtl ? TextLayout::Align::Right : TextLayout::Align::Left;
    else if (align == TextLayout::Align::End)
        align = layout.rtl ? TextLayout::Align::Left : TextLayout::Align::Right;
    switch (align) {
    case TextLayout::Align::Right:
        return width;
    case TextLayout::Align::Center:
        return width / 2;
    default:
        return 0;
    }
}

// The glyphs drawn white on transparent at `scale` device pixels to the text
// unit, the alphabetic baseline at `baseline_y` and the pen starting at
// `origin_x`: what the run's coverage is taken from.
void draw_run(Bitmap& target, std::vector<PlacedGlyph> const& placed, Font const& font, double scale, double origin_x,
    double baseline_y)
{
    auto const size = static_cast<float>(static_cast<double>(font.size) * scale);
    for (PlacedGlyph const& g : placed) {
        g.glyph.face->draw_glyph(target, g.glyph.glyph, static_cast<float>(origin_x + g.x * scale),
            static_cast<float>(baseline_y), size, Color { 255, 255, 255, 255 }, font.weight >= 600, font.italic);
    }
}

// A glyph mask widened to a pen of `radius` pixels, less the inside it
// leaves: the outline a stroke of the glyphs covers.
void outline_mask(std::vector<std::uint8_t>& alpha, int width, int height, int radius)
{
    if (radius < 1)
        radius = 1;
    auto const filtered = [&](bool grow) {
        std::vector<std::uint8_t> horizontal(alpha.size());
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                std::uint8_t v = grow ? 0 : 255;
                for (int k = -radius; k <= radius; ++k) {
                    int const sx = x + k;
                    std::uint8_t const s = (sx < 0 || sx >= width) ? 0 : alpha[static_cast<std::size_t>(y * width + sx)];
                    v = grow ? std::max(v, s) : std::min(v, s);
                }
                horizontal[static_cast<std::size_t>(y * width + x)] = v;
            }
        }
        std::vector<std::uint8_t> out(alpha.size());
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                std::uint8_t v = grow ? 0 : 255;
                for (int k = -radius; k <= radius; ++k) {
                    int const sy = y + k;
                    std::uint8_t const s = (sy < 0 || sy >= height) ? 0 : horizontal[static_cast<std::size_t>(sy * width + x)];
                    v = grow ? std::max(v, s) : std::min(v, s);
                }
                out[static_cast<std::size_t>(y * width + x)] = v;
            }
        }
        return out;
    };
    std::vector<std::uint8_t> const grown = filtered(true);
    std::vector<std::uint8_t> const shrunk = filtered(false);
    for (std::size_t i = 0; i < alpha.size(); ++i)
        alpha[i] = static_cast<std::uint8_t>(std::max(0, grown[i] - shrunk[i]));
}

} // namespace

TextMeasure measure_text(std::u32string_view text, Font const& font, TextLayout const& layout)
{
    text::FontStack const& stack = stack_for(font);
    double advance = 0;
    std::vector<PlacedGlyph> const placed = place(text, font, layout, stack, advance);
    VerticalMetrics const v = vertical_metrics(stack, font);
    double const b = baseline_height(layout.baseline, v);
    TextMeasure m;
    m.width = advance;
    m.font_ascent = v.ascent - b;
    m.font_descent = v.descent + b;
    m.em_ascent = v.em_ascent - b;
    m.em_descent = v.em_descent + b;
    // The baselines are measured down from the textBaseline's line.
    m.hanging_baseline = b - v.em_ascent * 0.8;
    m.alphabetic_baseline = b;
    m.ideographic_baseline = b + v.em_descent;
    double const start = align_offset(layout, advance);
    // The ink: the glyphs drawn once and looked at.
    double ink_left = 0;
    double ink_right = 0;
    double ink_top = 0;
    double ink_bottom = 0;
    bool inked = false;
    double const room = (advance + 2 * static_cast<double>(font.size) + 8) * (v.ascent + v.descent + 2 * static_cast<double>(font.size) + 8);
    if (!placed.empty() && text.size() <= 4096 && std::isfinite(room) && room < static_cast<double>(max_layer_pixels / 4)) {
        int const pad = static_cast<int>(std::ceil(font.size)) + 2;
        int const w = static_cast<int>(std::ceil(advance)) + 2 * pad;
        int const h = static_cast<int>(std::ceil(v.ascent + v.descent)) + 2 * pad;
        if (w > 0 && h > 0 && static_cast<std::size_t>(w) * static_cast<std::size_t>(h) < max_layer_pixels / 4) {
            Bitmap scratch(w, h, Color { 0, 0, 0, 0 });
            double const baseline_y = pad + v.ascent;
            draw_run(scratch, placed, font, 1, pad, baseline_y);
            int min_x = w;
            int max_x = -1;
            int min_y = h;
            int max_y = -1;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (scratch.pixels()[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u + 3u] == 0)
                        continue;
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                }
            }
            if (max_x >= 0) {
                inked = true;
                ink_left = min_x - pad;
                ink_right = max_x + 1 - pad;
                ink_top = baseline_y - min_y;
                ink_bottom = max_y + 1 - baseline_y;
            }
        }
    }
    if (inked) {
        m.actual_left = start - ink_left;
        m.actual_right = ink_right - start;
        m.actual_ascent = ink_top - b;
        m.actual_descent = ink_bottom + b;
    } else {
        m.actual_left = start;
        m.actual_right = -start;
        m.actual_ascent = -b;
        m.actual_descent = b;
    }
    return m;
}

svg::Mask text_coverage(Surface const& surface, std::u32string_view text, Font const& font, TextLayout const& layout,
    double x, double y, std::optional<double> max_width, Transform const& transform, std::optional<double> stroke_width)
{
    svg::Mask mask;
    if (text.empty() || surface.width <= 0 || surface.height <= 0 || !(font.size > 0))
        return mask;
    text::FontStack const& stack = stack_for(font);
    double advance = 0;
    std::vector<PlacedGlyph> const placed = place(text, font, layout, stack, advance);
    VerticalMetrics const v = vertical_metrics(stack, font);
    double squeeze = 1;
    if (max_width) {
        if (!(*max_width > 0))
            return mask;
        if (advance > *max_width)
            squeeze = *max_width / advance;
    }
    double const shift_x = -align_offset(layout, advance) * squeeze;
    double const shift_y = baseline_height(layout.baseline, v);
    // Text units to device pixels: the run's origin at its alphabetic
    // baseline, squeezed along its line.
    Transform const run = transform.multiply(Transform { squeeze, 0, 0, 1, x + shift_x, y + shift_y });
    double const scale = run.scale_factor();
    if (!(scale > 0) || !std::isfinite(scale) || !run.is_finite())
        return mask;
    double const pad_units = static_cast<double>(font.size) * 0.5 + (stroke_width ? *stroke_width : 0) + 2;
    // The run's box in text units.
    double const box_left = -pad_units;
    double const box_right = advance + pad_units;
    double const box_top = -(v.ascent + pad_units);
    double const box_bottom = v.descent + pad_units;
    // A run too large to draw, or placed where no int reaches, draws nothing.
    double const area = (box_right - box_left) * (box_bottom - box_top) * scale * scale;
    if (!std::isfinite(area) || area > static_cast<double>(max_layer_pixels / 4) || std::abs(run.e) > 1e7 || std::abs(run.f) > 1e7)
        return mask;
    int const stroke_radius = stroke_width ? static_cast<int>(std::lround(*stroke_width * scale / 2)) : 0;

    bool const upright = run.b == 0 && run.c == 0 && run.a > 0 && run.d > 0 && run.a == run.d;
    if (upright) {
        // Drawn where it lands: the scratch is a piece of the device.
        int const left = static_cast<int>(std::floor(run.e + box_left * scale));
        int const top = static_cast<int>(std::floor(run.f + box_top * scale));
        int const right = static_cast<int>(std::ceil(run.e + box_right * scale));
        int const bottom = static_cast<int>(std::ceil(run.f + box_bottom * scale));
        int const w = right - left;
        int const h = bottom - top;
        if (w <= 0 || h <= 0 || static_cast<std::size_t>(w) * static_cast<std::size_t>(h) > max_layer_pixels / 4)
            return mask;
        if (right < 0 || bottom < 0 || left > surface.width || top > surface.height)
            return mask;
        Bitmap scratch(w, h, Color { 0, 0, 0, 0 });
        draw_run(scratch, placed, font, scale, run.e - left, run.f - top);
        mask.left = left;
        mask.top = top;
        mask.width = w;
        mask.height = h;
        mask.alpha.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (std::size_t i = 0; i < mask.alpha.size(); ++i)
            mask.alpha[i] = scratch.pixels()[i * 4u + 3u];
        if (stroke_width)
            outline_mask(mask.alpha, w, h, stroke_radius);
        return mask;
    }
    // Any other transform: drawn upright at the size it lands at, then
    // resampled through the transform.
    auto const sw = static_cast<int>(std::ceil((box_right - box_left) * scale));
    auto const sh = static_cast<int>(std::ceil((box_bottom - box_top) * scale));
    if (sw <= 0 || sh <= 0 || static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) > max_layer_pixels / 4)
        return mask;
    Bitmap scratch(sw, sh, Color { 0, 0, 0, 0 });
    draw_run(scratch, placed, font, scale, -box_left * scale, -box_top * scale);
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh));
    for (std::size_t i = 0; i < alpha.size(); ++i)
        alpha[i] = scratch.pixels()[i * 4u + 3u];
    if (stroke_width)
        outline_mask(alpha, sw, sh, stroke_radius);
    std::optional<Transform> const inverse = run.inverse();
    if (!inverse)
        return mask;
    // The device box the run's box lands in.
    double min_x = 1e300;
    double min_y = 1e300;
    double max_x = -1e300;
    double max_y = -1e300;
    for (auto const& [cx, cy] : { std::pair { box_left, box_top }, std::pair { box_right, box_top },
             std::pair { box_left, box_bottom }, std::pair { box_right, box_bottom } }) {
        double const dx = run.a * cx + run.c * cy + run.e;
        double const dy = run.b * cx + run.d * cy + run.f;
        min_x = std::min(min_x, dx);
        min_y = std::min(min_y, dy);
        max_x = std::max(max_x, dx);
        max_y = std::max(max_y, dy);
    }
    int const left = std::max(0, static_cast<int>(std::floor(min_x)));
    int const top = std::max(0, static_cast<int>(std::floor(min_y)));
    int const right = std::min(surface.width, static_cast<int>(std::ceil(max_x)));
    int const bottom = std::min(surface.height, static_cast<int>(std::ceil(max_y)));
    if (right <= left || bottom <= top)
        return mask;
    mask.left = left;
    mask.top = top;
    mask.width = right - left;
    mask.height = bottom - top;
    mask.alpha.assign(static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height), 0);
    auto const sample = [&](int sx, int sy) -> float {
        if (sx < 0 || sy < 0 || sx >= sw || sy >= sh)
            return 0;
        return alpha[static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(sx)];
    };
    for (int py = top; py < bottom; ++py) {
        for (int px = left; px < right; ++px) {
            svg::Point const q = inverse->apply(px + 0.5, py + 0.5);
            double const u = (static_cast<double>(q.x) - box_left) * scale - 0.5;
            double const w = (static_cast<double>(q.y) - box_top) * scale - 0.5;
            double const fu = std::floor(u);
            double const fw = std::floor(w);
            int const x0 = static_cast<int>(fu);
            int const y0 = static_cast<int>(fw);
            auto const ax = static_cast<float>(u - fu);
            auto const ay = static_cast<float>(w - fw);
            float const top_row = sample(x0, y0) + (sample(x0 + 1, y0) - sample(x0, y0)) * ax;
            float const bottom_row = sample(x0, y0 + 1) + (sample(x0 + 1, y0 + 1) - sample(x0, y0 + 1)) * ax;
            float const value = top_row + (bottom_row - top_row) * ay;
            mask.alpha[static_cast<std::size_t>(py - top) * static_cast<std::size_t>(mask.width) + static_cast<std::size_t>(px - left)]
                = static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
        }
    }
    return mask;
}

}
