#include "net/Idna.h"

#include "core/Unicode.h"
#include "net/IdnaData.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>

namespace sashfold::net {

// --- Punycode (RFC 3492) ------------------------------------------------------

namespace {

constexpr std::uint32_t punycode_base = 36;
constexpr std::uint32_t punycode_tmin = 1;
constexpr std::uint32_t punycode_tmax = 26;
constexpr std::uint32_t punycode_skew = 38;
constexpr std::uint32_t punycode_damp = 700;
constexpr std::uint32_t punycode_initial_bias = 72;
constexpr std::uint32_t punycode_initial_n = 128;

std::uint32_t punycode_adapt(std::uint32_t delta, std::uint32_t num_points, bool first_time)
{
    delta = first_time ? delta / punycode_damp : delta / 2;
    delta += delta / num_points;
    std::uint32_t k = 0;
    while (delta > ((punycode_base - punycode_tmin) * punycode_tmax) / 2) {
        delta /= punycode_base - punycode_tmin;
        k += punycode_base;
    }
    return k + (((punycode_base - punycode_tmin + 1) * delta) / (delta + punycode_skew));
}

char punycode_digit(std::uint32_t d)
{
    return d < 26 ? static_cast<char>('a' + d) : static_cast<char>('0' + d - 26);
}

// 'a'-'z'/'A'-'Z' -> 0-25, '0'-'9' -> 26-35, anything else -> >=36.
std::uint32_t punycode_digit_value(char c)
{
    if (c >= 'a' && c <= 'z')
        return static_cast<std::uint32_t>(c - 'a');
    if (c >= 'A' && c <= 'Z')
        return static_cast<std::uint32_t>(c - 'A');
    if (c >= '0' && c <= '9')
        return static_cast<std::uint32_t>(c - '0') + 26;
    return punycode_base;
}

}

std::optional<std::string> punycode_encode(std::u32string_view label)
{
    std::string output;
    std::uint32_t handled = 0;
    for (char32_t const c : label) {
        if (c < 0x80) {
            output += static_cast<char>(c);
            ++handled;
        }
    }
    std::uint32_t const basic_count = handled;
    if (basic_count > 0)
        output += '-';

    std::uint32_t n = punycode_initial_n;
    std::uint32_t delta = 0;
    std::uint32_t bias = punycode_initial_bias;
    while (handled < label.size()) {
        char32_t minimum = 0x10FFFF;
        for (char32_t const c : label) {
            if (c >= n && c < minimum)
                minimum = c;
        }
        std::uint64_t const advance = static_cast<std::uint64_t>(minimum - n)
            * static_cast<std::uint64_t>(handled + 1);
        if (delta + advance > 0xFFFFFFFFu)
            return std::nullopt; // overflow
        delta += static_cast<std::uint32_t>(advance);
        n = minimum;
        for (char32_t const c : label) {
            if (c < n) {
                if (++delta == 0)
                    return std::nullopt; // overflow
            }
            if (c == n) {
                std::uint32_t q = delta;
                for (std::uint32_t k = punycode_base;; k += punycode_base) {
                    std::uint32_t const threshold = k <= bias ? punycode_tmin
                        : k >= bias + punycode_tmax             ? punycode_tmax
                                                                : k - bias;
                    if (q < threshold)
                        break;
                    output += punycode_digit(threshold + (q - threshold) % (punycode_base - threshold));
                    q = (q - threshold) / (punycode_base - threshold);
                }
                output += punycode_digit(q);
                bias = punycode_adapt(delta, handled + 1, handled == basic_count);
                delta = 0;
                ++handled;
            }
        }
        ++delta;
        ++n;
    }
    return output;
}

std::optional<std::u32string> punycode_decode(std::string_view encoded)
{
    constexpr std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
    std::u32string output;
    std::size_t position = 0;
    std::size_t const delimiter = encoded.rfind('-');
    if (delimiter != std::string_view::npos) {
        for (std::size_t i = 0; i < delimiter; ++i) {
            char const c = encoded[i];
            if (static_cast<unsigned char>(c) >= 0x80)
                return std::nullopt;
            output += static_cast<char32_t>(c);
        }
        position = delimiter + 1;
    }

    std::uint32_t n = punycode_initial_n;
    std::uint32_t i = 0;
    std::uint32_t bias = punycode_initial_bias;
    while (position < encoded.size()) {
        std::uint32_t const old_i = i;
        std::uint32_t w = 1;
        for (std::uint32_t k = punycode_base;; k += punycode_base) {
            if (position >= encoded.size())
                return std::nullopt; // truncated variable-length integer
            std::uint32_t const digit = punycode_digit_value(encoded[position++]);
            if (digit >= punycode_base)
                return std::nullopt;
            if (digit > (max - i) / w)
                return std::nullopt; // overflow
            i += digit * w;
            std::uint32_t const threshold = k <= bias ? punycode_tmin
                : k >= bias + punycode_tmax             ? punycode_tmax
                                                        : k - bias;
            if (digit < threshold)
                break;
            if (w > max / (punycode_base - threshold))
                return std::nullopt; // overflow
            w *= punycode_base - threshold;
        }
        std::uint32_t const length = static_cast<std::uint32_t>(output.size()) + 1;
        bias = punycode_adapt(i - old_i, length, old_i == 0);
        if (i / length > max - n)
            return std::nullopt; // overflow
        n += i / length;
        i %= length;
        if (n > 0x10FFFF || is_surrogate(n))
            return std::nullopt;
        output.insert(output.begin() + static_cast<std::ptrdiff_t>(i), static_cast<char32_t>(n));
        ++i;
    }
    return output;
}

// --- UTS #46 ------------------------------------------------------------------

namespace {

IdnaRange const* find_idna_range(char32_t code_point)
{
    auto const it = std::upper_bound(std::begin(idna_ranges), std::end(idna_ranges), code_point,
        [](char32_t value, IdnaRange const& range) { return value < range.first; });
    if (it == std::begin(idna_ranges))
        return nullptr;
    IdnaRange const& range = *std::prev(it);
    return code_point <= range.last ? &range : nullptr;
}

// The UTS #46 §4.1 validity criteria under our parameters, applied to one
// label already in NFC: no leading "xn--" (the criterion that replaces the
// hyphen rules when CheckHyphens=false), no leading combining mark, every
// code point valid. (CheckBidi/CheckJoiners are the documented gap in Idna.h.)
bool label_is_valid(std::u32string_view label)
{
    if (label.starts_with(U"xn--"))
        return false;
    if (!label.empty() && is_combining_mark(label[0]))
        return false;
    for (char32_t const code_point : label) {
        IdnaRange const* const range = find_idna_range(code_point);
        if (!range || range->status != IdnaStatus::Valid)
            return false;
    }
    return true;
}

}

std::optional<std::string> domain_to_ascii(std::string_view domain_utf8)
{
    // The WHATWG "domain to ASCII" fast path (beStrict=false): an all-ASCII
    // domain never reaches Unicode ToASCII — it is simply lowercased, for web
    // compatibility (xn-- labels survive as typed even when their decoded
    // form would fail the validity criteria). The forbidden-code-point check
    // stays with the host parser.
    if (std::all_of(domain_utf8.begin(), domain_utf8.end(),
            [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
        std::string lowered(domain_utf8);
        for (char& c : lowered)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return lowered;
    }

    // 1. Map (the table already carries our parameter choices).
    std::u32string mapped;
    for (char32_t const code_point : decode_utf8(domain_utf8)) {
        IdnaRange const* const range = find_idna_range(code_point);
        if (!range || range->status == IdnaStatus::Disallowed)
            return std::nullopt;
        switch (range->status) {
        case IdnaStatus::Valid:
            mapped += code_point;
            break;
        case IdnaStatus::Ignored:
            break;
        case IdnaStatus::Mapped:
            mapped.append(idna_mapping_pool + range->mapping_offset, range->mapping_length);
            break;
        case IdnaStatus::Disallowed:
            break; // unreachable, handled above
        }
    }

    // 2. Normalize.
    std::u32string const normalized = nfc(mapped);

    // 3-4. Break at U+002E, validate each label, convert.
    std::string result;
    std::size_t label_start = 0;
    bool first = true;
    for (std::size_t i = 0; i <= normalized.size(); ++i) {
        if (i != normalized.size() && normalized[i] != U'.')
            continue;
        std::u32string_view const label
            = std::u32string_view(normalized).substr(label_start, i - label_start);
        label_start = i + 1;
        if (!first)
            result += '.';
        first = false;

        bool const ascii
            = std::all_of(label.begin(), label.end(), [](char32_t c) { return c < 0x80; });
        if (ascii && label.starts_with(U"xn--")) {
            // Punycode labels must decode, and the decoded form must satisfy
            // the validity criteria; the ASCII form is what the result keeps.
            std::string ascii_label;
            for (char32_t const c : label)
                ascii_label += static_cast<char>(c);
            std::optional<std::u32string> const decoded
                = punycode_decode(std::string_view(ascii_label).substr(4));
            if (!decoded || nfc(*decoded) != *decoded || !label_is_valid(*decoded))
                return std::nullopt;
            result += ascii_label;
        } else if (ascii) {
            for (char32_t const c : label)
                result += static_cast<char>(c);
        } else {
            if (!label_is_valid(label))
                return std::nullopt;
            std::optional<std::string> const encoded = punycode_encode(label);
            if (!encoded)
                return std::nullopt;
            result += "xn--" + *encoded;
        }
    }
    return result;
}

}
