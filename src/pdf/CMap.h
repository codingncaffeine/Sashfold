#pragma once

// CMaps (ISO 32000-2 §9.7.5, §9.10.3; Adobe Technical Note 5014): how a
// composite font's string bytes split into codes by its codespace ranges
// and become CIDs, and how a ToUnicode CMap turns codes into text. The
// Identity CMaps are built in; the predefined CJK CMaps are read when
// asked for from the system's CMap resources (poppler-data's or
// Ghostscript's, as poppler reads them), and `usecmap` reaches them too.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::pdf {

class CMap {
public:
    static std::shared_ptr<CMap const> identity(bool vertical);
    // Identity-H/V, else a predefined CMap from the system; null when none is found.
    static std::shared_ptr<CMap const> predefined(std::string const& name);
    // An embedded CMap stream's (or ToUnicode's) bytes.
    static std::shared_ptr<CMap const> parse(std::vector<std::uint8_t> const& data, int depth = 0);

    // The code at `at` in the string (moved past it) and its length in
    // bytes. False at the end of the string.
    bool next_code(std::string_view bytes, std::size_t& at, std::uint32_t& code, int& length) const;
    // The CID a code maps to (0, the .notdef CID, when none).
    std::uint32_t cid(std::uint32_t code, int length) const;
    // A ToUnicode CMap's text for a code; empty when it has none.
    std::u32string unicode(std::uint32_t code, int length) const;
    bool has_unicode() const { return !m_unicode.empty() || !m_unicode_ranges.empty(); }
    bool vertical() const { return m_vertical; }
    bool is_identity() const { return m_identity; }
    std::string const& name() const { return m_name; }

private:
    struct Codespace {
        int length = 1;
        std::uint32_t low = 0;
        std::uint32_t high = 0;
    };
    struct Range {
        int length = 1;
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        std::uint32_t cid = 0;
    };
    struct UnicodeRange {
        int length = 1;
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        std::u32string start; // the first code's text; its last character counts up
        std::vector<std::u32string> list; // or one text per code
    };
    void merge(CMap const& base);
    bool in_codespace(std::uint32_t code, int length) const;

    std::string m_name;
    bool m_identity = false;
    bool m_vertical = false;
    std::vector<Codespace> m_codespaces;
    std::vector<Range> m_ranges;
    std::map<std::pair<int, std::uint32_t>, std::uint32_t> m_chars;
    std::map<std::pair<int, std::uint32_t>, std::u32string> m_unicode;
    std::vector<UnicodeRange> m_unicode_ranges;
};

// A CID collection's CID-to-Unicode table from the system (poppler-data's
// cidToUnicode, e.g. "Adobe-Japan1"): what non-embedded CJK text maps to.
std::shared_ptr<std::vector<char32_t> const> cid_to_unicode_table(std::string const& collection);

// UTF-16BE bytes (a ToUnicode destination) as code points.
std::u32string utf16be_to_u32(std::string_view bytes);

}
