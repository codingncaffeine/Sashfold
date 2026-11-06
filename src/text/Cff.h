#pragma once

// CFF: the Compact Font Format an OpenType font carries in its `CFF ` table
// when its outlines are PostScript curves rather than TrueType ones — the
// gsfonts on a Linux desktop, Noto's CJK families, most of macOS. Read for
// what a renderer needs: the Type 2 charstrings, their subroutines, the
// charset for accent composition, and for a CID-keyed font the FDSelect
// that picks each glyph's private subroutines. A charstring's cubic
// curves are rewritten as the quadratic form the rasterizer draws, to
// within a unit. Fonts are attacker-controlled data: every offset is
// bounds-checked, every glyph is bounded in work, a malformed glyph fails
// alone.
//
// The font keeps offsets into the file's bytes, never the bytes: the
// caller hands the same bytes to every call.

#include "text/TrueType.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sashfold::text {

class CffFont {
public:
    // Parses the table at [offset, offset + length) of the file's bytes;
    // nullopt for anything but a CFF 1 font with Type 2 charstrings.
    static std::optional<CffFont> parse(std::vector<std::uint8_t> const& bytes, std::size_t offset,
        std::size_t length);

    std::uint16_t glyph_count() const { return m_glyph_count; }
    bool is_cid() const { return m_cid; }
    // The font matrix's scale when it is not the usual 1/1000: what the
    // charstring units are multiplied by to reach the font's own units.
    float units_scale(std::uint16_t units_per_em) const;

    // The glyph's contours in charstring units; false for a malformed
    // charstring. An empty outline is a valid result: a space.
    bool outline(std::vector<std::uint8_t> const& bytes, std::uint16_t glyph, GlyphOutline& out) const;

    // For a font embedded in a PDF, which names its glyphs by code, name
    // or CID: reads the charset, the glyph names and the built-in
    // encoding once — call it before the font is shared between threads.
    void load_names(std::vector<std::uint8_t> const& bytes);
    std::optional<std::uint16_t> glyph_by_name(std::string_view name) const;
    // A CID-keyed font's glyph for a CID (any other font: the CID is the glyph).
    std::optional<std::uint16_t> glyph_by_cid(std::uint16_t cid) const;
    // The built-in encoding after load_names: a glyph per code, 0 for none.
    std::array<std::uint16_t, 256> const& encoding() const { return m_encoding; }
    std::size_t glyph_names_known() const { return m_names.size(); }
    static char const* standard_string(int sid);

private:
    struct Span {
        std::size_t offset = 0;
        std::size_t length = 0;
    };
    // An INDEX: count items laid end to end behind an offset array.
    struct Index {
        std::size_t count = 0;
        std::uint8_t off_size = 0;
        std::size_t offsets = 0; // where the offset array starts
        std::size_t data = 0; // what the offsets are relative to, less one
        std::size_t end = 0; // one past the last byte
    };
    struct Private {
        Index subrs;
        bool has_subrs = false;
    };

    bool read_index(std::vector<std::uint8_t> const& bytes, std::size_t at, Index& out, std::size_t& next) const;
    bool index_item(std::vector<std::uint8_t> const& bytes, Index const& index, std::size_t i, Span& out) const;
    bool read_private(std::vector<std::uint8_t> const& bytes, std::size_t offset, std::size_t length,
        Private& out) const;
    std::uint16_t glyph_of_standard_code(std::vector<std::uint8_t> const& bytes, std::uint8_t code) const;
    void load_charset(std::vector<std::uint8_t> const& bytes) const;
    std::string glyph_name_of_sid(std::vector<std::uint8_t> const& bytes, std::uint16_t sid) const;
    Private const& private_for(std::uint16_t glyph) const;

    friend struct Type2;

    std::size_t m_offset = 0;
    std::size_t m_end = 0;
    Index m_global_subrs;
    Index m_charstrings;
    Private m_private;
    std::vector<Private> m_fd_privates; // CID: one per font dict
    std::vector<std::uint8_t> m_fd_select; // CID: the font dict of each glyph
    std::size_t m_charset = 0; // 0: the ISOAdobe charset, glyph i has SID i
    Index m_strings; // the String INDEX: names past the standard strings
    std::size_t m_encoding_offset = 0; // 0 Standard, 1 Expert, else where the table is
    std::array<std::uint16_t, 256> m_encoding {};
    std::unordered_map<std::string, std::uint16_t> m_names;
    mutable std::vector<std::uint16_t> m_sid_of_glyph; // loaded on the first accent
    mutable std::unordered_map<std::uint16_t, std::uint16_t> m_gid_of_sid;
    mutable bool m_charset_loaded = false;
    std::uint16_t m_glyph_count = 0;
    bool m_cid = false;
    double m_matrix_scale = 0.001;
};

}
