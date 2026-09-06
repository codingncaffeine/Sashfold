#include "net/tls/Der.h"

namespace sashfold::tls {

std::optional<DerTag> DerReader::peek_tag() const
{
    if (m_offset >= m_bytes.size())
        return std::nullopt;
    std::uint8_t const first = m_bytes[m_offset];
    DerTag tag;
    tag.cls = static_cast<DerClass>(first >> 6);
    tag.constructed = (first & 0x20) != 0;
    tag.number = static_cast<std::uint8_t>(first & 0x1f);
    if (tag.number == 0x1f)
        return std::nullopt; // high tag numbers: nothing in X.509 has one
    return tag;
}

std::optional<DerElement> DerReader::next()
{
    std::optional<DerTag> const tag = peek_tag();
    if (!tag)
        return std::nullopt;
    std::size_t const start = m_offset;
    std::size_t offset = m_offset + 1;
    if (offset >= m_bytes.size())
        return std::nullopt;
    std::uint8_t const first_length = m_bytes[offset++];
    std::size_t length = 0;
    if (first_length < 0x80) {
        length = first_length;
    } else {
        // X.690 §8.1.3 and §10.1: the long form only for lengths past 127,
        // in the fewest bytes, never indefinite.
        std::uint8_t const count = static_cast<std::uint8_t>(first_length & 0x7f);
        if (count == 0 || count > 4 || offset + count > m_bytes.size())
            return std::nullopt;
        if (m_bytes[offset] == 0)
            return std::nullopt;
        for (std::uint8_t i = 0; i < count; ++i)
            length = (length << 8) | m_bytes[offset++];
        if (length < 0x80)
            return std::nullopt;
    }
    if (length > m_bytes.size() - offset)
        return std::nullopt;
    DerElement element;
    element.tag = *tag;
    element.content = m_bytes.subspan(offset, length);
    element.whole = m_bytes.subspan(start, offset + length - start);
    m_offset = offset + length;
    return element;
}

std::optional<DerElement> DerReader::next(DerType type)
{
    std::optional<DerTag> const tag = peek_tag();
    if (!tag || !tag->is(type))
        return std::nullopt;
    return next();
}

std::optional<DerElement> DerReader::next_context(std::uint8_t number)
{
    if (!peek_context(number))
        return std::nullopt;
    return next();
}

bool DerReader::peek_context(std::uint8_t number) const
{
    std::optional<DerTag> const tag = peek_tag();
    return tag && tag->is_context(number);
}

std::optional<bool> der_boolean(DerElement const& element)
{
    if (!element.tag.is(DerType::Boolean) || element.content.size() != 1)
        return std::nullopt;
    if (element.content[0] == 0x00)
        return false;
    if (element.content[0] == 0xff)
        return true;
    return std::nullopt; // §11.1: DER writes TRUE as 0xff only
}

std::optional<std::span<std::uint8_t const>> der_integer(DerElement const& element)
{
    if (!element.tag.is(DerType::Integer) || element.content.empty())
        return std::nullopt;
    std::span<std::uint8_t const> bytes = element.content;
    if (bytes[0] & 0x80)
        return std::nullopt; // negative: no serial, modulus or exponent is
    if (bytes.size() > 1 && bytes[0] == 0 && (bytes[1] & 0x80) == 0)
        return std::nullopt; // §8.3.2: a leading zero only to keep the sign
    if (bytes[0] == 0 && bytes.size() > 1)
        bytes = bytes.subspan(1);
    return bytes;
}

std::optional<std::uint64_t> der_small_integer(DerElement const& element)
{
    std::optional<std::span<std::uint8_t const>> const bytes = der_integer(element);
    if (!bytes || bytes->size() > 8)
        return std::nullopt;
    std::uint64_t value = 0;
    for (std::uint8_t const b : *bytes)
        value = (value << 8) | b;
    return value;
}

std::optional<std::string> der_oid(DerElement const& element)
{
    if (!element.tag.is(DerType::Oid) || element.content.empty())
        return std::nullopt;
    std::string out;
    std::span<std::uint8_t const> const bytes = element.content;
    std::size_t i = 0;
    bool first = true;
    while (i < bytes.size()) {
        if (bytes[i] == 0x80)
            return std::nullopt; // §8.19.2: no leading zero septets
        std::uint64_t value = 0;
        std::size_t count = 0;
        for (;;) {
            if (i >= bytes.size() || count == 5)
                return std::nullopt;
            std::uint8_t const b = bytes[i++];
            ++count;
            value = (value << 7) | (b & 0x7f);
            if ((b & 0x80) == 0)
                break;
        }
        if (first) {
            // The first two arcs share one number: 40·x + y, x ≤ 2.
            std::uint64_t const x = value < 80 ? value / 40 : 2;
            std::uint64_t const y = value < 80 ? value % 40 : value - 80;
            out += std::to_string(x) + "." + std::to_string(y);
            first = false;
        } else {
            out += "." + std::to_string(value);
        }
    }
    return out;
}

std::optional<std::pair<std::uint8_t, std::span<std::uint8_t const>>> der_bit_string_with_unused(DerElement const& element)
{
    if (!element.tag.is(DerType::BitString) || element.content.empty())
        return std::nullopt;
    std::uint8_t const unused = element.content[0];
    if (unused > 7)
        return std::nullopt;
    std::span<std::uint8_t const> const bits = element.content.subspan(1);
    if (bits.empty() && unused != 0)
        return std::nullopt;
    if (!bits.empty() && (bits.back() & ((1u << unused) - 1)) != 0)
        return std::nullopt; // §11.2.1: unused bits are zero
    return std::make_pair(unused, bits);
}

std::optional<std::span<std::uint8_t const>> der_bit_string(DerElement const& element)
{
    auto const with_unused = der_bit_string_with_unused(element);
    if (!with_unused || with_unused->first != 0)
        return std::nullopt;
    return with_unused->second;
}

namespace {

void append_utf8(std::string& out, std::uint32_t code_point)
{
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xc0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xe0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    }
}

}

std::optional<std::string> der_string(DerElement const& element)
{
    if (element.tag.cls != DerClass::Universal)
        return std::nullopt;
    std::span<std::uint8_t const> const bytes = element.content;
    switch (static_cast<DerType>(element.tag.number)) {
    case DerType::Utf8String:
        return std::string(bytes.begin(), bytes.end());
    case DerType::PrintableString:
    case DerType::Ia5String:
    case DerType::VisibleString:
        for (std::uint8_t const b : bytes) {
            if (b >= 0x80 || b == 0)
                return std::nullopt;
        }
        return std::string(bytes.begin(), bytes.end());
    case DerType::T61String: {
        std::string out;
        for (std::uint8_t const b : bytes)
            append_utf8(out, b);
        return out;
    }
    case DerType::BmpString: {
        if (bytes.size() % 2 != 0)
            return std::nullopt;
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); i += 2) {
            std::uint32_t unit = (std::uint32_t(bytes[i]) << 8) | bytes[i + 1];
            if (unit >= 0xd800 && unit < 0xdc00 && i + 3 < bytes.size()) {
                std::uint32_t const low = (std::uint32_t(bytes[i + 2]) << 8) | bytes[i + 3];
                if (low >= 0xdc00 && low < 0xe000) {
                    unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                    i += 2;
                }
            }
            append_utf8(out, unit);
        }
        return out;
    }
    default:
        return std::nullopt;
    }
}

std::int64_t seconds_from_civil(int year, int month, int day, int hour, int minute, int second)
{
    // Days from 1970-01-01 by the era arithmetic of the proleptic
    // Gregorian calendar (the algorithm every date library uses).
    std::int64_t y = year;
    if (month <= 2)
        y -= 1;
    std::int64_t const era = (y >= 0 ? y : y - 399) / 400;
    std::int64_t const yoe = y - era * 400;
    std::int64_t const doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    std::int64_t const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    std::int64_t const days = era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

std::optional<std::int64_t> der_time(DerElement const& element)
{
    std::span<std::uint8_t const> const bytes = element.content;
    int year = 0;
    std::size_t offset = 0;
    auto digits = [&](std::size_t count) -> std::optional<int> {
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (offset >= bytes.size() || bytes[offset] < '0' || bytes[offset] > '9')
                return std::nullopt;
            value = value * 10 + (bytes[offset++] - '0');
        }
        return value;
    };
    if (element.tag.is(DerType::UtcTime)) {
        // RFC 5280 §4.1.2.5.1: YYMMDDHHMMSSZ, years 1950 through 2049.
        if (bytes.size() != 13)
            return std::nullopt;
        std::optional<int> const yy = digits(2);
        if (!yy)
            return std::nullopt;
        year = *yy < 50 ? 2000 + *yy : 1900 + *yy;
    } else if (element.tag.is(DerType::GeneralizedTime)) {
        // §4.1.2.5.2: YYYYMMDDHHMMSSZ, no fractions.
        if (bytes.size() != 15)
            return std::nullopt;
        std::optional<int> const yyyy = digits(4);
        if (!yyyy)
            return std::nullopt;
        year = *yyyy;
    } else {
        return std::nullopt;
    }
    std::optional<int> const month = digits(2);
    std::optional<int> const day = digits(2);
    std::optional<int> const hour = digits(2);
    std::optional<int> const minute = digits(2);
    std::optional<int> const second = digits(2);
    if (!month || !day || !hour || !minute || !second || bytes[offset] != 'Z')
        return std::nullopt;
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 59)
        return std::nullopt;
    return seconds_from_civil(year, *month, *day, *hour, *minute, *second);
}

}
