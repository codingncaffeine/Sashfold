// Encodes a bitmap, then parses the result back with an INDEPENDENT decoder
// written here on purpose: reusing the encoder's own CRC routine would let a
// wrong-but-consistent implementation pass. This walks the chunk structure,
// re-derives every CRC, inflates the stored deflate blocks, and compares the
// recovered scanlines against the source pixels.

#include "Test.h"

#include "core/Bitmap.h"
#include "core/Png.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// --- Building PNGs by hand for the decoder ----------------------------------------

std::uint32_t independent_adler32(std::vector<std::uint8_t> const& data)
{
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t const byte : data) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return b << 16 | a;
}

// zlib with stored blocks only: valid for any decoder, no compression.
std::vector<std::uint8_t> stored_zlib(std::vector<std::uint8_t> const& raw)
{
    std::vector<std::uint8_t> out { 0x78, 0x01 };
    std::size_t at = 0;
    do {
        std::size_t const length = std::min<std::size_t>(65535, raw.size() - at);
        bool const last = at + length >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length & 0xFF));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        out.push_back(static_cast<std::uint8_t>(~length & 0xFF));
        out.push_back(static_cast<std::uint8_t>((~length >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
            raw.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < raw.size());
    std::uint32_t const adler = independent_adler32(raw);
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(adler >> shift));
    return out;
}

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put_chunk(std::vector<std::uint8_t>& out, char const* type, std::vector<std::uint8_t> const& data,
    bool corrupt_crc = false)
{
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), data.begin(), data.end());
    out.insert(out.end(), typed.begin(), typed.end());
    put_be32(out, independent_crc32(typed.data(), typed.size()) ^ (corrupt_crc ? 1u : 0u));
}

struct HandMadePng {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int depth = 8;
    int color_type = 6;
    bool interlaced = false;
    std::vector<std::uint8_t> palette; // RGB triples
    std::vector<std::uint8_t> trns;
    std::vector<std::uint8_t> raw; // filter bytes and scanlines, already in file order
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> extra_before_idat;
    bool corrupt_ihdr_crc = false;
    bool omit_iend = false;
};

std::vector<std::uint8_t> build_png(HandMadePng const& spec)
{
    std::vector<std::uint8_t> out { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, spec.width);
    put_be32(ihdr, spec.height);
    ihdr.push_back(static_cast<std::uint8_t>(spec.depth));
    ihdr.push_back(static_cast<std::uint8_t>(spec.color_type));
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(spec.interlaced ? 1 : 0);
    put_chunk(out, "IHDR", ihdr, spec.corrupt_ihdr_crc);
    if (!spec.palette.empty())
        put_chunk(out, "PLTE", spec.palette);
    if (!spec.trns.empty())
        put_chunk(out, "tRNS", spec.trns);
    for (auto const& [type, data] : spec.extra_before_idat)
        put_chunk(out, type.c_str(), data);
    put_chunk(out, "IDAT", stored_zlib(spec.raw));
    if (!spec.omit_iend)
        put_chunk(out, "IEND", {});
    return out;
}

// Scanlines of `bytes_per_row` from a flat sample buffer, each with a filter
// byte: type 0 unless `filters` says otherwise per row, applied forward.
std::vector<std::uint8_t> filtered_rows(std::vector<std::uint8_t> const& samples, std::size_t bytes_per_row,
    std::size_t bpp, std::vector<std::uint8_t> const& filters = {})
{
    std::vector<std::uint8_t> out;
    std::size_t const rows = samples.size() / bytes_per_row;
    std::vector<std::uint8_t> previous(bytes_per_row, 0);
    for (std::size_t y = 0; y < rows; ++y) {
        std::uint8_t const filter = y < filters.size() ? filters[y] : 0;
        std::vector<std::uint8_t> const line(samples.begin() + static_cast<std::ptrdiff_t>(y * bytes_per_row),
            samples.begin() + static_cast<std::ptrdiff_t>((y + 1) * bytes_per_row));
        out.push_back(filter);
        for (std::size_t i = 0; i < bytes_per_row; ++i) {
            int const a = i >= bpp ? line[i - bpp] : 0;
            int const b = previous[i];
            int const c = i >= bpp ? previous[i - bpp] : 0;
            int predictor = 0;
            if (filter == 1)
                predictor = a;
            else if (filter == 2)
                predictor = b;
            else if (filter == 3)
                predictor = (a + b) / 2;
            else if (filter == 4) {
                int const p = a + b - c;
                int const pa = std::abs(p - a);
                int const pb = std::abs(p - b);
                int const pc = std::abs(p - c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            }
            out.push_back(static_cast<std::uint8_t>(line[i] - predictor));
        }
        previous = line;
    }
    return out;
}

bool same_pixels(Bitmap const& a, Bitmap const& b)
{
    return a.width() == b.width() && a.height() == b.height() && a.pixels() == b.pixels();
}

int main(int argc, char** argv)
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

    // --- Decoding: the encoder's output comes back whole ----------------------------
    {
        Bitmap source(37, 23, Color::rgba(0, 0, 0, 0));
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                source.set_pixel(x, y, Color::rgba(static_cast<std::uint8_t>(x * 7), static_cast<std::uint8_t>(y * 11),
                    static_cast<std::uint8_t>((x + y) * 3), static_cast<std::uint8_t>(255 - x * 2)));
            }
        }
        std::optional<Bitmap> const decoded = decode_png(encode_png(source));
        CHECK(decoded.has_value());
        if (decoded)
            CHECK(same_pixels(*decoded, source));
        CHECK(looks_like_png(encode_png(source)));
        CHECK(!looks_like_png({ 1, 2, 3 }));
    }

    // --- Every color type and depth --------------------------------------------------
    {
        // Grayscale 1-bit: a 10x2 checkerboard.
        HandMadePng gray1;
        gray1.width = 10;
        gray1.height = 2;
        gray1.depth = 1;
        gray1.color_type = 0;
        gray1.raw = filtered_rows({ 0b10101010, 0b10000000, 0b01010101, 0b01000000 }, 2, 1);
        std::optional<Bitmap> const g1 = decode_png(build_png(gray1));
        if (CHECK(g1.has_value())) {
            CHECK(g1->pixel(0, 0) == Color::rgb(255, 255, 255));
            CHECK(g1->pixel(1, 0) == Color::rgb(0, 0, 0));
            CHECK(g1->pixel(9, 0) == Color::rgb(0, 0, 0));
            CHECK(g1->pixel(0, 1) == Color::rgb(0, 0, 0));
            CHECK(g1->pixel(9, 1) == Color::rgb(255, 255, 255));
        }
        // Grayscale 2- and 4-bit scale to the full range.
        HandMadePng gray2;
        gray2.width = 4;
        gray2.height = 1;
        gray2.depth = 2;
        gray2.color_type = 0;
        gray2.raw = filtered_rows({ 0b00011011 }, 1, 1);
        std::optional<Bitmap> const g2 = decode_png(build_png(gray2));
        if (CHECK(g2.has_value())) {
            CHECK(g2->pixel(0, 0) == Color::rgb(0, 0, 0));
            CHECK(g2->pixel(1, 0) == Color::rgb(85, 85, 85));
            CHECK(g2->pixel(2, 0) == Color::rgb(170, 170, 170));
            CHECK(g2->pixel(3, 0) == Color::rgb(255, 255, 255));
        }
        HandMadePng gray4;
        gray4.width = 2;
        gray4.height = 1;
        gray4.depth = 4;
        gray4.color_type = 0;
        gray4.raw = filtered_rows({ 0x3F }, 1, 1);
        std::optional<Bitmap> const g4 = decode_png(build_png(gray4));
        if (CHECK(g4.has_value())) {
            CHECK(g4->pixel(0, 0) == Color::rgb(51, 51, 51));
            CHECK(g4->pixel(1, 0) == Color::rgb(255, 255, 255));
        }
        // Grayscale 16-bit keeps the high byte; a tRNS key makes one value clear.
        HandMadePng gray16;
        gray16.width = 2;
        gray16.height = 1;
        gray16.depth = 16;
        gray16.color_type = 0;
        gray16.trns = { 0x12, 0x34 };
        gray16.raw = filtered_rows({ 0x80, 0x00, 0x12, 0x34 }, 4, 2);
        std::optional<Bitmap> const g16 = decode_png(build_png(gray16));
        if (CHECK(g16.has_value())) {
            CHECK(g16->pixel(0, 0) == Color::rgb(128, 128, 128));
            CHECK_EQ(static_cast<int>(g16->pixel(1, 0).a), 0);
        }
        // Indexed 2-bit with a palette and partial tRNS.
        HandMadePng indexed;
        indexed.width = 4;
        indexed.height = 1;
        indexed.depth = 2;
        indexed.color_type = 3;
        indexed.palette = { 255, 0, 0, 0, 255, 0, 0, 0, 255 }; // three entries; index 3 is out of range
        indexed.trns = { 255, 0 }; // entry 1 is transparent
        indexed.raw = filtered_rows({ 0b00011011 }, 1, 1);
        std::optional<Bitmap> const idx = decode_png(build_png(indexed));
        if (CHECK(idx.has_value())) {
            CHECK(idx->pixel(0, 0) == Color::rgb(255, 0, 0));
            CHECK(idx->pixel(1, 0) == Color::rgba(0, 255, 0, 0));
            CHECK(idx->pixel(2, 0) == Color::rgb(0, 0, 255));
            CHECK(idx->pixel(3, 0) == Color::rgb(0, 0, 0)); // out of range: black
        }
        // Truecolor 8-bit with a color key; 16-bit truecolor.
        HandMadePng rgb;
        rgb.width = 2;
        rgb.height = 1;
        rgb.depth = 8;
        rgb.color_type = 2;
        rgb.trns = { 0, 10, 0, 20, 0, 30 };
        rgb.raw = filtered_rows({ 10, 20, 30, 40, 50, 60 }, 6, 3);
        std::optional<Bitmap> const rgb8 = decode_png(build_png(rgb));
        if (CHECK(rgb8.has_value())) {
            CHECK(rgb8->pixel(0, 0) == Color::rgba(10, 20, 30, 0));
            CHECK(rgb8->pixel(1, 0) == Color::rgb(40, 50, 60));
        }
        HandMadePng rgb16;
        rgb16.width = 1;
        rgb16.height = 1;
        rgb16.depth = 16;
        rgb16.color_type = 2;
        rgb16.raw = filtered_rows({ 0xFF, 0xFF, 0x80, 0x01, 0x00, 0x00 }, 6, 6);
        std::optional<Bitmap> const r16 = decode_png(build_png(rgb16));
        if (CHECK(r16.has_value()))
            CHECK(r16->pixel(0, 0) == Color::rgb(255, 128, 0));
        // Gray + alpha 8-bit; RGBA 16-bit.
        HandMadePng ga;
        ga.width = 1;
        ga.height = 1;
        ga.depth = 8;
        ga.color_type = 4;
        ga.raw = filtered_rows({ 200, 100 }, 2, 2);
        std::optional<Bitmap> const ga8 = decode_png(build_png(ga));
        if (CHECK(ga8.has_value()))
            CHECK(ga8->pixel(0, 0) == Color::rgba(200, 200, 200, 100));
        HandMadePng rgba16;
        rgba16.width = 1;
        rgba16.height = 1;
        rgba16.depth = 16;
        rgba16.color_type = 6;
        rgba16.raw = filtered_rows({ 0x11, 0x00, 0x22, 0x00, 0x33, 0x00, 0x44, 0x00 }, 8, 8);
        std::optional<Bitmap> const a16 = decode_png(build_png(rgba16));
        if (CHECK(a16.has_value()))
            CHECK(a16->pixel(0, 0) == Color::rgba(0x11, 0x22, 0x33, 0x44));
    }

    // --- Every filter, then Adam7 --------------------------------------------------------
    {
        Bitmap source(5, 5, Color::rgba(0, 0, 0, 0));
        std::vector<std::uint8_t> samples;
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 5; ++x) {
                Color const c = Color::rgba(static_cast<std::uint8_t>(x * 50 + y), static_cast<std::uint8_t>(y * 40),
                    static_cast<std::uint8_t>(x * y * 9), static_cast<std::uint8_t>(255 - y * 30));
                source.set_pixel(x, y, c);
                samples.insert(samples.end(), { c.r, c.g, c.b, c.a });
            }
        }
        HandMadePng filtered;
        filtered.width = 5;
        filtered.height = 5;
        filtered.raw = filtered_rows(samples, 20, 4, { 0, 1, 2, 3, 4 });
        std::optional<Bitmap> const unfiltered = decode_png(build_png(filtered));
        CHECK(unfiltered && same_pixels(*unfiltered, source));

        // Interlaced: the same 5x5, written pass by pass.
        HandMadePng interlaced;
        interlaced.width = 5;
        interlaced.height = 5;
        interlaced.interlaced = true;
        struct Step {
            int xs, ys, xstep, ystep;
        };
        Step const steps[7] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 },
            { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
        int pass_filter = 0;
        for (Step const& step : steps) {
            std::vector<std::uint8_t> pass_samples;
            int rows = 0;
            int columns = 0;
            for (int y = step.ys; y < 5; y += step.ystep) {
                ++rows;
                columns = 0;
                for (int x = step.xs; x < 5; x += step.xstep) {
                    Color const c = source.pixel(x, y);
                    pass_samples.insert(pass_samples.end(), { c.r, c.g, c.b, c.a });
                    ++columns;
                }
            }
            if (rows == 0 || columns == 0)
                continue;
            std::vector<std::uint8_t> const filters(static_cast<std::size_t>(rows),
                static_cast<std::uint8_t>(pass_filter++ % 5));
            std::vector<std::uint8_t> const pass_rows
                = filtered_rows(pass_samples, static_cast<std::size_t>(columns) * 4, 4, filters);
            interlaced.raw.insert(interlaced.raw.end(), pass_rows.begin(), pass_rows.end());
        }
        std::optional<Bitmap> const deinterlaced = decode_png(build_png(interlaced));
        CHECK(deinterlaced && same_pixels(*deinterlaced, source));
    }

    // --- Malformed input never decodes, and never crashes -------------------------------
    {
        HandMadePng good;
        good.width = 2;
        good.height = 2;
        good.raw = filtered_rows(std::vector<std::uint8_t>(16, 7), 8, 4);
        std::vector<std::uint8_t> const bytes = build_png(good);
        CHECK(decode_png(bytes).has_value());
        CHECK(!decode_png({}).has_value());
        CHECK(!decode_png(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 20)).has_value());
        CHECK(!decode_png(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 13)).has_value()); // IDAT cut
        for (std::size_t length = 0; length < bytes.size(); length += 3)
            (void)decode_png(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length)));
        std::vector<std::uint8_t> flipped = bytes;
        flipped[1] = 'Q';
        CHECK(!decode_png(flipped).has_value());
        HandMadePng bad_crc = good;
        bad_crc.corrupt_ihdr_crc = true;
        CHECK(!decode_png(build_png(bad_crc)).has_value());
        HandMadePng no_iend = good;
        no_iend.omit_iend = true;
        CHECK(decode_png(build_png(no_iend)).has_value()); // lenient: the image is whole
        HandMadePng short_data = good;
        short_data.raw.pop_back();
        CHECK(!decode_png(build_png(short_data)).has_value());
        HandMadePng bad_depth = good;
        bad_depth.color_type = 2;
        bad_depth.depth = 4;
        CHECK(!decode_png(build_png(bad_depth)).has_value());
        HandMadePng no_palette = good;
        no_palette.color_type = 3;
        no_palette.depth = 8;
        no_palette.raw = filtered_rows({ 0, 0, 0, 0 }, 2, 1);
        CHECK(!decode_png(build_png(no_palette)).has_value());
        HandMadePng huge = good;
        huge.width = 60000;
        huge.height = 60000;
        CHECK(!decode_png(build_png(huge)).has_value());
        HandMadePng ancillary = good;
        ancillary.extra_before_idat.push_back({ "tEXt", { 'a', 0, 'b' } });
        ancillary.extra_before_idat.push_back({ "gAMA", { 0, 0, 0xB1, 0x8F } });
        CHECK(decode_png(build_png(ancillary)).has_value());
        std::vector<std::uint8_t> ancillary_bad = build_png(ancillary);
        // Corrupt the tEXt chunk's CRC: ancillary, so it is skipped, and the image still decodes.
        std::size_t const text_at = std::string(ancillary_bad.begin(), ancillary_bad.end()).find("tEXt");
        ancillary_bad[text_at + 4 + 3 + 3] ^= 0xFF;
        CHECK(decode_png(ancillary_bad).has_value());
        HandMadePng unknown_critical = good;
        unknown_critical.extra_before_idat.push_back({ "ZZZZ", { 1 } });
        CHECK(!decode_png(build_png(unknown_critical)).has_value());
        HandMadePng bad_filter = good;
        bad_filter.raw[0] = 9;
        CHECK(!decode_png(build_png(bad_filter)).has_value());
        CHECK(!decode_png(bytes, 3).has_value()); // over the pixel budget
    }

    // --- The repository's own PNGs decode and re-encode to the same bytes -------------
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path const directory(argv[i]);
        std::error_code error;
        for (auto const& entry : std::filesystem::directory_iterator(directory, error)) {
            if (entry.path().extension() != ".png")
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> const bytes((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            std::optional<Bitmap> const decoded = decode_png(bytes);
            if (!CHECK(decoded.has_value()))
                continue;
            CHECK(decoded->width() > 0 && decoded->height() > 0);
            CHECK(encode_png(*decoded) == bytes);
        }
    }

    return sashfold::test::report("png");
}
