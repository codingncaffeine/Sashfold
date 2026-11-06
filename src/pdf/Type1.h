#pragma once

// Type 1 fonts (Adobe Type 1 Font Format), as PDF embeds them in /FontFile:
// a cleartext part with the font's encoding and matrix, an eexec-encrypted
// part with the private dictionary's subroutines and the charstrings, and
// (PFB) segment headers when a file was copied whole. The charstrings run
// through an interpreter of their own here — moves, lines, curves,
// subroutines, flex, hint replacement skipped, seac's accented letters —
// to outlines in charstring units. Fonts are attacker-controlled data:
// every read is bounded, a malformed glyph fails alone.

#include "svg/Raster.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::pdf {

class Type1Font {
public:
    // The /FontFile stream's bytes; Length1 and Length2 say where the
    // cleartext ends and the encrypted part does (0 when unknown: found).
    static std::optional<Type1Font> parse(std::vector<std::uint8_t> const& file, std::size_t length1 = 0,
        std::size_t length2 = 0);

    // The glyph's outline in charstring units and its advance; nullopt
    // when the font has no such glyph or its charstring is malformed.
    std::optional<svg::Path> glyph(std::string_view name, double* advance = nullptr) const;
    bool has_glyph(std::string_view name) const { return m_charstrings.count(std::string(name)) != 0; }

    // The font's own encoding: a name per code (empty for none). True when
    // it said StandardEncoding.
    std::array<std::string, 256> const& encoding() const { return m_encoding; }
    bool standard_encoding() const { return m_standard_encoding; }
    // Charstring units to glyph space (usually 0.001).
    std::array<double, 6> const& font_matrix() const { return m_font_matrix; }
    std::size_t glyph_count() const { return m_charstrings.size(); }

private:
    bool run(std::vector<std::uint8_t> const& charstring, int depth, struct Type1State& state) const;

    std::map<std::string, std::vector<std::uint8_t>, std::less<>> m_charstrings; // decrypted, lenIV dropped
    std::vector<std::vector<std::uint8_t>> m_subrs;
    std::array<std::string, 256> m_encoding;
    bool m_standard_encoding = false;
    std::array<double, 6> m_font_matrix { 0.001, 0, 0, 0.001, 0, 0 };
};

// eexec's (55665) and charstring (4330) decryption.
std::vector<std::uint8_t> type1_decrypt(std::vector<std::uint8_t> const& data, std::uint16_t key, std::size_t skip);

}
