#include "media/RangeDecoder.h"

#include <algorithm>

namespace sashfold::media {

namespace {

// The coder's constants (RFC 6716 §4.1): eight-bit symbols in a 32-bit
// range, with the top bit kept clear so that a carry never escapes.
constexpr unsigned symbol_bits = 8;
constexpr unsigned code_bits = 32;
constexpr std::uint32_t symbol_max = (1u << symbol_bits) - 1;
constexpr std::uint32_t code_top = 1u << (code_bits - 1);
constexpr std::uint32_t code_bottom = code_top >> symbol_bits;
constexpr unsigned code_extra = (code_bits - 2) % symbol_bits + 1;
constexpr unsigned uint_bits = 8;
constexpr int bit_resolution = 3;

}

RangeDecoder::RangeDecoder(std::span<std::uint8_t const> bytes)
    : m_bytes(bytes.data())
    , m_storage(static_cast<std::uint32_t>(bytes.size()))
{
    // §4.1.1: the first byte seeds the value, and the normalisation that
    // follows reads until the range fills its top byte. The count of bits
    // starts where it does so that after that normalisation it agrees with
    // the encoder's.
    m_total_bits = static_cast<int>(code_bits + 1 - ((code_bits - code_extra) / symbol_bits) * symbol_bits);
    m_range = 1u << code_extra;
    m_rem = read_byte();
    m_value = m_range - 1 - (static_cast<std::uint32_t>(m_rem) >> (symbol_bits - code_extra));
    normalize();
}

int RangeDecoder::read_byte()
{
    return m_offset < m_storage ? m_bytes[m_offset++] : 0;
}

int RangeDecoder::read_byte_from_end()
{
    return m_end_offset < m_storage ? m_bytes[m_storage - ++m_end_offset] : 0;
}

void RangeDecoder::normalize()
{
    // §4.1.2.1: whenever the range has shrunk to its bottom byte, shift a
    // byte in. The value holds the distance to the top of the range, so the
    // incoming bits are taken inverted.
    while (m_range <= code_bottom) {
        m_total_bits += static_cast<int>(symbol_bits);
        m_range <<= symbol_bits;
        int symbol = m_rem;
        m_rem = read_byte();
        symbol = (symbol << symbol_bits | m_rem) >> (symbol_bits - code_extra);
        m_value = ((m_value << symbol_bits) + (symbol_max & ~static_cast<std::uint32_t>(symbol))) & (code_top - 1);
    }
}

unsigned RangeDecoder::decode(unsigned total)
{
    m_scale = m_range / total;
    unsigned const s = m_value / m_scale;
    return total - std::min(s + 1, total);
}

unsigned RangeDecoder::decode_bin(unsigned bits)
{
    m_scale = m_range >> bits;
    unsigned const s = m_value / m_scale;
    return (1u << bits) - std::min(s + 1, 1u << bits);
}

void RangeDecoder::update(unsigned low, unsigned high, unsigned total)
{
    std::uint32_t const s = m_scale * (total - high);
    m_value -= s;
    m_range = low > 0 ? m_scale * (high - low) : m_range - s;
    normalize();
}

bool RangeDecoder::bit_logp(unsigned logp)
{
    std::uint32_t const r = m_range;
    std::uint32_t const d = m_value;
    std::uint32_t const s = r >> logp;
    bool const one = d < s;
    if (!one)
        m_value = d - s;
    m_range = one ? s : r - s;
    normalize();
    return one;
}

int RangeDecoder::icdf(unsigned char const* table, unsigned bits)
{
    std::uint32_t s = m_range;
    std::uint32_t const d = m_value;
    std::uint32_t const r = s >> bits;
    std::uint32_t t;
    int symbol = -1;
    do {
        t = s;
        s = r * table[++symbol];
    } while (d < s);
    m_value = d - s;
    m_range = t - s;
    normalize();
    return symbol;
}

std::uint32_t RangeDecoder::uint(std::uint32_t total)
{
    // Only the top eight bits of the value go through the range; the rest,
    // being nearly uniform, are raw bits from the end.
    std::uint32_t const top = total - 1;
    int bits_needed = ilog(top);
    if (bits_needed > static_cast<int>(uint_bits)) {
        bits_needed -= static_cast<int>(uint_bits);
        unsigned const high_total = static_cast<unsigned>(top >> bits_needed) + 1;
        unsigned const s = decode(high_total);
        update(s, s + 1, high_total);
        std::uint32_t const value = static_cast<std::uint32_t>(s) << bits_needed | bits(static_cast<unsigned>(bits_needed));
        if (value <= top)
            return value;
        m_error = true;
        return top;
    }
    unsigned const s = decode(total);
    update(s, s + 1, total);
    return s;
}

std::uint32_t RangeDecoder::bits(unsigned count)
{
    std::uint32_t window = m_end_window;
    int available = m_end_bits;
    if (static_cast<unsigned>(available) < count) {
        do {
            window |= static_cast<std::uint32_t>(read_byte_from_end()) << available;
            available += static_cast<int>(symbol_bits);
        } while (available <= 32 - static_cast<int>(symbol_bits));
    }
    std::uint32_t const value = count == 32 ? window : window & ((1u << count) - 1);
    window = count == 32 ? 0 : window >> count;
    available -= static_cast<int>(count);
    m_end_window = window;
    m_end_bits = available;
    m_total_bits += static_cast<int>(count);
    return value;
}

int RangeDecoder::tell() const
{
    return m_total_bits - ilog(m_range);
}

std::uint32_t RangeDecoder::tell_frac() const
{
    // §4.1.6.2: the whole bits used, less the fraction of a bit the range
    // still holds, found to three binary places by squaring its top sixteen
    // bits three times.
    std::uint32_t const whole = static_cast<std::uint32_t>(m_total_bits) << bit_resolution;
    int l = ilog(m_range);
    std::uint32_t r = m_range >> (l - 16);
    for (int i = bit_resolution; i-- > 0;) {
        r = r * r >> 15;
        int const b = static_cast<int>(r >> 16);
        l = l << 1 | b;
        r >>= b;
    }
    return whole - static_cast<std::uint32_t>(l);
}

}
