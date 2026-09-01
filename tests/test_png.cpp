// Encodes a bitmap, then parses the result back with an INDEPENDENT decoder
// written here on purpose: reusing the encoder's own CRC routine would let a
// wrong-but-consistent implementation pass. This walks the chunk structure,
// re-derives every CRC, inflates the stored deflate blocks, and compares the
// recovered scanlines against the source pixels.

#include "Test.h"

#include "core/Bitmap.h"
#include "core/Png.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

std::uint32_t independent_crc32(std::uint8_t const* data, std::size_t length)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            std::uint32_t const mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::uint32_t read_be32(std::vector<std::uint8_t> const& data, std::size_t at)
{
    return (static_cast<std::uint32_t>(data[at]) << 24)
        | (static_cast<std::uint32_t>(data[at + 1]) << 16)
        | (static_cast<std::uint32_t>(data[at + 2]) << 8)
        | static_cast<std::uint32_t>(data[at + 3]);
}

struct Chunk {
    std::string type;
    std::vector<std::uint8_t> data;
    bool crc_ok = false;
};

std::vector<Chunk> parse_chunks(std::vector<std::uint8_t> const& png)
{
    std::vector<Chunk> chunks;
    std::size_t at = 8; // skip signature
    while (at + 12 <= png.size()) {
        std::uint32_t const length = read_be32(png, at);
        if (at + 12 + length > png.size())
            break;

        Chunk chunk;
        chunk.type.assign(png.begin() + static_cast<std::ptrdiff_t>(at + 4),
            png.begin() + static_cast<std::ptrdiff_t>(at + 8));
        chunk.data.assign(png.begin() + static_cast<std::ptrdiff_t>(at + 8),
            png.begin() + static_cast<std::ptrdiff_t>(at + 8 + length));

        std::uint32_t const stated = read_be32(png, at + 8 + length);
        chunk.crc_ok = independent_crc32(png.data() + at + 4, 4u + length) == stated;

        chunks.push_back(std::move(chunk));
        at += 12 + length;
    }
    return chunks;
}

// An independent inflate for the test: stored and fixed-Huffman blocks,
// written straight from RFC 1951 without looking at the encoder.
struct BitReader {
    std::vector<std::uint8_t> const& data;
    std::size_t byte = 0;
    int bit = 0;
    bool overrun = false;

    unsigned take(int count) // LSB-first data bits
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

    unsigned take_reversed(int count) // Huffman codes arrive MSB-first
    {
        unsigned value = 0;
        for (int i = 0; i < count; ++i)
            value = (value << 1) | take(1);
        return value;
    }

    void align() // to the next byte boundary (stored blocks)
    {
        if (bit != 0) {
            bit = 0;
            ++byte;
        }
    }
};

// Reads one fixed-Huffman literal/length symbol: 7 bits first, extending a
// bit at a time through the 8- and 9-bit code ranges (canonical decode).
std::optional<unsigned> read_fixed_symbol(BitReader& reader)
{
    unsigned code = reader.take_reversed(7);
    if (reader.overrun)
        return std::nullopt;
    if (code <= 0x17)
        return 256 + code;
    code = (code << 1) | reader.take(1);
    if (reader.overrun)
        return std::nullopt;
    if (code >= 0x30 && code <= 0xBF)
        return code - 0x30;
    if (code >= 0xC0 && code <= 0xC7)
        return 280 + (code - 0xC0);
    code = (code << 1) | reader.take(1);
    if (reader.overrun)
        return std::nullopt;
    if (code >= 0x190 && code <= 0x1FF)
        return 144 + (code - 0x190);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> independent_inflate(std::vector<std::uint8_t> const& zlib)
{
    if (zlib.size() < 6)
        return std::nullopt;
    if ((static_cast<unsigned>(zlib[0]) << 8 | zlib[1]) % 31u != 0u)
        return std::nullopt; // zlib header check bits

    static constexpr unsigned length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23,
        27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static constexpr int length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
        3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static constexpr unsigned distance_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65,
        97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
        24577 };
    static constexpr int distance_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7,
        7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

    BitReader reader { zlib, 2, 0, false };
    std::vector<std::uint8_t> out;
    while (true) {
        unsigned const is_final = reader.take(1);
        unsigned const type = reader.take(2);
        if (reader.overrun)
            return std::nullopt;
        if (type == 0) { // stored
            reader.align();
            if (reader.byte + 4 > zlib.size())
                return std::nullopt;
            unsigned const length = zlib[reader.byte] | (zlib[reader.byte + 1] << 8);
            unsigned const inverted = zlib[reader.byte + 2] | (zlib[reader.byte + 3] << 8);
            if ((length ^ inverted) != 0xFFFFu)
                return std::nullopt;
            reader.byte += 4;
            if (reader.byte + length > zlib.size())
                return std::nullopt;
            out.insert(out.end(), zlib.begin() + static_cast<std::ptrdiff_t>(reader.byte),
                zlib.begin() + static_cast<std::ptrdiff_t>(reader.byte + length));
            reader.byte += length;
        } else if (type == 1) { // fixed Huffman
            while (true) {
                std::optional<unsigned> const symbol = read_fixed_symbol(reader);
                if (!symbol || reader.overrun)
                    return std::nullopt;
                if (*symbol == 256)
                    break;
                if (*symbol < 256) {
                    out.push_back(static_cast<std::uint8_t>(*symbol));
                    continue;
                }
                unsigned const length_index = *symbol - 257;
                if (length_index >= 29)
                    return std::nullopt;
                unsigned const length = length_base[length_index]
                    + reader.take(length_extra[length_index]);
                unsigned const distance_symbol = reader.take_reversed(5);
                if (distance_symbol >= 30)
                    return std::nullopt;
                unsigned const distance = distance_base[distance_symbol]
                    + reader.take(distance_extra[distance_symbol]);
                if (reader.overrun || distance > out.size())
                    return std::nullopt;
                for (unsigned i = 0; i < length; ++i)
                    out.push_back(out[out.size() - distance]);
            }
        } else {
            return std::nullopt; // dynamic Huffman: not needed for our encoder
        }
        if (is_final)
            return out;
    }
}

}

int main()
{
    Bitmap bitmap(3, 2, Color::rgb(255, 255, 255));
    bitmap.set_pixel(0, 0, Color::rgb(255, 0, 0));
    bitmap.set_pixel(2, 1, Color::rgba(0, 128, 255, 64));

    std::vector<std::uint8_t> const png = encode_png(bitmap);

    // Signature.
    std::array<std::uint8_t, 8> const signature { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    CHECK(png.size() > 8);
    for (std::size_t i = 0; i < signature.size(); ++i)
        CHECK_EQ(static_cast<int>(png[i]), static_cast<int>(signature[i]));

    std::vector<Chunk> const chunks = parse_chunks(png);
    CHECK_EQ(chunks.size(), std::size_t { 3 });
    if (chunks.size() != 3)
        return sashfold::test::report("png");

    for (Chunk const& chunk : chunks)
        CHECK(chunk.crc_ok);

    // IHDR describes the image we handed in.
    CHECK_EQ(chunks[0].type, std::string("IHDR"));
    CHECK_EQ(chunks[0].data.size(), std::size_t { 13 });
    CHECK_EQ(read_be32(chunks[0].data, 0), std::uint32_t { 3 }); // width
    CHECK_EQ(read_be32(chunks[0].data, 4), std::uint32_t { 2 }); // height
    CHECK_EQ(static_cast<int>(chunks[0].data[8]), 8); // bit depth
    CHECK_EQ(static_cast<int>(chunks[0].data[9]), 6); // RGBA
    CHECK_EQ(static_cast<int>(chunks[0].data[12]), 0); // not interlaced

    CHECK_EQ(chunks[2].type, std::string("IEND"));
    CHECK_EQ(chunks[2].data.size(), std::size_t { 0 });

    // IDAT must inflate back to exactly the scanlines we fed the encoder.
    CHECK_EQ(chunks[1].type, std::string("IDAT"));
    std::optional<std::vector<std::uint8_t>> const raw = independent_inflate(chunks[1].data);
    CHECK(raw.has_value());
    if (raw.has_value()) {
        std::size_t const stride = 3u * 4u;
        CHECK_EQ(raw->size(), (stride + 1u) * 2u);

        for (int y = 0; y < bitmap.height(); ++y) {
            std::size_t const row = static_cast<std::size_t>(y) * (stride + 1u);
            CHECK_EQ(static_cast<int>((*raw)[row]), 0); // filter type None
            for (int x = 0; x < bitmap.width(); ++x) {
                Color const expected = bitmap.pixel(x, y);
                std::size_t const at = row + 1u + static_cast<std::size_t>(x) * 4u;
                CHECK_EQ(static_cast<int>((*raw)[at + 0]), static_cast<int>(expected.r));
                CHECK_EQ(static_cast<int>((*raw)[at + 1]), static_cast<int>(expected.g));
                CHECK_EQ(static_cast<int>((*raw)[at + 2]), static_cast<int>(expected.b));
                CHECK_EQ(static_cast<int>((*raw)[at + 3]), static_cast<int>(expected.a));
            }
        }
    }

    // A bitmap larger than one stored block must still round-trip.
    Bitmap wide(400, 60, Color::rgb(10, 20, 30));
    std::vector<std::uint8_t> const big = encode_png(wide);
    std::vector<Chunk> const big_chunks = parse_chunks(big);
    CHECK_EQ(big_chunks.size(), std::size_t { 3 });
    if (big_chunks.size() == 3) {
        CHECK(big_chunks[1].crc_ok);
        std::optional<std::vector<std::uint8_t>> const big_raw = independent_inflate(big_chunks[1].data);
        CHECK(big_raw.has_value());
        if (big_raw.has_value())
            CHECK_EQ(big_raw->size(), (400u * 4u + 1u) * 60u);
    }

    return sashfold::test::report("png");
}
