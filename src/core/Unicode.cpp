#include "core/Unicode.h"

namespace sashfold {

void append_utf8(std::string& out, char32_t c)
{
    if (c > 0x10FFFF)
        c = replacement_character;

    if (c <= 0x7F) {
        out.push_back(static_cast<char>(c));
    } else if (c <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

std::string to_utf8(std::u32string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char32_t c : text)
        append_utf8(out, c);
    return out;
}

std::u32string decode_utf8(std::string_view bytes, bool permit_surrogates)
{
    std::u32string out;
    out.reserve(bytes.size());

    std::size_t i = 0;
    while (i < bytes.size()) {
        unsigned char const lead = static_cast<unsigned char>(bytes[i]);

        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }

        int continuation_count = 0;
        char32_t code_point = 0;
        char32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            continuation_count = 1;
            code_point = lead & 0x1Fu;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            continuation_count = 2;
            code_point = lead & 0x0Fu;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            continuation_count = 3;
            code_point = lead & 0x07u;
            minimum = 0x10000;
        } else {
            out.push_back(replacement_character);
            ++i;
            continue;
        }

        bool ok = true;
        std::size_t j = i + 1;
        for (int k = 0; k < continuation_count; ++k, ++j) {
            if (j >= bytes.size() || (static_cast<unsigned char>(bytes[j]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            code_point = (code_point << 6) | (static_cast<unsigned char>(bytes[j]) & 0x3Fu);
        }

        if (ok && (code_point < minimum || code_point > 0x10FFFF))
            ok = false; // overlong or out of range
        if (ok && !permit_surrogates && is_surrogate(code_point))
            ok = false;

        if (ok) {
            out.push_back(code_point);
            i = j;
        } else {
            out.push_back(replacement_character);
            ++i; // resync on the next byte
        }
    }
    return out;
}

}
