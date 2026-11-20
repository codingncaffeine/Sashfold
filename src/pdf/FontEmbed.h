#pragma once

// Embedding a subset of a system font in a PDF (ISO 32000-2 §9.7, §9.8,
// §9.9, §9.10.3): what a form field's appearance is written with when the
// field's own font cannot show its value's characters, so that every
// other reader shows, copies and searches the typed text. The font is a
// Type 0 font over Identity-H whose codes are the face's own glyph ids:
// a CIDFontType2 with the TrueType outlines (FontFile2, every glyph id
// kept, the unused ones empty, CIDToGIDMap Identity), or a CIDFontType0
// with a CID-keyed CFF (FontFile3 /CIDFontType0C) of the used glyphs
// alone, each keeping its glyph id as its CID through the charset. Both
// carry /W widths, a FontDescriptor read from the face's tables and a
// ToUnicode CMap from each glyph to the text it stands for.

#include "pdf/Object.h"
#include "text/TrueType.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::pdf {

// One face of a font file on the system, read once and kept for the
// program's life (CJK collections are large; a form saved twice reads
// them once).
class SystemFace {
public:
    static std::shared_ptr<SystemFace const> load(std::string const& path, std::size_t face_index);
    // A face over bytes already in memory (tests).
    static std::shared_ptr<SystemFace const> from_bytes(std::vector<std::uint8_t> bytes, std::size_t face_index, std::string key);

    text::TrueTypeFont const& font() const { return *m_font; }
    std::string const& key() const { return m_key; } // path and face index
    std::uint16_t glyph(char32_t code_point) const { return m_font->glyph_index(code_point); }
    // The advance in thousandths of the em, rounded as /W writes it.
    int advance(std::uint16_t glyph) const;
    bool has_cff() const { return m_cff.second != 0; }
    // The PostScript name a PDF calls it by (name ID 6, else family-style).
    std::string const& postscript_name() const { return m_postscript_name; }

    struct Metrics {
        std::array<double, 4> bbox { 0, 0, 0, 0 }; // thousandths of the em
        double ascent = 0;
        double descent = 0;
        double cap_height = 0;
        double italic_angle = 0;
        double stem_v = 0;
        std::uint32_t flags = 0;
    };
    Metrics const& metrics() const { return m_metrics; }

    // The subsets themselves, for the font's file stream.
    std::vector<std::uint8_t> truetype_subset(std::vector<std::uint16_t> const& glyphs) const;
    std::vector<std::uint8_t> cff_subset(std::vector<std::uint16_t> const& glyphs) const;

private:
    SystemFace() = default;
    bool read(std::size_t face_index);
    std::pair<std::uint32_t, std::uint32_t> table(char const* tag) const;

    std::vector<std::uint8_t> m_bytes;
    std::unique_ptr<text::TrueTypeFont> m_font;
    std::string m_key;
    std::string m_postscript_name;
    std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> m_tables; // tag -> (offset, length)
    std::pair<std::uint32_t, std::uint32_t> m_cff { 0, 0 };
    Metrics m_metrics;
};

// The Type 0 font for the glyphs a document's appearances show (glyph id
// -> the text it stands for), returned as its dictionary. `place` puts
// each object the font is made of: it returns what to reference it by —
// the object itself, for a font used in place, or a reference to it, for
// one written into a file.
// `identity_crypt`: the streams are marked as not encrypted (an
// encrypted document read in memory), else they are plain.
Object embed_type0_font(SystemFace const& face, std::map<std::uint16_t, std::u32string> const& glyphs,
    std::function<Object(Object)> const& place, bool identity_crypt = false);

// A ToUnicode CMap (§9.10.3) of two-byte codes to their text.
std::string to_unicode_cmap(std::map<std::uint16_t, std::u32string> const& codes);

}
