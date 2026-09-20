#include "core/Png.h"

#include "core/Bitmap.h"
#include "core/Inflate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

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

// --- Decoding -------------------------------------------------------------------

namespace {

constexpr std::array<std::uint8_t, 8> png_signature { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

std::uint32_t read_be32(std::uint8_t const* at)
{
    return static_cast<std::uint32_t>(at[0]) << 24 | static_cast<std::uint32_t>(at[1]) << 16
        | static_cast<std::uint32_t>(at[2]) << 8 | static_cast<std::uint32_t>(at[3]);
}

int channels_of(int color_type)
{
    switch (color_type) {
    case 0: return 1; // grayscale
    case 2: return 3; // truecolor
    case 3: return 1; // indexed
    case 4: return 2; // grayscale + alpha
    case 6: return 4; // truecolor + alpha
    default: return 0;
    }
}

bool valid_depth(int color_type, int depth)
{
    switch (color_type) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    default: return false;
    }
}

std::uint8_t paeth(int a, int b, int c)
{
    int const p = a + b - c;
    int const pa = std::abs(p - a);
    int const pb = std::abs(p - b);
    int const pc = std::abs(p - c);
    if (pa <= pb && pa <= pc)
        return static_cast<std::uint8_t>(a);
    if (pb <= pc)
        return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

// Reverses one scanline's filter in place; `previous` is the line above,
// zeros for the first line of a pass.
bool unfilter(std::uint8_t filter, std::uint8_t* line, std::uint8_t const* previous,
    std::size_t length, std::size_t bpp)
{
    switch (filter) {
    case 0:
        return true;
    case 1:
        for (std::size_t i = bpp; i < length; ++i)
            line[i] = static_cast<std::uint8_t>(line[i] + line[i - bpp]);
        return true;
    case 2:
        for (std::size_t i = 0; i < length; ++i)
            line[i] = static_cast<std::uint8_t>(line[i] + previous[i]);
        return true;
    case 3:
        for (std::size_t i = 0; i < length; ++i) {
            int const left = i >= bpp ? line[i - bpp] : 0;
            line[i] = static_cast<std::uint8_t>(line[i] + (left + previous[i]) / 2);
        }
        return true;
    case 4:
        for (std::size_t i = 0; i < length; ++i) {
            int const a = i >= bpp ? line[i - bpp] : 0;
            int const b = previous[i];
            int const c = i >= bpp ? previous[i - bpp] : 0;
            line[i] = static_cast<std::uint8_t>(line[i] + paeth(a, b, c));
        }
        return true;
    default:
        return false;
    }
}

// Sample `index` of a scanline at the bit depth, most significant bits first.
std::uint32_t sample_at(std::uint8_t const* line, std::size_t index, int depth)
{
    switch (depth) {
    case 1: return (line[index >> 3] >> (7 - (index & 7))) & 1u;
    case 2: return (line[index >> 2] >> (6 - 2 * (index & 3))) & 3u;
    case 4: return (line[index >> 1] >> (4 - 4 * (index & 1))) & 15u;
    case 8: return line[index];
    default: return static_cast<std::uint32_t>(line[index * 2]) << 8 | line[index * 2 + 1];
    }
}

std::uint8_t to_8_bits(std::uint32_t value, int depth)
{
    switch (depth) {
    case 1: return value ? 255 : 0;
    case 2: return static_cast<std::uint8_t>(value * 85);
    case 4: return static_cast<std::uint8_t>(value * 17);
    case 8: return static_cast<std::uint8_t>(value);
    default: return static_cast<std::uint8_t>(value >> 8);
    }
}

struct Interlace {
    std::uint32_t x_start, y_start, x_step, y_step;
};
constexpr std::array<Interlace, 7> adam7 { {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 },
    { 0, 1, 1, 2 } } };

} // namespace

namespace {

using Format = PngSamples;

// A zlib stream of filtered scanlines into a picture of the size given: the
// whole of a still PNG, or one frame of an animated one, which has a size
// of its own.
std::optional<Bitmap> decode_pixels(Format const& format, std::uint32_t image_width, std::uint32_t image_height,
    std::vector<std::uint8_t> const& zlib_data)
{
    int const channels = channels_of(format.color_type);
    std::size_t const bits_per_pixel = static_cast<std::size_t>(channels) * static_cast<std::size_t>(format.depth);
    std::size_t const bpp = std::max<std::size_t>(1, bits_per_pixel / 8);
    auto const row_bytes = [&](std::uint32_t width) {
        return (static_cast<std::size_t>(width) * bits_per_pixel + 7) / 8;
    };
    struct Pass {
        std::uint32_t width, height, x_start, y_start, x_step, y_step;
    };
    std::vector<Pass> passes;
    if (format.interlaced) {
        for (Interlace const& step : adam7) {
            std::uint32_t const width
                = image_width > step.x_start ? (image_width - step.x_start + step.x_step - 1) / step.x_step : 0;
            std::uint32_t const height
                = image_height > step.y_start ? (image_height - step.y_start + step.y_step - 1) / step.y_step : 0;
            if (width && height)
                passes.push_back(Pass { width, height, step.x_start, step.y_start, step.x_step, step.y_step });
        }
    } else {
        passes.push_back(Pass { image_width, image_height, 0, 0, 1, 1 });
    }
    std::size_t expected = 0;
    for (Pass const& pass : passes)
        expected += static_cast<std::size_t>(pass.height) * (1 + row_bytes(pass.width));
    std::optional<std::vector<std::uint8_t>> raw = zlib_decompress(zlib_data, expected);
    if (!raw || raw->size() != expected)
        return std::nullopt;

    Bitmap out(static_cast<int>(image_width), static_cast<int>(image_height), Color::rgba(0, 0, 0, 0));
    int const depth = format.depth;
    int const color_type = format.color_type;
    bool const has_key = format.has_key;
    std::array<std::uint32_t, 3> const& key = format.key;
    std::vector<Color> const& palette = format.palette;
    auto const store = [&](std::uint8_t const* line, Pass const& pass, std::uint32_t y) {
        for (std::uint32_t i = 0; i < pass.width; ++i) {
            Color color;
            switch (color_type) {
            case 0: {
                std::uint32_t const v = sample_at(line, i, depth);
                std::uint8_t const g = to_8_bits(v, depth);
                color = Color::rgba(g, g, g, has_key && v == key[0] ? 0 : 255);
                break;
            }
            case 2: {
                std::uint32_t const r = sample_at(line, i * 3, depth);
                std::uint32_t const g = sample_at(line, i * 3 + 1, depth);
                std::uint32_t const b = sample_at(line, i * 3 + 2, depth);
                bool const keyed = has_key && r == key[0] && g == key[1] && b == key[2];
                color = Color::rgba(to_8_bits(r, depth), to_8_bits(g, depth), to_8_bits(b, depth), keyed ? 0 : 255);
                break;
            }
            case 3: {
                std::uint32_t const index = sample_at(line, i, depth);
                color = index < palette.size() ? palette[index] : Color::rgb(0, 0, 0);
                break;
            }
            case 4: {
                std::uint8_t const g = to_8_bits(sample_at(line, i * 2, depth), depth);
                color = Color::rgba(g, g, g, to_8_bits(sample_at(line, i * 2 + 1, depth), depth));
                break;
            }
            default: {
                color = Color::rgba(to_8_bits(sample_at(line, i * 4, depth), depth),
                    to_8_bits(sample_at(line, i * 4 + 1, depth), depth),
                    to_8_bits(sample_at(line, i * 4 + 2, depth), depth),
                    to_8_bits(sample_at(line, i * 4 + 3, depth), depth));
                break;
            }
            }
            out.set_pixel(static_cast<int>(pass.x_start + i * pass.x_step), static_cast<int>(y), color);
        }
    };
    std::size_t pos = 0;
    for (Pass const& pass : passes) {
        std::size_t const stride = row_bytes(pass.width);
        std::vector<std::uint8_t> previous(stride, 0);
        for (std::uint32_t py = 0; py < pass.height; ++py) {
            std::uint8_t const filter = (*raw)[pos];
            std::uint8_t* line = raw->data() + pos + 1;
            if (!unfilter(filter, line, previous.data(), stride, bpp))
                return std::nullopt;
            store(line, pass, pass.y_start + py * pass.y_step);
            previous.assign(line, line + stride);
            pos += 1 + stride;
        }
    }
    return out;
}

// A PNG's chunks, walked once: the header, what it says of its samples, the
// still picture's data, and — for an animated one (APNG 1.0) — its frames.
struct Parsed {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Format format;
    std::vector<std::uint8_t> idat;
    bool animated = false; // an acTL came before the picture's data
    std::uint32_t plays = 0;
    std::vector<ApngFrame> frames;
};

std::optional<Parsed> parse(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels, bool still_only)
{
    if (!looks_like_png(bytes))
        return std::nullopt;
    Parsed parsed;
    bool have_header = false;
    bool saw_idat = false;
    bool saw_iend = false;
    constexpr std::size_t max_idat = 64u * 1024u * 1024u;
    constexpr std::size_t max_frames = 10000;
    // The frame control chunk that came last and has had no data yet: the
    // picture's own data belongs to it when it came before that data.
    bool frame_open = false;

    std::size_t at = png_signature.size();
    while (at + 12 <= bytes.size() && !saw_iend) {
        std::uint32_t const length = read_be32(&bytes[at]);
        if (length > bytes.size() - at - 12)
            return std::nullopt; // truncated
        std::uint8_t const* type = &bytes[at + 4];
        std::uint8_t const* data = &bytes[at + 8];
        std::size_t const data_at = at + 8;
        bool const critical = (type[0] & 0x20) == 0;
        bool const crc_ok = crc32(type, 4u + length) == read_be32(data + length);
        std::string const name(reinterpret_cast<char const*>(type), 4);
        at += 12 + length;
        if (!crc_ok) {
            if (critical)
                return std::nullopt;
            continue;
        }
        if (name == "IHDR") {
            if (have_header || length != 13)
                return std::nullopt;
            parsed.width = read_be32(data);
            parsed.height = read_be32(data + 4);
            parsed.format.depth = data[8];
            parsed.format.color_type = data[9];
            parsed.format.interlaced = data[12] == 1;
            if (parsed.width == 0 || parsed.height == 0 || parsed.width > 0x7FFFFFFFu
                || parsed.height > 0x7FFFFFFFu || data[10] != 0 || data[11] != 0
                || (data[12] != 0 && data[12] != 1) || !valid_depth(parsed.format.color_type, parsed.format.depth))
                return std::nullopt;
            if (static_cast<std::uint64_t>(parsed.width) * parsed.height > max_pixels
                || parsed.width > 65535 || parsed.height > 65535)
                return std::nullopt;
            have_header = true;
            continue;
        }
        if (!have_header)
            return std::nullopt; // IHDR comes first
        Format& format = parsed.format;
        if (name == "PLTE") {
            if (length == 0 || length % 3 != 0 || length > 768 || !format.palette.empty() || saw_idat)
                return std::nullopt;
            for (std::uint32_t i = 0; i < length; i += 3)
                format.palette.push_back(Color::rgb(data[i], data[i + 1], data[i + 2]));
        } else if (name == "tRNS") {
            if (saw_idat)
                return std::nullopt;
            if (format.color_type == 3) {
                if (length > format.palette.size())
                    return std::nullopt;
                for (std::uint32_t i = 0; i < length; ++i)
                    format.palette[i].a = data[i];
            } else if (format.color_type == 0) {
                if (length != 2)
                    return std::nullopt;
                format.has_key = true;
                format.key[0] = static_cast<std::uint32_t>(data[0]) << 8 | data[1];
            } else if (format.color_type == 2) {
                if (length != 6)
                    return std::nullopt;
                format.has_key = true;
                for (int i = 0; i < 3; ++i)
                    format.key[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(data[i * 2]) << 8 | data[i * 2 + 1];
            } else {
                return std::nullopt; // alpha types carry no tRNS
            }
        } else if (name == "IDAT") {
            saw_idat = true;
            if (parsed.idat.size() + length > max_idat)
                return std::nullopt;
            parsed.idat.insert(parsed.idat.end(), data, data + length);
            // The picture itself is the first frame when a frame control
            // chunk came before it, and no part of the animation otherwise.
            if (frame_open && !parsed.frames.empty())
                parsed.frames.back().data.emplace_back(data_at, length);
        } else if (name == "IEND") {
            saw_iend = true;
        } else if (name == "acTL" && !still_only) {
            if (length == 8 && !saw_idat) {
                parsed.animated = true;
                parsed.plays = read_be32(data + 4);
            }
        } else if (name == "fcTL" && !still_only) {
            if (!parsed.animated || length != 26 || parsed.frames.size() >= max_frames)
                continue;
            ApngFrame frame;
            std::uint32_t const width = read_be32(data + 4);
            std::uint32_t const height = read_be32(data + 8);
            std::uint32_t const x = read_be32(data + 12);
            std::uint32_t const y = read_be32(data + 16);
            // A frame lies inside the picture, whole (APNG 1.0): one that
            // does not is the end of what can be played.
            if (width == 0 || height == 0 || x > parsed.width || y > parsed.height || width > parsed.width - x
                || height > parsed.height - y)
                break;
            frame.x = static_cast<int>(x);
            frame.y = static_cast<int>(y);
            frame.width = static_cast<int>(width);
            frame.height = static_cast<int>(height);
            std::uint32_t const numerator = static_cast<std::uint32_t>(data[20]) << 8 | data[21];
            std::uint32_t denominator = static_cast<std::uint32_t>(data[22]) << 8 | data[23];
            if (denominator == 0)
                denominator = 100;
            frame.delay_ms = numerator * 1000u / denominator;
            frame.dispose = data[24] <= 2 ? data[24] : 0;
            frame.blend = data[25] == 1 ? 1 : 0;
            parsed.frames.push_back(std::move(frame));
            frame_open = true;
        } else if (name == "fdAT" && !still_only) {
            // A frame's data: a sequence number, then what an IDAT would hold.
            if (frame_open && !parsed.frames.empty() && saw_idat && length > 4)
                parsed.frames.back().data.emplace_back(data_at + 4, length - 4);
        } else if (critical) {
            return std::nullopt; // a critical chunk this decoder does not know
        }
    }
    if (!have_header || !saw_idat)
        return std::nullopt;
    if (parsed.format.color_type == 3 && parsed.format.palette.empty())
        return std::nullopt;
    // A color key is compared at the sample depth: a key out of range never matches.
    if (parsed.format.has_key && parsed.format.depth < 16) {
        std::uint32_t const limit = (1u << parsed.format.depth) - 1;
        for (std::uint32_t const k : parsed.format.key)
            if (k > limit)
                parsed.format.has_key = false;
    }
    // A frame that was given no data cannot be shown; it and what follows
    // it are left out.
    for (std::size_t i = 0; i < parsed.frames.size(); ++i) {
        if (parsed.frames[i].data.empty()) {
            parsed.frames.resize(i);
            break;
        }
    }
    return parsed;
}

} // namespace

bool looks_like_png(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= png_signature.size()
        && std::equal(png_signature.begin(), png_signature.end(), bytes.begin());
}

std::optional<Bitmap> decode_png(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels)
{
    std::optional<Parsed> const parsed = parse(bytes, max_pixels, true);
    if (!parsed)
        return std::nullopt;
    return decode_pixels(parsed->format, parsed->width, parsed->height, parsed->idat);
}

std::optional<Apng> scan_apng(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels)
{
    std::optional<Parsed> parsed = parse(bytes, max_pixels, false);
    if (!parsed || !parsed->animated || parsed->frames.empty())
        return std::nullopt;
    Apng animation;
    animation.width = static_cast<int>(parsed->width);
    animation.height = static_cast<int>(parsed->height);
    animation.loops = parsed->plays;
    animation.frames = std::move(parsed->frames);
    animation.samples = std::move(parsed->format);
    return animation;
}

std::optional<Bitmap> decode_apng_frame(std::vector<std::uint8_t> const& bytes, Apng const& animation, std::size_t index)
{
    if (index >= animation.frames.size())
        return std::nullopt;
    ApngFrame const& frame = animation.frames[index];
    std::vector<std::uint8_t> data;
    for (auto const& [offset, length] : frame.data) {
        if (offset > bytes.size() || length > bytes.size() - offset)
            return std::nullopt;
        data.insert(data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    return decode_pixels(animation.samples, static_cast<std::uint32_t>(frame.width),
        static_cast<std::uint32_t>(frame.height), data);
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

std::vector<std::uint8_t> zlib_compress(std::vector<std::uint8_t> const& raw)
{
    return zlib_deflate(raw);
}

}
