#pragma once

// TrueType: a reader for TTF, OTF and TTC files — the tables that turn a
// code point into an outline and an advance: head, maxp, hhea, hmtx, loca,
// glyf (simple and composite), cmap (formats 0, 4, 6, 12), name, OS/2 —
// and, for CFF-flavored OpenType, the `CFF ` table's charstrings through
// the interpreter in Cff.h; the kerning in GPOS or `kern`; and colour: a
// glyph as a PNG from a bitmap strike (CBDT/CBLC, sbix) or as outlines
// layered in palette colours (COLR/CPAL). Fonts are attacker-controlled
// data: every offset is bounds-checked, a malformed glyph fails alone
// instead of taking the face with it, and a harness fuzzes the parser.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::text {

class CffFont;

// The OS/2 ulUnicodeRange bit that covers a code point, when one does:
// what a font claims before anyone reads its cmap.
std::optional<int> os2_unicode_range_bit(char32_t code_point);

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

// What a font file says about one of its faces, read without loading it:
// enough to catalogue a directory of fonts and choose among them.
struct FaceInfo {
    std::string path;
    std::size_t face_index = 0;
    std::string family;
    std::string subfamily;
    std::uint16_t weight_class = 400;
    int stretch = 100; // OS/2 usWidthClass as CSS font-stretch reads it: 50 (ultra-condensed) to 200 (ultra-expanded), 100 normal
    bool italic = false;
    bool has_outlines = true; // something to draw from: glyf, CFF charstrings, bitmap strikes or colour layers
    std::array<std::uint32_t, 4> unicode_ranges { 0, 0, 0, 0 }; // OS/2 ulUnicodeRange1-4; all zero when unsaid
    bool color = false; // a colour font: bitmap strikes (CBDT, sbix) or COLR layers

    bool claims(char32_t code_point) const
    {
        std::optional<int> const bit = os2_unicode_range_bit(code_point);
        return bit && (unicode_ranges[static_cast<std::size_t>(*bit / 32)] >> (*bit % 32)) & 1u;
    }
    bool claims_nothing() const
    {
        return unicode_ranges[0] == 0 && unicode_ranges[1] == 0 && unicode_ranges[2] == 0
            && unicode_ranges[3] == 0;
    }
};

class TrueTypeFont {
public:
    // Faces in the file: 1 for a TTF, the collection count for a TTC, 0
    // when the bytes are not a font at all.
    static std::size_t face_count(std::vector<std::uint8_t> const& bytes);

    // Parses one face; nullopt on any structural problem.
    static std::optional<TrueTypeFont> parse(std::vector<std::uint8_t> bytes,
        std::size_t face_index = 0);

    // Reads only the table directory and the head, name and OS/2 tables of
    // each face in the file, by seeking: a catalogue entry per face, none
    // when the file is not a font.
    static std::vector<FaceInfo> scan_file(std::string const& path);

    std::uint16_t units_per_em() const { return m_units_per_em; }
    std::uint16_t glyph_count() const { return m_glyph_count; }
    std::int16_t ascender() const { return m_ascender; }
    std::int16_t descender() const { return m_descender; } // negative below the baseline
    std::int16_t line_gap() const { return m_line_gap; }
    std::int16_t x_height() const { return m_x_height; } // 0 when the font does not say
    std::int16_t cap_height() const { return m_cap_height; }
    std::uint16_t weight_class() const { return m_weight_class; } // 400 regular, 700 bold
    int stretch() const { return m_stretch; } // as FaceInfo::stretch
    bool is_italic() const { return m_italic; }
    bool is_bold() const { return m_weight_class >= 600; }
    // Whether the face can draw: a glyf table, a CFF table that parsed, a
    // bitmap strike or colour layers.
    bool has_outlines() const
    {
        return m_has_glyf || m_cff != nullptr || has_bitmap_glyphs() || has_color_layers();
    }
    bool has_cff() const { return m_has_cff; }
    bool has_bitmap_glyphs() const { return !m_strikes.empty(); }
    bool has_color_layers() const { return m_color_glyph_count != 0; }
    std::string const& family_name() const { return m_family; }
    std::string const& subfamily_name() const { return m_subfamily; }

    // 0 (.notdef) when the font has no glyph for the code point.
    std::uint16_t glyph_index(char32_t code_point) const;
    // How many code points the cmap maps to some glyph — the coverage a
    // fallback chain ranks by.
    std::size_t mapped_code_points() const;
    std::uint16_t advance_width(std::uint16_t glyph) const; // font units
    std::int16_t left_side_bearing(std::uint16_t glyph) const;
    // The adjustment to the advance between two glyphs side by side, font
    // units, from the GPOS `kern` feature's pair positioning when the font
    // has one and from the `kern` table otherwise; 0 for a pair neither
    // names. Negative pulls the pair together, as AV and To are.
    std::int16_t kerning(std::uint16_t left, std::uint16_t right) const;
    bool has_kerning() const { return !m_gpos_pair_subtables.empty() || !m_kern_subtables.empty(); }
    // Composites are flattened, and a CFF charstring's cubic curves are
    // rewritten as quadratics; nullopt for a malformed glyph. An empty
    // outline (no contours) is a valid result: spaces.
    std::optional<GlyphOutline> outline(std::uint16_t glyph) const;

    // A glyph as a picture: the PNG the bitmap strike nearest `size`
    // holds for it, with where the picture sits — in px at the strike's
    // ppem, from the pen: `left` to its left edge, `bottom` from the
    // baseline up to its bottom edge (so a picture standing on the
    // baseline has bottom 0). `width` and `height` are the strike's
    // metrics, or 0 when the strike gives none (sbix) and the picture's
    // own size is the size. nullopt when no strike has the glyph or the
    // image is not a PNG.
    struct BitmapGlyph {
        std::vector<std::uint8_t> png;
        std::uint16_t ppem = 0;
        int left = 0;
        int bottom = 0;
        int width = 0;
        int height = 0;
    };
    std::optional<BitmapGlyph> bitmap_glyph(std::uint16_t glyph, float size) const;

    // A glyph as layers of outlines in colour (COLR version 0 with the
    // first CPAL palette): drawn first to last, each the outline of
    // `glyph` filled with the colour, or with the text's own colour when
    // `foreground` is set. Empty for a glyph without layers.
    struct ColorLayer {
        std::uint16_t glyph = 0;
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
        std::uint8_t alpha = 255;
        bool foreground = false;
    };
    std::vector<ColorLayer> color_layers(std::uint16_t glyph) const;

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
    void load_kerning();
    void load_color();
    std::int16_t gpos_pair_adjustment(std::uint32_t subtable, std::uint16_t left, std::uint16_t right) const;
    std::int16_t kern_table_adjustment(std::uint32_t subtable, std::uint16_t left, std::uint16_t right) const;
    bool outline_into(std::uint16_t glyph, GlyphOutline& out, int depth) const;
    bool parse_simple_glyph(std::uint32_t offset, std::uint32_t length, GlyphOutline& out) const;
    bool parse_composite_glyph(std::uint32_t offset, std::uint32_t length, GlyphOutline& out,
        int depth) const;
    bool glyph_span(std::uint16_t glyph, std::uint32_t& offset, std::uint32_t& length) const;
    std::string name_string(std::uint16_t name_id) const;

    // A bitmap strike: a size's worth of pictures. CBLC's index subtables
    // are walked per glyph; sbix's strike holds one offset per glyph.
    struct Strike {
        std::uint16_t ppem = 0;
        bool sbix = false;
        std::uint32_t offset = 0; // CBLC: the index subtable array; sbix: the strike
        std::uint32_t count = 0; // CBLC: index subtables in the array
        std::uint16_t first_glyph = 0; // CBLC: the strike's glyph range
        std::uint16_t last_glyph = 0;
    };
    std::optional<BitmapGlyph> cblc_glyph(Strike const& strike, std::uint16_t glyph) const;
    std::optional<BitmapGlyph> sbix_glyph(Strike const& strike, std::uint16_t glyph) const;

    std::vector<std::uint8_t> m_bytes;
    Table m_head, m_hhea, m_hmtx, m_maxp, m_loca, m_glyf, m_cmap, m_name, m_os2, m_cff_table, m_kern,
        m_gpos, m_cblc, m_cbdt, m_sbix, m_colr, m_cpal;
    std::vector<Strike> m_strikes; // sorted by ppem
    // COLR version 0: the base glyph records (sorted by glyph) and the
    // layer records they index, as offsets into the table; CPAL's first
    // palette as colour records.
    std::uint32_t m_color_glyph_count = 0;
    std::uint32_t m_color_base_records = 0;
    std::uint32_t m_color_layer_records = 0;
    std::uint32_t m_color_layer_count = 0;
    std::uint32_t m_palette_records = 0; // absolute offset of the first palette's first colour
    std::uint16_t m_palette_size = 0;
    // Where the kerning is read from, found once: the pair positioning
    // subtables (formats 1 and 2) of every `kern` feature lookup in GPOS,
    // or, when GPOS names none, the horizontal format 0 subtables of the
    // `kern` table. Absolute offsets into the file, in lookup order.
    std::vector<std::uint32_t> m_gpos_pair_subtables;
    std::vector<std::uint32_t> m_kern_subtables;
    bool m_has_glyf = false;
    bool m_has_cff = false;
    // The CFF font's tables, parsed once; null when there are none or they
    // did not parse. Shared, so a copy of the face costs nothing here.
    std::shared_ptr<CffFont const> m_cff;
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
    int m_stretch = 100;
    bool m_italic = false;
    std::string m_family;
    std::string m_subfamily;
    std::vector<Run> m_runs; // sorted by first, non-overlapping
};

}
