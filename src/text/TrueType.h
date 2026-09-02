#pragma once

// TrueType: a reader for TTF and TTC files — the tables that turn a code
// point into an outline and an advance: head, maxp, hhea, hmtx, loca, glyf
// (simple and composite), cmap (formats 0, 4, 6, 12), name, OS/2. Fonts are
// attacker-controlled data: every offset is bounds-checked, a malformed glyph
// fails alone instead of taking the face with it, and a harness fuzzes the
// parser. CFF-flavored OpenType is recognized and declined until the CFF
// interpreter exists: its metrics and cmap load, its outlines do not.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::text {

struct GlyphPoint {
    std::int16_t x = 0; // font units, y up
    std::int16_t y = 0;
    bool on_curve = true; // off-curve points are quadratic control points
};

struct GlyphOutline {
    std::vector<GlyphPoint> points;
    std::vector<std::uint16_t> contour_ends; // index of each contour's last point
    std::int16_t x_min = 0; // computed from the points, never trusted from the file
    std::int16_t y_min = 0;
    std::int16_t x_max = 0;
    std::int16_t y_max = 0;
};

class TrueTypeFont {
public:
    // Faces in the file: 1 for a TTF, the collection count for a TTC, 0
    // when the bytes are not a font at all.
    static std::size_t face_count(std::vector<std::uint8_t> const& bytes);

    // Parses one face; nullopt on any structural problem.
    static std::optional<TrueTypeFont> parse(std::vector<std::uint8_t> bytes,
        std::size_t face_index = 0);

    std::uint16_t units_per_em() const { return m_units_per_em; }
    std::uint16_t glyph_count() const { return m_glyph_count; }
    std::int16_t ascender() const { return m_ascender; }
    std::int16_t descender() const { return m_descender; } // negative below the baseline
    std::int16_t line_gap() const { return m_line_gap; }
    std::int16_t x_height() const { return m_x_height; } // 0 when the font does not say
    std::int16_t cap_height() const { return m_cap_height; }
    std::uint16_t weight_class() const { return m_weight_class; } // 400 regular, 700 bold
    bool is_italic() const { return m_italic; }
    bool is_bold() const { return m_weight_class >= 600; }
    // False for CFF-flavored OpenType: metrics and cmap still work, outlines do not.
    bool has_outlines() const { return m_has_glyf; }
    bool has_cff() const { return m_has_cff; }
    std::string const& family_name() const { return m_family; }
    std::string const& subfamily_name() const { return m_subfamily; }

    // 0 (.notdef) when the font has no glyph for the code point.
    std::uint16_t glyph_index(char32_t code_point) const;
    // How many code points the cmap maps to some glyph — the coverage a
    // fallback chain ranks by.
    std::size_t mapped_code_points() const;
    std::uint16_t advance_width(std::uint16_t glyph) const; // font units
    std::int16_t left_side_bearing(std::uint16_t glyph) const;
    // Composites are flattened; nullopt for a malformed glyph or a CFF font.
    // An empty outline (no contours) is a valid result: spaces.
    std::optional<GlyphOutline> outline(std::uint16_t glyph) const;

private:
    struct Table {
        std::uint32_t offset = 0; // absolute, within the file
        std::uint32_t length = 0; // clipped to the file
        bool present() const { return length != 0; }
    };
    struct Run { // code points [first, last] map to glyph + (c - first)
        char32_t first = 0;
        char32_t last = 0;
        std::uint32_t glyph = 0;
    };

    TrueTypeFont() = default;
    bool load(std::size_t face_index);
    bool load_directory(std::uint32_t offset);
    bool load_head_and_metrics();
    bool load_cmap();
    bool load_cmap_subtable(std::size_t offset, std::vector<Run>& runs) const;
    static void push_run(std::vector<Run>& runs, char32_t first, char32_t last, std::uint32_t glyph);
    static void push_delta_run(std::vector<Run>& runs, char32_t first, char32_t last,
        std::uint16_t delta);
    void load_names();
    void load_os2();
    bool outline_into(std::uint16_t glyph, GlyphOutline& out, int depth) const;
    bool parse_simple_glyph(std::uint32_t offset, std::uint32_t length, GlyphOutline& out) const;
    bool parse_composite_glyph(std::uint32_t offset, std::uint32_t length, GlyphOutline& out,
        int depth) const;
    bool glyph_span(std::uint16_t glyph, std::uint32_t& offset, std::uint32_t& length) const;
    std::string name_string(std::uint16_t name_id) const;

    std::vector<std::uint8_t> m_bytes;
    Table m_head, m_hhea, m_hmtx, m_maxp, m_loca, m_glyf, m_cmap, m_name, m_os2;
    bool m_has_glyf = false;
    bool m_has_cff = false;
    bool m_long_loca = false;
    std::uint16_t m_mac_style = 0;
    std::uint16_t m_units_per_em = 1000;
    std::uint16_t m_glyph_count = 0;
    std::uint16_t m_metric_count = 0;
    std::int16_t m_ascender = 0;
    std::int16_t m_descender = 0;
    std::int16_t m_line_gap = 0;
    std::int16_t m_x_height = 0;
    std::int16_t m_cap_height = 0;
    std::uint16_t m_weight_class = 400;
    bool m_italic = false;
    std::string m_family;
    std::string m_subfamily;
    std::vector<Run> m_runs; // sorted by first, non-overlapping
};

}
