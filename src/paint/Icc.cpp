#include "paint/Icc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace sashfold::icc {

namespace {

constexpr int k_curve_steps = 4096; // a fast path's tone-curve table, linearly interpolated
constexpr std::size_t k_max_clut_nodes = std::size_t { 1 } << 24;
constexpr int k_max_channels = 15; // ICC.1 §10.10: at most 15 channels on either side
// The perceptual reference medium's black in the D50 PCS (ICC.1 §6.2.4.3).
constexpr Vec3 k_perceptual_black { 0.00336, 0.0034731, 0.00287 };

double clamp01(double v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v; // NaN goes to 1 here; inputs are checked finite where they enter
}

constexpr std::uint32_t sig(char const (&s)[5])
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24
        | static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16
        | static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8 | static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

// Big-endian reads that stay inside the bytes: a read past the end gives
// zero and marks the whole read as failed.
class Bytes {
public:
    explicit Bytes(std::span<std::uint8_t const> data)
        : m_data(data)
    {
    }
    std::size_t size() const { return m_data.size(); }
    bool has(std::size_t at, std::size_t n) const { return at <= m_data.size() && n <= m_data.size() - at; }
    bool bad() const { return m_bad; }
    std::uint8_t u8(std::size_t at) const
    {
        if (!has(at, 1)) {
            m_bad = true;
            return 0;
        }
        return m_data[at];
    }
    std::uint16_t u16(std::size_t at) const
    {
        if (!has(at, 2)) {
            m_bad = true;
            return 0;
        }
        return static_cast<std::uint16_t>(m_data[at] << 8 | m_data[at + 1]);
    }
    std::uint32_t u32(std::size_t at) const
    {
        if (!has(at, 4)) {
            m_bad = true;
            return 0;
        }
        return static_cast<std::uint32_t>(m_data[at]) << 24 | static_cast<std::uint32_t>(m_data[at + 1]) << 16
            | static_cast<std::uint32_t>(m_data[at + 2]) << 8 | static_cast<std::uint32_t>(m_data[at + 3]);
    }
    // s15Fixed16Number (ICC.1 §4.6).
    double s15(std::size_t at) const { return static_cast<double>(static_cast<std::int32_t>(u32(at))) / 65536.0; }
    Bytes sub(std::size_t at, std::size_t n) const
    {
        if (!has(at, n)) {
            m_bad = true;
            return Bytes({});
        }
        return Bytes(m_data.subspan(at, n));
    }

private:
    std::span<std::uint8_t const> m_data;
    mutable bool m_bad = false;
};

// How a table's PCS values are encoded (ICC.1 §6.3.4, Annex A): lut16Type
// keeps the version 2 ("legacy") 16-bit CIELAB encoding, the others the
// version 4 one. PCSXYZ is 1.0 at 0x8000 in both.
enum class Encoding : std::uint8_t { V4, Legacy16 };

Vec3 decode_pcs(double const* v, bool lab, Encoding encoding)
{
    if (!lab)
        return { v[0] * 65535.0 / 32768.0, v[1] * 65535.0 / 32768.0, v[2] * 65535.0 / 32768.0 };
    if (encoding == Encoding::Legacy16)
        return { v[0] * 65535.0 / 65280.0 * 100.0, v[1] * 65535.0 / 256.0 - 128.0, v[2] * 65535.0 / 256.0 - 128.0 };
    return { v[0] * 100.0, v[1] * 255.0 - 128.0, v[2] * 255.0 - 128.0 };
}

void encode_lab(Vec3 const& lab, Encoding encoding, double* out)
{
    if (encoding == Encoding::Legacy16) {
        out[0] = lab[0] / 100.0 * 65280.0 / 65535.0;
        out[1] = (lab[1] + 128.0) * 256.0 / 65535.0;
        out[2] = (lab[2] + 128.0) * 256.0 / 65535.0;
    } else {
        out[0] = lab[0] / 100.0;
        out[1] = (lab[1] + 128.0) / 255.0;
        out[2] = (lab[2] + 128.0) / 255.0;
    }
    for (int i = 0; i < 3; ++i)
        out[i] = clamp01(out[i]);
}

double lab_f(double t)
{
    constexpr double e = 6.0 / 29.0;
    return t > e * e * e ? std::cbrt(t) : t / (3 * e * e) + 4.0 / 29.0;
}

double lab_f_inverse(double t)
{
    constexpr double e = 6.0 / 29.0;
    return t > e ? t * t * t : 3 * e * e * (t - 4.0 / 29.0);
}

// The sRGB primaries (ITU-R BT.709) and D65 white to D50 XYZ, by Bradford.
struct SrgbMatrices {
    Mat3 to_pcs {};
    Mat3 from_pcs {};
};

SrgbMatrices const& srgb_matrices()
{
    static SrgbMatrices const matrices = [] {
        double const xs[3] = { 0.64, 0.30, 0.15 };
        double const ys[3] = { 0.33, 0.60, 0.06 };
        Mat3 primaries {};
        for (int c = 0; c < 3; ++c) {
            primaries[static_cast<std::size_t>(c)] = xs[c] / ys[c];
            primaries[static_cast<std::size_t>(3 + c)] = 1;
            primaries[static_cast<std::size_t>(6 + c)] = (1 - xs[c] - ys[c]) / ys[c];
        }
        Vec3 const s = transform_by(*invert(primaries), k_d65);
        Mat3 to_d65 = primaries;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c)
                to_d65[static_cast<std::size_t>(r * 3 + c)] *= s[static_cast<std::size_t>(c)];
        }
        SrgbMatrices m;
        m.to_pcs = multiply(bradford(k_d65, k_d50), to_d65);
        m.from_pcs = *invert(m.to_pcs);
        return m;
    }();
    return matrices;
}

// Linear light in [0, 1] to 8-bit sRGB, by 65536 steps (under a twentieth
// of an 8-bit step apart at the steepest part of the curve).
std::array<std::uint8_t, 65536> const& encode_table()
{
    static std::array<std::uint8_t, 65536> const table = [] {
        std::array<std::uint8_t, 65536> t {};
        for (std::size_t i = 0; i < t.size(); ++i)
            t[i] = static_cast<std::uint8_t>(std::lround(srgb_encode(static_cast<double>(i) / 65535.0) * 255.0));
        return t;
    }();
    return table;
}

std::uint8_t to_byte(double v)
{
    return static_cast<std::uint8_t>(std::lround(clamp01(v) * 255.0));
}

// Interpolation in a grid of `outputs` values per node, the first input
// varying slowest: tetrahedral across the last three inputs, linear across
// any before them (and linear or bilinear for one or two inputs), so a
// four-input table is two tetrahedra mixed along its first input.
template<typename T>
struct GridView {
    T const* data = nullptr;
    int inputs = 0;
    int outputs = 0;
    int const* points = nullptr;
    std::size_t const* strides = nullptr;
};

template<typename T>
void tetrahedral(GridView<T> const& g, std::size_t base, std::array<std::size_t, 3> const& step, std::array<double, 3> const& r,
    double* out)
{
    T const* p = g.data + base;
    std::size_t const sx = step[0];
    std::size_t const sy = step[1];
    std::size_t const sz = step[2];
    double const rx = r[0];
    double const ry = r[1];
    double const rz = r[2];
    for (int o = 0; o < g.outputs; ++o) {
        auto const at = [&](std::size_t offset) { return static_cast<double>(p[offset + static_cast<std::size_t>(o)]); };
        double const c000 = at(0);
        double const c111 = at(sx + sy + sz);
        double v = 0;
        if (rx >= ry && ry >= rz) {
            double const c100 = at(sx);
            double const c110 = at(sx + sy);
            v = c000 + rx * (c100 - c000) + ry * (c110 - c100) + rz * (c111 - c110);
        } else if (rx >= rz && rz >= ry) {
            double const c100 = at(sx);
            double const c101 = at(sx + sz);
            v = c000 + rx * (c100 - c000) + rz * (c101 - c100) + ry * (c111 - c101);
        } else if (rz >= rx && rx >= ry) {
            double const c001 = at(sz);
            double const c101 = at(sx + sz);
            v = c000 + rz * (c001 - c000) + rx * (c101 - c001) + ry * (c111 - c101);
        } else if (ry >= rx && rx >= rz) {
            double const c010 = at(sy);
            double const c110 = at(sx + sy);
            v = c000 + ry * (c010 - c000) + rx * (c110 - c010) + rz * (c111 - c110);
        } else if (ry >= rz && rz >= rx) {
            double const c010 = at(sy);
            double const c011 = at(sy + sz);
            v = c000 + ry * (c010 - c000) + rz * (c011 - c010) + rx * (c111 - c011);
        } else {
            double const c001 = at(sz);
            double const c011 = at(sy + sz);
            v = c000 + rz * (c001 - c000) + ry * (c011 - c001) + rx * (c111 - c011);
        }
        out[o] = v;
    }
}

template<typename T>
void interpolate_from(GridView<T> const& g, int dim, std::size_t base, int const* cells, double const* fracs, double* out)
{
    int const left = g.inputs - dim;
    std::size_t const s0 = g.strides[dim];
    if (left == 3) {
        std::size_t const at = base + static_cast<std::size_t>(cells[dim]) * s0
            + static_cast<std::size_t>(cells[dim + 1]) * g.strides[dim + 1] + static_cast<std::size_t>(cells[dim + 2]) * g.strides[dim + 2];
        tetrahedral(g, at, { s0, g.strides[dim + 1], g.strides[dim + 2] }, { fracs[dim], fracs[dim + 1], fracs[dim + 2] }, out);
        return;
    }
    std::size_t const at = base + static_cast<std::size_t>(cells[dim]) * s0;
    if (left == 1) {
        double const f = fracs[dim];
        for (int o = 0; o < g.outputs; ++o) {
            double const a = static_cast<double>(g.data[at + static_cast<std::size_t>(o)]);
            double const b = static_cast<double>(g.data[at + s0 + static_cast<std::size_t>(o)]);
            out[o] = a + f * (b - a);
        }
        return;
    }
    double low[k_max_channels];
    interpolate_from(g, dim + 1, at, cells, fracs, low);
    double const f = fracs[dim];
    if (f == 0) {
        std::copy(low, low + g.outputs, out);
        return;
    }
    double high[k_max_channels];
    interpolate_from(g, dim + 1, at + s0, cells, fracs, high);
    for (int o = 0; o < g.outputs; ++o)
        out[o] = low[o] + f * (high[o] - low[o]);
}

template<typename T>
void interpolate(GridView<T> const& g, double const* in, double* out)
{
    int cells[16] {};
    double fracs[16] {};
    for (int i = 0; i < g.inputs; ++i) {
        int const n = g.points[i];
        double const p = clamp01(in[i]) * (n - 1);
        int const cell = std::min(static_cast<int>(p), n - 2);
        cells[i] = cell;
        fracs[i] = p - cell;
    }
    interpolate_from(g, 0, 0, cells, fracs, out);
}

double finite_or_zero(double v)
{
    return std::isfinite(v) ? v : 0;
}

} // namespace

// --- Matrices and color math ---------------------------------------------

Mat3 multiply(Mat3 const& a, Mat3 const& b)
{
    Mat3 m {};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c)
            m[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
    return m;
}

Vec3 transform_by(Mat3 const& m, Vec3 const& v)
{
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

std::optional<Mat3> invert(Mat3 const& m)
{
    double const c0 = m[4] * m[8] - m[5] * m[7];
    double const c1 = m[5] * m[6] - m[3] * m[8];
    double const c2 = m[3] * m[7] - m[4] * m[6];
    double const det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        return std::nullopt;
    double const k = 1 / det;
    return Mat3 { c0 * k, (m[2] * m[7] - m[1] * m[8]) * k, (m[1] * m[5] - m[2] * m[4]) * k, c1 * k, (m[0] * m[8] - m[2] * m[6]) * k,
        (m[2] * m[3] - m[0] * m[5]) * k, c2 * k, (m[1] * m[6] - m[0] * m[7]) * k, (m[0] * m[4] - m[1] * m[3]) * k };
}

Mat3 bradford(Vec3 const& from_white, Vec3 const& to_white)
{
    static constexpr Mat3 cone { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
    static Mat3 const cone_inverse = *invert(cone);
    Vec3 const from = transform_by(cone, from_white);
    Vec3 const to = transform_by(cone, to_white);
    Mat3 scale {};
    for (std::size_t i = 0; i < 3; ++i)
        scale[i * 4] = from[i] != 0 ? to[i] / from[i] : 1;
    return multiply(cone_inverse, multiply(scale, cone));
}

Vec3 lab_to_xyz(Vec3 const& lab, Vec3 const& white)
{
    double const fy = (lab[0] + 16) / 116;
    double const fx = fy + lab[1] / 500;
    double const fz = fy - lab[2] / 200;
    return { white[0] * lab_f_inverse(fx), white[1] * lab_f_inverse(fy), white[2] * lab_f_inverse(fz) };
}

Vec3 xyz_to_lab(Vec3 const& xyz, Vec3 const& white)
{
    double const fx = lab_f(xyz[0] / white[0]);
    double const fy = lab_f(xyz[1] / white[1]);
    double const fz = lab_f(xyz[2] / white[2]);
    return { 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz) };
}

Vec3 pcs_to_linear_srgb(Vec3 const& xyz)
{
    return transform_by(srgb_matrices().from_pcs, xyz);
}

// IEC 61966-2-1 §5.2: the curve is linear below 0.04045 encoded (0.0031308 linear).
double srgb_encode(double linear)
{
    double const v = clamp01(linear);
    return v <= 0.04045 / 12.92 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
}

double srgb_decode(double encoded)
{
    double const v = clamp01(encoded);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// --- Curves ----------------------------------------------------------------

Curve Curve::gamma(double g)
{
    Curve c;
    c.kind = Kind::Gamma;
    c.params[0] = g;
    return c;
}

double Curve::eval(double x_in) const
{
    double const x = finite_or_zero(x_in);
    switch (kind) {
    case Kind::Identity:
        return x;
    case Kind::Gamma:
        return x <= 0 ? 0 : std::pow(x, params[0]);
    case Kind::Table: {
        if (table.empty())
            return x;
        if (table.size() == 1)
            return table[0];
        double const p = clamp01(x) * static_cast<double>(table.size() - 1);
        std::size_t const i = std::min(static_cast<std::size_t>(p), table.size() - 2);
        double const f = p - static_cast<double>(i);
        return table[i] + f * (table[i + 1] - table[i]);
    }
    case Kind::Parametric:
        break;
    }
    // ICC.1 §10.18, table 68.
    double const g = params[0];
    double const a = params[1];
    double const b = params[2];
    double const c = params[3];
    double const d = params[4];
    double const e = params[5];
    double const f = params[6];
    auto const power = [g](double base) { return base <= 0 ? 0.0 : std::pow(base, g); };
    switch (function) {
    case 0:
        return power(x);
    case 1:
        if (a == 0)
            return 0;
        return x >= -b / a ? power(a * x + b) : 0;
    case 2:
        if (a == 0)
            return c;
        return x >= -b / a ? power(a * x + b) + c : c;
    case 3:
        return x >= d ? power(a * x + b) : c * x;
    case 4:
        return x >= d ? power(a * x + b) + e : c * x + f;
    default:
        return x;
    }
}

namespace {

// A curveType or parametricCurveType element at the start of `b`; `used`
// is its length in bytes (unpadded).
std::optional<Curve> parse_curve(Bytes const& b, std::size_t& used)
{
    std::uint32_t const type = b.u32(0);
    Curve curve;
    if (type == sig("curv")) {
        std::uint32_t const count = b.u32(8);
        if (b.bad() || count > (b.size() - std::min<std::size_t>(b.size(), 12)) / 2)
            return std::nullopt;
        used = 12 + std::size_t { count } * 2;
        if (count == 0)
            return curve; // identity
        if (count == 1) {
            curve.kind = Curve::Kind::Gamma;
            curve.params[0] = b.u16(12) / 256.0; // u8Fixed8Number
            return curve;
        }
        curve.kind = Curve::Kind::Table;
        curve.table.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
            curve.table[i] = b.u16(12 + std::size_t { i } * 2) / 65535.0;
        return b.bad() ? std::nullopt : std::optional<Curve>(std::move(curve));
    }
    if (type == sig("para")) {
        static constexpr int k_counts[5] = { 1, 3, 4, 5, 7 };
        int const function = b.u16(8);
        if (function > 4)
            return std::nullopt;
        int const n = k_counts[function];
        curve.kind = Curve::Kind::Parametric;
        curve.function = function;
        for (int i = 0; i < n; ++i)
            curve.params[static_cast<std::size_t>(i)] = b.s15(12 + static_cast<std::size_t>(i) * 4);
        used = 12 + static_cast<std::size_t>(n) * 4;
        return b.bad() ? std::nullopt : std::optional<Curve>(std::move(curve));
    }
    return std::nullopt;
}

// `count` curves one after another from `offset`, each padded to four bytes.
bool parse_curves(Bytes const& tag, std::size_t offset, int count, std::vector<Curve>& out)
{
    for (int i = 0; i < count; ++i) {
        if (!tag.has(offset, 12))
            return false;
        Bytes const rest = tag.sub(offset, tag.size() - offset);
        std::size_t used = 0;
        std::optional<Curve> curve = parse_curve(rest, used);
        if (!curve)
            return false;
        out.push_back(std::move(*curve));
        offset += (used + 3) & ~std::size_t { 3 };
    }
    return true;
}

} // namespace

// --- Lookup tables -----------------------------------------------------------

struct Profile::Clut {
    int inputs = 0;
    int outputs = 0;
    std::array<int, 16> points {};
    std::array<std::size_t, 16> strides {};
    std::vector<float> data; // normalized to [0, 1]

    // Sizes the grid; false when it is degenerate or too large.
    bool shape(std::size_t& nodes)
    {
        nodes = 1;
        for (int i = 0; i < inputs; ++i) {
            if (points[static_cast<std::size_t>(i)] < 2)
                return false;
            nodes *= static_cast<std::size_t>(points[static_cast<std::size_t>(i)]);
            if (nodes > k_max_clut_nodes)
                return false;
        }
        std::size_t stride = static_cast<std::size_t>(outputs);
        for (int i = inputs - 1; i >= 0; --i) {
            strides[static_cast<std::size_t>(i)] = stride;
            stride *= static_cast<std::size_t>(points[static_cast<std::size_t>(i)]);
        }
        return true;
    }
    bool read(Bytes const& b, std::size_t offset, int precision)
    {
        std::size_t nodes = 0;
        if (!shape(nodes))
            return false;
        std::size_t const count = nodes * static_cast<std::size_t>(outputs);
        if (!b.has(offset, count * static_cast<std::size_t>(precision)))
            return false;
        data.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            data[i] = precision == 1 ? static_cast<float>(b.u8(offset + i) / 255.0) : static_cast<float>(b.u16(offset + i * 2) / 65535.0);
        return !b.bad();
    }
    void eval(double const* in, double* out) const
    {
        GridView<float> const view { data.data(), inputs, outputs, points.data(), strides.data() };
        interpolate(view, in, out);
    }
};

struct Profile::Lut {
    enum class Order : std::uint8_t { Lut8, Lut16, AToB, BToA };
    Order order = Order::Lut16;
    int inputs = 0;
    int outputs = 0;
    // lut8Type, lut16Type: `a` the input tables, `b` the output tables.
    // lutAToBType: A, CLUT, M, matrix, B in that order; lutBToAType the reverse.
    std::vector<Curve> a;
    std::vector<Curve> m;
    std::vector<Curve> b;
    bool has_matrix = false;
    std::array<double, 12> matrix {};
    std::optional<Clut> clut;

    // Every curve sampled at k_curve_steps + 1 points over [0, 1], for the
    // fast evaluation: one table after another, a, m, b.
    std::vector<float> sampled;

    Encoding encoding() const { return order == Order::Lut16 ? Encoding::Legacy16 : Encoding::V4; }

    void sample()
    {
        std::size_t const steps = k_curve_steps + 1;
        sampled.resize((a.size() + m.size() + b.size()) * steps);
        std::size_t at = 0;
        for (auto const* list : { &a, &m, &b }) {
            for (Curve const& curve : *list) {
                for (std::size_t i = 0; i < steps; ++i)
                    sampled[at + i] = static_cast<float>(curve.eval(static_cast<double>(i) / k_curve_steps));
                at += steps;
            }
        }
    }

    // `fast`: the curves from their samples (within a few millionths of
    // the curves, linearly interpolated).
    void eval(double const* in, double* out, bool fast = false) const
    {
        double v[16] {};
        double w[16] {};
        for (int i = 0; i < inputs; ++i)
            v[i] = in[i];
        std::size_t const steps = k_curve_steps + 1;
        auto const curves = [&](std::vector<Curve> const& list, double* values) {
            if (!fast || sampled.empty()) {
                for (std::size_t i = 0; i < list.size(); ++i)
                    values[i] = list[i].eval(values[i]);
                return;
            }
            std::size_t const first = &list == &a ? 0 : &list == &m ? a.size() : a.size() + m.size();
            for (std::size_t i = 0; i < list.size(); ++i) {
                // Past [0, 1] (after a lutAToBType matrix) only the curve itself knows.
                if (list[i].kind == Curve::Kind::Identity || !(values[i] >= 0 && values[i] <= 1)) {
                    values[i] = list[i].eval(values[i]);
                    continue;
                }
                float const* table = sampled.data() + (first + i) * steps;
                double const p = values[i] * k_curve_steps;
                std::size_t const cell = std::min(static_cast<std::size_t>(p), static_cast<std::size_t>(k_curve_steps - 1));
                double const f = p - static_cast<double>(cell);
                double const low = static_cast<double>(table[cell]);
                values[i] = low + f * (static_cast<double>(table[cell + 1]) - low);
            }
        };
        auto const run_matrix = [this](double* values) {
            double const x = values[0];
            double const y = values[1];
            double const z = values[2];
            for (std::size_t r = 0; r < 3; ++r)
                values[r] = matrix[r * 3] * x + matrix[r * 3 + 1] * y + matrix[r * 3 + 2] * z + matrix[9 + r];
        };
        auto const run_clut = [&](double* values) {
            if (!clut)
                return;
            for (int i = 0; i < clut->inputs; ++i)
                values[i] = clamp01(values[i]);
            clut->eval(values, w);
            std::copy(w, w + clut->outputs, values);
        };
        switch (order) {
        case Order::Lut8:
        case Order::Lut16:
            for (int i = 0; i < inputs; ++i)
                v[i] = clamp01(v[i]);
            // The matrix of a table whose input is PCSXYZ (ICC.1 §10.10).
            if (has_matrix) {
                run_matrix(v);
                for (int i = 0; i < 3; ++i)
                    v[i] = clamp01(v[i]);
            }
            curves(a, v);
            run_clut(v);
            for (int i = 0; i < outputs; ++i)
                v[i] = clamp01(v[i]);
            curves(b, v);
            break;
        case Order::AToB:
            for (int i = 0; i < inputs; ++i)
                v[i] = clamp01(v[i]);
            curves(a, v);
            run_clut(v);
            curves(m, v);
            if (has_matrix)
                run_matrix(v);
            curves(b, v);
            break;
        case Order::BToA:
            for (int i = 0; i < inputs; ++i)
                v[i] = clamp01(v[i]);
            curves(b, v);
            if (has_matrix)
                run_matrix(v);
            curves(m, v);
            run_clut(v);
            curves(a, v);
            break;
        }
        // A device side is clipped to its range; a lutAToBType's PCS side
        // may run past it (a high-dynamic-range display's white is far
        // above the PCS white) and is clipped only in sRGB.
        for (int i = 0; i < outputs; ++i)
            out[i] = order == Order::AToB ? finite_or_zero(v[i]) : clamp01(v[i]);
    }
};

// A table of three or four inputs and a CLUT, laid out for the pixel loop:
// the same steps as Lut::eval with sampled curves, in floats, unrolled.
struct CompiledTable {
    int inputs = 0;
    bool lut_order = true; // lut8Type/lut16Type: a, CLUT, b; else lutAToBType: a, CLUT, m, matrix, b
    std::array<float const*, 4> a {}; // sampled curves; null for the identity
    std::array<float const*, 3> m {};
    std::array<float const*, 3> b {};
    std::array<Curve const*, 3> m_curves {};
    std::array<Curve const*, 3> b_curves {};
    bool has_matrix = false;
    std::array<float, 12> matrix {};
    float const* grid = nullptr;
    std::array<int, 4> points {};
    std::array<std::size_t, 4> strides {};
    bool lab = false;
    std::array<float, 3> decode_scale {};
    std::array<float, 3> decode_offset {};
    std::array<float, 12> to_linear {};

    static float sampled(float const* table, float x)
    {
        float const p = x * static_cast<float>(k_curve_steps);
        std::size_t const cell = std::min(static_cast<std::size_t>(p), static_cast<std::size_t>(k_curve_steps - 1));
        float const f = p - static_cast<float>(cell);
        return table[cell] + f * (table[cell + 1] - table[cell]);
    }
    static float clamp01f(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; } // NaN goes to 0

    // Curves after the CLUT or the matrix: a value past [0, 1] is left to the curve itself.
    static void curves(std::array<float const*, 3> const& tables, std::array<Curve const*, 3> const& exact, float* v)
    {
        for (std::size_t i = 0; i < 3; ++i) {
            if (!tables[i])
                continue;
            if (v[i] >= 0 && v[i] <= 1)
                v[i] = sampled(tables[i], v[i]);
            else
                v[i] = static_cast<float>(exact[i]->eval(static_cast<double>(v[i])));
        }
    }

    void tetrahedron(std::size_t base, std::size_t sx, std::size_t sy, std::size_t sz, float rx, float ry, float rz, float* out) const
    {
        float const* p = grid + base;
        float const* c111 = p + sx + sy + sz;
        float const *first = nullptr, *second = nullptr;
        float w1 = 0, w2 = 0, w3 = 0;
        // The tetrahedron the point is in: c000, then the corners along
        // the largest fraction, the next, the last (as in Clut::eval).
        if (rx >= ry && ry >= rz) {
            first = p + sx, second = p + sx + sy, w1 = rx, w2 = ry, w3 = rz;
        } else if (rx >= rz && rz >= ry) {
            first = p + sx, second = p + sx + sz, w1 = rx, w2 = rz, w3 = ry;
        } else if (rz >= rx && rx >= ry) {
            first = p + sz, second = p + sx + sz, w1 = rz, w2 = rx, w3 = ry;
        } else if (ry >= rx && rx >= rz) {
            first = p + sy, second = p + sx + sy, w1 = ry, w2 = rx, w3 = rz;
        } else if (ry >= rz && rz >= rx) {
            first = p + sy, second = p + sy + sz, w1 = ry, w2 = rz, w3 = rx;
        } else {
            first = p + sz, second = p + sy + sz, w1 = rz, w2 = ry, w3 = rx;
        }
        for (int o = 0; o < 3; ++o)
            out[o] = p[o] + w1 * (first[o] - p[o]) + w2 * (second[o] - first[o]) + w3 * (c111[o] - second[o]);
    }

    void run(float const* in, float* linear) const
    {
        int cell[4];
        float frac[4];
        for (int i = 0; i < inputs; ++i) {
            float x = clamp01f(in[i]);
            if (a[static_cast<std::size_t>(i)])
                x = clamp01f(sampled(a[static_cast<std::size_t>(i)], x));
            int const n = points[static_cast<std::size_t>(i)];
            float const p = x * static_cast<float>(n - 1);
            cell[i] = std::min(static_cast<int>(p), n - 2);
            frac[i] = p - static_cast<float>(cell[i]);
        }
        float out[3];
        int const d = inputs - 3; // the inputs before the tetrahedral three
        std::size_t base = 0;
        for (int i = 0; i < inputs; ++i)
            base += static_cast<std::size_t>(cell[i]) * strides[static_cast<std::size_t>(i)];
        std::size_t const sx = strides[static_cast<std::size_t>(d)];
        std::size_t const sy = strides[static_cast<std::size_t>(d + 1)];
        std::size_t const sz = strides[static_cast<std::size_t>(d + 2)];
        tetrahedron(base, sx, sy, sz, frac[d], frac[d + 1], frac[d + 2], out);
        if (d == 1 && frac[0] > 0) {
            float high[3];
            tetrahedron(base + strides[0], sx, sy, sz, frac[1], frac[2], frac[3], high);
            for (int o = 0; o < 3; ++o)
                out[o] += frac[0] * (high[o] - out[o]);
        }
        if (lut_order) {
            for (std::size_t o = 0; o < 3; ++o) {
                out[o] = clamp01f(out[o]);
                if (b[o])
                    out[o] = sampled(b[o], out[o]);
            }
        } else {
            curves(m, m_curves, out);
            if (has_matrix) {
                float const x = out[0];
                float const y = out[1];
                float const z = out[2];
                for (std::size_t r = 0; r < 3; ++r)
                    out[r] = matrix[r * 3] * x + matrix[r * 3 + 1] * y + matrix[r * 3 + 2] * z + matrix[9 + r];
            }
            curves(b, b_curves, out);
        }
        float pcs[3];
        for (std::size_t o = 0; o < 3; ++o)
            pcs[o] = out[o] * decode_scale[o] + decode_offset[o];
        float xyz[3];
        if (lab) {
            constexpr float e = 6.0f / 29.0f;
            auto const inverse = [](float t) { return t > e ? t * t * t : 3 * e * e * (t - 4.0f / 29.0f); };
            float const fy = (pcs[0] + 16) / 116;
            xyz[0] = static_cast<float>(k_d50[0]) * inverse(fy + pcs[1] / 500);
            xyz[1] = inverse(fy);
            xyz[2] = static_cast<float>(k_d50[2]) * inverse(fy - pcs[2] / 200);
        } else {
            xyz[0] = pcs[0];
            xyz[1] = pcs[1];
            xyz[2] = pcs[2];
        }
        for (std::size_t r = 0; r < 3; ++r)
            linear[r] = to_linear[r * 3] * xyz[0] + to_linear[r * 3 + 1] * xyz[1] + to_linear[r * 3 + 2] * xyz[2] + to_linear[9 + r];
    }
};

namespace {

std::shared_ptr<CompiledTable const> compile(Profile::Lut const& lut, bool pcs_lab, std::array<double, 12> const& to_linear)
{
    if (!lut.clut || (lut.inputs != 3 && lut.inputs != 4) || lut.outputs != 3 || lut.sampled.empty() || lut.order == Profile::Lut::Order::BToA)
        return nullptr;
    auto c = std::make_shared<CompiledTable>();
    c->inputs = lut.inputs;
    c->lut_order = lut.order == Profile::Lut::Order::Lut8 || lut.order == Profile::Lut::Order::Lut16;
    std::size_t const steps = k_curve_steps + 1;
    std::size_t at = 0;
    auto const take = [&](std::vector<Curve> const& list, auto& tables, std::array<Curve const*, 3>* exact) {
        for (std::size_t i = 0; i < list.size() && i < tables.size(); ++i) {
            if (list[i].kind != Curve::Kind::Identity)
                tables[i] = lut.sampled.data() + (at + i) * steps;
            if (exact)
                (*exact)[i] = &list[i];
        }
        at += list.size();
    };
    take(lut.a, c->a, nullptr);
    take(lut.m, c->m, &c->m_curves);
    take(lut.b, c->b, &c->b_curves);
    c->has_matrix = lut.has_matrix;
    for (std::size_t i = 0; i < 12; ++i)
        c->matrix[i] = static_cast<float>(lut.matrix[i]);
    c->grid = lut.clut->data.data();
    for (std::size_t i = 0; i < 4; ++i) {
        c->points[i] = lut.clut->points[i];
        c->strides[i] = lut.clut->strides[i];
    }
    c->lab = pcs_lab;
    // decode_pcs, as a scale and an offset per channel.
    double const zero[3] = { 0, 0, 0 };
    double const one[3] = { 1, 1, 1 };
    Vec3 const low = decode_pcs(zero, pcs_lab, lut.encoding());
    Vec3 const high = decode_pcs(one, pcs_lab, lut.encoding());
    for (std::size_t i = 0; i < 3; ++i) {
        c->decode_offset[i] = static_cast<float>(low[i]);
        c->decode_scale[i] = static_cast<float>(high[i] - low[i]);
    }
    for (std::size_t i = 0; i < 12; ++i)
        c->to_linear[i] = static_cast<float>(to_linear[i]);
    return c;
}

// `xyz_input`: the table's input is PCSXYZ (a B2A table on the XYZ PCS),
// so a lut8Type or lut16Type matrix applies.
std::unique_ptr<Profile::Lut> parse_lut(Bytes const& tag, bool b_to_a, bool xyz_input)
{
    using Lut = Profile::Lut;
    using Clut = Profile::Clut;
    std::uint32_t const type = tag.u32(0);
    int const inputs = tag.u8(8);
    int const outputs = tag.u8(9);
    if (tag.bad() || inputs < 1 || inputs > k_max_channels || outputs < 1 || outputs > k_max_channels)
        return nullptr;
    auto lut = std::make_unique<Lut>();
    lut->inputs = inputs;
    lut->outputs = outputs;
    if (type == sig("mft1") || type == sig("mft2")) {
        // ICC.1 §10.10 lut16Type, §10.11 lut8Type.
        bool const wide = type == sig("mft2");
        lut->order = wide ? Lut::Order::Lut16 : Lut::Order::Lut8;
        int const grid = tag.u8(10);
        std::size_t const in_entries = wide ? tag.u16(48) : 256;
        std::size_t const out_entries = wide ? tag.u16(50) : 256;
        std::size_t offset = wide ? 52 : 48;
        std::size_t const width = wide ? 2 : 1;
        if (tag.bad() || in_entries < 2 || in_entries > 4096 || out_entries < 2 || out_entries > 4096 || grid < 2)
            return nullptr;
        if (xyz_input && inputs == 3) {
            lut->has_matrix = true;
            for (std::size_t i = 0; i < 9; ++i)
                lut->matrix[i] = tag.s15(12 + i * 4);
        }
        auto const read_tables = [&](int count, std::size_t entries, std::vector<Curve>& list) {
            if (!tag.has(offset, static_cast<std::size_t>(count) * entries * width))
                return false;
            for (int c = 0; c < count; ++c) {
                Curve curve;
                curve.kind = Curve::Kind::Table;
                curve.table.resize(entries);
                for (std::size_t i = 0; i < entries; ++i)
                    curve.table[i] = wide ? tag.u16(offset + i * 2) / 65535.0 : tag.u8(offset + i) / 255.0;
                offset += entries * width;
                list.push_back(std::move(curve));
            }
            return true;
        };
        if (!read_tables(inputs, in_entries, lut->a))
            return nullptr;
        Clut clut;
        clut.inputs = inputs;
        clut.outputs = outputs;
        for (int i = 0; i < inputs; ++i)
            clut.points[static_cast<std::size_t>(i)] = grid;
        if (!clut.read(tag, offset, static_cast<int>(width)))
            return nullptr;
        offset += clut.data.size() * width;
        lut->clut = std::move(clut);
        if (!read_tables(outputs, out_entries, lut->b))
            return nullptr;
        return tag.bad() ? nullptr : std::move(lut);
    }
    if ((!b_to_a && type == sig("mAB ")) || (b_to_a && type == sig("mBA "))) {
        // ICC.1 §10.12 lutAToBType, §10.13 lutBToAType.
        lut->order = b_to_a ? Lut::Order::BToA : Lut::Order::AToB;
        std::size_t const b_at = tag.u32(12);
        std::size_t const matrix_at = tag.u32(16);
        std::size_t const m_at = tag.u32(20);
        std::size_t const clut_at = tag.u32(24);
        std::size_t const a_at = tag.u32(28);
        if (tag.bad() || b_at == 0)
            return nullptr;
        // The side the B, M curves and matrix sit on: the PCS side.
        int const pcs_side = b_to_a ? inputs : outputs;
        int const device_side = b_to_a ? outputs : inputs;
        if (!parse_curves(tag, b_at, pcs_side, lut->b))
            return nullptr;
        if (m_at != 0 || matrix_at != 0) {
            if (pcs_side != 3 || m_at == 0 || matrix_at == 0)
                return nullptr;
            if (!parse_curves(tag, m_at, 3, lut->m) || !tag.has(matrix_at, 48))
                return nullptr;
            lut->has_matrix = true;
            for (std::size_t i = 0; i < 12; ++i)
                lut->matrix[i] = tag.s15(matrix_at + i * 4);
        }
        if (clut_at != 0) {
            if (a_at == 0 || !tag.has(clut_at, 20))
                return nullptr;
            Clut clut;
            clut.inputs = inputs;
            clut.outputs = outputs;
            for (int i = 0; i < inputs; ++i)
                clut.points[static_cast<std::size_t>(i)] = tag.u8(clut_at + static_cast<std::size_t>(i));
            int const precision = tag.u8(clut_at + 16);
            if ((precision != 1 && precision != 2) || !clut.read(tag, clut_at + 20, precision))
                return nullptr;
            lut->clut = std::move(clut);
            if (!parse_curves(tag, a_at, device_side, lut->a))
                return nullptr;
        } else if (a_at != 0 || inputs != outputs) {
            return nullptr;
        }
        return tag.bad() ? nullptr : std::move(lut);
    }
    return nullptr;
}

std::optional<Vec3> parse_xyz(Bytes const& tag)
{
    if (tag.u32(0) != sig("XYZ ") || !tag.has(8, 12))
        return std::nullopt;
    Vec3 v { tag.s15(8), tag.s15(12), tag.s15(16) };
    return tag.bad() ? std::nullopt : std::optional<Vec3>(v);
}

int channels_of(DataSpace space)
{
    switch (space) {
    case DataSpace::Gray:
        return 1;
    case DataSpace::CMYK:
        return 4;
    case DataSpace::RGB:
    case DataSpace::Lab:
    case DataSpace::XYZ:
        return 3;
    case DataSpace::Other:
        break;
    }
    return 0;
}

} // namespace

// --- Profiles --------------------------------------------------------------

Profile::Profile() = default;
Profile::~Profile() = default;

int Profile::channels() const
{
    return channels_of(m_space);
}

std::shared_ptr<Profile const> Profile::parse(std::span<std::uint8_t const> bytes_in)
{
    // ICC.1 §7.2: the 128-byte header, then the tag count and table.
    if (bytes_in.size() < 132)
        return nullptr;
    Bytes const whole(bytes_in);
    std::size_t const declared = whole.u32(0);
    if (declared < 132)
        return nullptr;
    Bytes const b(bytes_in.subspan(0, std::min(declared, bytes_in.size())));
    if (b.u32(36) != sig("acsp"))
        return nullptr;
    auto profile = std::make_shared<Profile>();
    profile->m_version = b.u8(8);
    std::uint32_t const profile_class = b.u32(12);
    profile->m_class = profile_class == sig("scnr") ? ProfileClass::Input
        : profile_class == sig("mntr")              ? ProfileClass::Display
        : profile_class == sig("prtr")              ? ProfileClass::Output
        : profile_class == sig("spac")              ? ProfileClass::ColorSpace
        : profile_class == sig("abst")              ? ProfileClass::Abstract
        : profile_class == sig("link")              ? ProfileClass::Link
        : profile_class == sig("nmcl")              ? ProfileClass::NamedColor
                                                    : ProfileClass::Other;
    if (profile->m_class != ProfileClass::Input && profile->m_class != ProfileClass::Display && profile->m_class != ProfileClass::Output
        && profile->m_class != ProfileClass::ColorSpace)
        return nullptr;
    std::uint32_t const space = b.u32(16);
    profile->m_space = space == sig("GRAY") ? DataSpace::Gray
        : space == sig("RGB ")              ? DataSpace::RGB
        : space == sig("CMYK")              ? DataSpace::CMYK
        : space == sig("Lab ")              ? DataSpace::Lab
                                            : DataSpace::Other;
    if (profile->m_space == DataSpace::Other)
        return nullptr;
    std::uint32_t const pcs = b.u32(20);
    if (pcs != sig("XYZ ") && pcs != sig("Lab "))
        return nullptr;
    profile->m_pcs_lab = pcs == sig("Lab ");

    std::size_t const tag_count = b.u32(128);
    if (b.bad() || tag_count > (b.size() - 132) / 12)
        return nullptr;
    std::map<std::uint32_t, Bytes> tags;
    for (std::size_t i = 0; i < tag_count; ++i) {
        std::size_t const at = 132 + i * 12;
        std::uint32_t const name = b.u32(at);
        std::size_t const offset = b.u32(at + 4);
        std::size_t const size = b.u32(at + 8);
        if (offset < 128 || size < 8 || !b.has(offset, size))
            continue;
        tags.emplace(name, b.sub(offset, size));
    }
    auto const tag = [&](char const (&name)[5]) -> Bytes const* {
        auto const it = tags.find(sig(name));
        return it == tags.end() ? nullptr : &it->second;
    };

    if (Bytes const* t = tag("wtpt")) {
        if (auto white = parse_xyz(*t); white && (*white)[1] > 0)
            profile->m_white = *white;
    }
    if (Bytes const* t = tag("chad"); t && t->u32(0) == sig("sf32") && t->has(8, 36)) {
        Mat3 m {};
        for (std::size_t i = 0; i < 9; ++i)
            m[i] = t->s15(8 + i * 4);
        if (!t->bad() && invert(m))
            profile->m_chad = m;
    }

    int const channels = profile->channels();
    std::uint32_t const a2b_names[3] = { sig("A2B0"), sig("A2B1"), sig("A2B2") };
    for (std::size_t i = 0; i < 3; ++i) {
        auto const it = tags.find(a2b_names[i]);
        if (it == tags.end())
            continue;
        std::unique_ptr<Lut> lut = parse_lut(it->second, false, false);
        if (lut && lut->inputs == channels && lut->outputs == 3) {
            lut->sample();
            profile->m_a2b[i] = std::move(lut);
        }
    }
    if (Bytes const* t = tag("B2A0")) {
        std::unique_ptr<Lut> lut = parse_lut(*t, true, !profile->m_pcs_lab);
        if (lut && lut->inputs == 3 && lut->outputs == channels)
            profile->m_b2a0 = std::move(lut);
    }

    // The matrix and curves (ICC.1 §F.2, F.3).
    if (profile->m_space == DataSpace::RGB) {
        Bytes const* const columns[3] = { tag("rXYZ"), tag("gXYZ"), tag("bXYZ") };
        Bytes const* const curves[3] = { tag("rTRC"), tag("gTRC"), tag("bTRC") };
        bool ok = true;
        for (std::size_t c = 0; c < 3 && ok; ++c) {
            std::optional<Vec3> column = columns[c] ? parse_xyz(*columns[c]) : std::nullopt;
            std::size_t used = 0;
            std::optional<Curve> curve = curves[c] ? parse_curve(*curves[c], used) : std::nullopt;
            ok = column && curve;
            if (!ok)
                break;
            for (std::size_t r = 0; r < 3; ++r)
                profile->m_matrix[r * 3 + c] = (*column)[r];
            profile->m_trc.push_back(std::move(*curve));
        }
        profile->m_has_matrix = ok && invert(profile->m_matrix).has_value();
        if (!profile->m_has_matrix)
            profile->m_trc.clear();
    } else if (profile->m_space == DataSpace::Gray) {
        std::size_t used = 0;
        if (Bytes const* t = tag("kTRC")) {
            if (std::optional<Curve> curve = parse_curve(*t, used)) {
                profile->m_trc.push_back(std::move(*curve));
                profile->m_matrix = { k_d50[0], 0, 0, k_d50[1], 0, 0, k_d50[2], 0, 0 };
                profile->m_has_matrix = true;
            }
        }
    }
    if (!profile->m_has_matrix && !profile->m_a2b[0] && !profile->m_a2b[1] && !profile->m_a2b[2])
        return nullptr;
    return profile;
}

Profile::Lut const* Profile::table_for(Intent intent) const
{
    // ICC.1 §8.4–8.6, table 25: A2B0 perceptual, A2B1 colorimetric, A2B2
    // saturation; a missing one falls back to A2B0.
    std::size_t const index = intent == Intent::Perceptual ? 0 : intent == Intent::Saturation ? 2 : 1;
    if (m_a2b[index])
        return m_a2b[index].get();
    return m_a2b[0].get();
}

bool Profile::is_matrix_shaper() const
{
    return m_has_matrix;
}

bool Profile::pcs_value(Intent intent, double const* in, Vec3& value, bool& lab, bool fast) const
{
    int const n = channels();
    double device[16] {};
    for (int i = 0; i < n; ++i)
        device[i] = finite_or_zero(in[i]);
    lab = m_pcs_lab;
    Lut const* const lut = table_for(intent);
    if (lut) {
        double encoded[16] {};
        if (m_space == DataSpace::Lab) {
            encode_lab({ device[0], device[1], device[2] }, lut->encoding(), encoded);
        } else {
            for (int i = 0; i < n; ++i)
                encoded[i] = clamp01(device[i]);
        }
        double out[16] {};
        lut->eval(encoded, out, fast);
        value = decode_pcs(out, m_pcs_lab, lut->encoding());
        return true;
    }
    if (!m_has_matrix)
        return false;
    if (m_space == DataSpace::Gray) {
        double const y = m_trc[0].eval(clamp01(device[0]));
        // A gray profile on the Lab PCS connects to L* alone (ICC.1 §F.3).
        value = m_pcs_lab ? Vec3 { clamp01(y) * 100, 0, 0 } : Vec3 { k_d50[0] * y, k_d50[1] * y, k_d50[2] * y };
        return true;
    }
    Vec3 const linear { m_trc[0].eval(clamp01(device[0])), m_trc[1].eval(clamp01(device[1])), m_trc[2].eval(clamp01(device[2])) };
    value = transform_by(m_matrix, linear);
    lab = false; // the matrix gives XYZ, whatever the header's PCS
    return true;
}

Vec3 Profile::absolute_scale(Intent intent) const
{
    // ICC.1 Annex D: ICC-absolute = media-relative x media white / PCS
    // white. A version 2 display profile's media white is its own
    // display's white, which the PCS already maps to D50.
    if (intent != Intent::AbsoluteColorimetric || (m_version < 4 && m_class == ProfileClass::Display))
        return { 1, 1, 1 };
    return { m_white[0] / k_d50[0], m_white[1] / k_d50[1], m_white[2] / k_d50[2] };
}

bool Profile::to_pcs(Intent intent, double const* in, Vec3& xyz) const
{
    Vec3 value {};
    bool lab = false;
    if (!pcs_value(intent, in, value, lab))
        return false;
    xyz = lab ? lab_to_xyz(value, k_d50) : value;
    Vec3 const scale = absolute_scale(intent);
    for (std::size_t i = 0; i < 3; ++i)
        xyz[i] *= scale[i];
    return true;
}

bool Profile::darker_colorant(Intent intent, Vec3& black) const
{
    // ISO 18619 §6.2.2: the profile's black is its darkest colorant
    // through the intent, made neutral and no lighter than L* 50. An
    // intent the profile has no table of its own for (nor a matrix) has
    // no black point to compensate.
    std::size_t const own = intent == Intent::Perceptual ? 0 : intent == Intent::Saturation ? 2 : 1;
    if (!m_a2b[own] && !m_has_matrix) {
        black = { 0, 0, 0 };
        return true;
    }
    double device[4] {};
    if (m_space == DataSpace::CMYK)
        device[0] = device[1] = device[2] = device[3] = 1;
    Vec3 xyz {};
    if (!to_pcs(intent, device, xyz))
        return false;
    Vec3 lab = xyz_to_lab(xyz, k_d50);
    lab[0] = std::clamp(lab[0], 0.0, 50.0);
    lab[1] = lab[2] = 0;
    black = lab_to_xyz(lab, k_d50);
    return true;
}

bool Profile::perceptual_round_trip(Vec3& black) const
{
    // ISO 18619 §6.2.3: an output profile's black is the PCS black taken
    // to the device by its perceptual B2A0 and back by its colorimetric A2B.
    if (!m_b2a0)
        return false;
    // The PCS black: a version 4 perceptual table's is the perceptual
    // reference medium's (ICC.1 §6.2.4.3), a version 2 one's is zero.
    Vec3 const start = m_version >= 4 ? k_perceptual_black : Vec3 { 0, 0, 0 };
    double pcs[3] {};
    if (m_pcs_lab) {
        encode_lab(xyz_to_lab(start, k_d50), m_b2a0->encoding(), pcs);
    } else {
        for (std::size_t i = 0; i < 3; ++i)
            pcs[i] = clamp01(start[i] * 32768.0 / 65535.0);
    }
    double device[16] {};
    m_b2a0->eval(pcs, device);
    Vec3 xyz {};
    if (!to_pcs(Intent::RelativeColorimetric, device, xyz))
        return false;
    Vec3 lab = xyz_to_lab(xyz, k_d50);
    lab[0] = std::clamp(lab[0], 0.0, 50.0);
    lab[1] = lab[2] = 0;
    black = lab_to_xyz(lab, k_d50);
    return true;
}

Vec3 Profile::black_point(Intent intent) const
{
    Vec3 black { 0, 0, 0 };
    if (intent == Intent::AbsoluteColorimetric)
        return black;
    if (m_version >= 4 && (intent == Intent::Perceptual || intent == Intent::Saturation)) {
        // Version 4 perceptual and saturation tables are made against the
        // perceptual reference medium, whose black ICC.1 §6.2.4.3 fixes;
        // a matrix/TRC profile has no such table, so its colorimetric black.
        if (is_matrix_shaper()) {
            darker_colorant(Intent::RelativeColorimetric, black);
            return black;
        }
        return k_perceptual_black;
    }
    if (intent == Intent::RelativeColorimetric && m_class == ProfileClass::Output && m_space == DataSpace::CMYK) {
        if (!perceptual_round_trip(black))
            black = { 0, 0, 0 };
        return black;
    }
    if (!darker_colorant(intent, black))
        black = { 0, 0, 0 };
    return black;
}

std::shared_ptr<Transform const> Profile::transform(Intent intent) const
{
    std::size_t const index = static_cast<std::size_t>(intent) & 3;
    std::call_once(m_once[index], [&] { m_transforms[index] = build(intent); });
    if (!m_transforms[index])
        return nullptr;
    // Shares the profile's ownership: the transform reads the profile's tables.
    return std::shared_ptr<Transform const>(shared_from_this(), m_transforms[index].get());
}

std::unique_ptr<Transform> Profile::build(Intent intent) const
{
    auto t = std::make_unique<Transform>();
    t->m_profile = this;
    t->m_intent = intent;
    t->m_channels = channels();
    if (intent != Intent::AbsoluteColorimetric) {
        Vec3 const black = black_point(intent);
        t->m_black = BlackPoint::compensating(black);
    }
    Lut const* const lut = table_for(intent);
    if (!lut && !m_has_matrix)
        return nullptr;
    // From PCS XYZ to linear sRGB: the absolute scaling, the black point,
    // then the sRGB matrix, as one matrix and an offset.
    Vec3 const absolute = absolute_scale(intent);
    Mat3 finish {};
    for (std::size_t r = 0; r < 3; ++r)
        finish[r * 4] = absolute[r] * t->m_black.scale[r];
    finish = multiply(srgb_matrices().from_pcs, finish);
    Vec3 const offset = pcs_to_linear_srgb(t->m_black.offset);
    bool const shaper = !lut && !(m_space == DataSpace::Gray && m_pcs_lab);
    Mat3 const to_linear = shaper ? multiply(finish, m_matrix) : finish;
    for (std::size_t i = 0; i < 9; ++i)
        t->m_to_linear[i] = to_linear[i];
    for (std::size_t i = 0; i < 3; ++i)
        t->m_to_linear[9 + i] = offset[i];
    if (shaper) {
        t->m_kind = Transform::Kind::ShaperMatrix;
        t->m_curves = m_trc;
        t->finish_shaper();
        return t;
    }
    t->m_kind = Transform::Kind::Table;
    if (lut && m_space != DataSpace::Lab)
        t->m_compiled = compile(*lut, m_pcs_lab, t->m_to_linear);
    return t;
}

// --- Black-point compensation ------------------------------------------------

BlackPoint BlackPoint::compensating(Vec3 const& black)
{
    // ISO 18619 §6.3: per XYZ component, a line through (black, 0) and
    // (white, white) of the D50 PCS.
    BlackPoint bp;
    for (std::size_t i = 0; i < 3; ++i) {
        double const span = k_d50[i] - black[i];
        if (black[i] <= 0 || span <= 1e-6)
            continue;
        bp.scale[i] = k_d50[i] / span;
        bp.offset[i] = -k_d50[i] * black[i] / span;
    }
    return bp;
}

Vec3 BlackPoint::apply(Vec3 const& xyz) const
{
    return { xyz[0] * scale[0] + offset[0], xyz[1] * scale[1] + offset[1], xyz[2] * scale[2] + offset[2] };
}

// --- Transforms -------------------------------------------------------------

std::shared_ptr<Transform const> Transform::shaper_matrix(std::vector<Curve> curves, Mat3 const& to_pcs, Vec3 const& black)
{
    if (curves.size() != 1 && curves.size() != 3)
        return nullptr;
    auto t = std::make_shared<Transform>();
    t->m_kind = Kind::ShaperMatrix;
    t->m_channels = static_cast<int>(curves.size());
    t->m_curves = std::move(curves);
    t->m_black = BlackPoint::compensating(black);
    Mat3 m = to_pcs;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c)
            m[r * 3 + c] *= t->m_black.scale[r];
    }
    Mat3 const to_linear = multiply(srgb_matrices().from_pcs, m);
    Vec3 const offset = pcs_to_linear_srgb(t->m_black.offset);
    for (std::size_t i = 0; i < 9; ++i)
        t->m_to_linear[i] = to_linear[i];
    for (std::size_t i = 0; i < 3; ++i)
        t->m_to_linear[9 + i] = offset[i];
    t->finish_shaper();
    return t;
}

void Transform::finish_shaper()
{
    std::size_t const steps = k_curve_steps + 1;
    m_curve_tables.resize(m_curves.size() * steps);
    for (std::size_t c = 0; c < m_curves.size(); ++c) {
        for (std::size_t i = 0; i < steps; ++i)
            m_curve_tables[c * steps + i] = static_cast<float>(m_curves[c].eval(static_cast<double>(i) / k_curve_steps));
    }
    // An sRGB profile (by its numbers, whatever its name): the transform
    // moves no value, along the ramps of each primary and of gray and over
    // a grid of the cube, by more than one 8-bit step, so it is skipped.
    // (The sRGB profiles in circulation differ in the last digits of their
    // primaries, which the steep foot of the sRGB curve turns into up to
    // half a step for the common version 2 one.)
    constexpr double k_identity_tolerance = 1.0 / 255;
    double worst = 0;
    auto const probe = [&](double const* in) {
        double rgb[3];
        to_srgb_exact(in, rgb);
        for (int k = 0; k < 3; ++k)
            worst = std::max(worst, std::abs(rgb[k] - in[m_channels == 1 ? 0 : k]));
    };
    for (int v = 0; v <= 255 && worst <= k_identity_tolerance; ++v) {
        double const x = v / 255.0;
        double const ramps[4][3] = { { x, x, x }, { x, 0, 0 }, { 0, x, 0 }, { 0, 0, x } };
        for (int r = 0; r < (m_channels == 1 ? 1 : 4); ++r)
            probe(ramps[r]);
    }
    for (int i = 0; i < 9 * 9 * 9 && worst <= k_identity_tolerance && m_channels == 3; ++i) {
        double const in[3] = { (i / 81) / 8.0, (i / 9 % 9) / 8.0, (i % 9) / 8.0 };
        probe(in);
    }
    if (worst <= k_identity_tolerance)
        m_kind = Kind::Identity;
}

void Transform::exact_pcs(double const* in, Vec3& xyz) const
{
    xyz = { 0, 0, 0 };
    if (m_profile && m_profile->to_pcs(m_intent, in, xyz))
        xyz = m_black.apply(xyz);
}

void Transform::to_srgb_exact(double const* in, double* rgb) const
{
    if (m_kind == Kind::ShaperMatrix || m_kind == Kind::Identity) {
        double c[3] { 0, 0, 0 };
        for (std::size_t i = 0; i < m_curves.size(); ++i)
            c[i] = m_curves[i].eval(clamp01(finite_or_zero(in[i])));
        for (std::size_t r = 0; r < 3; ++r) {
            double const v = m_to_linear[r * 3] * c[0] + m_to_linear[r * 3 + 1] * c[1] + m_to_linear[r * 3 + 2] * c[2] + m_to_linear[9 + r];
            rgb[r] = srgb_encode(v);
        }
        return;
    }
    Vec3 xyz {};
    exact_pcs(in, xyz);
    Vec3 const linear = pcs_to_linear_srgb(xyz);
    for (std::size_t r = 0; r < 3; ++r)
        rgb[r] = srgb_encode(linear[r]);
}

void Transform::table_linear(float const* in, double* linear) const
{
    // The profile's table run with its sampled curves, to its own PCS
    // values (clipped by nothing yet), then one matrix to linear sRGB.
    double device[16] {};
    for (int i = 0; i < m_channels; ++i)
        device[i] = static_cast<double>(in[i]);
    Vec3 value {};
    bool lab = false;
    if (!m_profile->pcs_value(m_intent, device, value, lab, true))
        value = { 0, 0, 0 };
    Vec3 const xyz = lab ? lab_to_xyz(value, k_d50) : value;
    for (std::size_t r = 0; r < 3; ++r)
        linear[r] = m_to_linear[r * 3] * xyz[0] + m_to_linear[r * 3 + 1] * xyz[1] + m_to_linear[r * 3 + 2] * xyz[2] + m_to_linear[9 + r];
}

void Transform::to_srgb(float const* in, float* rgb) const
{
    switch (m_kind) {
    case Kind::Identity:
        for (int k = 0; k < 3; ++k)
            rgb[k] = static_cast<float>(clamp01(finite_or_zero(static_cast<double>(in[m_channels == 1 ? 0 : k]))));
        return;
    case Kind::Table: {
        double linear[3];
        table_linear(in, linear);
        for (int k = 0; k < 3; ++k)
            rgb[k] = static_cast<float>(srgb_encode(finite_or_zero(linear[k])));
        return;
    }
    case Kind::ShaperMatrix: {
        double d[16] {};
        for (int i = 0; i < m_channels; ++i)
            d[i] = static_cast<double>(in[i]);
        double out[3];
        to_srgb_exact(d, out);
        for (int k = 0; k < 3; ++k)
            rgb[k] = static_cast<float>(out[k]);
        return;
    }
    }
}

void Transform::to_srgb8(float const* in, std::size_t count, std::uint8_t* rgb) const
{
    std::size_t const n = static_cast<std::size_t>(m_channels);
    switch (m_kind) {
    case Kind::Identity:
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t k = 0; k < 3; ++k)
                rgb[i * 3 + k] = to_byte(finite_or_zero(static_cast<double>(in[i * n + (n == 1 ? 0 : k)])));
        }
        return;
    case Kind::ShaperMatrix: {
        auto const& encode = encode_table();
        std::size_t const steps = k_curve_steps + 1;
        float const m[12] = { static_cast<float>(m_to_linear[0]), static_cast<float>(m_to_linear[1]), static_cast<float>(m_to_linear[2]),
            static_cast<float>(m_to_linear[3]), static_cast<float>(m_to_linear[4]), static_cast<float>(m_to_linear[5]),
            static_cast<float>(m_to_linear[6]), static_cast<float>(m_to_linear[7]), static_cast<float>(m_to_linear[8]),
            static_cast<float>(m_to_linear[9]), static_cast<float>(m_to_linear[10]), static_cast<float>(m_to_linear[11]) };
        for (std::size_t i = 0; i < count; ++i) {
            float c[3] = { 0, 0, 0 };
            for (std::size_t ch = 0; ch < n; ++ch) {
                float v = in[i * n + ch];
                v = v > 0 ? (v < 1 ? v : 1) : 0; // NaN goes to 0
                float const p = v * k_curve_steps;
                std::size_t const at = std::min(static_cast<std::size_t>(p), static_cast<std::size_t>(k_curve_steps - 1));
                float const f = p - static_cast<float>(at);
                float const* table = m_curve_tables.data() + ch * steps;
                c[ch] = table[at] + f * (table[at + 1] - table[at]);
            }
            for (std::size_t r = 0; r < 3; ++r) {
                float const v = m[r * 3] * c[0] + m[r * 3 + 1] * c[1] + m[r * 3 + 2] * c[2] + m[9 + r];
                float const clamped = v > 0 ? (v < 1 ? v : 1) : 0;
                rgb[i * 3 + r] = encode[static_cast<std::size_t>(clamped * 65535.0f + 0.5f)];
            }
        }
        return;
    }
    case Kind::Table: {
        // Encoded by the table, with the last color kept (flat areas repeat).
        auto const& encode = encode_table();
        std::array<float, 16> last_in;
        last_in.fill(-1);
        std::uint8_t last_out[3] = { 0, 0, 0 };
        for (std::size_t i = 0; i < count; ++i) {
            float const* px = in + i * n;
            if (std::memcmp(px, last_in.data(), n * sizeof(float)) != 0) {
                if (m_compiled) {
                    float linear[3];
                    m_compiled->run(px, linear);
                    for (std::size_t k = 0; k < 3; ++k)
                        last_out[k] = encode[static_cast<std::size_t>(CompiledTable::clamp01f(linear[k]) * 65535.0f + 0.5f)];
                } else {
                    double linear[3];
                    table_linear(px, linear);
                    for (std::size_t k = 0; k < 3; ++k)
                        last_out[k] = encode[static_cast<std::size_t>(clamp01(finite_or_zero(linear[k])) * 65535.0 + 0.5)];
                }
                std::memcpy(last_in.data(), px, n * sizeof(float));
            }
            std::memcpy(rgb + i * 3, last_out, 3);
        }
        return;
    }
    }
}

}
