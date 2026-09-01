#include "html/Encoding.h"

#include "core/Unicode.h"

#include <algorithm>
#include <array>
#include <vector>

namespace sashfold::html {

namespace {

bool is_ascii_whitespace_byte(unsigned char byte)
{
    return byte == 0x09 || byte == 0x0A || byte == 0x0C || byte == 0x0D || byte == 0x20;
}

char to_lower_byte(unsigned char byte)
{
    return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + 0x20 : byte);
}

struct LabelEntry {
    std::string_view label;
    Encoding encoding;
};

// The Encoding standard's labels for the encodings we implement.
constexpr std::array<LabelEntry, 32> label_table { {
    { "ansi_x3.4-1968", Encoding::Windows1252 },
    { "ascii", Encoding::Windows1252 },
    { "cp1252", Encoding::Windows1252 },
    { "cp819", Encoding::Windows1252 },
    { "csisolatin1", Encoding::Windows1252 },
    { "csunicode", Encoding::Utf16Le },
    { "ibm819", Encoding::Windows1252 },
    { "iso-10646-ucs-2", Encoding::Utf16Le },
    { "iso-8859-1", Encoding::Windows1252 },
    { "iso-ir-100", Encoding::Windows1252 },
    { "iso8859-1", Encoding::Windows1252 },
    { "iso88591", Encoding::Windows1252 },
    { "iso_8859-1", Encoding::Windows1252 },
    { "iso_8859-1:1987", Encoding::Windows1252 },
    { "l1", Encoding::Windows1252 },
    { "latin1", Encoding::Windows1252 },
    { "ucs-2", Encoding::Utf16Le },
    { "unicode", Encoding::Utf16Le },
    { "unicode-1-1-utf-8", Encoding::Utf8 },
    { "unicode11utf8", Encoding::Utf8 },
    { "unicode20utf8", Encoding::Utf8 },
    { "unicodefeff", Encoding::Utf16Le },
    { "unicodefffe", Encoding::Utf16Be },
    { "us-ascii", Encoding::Windows1252 },
    { "utf-16", Encoding::Utf16Le },
    { "utf-16be", Encoding::Utf16Be },
    { "utf-16le", Encoding::Utf16Le },
    { "utf-8", Encoding::Utf8 },
    { "utf8", Encoding::Utf8 },
    { "windows-1252", Encoding::Windows1252 },
    { "x-cp1252", Encoding::Windows1252 },
    { "x-unicode20utf8", Encoding::Utf8 },
} };

// windows-1252, bytes 0x80-0x9F (the rest is identity to Unicode).
constexpr std::array<char16_t, 32> windows_1252_high {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

// The prescan's "get an attribute". Returns false when there is no further
// attribute; leaves `position` on the byte that ended the scan.
bool prescan_get_attribute(std::string_view bytes, std::size_t& position,
    std::string& name, std::string& value)
{
    name.clear();
    value.clear();

    while (position < bytes.size()
        && (is_ascii_whitespace_byte(static_cast<unsigned char>(bytes[position])) || bytes[position] == '/'))
        ++position;
    if (position >= bytes.size() || bytes[position] == '>')
        return false;

    // Attribute name.
    while (position < bytes.size()) {
        char const byte = bytes[position];
        if (byte == '=' && !name.empty()) {
            ++position;
            break;
        }
        if (is_ascii_whitespace_byte(static_cast<unsigned char>(byte))) {
            // Spaces between name and a possible "=".
            while (position < bytes.size()
                && is_ascii_whitespace_byte(static_cast<unsigned char>(bytes[position])))
                ++position;
            if (position >= bytes.size() || bytes[position] != '=')
                return true; // value-less attribute
            ++position;
            break;
        }
        if (byte == '/' || byte == '>')
            return true; // value-less attribute
        name += to_lower_byte(static_cast<unsigned char>(byte));
        ++position;
    }
    if (position >= bytes.size())
        return true;

    while (position < bytes.size()
        && is_ascii_whitespace_byte(static_cast<unsigned char>(bytes[position])))
        ++position;
    if (position >= bytes.size())
        return true;

    // Attribute value.
    if (bytes[position] == '"' || bytes[position] == '\'') {
        char const quote = bytes[position++];
        while (position < bytes.size()) {
            char const byte = bytes[position++];
            if (byte == quote)
                return true;
            value += to_lower_byte(static_cast<unsigned char>(byte));
        }
        return true; // unterminated: processing will stop at the 1024-byte cap anyway
    }
    while (position < bytes.size()) {
        char const byte = bytes[position];
        if (is_ascii_whitespace_byte(static_cast<unsigned char>(byte)) || byte == '>')
            return true;
        value += to_lower_byte(static_cast<unsigned char>(byte));
        ++position;
    }
    return true;
}

// Spec "algorithm for extracting a character encoding from a meta element":
// finds charset=... inside a content="text/html; charset=..." style value.
std::optional<std::string> extract_charset_from_content(std::string_view content)
{
    std::size_t search = 0;
    while (true) {
        std::size_t const found = content.find("charset", search);
        if (found == std::string_view::npos)
            return std::nullopt;
        std::size_t position = found + 7;
        while (position < content.size()
            && is_ascii_whitespace_byte(static_cast<unsigned char>(content[position])))
            ++position;
        if (position >= content.size() || content[position] != '=') {
            search = position;
            continue;
        }
        ++position;
        while (position < content.size()
            && is_ascii_whitespace_byte(static_cast<unsigned char>(content[position])))
            ++position;
        if (position >= content.size())
            return std::nullopt;
        if (content[position] == '"' || content[position] == '\'') {
            char const quote = content[position++];
            std::size_t const end = content.find(quote, position);
            if (end == std::string_view::npos)
                return std::nullopt;
            return std::string(content.substr(position, end - position));
        }
        std::size_t end = position;
        while (end < content.size()
            && !is_ascii_whitespace_byte(static_cast<unsigned char>(content[end]))
            && content[end] != ';')
            ++end;
        return std::string(content.substr(position, end - position));
    }
}

bool starts_with_ci(std::string_view bytes, std::size_t position, std::string_view lowercase_needle)
{
    if (position + lowercase_needle.size() > bytes.size())
        return false;
    for (std::size_t i = 0; i < lowercase_needle.size(); ++i) {
        if (to_lower_byte(static_cast<unsigned char>(bytes[position + i])) != lowercase_needle[i])
            return false;
    }
    return true;
}

bool is_valid_utf8(std::string_view bytes)
{
    std::size_t i = 0;
    while (i < bytes.size()) {
        unsigned char const lead = static_cast<unsigned char>(bytes[i]);
        int continuations = 0;
        char32_t code_point = 0;
        char32_t minimum = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            continuations = 1;
            code_point = lead & 0x1Fu;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            continuations = 2;
            code_point = lead & 0x0Fu;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            continuations = 3;
            code_point = lead & 0x07u;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(continuations) >= bytes.size())
            return false;
        for (int k = 1; k <= continuations; ++k) {
            unsigned char const byte = static_cast<unsigned char>(bytes[i + static_cast<std::size_t>(k)]);
            if ((byte & 0xC0) != 0x80)
                return false;
            code_point = (code_point << 6) | (byte & 0x3Fu);
        }
        if (code_point < minimum || code_point > 0x10FFFF
            || (code_point >= 0xD800 && code_point <= 0xDFFF))
            return false;
        i += 1 + static_cast<std::size_t>(continuations);
    }
    return true;
}

std::u32string decode_utf16(std::string_view bytes, bool big_endian)
{
    std::u32string out;
    out.reserve(bytes.size() / 2);
    std::size_t const pairs = bytes.size() / 2;
    std::size_t i = 0;
    while (i < pairs) {
        unsigned char const first = static_cast<unsigned char>(bytes[2 * i]);
        unsigned char const second = static_cast<unsigned char>(bytes[2 * i + 1]);
        char32_t unit = big_endian ? (static_cast<char32_t>(first) << 8) | second
                                   : (static_cast<char32_t>(second) << 8) | first;
        ++i;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i < pairs) {
                unsigned char const third = static_cast<unsigned char>(bytes[2 * i]);
                unsigned char const fourth = static_cast<unsigned char>(bytes[2 * i + 1]);
                char32_t const low = big_endian ? (static_cast<char32_t>(third) << 8) | fourth
                                                : (static_cast<char32_t>(fourth) << 8) | third;
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    ++i;
                    out.push_back(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    continue;
                }
            }
            out.push_back(replacement_character);
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            out.push_back(replacement_character);
            continue;
        }
        out.push_back(unit);
    }
    if (bytes.size() % 2 != 0)
        out.push_back(replacement_character);
    return out;
}

std::u32string decode_windows_1252(std::string_view bytes)
{
    std::u32string out;
    out.reserve(bytes.size());
    for (char const c : bytes) {
        unsigned char const byte = static_cast<unsigned char>(c);
        if (byte >= 0x80 && byte <= 0x9F)
            out.push_back(windows_1252_high[byte - 0x80]);
        else
            out.push_back(byte);
    }
    return out;
}

} // namespace

std::optional<Encoding> encoding_from_label(std::string_view label)
{
    while (!label.empty() && is_ascii_whitespace_byte(static_cast<unsigned char>(label.front())))
        label.remove_prefix(1);
    while (!label.empty() && is_ascii_whitespace_byte(static_cast<unsigned char>(label.back())))
        label.remove_suffix(1);
    std::string lowered;
    lowered.reserve(label.size());
    for (char const c : label)
        lowered += to_lower_byte(static_cast<unsigned char>(c));
    for (LabelEntry const& entry : label_table) {
        if (entry.label == lowered)
            return entry.encoding;
    }
    if (lowered == "x-user-defined")
        return Encoding::XUserDefined;
    return std::nullopt;
}

std::optional<Encoding> prescan_for_encoding(std::string_view bytes)
{
    if (bytes.size() > 1024)
        bytes = bytes.substr(0, 1024);

    std::size_t position = 0;
    while (position < bytes.size()) {
        if (starts_with_ci(bytes, position, "<!--")) {
            std::size_t const end = bytes.find("-->", position + 4);
            if (end == std::string_view::npos)
                return std::nullopt;
            position = end + 3;
            continue;
        }
        if (starts_with_ci(bytes, position, "<meta")
            && position + 5 < bytes.size()
            && (is_ascii_whitespace_byte(static_cast<unsigned char>(bytes[position + 5]))
                || bytes[position + 5] == '/')) {
            position += 5;
            bool got_pragma = false;
            std::optional<bool> need_pragma;
            std::optional<Encoding> charset;
            std::string name;
            std::string value;
            std::vector<std::string> seen;
            while (prescan_get_attribute(bytes, position, name, value)) {
                if (name.empty())
                    continue;
                if (std::find(seen.begin(), seen.end(), name) != seen.end())
                    continue;
                seen.push_back(name);
                if (name == "http-equiv") {
                    if (value == "content-type")
                        got_pragma = true;
                } else if (name == "content") {
                    if (!charset) {
                        if (std::optional<std::string> extracted = extract_charset_from_content(value)) {
                            charset = encoding_from_label(*extracted);
                            if (charset)
                                need_pragma = true;
                        }
                    }
                } else if (name == "charset") {
                    charset = encoding_from_label(value);
                    need_pragma = false;
                }
            }
            if (!need_pragma || (*need_pragma && !got_pragma) || !charset) {
                ++position;
                continue;
            }
            if (*charset == Encoding::Utf16Le || *charset == Encoding::Utf16Be)
                return Encoding::Utf8; // a 16-bit family label in ASCII-compatible bytes
            if (*charset == Encoding::XUserDefined)
                return Encoding::Windows1252;
            return charset;
        }
        if (position + 1 < bytes.size() && bytes[position] == '<'
            && (((bytes[position + 1] | 0x20) >= 'a' && (bytes[position + 1] | 0x20) <= 'z')
                || (bytes[position + 1] == '/' && position + 2 < bytes.size()
                    && (bytes[position + 2] | 0x20) >= 'a' && (bytes[position + 2] | 0x20) <= 'z'))) {
            // A tag: skip to the first whitespace or ">", then drain attributes.
            while (position < bytes.size()
                && !is_ascii_whitespace_byte(static_cast<unsigned char>(bytes[position]))
                && bytes[position] != '>')
                ++position;
            std::string name;
            std::string value;
            while (prescan_get_attribute(bytes, position, name, value)) { }
            ++position;
            continue;
        }
        if (position + 1 < bytes.size() && bytes[position] == '<'
            && (bytes[position + 1] == '!' || bytes[position + 1] == '/' || bytes[position + 1] == '?')) {
            std::size_t const end = bytes.find('>', position + 2);
            if (end == std::string_view::npos)
                return std::nullopt;
            position = end + 1;
            continue;
        }
        ++position;
    }
    return std::nullopt;
}

SniffedEncoding sniff_encoding(std::string_view bytes)
{
    auto const byte_at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 3 && byte_at(0) == 0xEF && byte_at(1) == 0xBB && byte_at(2) == 0xBF)
        return { Encoding::Utf8, 3 };
    if (bytes.size() >= 2 && byte_at(0) == 0xFE && byte_at(1) == 0xFF)
        return { Encoding::Utf16Be, 2 };
    if (bytes.size() >= 2 && byte_at(0) == 0xFF && byte_at(1) == 0xFE)
        return { Encoding::Utf16Le, 2 };

    if (std::optional<Encoding> prescanned = prescan_for_encoding(bytes))
        return { *prescanned, 0 };

    if (is_valid_utf8(bytes))
        return { Encoding::Utf8, 0 };
    return { Encoding::Windows1252, 0 };
}

std::u32string decode(std::string_view bytes, Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8:
        return decode_utf8(bytes);
    case Encoding::Utf16Le:
        return decode_utf16(bytes, false);
    case Encoding::Utf16Be:
        return decode_utf16(bytes, true);
    case Encoding::Windows1252:
    case Encoding::XUserDefined:
        return decode_windows_1252(bytes);
    }
    return {};
}

std::u32string decode_document_bytes(std::string_view bytes)
{
    SniffedEncoding const sniffed = sniff_encoding(bytes);
    return decode(bytes.substr(sniffed.bom_length), sniffed.encoding);
}

}
