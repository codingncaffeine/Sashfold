#include "text/SashfoldMono.h"

#include <array>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace sashfold::text {

namespace {

// One stroke of a glyph, on the 20x32 grid.
//   V: centerline from (a,b) to (c,d), thickened horizontally
//   H: centerline from (a,b) to (c,d), thickened vertically
//   B: axis-aligned box with corners (a,b)-(c,d), drawn as-is
struct Seg {
    char kind;
    std::int8_t a, b, c, d;
};

struct GlyphDef {
    char32_t code_point;
    std::vector<Seg> segments;
};

Seg V(int a, int b, int c, int d) { return { 'V', static_cast<std::int8_t>(a), static_cast<std::int8_t>(b), static_cast<std::int8_t>(c), static_cast<std::int8_t>(d) }; }
Seg H(int a, int b, int c, int d) { return { 'H', static_cast<std::int8_t>(a), static_cast<std::int8_t>(b), static_cast<std::int8_t>(c), static_cast<std::int8_t>(d) }; }
Seg B(int a, int b, int c, int d) { return { 'B', static_cast<std::int8_t>(a), static_cast<std::int8_t>(b), static_cast<std::int8_t>(c), static_cast<std::int8_t>(d) }; }

// The face. Key ys: cap top 3, x-height top 10, baseline 25, descender 31.
// Key xs: left 4, right 16, center 10 (advance 20).
std::vector<GlyphDef> build_glyphs()
{
    std::vector<GlyphDef> defs;
    auto add = [&](char32_t code_point, std::vector<Seg> segments) {
        defs.push_back(GlyphDef { code_point, std::move(segments) });
    };

    // Uppercase.
    add(U'A', { V(10, 3, 4, 25), V(10, 3, 16, 25), H(6, 18, 14, 18) });
    add(U'B', { V(4, 3, 4, 25), H(5, 4, 14, 4), H(5, 14, 14, 14), H(5, 24, 15, 24), V(15, 5, 15, 13), V(16, 15, 16, 23) });
    add(U'C', { V(4, 5, 4, 23), H(6, 4, 16, 4), H(6, 24, 16, 24) });
    add(U'D', { V(4, 3, 4, 25), H(5, 4, 13, 4), H(5, 24, 13, 24), V(16, 6, 16, 22) });
    add(U'E', { V(4, 3, 4, 25), H(6, 4, 17, 4), H(6, 14, 14, 14), H(6, 24, 17, 24) });
    add(U'F', { V(4, 3, 4, 25), H(6, 4, 17, 4), H(6, 14, 14, 14) });
    add(U'G', { V(4, 5, 4, 23), H(6, 4, 16, 4), H(6, 24, 15, 24), V(16, 15, 16, 23), H(11, 15, 15, 15) });
    add(U'H', { V(4, 3, 4, 25), V(16, 3, 16, 25), H(6, 14, 14, 14) });
    add(U'I', { V(10, 3, 10, 25), H(5, 4, 15, 4), H(5, 24, 15, 24) });
    add(U'J', { V(15, 3, 15, 22), H(6, 24, 13, 24), V(4, 19, 4, 23) });
    add(U'K', { V(4, 3, 4, 25), V(16, 3, 6, 14), V(8, 15, 16, 25) });
    add(U'L', { V(4, 3, 4, 25), H(6, 24, 17, 24) });
    add(U'M', { V(3, 3, 3, 25), V(17, 3, 17, 25), V(3, 3, 10, 16), V(17, 3, 10, 16) });
    add(U'N', { V(4, 3, 4, 25), V(16, 3, 16, 25), V(4, 3, 16, 25) });
    add(U'O', { V(4, 5, 4, 23), V(16, 5, 16, 23), H(6, 4, 14, 4), H(6, 24, 14, 24) });
    add(U'P', { V(4, 3, 4, 25), H(5, 4, 14, 4), H(5, 15, 14, 15), V(16, 5, 16, 14) });
    add(U'Q', { V(4, 5, 4, 23), V(16, 5, 16, 23), H(6, 4, 14, 4), H(6, 24, 14, 24), V(12, 19, 17, 28) });
    add(U'R', { V(4, 3, 4, 25), H(5, 4, 14, 4), H(5, 15, 14, 15), V(16, 5, 16, 14), V(9, 15, 16, 25) });
    add(U'S', { H(6, 4, 16, 4), V(4, 5, 4, 13), H(6, 14, 14, 14), V(16, 15, 16, 23), H(4, 24, 14, 24) });
    add(U'T', { H(3, 4, 17, 4), V(10, 5, 10, 25) });
    add(U'U', { V(4, 3, 4, 23), V(16, 3, 16, 23), H(6, 24, 14, 24) });
    add(U'V', { V(4, 3, 10, 25), V(16, 3, 10, 25) });
    add(U'W', { V(3, 3, 7, 25), V(10, 8, 7, 25), V(10, 8, 13, 25), V(17, 3, 13, 25) });
    add(U'X', { V(4, 3, 16, 25), V(16, 3, 4, 25) });
    add(U'Y', { V(4, 3, 10, 14), V(16, 3, 10, 14), V(10, 14, 10, 25) });
    add(U'Z', { H(4, 4, 16, 4), V(16, 5, 4, 22), H(4, 24, 16, 24) });

    // Lowercase.
    add(U'a', { H(6, 11, 15, 11), V(16, 12, 16, 24), H(6, 24, 14, 24), V(4, 17, 4, 23), H(6, 16, 14, 16) });
    add(U'b', { V(4, 3, 4, 25), H(6, 11, 14, 11), V(16, 13, 16, 23), H(6, 24, 14, 24) });
    add(U'c', { H(6, 11, 16, 11), V(4, 13, 4, 23), H(6, 24, 16, 24) });
    add(U'd', { V(16, 3, 16, 25), H(6, 11, 14, 11), V(4, 13, 4, 23), H(6, 24, 14, 24) });
    add(U'e', { H(6, 11, 14, 11), V(16, 12, 16, 16), H(6, 17, 16, 17), V(4, 13, 4, 23), H(6, 24, 15, 24) });
    add(U'f', { H(9, 4, 15, 4), V(7, 5, 7, 25), H(3, 11, 13, 11) });
    add(U'g', { H(6, 11, 14, 11), V(4, 13, 4, 21), H(6, 22, 14, 22), V(16, 11, 16, 28), H(5, 30, 14, 30) });
    add(U'h', { V(4, 3, 4, 25), H(6, 11, 14, 11), V(16, 13, 16, 25) });
    add(U'i', { V(10, 10, 10, 25), H(9, 5, 11, 5) });
    add(U'j', { V(12, 10, 12, 28), H(4, 30, 10, 30), H(11, 5, 13, 5) });
    add(U'k', { V(4, 3, 4, 25), V(15, 10, 6, 17), V(8, 16, 15, 25) });
    add(U'l', { V(10, 3, 10, 25) });
    add(U'm', { V(3, 10, 3, 25), H(4, 11, 16, 11), V(10, 12, 10, 25), V(17, 12, 17, 25) });
    add(U'n', { V(4, 10, 4, 25), H(6, 11, 14, 11), V(16, 13, 16, 25) });
    add(U'o', { H(6, 11, 14, 11), H(6, 24, 14, 24), V(4, 13, 4, 23), V(16, 13, 16, 23) });
    add(U'p', { V(4, 10, 4, 31), H(6, 11, 14, 11), V(16, 13, 16, 23), H(6, 24, 14, 24) });
    add(U'q', { V(16, 10, 16, 31), H(6, 11, 14, 11), V(4, 13, 4, 23), H(6, 24, 14, 24) });
    add(U'r', { V(4, 10, 4, 25), H(6, 11, 16, 11) });
    add(U's', { H(6, 11, 16, 11), V(4, 12, 4, 16), H(6, 17, 14, 17), V(16, 18, 16, 22), H(4, 24, 14, 24) });
    add(U't', { V(7, 4, 7, 25), H(3, 10, 13, 10) });
    add(U'u', { V(4, 10, 4, 23), V(16, 10, 16, 25), H(6, 24, 14, 24) });
    add(U'v', { V(4, 10, 10, 25), V(16, 10, 10, 25) });
    add(U'w', { V(3, 10, 7, 25), V(10, 14, 7, 25), V(10, 14, 13, 25), V(17, 10, 13, 25) });
    add(U'x', { V(4, 10, 16, 25), V(16, 10, 4, 25) });
    add(U'y', { V(4, 10, 10, 22), V(16, 10, 8, 31) });
    add(U'z', { H(4, 11, 16, 11), V(15, 12, 5, 22), H(4, 24, 16, 24) });

    // Digits.
    add(U'0', { V(4, 5, 4, 23), V(16, 5, 16, 23), H(6, 4, 14, 4), H(6, 24, 14, 24), V(14, 7, 6, 21) });
    add(U'1', { V(10, 3, 10, 25), V(10, 3, 6, 7), H(5, 24, 15, 24) });
    add(U'2', { H(4, 4, 15, 4), V(16, 5, 16, 12), V(15, 13, 4, 22), H(4, 24, 17, 24) });
    add(U'3', { H(4, 4, 15, 4), V(16, 5, 16, 12), H(8, 14, 15, 14), V(16, 16, 16, 22), H(4, 24, 15, 24) });
    add(U'4', { V(13, 3, 3, 17), H(3, 18, 17, 18), V(13, 3, 13, 25) });
    add(U'5', { H(4, 4, 17, 4), V(4, 5, 4, 13), H(6, 13, 14, 13), V(16, 15, 16, 22), H(4, 24, 14, 24) });
    add(U'6', { V(4, 5, 4, 23), H(6, 4, 16, 4), H(6, 14, 14, 14), V(16, 16, 16, 22), H(6, 24, 14, 24) });
    add(U'7', { H(3, 4, 17, 4), V(17, 5, 8, 25) });
    add(U'8', { H(6, 4, 14, 4), V(4, 5, 4, 12), V(16, 5, 16, 12), H(6, 14, 14, 14), V(4, 16, 4, 23), V(16, 16, 16, 23), H(6, 24, 14, 24) });
    add(U'9', { H(6, 4, 14, 4), V(4, 6, 4, 12), V(16, 6, 16, 23), H(6, 14, 14, 14), H(6, 24, 14, 24) });

    // ASCII punctuation.
    add(U'!', { V(10, 3, 10, 18), H(9, 23, 11, 23) });
    add(U'"', { V(7, 3, 7, 9), V(13, 3, 13, 9) });
    add(U'#', { V(8, 4, 6, 24), V(14, 4, 12, 24), H(3, 10, 17, 10), H(3, 18, 17, 18) });
    add(U'$', { H(6, 5, 16, 5), V(4, 6, 4, 13), H(6, 14, 14, 14), V(16, 15, 16, 22), H(4, 23, 14, 23), V(10, 1, 10, 27) });
    add(U'%', { B(3, 3, 8, 8), B(12, 20, 17, 25), V(16, 4, 4, 24) });
    add(U'&', { H(6, 4, 12, 4), V(4, 6, 4, 11), V(13, 6, 13, 11), V(13, 11, 4, 19), V(4, 13, 4, 23), H(6, 24, 13, 24), V(9, 15, 17, 25) });
    add(U'\'', { V(10, 3, 10, 9) });
    add(U'(', { V(13, 3, 10, 10), V(10, 9, 10, 19), V(10, 18, 13, 25) });
    add(U')', { V(7, 3, 10, 10), V(10, 9, 10, 19), V(10, 18, 7, 25) });
    add(U'*', { V(10, 4, 10, 14), V(5, 6, 15, 12), V(15, 6, 5, 12) });
    add(U'+', { V(10, 9, 10, 21), H(4, 15, 16, 15) });
    add(U',', { H(9, 23, 11, 23), V(10, 25, 8, 29) });
    add(U'-', { H(5, 15, 15, 15) });
    add(U'.', { H(9, 23, 11, 23) });
    add(U'/', { V(15, 3, 5, 27) });
    add(U':', { H(9, 11, 11, 11), H(9, 23, 11, 23) });
    add(U';', { H(9, 11, 11, 11), H(9, 23, 11, 23), V(10, 25, 8, 29) });
    add(U'<', { V(15, 5, 5, 15), V(5, 15, 15, 25) });
    add(U'=', { H(4, 11, 16, 11), H(4, 19, 16, 19) });
    add(U'>', { V(5, 5, 15, 15), V(15, 15, 5, 25) });
    add(U'?', { H(4, 4, 14, 4), V(16, 5, 16, 10), V(15, 11, 10, 16), V(10, 15, 10, 18), H(9, 23, 11, 23) });
    add(U'@', { H(5, 4, 15, 4), V(3, 6, 3, 22), H(5, 24, 16, 24), V(17, 6, 17, 16), V(13, 10, 13, 17), H(9, 10, 12, 10), V(8, 11, 8, 16), H(9, 17, 14, 17) });
    add(U'[', { V(7, 3, 7, 28), H(8, 4, 13, 4), H(8, 27, 13, 27) });
    add(U'\\', { V(5, 3, 15, 27) });
    add(U']', { V(13, 3, 13, 28), H(7, 4, 12, 4), H(7, 27, 12, 27) });
    add(U'^', { V(10, 3, 5, 10), V(10, 3, 15, 10) });
    add(U'_', { H(2, 29, 18, 29) });
    add(U'`', { V(8, 3, 11, 7) });
    add(U'{', { V(12, 4, 9, 9), V(9, 8, 9, 13), H(5, 15, 9, 15), V(9, 17, 9, 22), V(9, 21, 12, 26) });
    add(U'|', { V(10, 3, 10, 29) });
    add(U'}', { V(8, 4, 11, 9), V(11, 8, 11, 13), H(11, 15, 15, 15), V(11, 17, 11, 22), V(11, 21, 8, 26) });
    add(U'~', { V(4, 15, 8, 12), V(8, 12, 12, 15), V(12, 15, 16, 12) });

    // Specials.
    add(0xFFFD, { H(4, 4, 16, 4), H(4, 24, 16, 24), V(3, 5, 3, 23), V(17, 5, 17, 23), V(13, 8, 7, 20) });
    add(0x2022, { B(6, 12, 14, 20) }); // bullet
    add(0x25E6, { H(7, 13, 13, 13), H(7, 19, 13, 19), V(6, 14, 6, 18), V(14, 14, 14, 18) }); // white bullet
    add(0x25AA, { B(6, 13, 14, 21) }); // small black square
    add(0x2013, { H(3, 15, 17, 15) }); // en dash
    add(0x2014, { H(1, 15, 19, 15) }); // em dash
    add(0x2018, { V(10, 3, 8, 9) }); // quotes
    add(0x2019, { V(8, 3, 10, 9) });
    add(0x201C, { V(9, 3, 7, 9), V(14, 3, 12, 9) });
    add(0x201D, { V(7, 3, 9, 9), V(12, 3, 14, 9) });
    add(0x2026, { H(3, 23, 5, 23), H(9, 23, 11, 23), H(15, 23, 17, 23) }); // ellipsis
    add(0x00A0, {}); // nbsp draws nothing

    return defs;
}

struct Point {
    std::int64_t x, y; // 1/4-subpixel fixed point
};

using Quad = std::array<Point, 4>;

// Expands a segment to its quad on the design grid, in grid units x4
// (so half-unit offsets stay integral).
Quad expand(Seg const& seg, int weight)
{
    auto const gp = [](int gx, int gy) { return Point { gx * 4, gy * 4 }; };
    int const half = 2 * weight; // stroke half-width in quarter-units (weight 3 -> 6)
    switch (seg.kind) {
    case 'V': {
        Point const a = gp(seg.a, seg.b);
        Point const b = gp(seg.c, seg.d);
        return Quad { Point { a.x - half, a.y }, Point { a.x + half, a.y },
            Point { b.x + half, b.y }, Point { b.x - half, b.y } };
    }
    case 'H': {
        Point const a = gp(seg.a, seg.b);
        Point const b = gp(seg.c, seg.d);
        return Quad { Point { a.x, a.y - half }, Point { b.x, b.y - half },
            Point { b.x, b.y + half }, Point { a.x, a.y + half } };
    }
    default: { // 'B'
        Point const a = gp(seg.a, seg.b);
        Point const b = gp(seg.c, seg.d);
        return Quad { Point { a.x, a.y }, Point { b.x, a.y }, Point { b.x, b.y },
            Point { a.x, b.y } };
    }
    }
}

// Even-odd point-in-polygon, integer arithmetic only.
bool inside(Quad const& quad, std::int64_t px, std::int64_t py)
{
    bool in = false;
    for (std::size_t i = 0, j = 3; i < 4; j = i++) {
        Point const& a = quad[i];
        Point const& b = quad[j];
        if ((a.y > py) == (b.y > py))
            continue;
        // x of the edge at height py, compared without division:
        // px < a.x + (b.x-a.x)*(py-a.y)/(b.y-a.y)
        std::int64_t const dy = b.y - a.y;
        std::int64_t const lhs = (px - a.x) * dy;
        std::int64_t const rhs = (b.x - a.x) * (py - a.y);
        if (dy > 0 ? lhs < rhs : lhs > rhs)
            in = !in;
    }
    return in;
}

struct GlyphMask {
    int left = 0; // px offset from the glyph origin
    int top = 0; // px offset from the TOP of the em box (origin y - ascent)
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
};

struct MaskKey {
    char32_t code_point;
    int size_q; // size in quarter px
    bool bold;
    bool italic;
    bool operator==(MaskKey const&) const = default;
};

struct MaskKeyHash {
    std::size_t operator()(MaskKey const& key) const
    {
        std::size_t hash = static_cast<std::size_t>(key.code_point);
        hash = hash * 1315423911u ^ static_cast<std::size_t>(key.size_q);
        hash = hash * 1315423911u ^ (static_cast<std::size_t>(key.bold) << 1
                                        | static_cast<std::size_t>(key.italic));
        return hash;
    }
};

class Rasterizer {
public:
    std::unordered_map<char32_t, std::vector<Seg> const*> index;
    std::vector<GlyphDef> glyphs = build_glyphs();
    std::unordered_map<MaskKey, GlyphMask, MaskKeyHash> cache;

    Rasterizer()
    {
        for (GlyphDef const& glyph : glyphs)
            index.emplace(glyph.code_point, &glyph.segments);
    }

    std::vector<Seg> const* find(char32_t code_point)
    {
        auto const it = index.find(code_point);
        if (it != index.end())
            return it->second;
        return nullptr;
    }

    GlyphMask const& mask_for(char32_t code_point, int size_q, bool bold, bool italic)
    {
        MaskKey const key { code_point, size_q, bold, italic };
        if (auto const it = cache.find(key); it != cache.end())
            return it->second;

        std::vector<Seg> const* segments = find(code_point);
        if (!segments)
            segments = find(0xFFFD);

        GlyphMask mask;
        // Scale: pixels = grid * size / 32. In fixed point: the glyph grid is
        // held in quarter-units; one grid unit = size_q/32 quarter-pixels.
        // A point at grid-quarter g maps to quarter-pixels g * size_q / 128.
        std::int64_t const size_quarters = size_q;
        auto const to_quarter_px = [&](std::int64_t grid_quarters) {
            return grid_quarters * size_quarters / 128;
        };

        int const weight = bold ? 4 : 3;
        std::vector<Quad> quads;
        for (Seg const& seg : *segments) {
            Quad quad = expand(seg, weight);
            for (Point& point : quad) {
                if (italic) {
                    // Oblique shear: x grows as y rises above the baseline.
                    std::int64_t const baseline = 25 * 4;
                    point.x += (baseline - point.y) / 4;
                }
                point.x = to_quarter_px(point.x);
                point.y = to_quarter_px(point.y);
            }
            quads.push_back(quad);
        }

        // Pixel bounds of the em box, padded for stroke overhang, AA bleed,
        // and the italic shear.
        int const em_px = (size_q + 3) / 4;
        mask.left = -2;
        mask.top = -2;
        mask.width = em_px * 20 / 32 + 5 + (italic ? em_px / 4 + 1 : 0);
        mask.height = em_px + 5;
        mask.alpha.assign(static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height), 0);

        // Sample points double once so subcell centers stay integral.
        std::vector<Quad> doubled(quads.size());
        for (std::size_t i = 0; i < quads.size(); ++i) {
            for (std::size_t j = 0; j < 4; ++j)
                doubled[i][j] = Point { quads[i][j].x * 2, quads[i][j].y * 2 };
        }

        for (int py = 0; py < mask.height; ++py) {
            for (int px = 0; px < mask.width; ++px) {
                // 4x4 subsamples at quarter-pixel centers.
                int hits = 0;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sx = 0; sx < 4; ++sx) {
                        std::int64_t const qx = (static_cast<std::int64_t>(px) + mask.left) * 4 + sx;
                        std::int64_t const qy = (static_cast<std::int64_t>(py) + mask.top) * 4 + sy;
                        for (Quad const& quad : doubled) {
                            if (inside(quad, qx * 2 + 1, qy * 2 + 1)) {
                                ++hits;
                                break;
                            }
                        }
                    }
                }
                mask.alpha[static_cast<std::size_t>(py) * static_cast<std::size_t>(mask.width)
                    + static_cast<std::size_t>(px)]
                    = static_cast<std::uint8_t>(hits * 255 / 16);
            }
        }

        auto const [it, inserted] = cache.emplace(key, std::move(mask));
        (void)inserted;
        return it->second;
    }
};

Rasterizer& rasterizer()
{
    static Rasterizer instance;
    return instance;
}

} // namespace

SashfoldMono const& SashfoldMono::instance()
{
    static SashfoldMono font;
    return font;
}

void SashfoldMono::draw_glyph(Bitmap& target, char32_t code_point, float x, float baseline_y,
    float size, Color color, bool bold, bool italic) const
{
    if (code_point == U' ' || code_point == 0xA0 || code_point < 0x21)
        return;
    int const size_q = static_cast<int>(size * 4.0f + 0.5f);
    if (size_q <= 0)
        return;
    GlyphMask const& mask = rasterizer().mask_for(code_point, size_q, bold, italic);

    float const ascent = design_ascent * size / static_cast<float>(units_per_em);
    int const origin_x = static_cast<int>(x + 0.5f) + mask.left;
    int const origin_y = static_cast<int>(baseline_y - ascent + 0.5f) + mask.top;
    for (int py = 0; py < mask.height; ++py) {
        for (int px = 0; px < mask.width; ++px) {
            std::uint8_t const alpha = mask.alpha[static_cast<std::size_t>(py)
                    * static_cast<std::size_t>(mask.width)
                + static_cast<std::size_t>(px)];
            if (alpha == 0)
                continue;
            Color shaded = color;
            shaded.a = static_cast<std::uint8_t>(static_cast<int>(color.a) * alpha / 255);
            target.blend_pixel(origin_x + px, origin_y + py, shaded);
        }
    }
}

}
