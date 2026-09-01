#include "core/Png.h"

#include "core/Bitmap.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>

namespace sashfold {

namespace {

std::array<std::uint32_t, 256> const& crc_table()
{
    static std::array<std::uint32_t, 256> const table = [] {
        std::array<std::uint32_t, 256> result {};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            result[n] = c;
        }
        return result;
    }();
    return table;
}

std::uint32_t crc32(std::uint8_t const* data, std::size_t length)
{
    std::uint32_t c = 0xFFFFFFFFu;
    auto const& table = crc_table();
    for (std::size_t i = 0; i < length; ++i)
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(std::vector<std::uint8_t> const& data)
{
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : data) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void push_be32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void push_chunk(std::vector<std::uint8_t>& out, char const (&type)[5], std::vector<std::uint8_t> const& data)
{
    push_be32(out, static_cast<std::uint32_t>(data.size()));

    std::vector<std::uint8_t> typed_payload;
    typed_payload.reserve(4u + data.size());
    for (int i = 0; i < 4; ++i)
        typed_payload.push_back(static_cast<std::uint8_t>(type[i]));
    typed_payload.insert(typed_payload.end(), data.begin(), data.end());

    out.insert(out.end(), typed_payload.begin(), typed_payload.end());
    push_be32(out, crc32(typed_payload.data(), typed_payload.size()));
}

// zlib stream (RFC 1950) wrapping one fixed-Huffman deflate block (RFC 1951)
// with a greedy LZ77 matcher: a 3-byte hash remembers the latest position,
// one probe per step, matches up to 258 within the 32K window. Flat scanlines
// become distance-1 runs, which is most of what a rendered page is.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out)
        : m_out(out)
    {
    }

    // Deflate packs data elements starting at the least significant bit.
    void put_bits(std::uint32_t value, int count)
    {
        m_bits |= static_cast<std::uint64_t>(value) << m_bit_count;
        m_bit_count += count;
        while (m_bit_count >= 8) {
            m_out.push_back(static_cast<std::uint8_t>(m_bits & 0xFFu));
            m_bits >>= 8;
            m_bit_count -= 8;
        }
    }

    // Huffman codes pack most significant bit first: reverse, then pack.
    void put_code(std::uint32_t code, int length)
    {
        std::uint32_t reversed = 0;
        for (int i = 0; i < length; ++i)
            reversed |= ((code >> i) & 1u) << (length - 1 - i);
        put_bits(reversed, length);
    }

    void flush()
    {
        if (m_bit_count > 0) {
            m_out.push_back(static_cast<std::uint8_t>(m_bits & 0xFFu));
            m_bits = 0;
            m_bit_count = 0;
        }
    }

private:
    std::vector<std::uint8_t>& m_out;
    std::uint64_t m_bits = 0;
    int m_bit_count = 0;
};

// RFC 1951 §3.2.6: the fixed literal/length code.
void put_fixed_symbol(BitWriter& writer, unsigned symbol)
{
    if (symbol <= 143)
        writer.put_code(0x30u + symbol, 8);
    else if (symbol <= 255)
        writer.put_code(0x190u + (symbol - 144), 9);
    else if (symbol <= 279)
        writer.put_code(symbol - 256, 7);
    else
        writer.put_code(0xC0u + (symbol - 280), 8);
}

struct PrefixCode {
    unsigned symbol;
    int extra_bits;
    unsigned base;
};

// RFC 1951 §3.2.5, length codes 257-285 for lengths 3-258.
PrefixCode length_code_for(unsigned length)
{
    static constexpr struct {
        unsigned base;
        int extra;
    } table[29] = {
        { 3, 0 }, { 4, 0 }, { 5, 0 }, { 6, 0 }, { 7, 0 }, { 8, 0 }, { 9, 0 }, { 10, 0 },
        { 11, 1 }, { 13, 1 }, { 15, 1 }, { 17, 1 }, { 19, 2 }, { 23, 2 }, { 27, 2 }, { 31, 2 },
        { 35, 3 }, { 43, 3 }, { 51, 3 }, { 59, 3 }, { 67, 4 }, { 83, 4 }, { 99, 4 }, { 115, 4 },
        { 131, 5 }, { 163, 5 }, { 195, 5 }, { 227, 5 }, { 258, 0 }
    };
    for (unsigned i = 28;; --i) {
        if (length >= table[i].base)
            return { 257 + i, table[i].extra, table[i].base };
        if (i == 0)
            break;
    }
    return { 257, 0, 3 };
}

// RFC 1951 §3.2.5, distance codes 0-29 for distances 1-32768.
PrefixCode distance_code_for(unsigned distance)
{
    static constexpr struct {
        unsigned base;
        int extra;
    } table[30] = {
        { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 1 }, { 7, 1 }, { 9, 2 }, { 13, 2 },
        { 17, 3 }, { 25, 3 }, { 33, 4 }, { 49, 4 }, { 65, 5 }, { 97, 5 }, { 129, 6 }, { 193, 6 },
        { 257, 7 }, { 385, 7 }, { 513, 8 }, { 769, 8 }, { 1025, 9 }, { 1537, 9 },
        { 2049, 10 }, { 3073, 10 }, { 4097, 11 }, { 6145, 11 }, { 8193, 12 }, { 12289, 12 },
        { 16385, 13 }, { 24577, 13 }
    };
    for (unsigned i = 29;; --i) {
        if (distance >= table[i].base)
            return { i, table[i].extra, table[i].base };
        if (i == 0)
            break;
    }
    return { 0, 0, 1 };
}

std::vector<std::uint8_t> zlib_deflate(std::vector<std::uint8_t> const& raw)
{
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() / 4 + 64);
    out.push_back(0x78); // CM = 8 (deflate), CINFO = 7 (32K window)
    out.push_back(0x01); // FCHECK: 0x7801 is a multiple of 31, no dictionary

    BitWriter writer(out);
    writer.put_bits(1, 1); // BFINAL
    writer.put_bits(1, 2); // BTYPE = 01: fixed Huffman

    constexpr std::size_t hash_size = 1u << 15;
    std::vector<std::int64_t> head(hash_size, -1);
    auto const hash_at = [&](std::size_t i) {
        std::uint32_t const h = (static_cast<std::uint32_t>(raw[i]) << 16)
            ^ (static_cast<std::uint32_t>(raw[i + 1]) << 8) ^ raw[i + 2];
        return (h * 2654435761u) >> 17; // top 15 bits
    };

    std::size_t i = 0;
    while (i < raw.size()) {
        unsigned best_length = 0;
        std::size_t best_distance = 0;
        if (i + 2 < raw.size()) {
            std::uint32_t const h = hash_at(i);
            std::int64_t const candidate = head[h];
            head[h] = static_cast<std::int64_t>(i);
            if (candidate >= 0) {
                std::size_t const distance = i - static_cast<std::size_t>(candidate);
                if (distance <= 32768) {
                    std::size_t const limit = std::min<std::size_t>(258, raw.size() - i);
                    std::size_t length = 0;
                    while (length < limit
                        && raw[static_cast<std::size_t>(candidate) + length] == raw[i + length])
                        ++length;
                    if (length >= 3) {
                        best_length = static_cast<unsigned>(length);
                        best_distance = distance;
                    }
                }
            }
        }
        if (best_length >= 3) {
            PrefixCode const length_code = length_code_for(best_length);
            put_fixed_symbol(writer, length_code.symbol);
            if (length_code.extra_bits > 0)
                writer.put_bits(best_length - length_code.base, length_code.extra_bits);
            PrefixCode const distance_code
                = distance_code_for(static_cast<unsigned>(best_distance));
            writer.put_code(distance_code.symbol, 5);
            if (distance_code.extra_bits > 0)
                writer.put_bits(static_cast<std::uint32_t>(best_distance) - distance_code.base,
                    distance_code.extra_bits);
            // Keep the hash warm through the match body.
            std::size_t const end = i + best_length;
            for (++i; i < end && i + 2 < raw.size(); ++i)
                head[hash_at(i)] = static_cast<std::int64_t>(i);
            i = end;
            continue;
        }
        put_fixed_symbol(writer, raw[i]);
        ++i;
    }
    put_fixed_symbol(writer, 256); // end of block
    writer.flush();

    push_be32(out, adler32(raw));
    return out;
}

}

std::vector<std::uint8_t> encode_png(Bitmap const& bitmap)
{
    std::vector<std::uint8_t> out { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

    std::vector<std::uint8_t> ihdr;
    push_be32(ihdr, static_cast<std::uint32_t>(bitmap.width()));
    push_be32(ihdr, static_cast<std::uint32_t>(bitmap.height()));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // colour type: truecolour with alpha
    ihdr.push_back(0); // compression: deflate
    ihdr.push_back(0); // filter method: adaptive
    ihdr.push_back(0); // interlace: none
    push_chunk(out, "IHDR", ihdr);

    // Each scanline is prefixed with its filter type. Filter 0 (None) keeps the
    // encoder simple; better filters are a compression win, not a correctness one.
    std::vector<std::uint8_t> raw;
    std::size_t const stride = static_cast<std::size_t>(bitmap.width()) * 4u;
    raw.reserve((stride + 1u) * static_cast<std::size_t>(bitmap.height()));
    auto const& pixels = bitmap.pixels();
    for (int y = 0; y < bitmap.height(); ++y) {
        raw.push_back(0);
        std::size_t const row = static_cast<std::size_t>(y) * stride;
        raw.insert(raw.end(), pixels.begin() + static_cast<std::ptrdiff_t>(row),
            pixels.begin() + static_cast<std::ptrdiff_t>(row + stride));
    }

    push_chunk(out, "IDAT", zlib_deflate(raw));
    push_chunk(out, "IEND", {});
    return out;
}

bool write_png(std::string const& path, Bitmap const& bitmap)
{
    std::vector<std::uint8_t> const bytes = encode_png(bitmap);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

}
