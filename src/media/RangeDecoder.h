#pragma once

// The range decoder Opus codes everything with (RFC 6716 §4.1): symbols are
// read from the front of a frame by narrowing a range over their
// probabilities, and raw bits are read from the back. Both layers of the
// codec, SILK and CELT, draw every symbol from one of these, and the state
// it ends in — the final range — is what an encoder and a decoder compare to
// prove they read the same bits the same way.

#include <bit>
#include <cstdint>
#include <span>

namespace sashfold::media {

class RangeDecoder {
public:
    explicit RangeDecoder(std::span<std::uint8_t const> bytes);

    // §4.1.2: the symbol's frequency position within a total of `total`,
    // which the caller turns into a symbol and hands to update().
    unsigned decode(unsigned total);
    // The same with a total of 2^bits, which needs no division.
    unsigned decode_bin(unsigned bits);
    // Consumes the symbol whose frequencies are [low, high) of `total`.
    void update(unsigned low, unsigned high, unsigned total);
    // A one with probability 1/2^logp (§4.1.3.2).
    bool bit_logp(unsigned logp);
    // A symbol by an inverse cumulative table in eighths of 2^bits (§4.1.3.3).
    int icdf(unsigned char const* table, unsigned bits);
    // A whole number in [0, total) (§4.1.5): the top eight bits through the
    // range, the rest raw.
    std::uint32_t uint(std::uint32_t total);
    // Raw bits from the end of the frame (§4.1.4).
    std::uint32_t bits(unsigned count);

    // Bits used so far, rounded up (§4.1.6.1), and the same in eighths.
    int tell() const;
    std::uint32_t tell_frac() const;

    std::uint32_t range() const { return m_range; }
    std::uint32_t storage() const { return m_storage; }
    // The bytes the frame holds for the range decoder: a redundant frame at
    // the end of an Opus frame takes some away before CELT reads.
    void shrink(std::uint32_t bytes) { m_storage -= bytes; }
    // Whether a whole number read past what its total allowed.
    bool error() const { return m_error; }
    // For a caller that has decided the rest of the frame is silence and
    // must account for every bit as read.
    void skip_to(int total_bits) { m_total_bits += total_bits - tell(); }

private:
    int read_byte();
    int read_byte_from_end();
    void normalize();

    std::uint8_t const* m_bytes;
    std::uint32_t m_storage;
    std::uint32_t m_end_offset = 0;
    std::uint32_t m_end_window = 0;
    int m_end_bits = 0;
    int m_total_bits = 0;
    std::uint32_t m_offset = 0;
    std::uint32_t m_range = 0;
    std::uint32_t m_value = 0;
    std::uint32_t m_scale = 0; // the normalization decode() leaves for update()
    int m_rem = 0;
    bool m_error = false;
};

// The number of bits needed to write a value: 0 for 0, 1 for 1, 2 for 2
// and 3, and so on (the codec's ilog).
inline int ilog(std::uint32_t value)
{
    return static_cast<int>(std::bit_width(value));
}

}
