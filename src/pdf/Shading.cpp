#include "pdf/Shading.h"

#include "pdf/Document.h"

#include <algorithm>
#include <cmath>

namespace sashfold::pdf {

namespace {

constexpr std::size_t k_max_triangles = 2'000'000;

std::vector<double> doubles(Document const& doc, Object const& object)
{
    std::vector<double> out;
    for (Object const& item : doc.resolve(object).as_array())
        out.push_back(doc.resolve(item).as_number());
    return out;
}

struct BitReader {
    std::vector<std::uint8_t> const& data;
    std::size_t bit = 0;
    bool done() const { return bit / 8 >= data.size(); }
    std::uint32_t read(int count)
    {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i, ++bit) {
            std::size_t const byte = bit / 8;
            int const v = byte < data.size() ? (data[byte] >> (7 - bit % 8)) & 1 : 0;
            value = (value << 1) | static_cast<std::uint32_t>(v);
        }
        return value;
    }
    double read_scaled(int count, double low, double high)
    {
        if (count <= 0)
            return low;
        double const max = count >= 32 ? 4294967295.0 : static_cast<double>((1ull << count) - 1);
        return low + static_cast<double>(read(count)) * (high - low) / max;
    }
    void align() { bit = (bit + 7) / 8 * 8; }
};

void add_triangle(std::vector<ShadingVertex>& out, ShadingVertex const& a, ShadingVertex const& b, ShadingVertex const& c)
{
    if (out.size() / 3 >= k_max_triangles)
        return;
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
}

// A patch as the standard writes it: P[i * 4 + j] is p_ij, i along u and j
// along v, S(u, v) = sum of p_ij B_i(u) B_j(v); the corner colors are at
// p00, p03, p33 and p30. Cut into a grid of triangles, colors bilinear.
void patch_to_triangles(std::array<std::array<double, 2>, 16> const& p, std::array<std::array<float, 32>, 4> const& corner_colors,
    int values, std::vector<ShadingVertex>& out)
{
    auto const bernstein = [](double t, std::array<double, 4>& b) {
        double const s = 1 - t;
        b = { s * s * s, 3 * t * s * s, 3 * t * t * s, t * t * t };
    };
    constexpr int k_steps = 12;
    std::array<std::array<ShadingVertex, k_steps + 1>, k_steps + 1> grid {}; // [u step][v step]
    for (int a = 0; a <= k_steps; ++a) {
        double const u = static_cast<double>(a) / k_steps;
        std::array<double, 4> bu {};
        bernstein(u, bu);
        for (int b = 0; b <= k_steps; ++b) {
            double const v = static_cast<double>(b) / k_steps;
            std::array<double, 4> bv {};
            bernstein(v, bv);
            double x = 0;
            double y = 0;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    double const w = bu[static_cast<std::size_t>(i)] * bv[static_cast<std::size_t>(j)];
                    x += w * p[static_cast<std::size_t>(i * 4 + j)][0];
                    y += w * p[static_cast<std::size_t>(i * 4 + j)][1];
                }
            }
            ShadingVertex& vertex = grid[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)];
            vertex.x = x;
            vertex.y = y;
            for (int k = 0; k < values; ++k) {
                auto const kk = static_cast<std::size_t>(k);
                vertex.color[kk] = static_cast<float>((1 - u) * (1 - v) * static_cast<double>(corner_colors[0][kk])
                    + (1 - u) * v * static_cast<double>(corner_colors[1][kk]) + u * v * static_cast<double>(corner_colors[2][kk])
                    + u * (1 - v) * static_cast<double>(corner_colors[3][kk]));
            }
        }
    }
    for (int a = 0; a < k_steps; ++a) {
        for (int b = 0; b < k_steps; ++b) {
            auto const& p00 = grid[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)];
            auto const& p01 = grid[static_cast<std::size_t>(a)][static_cast<std::size_t>(b + 1)];
            auto const& p11 = grid[static_cast<std::size_t>(a + 1)][static_cast<std::size_t>(b + 1)];
            auto const& p10 = grid[static_cast<std::size_t>(a + 1)][static_cast<std::size_t>(b)];
            add_triangle(out, p00, p01, p11);
            add_triangle(out, p00, p11, p10);
        }
    }
}

void parse_mesh(Document const& doc, Stream const& stream, Shading& shading)
{
    Dict const& dict = stream.dict;
    int const bits_coordinate = static_cast<int>(doc.resolve(dict.get("BitsPerCoordinate")).as_int(0));
    int const bits_component = static_cast<int>(doc.resolve(dict.get("BitsPerComponent")).as_int(0));
    int const bits_flag = static_cast<int>(doc.resolve(dict.get("BitsPerFlag")).as_int(8));
    std::vector<double> const decode = doubles(doc, dict.get("Decode"));
    int const values = shading.color_values();
    if (bits_coordinate < 1 || bits_coordinate > 32 || bits_component < 1 || bits_component > 16
        || decode.size() < static_cast<std::size_t>(4 + 2 * values))
        return;
    std::vector<std::uint8_t> const data = doc.decode(stream);
    BitReader reader { data };
    auto const read_vertex = [&](ShadingVertex& v) {
        v.x = reader.read_scaled(bits_coordinate, decode[0], decode[1]);
        v.y = reader.read_scaled(bits_coordinate, decode[2], decode[3]);
        for (int k = 0; k < values; ++k)
            v.color[static_cast<std::size_t>(k)] = static_cast<float>(
                reader.read_scaled(bits_component, decode[static_cast<std::size_t>(4 + 2 * k)], decode[static_cast<std::size_t>(5 + 2 * k)]));
    };
    auto const read_point = [&](std::array<double, 2>& p) {
        p[0] = reader.read_scaled(bits_coordinate, decode[0], decode[1]);
        p[1] = reader.read_scaled(bits_coordinate, decode[2], decode[3]);
    };
    auto const read_color = [&](std::array<float, 32>& c) {
        for (int k = 0; k < values; ++k)
            c[static_cast<std::size_t>(k)] = static_cast<float>(
                reader.read_scaled(bits_component, decode[static_cast<std::size_t>(4 + 2 * k)], decode[static_cast<std::size_t>(5 + 2 * k)]));
    };
    if (shading.type == 4) {
        // Free-form: a flag per vertex; 0 starts a triangle, 1 and 2 reuse two of the previous.
        std::vector<ShadingVertex> recent;
        while (!reader.done() && shading.triangles.size() / 3 < k_max_triangles) {
            std::uint32_t const flag = reader.read(bits_flag);
            ShadingVertex v;
            read_vertex(v);
            reader.align();
            if (reader.bit / 8 > data.size())
                break;
            if (flag == 0) {
                recent.push_back(v);
                if (recent.size() == 3) {
                    add_triangle(shading.triangles, recent[0], recent[1], recent[2]);
                } else if (recent.size() > 3) {
                    recent.erase(recent.begin(), recent.end() - 1);
                }
                continue;
            }
            if (recent.size() < 3)
                continue;
            ShadingVertex const a = flag == 1 ? recent[1] : recent[0];
            ShadingVertex const b = recent[2];
            add_triangle(shading.triangles, a, b, v);
            recent = { a, b, v };
        }
        return;
    }
    if (shading.type == 5) {
        int const per_row = static_cast<int>(doc.resolve(dict.get("VerticesPerRow")).as_int(0));
        if (per_row < 2)
            return;
        std::vector<ShadingVertex> previous;
        while (!reader.done()) {
            std::vector<ShadingVertex> row(static_cast<std::size_t>(per_row));
            for (ShadingVertex& v : row)
                read_vertex(v);
            if (reader.bit / 8 > data.size())
                break;
            if (!previous.empty()) {
                for (int i = 0; i + 1 < per_row; ++i) {
                    auto const ii = static_cast<std::size_t>(i);
                    add_triangle(shading.triangles, previous[ii], previous[ii + 1], row[ii]);
                    add_triangle(shading.triangles, previous[ii + 1], row[ii + 1], row[ii]);
                }
            }
            previous = std::move(row);
            if (shading.triangles.size() / 3 >= k_max_triangles)
                break;
        }
        return;
    }
    // Patches: the boundary in the order the standard gives (12 points),
    // then for type 7 the inner four.
    std::array<std::array<double, 2>, 12> last_boundary {};
    std::array<std::array<float, 32>, 4> last_colors {};
    bool have_last = false;
    while (!reader.done() && shading.triangles.size() / 3 < k_max_triangles) {
        std::uint32_t const flag = reader.read(bits_flag);
        std::array<std::array<double, 2>, 12> boundary {};
        std::array<std::array<float, 32>, 4> colors {};
        int first_new = 0;
        if (flag != 0) {
            if (!have_last)
                break;
            // The shared edge of the previous patch becomes points 0-3 and colors 0-1.
            static constexpr int k_edges[4][4] = { { 0, 0, 0, 0 }, { 3, 4, 5, 6 }, { 6, 7, 8, 9 }, { 9, 10, 11, 0 } };
            static constexpr int k_edge_colors[4][2] = { { 0, 0 }, { 1, 2 }, { 2, 3 }, { 3, 0 } };
            if (flag > 3)
                break;
            for (int i = 0; i < 4; ++i)
                boundary[static_cast<std::size_t>(i)] = last_boundary[static_cast<std::size_t>(k_edges[flag][i])];
            colors[0] = last_colors[static_cast<std::size_t>(k_edge_colors[flag][0])];
            colors[1] = last_colors[static_cast<std::size_t>(k_edge_colors[flag][1])];
            first_new = 4;
        }
        for (int i = first_new; i < 12; ++i)
            read_point(boundary[static_cast<std::size_t>(i)]);
        std::array<std::array<double, 2>, 4> inner {};
        if (shading.type == 7) {
            for (auto& point : inner)
                read_point(point);
        }
        for (int i = flag != 0 ? 2 : 0; i < 4; ++i)
            read_color(colors[static_cast<std::size_t>(i)]);
        reader.align();
        if (reader.bit / 8 > data.size())
            break;
        last_boundary = boundary;
        last_colors = colors;
        have_last = true;
        // The boundary in the stream's order: p00 p01 p02 p03 p13 p23 p33
        // p32 p31 p30 p20 p10 (p_ij, i along u, j along v).
        std::array<std::array<double, 2>, 16> p {};
        auto const set = [&](int i, int j, std::array<double, 2> const& value) { p[static_cast<std::size_t>(i * 4 + j)] = value; };
        auto const at = [&](int i, int j) { return p[static_cast<std::size_t>(i * 4 + j)]; };
        static constexpr int k_boundary[12][2] = { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 3 }, { 2, 3 }, { 3, 3 }, { 3, 2 },
            { 3, 1 }, { 3, 0 }, { 2, 0 }, { 1, 0 } };
        for (int k = 0; k < 12; ++k)
            set(k_boundary[k][0], k_boundary[k][1], boundary[static_cast<std::size_t>(k)]);
        if (shading.type == 7) {
            // The inner four follow in the order p11 p12 p22 p21.
            set(1, 1, inner[0]);
            set(1, 2, inner[1]);
            set(2, 2, inner[2]);
            set(2, 1, inner[3]);
        } else {
            // A Coons patch's inner points, from its boundary (the standard's
            // formula): for p_ij with the nearest corner (ie, je) and the far
            // one (if, jf).
            auto const inner_point = [&](int i, int j) {
                int const ie = i == 1 ? 0 : 3;
                int const je = j == 1 ? 0 : 3;
                int const ifar = 3 - ie;
                int const jfar = 3 - je;
                std::array<double, 2> out {};
                for (std::size_t k = 0; k < 2; ++k) {
                    out[k] = (-4 * at(ie, je)[k] + 6 * (at(ie, j)[k] + at(i, je)[k]) - 2 * (at(ie, jfar)[k] + at(ifar, je)[k])
                                 + 3 * (at(ifar, j)[k] + at(i, jfar)[k]) - at(ifar, jfar)[k])
                        / 9;
                }
                return out;
            };
            set(1, 1, inner_point(1, 1));
            set(1, 2, inner_point(1, 2));
            set(2, 1, inner_point(2, 1));
            set(2, 2, inner_point(2, 2));
        }
        patch_to_triangles(p, colors, shading.color_values(), shading.triangles);
    }
}

std::array<float, 3> to_rgb_values(Shading const& shading, float const* values)
{
    std::array<float, 3> rgb {};
    shading.rgb_of(values, rgb.data());
    return rgb;
}

} // namespace

void Shading::rgb_of(float const* values, float* rgb) const
{
    if (function) {
        std::array<float, 32> out {};
        function->evaluate(values, out.data());
        space->to_rgb(out.data(), rgb);
        return;
    }
    space->to_rgb(values, rgb);
}

std::shared_ptr<Shading const> parse_shading(Document const& doc, Object const& object_in, Dict const* resources)
{
    Object const object = doc.resolve(object_in);
    Dict const& dict = object.as_dict();
    auto shading = std::make_shared<Shading>();
    shading->type = static_cast<int>(doc.resolve(dict.get("ShadingType")).as_int(0));
    shading->space = ColorSpace::parse(doc, dict.get("ColorSpace"), resources);
    if (!shading->space || shading->space->family() == ColorSpace::Family::Pattern || shading->type < 1 || shading->type > 7)
        return nullptr;
    if (dict.has("Function")) {
        shading->function = Function::parse(doc, dict.get("Function"));
        if (shading->function && shading->function->outputs() < shading->space->components())
            shading->function = nullptr;
        if (!shading->function && shading->type <= 3)
            return nullptr;
    } else if (shading->type <= 3) {
        return nullptr;
    }
    std::vector<double> const background = doubles(doc, dict.get("Background"));
    if (static_cast<int>(background.size()) == shading->space->components()) {
        std::array<float, 32> b {};
        for (std::size_t i = 0; i < background.size(); ++i)
            b[i] = static_cast<float>(background[i]);
        shading->background = b;
    }
    std::vector<double> const bbox = doubles(doc, dict.get("BBox"));
    if (bbox.size() == 4)
        shading->bbox = std::array<double, 4> { std::min(bbox[0], bbox[2]), std::min(bbox[1], bbox[3]), std::max(bbox[0], bbox[2]), std::max(bbox[1], bbox[3]) };
    std::vector<double> const domain = doubles(doc, dict.get("Domain"));
    Array const& extend_array = doc.resolve(dict.get("Extend")).as_array();
    if (extend_array.size() == 2) {
        shading->extend = { doc.resolve(extend_array[0]).as_bool(), doc.resolve(extend_array[1]).as_bool() };
    }
    if (shading->type == 1) {
        if (domain.size() == 4)
            shading->domain = { domain[0], domain[1], domain[2], domain[3] };
        std::vector<double> const matrix = doubles(doc, dict.get("Matrix"));
        if (matrix.size() == 6)
            shading->matrix = canvas::Transform { matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5] };
        if (shading->function->inputs() != 2)
            return nullptr;
        return shading;
    }
    if (shading->type == 2 || shading->type == 3) {
        std::vector<double> const coords = doubles(doc, dict.get("Coords"));
        std::size_t const needed = shading->type == 2 ? 4 : 6;
        if (coords.size() < needed)
            return nullptr;
        for (std::size_t i = 0; i < needed; ++i)
            shading->coords[i] = coords[i];
        if (domain.size() >= 2)
            shading->domain = { domain[0], domain[1], 0, 1 };
        if (shading->function->inputs() != 1)
            return nullptr;
        return shading;
    }
    Stream const* const stream = object.as_stream();
    if (!stream)
        return nullptr;
    if (shading->function && shading->function->inputs() != 1)
        return nullptr;
    parse_mesh(doc, *stream, *shading);
    return shading;
}

void paint_shading(canvas::Surface& layer, Shading const& shading, canvas::Transform const& to_device, std::array<int, 4> area,
    bool use_background)
{
    int const x0 = std::max(0, area[0]);
    int const y0 = std::max(0, area[1]);
    int const x1 = std::min(layer.width, area[2]);
    int const y1 = std::min(layer.height, area[3]);
    if (x0 >= x1 || y0 >= y1)
        return;
    std::optional<canvas::Transform> const inverse = to_device.inverse();
    if (!inverse)
        return;
    // Points in doubles: the shading's geometry is in doubles throughout.
    struct DPoint {
        double x = 0;
        double y = 0;
    };
    auto const to_shading = [&](double x, double y) {
        return DPoint { inverse->a * x + inverse->c * y + inverse->e, inverse->b * x + inverse->d * y + inverse->f };
    };
    auto const put = [&](int x, int y, float const* rgb) {
        std::uint8_t* const pixel = layer.pixels.data() + layer.offset(x, y);
        pixel[0] = static_cast<std::uint8_t>(std::lround(std::clamp(rgb[0], 0.0f, 1.0f) * 255));
        pixel[1] = static_cast<std::uint8_t>(std::lround(std::clamp(rgb[1], 0.0f, 1.0f) * 255));
        pixel[2] = static_cast<std::uint8_t>(std::lround(std::clamp(rgb[2], 0.0f, 1.0f) * 255));
        pixel[3] = 255;
    };
    std::array<float, 3> background_rgb {};
    bool const fill_background = use_background && shading.background;
    if (fill_background)
        shading.space->to_rgb(shading.background->data(), background_rgb.data());
    auto const in_bbox = [&](DPoint p) {
        if (!shading.bbox)
            return true;
        auto const& b = *shading.bbox;
        return p.x >= b[0] && p.x <= b[2] && p.y >= b[1] && p.y <= b[3];
    };

    if (shading.type == 2 || shading.type == 3) {
        SampledFunction1D const sampled(*shading.function, static_cast<float>(shading.domain[0]), static_cast<float>(shading.domain[1]), 1024);
        // The colors along t, converted once.
        constexpr int k_colors = 1024;
        std::vector<std::array<float, 3>> colors(k_colors);
        for (int i = 0; i < k_colors; ++i) {
            float const t = static_cast<float>(shading.domain[0] + (shading.domain[1] - shading.domain[0]) * i / (k_colors - 1));
            std::array<float, 32> out {};
            sampled.evaluate(t, out.data());
            shading.space->to_rgb(out.data(), colors[static_cast<std::size_t>(i)].data());
        }
        auto const color_at = [&](double s) { return colors[static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(s * (k_colors - 1))), 0, k_colors - 1))]; };
        auto const& c = shading.coords;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                DPoint const p = to_shading(x + 0.5, y + 0.5);
                if (!in_bbox(p))
                    continue;
                double s = 0;
                bool valid = true;
                if (shading.type == 2) {
                    double const dx = c[2] - c[0];
                    double const dy = c[3] - c[1];
                    double const len = dx * dx + dy * dy;
                    s = len == 0 ? 0 : ((p.x - c[0]) * dx + (p.y - c[1]) * dy) / len;
                    if (s < 0) {
                        if (!shading.extend[0])
                            valid = false;
                        s = 0;
                    } else if (s > 1) {
                        if (!shading.extend[1])
                            valid = false;
                        s = 1;
                    }
                } else {
                    // The largest s with the point on the circle of s and a radius not negative.
                    double const cdx = c[3] - c[0];
                    double const cdy = c[4] - c[1];
                    double const dr = c[5] - c[2];
                    double const pdx = p.x - c[0];
                    double const pdy = p.y - c[1];
                    double const a = cdx * cdx + cdy * cdy - dr * dr;
                    double const b = pdx * cdx + pdy * cdy + c[2] * dr;
                    double const cc = pdx * pdx + pdy * pdy - c[2] * c[2];
                    auto const acceptable = [&](double candidate, double& clamped) {
                        if (c[2] + candidate * dr < 0)
                            return false;
                        if (candidate >= 0 && candidate <= 1) {
                            clamped = candidate;
                            return true;
                        }
                        if (candidate > 1 && shading.extend[1]) {
                            clamped = 1;
                            return true;
                        }
                        if (candidate < 0 && shading.extend[0]) {
                            clamped = 0;
                            return true;
                        }
                        return false;
                    };
                    valid = false;
                    if (std::abs(a) < 1e-9) {
                        if (std::abs(b) > 1e-12) {
                            double const candidate = cc / (2 * b);
                            valid = acceptable(candidate, s);
                        }
                    } else {
                        double const disc = b * b - a * cc;
                        if (disc >= 0) {
                            double const root = std::sqrt(disc);
                            double const s1 = (b + root) / a;
                            double const s2 = (b - root) / a;
                            double const hi = std::max(s1, s2);
                            double const lo = std::min(s1, s2);
                            valid = acceptable(hi, s) || acceptable(lo, s);
                        }
                    }
                }
                if (!valid) {
                    if (fill_background)
                        put(x, y, background_rgb.data());
                    continue;
                }
                put(x, y, color_at(s).data());
            }
        }
        return;
    }
    if (shading.type == 1) {
        std::optional<canvas::Transform> const to_domain = shading.matrix.inverse();
        if (!to_domain)
            return;
        std::array<float, 3> last_rgb {};
        float last_in[2] = { NAN, NAN };
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                DPoint const p = to_shading(x + 0.5, y + 0.5);
                if (!in_bbox(p))
                    continue;
                DPoint const d { to_domain->a * p.x + to_domain->c * p.y + to_domain->e, to_domain->b * p.x + to_domain->d * p.y + to_domain->f };
                if (d.x < shading.domain[0] || d.x > shading.domain[1] || d.y < shading.domain[2] || d.y > shading.domain[3]) {
                    if (fill_background)
                        put(x, y, background_rgb.data());
                    continue;
                }
                float const in[2] = { static_cast<float>(d.x), static_cast<float>(d.y) };
                if (in[0] != last_in[0] || in[1] != last_in[1]) {
                    std::array<float, 32> out {};
                    shading.function->evaluate(in, out.data());
                    shading.space->to_rgb(out.data(), last_rgb.data());
                    last_in[0] = in[0];
                    last_in[1] = in[1];
                }
                put(x, y, last_rgb.data());
            }
        }
        return;
    }
    // Meshes: the background first, then each triangle, its colors interpolated across it.
    if (fill_background) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x)
                put(x, y, background_rgb.data());
        }
    }
    int const values = shading.color_values();
    for (std::size_t t = 0; t + 2 < shading.triangles.size(); t += 3) {
        ShadingVertex const* v[3] = { &shading.triangles[t], &shading.triangles[t + 1], &shading.triangles[t + 2] };
        DPoint d[3];
        for (int i = 0; i < 3; ++i)
            d[i] = DPoint { to_device.a * v[i]->x + to_device.c * v[i]->y + to_device.e, to_device.b * v[i]->x + to_device.d * v[i]->y + to_device.f };
        double const area2 = (d[1].x - d[0].x) * (d[2].y - d[0].y) - (d[2].x - d[0].x) * (d[1].y - d[0].y);
        if (std::abs(area2) < 1e-9)
            continue;
        int const left = std::max(x0, static_cast<int>(std::floor(std::min({ d[0].x, d[1].x, d[2].x }))));
        int const right = std::min(x1, static_cast<int>(std::ceil(std::max({ d[0].x, d[1].x, d[2].x }))) + 1);
        int const top = std::max(y0, static_cast<int>(std::floor(std::min({ d[0].y, d[1].y, d[2].y }))));
        int const bottom = std::min(y1, static_cast<int>(std::ceil(std::max({ d[0].y, d[1].y, d[2].y }))) + 1);
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                double const px = x + 0.5;
                double const py = y + 0.5;
                double const w0 = ((d[1].x - px) * (d[2].y - py) - (d[2].x - px) * (d[1].y - py)) / area2;
                double const w1 = ((d[2].x - px) * (d[0].y - py) - (d[0].x - px) * (d[2].y - py)) / area2;
                double const w2 = 1 - w0 - w1;
                constexpr double k_edge = -1e-6;
                if (w0 < k_edge || w1 < k_edge || w2 < k_edge)
                    continue;
                std::array<float, 32> values_at {};
                for (int k = 0; k < values; ++k) {
                    auto const kk = static_cast<std::size_t>(k);
                    values_at[kk] = static_cast<float>(w0 * static_cast<double>(v[0]->color[kk]) + w1 * static_cast<double>(v[1]->color[kk])
                        + w2 * static_cast<double>(v[2]->color[kk]));
                }
                std::array<float, 3> const rgb = to_rgb_values(shading, values_at.data());
                put(x, y, rgb.data());
            }
        }
    }
}

}
