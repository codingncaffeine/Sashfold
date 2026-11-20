#include "auth/Cbor.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace sashfold::auth::cbor {

Value Value::unsigned_integer(std::uint64_t value)
{
    Value v;
    v.m_type = Type::Unsigned;
    v.m_number = value;
    return v;
}

Value Value::negative_minus_one(std::uint64_t n)
{
    Value v;
    v.m_type = Type::Negative;
    v.m_number = n;
    return v;
}

Value Value::integer(std::int64_t value)
{
    if (value >= 0)
        return unsigned_integer(static_cast<std::uint64_t>(value));
    // −1 − value without overflow at INT64_MIN.
    return negative_minus_one(static_cast<std::uint64_t>(-(value + 1)));
}

Value Value::bytes(std::span<std::uint8_t const> value)
{
    Value v;
    v.m_type = Type::Bytes;
    v.m_number = 0;
    v.m_bytes.assign(value.begin(), value.end());
    return v;
}

Value Value::bytes(std::vector<std::uint8_t> value)
{
    Value v;
    v.m_type = Type::Bytes;
    v.m_number = 0;
    v.m_bytes = std::move(value);
    return v;
}

Value Value::text(std::string_view value)
{
    Value v;
    v.m_type = Type::Text;
    v.m_number = 0;
    v.m_bytes.assign(reinterpret_cast<std::uint8_t const*>(value.data()), reinterpret_cast<std::uint8_t const*>(value.data()) + value.size());
    return v;
}

Value Value::array(std::vector<Value> items)
{
    Value v;
    v.m_type = Type::Array;
    v.m_number = 0;
    v.m_items = std::move(items);
    return v;
}

Value Value::map(std::vector<std::pair<Value, Value>> entries)
{
    Value v;
    v.m_type = Type::Map;
    v.m_number = 0;
    v.m_entries = std::move(entries);
    return v;
}

Value Value::tag(std::uint64_t number, Value item)
{
    Value v;
    v.m_type = Type::Tag;
    v.m_number = number;
    v.m_items.push_back(std::move(item));
    return v;
}

Value Value::boolean(bool value) { return simple(value ? 21 : 20); }
Value Value::null() { return simple(22); }
Value Value::undefined() { return simple(23); }

Value Value::simple(std::uint8_t value)
{
    Value v;
    v.m_type = Type::Simple;
    v.m_number = value;
    return v;
}

Value Value::floating(double value, std::uint8_t width)
{
    Value v;
    v.m_type = Type::Float;
    v.m_number = width == 2 || width == 4 ? width : 8;
    v.m_float = value;
    return v;
}

std::optional<std::int64_t> Value::as_int64() const
{
    if (m_type == Type::Unsigned) {
        if (m_number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        return static_cast<std::int64_t>(m_number);
    }
    if (m_type == Type::Negative) {
        if (m_number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        return -1 - static_cast<std::int64_t>(m_number);
    }
    return std::nullopt;
}

std::optional<bool> Value::as_bool() const
{
    if (!is_bool())
        return std::nullopt;
    return m_number == 21;
}

Value const* Value::find(std::int64_t key) const
{
    if (m_type != Type::Map)
        return nullptr;
    for (auto const& [k, v] : m_entries) {
        if (k.is_integer() && k.as_int64() == key)
            return &v;
    }
    return nullptr;
}

Value const* Value::find(std::string_view key) const
{
    if (m_type != Type::Map)
        return nullptr;
    for (auto const& [k, v] : m_entries) {
        if (k.is_text() && k.as_text() == key)
            return &v;
    }
    return nullptr;
}

Value& Value::set(Value key, Value value)
{
    for (auto& [k, v] : m_entries) {
        if (k == key) {
            v = std::move(value);
            return v;
        }
    }
    m_entries.emplace_back(std::move(key), std::move(value));
    return m_entries.back().second;
}

bool Value::operator==(Value const& other) const
{
    if (m_type != other.m_type || m_number != other.m_number)
        return false;
    if (m_type == Type::Float)
        return m_float == other.m_float || (std::isnan(m_float) && std::isnan(other.m_float));
    return m_bytes == other.m_bytes && m_items == other.m_items && m_entries == other.m_entries;
}

namespace {

void write_head(std::vector<std::uint8_t>& out, std::uint8_t major, std::uint64_t argument)
{
    std::uint8_t const high = static_cast<std::uint8_t>(major << 5);
    if (argument < 24) {
        out.push_back(static_cast<std::uint8_t>(high | argument));
    } else if (argument <= 0xff) {
        out.push_back(high | 24);
        out.push_back(static_cast<std::uint8_t>(argument));
    } else if (argument <= 0xffff) {
        out.push_back(high | 25);
        out.push_back(static_cast<std::uint8_t>(argument >> 8));
        out.push_back(static_cast<std::uint8_t>(argument));
    } else if (argument <= 0xffffffffu) {
        out.push_back(high | 26);
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(argument >> shift));
    } else {
        out.push_back(high | 27);
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(argument >> shift));
    }
}

// IEEE 754 binary16 from a double (round to nearest even), for values the
// caller has already chosen to hold in a half.
std::uint16_t half_from_double(double value)
{
    float const f = static_cast<float>(value);
    std::uint32_t const bits = std::bit_cast<std::uint32_t>(f);
    std::uint32_t const sign = (bits >> 16) & 0x8000u;
    std::int32_t const exponent = static_cast<std::int32_t>((bits >> 23) & 0xff);
    std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xff) // infinity or NaN
        return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
    std::int32_t const half_exponent = exponent - 127 + 15;
    if (half_exponent >= 0x1f)
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (half_exponent <= 0) {
        if (half_exponent < -10)
            return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        std::uint32_t const shift = static_cast<std::uint32_t>(14 - half_exponent);
        std::uint32_t half_mantissa = mantissa >> shift;
        std::uint32_t const rest = mantissa & ((1u << shift) - 1);
        std::uint32_t const halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half_mantissa & 1u) != 0))
            ++half_mantissa;
        return static_cast<std::uint16_t>(sign | half_mantissa);
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(half_exponent) << 10) | (mantissa >> 13);
    std::uint32_t const rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0))
        ++half;
    return static_cast<std::uint16_t>(half);
}

// RFC 8949 Appendix D.
double double_from_half(std::uint16_t half)
{
    int const exponent = (half >> 10) & 0x1f;
    int const mantissa = half & 0x3ff;
    double value;
    if (exponent == 0)
        value = std::ldexp(mantissa, -24);
    else if (exponent != 31)
        value = std::ldexp(mantissa + 1024, exponent - 25);
    else
        value = mantissa == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
    return (half & 0x8000) != 0 ? -value : value;
}

}

void encode_into(Value const& value, std::vector<std::uint8_t>& out)
{
    switch (value.type()) {
    case Type::Unsigned:
        write_head(out, 0, value.raw_number());
        return;
    case Type::Negative:
        write_head(out, 1, value.raw_number());
        return;
    case Type::Bytes:
    case Type::Text:
        write_head(out, value.type() == Type::Bytes ? 2 : 3, value.as_bytes().size());
        out.insert(out.end(), value.as_bytes().begin(), value.as_bytes().end());
        return;
    case Type::Array:
        write_head(out, 4, value.items().size());
        for (Value const& item : value.items())
            encode_into(item, out);
        return;
    case Type::Map: {
        // Each key is encoded once, then the entries are written in the
        // order of their encoded keys.
        std::vector<std::pair<std::vector<std::uint8_t>, Value const*>> sorted;
        sorted.reserve(value.entries().size());
        for (auto const& [k, v] : value.entries())
            sorted.emplace_back(encode(k), &v);
        std::stable_sort(sorted.begin(), sorted.end(), [](auto const& a, auto const& b) { return canonical_key_less(a.first, b.first); });
        write_head(out, 5, sorted.size());
        for (auto const& [key, item] : sorted) {
            out.insert(out.end(), key.begin(), key.end());
            encode_into(*item, out);
        }
        return;
    }
    case Type::Tag:
        write_head(out, 6, value.raw_number());
        encode_into(value.tagged(), out);
        return;
    case Type::Simple:
        if (value.raw_number() < 24) {
            out.push_back(static_cast<std::uint8_t>(0xe0 | value.raw_number()));
        } else {
            out.push_back(0xf8);
            out.push_back(static_cast<std::uint8_t>(value.raw_number()));
        }
        return;
    case Type::Float:
        if (value.float_width() == 2) {
            std::uint16_t const half = half_from_double(value.as_double());
            out.push_back(0xf9);
            out.push_back(static_cast<std::uint8_t>(half >> 8));
            out.push_back(static_cast<std::uint8_t>(half));
        } else if (value.float_width() == 4) {
            std::uint32_t const bits = std::bit_cast<std::uint32_t>(static_cast<float>(value.as_double()));
            out.push_back(0xfa);
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(static_cast<std::uint8_t>(bits >> shift));
        } else {
            std::uint64_t const bits = std::bit_cast<std::uint64_t>(value.as_double());
            out.push_back(0xfb);
            for (int shift = 56; shift >= 0; shift -= 8)
                out.push_back(static_cast<std::uint8_t>(bits >> shift));
        }
        return;
    }
}

std::vector<std::uint8_t> encode(Value const& value)
{
    std::vector<std::uint8_t> out;
    encode_into(value, out);
    return out;
}

bool canonical_key_less(std::span<std::uint8_t const> a, std::span<std::uint8_t const> b)
{
    // CTAP 2.1 §8: the major type, then the shorter, then byte-wise.
    if (a.empty() || b.empty())
        return a.size() < b.size();
    int const major_a = a[0] >> 5;
    int const major_b = b[0] >> 5;
    if (major_a != major_b)
        return major_a < major_b;
    if (a.size() != b.size())
        return a.size() < b.size();
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

namespace {

bool valid_utf8(std::span<std::uint8_t const> bytes)
{
    std::size_t i = 0;
    while (i < bytes.size()) {
        std::uint8_t const lead = bytes[i];
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length;
        std::uint32_t code;
        if ((lead & 0xe0) == 0xc0) {
            length = 2;
            code = lead & 0x1fu;
        } else if ((lead & 0xf0) == 0xe0) {
            length = 3;
            code = lead & 0x0fu;
        } else if ((lead & 0xf8) == 0xf0) {
            length = 4;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (bytes.size() - i < length)
            return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((bytes[i + k] & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (bytes[i + k] & 0x3fu);
        }
        // Overlong forms, surrogates and values past U+10FFFF.
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || code > 0x10ffff
            || (code >= 0xd800 && code <= 0xdfff))
            return false;
        i += length;
    }
    return true;
}

class Decoder {
public:
    Decoder(std::span<std::uint8_t const> bytes, Mode mode)
        : m_bytes(bytes)
        , m_mode(mode)
    {
    }

    std::size_t offset() const { return m_offset; }

    std::optional<Value> item(int depth)
    {
        if (m_offset >= m_bytes.size())
            return std::nullopt;
        std::uint8_t const initial = m_bytes[m_offset++];
        std::uint8_t const major = initial >> 5;
        std::uint8_t const info = initial & 0x1f;

        if (major == 7) {
            auto value = simple_or_float(info);
            // CTAP2 messages carry false, true and null of the simple
            // values; the rest have no meaning there.
            if (value && m_mode == Mode::Ctap2 && value->type() == Type::Simple && !value->is_bool() && !value->is_null())
                return std::nullopt;
            return value;
        }

        bool indefinite = false;
        std::uint64_t argument = 0;
        if (info == 31) {
            // Indefinite length: strings, arrays and maps in General mode.
            if (m_mode == Mode::Ctap2 || major < 2 || major == 6)
                return std::nullopt;
            indefinite = true;
        } else if (!read_argument(info, argument)) {
            return std::nullopt;
        }

        switch (major) {
        case 0:
            return Value::unsigned_integer(argument);
        case 1:
            return Value::negative_minus_one(argument);
        case 2:
        case 3: {
            std::vector<std::uint8_t> content;
            if (indefinite) {
                // Definite chunks of the same major type up to the break.
                for (;;) {
                    if (m_offset >= m_bytes.size())
                        return std::nullopt;
                    if (m_bytes[m_offset] == 0xff) {
                        ++m_offset;
                        break;
                    }
                    std::uint8_t const chunk_initial = m_bytes[m_offset++];
                    if ((chunk_initial >> 5) != major || (chunk_initial & 0x1f) == 31)
                        return std::nullopt;
                    std::uint64_t chunk_length = 0;
                    if (!read_argument(chunk_initial & 0x1f, chunk_length) || chunk_length > remaining())
                        return std::nullopt;
                    auto const chunk = m_bytes.subspan(m_offset, static_cast<std::size_t>(chunk_length));
                    // Each chunk of a text string is whole UTF-8 by itself.
                    if (major == 3 && !valid_utf8(chunk))
                        return std::nullopt;
                    content.insert(content.end(), chunk.begin(), chunk.end());
                    m_offset += static_cast<std::size_t>(chunk_length);
                }
            } else {
                if (argument > remaining())
                    return std::nullopt;
                auto const chunk = m_bytes.subspan(m_offset, static_cast<std::size_t>(argument));
                content.assign(chunk.begin(), chunk.end());
                m_offset += static_cast<std::size_t>(argument);
            }
            if (major == 3) {
                if (!valid_utf8(content))
                    return std::nullopt;
                Value v = Value::text(std::string_view(reinterpret_cast<char const*>(content.data()), content.size()));
                return v;
            }
            return Value::bytes(std::move(content));
        }
        case 4: {
            if (depth + 1 > max_depth())
                return std::nullopt;
            std::vector<Value> items;
            if (indefinite) {
                for (;;) {
                    if (m_offset >= m_bytes.size())
                        return std::nullopt;
                    if (m_bytes[m_offset] == 0xff) {
                        ++m_offset;
                        break;
                    }
                    auto element = item(depth + 1);
                    if (!element)
                        return std::nullopt;
                    items.push_back(std::move(*element));
                }
            } else {
                // Every item takes at least one byte: a count beyond what
                // remains cannot be honest, and nothing is reserved for it.
                if (argument > remaining())
                    return std::nullopt;
                items.reserve(static_cast<std::size_t>(argument));
                for (std::uint64_t i = 0; i < argument; ++i) {
                    auto element = item(depth + 1);
                    if (!element)
                        return std::nullopt;
                    items.push_back(std::move(*element));
                }
            }
            return Value::array(std::move(items));
        }
        case 5: {
            if (depth + 1 > max_depth())
                return std::nullopt;
            std::vector<std::pair<Value, Value>> entries;
            std::vector<std::vector<std::uint8_t>> keys; // as encoded, for order and duplicates
            auto const entry = [&]() -> bool {
                std::size_t const key_start = m_offset;
                auto key = item(depth + 1);
                if (!key)
                    return false;
                std::vector<std::uint8_t> encoded_key;
                if (m_mode == Mode::Ctap2) {
                    // Canonical input: the key's own bytes are its encoding.
                    auto const raw = m_bytes.subspan(key_start, m_offset - key_start);
                    encoded_key.assign(raw.begin(), raw.end());
                    if (!keys.empty() && !canonical_key_less(keys.back(), encoded_key))
                        return false; // out of order, or the same key twice
                } else {
                    encoded_key = encode(*key);
                    for (auto const& seen : keys) {
                        if (seen == encoded_key)
                            return false;
                    }
                }
                auto value = item(depth + 1);
                if (!value)
                    return false;
                keys.push_back(std::move(encoded_key));
                entries.emplace_back(std::move(*key), std::move(*value));
                return true;
            };
            if (indefinite) {
                for (;;) {
                    if (m_offset >= m_bytes.size())
                        return std::nullopt;
                    if (m_bytes[m_offset] == 0xff) {
                        ++m_offset;
                        break;
                    }
                    if (!entry())
                        return std::nullopt;
                }
            } else {
                if (argument > remaining() / 2)
                    return std::nullopt;
                entries.reserve(static_cast<std::size_t>(argument));
                for (std::uint64_t i = 0; i < argument; ++i) {
                    if (!entry())
                        return std::nullopt;
                }
            }
            return Value::map(std::move(entries));
        }
        case 6: {
            if (m_mode == Mode::Ctap2) // CTAP 2.1 §8: no tags
                return std::nullopt;
            if (depth + 1 > max_depth())
                return std::nullopt;
            auto tagged = item(depth + 1);
            if (!tagged)
                return std::nullopt;
            return Value::tag(argument, std::move(*tagged));
        }
        }
        return std::nullopt;
    }

private:
    std::size_t remaining() const { return m_bytes.size() - m_offset; }
    int max_depth() const { return m_mode == Mode::Ctap2 ? 4 : 16; }

    bool read_argument(std::uint8_t info, std::uint64_t& argument)
    {
        if (info < 24) {
            argument = info;
            return true;
        }
        if (info > 27)
            return false; // 28 to 30 are reserved
        std::size_t const size = std::size_t { 1 } << (info - 24);
        if (size > remaining())
            return false;
        argument = 0;
        for (std::size_t i = 0; i < size; ++i)
            argument = (argument << 8) | m_bytes[m_offset + i];
        m_offset += size;
        if (m_mode == Mode::Ctap2) {
            // The shortest form only (CTAP 2.1 §8).
            std::uint64_t const smallest_for_size[] = { 24, 0x100, 0x10000, 0x100000000ull };
            if (argument < smallest_for_size[info - 24])
                return false;
        }
        return true;
    }

    std::optional<Value> simple_or_float(std::uint8_t info)
    {
        if (info < 24)
            return Value::simple(info);
        if (info == 24) {
            if (remaining() < 1)
                return std::nullopt;
            std::uint8_t const value = m_bytes[m_offset++];
            // Values below 32 have their one-byte form (RFC 8949 §3.3).
            if (value < 32)
                return std::nullopt;
            return Value::simple(value);
        }
        if (info >= 25 && info <= 27) {
            std::size_t const size = std::size_t { 1 } << (info - 24);
            if (size > remaining())
                return std::nullopt;
            std::uint64_t bits = 0;
            for (std::size_t i = 0; i < size; ++i)
                bits = (bits << 8) | m_bytes[m_offset + i];
            m_offset += size;
            if (size == 2)
                return Value::floating(double_from_half(static_cast<std::uint16_t>(bits)), 2);
            if (size == 4)
                return Value::floating(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits))), 4);
            return Value::floating(std::bit_cast<double>(bits), 8);
        }
        // 28 to 30 are reserved; 31 is a break with nothing to end.
        return std::nullopt;
    }

    std::span<std::uint8_t const> m_bytes;
    Mode m_mode;
    std::size_t m_offset = 0;
};

}

std::optional<std::pair<Value, std::size_t>> decode_prefix(std::span<std::uint8_t const> bytes, Mode mode)
{
    Decoder decoder(bytes, mode);
    auto value = decoder.item(0);
    if (!value)
        return std::nullopt;
    return std::pair<Value, std::size_t>(std::move(*value), decoder.offset());
}

std::optional<Value> decode(std::span<std::uint8_t const> bytes, Mode mode)
{
    auto result = decode_prefix(bytes, mode);
    if (!result || result->second != bytes.size())
        return std::nullopt;
    return std::move(result->first);
}

}
