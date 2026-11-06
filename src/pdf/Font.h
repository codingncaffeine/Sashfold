#pragma once

// A PDF font (ISO 32000-2 §9.5-9.10): how a string's bytes become glyphs,
// each with its width and its text. Simple fonts (Type 1, TrueType, Type 3)
// take a byte per glyph through their encoding — the base encoding, the
// /Differences, the font program's own — and composite fonts (Type 0) split
// their strings by a CMap into CIDs, which the CIDFont maps to glyphs.
// Outlines come from the embedded program: Type 1, bare CFF, TrueType or
// OpenType. A font with no program is drawn with a system font of its
// kind — the URW fonts made to the metrics of the standard 14, Liberation,
// the Noto CJK families for CJK collections — each glyph condensed to
// the width the PDF gives it, as poppler does. Loaded once per document
// and read from any thread.

#include "paint/Canvas2D.h"
#include "pdf/CMap.h"
#include "pdf/Document.h"
#include "svg/Raster.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::text {
class TrueTypeFont;
class CffFont;
}

namespace sashfold::pdf {

class Type1Font;

struct ShownGlyph {
    std::uint32_t code = 0;
    int length = 1; // bytes the code took
    std::uint32_t cid = 0; // composite fonts: the CID; simple: the code
    double width = 0; // horizontal advance in text space units per unit of font size (w0 / 1000)
    double vertical_advance = 0; // composite vertical: w1 / 1000 (negative downward)
    double origin_x = 0; // vertical writing: the position vector v / 1000
    double origin_y = 0;
    bool word_space = false; // a single-byte code 32: Tw applies
};

class Font {
public:
    enum class Kind { Simple, Type3, Composite };

    ~Font();
    Font(Font const&) = delete;
    Font& operator=(Font const&) = delete;

    // From a font dictionary (or a reference to one). Never null for a
    // dictionary: a font that cannot be read draws with a substitute.
    static std::shared_ptr<Font const> load(Document const& doc, Object const& font);

    Kind kind() const { return m_kind; }
    bool vertical() const { return m_vertical; }

    // The glyph at `at` in a string (moved past it); false at its end.
    bool next(std::string_view bytes, std::size_t& at, ShownGlyph& glyph) const;
    // The glyph's outline in text space per unit of font size (y up),
    // already condensed to the PDF's width for a substitute; null for a
    // glyph that draws nothing (and for Type 3, whose glyphs are content).
    std::shared_ptr<svg::Path const> outline(ShownGlyph const& glyph) const;
    // The text a glyph stands for: ToUnicode, else its glyph name, else the CID collection's table.
    std::u32string text(ShownGlyph const& glyph) const;

    // Type 3: the glyph's content stream, the font matrix and resources.
    Stream const* type3_procedure(ShownGlyph const& glyph) const;
    canvas::Transform const& type3_matrix() const { return m_type3_matrix; }
    Dict const& type3_resources() const { return m_type3_resources; }

    bool substituted() const { return m_substituted; }
    std::string const& base_font() const { return m_base_font; }

private:
    Font() = default;
    void load_simple(Document const& doc, Dict const& dict);
    void load_composite(Document const& doc, Dict const& dict);
    void load_program(Document const& doc, Dict const& descriptor, std::string const& subtype);
    void choose_substitute(Dict const& descriptor, std::string const& ordering);
    std::optional<std::uint16_t> glyph_of(ShownGlyph const& glyph) const;
    std::shared_ptr<svg::Path const> build_outline(std::uint16_t glyph, std::string const& type1_name) const;
    double program_advance(std::uint16_t glyph) const; // in glyph space per 1000

    Kind m_kind = Kind::Simple;
    bool m_vertical = false;
    bool m_substituted = false;
    std::string m_base_font;

    // Simple fonts: per code, its glyph name, width (glyph space / 1000) and glyph.
    std::array<std::string, 256> m_names;
    std::array<double, 256> m_widths {};
    std::array<bool, 256> m_has_width {};
    std::array<std::int32_t, 256> m_code_glyphs {}; // -1: none decided at load
    double m_missing_width = 0;
    bool m_symbolic = false;

    // Composite fonts.
    std::shared_ptr<CMap const> m_encoding_cmap;
    std::map<std::uint32_t, double> m_cid_widths;
    double m_default_width = 1000;
    std::map<std::uint32_t, std::array<double, 3>> m_cid_vertical; // w1 vx vy
    std::array<double, 2> m_default_vertical { 880, -1000 };
    std::vector<std::uint16_t> m_cid_to_gid; // empty: identity
    bool m_cid_to_gid_identity = true;
    std::string m_collection; // Registry-Ordering
    std::shared_ptr<std::vector<char32_t> const> m_collection_unicode;

    std::shared_ptr<CMap const> m_to_unicode;

    // The program.
    std::shared_ptr<std::vector<std::uint8_t> const> m_program_bytes;
    std::shared_ptr<text::TrueTypeFont const> m_truetype;
    std::shared_ptr<text::CffFont const> m_cff; // a bare CFF (FontFile3 /Type1C or /CIDFontType0C)
    std::shared_ptr<Type1Font const> m_type1;
    std::map<std::string, std::uint16_t> m_truetype_names; // from 'post'
    double m_program_scale = 0.001; // program units to text space per unit of font size
    canvas::Transform m_program_matrix; // a Type 1's or CFF's font matrix, when not a plain scale

    // Type 3.
    canvas::Transform m_type3_matrix { 0.001, 0, 0, 0.001, 0, 0 };
    Dict m_type3_resources;
    Dict m_char_procs;
    Document const* m_doc = nullptr;

    mutable std::mutex m_mutex;
    mutable std::map<std::uint32_t, std::shared_ptr<svg::Path const>> m_outlines; // by glyph (or code for Type 1 names)
};

}
