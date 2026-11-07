#include "pdf/ImageCodecs.h"
#include "pdf/Mq.h"

// JBIG2 pictures, PDF's /JBIG2Decode (ISO 32000-2 §7.4.7), written from
// ITU-T T.88: the coding of scanned pages as the page's text in symbols
// placed by a text region, the rest as generic regions, halftones as
// patterns on a grid, and refinements of any of them.
//
// A PDF carries the embedded stream organization (Annex D.3): segments
// without a file header, the page's in the image stream and the ones
// shared by many pages (symbol dictionaries, patterns, tables) in the
// /JBIG2Globals stream, which is read first. Each segment is a header
// (7.2) and its data; the region segments compose into the page bitmap
// (8.2) with their combination operator; intermediate regions are kept
// for the refinement that refers to them.
//
// Two entropy coders: the MQ arithmetic decoder (Annex E, shared with
// JPEG 2000 in Mq.cpp) with the integer decoding procedures of Annex A,
// and the Huffman tables of Annex B (fifteen standard ones and custom
// table segments); the bilevel coding of generic regions is either the
// template contexts of 6.2.5.7 or MMR, T.6 read from Ccitt.cpp.
//
// Damaged data ends a region where it fails; what was decoded stands.
// Not read: the colour extension and the twelve-pixel EXTTEMPLATE of
// the 2018 amendment (neither pdf.js nor poppler reads them).

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::pdf {

namespace {

constexpr std::size_t k_max_region_pixels = std::size_t { 1 } << 28; // 16,384 x 16,384

// --- Bitmaps, one byte a pixel, 1 = black --------------------------------

struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    Bitmap() = default;
    Bitmap(int w, int h, std::uint8_t fill = 0)
        : width(w)
        , height(h)
        , pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), fill)
    {
    }
    static bool fits(std::uint64_t w, std::uint64_t h) { return w > 0 && h > 0 && w * h <= k_max_region_pixels; }

    std::uint8_t get(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return 0;
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
    void set(int x, int y, std::uint8_t v)
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return;
        pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = v;
    }
    std::uint8_t* row(int y) { return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width); }
    std::uint8_t const* row(int y) const
    {
        return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
    }
};

// What a stream may ask the decoder to draw, from the page's size: no
// bitmap larger than twice the page (a region may run past its edge),
// and no more decoding in all than a page could carry (broken data can
// ask for any number of regions, and the arithmetic decoder goes on
// past the data's end). Symbols' pixels count twice, decoded and placed.
struct Budget {
    std::size_t per_bitmap = k_max_region_pixels;
    std::size_t total_left = 0;

    Budget(int page_width, int page_height)
    {
        std::size_t const page = static_cast<std::size_t>(page_width) * static_cast<std::size_t>(page_height);
        per_bitmap = std::min(k_max_region_pixels, 2 * page + (std::size_t { 1 } << 20));
        total_left = std::min(std::size_t { 1 } << 31, 64 * page + (std::size_t { 1 } << 24));
    }
    // true when a bitmap of w x h may be decoded, and that much is spent.
    bool take(std::uint64_t w, std::uint64_t h)
    {
        if (!Bitmap::fits(w, h) || w * h > per_bitmap || w * h > total_left)
            return false;
        total_left -= static_cast<std::size_t>(w * h);
        return true;
    }
};

enum class Op { Or = 0, And = 1, Xor = 2, Xnor = 3, Replace = 4 };

Op op_from(unsigned v)
{
    return v <= 4 ? static_cast<Op>(v) : Op::Or;
}

std::uint8_t combine(std::uint8_t dst, std::uint8_t src, Op op)
{
    switch (op) {
    case Op::Or:
        return dst | src;
    case Op::And:
        return dst & src;
    case Op::Xor:
        return dst ^ src;
    case Op::Xnor:
        return static_cast<std::uint8_t>(1 - (dst ^ src));
    case Op::Replace:
        return src;
    }
    return src;
}

// Draws src onto dst with its top-left at (x0, y0), clipped (6.4.5 step 3
// c x, 8.2 step 5).
void compose(Bitmap& dst, Bitmap const& src, int x0, int y0, Op op)
{
    int const left = std::max(0, -x0);
    int const top = std::max(0, -y0);
    int const right = std::min(src.width, dst.width - x0);
    int const bottom = std::min(src.height, dst.height - y0);
    for (int y = top; y < bottom; ++y) {
        std::uint8_t const* s = src.row(y);
        std::uint8_t* d = dst.row(y + y0);
        for (int x = left; x < right; ++x)
            d[x + x0] = combine(d[x + x0], s[x], op);
    }
}

// --- Byte and bit readers --------------------------------------------------

class Reader {
public:
    explicit Reader(std::span<std::uint8_t const> data)
        : m_data(data)
    {
    }
    bool ok() const { return !m_failed; }
    bool at_end() const { return m_position >= m_data.size(); }
    std::size_t position() const { return m_position; }
    std::size_t remaining() const { return m_position < m_data.size() ? m_data.size() - m_position : 0; }
    void seek(std::size_t p) { m_position = std::min(p, m_data.size()); }
    void skip(std::size_t n)
    {
        if (n > remaining())
            m_failed = true;
        m_position = std::min(m_data.size(), m_position + n);
    }
    std::uint8_t u8()
    {
        if (m_position >= m_data.size()) {
            m_failed = true;
            return 0;
        }
        return m_data[m_position++];
    }
    std::int8_t s8() { return static_cast<std::int8_t>(u8()); }
    std::uint16_t u16() { return static_cast<std::uint16_t>((u8() << 8) | u8()); }
    std::uint32_t u32()
    {
        std::uint32_t v = static_cast<std::uint32_t>(u8()) << 24;
        v |= static_cast<std::uint32_t>(u8()) << 16;
        v |= static_cast<std::uint32_t>(u8()) << 8;
        return v | u8();
    }
    std::int32_t s32() { return static_cast<std::int32_t>(u32()); }
    std::span<std::uint8_t const> rest() const { return m_data.subspan(std::min(m_position, m_data.size())); }
    std::span<std::uint8_t const> take(std::size_t n)
    {
        if (n > remaining()) {
            m_failed = true;
            n = remaining();
        }
        auto const s = m_data.subspan(m_position, n);
        m_position += n;
        return s;
    }

    // Bits, most significant first, for the Huffman-coded procedures.
    int bit()
    {
        if (m_position >= m_data.size()) {
            m_failed = true;
            return 0;
        }
        int const b = (m_data[m_position] >> (7 - m_bit)) & 1;
        if (++m_bit == 8) {
            m_bit = 0;
            ++m_position;
        }
        return b;
    }
    std::uint32_t bits(int n)
    {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | static_cast<std::uint32_t>(bit());
        return v;
    }
    void align()
    {
        if (m_bit != 0) {
            m_bit = 0;
            ++m_position;
        }
    }

private:
    std::span<std::uint8_t const> m_data;
    std::size_t m_position = 0;
    int m_bit = 0;
    bool m_failed = false;
};

// --- Annex A: arithmetic integer decoding ---------------------------------

struct IntContext {
    std::array<MqContext, 512> cx {};
};

// A.2: nothing for OOB.
std::optional<std::int32_t> decode_int(MqDecoder& decoder, IntContext& context)
{
    std::uint32_t prev = 1;
    auto const bit = [&]() {
        int const b = decoder.decode(context.cx[prev]);
        prev = prev < 256 ? ((prev << 1) | static_cast<std::uint32_t>(b))
                          : ((((prev << 1) | static_cast<std::uint32_t>(b)) & 511) | 256);
        return b;
    };
    auto const read = [&](int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | static_cast<std::uint32_t>(bit());
        return v;
    };
    int const sign = bit();
    std::uint32_t value;
    if (!bit())
        value = read(2);
    else if (!bit())
        value = read(4) + 4;
    else if (!bit())
        value = read(6) + 20;
    else if (!bit())
        value = read(8) + 84;
    else if (!bit())
        value = read(12) + 340;
    else
        value = read(32) + 4436;
    if (sign && value == 0)
        return std::nullopt; // OOB
    if (value > 0x7FFFFFFFu)
        value = 0x7FFFFFFFu;
    return sign ? -static_cast<std::int32_t>(value) : static_cast<std::int32_t>(value);
}

// A.3: a symbol ID of symcodelen bits.
struct IaidContext {
    std::vector<MqContext> cx;
    int code_length = 0;
    explicit IaidContext(int length)
        : cx(std::size_t { 1 } << (length + 1))
        , code_length(length)
    {
    }
};

std::uint32_t decode_iaid(MqDecoder& decoder, IaidContext& context)
{
    std::uint32_t prev = 1;
    for (int i = 0; i < context.code_length; ++i)
        prev = (prev << 1) | static_cast<std::uint32_t>(decoder.decode(context.cx[prev]));
    return prev - (1u << context.code_length);
}

int ceil_log2(std::uint32_t n)
{
    int bits = 0;
    while ((std::uint64_t { 1 } << bits) < n)
        ++bits;
    return bits;
}

// --- Annex B: Huffman tables -----------------------------------------------

struct HuffmanLine {
    int prefix_length = 0;
    int range_length = 0; // 32 = the open-ended lines
    std::int32_t range_low = 0;
    bool lower = false; // the lower range line counts down from range_low
    bool oob = false;
    std::uint32_t code = 0;
};

class HuffmanTable {
public:
    HuffmanTable() = default;
    explicit HuffmanTable(std::vector<HuffmanLine> lines)
        : m_lines(std::move(lines))
    {
        assign_codes();
    }
    bool empty() const { return m_lines.empty(); }
    bool has_oob() const
    {
        return std::any_of(m_lines.begin(), m_lines.end(), [](HuffmanLine const& l) { return l.oob; });
    }

    // One value; nothing for OOB or when no code matches 32 bits.
    // `oob` tells OOB from a failure.
    std::optional<std::int32_t> decode(Reader& reader, bool* oob = nullptr) const
    {
        if (oob)
            *oob = false;
        std::uint32_t code = 0;
        for (int length = 1; length <= 32; ++length) {
            code = (code << 1) | static_cast<std::uint32_t>(reader.bit());
            for (HuffmanLine const& line : m_lines) {
                if (line.prefix_length != length || line.code != code)
                    continue;
                if (line.oob) {
                    if (oob)
                        *oob = true;
                    return std::nullopt;
                }
                std::uint32_t const v = reader.bits(line.range_length);
                if (line.lower)
                    return line.range_low - static_cast<std::int32_t>(v);
                return line.range_low + static_cast<std::int32_t>(v);
            }
            if (!reader.ok())
                return std::nullopt;
        }
        return std::nullopt;
    }

private:
    // B.3: canonical codes by prefix length, in the lines' order.
    void assign_codes()
    {
        int max_length = 0;
        for (HuffmanLine const& line : m_lines)
            max_length = std::max(max_length, line.prefix_length);
        std::vector<int> count(static_cast<std::size_t>(max_length) + 2, 0);
        for (HuffmanLine const& line : m_lines) {
            if (line.prefix_length > 0)
                ++count[static_cast<std::size_t>(line.prefix_length)];
        }
        std::uint32_t first = 0;
        count[0] = 0;
        for (int length = 1; length <= max_length; ++length) {
            first = (first + static_cast<std::uint32_t>(count[static_cast<std::size_t>(length) - 1])) << 1;
            std::uint32_t code = first;
            for (HuffmanLine& line : m_lines) {
                if (line.prefix_length == length)
                    line.code = code++;
            }
        }
    }

    std::vector<HuffmanLine> m_lines;
};

// Table B.1 to B.15 as (prefix length, range length, range low); L marks
// the lower range line, O the OOB code.
struct StdLine {
    int prefix;
    int range;
    std::int32_t low;
    char kind; // ' ', 'L', 'O'
};

HuffmanTable make_table(std::initializer_list<StdLine> lines)
{
    std::vector<HuffmanLine> out;
    for (StdLine const& l : lines) {
        HuffmanLine line;
        line.prefix_length = l.prefix;
        line.range_length = l.range;
        line.range_low = l.low;
        line.lower = l.kind == 'L';
        line.oob = l.kind == 'O';
        out.push_back(line);
    }
    return HuffmanTable(std::move(out));
}

HuffmanTable const& standard_table(int number)
{
    static std::array<HuffmanTable, 16> tables = [] {
        std::array<HuffmanTable, 16> t;
        t[1] = make_table({ { 1, 4, 0, ' ' }, { 2, 8, 16, ' ' }, { 3, 16, 272, ' ' }, { 3, 32, 65808, ' ' } });
        t[2] = make_table({ { 1, 0, 0, ' ' }, { 2, 0, 1, ' ' }, { 3, 0, 2, ' ' }, { 4, 3, 3, ' ' }, { 5, 6, 11, ' ' },
            { 6, 32, 75, ' ' }, { 6, 0, 0, 'O' } });
        t[3] = make_table({ { 8, 8, -256, ' ' }, { 1, 0, 0, ' ' }, { 2, 0, 1, ' ' }, { 3, 0, 2, ' ' }, { 4, 3, 3, ' ' },
            { 5, 6, 11, ' ' }, { 8, 32, -257, 'L' }, { 7, 32, 75, ' ' }, { 6, 0, 0, 'O' } });
        t[4] = make_table({ { 1, 0, 1, ' ' }, { 2, 0, 2, ' ' }, { 3, 0, 3, ' ' }, { 4, 3, 4, ' ' }, { 5, 6, 12, ' ' },
            { 5, 32, 76, ' ' } });
        t[5] = make_table({ { 7, 8, -255, ' ' }, { 1, 0, 1, ' ' }, { 2, 0, 2, ' ' }, { 3, 0, 3, ' ' }, { 4, 3, 4, ' ' },
            { 5, 6, 12, ' ' }, { 7, 32, -256, 'L' }, { 6, 32, 76, ' ' } });
        t[6] = make_table({ { 5, 10, -2048, ' ' }, { 4, 9, -1024, ' ' }, { 4, 8, -512, ' ' }, { 4, 7, -256, ' ' },
            { 5, 6, -128, ' ' }, { 5, 5, -64, ' ' }, { 4, 5, -32, ' ' }, { 2, 7, 0, ' ' }, { 3, 7, 128, ' ' },
            { 3, 8, 256, ' ' }, { 4, 9, 512, ' ' }, { 4, 10, 1024, ' ' }, { 6, 32, -2049, 'L' }, { 6, 32, 2048, ' ' } });
        t[7] = make_table({ { 4, 9, -1024, ' ' }, { 3, 8, -512, ' ' }, { 4, 7, -256, ' ' }, { 5, 6, -128, ' ' },
            { 5, 5, -64, ' ' }, { 4, 5, -32, ' ' }, { 4, 9, 0, ' ' }, { 5, 10, 512, ' ' }, { 3, 10, 1536, ' ' },
            { 6, 32, -1025, 'L' }, { 6, 32, 2560, ' ' } });
        t[8] = make_table({ { 8, 3, -15, ' ' }, { 9, 1, -7, ' ' }, { 8, 1, -5, ' ' }, { 9, 0, -3, ' ' }, { 7, 0, -2, ' ' },
            { 4, 0, -1, ' ' }, { 2, 1, 0, ' ' }, { 5, 0, 2, ' ' }, { 6, 0, 3, ' ' }, { 3, 4, 4, ' ' }, { 6, 1, 20, ' ' },
            { 4, 4, 22, ' ' }, { 4, 5, 38, ' ' }, { 5, 6, 70, ' ' }, { 5, 7, 134, ' ' }, { 6, 7, 262, ' ' },
            { 7, 8, 390, ' ' }, { 6, 10, 646, ' ' }, { 9, 32, -16, 'L' }, { 9, 32, 1670, ' ' }, { 2, 0, 0, 'O' } });
        t[9] = make_table({ { 8, 4, -31, ' ' }, { 9, 2, -15, ' ' }, { 8, 2, -11, ' ' }, { 9, 1, -7, ' ' }, { 7, 1, -5, ' ' },
            { 4, 1, -3, ' ' }, { 3, 1, -1, ' ' }, { 3, 1, 1, ' ' }, { 5, 1, 3, ' ' }, { 6, 1, 5, ' ' }, { 3, 5, 7, ' ' },
            { 6, 2, 39, ' ' }, { 4, 5, 43, ' ' }, { 4, 6, 75, ' ' }, { 5, 7, 139, ' ' }, { 5, 8, 267, ' ' },
            { 6, 8, 523, ' ' }, { 7, 9, 779, ' ' }, { 6, 11, 1291, ' ' }, { 9, 32, -32, 'L' }, { 9, 32, 3339, ' ' },
            { 2, 0, 0, 'O' } });
        t[10] = make_table({ { 7, 4, -21, ' ' }, { 8, 0, -5, ' ' }, { 7, 0, -4, ' ' }, { 5, 0, -3, ' ' }, { 2, 2, -2, ' ' },
            { 5, 0, 2, ' ' }, { 6, 0, 3, ' ' }, { 7, 0, 4, ' ' }, { 8, 0, 5, ' ' }, { 2, 6, 6, ' ' }, { 5, 5, 70, ' ' },
            { 6, 6, 102, ' ' }, { 7, 7, 166, ' ' }, { 8, 8, 294, ' ' }, { 9, 9, 550, ' ' }, { 10, 10, 1062, ' ' },
            { 9, 32, -22, 'L' }, { 9, 32, 2086, ' ' }, { 2, 0, 0, 'O' } });
        t[11] = make_table({ { 1, 0, 1, ' ' }, { 2, 1, 2, ' ' }, { 4, 0, 4, ' ' }, { 4, 1, 5, ' ' }, { 5, 1, 7, ' ' },
            { 5, 2, 9, ' ' }, { 6, 2, 13, ' ' }, { 7, 2, 17, ' ' }, { 7, 3, 21, ' ' }, { 7, 4, 29, ' ' }, { 7, 5, 45, ' ' },
            { 7, 6, 77, ' ' }, { 7, 32, 141, ' ' } });
        t[12] = make_table({ { 1, 0, 1, ' ' }, { 2, 0, 2, ' ' }, { 3, 1, 3, ' ' }, { 5, 0, 5, ' ' }, { 5, 1, 6, ' ' },
            { 6, 1, 8, ' ' }, { 7, 0, 10, ' ' }, { 7, 1, 11, ' ' }, { 7, 2, 13, ' ' }, { 7, 3, 17, ' ' }, { 7, 4, 25, ' ' },
            { 8, 5, 41, ' ' }, { 8, 32, 73, ' ' } });
        t[13] = make_table({ { 1, 0, 1, ' ' }, { 3, 0, 2, ' ' }, { 4, 0, 3, ' ' }, { 5, 0, 4, ' ' }, { 4, 1, 5, ' ' },
            { 3, 3, 7, ' ' }, { 6, 1, 15, ' ' }, { 6, 2, 17, ' ' }, { 6, 3, 21, ' ' }, { 6, 4, 29, ' ' }, { 6, 5, 45, ' ' },
            { 7, 6, 77, ' ' }, { 7, 32, 141, ' ' } });
        t[14] = make_table({ { 3, 0, -2, ' ' }, { 3, 0, -1, ' ' }, { 1, 0, 0, ' ' }, { 3, 0, 1, ' ' }, { 3, 0, 2, ' ' } });
        t[15] = make_table({ { 7, 4, -24, ' ' }, { 6, 2, -8, ' ' }, { 5, 1, -4, ' ' }, { 4, 0, -2, ' ' }, { 3, 0, -1, ' ' },
            { 1, 0, 0, ' ' }, { 3, 0, 1, ' ' }, { 4, 0, 2, ' ' }, { 5, 1, 3, ' ' }, { 6, 2, 5, ' ' }, { 7, 4, 9, ' ' },
            { 7, 32, -25, 'L' }, { 7, 32, 25, ' ' } });
        return t;
    }();
    return tables[static_cast<std::size_t>(std::clamp(number, 1, 15))];
}

// B.2: a custom table segment (type 53).
std::optional<HuffmanTable> parse_table_segment(std::span<std::uint8_t const> data)
{
    Reader r(data);
    std::uint8_t const flags = r.u8();
    bool const oob = flags & 1;
    int const prefix_size = ((flags >> 1) & 7) + 1;
    int const range_size = ((flags >> 4) & 7) + 1;
    std::int32_t const low = r.s32();
    std::int32_t const high = r.s32();
    if (!r.ok() || low > high)
        return std::nullopt;
    std::vector<HuffmanLine> lines;
    std::int64_t current = low;
    while (current < high) {
        HuffmanLine line;
        line.prefix_length = static_cast<int>(r.bits(prefix_size));
        line.range_length = static_cast<int>(r.bits(range_size));
        line.range_low = static_cast<std::int32_t>(current);
        lines.push_back(line);
        if (line.range_length >= 32) {
            if (line.range_length > 32)
                return std::nullopt;
            break;
        }
        current += std::int64_t { 1 } << line.range_length;
        if (!r.ok() || lines.size() > 10000)
            return std::nullopt;
    }
    HuffmanLine lower;
    lower.prefix_length = static_cast<int>(r.bits(prefix_size));
    lower.range_length = 32;
    lower.range_low = low - 1;
    lower.lower = true;
    lines.push_back(lower);
    HuffmanLine upper;
    upper.prefix_length = static_cast<int>(r.bits(prefix_size));
    upper.range_length = 32;
    upper.range_low = high;
    lines.push_back(upper);
    if (oob) {
        HuffmanLine line;
        line.prefix_length = static_cast<int>(r.bits(prefix_size));
        line.oob = true;
        lines.push_back(line);
    }
    if (!r.ok())
        return std::nullopt;
    // Lines with a zero prefix length have no code.
    std::erase_if(lines, [](HuffmanLine const& l) { return l.prefix_length == 0; });
    return HuffmanTable(std::move(lines));
}

// --- 6.2: generic region decoding -----------------------------------------

struct AtPixel {
    std::int8_t x = 0;
    std::int8_t y = 0;
};

struct GenericContexts {
    std::vector<MqContext> cx = std::vector<MqContext>(1 << 16);
};

struct RefinementContexts {
    std::vector<MqContext> cx = std::vector<MqContext>(1 << 13);
};

struct TemplatePixel {
    int x;
    int y;
};

// The template's pixels in the order their bits form the context (Figures
// 4 to 7 read with Figure 8: the top row first, left to right, the
// current row last), AT pixels in their nominal places.
std::vector<TemplatePixel> template_pixels(int gb_template, std::array<AtPixel, 4> const& at)
{
    auto const a = [&](int i) { return TemplatePixel { at[static_cast<std::size_t>(i)].x, at[static_cast<std::size_t>(i)].y }; };
    switch (gb_template) {
    case 0:
        return { a(3), { -1, -2 }, { 0, -2 }, { 1, -2 }, a(2), a(1), { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 2, -1 },
            a(0), { -4, 0 }, { -3, 0 }, { -2, 0 }, { -1, 0 } };
    case 1:
        return { { -1, -2 }, { 0, -2 }, { 1, -2 }, { 2, -2 }, { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { 2, -1 }, a(0),
            { -3, 0 }, { -2, 0 }, { -1, 0 } };
    case 2:
        return { { -1, -2 }, { 0, -2 }, { 1, -2 }, { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, a(0), { -2, 0 }, { -1, 0 } };
    default:
        return { { -3, -1 }, { -2, -1 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, a(0), { -4, 0 }, { -3, 0 }, { -2, 0 }, { -1, 0 } };
    }
}

struct GenericParams {
    int width = 0;
    int height = 0;
    int gb_template = 0;
    std::array<AtPixel, 4> at {};
    bool tpgdon = false;
    Bitmap const* skip = nullptr; // 6.2.5.7 step 3 c: pixels not decoded, 0
    Budget* budget = nullptr;
};

// 6.2.5.7, the arithmetic path. The context of a pixel is the one to its
// left shifted by one, where a template pixel's right-hand neighbor is
// also in the template, plus the pixels that are not (the rightmost of
// each row of the template, and any AT pixel on its own).
bool decode_generic(MqDecoder& decoder, GenericContexts& contexts, GenericParams const& p, Bitmap& out)
{
    if (!p.budget || !p.budget->take(static_cast<std::uint64_t>(p.width), static_cast<std::uint64_t>(p.height)))
        return false;
    out = Bitmap(p.width, p.height);
    std::vector<TemplatePixel> const pixels = template_pixels(p.gb_template, p.at);
    int const n = static_cast<int>(pixels.size());
    for (TemplatePixel const& t : pixels) {
        // Every template pixel is above the current one, or to its left.
        if (t.y > 0 || (t.y == 0 && t.x >= 0))
            return false;
    }
    std::uint32_t reuse_mask = 0;
    std::vector<int> changing; // indices into pixels
    for (int i = 0; i < n; ++i) {
        bool reusable = false;
        if (i + 1 < n && pixels[static_cast<std::size_t>(i) + 1].x == pixels[static_cast<std::size_t>(i)].x + 1
            && pixels[static_cast<std::size_t>(i) + 1].y == pixels[static_cast<std::size_t>(i)].y)
            reusable = true;
        if (reusable)
            reuse_mask |= 1u << (n - 1 - i);
        else
            changing.push_back(i);
    }
    static constexpr std::uint16_t tpgd_context[4] = { 0x9B25, 0x0795, 0x00E5, 0x0195 };
    bool ltp = false;
    for (int y = 0; y < p.height; ++y) {
        if (p.tpgdon) {
            if (decoder.decode(contexts.cx[tpgd_context[p.gb_template & 3]]))
                ltp = !ltp;
            if (ltp) {
                if (y > 0)
                    std::copy_n(out.row(y - 1), p.width, out.row(y));
                continue;
            }
        }
        std::uint8_t* const row = out.row(y);
        std::uint8_t const* const skip_row = p.skip && y < p.skip->height ? p.skip->row(y) : nullptr;
        // The whole context of the first pixel, then the incremental one.
        std::uint32_t context = 0;
        for (int i = 0; i < n; ++i)
            context = (context << 1) | out.get(pixels[static_cast<std::size_t>(i)].x, y + pixels[static_cast<std::size_t>(i)].y);
        for (int x = 0; x < p.width; ++x) {
            if (x > 0) {
                context = (context << 1) & reuse_mask;
                for (int i : changing) {
                    TemplatePixel const& t = pixels[static_cast<std::size_t>(i)];
                    context |= static_cast<std::uint32_t>(out.get(x + t.x, y + t.y)) << (n - 1 - i);
                }
            }
            if (skip_row && x < p.skip->width && skip_row[x]) {
                row[x] = 0;
                continue;
            }
            row[x] = static_cast<std::uint8_t>(decoder.decode(contexts.cx[context]));
        }
    }
    return true;
}

// 6.2.6: the MMR path.
bool decode_generic_mmr(MmrDecoder& mmr, int width, int height, Budget& budget, Bitmap& out)
{
    if (!budget.take(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height)))
        return false;
    out = Bitmap(width, height);
    mmr.decode(width, height, out.pixels.data());
    return true;
}

// --- 6.3: generic refinement region decoding -------------------------------

struct RefinementParams {
    int width = 0;
    int height = 0;
    int gr_template = 0;
    Bitmap const* reference = nullptr;
    int dx = 0;
    int dy = 0;
    bool tpgron = false;
    std::array<AtPixel, 2> at {};
    Budget* budget = nullptr;
};

bool decode_refinement(MqDecoder& decoder, RefinementContexts& contexts, RefinementParams const& p, Bitmap& out)
{
    if (!p.reference || !p.budget || !p.budget->take(static_cast<std::uint64_t>(p.width), static_cast<std::uint64_t>(p.height)))
        return false;
    out = Bitmap(p.width, p.height);
    Bitmap const& ref = *p.reference;
    // Figures 12 and 13: the current bitmap's pixels, then the reference's
    // around (x - dx, y - dy). The bit order is ours; the only context the
    // standard names is the typical prediction one (Figures 14 and 15), the
    // pattern with the reference's center pixel alone set.
    struct Pixel {
        bool reference;
        int x;
        int y;
    };
    std::vector<Pixel> pixels;
    if (p.gr_template == 0) {
        pixels = { { false, 0, -1 }, { false, 1, -1 }, { false, -1, 0 }, { false, p.at[0].x, p.at[0].y }, { true, 0, -1 },
            { true, 1, -1 }, { true, -1, 0 }, { true, 0, 0 }, { true, 1, 0 }, { true, -1, 1 }, { true, 0, 1 }, { true, 1, 1 },
            { true, p.at[1].x, p.at[1].y } };
    } else {
        pixels = { { false, -1, -1 }, { false, 0, -1 }, { false, 1, -1 }, { false, -1, 0 }, { true, 0, -1 }, { true, -1, 0 },
            { true, 0, 0 }, { true, 1, 0 }, { true, 0, 1 }, { true, 1, 1 } };
    }
    int const n = static_cast<int>(pixels.size());
    std::uint32_t ltp_context = 0;
    for (int i = 0; i < n; ++i) {
        if (pixels[static_cast<std::size_t>(i)].reference && pixels[static_cast<std::size_t>(i)].x == 0
            && pixels[static_cast<std::size_t>(i)].y == 0)
            ltp_context = 1u << (n - 1 - i);
    }
    for (Pixel const& t : pixels) {
        if (!t.reference && (t.y > 0 || (t.y == 0 && t.x >= 0)))
            return false;
    }
    bool ltp = false;
    for (int y = 0; y < p.height; ++y) {
        if (p.tpgron && decoder.decode(contexts.cx[ltp_context]))
            ltp = !ltp;
        std::uint8_t* const row = out.row(y);
        for (int x = 0; x < p.width; ++x) {
            int const rx = x - p.dx;
            int const ry = y - p.dy;
            if (ltp) {
                // 6.3.5.6: a pixel whose reference neighborhood is all one
                // color is that color.
                int sum = 0;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i)
                        sum += ref.get(rx + i, ry + j);
                }
                if (sum == 0) {
                    row[x] = 0;
                    continue;
                }
                if (sum == 9) {
                    row[x] = 1;
                    continue;
                }
            }
            std::uint32_t context = 0;
            for (Pixel const& t : pixels) {
                std::uint8_t const v = t.reference ? ref.get(rx + t.x, ry + t.y) : out.get(x + t.x, y + t.y);
                context = (context << 1) | v;
            }
            row[x] = static_cast<std::uint8_t>(decoder.decode(contexts.cx[context]));
        }
    }
    return true;
}

// --- Segments ---------------------------------------------------------------

struct SegmentHeader {
    std::uint32_t number = 0;
    int type = 0;
    std::vector<std::uint32_t> referred;
    std::uint32_t page = 0;
    std::uint32_t data_length = 0;
    bool unknown_length = false;
};

// 7.2: nothing when the header is cut short.
std::optional<SegmentHeader> parse_segment_header(Reader& r)
{
    SegmentHeader h;
    h.number = r.u32();
    std::uint8_t const flags = r.u8();
    h.type = flags & 0x3F;
    bool const page_4 = flags & 0x40;
    std::uint8_t const rts = r.u8();
    std::uint32_t count = rts >> 5;
    if (count == 7) {
        r.seek(r.position() - 1);
        count = r.u32() & 0x1FFFFFFF;
        r.skip((count + 8) / 8); // retain bits
    }
    if (count > 100000)
        return std::nullopt;
    int const ref_size = h.number <= 256 ? 1 : h.number <= 65536 ? 2 : 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t v = ref_size == 1 ? r.u8() : ref_size == 2 ? r.u16() : r.u32();
        h.referred.push_back(v);
    }
    h.page = page_4 ? r.u32() : r.u8();
    h.data_length = r.u32();
    h.unknown_length = h.data_length == 0xFFFFFFFFu;
    if (!r.ok())
        return std::nullopt;
    return h;
}

struct RegionInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    Op op = Op::Or;
    bool colour = false;
};

RegionInfo parse_region_info(Reader& r)
{
    RegionInfo info;
    info.width = r.u32();
    info.height = r.u32();
    info.x = r.u32();
    info.y = r.u32();
    std::uint8_t const flags = r.u8();
    info.op = op_from(flags & 7);
    info.colour = flags & 8;
    return info;
}

struct SymbolDictionary {
    std::vector<Bitmap> exported;
    // 7.4.3.1.1 bits 8 and 9: the coding contexts kept for a dictionary
    // that says it uses them.
    std::shared_ptr<GenericContexts> generic_contexts;
    std::shared_ptr<RefinementContexts> refinement_contexts;
};

struct PatternDictionary {
    std::vector<Bitmap> patterns;
};

struct TextRegionParams {
    bool huffman = false;
    bool refine = false;
    int width = 0;
    int height = 0;
    std::uint32_t instances = 0;
    int log_strips = 0;
    int strips = 1;
    int ref_corner = 1; // 0 BL, 1 TL, 2 BR, 3 TR
    bool transposed = false;
    Op op = Op::Or;
    std::uint8_t default_pixel = 0;
    int ds_offset = 0;
    int rtemplate = 0;
    std::array<AtPixel, 2> rat {};
    std::vector<Bitmap const*> symbols;
    int symcodelen = 0;
    // Huffman tables.
    HuffmanTable const* fs = nullptr;
    HuffmanTable const* ds = nullptr;
    HuffmanTable const* dt = nullptr;
    HuffmanTable const* rdw = nullptr;
    HuffmanTable const* rdh = nullptr;
    HuffmanTable const* rdx = nullptr;
    HuffmanTable const* rdy = nullptr;
    HuffmanTable const* rsize = nullptr;
    HuffmanTable const* symcode = nullptr;
    Budget* budget = nullptr;
};

// The arithmetic decoding contexts a text region or symbol dictionary uses.
struct TextContexts {
    IntContext iadt, iafs, iads, iait, iari, iardw, iardh, iardx, iardy;
    std::unique_ptr<IaidContext> iaid;
};

// 6.4.5. `decoder` is the arithmetic decoder (when !huffman), `reader` the
// Huffman bit reader (when huffman), both positioned at the region data.
bool decode_text_region(TextRegionParams const& p, MqDecoder* decoder, Reader* reader, TextContexts& tc,
    RefinementContexts& rcx, Bitmap& out)
{
    if (!p.budget || !p.budget->take(static_cast<std::uint64_t>(p.width), static_cast<std::uint64_t>(p.height)))
        return false;
    out = Bitmap(p.width, p.height, p.default_pixel);
    if (p.symbols.empty() && p.instances > 0)
        return false;
    if (p.huffman && !(p.fs && p.ds && p.dt && p.symcode))
        return false;
    if (p.huffman && p.refine && !(p.rdw && p.rdh && p.rdx && p.rdy && p.rsize))
        return false;
    auto const decode_dt = [&]() -> std::optional<std::int32_t> {
        return p.huffman ? p.dt->decode(*reader) : decode_int(*decoder, tc.iadt);
    };
    std::optional<std::int32_t> const first_t = decode_dt();
    if (!first_t)
        return false;
    std::int64_t stript = -static_cast<std::int64_t>(*first_t) * p.strips;
    std::int64_t firsts = 0;
    std::uint32_t instances = 0;
    while (instances < p.instances) {
        std::optional<std::int32_t> const dt = decode_dt();
        if (!dt)
            return false;
        stript += static_cast<std::int64_t>(*dt) * p.strips;
        std::optional<std::int32_t> const dfs = p.huffman ? p.fs->decode(*reader) : decode_int(*decoder, tc.iafs);
        if (!dfs)
            return false;
        firsts += *dfs;
        std::int64_t curs = firsts;
        bool first = true;
        while (true) {
            if (!first) {
                bool oob = false;
                std::optional<std::int32_t> ids;
                if (p.huffman) {
                    ids = p.ds->decode(*reader, &oob);
                    if (!ids && !oob)
                        return false;
                } else {
                    ids = decode_int(*decoder, tc.iads);
                }
                if (!ids)
                    break; // OOB: the strip's end
                curs += *ids + p.ds_offset;
            }
            first = false;
            if (instances >= p.instances)
                break;
            std::int64_t curt = 0;
            if (p.strips > 1) {
                if (p.huffman) {
                    curt = reader->bits(p.log_strips);
                } else {
                    std::optional<std::int32_t> const v = decode_int(*decoder, tc.iait);
                    if (!v)
                        return false;
                    curt = *v;
                }
            }
            std::int64_t const tt = stript + curt;
            std::uint32_t id;
            if (p.huffman) {
                std::optional<std::int32_t> const v = p.symcode->decode(*reader);
                if (!v)
                    return false;
                id = static_cast<std::uint32_t>(*v);
            } else {
                id = decode_iaid(*decoder, *tc.iaid);
            }
            if (id >= p.symbols.size() || !p.symbols[id])
                return false;
            Bitmap const* symbol = p.symbols[id];
            Bitmap refined;
            if (p.refine) {
                std::int32_t ri = 0;
                if (p.huffman) {
                    ri = static_cast<std::int32_t>(reader->bit());
                } else {
                    std::optional<std::int32_t> const v = decode_int(*decoder, tc.iari);
                    if (!v)
                        return false;
                    ri = *v;
                }
                if (ri != 0) {
                    auto const dec = [&](HuffmanTable const* table, IntContext& cx) -> std::optional<std::int32_t> {
                        return p.huffman ? table->decode(*reader) : decode_int(*decoder, cx);
                    };
                    std::optional<std::int32_t> const rdw = dec(p.rdw, tc.iardw);
                    std::optional<std::int32_t> const rdh = dec(p.rdh, tc.iardh);
                    std::optional<std::int32_t> const rdx = dec(p.rdx, tc.iardx);
                    std::optional<std::int32_t> const rdy = dec(p.rdy, tc.iardy);
                    if (!rdw || !rdh || !rdx || !rdy)
                        return false;
                    std::optional<std::int32_t> bmsize;
                    if (p.huffman) {
                        bmsize = p.rsize->decode(*reader);
                        if (!bmsize)
                            return false;
                        reader->align();
                    }
                    RefinementParams rp;
                    rp.width = symbol->width + *rdw;
                    rp.height = symbol->height + *rdh;
                    rp.gr_template = p.rtemplate;
                    rp.reference = symbol;
                    // GRREFERENCEDX = floor(RDW / 2) + RDX (6.4.11.1); the shift floors.
                    rp.dx = (*rdw >> 1) + *rdx;
                    rp.dy = (*rdh >> 1) + *rdy;
                    rp.tpgron = false;
                    rp.at = p.rat;
                    rp.budget = p.budget;
                    if (rp.width <= 0 || rp.height <= 0 || rp.width > 10000 || rp.height > 10000)
                        return false;
                    if (p.huffman) {
                        // The refinement is arithmetic in BMSIZE bytes of its own.
                        std::span<std::uint8_t const> const bytes
                            = *bmsize > 0 ? reader->take(static_cast<std::size_t>(*bmsize)) : reader->rest();
                        MqDecoder local(bytes);
                        RefinementContexts local_cx;
                        if (!decode_refinement(local, local_cx, rp, refined))
                            return false;
                        if (*bmsize == 0)
                            reader->skip(local.position());
                        reader->align();
                    } else {
                        if (!decode_refinement(*decoder, rcx, rp, refined))
                            return false;
                    }
                    symbol = &refined;
                }
            }
            int const wi = symbol->width;
            int const hi = symbol->height;
            if (!p.budget->take(static_cast<std::uint64_t>(wi), static_cast<std::uint64_t>(hi)))
                return false;
            // 6.4.5 step 3 c vi to xi: the corner at (S, T), S advancing
            // by the symbol's extent along the strip.
            if (!p.transposed && (p.ref_corner == 2 || p.ref_corner == 3))
                curs += wi - 1;
            if (p.transposed && (p.ref_corner == 0 || p.ref_corner == 2))
                curs += hi - 1;
            std::int64_t x0, y0;
            if (!p.transposed) {
                x0 = (p.ref_corner == 2 || p.ref_corner == 3) ? curs - wi + 1 : curs;
                y0 = (p.ref_corner == 0 || p.ref_corner == 2) ? tt - hi + 1 : tt;
            } else {
                y0 = (p.ref_corner == 0 || p.ref_corner == 2) ? curs - hi + 1 : curs;
                x0 = (p.ref_corner == 2 || p.ref_corner == 3) ? tt - wi + 1 : tt;
            }
            if (x0 > -100000 && x0 < 100000 && y0 > -100000 && y0 < 100000)
                compose(out, *symbol, static_cast<int>(x0), static_cast<int>(y0), p.op);
            if (!p.transposed && (p.ref_corner == 0 || p.ref_corner == 1))
                curs += wi - 1;
            if (p.transposed && (p.ref_corner == 1 || p.ref_corner == 3))
                curs += hi - 1;
            ++instances;
        }
    }
    return true;
}

// 7.4.3.1.7: the symbol ID codes of a Huffman text region, run-coded.
std::optional<HuffmanTable> read_symbol_id_codes(Reader& r, std::uint32_t symbol_count)
{
    std::vector<HuffmanLine> runcode_lines;
    for (int i = 0; i < 35; ++i) {
        HuffmanLine line;
        line.prefix_length = static_cast<int>(r.bits(4));
        line.range_length = 0;
        line.range_low = i;
        if (line.prefix_length > 0)
            runcode_lines.push_back(line);
    }
    HuffmanTable const runcodes(std::move(runcode_lines));
    std::vector<HuffmanLine> lines;
    int previous = 0;
    std::uint32_t i = 0;
    while (i < symbol_count) {
        std::optional<std::int32_t> const code = runcodes.decode(r);
        if (!code || !r.ok())
            return std::nullopt;
        if (*code < 32) {
            HuffmanLine line;
            line.prefix_length = *code;
            line.range_low = static_cast<std::int32_t>(i);
            lines.push_back(line);
            previous = *code;
            ++i;
            continue;
        }
        std::uint32_t repeat;
        int length;
        if (*code == 32) {
            repeat = r.bits(2) + 3;
            length = previous;
        } else if (*code == 33) {
            repeat = r.bits(3) + 3;
            length = 0;
        } else {
            repeat = r.bits(7) + 11;
            length = 0;
        }
        for (std::uint32_t k = 0; k < repeat && i < symbol_count; ++k, ++i) {
            HuffmanLine line;
            line.prefix_length = length;
            line.range_low = static_cast<std::int32_t>(i);
            lines.push_back(line);
        }
    }
    std::erase_if(lines, [](HuffmanLine const& l) { return l.prefix_length == 0; });
    r.align();
    if (!r.ok())
        return std::nullopt;
    return HuffmanTable(std::move(lines));
}

// The whole decoding of one embedded stream pair.
class Decoder {
public:
    Decoder(int width, int height)
        : m_width(width)
        , m_height(height)
        , m_budget(width, height)
    {
    }

    bool run(std::span<std::uint8_t const> globals, std::span<std::uint8_t const> page)
    {
        if (!globals.empty() && !read_stream(globals))
            return false;
        return read_stream(page);
    }

    Bitmap const& page() const { return m_page; }
    bool has_page() const { return m_page.width > 0; }

private:
    bool read_stream(std::span<std::uint8_t const> data)
    {
        Reader r(data);
        // A file header (D.4.1), should a stream carry one: the sequential
        // organization is read; random-access puts every header first.
        static constexpr std::uint8_t id[8] = { 0x97, 0x4A, 0x42, 0x32, 0x0D, 0x0A, 0x1A, 0x0A };
        bool random_access = false;
        if (data.size() >= 9 && std::memcmp(data.data(), id, 8) == 0) {
            r.skip(8);
            std::uint8_t const flags = r.u8();
            random_access = !(flags & 1);
            if (!(flags & 2))
                r.u32(); // number of pages
        }
        if (random_access) {
            std::vector<SegmentHeader> headers;
            while (!r.at_end()) {
                std::optional<SegmentHeader> h = parse_segment_header(r);
                if (!h)
                    return false;
                bool const last = h->type == 51;
                headers.push_back(std::move(*h));
                if (last)
                    break;
            }
            for (SegmentHeader const& h : headers) {
                if (h.unknown_length)
                    return false;
                std::span<std::uint8_t const> const body = r.take(h.data_length);
                if (!r.ok())
                    return false;
                if (!handle_segment(h, body))
                    return false;
            }
            return true;
        }
        while (!r.at_end()) {
            std::optional<SegmentHeader> const h = parse_segment_header(r);
            if (!h)
                return !m_regions.empty() || has_page(); // a cut-short trailer keeps what was drawn
            std::size_t length = h->data_length;
            if (h->unknown_length) {
                // 7.2.7: only an immediate generic region may say so; its
                // data ends at a terminator and a row count.
                if (h->type != 36 && h->type != 38 && h->type != 39)
                    return false;
                std::optional<std::size_t> const found = unknown_region_length(r.rest());
                if (!found)
                    return false;
                length = *found;
            }
            std::span<std::uint8_t const> const body = r.take(length);
            if (!handle_segment(*h, body))
                return false;
            if (h->type == 51)
                break;
        }
        return true;
    }

    // 7.2.7: the length of an immediate generic region whose header does
    // not say: region info, flags, AT bytes, then the coded data up to
    // the terminator (0xFF 0xAC arithmetic, 0x00 0x00 MMR) and the four
    // bytes of the row count after it.
    static std::optional<std::size_t> unknown_region_length(std::span<std::uint8_t const> data)
    {
        Reader r(data);
        (void)parse_region_info(r);
        std::uint8_t const flags = r.u8();
        bool const mmr = flags & 1;
        int const gb_template = (flags >> 1) & 3;
        if (!mmr)
            r.skip(gb_template == 0 ? 8 : 2);
        if (!r.ok())
            return std::nullopt;
        std::uint8_t const t0 = mmr ? 0x00 : 0xFF;
        std::uint8_t const t1 = mmr ? 0x00 : 0xAC;
        for (std::size_t i = r.position(); i + 1 < data.size(); ++i) {
            if (data[i] == t0 && data[i + 1] == t1)
                return std::min(data.size(), i + 2 + 4);
        }
        return std::nullopt;
    }

    bool handle_segment(SegmentHeader const& h, std::span<std::uint8_t const> data)
    {
        // SASHFOLD_JBIG2_TRACE=1: each segment and whether it decoded, the
        // instrument for a scan that comes out wrong.
        bool const ok = handle_segment_inner(h, data);
        if (std::getenv("SASHFOLD_JBIG2_TRACE"))
            std::fprintf(stderr, "jbig2: segment %u type %d length %zu -> %s\n", h.number, h.type, data.size(), ok ? "ok" : "FAILED");
        return ok;
    }

    bool handle_segment_inner(SegmentHeader const& h, std::span<std::uint8_t const> data)
    {
        switch (h.type) {
        case 0:
            return read_symbol_dictionary(h, data);
        case 4:
        case 6:
        case 7:
            return read_text_region(h, data, h.type == 4);
        case 16:
            return read_pattern_dictionary(h, data);
        case 20:
        case 22:
        case 23:
            return read_halftone_region(h, data, h.type == 20);
        case 36:
        case 38:
        case 39:
            return read_generic_region(h, data, h.type == 36);
        case 40:
        case 42:
        case 43:
            return read_refinement_region(h, data, h.type == 40);
        case 48:
            return read_page_info(data);
        case 50: // end of stripe: the page is the image's size already
            ensure_page();
            return true;
        case 53: {
            std::optional<HuffmanTable> table = parse_table_segment(data);
            if (!table)
                return false;
            m_tables[h.number] = std::move(*table);
            return true;
        }
        default:
            // End of page/file, profiles, extensions, colour palette: nothing to draw.
            return true;
        }
    }

    // --- The page (8.2) ---

    bool read_page_info(std::span<std::uint8_t const> data)
    {
        Reader r(data);
        std::uint32_t const width = r.u32();
        std::uint32_t const height = r.u32();
        r.u32(); // x resolution
        r.u32(); // y resolution
        std::uint8_t const flags = r.u8();
        if (!r.ok())
            return false;
        m_page_default = (flags >> 2) & 1;
        (void)width;
        (void)height;
        // The page bitmap is the image's size: the PDF says what the
        // picture is, a striped page with an unknown height included.
        ensure_page();
        return true;
    }

    void ensure_page()
    {
        if (m_page.width == 0)
            m_page = Bitmap(m_width, m_height, m_page_default);
    }

    void draw_on_page(Bitmap const& region, RegionInfo const& info)
    {
        ensure_page();
        if (info.x > 0x7FFFFFFFu || info.y > 0x7FFFFFFFu)
            return;
        compose(m_page, region, static_cast<int>(info.x), static_cast<int>(info.y), info.op);
    }

    // --- Referred segments ---

    std::vector<HuffmanTable const*> referred_tables(SegmentHeader const& h) const
    {
        std::vector<HuffmanTable const*> out;
        for (std::uint32_t n : h.referred) {
            auto const it = m_tables.find(n);
            if (it != m_tables.end())
                out.push_back(&it->second);
        }
        return out;
    }

    std::vector<SymbolDictionary const*> referred_dictionaries(SegmentHeader const& h) const
    {
        std::vector<SymbolDictionary const*> out;
        for (std::uint32_t n : h.referred) {
            auto const it = m_symbol_dictionaries.find(n);
            if (it != m_symbol_dictionaries.end())
                out.push_back(it->second.get());
        }
        return out;
    }

    // --- 7.4.6: generic region ---

    bool read_generic_region(SegmentHeader const& h, std::span<std::uint8_t const> data, bool intermediate)
    {
        Reader r(data);
        RegionInfo info = parse_region_info(r);
        std::uint8_t const flags = r.u8();
        bool const mmr = flags & 1;
        int const gb_template = (flags >> 1) & 3;
        bool const tpgdon = (flags >> 3) & 1;
        bool const ext_template = (flags >> 4) & 1;
        if (info.colour || ext_template)
            return false;
        std::array<AtPixel, 4> at {};
        if (!mmr) {
            int const count = gb_template == 0 ? 4 : 1;
            for (int i = 0; i < count; ++i) {
                at[static_cast<std::size_t>(i)].x = r.s8();
                at[static_cast<std::size_t>(i)].y = r.s8();
            }
        }
        if (!r.ok())
            return false;
        if (h.unknown_length) {
            // The row count after the terminator replaces the height.
            if (data.size() >= 4) {
                Reader tail(data.subspan(data.size() - 4));
                std::uint32_t const rows = tail.u32();
                if (rows < info.height)
                    info.height = rows;
            }
            data = data.subspan(0, data.size() - std::min<std::size_t>(4, data.size()));
            r = Reader(data);
            r.skip(17 + 1 + (mmr ? 0 : (gb_template == 0 ? 8 : 2)));
        }
        if (info.height == 0xFFFFFFFFu && m_height > 0) {
            // A region of unknown height runs to the page's end.
            info.height = static_cast<std::uint32_t>(std::max(0, m_height - static_cast<int>(std::min<std::uint32_t>(info.y, 0x7FFFFFFFu))));
        }
        if (!Bitmap::fits(info.width, info.height))
            return false;
        Bitmap bitmap;
        if (mmr) {
            MmrDecoder decoder(r.rest());
            if (!decode_generic_mmr(decoder, static_cast<int>(info.width), static_cast<int>(info.height), m_budget, bitmap))
                return false;
        } else {
            MqDecoder decoder(r.rest());
            GenericContexts contexts;
            GenericParams p;
            p.width = static_cast<int>(info.width);
            p.height = static_cast<int>(info.height);
            p.gb_template = gb_template;
            p.at = at;
            p.tpgdon = tpgdon;
            p.budget = &m_budget;
            if (!decode_generic(decoder, contexts, p, bitmap))
                return false;
        }
        if (intermediate)
            m_regions[h.number] = { std::move(bitmap), info };
        else
            draw_on_page(bitmap, info);
        return true;
    }

    // --- 7.4.7: refinement region ---

    bool read_refinement_region(SegmentHeader const& h, std::span<std::uint8_t const> data, bool intermediate)
    {
        Reader r(data);
        RegionInfo const info = parse_region_info(r);
        std::uint8_t const flags = r.u8();
        int const gr_template = flags & 1;
        bool const tpgron = (flags >> 1) & 1;
        std::array<AtPixel, 2> at {};
        if (gr_template == 0) {
            for (auto& a : at) {
                a.x = r.s8();
                a.y = r.s8();
            }
        }
        if (!r.ok() || info.colour || !m_budget.take(info.width, info.height))
            return false;
        // The reference: an intermediate region referred to, else the page
        // under the region (6.3.2), which the result then replaces.
        Bitmap reference;
        bool from_page = true;
        for (std::uint32_t n : h.referred) {
            auto const it = m_regions.find(n);
            if (it != m_regions.end()) {
                reference = it->second.bitmap;
                from_page = false;
                m_regions.erase(it);
                break;
            }
        }
        if (from_page) {
            ensure_page();
            reference = Bitmap(static_cast<int>(info.width), static_cast<int>(info.height));
            for (int y = 0; y < reference.height; ++y) {
                for (int x = 0; x < reference.width; ++x)
                    reference.set(x, y, m_page.get(x + static_cast<int>(info.x), y + static_cast<int>(info.y)));
            }
        }
        MqDecoder decoder(r.rest());
        RefinementContexts contexts;
        RefinementParams p;
        p.width = static_cast<int>(info.width);
        p.height = static_cast<int>(info.height);
        p.gr_template = gr_template;
        p.reference = &reference;
        p.dx = 0;
        p.dy = 0;
        p.tpgron = tpgron;
        p.at = at;
        p.budget = &m_budget;
        Bitmap bitmap;
        if (!decode_refinement(decoder, contexts, p, bitmap))
            return false;
        if (intermediate) {
            m_regions[h.number] = { std::move(bitmap), info };
        } else {
            RegionInfo drawn = info;
            if (from_page)
                drawn.op = Op::Replace;
            draw_on_page(bitmap, drawn);
        }
        return true;
    }

    // --- 7.4.3: symbol dictionary (6.5) ---

    bool read_symbol_dictionary(SegmentHeader const& h, std::span<std::uint8_t const> data)
    {
        Reader r(data);
        std::uint16_t const flags = r.u16();
        bool const huffman = flags & 1;
        bool const refagg = (flags >> 1) & 1;
        int const sel_dh = (flags >> 2) & 3;
        int const sel_dw = (flags >> 4) & 3;
        int const sel_bmsize = (flags >> 6) & 1;
        int const sel_agginst = (flags >> 7) & 1;
        bool const contexts_used = (flags >> 8) & 1;
        bool const contexts_retained = (flags >> 9) & 1;
        int const sd_template = (flags >> 10) & 3;
        int const sdr_template = (flags >> 12) & 1;
        std::array<AtPixel, 4> at {};
        if (!huffman) {
            int const count = sd_template == 0 ? 4 : 1;
            for (int i = 0; i < count; ++i) {
                at[static_cast<std::size_t>(i)].x = r.s8();
                at[static_cast<std::size_t>(i)].y = r.s8();
            }
        }
        std::array<AtPixel, 2> rat {};
        if (refagg && sdr_template == 0) {
            for (auto& a : rat) {
                a.x = r.s8();
                a.y = r.s8();
            }
        }
        std::uint32_t const exported_count = r.u32();
        std::uint32_t const new_count = r.u32();
        if (!r.ok() || new_count > 100000 || exported_count > 100000)
            return false;

        // Input symbols: those exported by the dictionaries referred to.
        std::vector<SymbolDictionary const*> const inputs = referred_dictionaries(h);
        std::vector<Bitmap const*> symbols;
        for (SymbolDictionary const* d : inputs) {
            for (Bitmap const& b : d->exported)
                symbols.push_back(&b);
        }
        std::uint32_t const input_count = static_cast<std::uint32_t>(symbols.size());
        std::vector<Bitmap> new_symbols(new_count);
        for (Bitmap& b : new_symbols)
            symbols.push_back(&b); // filled in as decoded

        // Huffman tables (7.4.3.1.2), the custom ones in the order referred.
        std::vector<HuffmanTable const*> const custom = referred_tables(h);
        std::size_t custom_index = 0;
        auto const pick = [&](int selector, int std_a, int std_b) -> HuffmanTable const* {
            if (selector == 3 || (std_b < 0 && selector == 1)) {
                if (custom_index < custom.size())
                    return custom[custom_index++];
                return nullptr;
            }
            return &standard_table(selector == 0 ? std_a : std_b);
        };
        HuffmanTable const* table_dh = nullptr;
        HuffmanTable const* table_dw = nullptr;
        HuffmanTable const* table_bmsize = nullptr;
        HuffmanTable const* table_agginst = nullptr;
        if (huffman) {
            table_dh = pick(sel_dh, 4, 5);
            table_dw = pick(sel_dw, 2, 3);
            table_bmsize = pick(sel_bmsize, 1, -1);
            table_agginst = pick(sel_agginst, 1, -1);
            if (!table_dh || !table_dw || !table_bmsize || !table_agginst)
                return false;
        }

        // Arithmetic contexts, perhaps those a referred dictionary kept.
        std::shared_ptr<GenericContexts> gcx;
        std::shared_ptr<RefinementContexts> rcx;
        if (contexts_used) {
            for (SymbolDictionary const* d : inputs) {
                if (d->generic_contexts) {
                    gcx = std::make_shared<GenericContexts>(*d->generic_contexts);
                    rcx = d->refinement_contexts ? std::make_shared<RefinementContexts>(*d->refinement_contexts)
                                                 : std::make_shared<RefinementContexts>();
                }
            }
        }
        if (!gcx)
            gcx = std::make_shared<GenericContexts>();
        if (!rcx)
            rcx = std::make_shared<RefinementContexts>();

        IntContext iadh, iadw, iaex, iaai;
        TextContexts tc;
        int const symcodelen = std::max(1, ceil_log2(input_count + new_count));
        tc.iaid = std::make_unique<IaidContext>(symcodelen);
        std::optional<MqDecoder> decoder;
        if (!huffman)
            decoder.emplace(r.rest());

        // 6.5.5: height classes.
        std::int64_t hcheight = 0;
        std::uint32_t decoded = 0;
        std::vector<int> widths; // Huffman, no refinement: the class's widths
        while (decoded < new_count) {
            std::optional<std::int32_t> const dh = huffman ? table_dh->decode(r) : decode_int(*decoder, iadh);
            if (!dh)
                return false;
            hcheight += *dh;
            if (hcheight <= 0 || hcheight > 10000)
                return false;
            std::int64_t symwidth = 0;
            std::int64_t totwidth = 0;
            std::uint32_t const first_symbol = decoded;
            widths.clear();
            while (true) {
                bool oob = false;
                std::optional<std::int32_t> dw;
                if (huffman) {
                    dw = table_dw->decode(r, &oob);
                    if (!dw && !oob)
                        return false;
                } else {
                    dw = decode_int(*decoder, iadw);
                }
                if (!dw)
                    break; // OOB: the height class is complete
                symwidth += *dw;
                totwidth += symwidth;
                if (symwidth <= 0 || symwidth > 10000 || decoded >= new_count)
                    return false;
                Bitmap& target = new_symbols[decoded];
                if (huffman && !refagg) {
                    widths.push_back(static_cast<int>(symwidth));
                } else if (!refagg) {
                    GenericParams p;
                    p.width = static_cast<int>(symwidth);
                    p.height = static_cast<int>(hcheight);
                    p.gb_template = sd_template;
                    p.at = at;
                    p.tpgdon = false;
                    p.budget = &m_budget;
                    if (!decode_generic(*decoder, *gcx, p, target))
                        return false;
                } else {
                    std::optional<std::int32_t> const ninst
                        = huffman ? table_agginst->decode(r) : decode_int(*decoder, iaai);
                    if (!ninst)
                        return false;
                    if (*ninst == 1) {
                        // 6.5.8.2.2: one symbol refined.
                        std::uint32_t id;
                        std::int32_t rdx, rdy;
                        std::int32_t bmsize = 0;
                        if (huffman) {
                            id = r.bits(symcodelen);
                            std::optional<std::int32_t> const x = standard_table(15).decode(r);
                            std::optional<std::int32_t> const y = standard_table(15).decode(r);
                            std::optional<std::int32_t> const size = standard_table(1).decode(r);
                            if (!x || !y || !size)
                                return false;
                            rdx = *x;
                            rdy = *y;
                            bmsize = *size;
                            r.align();
                        } else {
                            id = decode_iaid(*decoder, *tc.iaid);
                            std::optional<std::int32_t> const x = decode_int(*decoder, tc.iardx);
                            std::optional<std::int32_t> const y = decode_int(*decoder, tc.iardy);
                            if (!x || !y)
                                return false;
                            rdx = *x;
                            rdy = *y;
                        }
                        if (id >= symbols.size() || !symbols[id] || symbols[id]->width == 0)
                            return false;
                        RefinementParams rp;
                        rp.width = static_cast<int>(symwidth);
                        rp.height = static_cast<int>(hcheight);
                        rp.gr_template = sdr_template;
                        rp.reference = symbols[id];
                        rp.dx = rdx;
                        rp.dy = rdy;
                        rp.tpgron = false;
                        rp.at = rat;
                        rp.budget = &m_budget;
                        if (huffman) {
                            std::span<std::uint8_t const> const bytes
                                = bmsize > 0 ? r.take(static_cast<std::size_t>(bmsize)) : r.rest();
                            MqDecoder local(bytes);
                            if (!decode_refinement(local, *rcx, rp, target))
                                return false;
                            if (bmsize == 0)
                                r.skip(local.position());
                            r.align();
                        } else if (!decode_refinement(*decoder, *rcx, rp, target)) {
                            return false;
                        }
                    } else {
                        // 6.5.8.2.1: a text region of ninst instances.
                        TextRegionParams tp;
                        tp.huffman = huffman;
                        tp.refine = true;
                        tp.width = static_cast<int>(symwidth);
                        tp.height = static_cast<int>(hcheight);
                        tp.instances = static_cast<std::uint32_t>(std::max(0, *ninst));
                        tp.log_strips = 0;
                        tp.strips = 1;
                        tp.ref_corner = 1;
                        tp.transposed = false;
                        tp.op = Op::Or;
                        tp.default_pixel = 0;
                        tp.ds_offset = 0;
                        tp.rtemplate = sdr_template;
                        tp.rat = rat;
                        tp.symbols = symbols;
                        tp.symcodelen = symcodelen;
                        tp.budget = &m_budget;
                        HuffmanTable symcode;
                        if (huffman) {
                            // Table 17: the symbol codes are symcodelen bits each.
                            std::vector<HuffmanLine> lines;
                            for (std::uint32_t i = 0; i < symbols.size(); ++i) {
                                HuffmanLine line;
                                line.prefix_length = symcodelen;
                                line.range_low = static_cast<std::int32_t>(i);
                                lines.push_back(line);
                            }
                            symcode = HuffmanTable(std::move(lines));
                            tp.fs = &standard_table(6);
                            tp.ds = &standard_table(8);
                            tp.dt = &standard_table(11);
                            tp.rdw = &standard_table(15);
                            tp.rdh = &standard_table(15);
                            tp.rdx = &standard_table(15);
                            tp.rdy = &standard_table(15);
                            tp.rsize = &standard_table(1);
                            tp.symcode = &symcode;
                        }
                        if (!decode_text_region(tp, huffman ? nullptr : &*decoder, huffman ? &r : nullptr, tc, *rcx, target))
                            return false;
                    }
                }
                ++decoded;
            }
            if (huffman && !refagg) {
                // 6.5.9: the height class's collective bitmap.
                std::optional<std::int32_t> const bmsize = table_bmsize->decode(r);
                if (!bmsize)
                    return false;
                r.align();
                if (totwidth <= 0 || !m_budget.take(static_cast<std::uint64_t>(totwidth), static_cast<std::uint64_t>(hcheight)))
                    return false;
                Bitmap collective(static_cast<int>(totwidth), static_cast<int>(hcheight));
                if (*bmsize == 0) {
                    // Uncompressed, rows padded to bytes.
                    std::size_t const stride = (static_cast<std::size_t>(totwidth) + 7) / 8;
                    std::span<std::uint8_t const> const raw = r.take(stride * static_cast<std::size_t>(hcheight));
                    if (!r.ok())
                        return false;
                    for (int y = 0; y < collective.height; ++y) {
                        for (int x = 0; x < collective.width; ++x) {
                            std::uint8_t const byte = raw[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x >> 3)];
                            collective.row(y)[x] = (byte >> (7 - (x & 7))) & 1;
                        }
                    }
                } else {
                    std::span<std::uint8_t const> const bytes = r.take(static_cast<std::size_t>(*bmsize));
                    if (!r.ok())
                        return false;
                    MmrDecoder mmr(bytes);
                    mmr.decode(collective.width, collective.height, collective.pixels.data());
                }
                int x = 0;
                for (std::size_t i = 0; i < widths.size(); ++i) {
                    Bitmap& symbol = new_symbols[first_symbol + i];
                    symbol = Bitmap(widths[i], collective.height);
                    for (int y = 0; y < symbol.height; ++y)
                        std::copy_n(collective.row(y) + x, widths[i], symbol.row(y));
                    x += widths[i];
                }
            }
        }

        // 6.5.10: which symbols are exported, as runs that alternate.
        auto dictionary = std::make_unique<SymbolDictionary>();
        std::uint32_t index = 0;
        bool exporting = false;
        std::uint32_t const total = input_count + new_count;
        // Broken data could say empty runs forever: as many runs as there
        // are symbols, twice, is every alternation possible.
        for (std::uint32_t runs = 0; index < total && dictionary->exported.size() < exported_count; ++runs) {
            if (runs > 2 * total + 2)
                return false;
            std::optional<std::int32_t> const run = huffman ? standard_table(1).decode(r) : decode_int(*decoder, iaex);
            if (!run || *run < 0)
                return false;
            std::uint32_t const length = std::min<std::uint32_t>(static_cast<std::uint32_t>(*run), total - index);
            if (exporting) {
                for (std::uint32_t i = 0; i < length; ++i)
                    dictionary->exported.push_back(*symbols[index + i]);
            }
            index += length;
            exporting = !exporting;
        }
        if (contexts_retained) {
            dictionary->generic_contexts = gcx;
            dictionary->refinement_contexts = rcx;
        }
        m_symbol_dictionaries[h.number] = std::move(dictionary);
        return true;
    }

    // --- 7.4.4: text region (6.4) ---

    bool read_text_region(SegmentHeader const& h, std::span<std::uint8_t const> data, bool intermediate)
    {
        Reader r(data);
        RegionInfo const info = parse_region_info(r);
        std::uint16_t const flags = r.u16();
        TextRegionParams p;
        p.huffman = flags & 1;
        p.refine = (flags >> 1) & 1;
        p.log_strips = (flags >> 2) & 3;
        p.strips = 1 << p.log_strips;
        p.ref_corner = (flags >> 4) & 3;
        p.transposed = (flags >> 6) & 1;
        p.op = op_from((flags >> 7) & 3);
        p.default_pixel = static_cast<std::uint8_t>((flags >> 9) & 1);
        int ds_offset = (flags >> 10) & 0x1F;
        if (ds_offset > 15)
            ds_offset -= 32;
        p.ds_offset = ds_offset;
        p.rtemplate = (flags >> 15) & 1;
        int sel_fs = 0, sel_ds = 0, sel_dt = 0, sel_rdw = 0, sel_rdh = 0, sel_rdx = 0, sel_rdy = 0, sel_rsize = 0;
        if (p.huffman) {
            std::uint16_t const hf = r.u16();
            sel_fs = hf & 3;
            sel_ds = (hf >> 2) & 3;
            sel_dt = (hf >> 4) & 3;
            sel_rdw = (hf >> 6) & 3;
            sel_rdh = (hf >> 8) & 3;
            sel_rdx = (hf >> 10) & 3;
            sel_rdy = (hf >> 12) & 3;
            sel_rsize = (hf >> 14) & 1;
        }
        if (p.refine && p.rtemplate == 0) {
            for (auto& a : p.rat) {
                a.x = r.s8();
                a.y = r.s8();
            }
        }
        p.instances = r.u32();
        if (!r.ok() || info.colour || !Bitmap::fits(info.width, info.height))
            return false;
        p.width = static_cast<int>(info.width);
        p.height = static_cast<int>(info.height);

        for (SymbolDictionary const* d : referred_dictionaries(h)) {
            for (Bitmap const& b : d->exported)
                p.symbols.push_back(&b);
        }
        std::uint32_t const symbol_count = static_cast<std::uint32_t>(p.symbols.size());
        p.symcodelen = std::max(1, ceil_log2(symbol_count));

        HuffmanTable symcode;
        if (p.huffman) {
            std::vector<HuffmanTable const*> const custom = referred_tables(h);
            std::size_t custom_index = 0;
            auto const pick = [&](int selector, int a, int b, int c) -> HuffmanTable const* {
                if (selector == 3 || (c < 0 && selector == 2) || (b < 0 && selector == 1)) {
                    if (custom_index < custom.size())
                        return custom[custom_index++];
                    return nullptr;
                }
                return &standard_table(selector == 0 ? a : selector == 1 ? b : c);
            };
            p.fs = pick(sel_fs, 6, 7, -1);
            p.ds = pick(sel_ds, 8, 9, 10);
            p.dt = pick(sel_dt, 11, 12, 13);
            p.rdw = pick(sel_rdw, 14, 15, -1);
            p.rdh = pick(sel_rdh, 14, 15, -1);
            p.rdx = pick(sel_rdx, 14, 15, -1);
            p.rdy = pick(sel_rdy, 14, 15, -1);
            p.rsize = sel_rsize == 0 ? &standard_table(1) : (custom_index < custom.size() ? custom[custom_index++] : nullptr);
            if (!p.fs || !p.ds || !p.dt || !p.rdw || !p.rdh || !p.rdx || !p.rdy || !p.rsize)
                return false;
            std::optional<HuffmanTable> codes = read_symbol_id_codes(r, symbol_count);
            if (!codes)
                return false;
            symcode = std::move(*codes);
            p.symcode = &symcode;
        }

        TextContexts tc;
        tc.iaid = std::make_unique<IaidContext>(p.symcodelen);
        RefinementContexts rcx;
        p.budget = &m_budget;
        Bitmap bitmap;
        std::optional<MqDecoder> decoder;
        if (!p.huffman)
            decoder.emplace(r.rest());
        if (!decode_text_region(p, p.huffman ? nullptr : &*decoder, p.huffman ? &r : nullptr, tc, rcx, bitmap))
            return false;
        if (intermediate)
            m_regions[h.number] = { std::move(bitmap), info };
        else
            draw_on_page(bitmap, info);
        return true;
    }

    // --- 7.4.5: pattern dictionary (6.7) ---

    bool read_pattern_dictionary(SegmentHeader const& h, std::span<std::uint8_t const> data)
    {
        Reader r(data);
        std::uint8_t const flags = r.u8();
        bool const mmr = flags & 1;
        int const hd_template = (flags >> 1) & 3;
        int const hdpw = r.u8();
        int const hdph = r.u8();
        std::uint32_t const graymax = r.u32();
        if (!r.ok() || hdpw == 0 || hdph == 0 || graymax > 10000)
            return false;
        // 6.7.5: one collective bitmap, the patterns side by side.
        std::uint64_t const total_width = (static_cast<std::uint64_t>(graymax) + 1) * static_cast<std::uint64_t>(hdpw);
        if (!Bitmap::fits(total_width, static_cast<std::uint64_t>(hdph)))
            return false;
        Bitmap collective;
        if (mmr) {
            MmrDecoder decoder(r.rest());
            if (!decode_generic_mmr(decoder, static_cast<int>(total_width), hdph, m_budget, collective))
                return false;
        } else {
            MqDecoder decoder(r.rest());
            GenericContexts contexts;
            GenericParams p;
            p.width = static_cast<int>(total_width);
            p.height = hdph;
            p.gb_template = hd_template;
            p.at = { AtPixel { static_cast<std::int8_t>(-hdpw), 0 }, AtPixel { -3, -1 }, AtPixel { 2, -2 }, AtPixel { -2, -2 } };
            p.tpgdon = false;
            p.budget = &m_budget;
            if (!decode_generic(decoder, contexts, p, collective))
                return false;
        }
        auto dictionary = std::make_unique<PatternDictionary>();
        for (std::uint32_t i = 0; i <= graymax; ++i) {
            Bitmap pattern(hdpw, hdph);
            for (int y = 0; y < hdph; ++y)
                std::copy_n(collective.row(y) + static_cast<std::size_t>(i) * static_cast<std::size_t>(hdpw), hdpw, pattern.row(y));
            dictionary->patterns.push_back(std::move(pattern));
        }
        m_pattern_dictionaries[h.number] = std::move(dictionary);
        return true;
    }

    // --- 7.4.5.2: halftone region (6.6) ---

    bool read_halftone_region(SegmentHeader const& h, std::span<std::uint8_t const> data, bool intermediate)
    {
        Reader r(data);
        RegionInfo const info = parse_region_info(r);
        std::uint8_t const flags = r.u8();
        bool const mmr = flags & 1;
        int const h_template = (flags >> 1) & 3;
        bool const enskip = (flags >> 3) & 1;
        Op const comb_op = op_from((flags >> 4) & 7);
        std::uint8_t const default_pixel = (flags >> 7) & 1;
        std::uint32_t const hgw = r.u32();
        std::uint32_t const hgh = r.u32();
        std::int32_t const hgx = r.s32();
        std::int32_t const hgy = r.s32();
        std::uint16_t const hrx = r.u16();
        std::uint16_t const hry = r.u16();
        if (!r.ok() || info.colour || !m_budget.take(info.width, info.height) || !m_budget.take(hgw, hgh))
            return false;
        PatternDictionary const* patterns = nullptr;
        for (std::uint32_t n : h.referred) {
            auto const it = m_pattern_dictionaries.find(n);
            if (it != m_pattern_dictionaries.end())
                patterns = it->second.get();
        }
        if (!patterns || patterns->patterns.empty())
            return false;
        int const hpw = patterns->patterns[0].width;
        int const hph = patterns->patterns[0].height;
        Bitmap region(static_cast<int>(info.width), static_cast<int>(info.height), default_pixel);

        // 6.6.5.1: the grid cells whose pattern lies wholly outside.
        Bitmap skip;
        if (enskip) {
            skip = Bitmap(static_cast<int>(hgw), static_cast<int>(hgh));
            for (std::uint32_t m = 0; m < hgh; ++m) {
                for (std::uint32_t n = 0; n < hgw; ++n) {
                    std::int64_t const x = (static_cast<std::int64_t>(hgx) + static_cast<std::int64_t>(m) * hry
                                               + static_cast<std::int64_t>(n) * hrx)
                        >> 8;
                    std::int64_t const y = (static_cast<std::int64_t>(hgy) + static_cast<std::int64_t>(m) * hrx
                                               - static_cast<std::int64_t>(n) * hry)
                        >> 8;
                    if (x + hpw <= 0 || x >= region.width || y + hph <= 0 || y >= region.height)
                        skip.row(static_cast<int>(m))[n] = 1;
                }
            }
        }

        // Annex C.5: the grayscale image, bitplanes from the top, Gray-coded.
        int const bpp = ceil_log2(static_cast<std::uint32_t>(patterns->patterns.size()));
        int const planes_count = std::max(1, bpp);
        std::vector<std::uint32_t> gray(static_cast<std::size_t>(hgw) * static_cast<std::size_t>(hgh), 0);
        std::optional<MmrDecoder> mmr_decoder;
        std::optional<MqDecoder> decoder;
        GenericContexts contexts;
        if (mmr)
            mmr_decoder.emplace(r.rest());
        else
            decoder.emplace(r.rest());
        Bitmap previous;
        for (int j = planes_count - 1; j >= 0; --j) {
            Bitmap plane;
            if (mmr) {
                if (!decode_generic_mmr(*mmr_decoder, static_cast<int>(hgw), static_cast<int>(hgh), m_budget, plane))
                    return false;
            } else {
                GenericParams p;
                p.width = static_cast<int>(hgw);
                p.height = static_cast<int>(hgh);
                p.gb_template = h_template;
                p.at = { AtPixel { static_cast<std::int8_t>(h_template <= 1 ? 3 : 2), -1 }, AtPixel { -3, -1 },
                    AtPixel { 2, -2 }, AtPixel { -2, -2 } };
                p.tpgdon = false;
                p.skip = enskip ? &skip : nullptr;
                p.budget = &m_budget;
                if (!decode_generic(*decoder, contexts, p, plane))
                    return false;
            }
            // GRAYCODE: this plane's bit is its value XOR the bit above.
            for (std::size_t i = 0; i < plane.pixels.size(); ++i) {
                std::uint32_t bit = plane.pixels[i];
                if (j != planes_count - 1)
                    bit ^= previous.pixels[i];
                plane.pixels[i] = static_cast<std::uint8_t>(bit);
                gray[i] |= bit << j;
            }
            previous = std::move(plane);
            // C.5 with MMR: each plane's data ends with an EOFB, the next
            // starting on a byte (as jbig2dec reads them).
            if (mmr)
                mmr_decoder->skip_end_of_block();
        }

        // 6.6.5.2: a pattern at each grid cell.
        for (std::uint32_t m = 0; m < hgh; ++m) {
            for (std::uint32_t n = 0; n < hgw; ++n) {
                if (enskip && skip.row(static_cast<int>(m))[n])
                    continue;
                std::int64_t const x = (static_cast<std::int64_t>(hgx) + static_cast<std::int64_t>(m) * hry
                                           + static_cast<std::int64_t>(n) * hrx)
                    >> 8;
                std::int64_t const y = (static_cast<std::int64_t>(hgy) + static_cast<std::int64_t>(m) * hrx
                                           - static_cast<std::int64_t>(n) * hry)
                    >> 8;
                std::uint32_t const value = gray[static_cast<std::size_t>(m) * hgw + n];
                Bitmap const& pattern = patterns->patterns[std::min<std::size_t>(value, patterns->patterns.size() - 1)];
                if (!m_budget.take(static_cast<std::uint64_t>(hpw), static_cast<std::uint64_t>(hph)))
                    return false;
                if (x > -100000 && x < 100000 && y > -100000 && y < 100000)
                    compose(region, pattern, static_cast<int>(x), static_cast<int>(y), comb_op);
            }
        }
        if (intermediate)
            m_regions[h.number] = { std::move(region), info };
        else
            draw_on_page(region, info);
        return true;
    }

    struct Region {
        Bitmap bitmap;
        RegionInfo info;
    };

    int m_width;
    int m_height;
    Budget m_budget;
    Bitmap m_page;
    std::uint8_t m_page_default = 0;
    std::map<std::uint32_t, Region> m_regions;
    std::map<std::uint32_t, std::unique_ptr<SymbolDictionary>> m_symbol_dictionaries;
    std::map<std::uint32_t, std::unique_ptr<PatternDictionary>> m_pattern_dictionaries;
    std::map<std::uint32_t, HuffmanTable> m_tables;
};

} // namespace

std::optional<std::vector<std::uint8_t>> decode_jbig2(std::vector<std::uint8_t> const& data,
    std::vector<std::uint8_t> const& globals, int width, int height)
{
    if (width <= 0 || height <= 0 || !Bitmap::fits(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height)))
        return std::nullopt;
    Decoder decoder(width, height);
    bool const whole = decoder.run(globals, data);
    if (!decoder.has_page())
        return std::nullopt;
    (void)whole; // damaged data: what was drawn stands
    // Packed rows, the filter's sense: 0 = black (ISO 32000-2 §7.4.7).
    Bitmap const& page = decoder.page();
    std::size_t const stride = (static_cast<std::size_t>(width) + 7) / 8;
    std::vector<std::uint8_t> out(stride * static_cast<std::size_t>(height), 0xFF);
    for (int y = 0; y < height; ++y) {
        std::uint8_t const* row = page.row(y);
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            if (row[x])
                dst[x >> 3] &= static_cast<std::uint8_t>(~(0x80 >> (x & 7)));
        }
    }
    return out;
}

}
