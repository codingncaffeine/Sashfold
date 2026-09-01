#include "core/Png.h"

#include "core/Bitmap.h"

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

// zlib stream (RFC 1950) wrapping a deflate stream of stored blocks (RFC 1951).
std::vector<std::uint8_t> zlib_stored(std::vector<std::uint8_t> const& raw)
{
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);

    out.push_back(0x78); // CM = 8 (deflate), CINFO = 7 (32K window)
    out.push_back(0x01); // FCHECK such that 0x7801 is a multiple of 31, no dictionary

    std::size_t offset = 0;
    do {
        std::size_t const remaining = raw.size() - offset;
        std::uint16_t const block = static_cast<std::uint16_t>(remaining > 65535u ? 65535u : remaining);
        bool const is_final = (offset + block) >= raw.size();

        out.push_back(is_final ? 0x01 : 0x00); // BFINAL, BTYPE = 00 (stored)
        out.push_back(static_cast<std::uint8_t>(block & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((block >> 8) & 0xFFu));
        std::uint16_t const inverted = static_cast<std::uint16_t>(~block);
        out.push_back(static_cast<std::uint8_t>(inverted & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((inverted >> 8) & 0xFFu));

        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
            raw.begin() + static_cast<std::ptrdiff_t>(offset + block));

        offset += block;
    } while (offset < raw.size());

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

    push_chunk(out, "IDAT", zlib_stored(raw));
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
