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

char32_t to_math_italic(char32_t c)
{
    // MathML Core's italic mappings, in its order: the Latin letters, the
    // dotless i and j, the Greek capitals with the theta symbol and nabla,
    // the Greek smalls with the partial differential and the symbol forms.
    struct Mapping {
        char32_t from;
        char32_t to;
    };
    static constexpr Mapping const mappings[] = {
        { 0x41, 0x1D434 }, { 0x42, 0x1D435 }, { 0x43, 0x1D436 }, { 0x44, 0x1D437 },
        { 0x45, 0x1D438 }, { 0x46, 0x1D439 }, { 0x47, 0x1D43A }, { 0x48, 0x1D43B },
        { 0x49, 0x1D43C }, { 0x4A, 0x1D43D }, { 0x4B, 0x1D43E }, { 0x4C, 0x1D43F },
        { 0x4D, 0x1D440 }, { 0x4E, 0x1D441 }, { 0x4F, 0x1D442 }, { 0x50, 0x1D443 },
        { 0x51, 0x1D444 }, { 0x52, 0x1D445 }, { 0x53, 0x1D446 }, { 0x54, 0x1D447 },
        { 0x55, 0x1D448 }, { 0x56, 0x1D449 }, { 0x57, 0x1D44A }, { 0x58, 0x1D44B },
        { 0x59, 0x1D44C }, { 0x5A, 0x1D44D }, { 0x61, 0x1D44E }, { 0x62, 0x1D44F },
        { 0x63, 0x1D450 }, { 0x64, 0x1D451 }, { 0x65, 0x1D452 }, { 0x66, 0x1D453 },
        { 0x67, 0x1D454 }, { 0x68, 0x210E }, { 0x69, 0x1D456 }, { 0x6A, 0x1D457 },
        { 0x6B, 0x1D458 }, { 0x6C, 0x1D459 }, { 0x6D, 0x1D45A }, { 0x6E, 0x1D45B },
        { 0x6F, 0x1D45C }, { 0x70, 0x1D45D }, { 0x71, 0x1D45E }, { 0x72, 0x1D45F },
        { 0x73, 0x1D460 }, { 0x74, 0x1D461 }, { 0x75, 0x1D462 }, { 0x76, 0x1D463 },
        { 0x77, 0x1D464 }, { 0x78, 0x1D465 }, { 0x79, 0x1D466 }, { 0x7A, 0x1D467 },
        { 0x131, 0x1D6A4 }, { 0x237, 0x1D6A5 }, { 0x391, 0x1D6E2 }, { 0x392, 0x1D6E3 },
        { 0x393, 0x1D6E4 }, { 0x394, 0x1D6E5 }, { 0x395, 0x1D6E6 }, { 0x396, 0x1D6E7 },
        { 0x397, 0x1D6E8 }, { 0x398, 0x1D6E9 }, { 0x399, 0x1D6EA }, { 0x39A, 0x1D6EB },
        { 0x39B, 0x1D6EC }, { 0x39C, 0x1D6ED }, { 0x39D, 0x1D6EE }, { 0x39E, 0x1D6EF },
        { 0x39F, 0x1D6F0 }, { 0x3A0, 0x1D6F1 }, { 0x3A1, 0x1D6F2 }, { 0x3F4, 0x1D6F3 },
        { 0x3A3, 0x1D6F4 }, { 0x3A4, 0x1D6F5 }, { 0x3A5, 0x1D6F6 }, { 0x3A6, 0x1D6F7 },
        { 0x3A7, 0x1D6F8 }, { 0x3A8, 0x1D6F9 }, { 0x3A9, 0x1D6FA }, { 0x2207, 0x1D6FB },
        { 0x3B1, 0x1D6FC }, { 0x3B2, 0x1D6FD }, { 0x3B3, 0x1D6FE }, { 0x3B4, 0x1D6FF },
        { 0x3B5, 0x1D700 }, { 0x3B6, 0x1D701 }, { 0x3B7, 0x1D702 }, { 0x3B8, 0x1D703 },
        { 0x3B9, 0x1D704 }, { 0x3BA, 0x1D705 }, { 0x3BB, 0x1D706 }, { 0x3BC, 0x1D707 },
        { 0x3BD, 0x1D708 }, { 0x3BE, 0x1D709 }, { 0x3BF, 0x1D70A }, { 0x3C0, 0x1D70B },
        { 0x3C1, 0x1D70C }, { 0x3C2, 0x1D70D }, { 0x3C3, 0x1D70E }, { 0x3C4, 0x1D70F },
        { 0x3C5, 0x1D710 }, { 0x3C6, 0x1D711 }, { 0x3C7, 0x1D712 }, { 0x3C8, 0x1D713 },
        { 0x3C9, 0x1D714 }, { 0x2202, 0x1D715 }, { 0x3F5, 0x1D716 }, { 0x3D1, 0x1D717 },
        { 0x3F0, 0x1D718 }, { 0x3D5, 0x1D719 }, { 0x3F1, 0x1D71A }, { 0x3D6, 0x1D71B },
    };
    for (Mapping const& mapping : mappings) {
        if (mapping.from == c)
            return mapping.to;
    }
    return c;
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
