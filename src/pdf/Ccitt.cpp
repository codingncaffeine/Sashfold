#include "pdf/ImageCodecs.h"

// CCITT fax pictures, PDF's /CCITTFaxDecode (ISO 32000-2 §7.4.6), written
// from ITU-T T.4 (Group 3: one-dimensional Modified Huffman, and the
// two-dimensional Modified READ, mixed by K > 0) and T.6 (Group 4: Modified
// Modified READ, K < 0).
//
// A line is a list of the places its color changes, starting white. A
// one-dimensional line is runs of alternating color, each run some
// make-up codes (multiples of 64) and one terminating code (0-63). A
// two-dimensional line is coded against the line above it (the
// reference, all white above the first): pass, horizontal (two runs as
// above) or vertical (the next change within three pixels of the
// reference's). Group 3 lines may begin with an end-of-line code
// (eleven zeros and a one) after fill zeros, and with K > 0 a tag bit
// after it says which coding the line has.
//
// Damaged data ends the picture where it fails: the lines decoded stand,
// the rest are white — as poppler and the browsers' PDF readers show one.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold::pdf {

namespace {

// --- The code tables (T.4 Tables 2 and 3), as (length, bits, run) -------

struct Code {
    std::uint8_t length;
    std::uint16_t bits;
    std::int16_t run;
};

// Terminating codes, runs 0-63, then make-up codes 64-1728.
constexpr Code white_codes[] = {
    { 8, 0b00110101, 0 }, { 6, 0b000111, 1 }, { 4, 0b0111, 2 }, { 4, 0b1000, 3 }, { 4, 0b1011, 4 }, { 4, 0b1100, 5 },
    { 4, 0b1110, 6 }, { 4, 0b1111, 7 }, { 5, 0b10011, 8 }, { 5, 0b10100, 9 }, { 5, 0b00111, 10 }, { 5, 0b01000, 11 },
    { 6, 0b001000, 12 }, { 6, 0b000011, 13 }, { 6, 0b110100, 14 }, { 6, 0b110101, 15 }, { 6, 0b101010, 16 },
    { 6, 0b101011, 17 }, { 7, 0b0100111, 18 }, { 7, 0b0001100, 19 }, { 7, 0b0001000, 20 }, { 7, 0b0010111, 21 },
    { 7, 0b0000011, 22 }, { 7, 0b0000100, 23 }, { 7, 0b0101000, 24 }, { 7, 0b0101011, 25 }, { 7, 0b0010011, 26 },
    { 7, 0b0100100, 27 }, { 7, 0b0011000, 28 }, { 8, 0b00000010, 29 }, { 8, 0b00000011, 30 }, { 8, 0b00011010, 31 },
    { 8, 0b00011011, 32 }, { 8, 0b00010010, 33 }, { 8, 0b00010011, 34 }, { 8, 0b00010100, 35 }, { 8, 0b00010101, 36 },
    { 8, 0b00010110, 37 }, { 8, 0b00010111, 38 }, { 8, 0b00101000, 39 }, { 8, 0b00101001, 40 }, { 8, 0b00101010, 41 },
    { 8, 0b00101011, 42 }, { 8, 0b00101100, 43 }, { 8, 0b00101101, 44 }, { 8, 0b00000100, 45 }, { 8, 0b00000101, 46 },
    { 8, 0b00001010, 47 }, { 8, 0b00001011, 48 }, { 8, 0b01010010, 49 }, { 8, 0b01010011, 50 }, { 8, 0b01010100, 51 },
    { 8, 0b01010101, 52 }, { 8, 0b00100100, 53 }, { 8, 0b00100101, 54 }, { 8, 0b01011000, 55 }, { 8, 0b01011001, 56 },
    { 8, 0b01011010, 57 }, { 8, 0b01011011, 58 }, { 8, 0b01001010, 59 }, { 8, 0b01001011, 60 }, { 8, 0b00110010, 61 },
    { 8, 0b00110011, 62 }, { 8, 0b00110100, 63 },
    { 5, 0b11011, 64 }, { 5, 0b10010, 128 }, { 6, 0b010111, 192 }, { 7, 0b0110111, 256 }, { 8, 0b00110110, 320 },
    { 8, 0b00110111, 384 }, { 8, 0b01100100, 448 }, { 8, 0b01100101, 512 }, { 8, 0b01101000, 576 },
    { 8, 0b01100111, 640 }, { 9, 0b011001100, 704 }, { 9, 0b011001101, 768 }, { 9, 0b011010010, 832 },
    { 9, 0b011010011, 896 }, { 9, 0b011010100, 960 }, { 9, 0b011010101, 1024 }, { 9, 0b011010110, 1088 },
    { 9, 0b011010111, 1152 }, { 9, 0b011011000, 1216 }, { 9, 0b011011001, 1280 }, { 9, 0b011011010, 1344 },
    { 9, 0b011011011, 1408 }, { 9, 0b010011000, 1472 }, { 9, 0b010011001, 1536 }, { 9, 0b010011010, 1600 },
    { 6, 0b011000, 1664 }, { 9, 0b010011011, 1728 },
};

constexpr Code black_codes[] = {
    { 10, 0b0000110111, 0 }, { 3, 0b010, 1 }, { 2, 0b11, 2 }, { 2, 0b10, 3 }, { 3, 0b011, 4 }, { 4, 0b0011, 5 },
    { 4, 0b0010, 6 }, { 5, 0b00011, 7 }, { 6, 0b000101, 8 }, { 6, 0b000100, 9 }, { 7, 0b0000100, 10 },
    { 7, 0b0000101, 11 }, { 7, 0b0000111, 12 }, { 8, 0b00000100, 13 }, { 8, 0b00000111, 14 }, { 9, 0b000011000, 15 },
    { 10, 0b0000010111, 16 }, { 10, 0b0000011000, 17 }, { 10, 0b0000001000, 18 }, { 11, 0b00001100111, 19 },
    { 11, 0b00001101000, 20 }, { 11, 0b00001101100, 21 }, { 11, 0b00000110111, 22 }, { 11, 0b00000101000, 23 },
    { 11, 0b00000010111, 24 }, { 11, 0b00000011000, 25 }, { 12, 0b000011001010, 26 }, { 12, 0b000011001011, 27 },
    { 12, 0b000011001100, 28 }, { 12, 0b000011001101, 29 }, { 12, 0b000001101000, 30 }, { 12, 0b000001101001, 31 },
    { 12, 0b000001101010, 32 }, { 12, 0b000001101011, 33 }, { 12, 0b000011010010, 34 }, { 12, 0b000011010011, 35 },
    { 12, 0b000011010100, 36 }, { 12, 0b000011010101, 37 }, { 12, 0b000011010110, 38 }, { 12, 0b000011010111, 39 },
    { 12, 0b000001101100, 40 }, { 12, 0b000001101101, 41 }, { 12, 0b000011011010, 42 }, { 12, 0b000011011011, 43 },
    { 12, 0b000001010100, 44 }, { 12, 0b000001010101, 45 }, { 12, 0b000001010110, 46 }, { 12, 0b000001010111, 47 },
    { 12, 0b000001100100, 48 }, { 12, 0b000001100101, 49 }, { 12, 0b000001010010, 50 }, { 12, 0b000001010011, 51 },
    { 12, 0b000000100100, 52 }, { 12, 0b000000110111, 53 }, { 12, 0b000000111000, 54 }, { 12, 0b000000100111, 55 },
    { 12, 0b000000101000, 56 }, { 12, 0b000001011000, 57 }, { 12, 0b000001011001, 58 }, { 12, 0b000000101011, 59 },
    { 12, 0b000000101100, 60 }, { 12, 0b000001011010, 61 }, { 12, 0b000001100110, 62 }, { 12, 0b000001100111, 63 },
    { 10, 0b0000001111, 64 }, { 12, 0b000011001000, 128 }, { 12, 0b000011001001, 192 }, { 12, 0b000001011011, 256 },
    { 12, 0b000000110011, 320 }, { 12, 0b000000110100, 384 }, { 12, 0b000000110101, 448 },
    { 13, 0b0000001101100, 512 }, { 13, 0b0000001101101, 576 }, { 13, 0b0000001001010, 640 },
    { 13, 0b0000001001011, 704 }, { 13, 0b0000001001100, 768 }, { 13, 0b0000001001101, 832 },
    { 13, 0b0000001110010, 896 }, { 13, 0b0000001110011, 960 }, { 13, 0b0000001110100, 1024 },
    { 13, 0b0000001110101, 1088 }, { 13, 0b0000001110110, 1152 }, { 13, 0b0000001110111, 1216 },
    { 13, 0b0000001010010, 1280 }, { 13, 0b0000001010011, 1344 }, { 13, 0b0000001010100, 1408 },
    { 13, 0b0000001010101, 1472 }, { 13, 0b0000001011010, 1536 }, { 13, 0b0000001011011, 1600 },
    { 13, 0b0000001100100, 1664 }, { 13, 0b0000001100101, 1728 },
};

// The make-up codes past 1728, the same for both colors (T.4 Table 3).
constexpr Code extended_codes[] = {
    { 11, 0b00000001000, 1792 }, { 11, 0b00000001100, 1856 }, { 11, 0b00000001101, 1920 },
    { 12, 0b000000010010, 1984 }, { 12, 0b000000010011, 2048 }, { 12, 0b000000010100, 2112 },
    { 12, 0b000000010101, 2176 }, { 12, 0b000000010110, 2240 }, { 12, 0b000000010111, 2304 },
    { 12, 0b000000011100, 2368 }, { 12, 0b000000011101, 2432 }, { 12, 0b000000011110, 2496 },
    { 12, 0b000000011111, 2560 },
};

constexpr int lookup_bits = 13;

// A color's codes by the next 13 bits: the code's length (0: none starts
// so) and its run.
struct Entry {
    std::uint8_t length = 0;
    std::int16_t run = 0;
};
using Table = std::array<Entry, 1u << lookup_bits>;

void fill(Table& table, Code const& code)
{
    int const free_bits = lookup_bits - code.length;
    std::uint32_t const first = static_cast<std::uint32_t>(code.bits) << free_bits;
    for (std::uint32_t i = 0; i < (1u << free_bits); ++i)
        table[first | i] = Entry { code.length, code.run };
}

Table build(Code const* codes, std::size_t count)
{
    Table table {};
    for (std::size_t i = 0; i < count; ++i)
        fill(table, codes[i]);
    for (Code const& code : extended_codes)
        fill(table, code);
    return table;
}

Table const& white_table()
{
    static Table const table = build(white_codes, std::size(white_codes));
    return table;
}

Table const& black_table()
{
    static Table const table = build(black_codes, std::size(black_codes));
    return table;
}

// --- Bits, first bit the byte's highest ----------------------------------

class Bits {
public:
    explicit Bits(std::vector<std::uint8_t> const& data)
        : m_data(data)
    {
    }

    // The next n bits (n <= 24), zeros past the end.
    std::uint32_t peek(int n) const
    {
        std::uint32_t value = 0;
        std::size_t position = m_position;
        for (int i = 0; i < n; ++i, ++position) {
            std::size_t const byte = position >> 3;
            int const bit = byte < m_data.size() ? (m_data[byte] >> (7 - (position & 7))) & 1 : 0;
            value = (value << 1) | static_cast<std::uint32_t>(bit);
        }
        return value;
    }
    void skip(int n) { m_position += static_cast<std::size_t>(n); }
    bool at_end() const { return (m_position >> 3) >= m_data.size(); }
    void align()
    {
        m_position = (m_position + 7) & ~static_cast<std::size_t>(7);
    }

private:
    std::vector<std::uint8_t> const& m_data;
    std::size_t m_position = 0;
};

// One run: make-up codes then a terminating one. Nothing on a code that
// is not one, or past the data.
std::optional<int> read_run(Bits& bits, bool black)
{
    Table const& table = black ? black_table() : white_table();
    int run = 0;
    for (int codes = 0; codes < 64; ++codes) {
        if (bits.at_end())
            return std::nullopt;
        Entry const entry = table[bits.peek(lookup_bits)];
        if (entry.length == 0)
            return std::nullopt;
        bits.skip(entry.length);
        run += entry.run;
        if (entry.run < 64)
            return run;
    }
    return std::nullopt;
}

// The two-dimensional modes (T.4 Table 4), by their codes.
enum class Mode { Pass, Horizontal, Vertical, Extension, EndOfLine, Bad };

struct ModeCode {
    Mode mode;
    int offset; // a Vertical mode's a1 - b1
};

ModeCode read_mode(Bits& bits)
{
    std::uint32_t const next = bits.peek(12);
    if (next & 0x800) { // 1
        bits.skip(1);
        return { Mode::Vertical, 0 };
    }
    if ((next >> 9) == 0b011) {
        bits.skip(3);
        return { Mode::Vertical, 1 };
    }
    if ((next >> 9) == 0b010) {
        bits.skip(3);
        return { Mode::Vertical, -1 };
    }
    if ((next >> 9) == 0b001) {
        bits.skip(3);
        return { Mode::Horizontal, 0 };
    }
    if ((next >> 8) == 0b0001) {
        bits.skip(4);
        return { Mode::Pass, 0 };
    }
    if ((next >> 6) == 0b000011) {
        bits.skip(6);
        return { Mode::Vertical, 2 };
    }
    if ((next >> 6) == 0b000010) {
        bits.skip(6);
        return { Mode::Vertical, -2 };
    }
    if ((next >> 5) == 0b0000011) {
        bits.skip(7);
        return { Mode::Vertical, 3 };
    }
    if ((next >> 5) == 0b0000010) {
        bits.skip(7);
        return { Mode::Vertical, -3 };
    }
    if ((next >> 5) == 0b0000001)
        return { Mode::Extension, 0 };
    if (next == 1)
        return { Mode::EndOfLine, 0 };
    return { Mode::Bad, 0 };
}

// A line's changes: where the color turns, starting white; each one
// further right than the one before, all within [0, columns].
using Changes = std::vector<int>;

// Appends a change at x, or takes back the last one when x meets it (a
// zero-length run).
void change_at(Changes& line, int x)
{
    if (!line.empty() && line.back() >= x) {
        if (line.back() == x)
            line.pop_back();
        else
            line.push_back(x); // out of order: the caller has failed the line already
        return;
    }
    line.push_back(x);
}

bool read_one_dimensional(Bits& bits, int columns, Changes& line)
{
    line.clear();
    int x = 0;
    bool black = false;
    while (x < columns) {
        std::optional<int> const run = read_run(bits, black);
        if (!run)
            return false;
        x = std::min(columns, x + *run);
        line.push_back(x);
        black = !black;
    }
    // A last run that ends at the line's end is no change.
    if (!line.empty() && line.back() >= columns)
        line.pop_back();
    return true;
}

bool read_two_dimensional(Bits& bits, int columns, Changes const& reference, Changes& line)
{
    line.clear();
    int a0 = -1;
    bool black = false;
    std::size_t i = 0; // into the reference, at or before b1
    while (a0 < columns) {
        // b1: the reference's first change right of a0 to the other color
        // (an even index turns to black), b2 the one after it. A vertical
        // code to the left puts a0 before the last b1, so the search may
        // step back first.
        while (i > 0 && reference[i - 1] > a0)
            --i;
        while (i < reference.size() && reference[i] <= a0)
            ++i;
        if (i < reference.size() && ((i % 2 == 0) == black))
            ++i;
        int const b1 = i < reference.size() ? reference[i] : columns;
        int const b2 = i + 1 < reference.size() ? reference[i + 1] : columns;
        ModeCode const mode = read_mode(bits);
        switch (mode.mode) {
        case Mode::Pass:
            // The color carries on under b1 and b2.
            a0 = b2;
            break;
        case Mode::Horizontal: {
            int const start = std::max(a0, 0);
            std::optional<int> const first = read_run(bits, black);
            std::optional<int> const second = first ? read_run(bits, !black) : std::nullopt;
            if (!second)
                return false;
            int const a1 = std::min(columns, start + *first);
            int const a2 = std::min(columns, a1 + *second);
            change_at(line, a1);
            change_at(line, a2);
            a0 = a2;
            break;
        }
        case Mode::Vertical: {
            int const a1 = b1 + mode.offset;
            if (a1 < 0 || a1 > columns || a1 < a0)
                return false;
            change_at(line, a1);
            a0 = a1;
            black = !black;
            break;
        }
        default:
            return false;
        }
        if (bits.at_end() && a0 < columns)
            return false;
    }
    if (!line.empty() && line.back() >= columns)
        line.pop_back();
    for (std::size_t k = 1; k < line.size(); ++k) {
        if (line[k] <= line[k - 1])
            return false;
    }
    return true;
}

// Fill bits and an end-of-line code where they stand: true when one was.
bool skip_end_of_line(Bits& bits)
{
    // Eight zeros or more are fill or an end-of-line, never a code's start.
    if (bits.peek(8) != 0)
        return false;
    for (int zeros = 0; zeros < 4096 && !bits.at_end(); ++zeros) {
        if (bits.peek(1)) {
            bits.skip(1);
            return zeros >= 11;
        }
        bits.skip(1);
    }
    return false;
}

void paint_row(Changes const& line, int columns, bool black_is_1, std::uint8_t* row)
{
    // White everywhere, then each black run.
    std::size_t const stride = (static_cast<std::size_t>(columns) + 7) / 8;
    std::fill(row, row + stride, black_is_1 ? 0x00 : 0xFF);
    for (std::size_t k = 0; k < line.size(); k += 2) {
        int const from = line[k];
        int const to = k + 1 < line.size() ? line[k + 1] : columns;
        for (int x = from; x < to; ++x) {
            std::uint8_t const mask = static_cast<std::uint8_t>(0x80 >> (x & 7));
            if (black_is_1)
                row[x >> 3] |= mask;
            else
                row[x >> 3] &= static_cast<std::uint8_t>(~mask);
        }
    }
}

int param(Dict const& params, char const* key, int fallback)
{
    Object const& value = params.get(key);
    return value.is_number() ? static_cast<int>(value.as_int(fallback)) : fallback;
}

bool flag(Dict const& params, char const* key, bool fallback)
{
    Object const& value = params.get(key);
    return value.is_bool() ? value.as_bool() : fallback;
}

} // namespace

std::optional<std::vector<std::uint8_t>> decode_ccitt(std::vector<std::uint8_t> const& data, Dict const& params, int width,
    int height)
{
    int const k = param(params, "K", 0);
    int const columns = param(params, "Columns", width > 0 ? width : 1728);
    int rows = param(params, "Rows", 0);
    if (rows <= 0)
        rows = height;
    bool const black_is_1 = flag(params, "BlackIs1", false);
    bool const byte_align = flag(params, "EncodedByteAlign", false);
    if (columns <= 0 || columns > (1 << 20) || rows <= 0 || width <= 0 || height <= 0)
        return std::nullopt;

    // Decoded at the coded width, given out at the image's.
    std::size_t const coded_stride = (static_cast<std::size_t>(columns) + 7) / 8;
    std::size_t const stride = (static_cast<std::size_t>(width) + 7) / 8;
    std::vector<std::uint8_t> out(stride * static_cast<std::size_t>(height), black_is_1 ? 0x00 : 0xFF);
    std::vector<std::uint8_t> coded_row(coded_stride);

    Bits bits(data);
    Changes reference; // all white above the first line
    Changes line;
    int const last_row = std::min(rows, height);
    for (int y = 0; y < last_row; ++y) {
        if (bits.at_end())
            break;
        bool two_dimensional = k < 0;
        if (k >= 0) {
            // Fill, an end-of-line, and with K > 0 the tag bit after it. A
            // byte-aligned line starts at the next byte: the bits left in
            // this one are padding, whatever they look like.
            if (byte_align)
                bits.align();
            skip_end_of_line(bits);
            if (bits.at_end())
                break;
            if (k > 0) {
                two_dimensional = bits.peek(1) == 0;
                bits.skip(1);
            }
            // The end of the facsimile block: end-of-lines in a row.
            if (bits.peek(12) == 1)
                break;
        } else {
            if (byte_align)
                bits.align();
            // EOFB: two end-of-lines.
            if (bits.peek(24) == 0x001001)
                break;
        }
        bool const read = two_dimensional ? read_two_dimensional(bits, columns, reference, line)
                                          : read_one_dimensional(bits, columns, line);
        if (!read)
            break;
        paint_row(line, columns, black_is_1, coded_row.data());
        std::uint8_t* const row = out.data() + static_cast<std::size_t>(y) * stride;
        std::copy_n(coded_row.data(), std::min(stride, coded_stride), row);
        if (width > columns) {
            // Past the coded width: white, the last byte's spare bits too.
            for (int x = columns; x < width; ++x) {
                std::uint8_t const mask = static_cast<std::uint8_t>(0x80 >> (x & 7));
                if (black_is_1)
                    row[x >> 3] &= static_cast<std::uint8_t>(~mask);
                else
                    row[x >> 3] |= mask;
            }
        }
        std::swap(reference, line);
    }
    return out;
}

}
