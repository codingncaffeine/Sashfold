#include "pdf/Encoding.h"

#include <algorithm>
#include <cstring>

namespace sashfold::pdf {

namespace {

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// A run of hex digits as a code point, when it is one.
std::optional<char32_t> hex_value(std::string_view digits)
{
    if (digits.empty() || digits.size() > 6)
        return std::nullopt;
    char32_t value = 0;
    for (char const c : digits) {
        int const v = hex_digit(c);
        if (v < 0)
            return std::nullopt;
        value = value * 16 + static_cast<char32_t>(v);
    }
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return std::nullopt;
    return value;
}

} // namespace

std::array<char const*, 256> const& base_encoding(BaseEncoding encoding)
{
    switch (encoding) {
    case BaseEncoding::WinAnsi: return encoding_data::k_win_ansi;
    case BaseEncoding::MacRoman: return encoding_data::k_mac_roman;
    case BaseEncoding::Symbol: return encoding_data::k_symbol;
    case BaseEncoding::ZapfDingbats: return encoding_data::k_zapf_dingbats;
    case BaseEncoding::Standard:
    case BaseEncoding::MacExpert:
        break;
    }
    return encoding_data::k_standard;
}

std::optional<BaseEncoding> base_encoding_named(std::string_view name)
{
    if (name == "StandardEncoding")
        return BaseEncoding::Standard;
    if (name == "WinAnsiEncoding")
        return BaseEncoding::WinAnsi;
    if (name == "MacRomanEncoding")
        return BaseEncoding::MacRoman;
    if (name == "MacExpertEncoding")
        return BaseEncoding::MacExpert;
    return std::nullopt;
}

std::u32string glyph_name_unicode(std::string_view name)
{
    if (name.empty())
        return {};
    // A suffix after a period is a variant of the same character.
    std::size_t const dot = name.find('.');
    if (dot != std::string_view::npos && dot > 0)
        name = name.substr(0, dot);
    // Ligatures: their parts' values in order.
    if (name.find('_') != std::string_view::npos) {
        std::u32string out;
        std::size_t start = 0;
        while (start <= name.size()) {
            std::size_t const end = std::min(name.find('_', start), name.size());
            out += glyph_name_unicode(name.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }
    std::span<GlyphUnicode const> const table = glyph_unicode_table();
    auto const it = std::lower_bound(table.begin(), table.end(), name,
        [](GlyphUnicode const& entry, std::string_view key) { return std::string_view(entry.name) < key; });
    if (it != table.end() && std::string_view(it->name) == name)
        return std::u32string(1, it->unicode);
    // uniXXXX (one or more groups of four), uXXXX to uXXXXXX.
    if (name.size() >= 7 && name.substr(0, 3) == "uni" && (name.size() - 3) % 4 == 0) {
        std::u32string out;
        for (std::size_t i = 3; i < name.size(); i += 4) {
            std::optional<char32_t> const v = hex_value(name.substr(i, 4));
            if (!v)
                return {};
            out += *v;
        }
        return out;
    }
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u') {
        if (std::optional<char32_t> const v = hex_value(name.substr(1)))
            return std::u32string(1, *v);
    }
    // The afii Cyrillic names: 10017-10049 capitals, 10065-10097 small, Yo out of order.
    if (name.size() == 9 && name.substr(0, 4) == "afii") {
        int number = 0;
        for (char const c : name.substr(4)) {
            if (c < '0' || c > '9')
                return {};
            number = number * 10 + (c - '0');
        }
        if (number == 10023)
            return U"Ё";
        if (number == 10071)
            return U"ё";
        if (number >= 10017 && number <= 10049) {
            int offset = number - 10017;
            if (number > 10023)
                --offset;
            return std::u32string(1, static_cast<char32_t>(0x410 + offset));
        }
        if (number >= 10065 && number <= 10097) {
            int offset = number - 10065;
            if (number > 10071)
                --offset;
            return std::u32string(1, static_cast<char32_t>(0x430 + offset));
        }
    }
    return {};
}

std::optional<int> code_in(std::array<char const*, 256> const& encoding, std::string_view name)
{
    for (int code = 0; code < 256; ++code) {
        if (encoding[static_cast<std::size_t>(code)] && name == encoding[static_cast<std::size_t>(code)])
            return code;
    }
    return std::nullopt;
}

char const* mac_glyph_name(int index)
{
    if (index < 0 || index >= 258)
        return nullptr;
    return encoding_data::k_mac_glyph_names[static_cast<std::size_t>(index)];
}

}
