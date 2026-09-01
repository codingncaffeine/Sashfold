#include "core/Inflate.h"

#include <algorithm>
#include <array>

namespace sashfold {

namespace {

struct BitReader {
    std::vector<std::uint8_t> const& data;
    std::size_t byte = 0;
    int bit = 0;
    bool overrun = false;

    unsigned take(int count) // data elements, LSB-first
    {
        unsigned value = 0;
        for (int i = 0; i < count; ++i) {
            if (byte >= data.size()) {
                overrun = true;
                return value;
            }
            value |= static_cast<unsigned>((data[byte] >> bit) & 1u) << i;
            if (++bit == 8) {
                bit = 0;
                ++byte;
            }
        }
        return value;
    }

    void align()
    {
        if (bit != 0) {
            bit = 0;
            ++byte;
        }
    }
};

// A canonical Huffman decoder built from code lengths (RFC 1951 §3.2.2):
// counts per length + first-code offsets, walked bit by bit.
struct HuffmanTable {
    std::array<std::uint16_t, 16> count {}; // codes of each length 1..15
    std::vector<std::uint16_t> symbols; // sorted by (length, symbol)

    static std::optional<HuffmanTable> build(std::uint8_t const* lengths, std::size_t n)
    {
        HuffmanTable table;
        for (std::size_t i = 0; i < n; ++i)
            ++table.count[lengths[i]];
        table.count[0] = 0;
        // Reject over-subscribed codes. Incomplete codes are tolerated: their
        // unused patterns simply never decode, and a hostile stream that uses
        // one dies at the checksum.
        int left = 1;
        std::size_t total = 0;
        for (int length = 1; length <= 15; ++length) {
            left <<= 1;
            left -= table.count[static_cast<std::size_t>(length)];
            if (left < 0)
                return std::nullopt;
            total += table.count[static_cast<std::size_t>(length)];
        }
        std::array<std::uint16_t, 16> position {};
        for (int length = 1; length < 15; ++length)
            position[static_cast<std::size_t>(length + 1)]
                = static_cast<std::uint16_t>(position[static_cast<std::size_t>(length)]
                    + table.count[static_cast<std::size_t>(length)]);
        table.symbols.assign(total, 0);
        for (std::size_t symbol = 0; symbol < n; ++symbol) {
            if (lengths[symbol] != 0)
                table.symbols[position[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
        }
        return table;
    }

    std::optional<unsigned> decode(BitReader& reader) const
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int length = 1; length <= 15; ++length) {
            code |= static_cast<int>(reader.take(1));
            if (reader.overrun)
                return std::nullopt;
            int const n = count[static_cast<std::size_t>(length)];
            if (code - first < n)
                return symbols[static_cast<std::size_t>(index + (code - first))];
            index += n;
            first = (first + n) << 1;
            code <<= 1;
        }
        return std::nullopt;
    }
};

constexpr unsigned length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
constexpr int length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4,
    4, 4, 4, 5, 5, 5, 5, 0 };
constexpr unsigned distance_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
constexpr int distance_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool inflate_block_body(BitReader& reader, HuffmanTable const& literals,
    HuffmanTable const& distances, std::vector<std::uint8_t>& out, std::size_t max_output)
{
    while (true) {
        std::optional<unsigned> const symbol = literals.decode(reader);
        if (!symbol)
            return false;
        if (*symbol < 256) {
            if (out.size() >= max_output)
                return false;
            out.push_back(static_cast<std::uint8_t>(*symbol));
            continue;
        }
        if (*symbol == 256)
            return true;
        unsigned const length_index = *symbol - 257;
        if (length_index >= 29)
            return false;
        unsigned const length
            = length_base[length_index] + reader.take(length_extra[length_index]);
        std::optional<unsigned> const distance_symbol = distances.decode(reader);
        if (!distance_symbol || *distance_symbol >= 30)
            return false;
        unsigned const distance
            = distance_base[*distance_symbol] + reader.take(distance_extra[*distance_symbol]);
        if (reader.overrun || distance > out.size())
            return false;
        if (out.size() + length > max_output)
            return false;
        for (unsigned i = 0; i < length; ++i)
            out.push_back(out[out.size() - distance]);
    }
}

std::optional<HuffmanTable> fixed_literal_table()
{
    std::array<std::uint8_t, 288> lengths {};
    for (std::size_t i = 0; i <= 143; ++i)
        lengths[i] = 8;
    for (std::size_t i = 144; i <= 255; ++i)
        lengths[i] = 9;
    for (std::size_t i = 256; i <= 279; ++i)
        lengths[i] = 7;
    for (std::size_t i = 280; i <= 287; ++i)
        lengths[i] = 8;
    return HuffmanTable::build(lengths.data(), lengths.size());
}

std::optional<HuffmanTable> fixed_distance_table()
{
    std::array<std::uint8_t, 30> lengths {};
    lengths.fill(5);
    return HuffmanTable::build(lengths.data(), lengths.size());
}

std::optional<std::vector<std::uint8_t>> inflate_from(BitReader& reader, std::size_t max_output)
{
    std::vector<std::uint8_t> out;
    while (true) {
        unsigned const is_final = reader.take(1);
        unsigned const type = reader.take(2);
        if (reader.overrun)
            return std::nullopt;

        if (type == 0) { // stored
            reader.align();
            if (reader.byte + 4 > reader.data.size())
                return std::nullopt;
            unsigned const length = reader.data[reader.byte]
                | (static_cast<unsigned>(reader.data[reader.byte + 1]) << 8);
            unsigned const inverted = reader.data[reader.byte + 2]
                | (static_cast<unsigned>(reader.data[reader.byte + 3]) << 8);
            if ((length ^ inverted) != 0xFFFFu)
                return std::nullopt;
            reader.byte += 4;
            if (reader.byte + length > reader.data.size() || out.size() + length > max_output)
                return std::nullopt;
            out.insert(out.end(), reader.data.begin() + static_cast<std::ptrdiff_t>(reader.byte),
                reader.data.begin() + static_cast<std::ptrdiff_t>(reader.byte + length));
            reader.byte += length;
        } else if (type == 1) { // fixed Huffman
            static std::optional<HuffmanTable> const literals = fixed_literal_table();
            static std::optional<HuffmanTable> const distances = fixed_distance_table();
            if (!literals || !distances)
                return std::nullopt;
            if (!inflate_block_body(reader, *literals, *distances, out, max_output))
                return std::nullopt;
        } else if (type == 2) { // dynamic Huffman
            unsigned const hlit = reader.take(5) + 257;
            unsigned const hdist = reader.take(5) + 1;
            unsigned const hclen = reader.take(4) + 4;
            if (reader.overrun || hlit > 286 || hdist > 30)
                return std::nullopt;
            static constexpr std::uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
                12, 3, 13, 2, 14, 1, 15 };
            std::array<std::uint8_t, 19> code_lengths {};
            for (unsigned i = 0; i < hclen; ++i)
                code_lengths[order[i]] = static_cast<std::uint8_t>(reader.take(3));
            if (reader.overrun)
                return std::nullopt;
            std::optional<HuffmanTable> const length_table
                = HuffmanTable::build(code_lengths.data(), code_lengths.size());
            if (!length_table)
                return std::nullopt;

            std::vector<std::uint8_t> lengths;
            lengths.reserve(hlit + hdist);
            while (lengths.size() < hlit + hdist) {
                std::optional<unsigned> const symbol = length_table->decode(reader);
                if (!symbol)
                    return std::nullopt;
                if (*symbol < 16) {
                    lengths.push_back(static_cast<std::uint8_t>(*symbol));
                } else if (*symbol == 16) {
                    if (lengths.empty())
                        return std::nullopt;
                    unsigned const repeat = 3 + reader.take(2);
                    lengths.insert(lengths.end(), repeat, lengths.back());
                } else if (*symbol == 17) {
                    unsigned const repeat = 3 + reader.take(3);
                    lengths.insert(lengths.end(), repeat, 0);
                } else {
                    unsigned const repeat = 11 + reader.take(7);
                    lengths.insert(lengths.end(), repeat, 0);
                }
                if (reader.overrun)
                    return std::nullopt;
            }
            if (lengths.size() != hlit + hdist)
                return std::nullopt;
            if (lengths[256] == 0)
                return std::nullopt; // no end-of-block code
            std::optional<HuffmanTable> const literals
                = HuffmanTable::build(lengths.data(), hlit);
            std::optional<HuffmanTable> const distances
                = HuffmanTable::build(lengths.data() + hlit, hdist);
            if (!literals || !distances)
                return std::nullopt;
            if (!inflate_block_body(reader, *literals, *distances, out, max_output))
                return std::nullopt;
        } else {
            return std::nullopt; // BTYPE == 3 is reserved
        }

        if (is_final)
            return out;
    }
}

std::uint32_t adler32(std::vector<std::uint8_t> const& data)
{
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t const byte : data) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

std::uint32_t crc32_of(std::vector<std::uint8_t> const& data)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t const byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            std::uint32_t const mask
                = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

} // namespace

std::optional<std::vector<std::uint8_t>> inflate(std::vector<std::uint8_t> const& data,
    std::size_t max_output)
{
    BitReader reader { data, 0, 0, false };
    return inflate_from(reader, max_output);
}

std::optional<std::vector<std::uint8_t>> zlib_decompress(std::vector<std::uint8_t> const& data,
    std::size_t max_output)
{
    if (data.size() < 6)
        return std::nullopt;
    std::uint8_t const cmf = data[0];
    std::uint8_t const flg = data[1];
    if ((cmf & 0x0F) != 8) // compression method: deflate
        return std::nullopt;
    if ((static_cast<unsigned>(cmf) * 256 + flg) % 31 != 0)
        return std::nullopt;
    if (flg & 0x20) // FDICT: preset dictionaries are not used on the web
        return std::nullopt;

    BitReader reader { data, 2, 0, false };
    std::optional<std::vector<std::uint8_t>> out = inflate_from(reader, max_output);
    if (!out)
        return std::nullopt;
    reader.align();
    if (reader.byte + 4 > data.size())
        return std::nullopt;
    std::uint32_t const stated = (static_cast<std::uint32_t>(data[reader.byte]) << 24)
        | (static_cast<std::uint32_t>(data[reader.byte + 1]) << 16)
        | (static_cast<std::uint32_t>(data[reader.byte + 2]) << 8)
        | static_cast<std::uint32_t>(data[reader.byte + 3]);
    if (adler32(*out) != stated)
        return std::nullopt;
    return out;
}

std::optional<std::vector<std::uint8_t>> gzip_decompress(std::vector<std::uint8_t> const& data,
    std::size_t max_output)
{
    if (data.size() < 18)
        return std::nullopt;
    if (data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) // magic + deflate
        return std::nullopt;
    std::uint8_t const flags = data[3];
    if (flags & 0xE0) // reserved bits must be zero
        return std::nullopt;
    std::size_t at = 10;
    if (flags & 0x04) { // FEXTRA
        if (at + 2 > data.size())
            return std::nullopt;
        std::size_t const extra
            = data[at] | (static_cast<std::size_t>(data[at + 1]) << 8);
        at += 2 + extra;
    }
    for (int field = 0; field < 2; ++field) { // FNAME then FCOMMENT
        if (!(flags & (field == 0 ? 0x08 : 0x10)))
            continue;
        while (at < data.size() && data[at] != 0)
            ++at;
        if (at >= data.size())
            return std::nullopt;
        ++at; // the NUL
    }
    if (flags & 0x02) // FHCRC
        at += 2;
    if (at >= data.size())
        return std::nullopt;

    BitReader reader { data, at, 0, false };
    std::optional<std::vector<std::uint8_t>> out = inflate_from(reader, max_output);
    if (!out)
        return std::nullopt;
    reader.align();
    if (reader.byte + 8 > data.size())
        return std::nullopt;
    auto const le32 = [&](std::size_t i) {
        return static_cast<std::uint32_t>(data[i]) | (static_cast<std::uint32_t>(data[i + 1]) << 8)
            | (static_cast<std::uint32_t>(data[i + 2]) << 16)
            | (static_cast<std::uint32_t>(data[i + 3]) << 24);
    };
    if (crc32_of(*out) != le32(reader.byte))
        return std::nullopt;
    if (le32(reader.byte + 4) != static_cast<std::uint32_t>(out->size() & 0xFFFFFFFFu))
        return std::nullopt;
    return out;
}

}
