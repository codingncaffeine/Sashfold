#include "core/Brotli.h"

#include "core/BrotliDictionary.h"

#include <algorithm>
#include <array>

namespace sashfold {

namespace {

// --- The tables the format fixes ---------------------------------------------

// The context ID lookups of the UTF8 and Signed literal context modes (§7.1).
constexpr std::array<std::uint8_t, 256> lut0 = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 4, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    8, 12, 16, 12, 12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12,
    44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12,
    12, 48, 52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48,
    52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12, 12,
    12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56,
    60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28, 12, 0,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
    2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
    2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
    2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
};

constexpr std::array<std::uint8_t, 256> lut1 = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};

constexpr std::array<std::uint8_t, 256> lut2 = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7,
};

// How many words of each length the static dictionary holds, as a power of
// two (§8), and so where the words of each length begin in it.
constexpr std::array<std::uint8_t, 25> dictionary_bits
    = { 0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5 };

constexpr std::array<std::uint32_t, 25> dictionary_offsets = [] {
    std::array<std::uint32_t, 25> offsets {};
    for (std::uint32_t length = 0; length < 24; ++length)
        offsets[length + 1] = offsets[length] + (length >= 4 ? length << dictionary_bits[length] : 0);
    return offsets;
}();

// A value range: the base the extra bits are added to, and how many there are.
struct Range {
    std::uint32_t base;
    std::uint8_t bits;
};

constexpr std::array<Range, 26> block_count_ranges = { {
    { 1, 2 }, { 5, 2 }, { 9, 2 }, { 13, 2 }, { 17, 3 }, { 25, 3 }, { 33, 3 }, { 41, 3 }, { 49, 4 },
    { 65, 4 }, { 81, 4 }, { 97, 4 }, { 113, 5 }, { 145, 5 }, { 177, 5 }, { 209, 5 }, { 241, 6 },
    { 305, 6 }, { 369, 7 }, { 497, 8 }, { 753, 9 }, { 1265, 10 }, { 2289, 11 }, { 4337, 12 },
    { 8433, 13 }, { 16625, 24 },
} };

constexpr std::array<Range, 24> insert_ranges = { {
    { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 }, { 6, 1 }, { 8, 1 }, { 10, 2 }, { 14, 2 },
    { 18, 3 }, { 26, 3 }, { 34, 4 }, { 50, 4 }, { 66, 5 }, { 98, 5 }, { 130, 6 }, { 194, 7 },
    { 322, 8 }, { 578, 9 }, { 1090, 10 }, { 2114, 12 }, { 6210, 14 }, { 22594, 24 },
} };

constexpr std::array<Range, 24> copy_ranges = { {
    { 2, 0 }, { 3, 0 }, { 4, 0 }, { 5, 0 }, { 6, 0 }, { 7, 0 }, { 8, 0 }, { 9, 0 }, { 10, 1 }, { 12, 1 },
    { 14, 2 }, { 18, 2 }, { 22, 3 }, { 30, 3 }, { 38, 4 }, { 54, 4 }, { 70, 5 }, { 102, 5 },
    { 134, 6 }, { 198, 7 }, { 326, 8 }, { 582, 9 }, { 1094, 10 }, { 2118, 24 },
} };

// The word transforms (Appendix B): a prefix, one elementary transform — 0
// the word itself, 1 its first letter fermented to upper case, 2 all of
// them, 3..11 the word without its first 1..9 bytes, 12..20 without its
// last 1..9 — and a suffix.
struct Transform {
    char const* prefix;
    std::uint8_t kind;
    char const* suffix;
};

constexpr std::array<Transform, 121> transforms = { {
    { "", 0, "" }, { "", 0, " " }, { " ", 0, " " }, { "", 3, "" }, { "", 1, " " }, { "", 0, " the " },
    { " ", 0, "" }, { "s ", 0, " " }, { "", 0, " of " }, { "", 1, "" }, { "", 0, " and " }, { "", 4, "" },
    { "", 12, "" }, { ", ", 0, " " }, { "", 0, ", " }, { " ", 1, " " }, { "", 0, " in " }, { "", 0, " to " },
    { "e ", 0, " " }, { "", 0, "\"" }, { "", 0, "." }, { "", 0, "\">" }, { "", 0, "\n" }, { "", 14, "" },
    { "", 0, "]" }, { "", 0, " for " }, { "", 5, "" }, { "", 13, "" }, { "", 0, " a " }, { "", 0, " that " },
    { " ", 1, "" }, { "", 0, ". " }, { ".", 0, "" }, { " ", 0, ", " }, { "", 6, "" }, { "", 0, " with " },
    { "", 0, "'" }, { "", 0, " from " }, { "", 0, " by " }, { "", 7, "" }, { "", 8, "" }, { " the ", 0, "" },
    { "", 15, "" }, { "", 0, ". The " }, { "", 2, "" }, { "", 0, " on " }, { "", 0, " as " },
    { "", 0, " is " }, { "", 18, "" }, { "", 12, "ing " }, { "", 0, "\n\t" }, { "", 0, ":" },
    { " ", 0, ". " }, { "", 0, "ed " }, { "", 11, "" }, { "", 9, "" }, { "", 17, "" }, { "", 0, "(" },
    { "", 1, ", " }, { "", 19, "" }, { "", 0, " at " }, { "", 0, "ly " }, { " the ", 0, " of " },
    { "", 16, "" }, { "", 20, "" }, { " ", 1, ", " }, { "", 1, "\"" }, { ".", 0, "(" }, { "", 2, " " },
    { "", 1, "\">" }, { "", 0, "=\"" }, { " ", 0, "." }, { ".com/", 0, "" }, { " the ", 0, " of the " },
    { "", 1, "'" }, { "", 0, ". This " }, { "", 0, "," }, { ".", 0, " " }, { "", 1, "(" }, { "", 1, "." },
    { "", 0, " not " }, { " ", 0, "=\"" }, { "", 0, "er " }, { " ", 2, " " }, { "", 0, "al " },
    { " ", 2, "" }, { "", 0, "='" }, { "", 2, "\"" }, { "", 1, ". " }, { " ", 0, "(" }, { "", 0, "ful " },
    { " ", 1, ". " }, { "", 0, "ive " }, { "", 0, "less " }, { "", 2, "'" }, { "", 0, "est " },
    { " ", 1, "." }, { "", 2, "\">" }, { " ", 0, "='" }, { "", 1, "," }, { "", 0, "ize " }, { "", 2, "." },
    { "\xc2\xa0", 0, "" }, { " ", 0, "," }, { "", 1, "=\"" }, { "", 2, "=\"" }, { "", 0, "ous " },
    { "", 2, ", " }, { "", 1, "='" }, { " ", 1, "," }, { " ", 2, "=\"" }, { " ", 2, ", " }, { "", 2, "," },
    { "", 2, "(" }, { "", 2, ". " }, { " ", 2, "." }, { "", 2, "='" }, { " ", 2, ". " }, { " ", 1, "=\"" },
    { " ", 2, "='" }, { " ", 1, "='" },
} };

// --- Bits ----------------------------------------------------------------------

// The stream's bits, least significant first within each byte (§1.5.1).
class BitReader {
public:
    BitReader(std::uint8_t const* data, std::size_t size)
        : m_data(data)
        , m_size(size)
    {
    }

    // The next `count` bits (at most 32) as a number; false when the stream
    // ends before them.
    [[nodiscard]] bool read(unsigned count, std::uint32_t& value)
    {
        fill();
        if (m_count < count)
            return false;
        value = static_cast<std::uint32_t>(m_buffer & ((std::uint64_t { 1 } << count) - 1));
        m_buffer >>= count;
        m_count -= count;
        return true;
    }

    // The next 15 bits without taking them, zeros past the end: what a
    // prefix code's table is looked up by.
    std::uint32_t peek()
    {
        fill();
        return static_cast<std::uint32_t>(m_buffer & 0x7FFF);
    }

    [[nodiscard]] bool skip(unsigned count)
    {
        std::uint32_t ignored = 0;
        return read(count, ignored);
    }

    // The bits up to the next byte boundary, which must all be zero.
    [[nodiscard]] bool align()
    {
        unsigned const into = static_cast<unsigned>((m_next * 8 - m_count) % 8);
        std::uint32_t fill_bits = 0;
        return read(into == 0 ? 0 : 8 - into, fill_bits) && fill_bits == 0;
    }

    // Whole bytes, from a byte boundary: appended to `out`, or passed over
    // when it is null.
    [[nodiscard]] bool bytes(std::size_t count, std::vector<std::uint8_t>* out)
    {
        while (count > 0 && m_count >= 8) {
            if (out)
                out->push_back(static_cast<std::uint8_t>(m_buffer & 0xFF));
            m_buffer >>= 8;
            m_count -= 8;
            --count;
        }
        if (count > m_size - m_next)
            return false;
        if (out)
            out->insert(out->end(), m_data + m_next, m_data + m_next + count);
        m_next += count;
        return true;
    }

private:
    void fill()
    {
        while (m_count <= 56 && m_next < m_size) {
            m_buffer |= std::uint64_t { m_data[m_next++] } << m_count;
            m_count += 8;
        }
    }

    std::uint8_t const* m_data;
    std::size_t m_size;
    std::size_t m_next = 0;
    std::uint64_t m_buffer = 0;
    unsigned m_count = 0;
};

// --- Prefix codes ---------------------------------------------------------------

// A canonical prefix code (§3.2) as a two-level table: the first eight bits
// of the stream index the root, and a code longer than that continues in a
// table of its own for its first eight bits.
class PrefixCode {
public:
    // From each symbol's code length (0 for a symbol not used). A code must
    // be complete, but one with a single symbol, which reads no bits at all.
    [[nodiscard]] bool build(std::vector<std::uint8_t> const& lengths)
    {
        constexpr unsigned root = 8;
        m_table.assign(std::size_t { 1 } << root, Entry {});
        std::array<std::uint32_t, 16> count {};
        std::size_t used = 0;
        std::uint16_t only = 0;
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] == 0)
                continue;
            if (lengths[symbol] > 15)
                return false;
            ++count[lengths[symbol]];
            ++used;
            only = static_cast<std::uint16_t>(symbol);
        }
        if (used == 0)
            return false;
        if (used == 1) {
            for (Entry& entry : m_table)
                entry = Entry { 0, only };
            return true;
        }
        std::uint32_t kraft = 0;
        for (unsigned bits = 1; bits <= 15; ++bits)
            kraft += count[bits] << (15 - bits);
        if (kraft != (1u << 15))
            return false;

        std::array<std::uint32_t, 16> next {};
        std::uint32_t code = 0;
        for (unsigned bits = 1; bits <= 15; ++bits) {
            code = (code + count[bits - 1]) << 1;
            next[bits] = code;
        }
        // Each symbol's code, bit-reversed: the stream gives a code's most
        // significant bit first, and the table is indexed by bits as read.
        struct Code {
            std::uint16_t symbol;
            std::uint8_t length;
            std::uint32_t reversed;
        };
        std::vector<Code> codes;
        codes.reserve(used);
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            unsigned const length = lengths[symbol];
            if (length == 0)
                continue;
            std::uint32_t const value = next[length]++;
            std::uint32_t reversed = 0;
            for (unsigned i = 0; i < length; ++i)
                reversed |= ((value >> i) & 1u) << (length - 1 - i);
            codes.push_back(Code { static_cast<std::uint16_t>(symbol), static_cast<std::uint8_t>(length), reversed });
        }
        // The longest code under each root entry sizes the table below it.
        std::array<std::uint8_t, 256> longest {};
        for (Code const& entry : codes) {
            if (entry.length > root) {
                std::uint8_t& deepest = longest[entry.reversed & 0xFF];
                deepest = std::max(deepest, entry.length);
            }
        }
        std::array<std::uint32_t, 256> offset {};
        for (unsigned prefix = 0; prefix < 256; ++prefix) {
            if (longest[prefix] == 0)
                continue;
            unsigned const below = longest[prefix] - root;
            offset[prefix] = static_cast<std::uint32_t>(m_table.size());
            if (offset[prefix] > 0xFFFF)
                return false;
            m_table[prefix] = Entry { static_cast<std::uint8_t>(root + below), static_cast<std::uint16_t>(offset[prefix]) };
            m_table.resize(m_table.size() + (std::size_t { 1 } << below));
        }
        for (Code const& entry : codes) {
            if (entry.length <= root) {
                for (std::uint32_t fill = 0; fill < (1u << (root - entry.length)); ++fill)
                    m_table[entry.reversed | (fill << entry.length)] = Entry { entry.length, entry.symbol };
                continue;
            }
            unsigned const prefix = entry.reversed & 0xFF;
            unsigned const below = longest[prefix] - root;
            unsigned const rest = entry.length - root;
            std::uint32_t const tail = entry.reversed >> root;
            for (std::uint32_t fill = 0; fill < (1u << (below - rest)); ++fill)
                m_table[offset[prefix] + (tail | (fill << rest))] = Entry { static_cast<std::uint8_t>(rest), entry.symbol };
        }
        return true;
    }

    [[nodiscard]] bool decode(BitReader& in, std::uint32_t& symbol) const
    {
        std::uint32_t const bits = in.peek();
        Entry entry = m_table[bits & 0xFF];
        unsigned taken = entry.bits;
        if (entry.bits > 8) {
            unsigned const below = entry.bits - 8u;
            entry = m_table[entry.value + ((bits >> 8) & ((1u << below) - 1))];
            taken = 8u + entry.bits;
        }
        symbol = entry.value;
        return in.skip(taken);
    }

private:
    // A leaf: the symbol and how many bits its code takes here. In the root,
    // a `bits` over eight instead names a table below: `value` is where it
    // begins and `bits` - 8 how many further bits index it.
    struct Entry {
        std::uint8_t bits = 0;
        std::uint16_t value = 0;
    };
    std::vector<Entry> m_table;
};

// The fewest bits that can spell every symbol of an alphabet this size.
unsigned bits_for(unsigned size)
{
    unsigned bits = 0;
    while ((1u << bits) < size)
        ++bits;
    return bits;
}

// A prefix code as the stream describes it (§3.4, §3.5).
[[nodiscard]] bool read_prefix_code(BitReader& in, unsigned alphabet, PrefixCode& code)
{
    std::uint32_t kind = 0;
    if (!in.read(2, kind))
        return false;
    std::vector<std::uint8_t> lengths(alphabet, 0);
    if (kind == 1) {
        // Simple: up to four symbols spelled out, their lengths implied.
        std::uint32_t count = 0;
        if (!in.read(2, count))
            return false;
        ++count;
        unsigned const width = bits_for(alphabet);
        std::array<std::uint32_t, 4> symbols {};
        for (unsigned i = 0; i < count; ++i) {
            if (!in.read(width, symbols[i]) || symbols[i] >= alphabet)
                return false;
            for (unsigned j = 0; j < i; ++j) {
                if (symbols[j] == symbols[i])
                    return false;
            }
        }
        std::array<std::uint8_t, 4> implied { 1, 1, 0, 0 };
        if (count == 3)
            implied = { 1, 2, 2, 0 };
        if (count == 4) {
            std::uint32_t select = 0;
            if (!in.read(1, select))
                return false;
            implied = select == 0 ? std::array<std::uint8_t, 4> { 2, 2, 2, 2 } : std::array<std::uint8_t, 4> { 1, 2, 3, 3 };
        }
        for (unsigned i = 0; i < count; ++i)
            lengths[symbols[i]] = implied[i];
        return code.build(lengths);
    }

    // Complex: the code lengths, themselves prefix coded. First the lengths
    // of that code's symbols, in the order the format fixes, each in a small
    // code of its own; `kind` counts the leading ones taken as zero.
    static constexpr std::array<std::uint8_t, 18> order = { 1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    std::vector<std::uint8_t> length_lengths(18, 0);
    int space = 32;
    unsigned nonzero = 0;
    for (unsigned i = kind; i < 18; ++i) {
        std::uint32_t const bits = in.peek();
        unsigned length = 0;
        unsigned taken = 2;
        switch (bits & 3) {
        case 0:
            length = 0;
            break;
        case 1:
            length = 4;
            break;
        case 2:
            length = 3;
            break;
        default:
            if ((bits & 7) == 3) {
                length = 2;
                taken = 3;
            } else if ((bits & 15) == 7) {
                length = 1;
                taken = 4;
            } else {
                length = 5;
                taken = 4;
            }
        }
        if (!in.skip(taken))
            return false;
        length_lengths[order[i]] = static_cast<std::uint8_t>(length);
        if (length != 0) {
            space -= 32 >> length;
            ++nonzero;
            if (space <= 0)
                break;
        }
    }
    if (nonzero != 1 && space != 0)
        return false;
    PrefixCode length_code;
    if (!length_code.build(length_lengths))
        return false;

    // Then the lengths: 0..15 as they are, 16 repeating the last nonzero
    // length and 17 a zero, where a repeat following a repeat of the same
    // kind extends the count rather than starting another.
    unsigned symbol = 0;
    unsigned previous = 8;
    unsigned repeat = 0;
    unsigned repeat_length = 0;
    int left = 32768;
    while (symbol < alphabet && left > 0) {
        std::uint32_t value = 0;
        if (!length_code.decode(in, value))
            return false;
        if (value < 16) {
            lengths[symbol++] = static_cast<std::uint8_t>(value);
            repeat = 0;
            if (value != 0) {
                previous = value;
                left -= 32768 >> value;
            }
            continue;
        }
        unsigned const extra = value == 16 ? 2 : 3;
        unsigned const length = value == 16 ? previous : 0;
        if (repeat_length != length) {
            repeat = 0;
            repeat_length = length;
        }
        unsigned const before = repeat;
        if (repeat > 0) {
            repeat -= 2;
            repeat <<= extra;
        }
        std::uint32_t more = 0;
        if (!in.read(extra, more))
            return false;
        repeat += more + 3;
        unsigned const added = repeat - before;
        if (added > alphabet - symbol)
            return false;
        for (unsigned i = 0; i < added; ++i)
            lengths[symbol++] = static_cast<std::uint8_t>(length);
        if (length != 0)
            left -= static_cast<int>(added) * (32768 >> length);
    }
    if (left != 0)
        return false;
    return code.build(lengths);
}

// A count of 1..256 in the variable-length code of §9.2 (block types, trees).
[[nodiscard]] bool read_count(BitReader& in, unsigned& value)
{
    std::uint32_t bit = 0;
    if (!in.read(1, bit))
        return false;
    if (bit == 0) {
        value = 1;
        return true;
    }
    std::uint32_t width = 0;
    if (!in.read(3, width))
        return false;
    if (width == 0) {
        value = 2;
        return true;
    }
    std::uint32_t extra = 0;
    if (!in.read(width, extra))
        return false;
    value = (1u << width) + extra + 1;
    return true;
}

// --- Blocks and context maps --------------------------------------------------

// One block category's state (§6): how many block types there are, the
// codes a switch is read with, the current and the previous type, and how
// many symbols the current block has left.
struct Blocks {
    unsigned types = 1;
    PrefixCode type_code;
    PrefixCode count_code;
    unsigned current = 0;
    unsigned previous = 1;
    std::uint32_t left = 1u << 24;
};

[[nodiscard]] bool read_block_count(BitReader& in, PrefixCode const& code, std::uint32_t& count)
{
    std::uint32_t symbol = 0;
    std::uint32_t extra = 0;
    if (!code.decode(in, symbol) || !in.read(block_count_ranges[symbol].bits, extra))
        return false;
    count = block_count_ranges[symbol].base + extra;
    return true;
}

[[nodiscard]] bool read_blocks(BitReader& in, Blocks& blocks)
{
    if (!read_count(in, blocks.types))
        return false;
    blocks.current = 0;
    blocks.previous = 1;
    blocks.left = 1u << 24;
    if (blocks.types < 2)
        return true;
    return read_prefix_code(in, blocks.types + 2, blocks.type_code)
        && read_prefix_code(in, 26, blocks.count_code)
        && read_block_count(in, blocks.count_code, blocks.left);
}

// A block switch, when the current block has run out: symbol 0 is the
// previous type, 1 the next after the current, and n the type n - 2.
[[nodiscard]] bool switch_block(BitReader& in, Blocks& blocks)
{
    std::uint32_t symbol = 0;
    if (!blocks.type_code.decode(in, symbol))
        return false;
    unsigned type = symbol == 0 ? blocks.previous : symbol == 1 ? blocks.current + 1 : symbol - 2;
    if (type >= blocks.types)
        type -= blocks.types;
    blocks.previous = blocks.current;
    blocks.current = type;
    return read_block_count(in, blocks.count_code, blocks.left);
}

// A context map (§7.3): run lengths of zeros and tree indexes under a prefix
// code, then perhaps an inverse move-to-front.
[[nodiscard]] bool read_context_map(BitReader& in, std::size_t size, unsigned trees, std::vector<std::uint8_t>& map)
{
    map.assign(size, 0);
    if (trees < 2)
        return true;
    std::uint32_t flag = 0;
    std::uint32_t runs = 0;
    if (!in.read(1, flag) || (flag && !in.read(4, runs)))
        return false;
    if (flag)
        ++runs;
    PrefixCode code;
    if (!read_prefix_code(in, trees + runs, code))
        return false;
    std::size_t i = 0;
    while (i < size) {
        std::uint32_t symbol = 0;
        if (!code.decode(in, symbol))
            return false;
        if (symbol == 0) {
            ++i;
            continue;
        }
        if (symbol <= runs) {
            std::uint32_t extra = 0;
            if (!in.read(symbol, extra))
                return false;
            std::size_t const zeros = (std::size_t { 1 } << symbol) + extra;
            if (zeros > size - i)
                return false;
            i += zeros;
            continue;
        }
        map[i++] = static_cast<std::uint8_t>(symbol - runs);
    }
    std::uint32_t inverse = 0;
    if (!in.read(1, inverse))
        return false;
    if (inverse) {
        std::array<std::uint8_t, 256> front {};
        for (unsigned k = 0; k < 256; ++k)
            front[k] = static_cast<std::uint8_t>(k);
        for (std::uint8_t& value : map) {
            unsigned const index = value;
            std::uint8_t const moved = front[index];
            value = moved;
            for (unsigned k = index; k > 0; --k)
                front[k] = front[k - 1];
            front[0] = moved;
        }
    }
    return std::all_of(map.begin(), map.end(), [&](std::uint8_t value) { return value < trees; });
}

// --- Dictionary words ---------------------------------------------------------

// What upper-casing a letter is in the format (§8): an ASCII letter flips
// its case bit, a two-byte UTF-8 sequence its second byte's, a three-byte
// one its third byte's; the answer is how many bytes the character took.
std::size_t ferment(std::uint8_t* word, std::size_t length, std::size_t at)
{
    if (word[at] < 192) {
        if (word[at] >= 97 && word[at] <= 122)
            word[at] = static_cast<std::uint8_t>(word[at] ^ 32);
        return 1;
    }
    if (word[at] < 224) {
        if (at + 1 < length)
            word[at + 1] = static_cast<std::uint8_t>(word[at + 1] ^ 32);
        return 2;
    }
    if (at + 2 < length)
        word[at + 2] = static_cast<std::uint8_t>(word[at + 2] ^ 5);
    return 3;
}

// A dictionary word under a transform: at most 24 bytes of word and 13 of
// prefix and suffix.
std::size_t transform_word(std::uint8_t const* base, std::size_t length, Transform const& transform,
    std::array<std::uint8_t, 38>& out)
{
    std::size_t n = 0;
    for (char const* p = transform.prefix; *p; ++p)
        out[n++] = static_cast<std::uint8_t>(*p);
    std::size_t skip = 0;
    std::size_t keep = length;
    if (transform.kind >= 3 && transform.kind <= 11) {
        skip = transform.kind - 2u;
        keep = skip > length ? 0 : length - skip;
    } else if (transform.kind >= 12) {
        std::size_t const cut = transform.kind - 11u;
        keep = cut > length ? 0 : length - cut;
    }
    std::size_t const start = n;
    for (std::size_t k = 0; k < keep; ++k)
        out[n++] = base[skip + k];
    if (transform.kind == 1 && keep > 0)
        ferment(out.data() + start, keep, 0);
    if (transform.kind == 2) {
        for (std::size_t at = 0; at < keep;)
            at += ferment(out.data() + start, keep, at);
    }
    for (char const* p = transform.suffix; *p; ++p)
        out[n++] = static_cast<std::uint8_t>(*p);
    return n;
}

// --- The stream -----------------------------------------------------------------

class Decoder {
public:
    Decoder(std::uint8_t const* data, std::size_t size, std::size_t cap)
        : m_in(data, size)
        , m_cap(cap)
    {
    }

    std::optional<std::vector<std::uint8_t>> run()
    {
        // The window (§9.1): a variable-length code for WBITS 10..24.
        std::uint32_t bit = 0;
        if (!m_in.read(1, bit))
            return std::nullopt;
        unsigned window_bits = 16;
        if (bit) {
            std::uint32_t high = 0;
            if (!m_in.read(3, high))
                return std::nullopt;
            if (high != 0) {
                window_bits = 17 + high;
            } else {
                std::uint32_t low = 0;
                if (!m_in.read(3, low) || low == 1)
                    return std::nullopt;
                window_bits = low == 0 ? 17 : 8 + low;
            }
        }
        m_window = (std::size_t { 1 } << window_bits) - 16;
        bool last = false;
        while (!last) {
            if (!meta_block(last))
                return std::nullopt;
        }
        if (!m_in.align())
            return std::nullopt;
        return std::move(m_out);
    }

private:
    [[nodiscard]] bool meta_block(bool& last)
    {
        std::uint32_t flag = 0;
        if (!m_in.read(1, flag))
            return false;
        last = flag != 0;
        if (last) {
            std::uint32_t empty = 0;
            if (!m_in.read(1, empty))
                return false;
            if (empty)
                return true;
        }
        std::uint32_t nibbles = 0;
        if (!m_in.read(2, nibbles))
            return false;
        if (nibbles == 3) {
            // A metadata block: skipped, never part of the output.
            std::uint32_t reserved = 0;
            std::uint32_t width = 0;
            if (!m_in.read(1, reserved) || reserved != 0 || !m_in.read(2, width))
                return false;
            std::uint32_t skip = 0;
            if (width > 0) {
                if (!m_in.read(width * 8, skip))
                    return false;
                if (width > 1 && (skip >> ((width - 1) * 8)) == 0)
                    return false;
                ++skip;
            }
            return m_in.align() && m_in.bytes(skip, nullptr);
        }
        nibbles += 4;
        std::uint32_t length_less_one = 0;
        if (!m_in.read(nibbles * 4, length_less_one))
            return false;
        if (nibbles > 4 && (length_less_one >> ((nibbles - 1) * 4)) == 0)
            return false;
        std::size_t const length = std::size_t { length_less_one } + 1;
        if (length > m_cap - m_out.size())
            return false;
        if (!last) {
            std::uint32_t uncompressed = 0;
            if (!m_in.read(1, uncompressed))
                return false;
            if (uncompressed)
                return m_in.align() && m_in.bytes(length, &m_out);
        }
        return compressed(length);
    }

    [[nodiscard]] bool compressed(std::size_t length)
    {
        Blocks literals;
        Blocks commands;
        Blocks distances;
        if (!read_blocks(m_in, literals) || !read_blocks(m_in, commands) || !read_blocks(m_in, distances))
            return false;
        std::uint32_t postfix = 0;
        std::uint32_t direct = 0;
        if (!m_in.read(2, postfix) || !m_in.read(4, direct))
            return false;
        direct <<= postfix;
        std::vector<std::uint8_t> modes(literals.types);
        for (std::uint8_t& mode : modes) {
            std::uint32_t value = 0;
            if (!m_in.read(2, value))
                return false;
            mode = static_cast<std::uint8_t>(value);
        }
        unsigned literal_trees = 0;
        std::vector<std::uint8_t> literal_map;
        unsigned distance_trees = 0;
        std::vector<std::uint8_t> distance_map;
        if (!read_count(m_in, literal_trees) || !read_context_map(m_in, 64 * std::size_t { literals.types }, literal_trees, literal_map)
            || !read_count(m_in, distance_trees)
            || !read_context_map(m_in, 4 * std::size_t { distances.types }, distance_trees, distance_map))
            return false;
        std::vector<PrefixCode> literal_codes(literal_trees);
        for (PrefixCode& code : literal_codes) {
            if (!read_prefix_code(m_in, 256, code))
                return false;
        }
        std::vector<PrefixCode> command_codes(commands.types);
        for (PrefixCode& code : command_codes) {
            if (!read_prefix_code(m_in, 704, code))
                return false;
        }
        std::vector<PrefixCode> distance_codes(distance_trees);
        for (PrefixCode& code : distance_codes) {
            if (!read_prefix_code(m_in, 16 + direct + (48u << postfix), code))
                return false;
        }

        std::size_t const end = m_out.size() + length;
        while (m_out.size() < end) {
            if (commands.types >= 2 && commands.left == 0 && !switch_block(m_in, commands))
                return false;
            --commands.left;
            std::uint32_t command = 0;
            if (!command_codes[commands.current].decode(m_in, command))
                return false;
            // The insert-and-copy cell (§5): which insert and copy length
            // codes the command's low six bits count from.
            static constexpr std::array<std::uint8_t, 11> insert_base = { 0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16 };
            static constexpr std::array<std::uint8_t, 11> copy_base = { 0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16 };
            unsigned const cell = command >> 6;
            Range const insert_range = insert_ranges[insert_base[cell] + ((command >> 3) & 7)];
            Range const copy_range = copy_ranges[copy_base[cell] + (command & 7)];
            std::uint32_t insert_extra = 0;
            std::uint32_t copy_extra = 0;
            if (!m_in.read(insert_range.bits, insert_extra) || !m_in.read(copy_range.bits, copy_extra))
                return false;
            std::size_t const insert = std::size_t { insert_range.base } + insert_extra;
            std::size_t const copy = std::size_t { copy_range.base } + copy_extra;
            if (insert > end - m_out.size())
                return false;
            for (std::size_t k = 0; k < insert; ++k) {
                if (literals.types >= 2 && literals.left == 0 && !switch_block(m_in, literals))
                    return false;
                --literals.left;
                std::size_t const size = m_out.size();
                std::uint8_t const p1 = size >= 1 ? m_out[size - 1] : 0;
                std::uint8_t const p2 = size >= 2 ? m_out[size - 2] : 0;
                unsigned context = 0;
                switch (modes[literals.current]) {
                case 0:
                    context = p1 & 0x3Fu;
                    break;
                case 1:
                    context = p1 >> 2u;
                    break;
                case 2:
                    context = static_cast<unsigned>(lut0[p1] | lut1[p2]);
                    break;
                default:
                    context = static_cast<unsigned>((lut2[p1] << 3) | lut2[p2]);
                    break;
                }
                std::uint32_t byte = 0;
                if (!literal_codes[literal_map[64 * std::size_t { literals.current } + context]].decode(m_in, byte))
                    return false;
                m_out.push_back(static_cast<std::uint8_t>(byte));
            }
            // A command whose literals finish the block has its copy ignored.
            if (m_out.size() == end)
                break;

            std::size_t distance = m_distances[0];
            std::uint32_t code = 0;
            bool const explicit_distance = command >= 128;
            if (explicit_distance) {
                if (distances.types >= 2 && distances.left == 0 && !switch_block(m_in, distances))
                    return false;
                --distances.left;
                unsigned const context = copy > 4 ? 3 : static_cast<unsigned>(copy - 2);
                if (!distance_codes[distance_map[4 * std::size_t { distances.current } + context]].decode(m_in, code)
                    || !resolve_distance(code, direct, postfix, distance))
                    return false;
            }
            std::size_t const reach = std::min(m_window, m_out.size());
            if (distance > reach) {
                // Past the data: a word from the static dictionary (§8).
                if (copy < 4 || copy > 24)
                    return false;
                std::size_t const word_id = distance - reach - 1;
                unsigned const bits = dictionary_bits[copy];
                std::size_t const index = word_id & ((std::size_t { 1 } << bits) - 1);
                std::size_t const transform = word_id >> bits;
                if (transform > 120)
                    return false;
                std::array<std::uint8_t, 38> word {};
                std::size_t const word_length = transform_word(
                    brotli_dictionary + dictionary_offsets[copy] + index * copy, copy, transforms[transform], word);
                if (word_length > end - m_out.size())
                    return false;
                m_out.insert(m_out.end(), word.begin(), word.begin() + static_cast<std::ptrdiff_t>(word_length));
                continue;
            }
            if (copy > end - m_out.size())
                return false;
            if (explicit_distance && code != 0) {
                m_distances[3] = m_distances[2];
                m_distances[2] = m_distances[1];
                m_distances[1] = m_distances[0];
                m_distances[0] = distance;
            }
            // Byte by byte: a copy may overlap what it is copying.
            m_out.reserve(m_out.size() + copy);
            std::size_t const from = m_out.size() - distance;
            for (std::size_t k = 0; k < copy; ++k) {
                std::uint8_t const byte = m_out[from + k];
                m_out.push_back(byte);
            }
        }
        return true;
    }

    // A distance code and its extra bits as a backward distance (§4).
    [[nodiscard]] bool resolve_distance(std::uint32_t code, std::uint32_t direct, std::uint32_t postfix, std::size_t& distance)
    {
        if (code < 16) {
            // The last four distances, and the last two nudged either way.
            static constexpr std::array<std::uint8_t, 16> slot = { 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1 };
            static constexpr std::array<std::int8_t, 16> nudge = { 0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3 };
            long long const value = static_cast<long long>(m_distances[slot[code]]) + nudge[code];
            if (value <= 0)
                return false;
            distance = static_cast<std::size_t>(value);
            return true;
        }
        if (code < 16 + direct) {
            distance = code - 15;
            return true;
        }
        std::uint32_t const d = code - direct - 16;
        unsigned const extra_bits = 1 + (d >> (postfix + 1));
        std::uint32_t extra = 0;
        if (!m_in.read(extra_bits, extra))
            return false;
        std::size_t const high = d >> postfix;
        std::size_t const low = d & ((1u << postfix) - 1);
        std::size_t const offset = ((2 + (high & 1)) << extra_bits) - 4;
        distance = ((offset + extra) << postfix) + low + direct + 1;
        return true;
    }

    BitReader m_in;
    std::size_t m_cap;
    std::size_t m_window = 0;
    std::vector<std::uint8_t> m_out;
    // The last four distances, the last first, as the stream starts them.
    std::array<std::size_t, 4> m_distances = { 4, 11, 15, 16 };
};

}

std::optional<std::vector<std::uint8_t>> brotli_decompress(std::uint8_t const* data, std::size_t size, std::size_t max_output)
{
    if (!data && size > 0)
        return std::nullopt;
    Decoder decoder(data, size, max_output);
    return decoder.run();
}

std::vector<std::uint8_t> brotli_transform_bytes()
{
    std::vector<std::uint8_t> bytes;
    for (Transform const& transform : transforms) {
        for (char const* p = transform.prefix; *p; ++p)
            bytes.push_back(static_cast<std::uint8_t>(*p));
        bytes.push_back(0);
        bytes.push_back(transform.kind);
        for (char const* p = transform.suffix; *p; ++p)
            bytes.push_back(static_cast<std::uint8_t>(*p));
        bytes.push_back(0);
    }
    return bytes;
}

std::uint8_t const* brotli_context_lookup(int table)
{
    return table == 0 ? lut0.data() : table == 1 ? lut1.data() : lut2.data();
}

std::uint8_t const* brotli_dictionary_bytes()
{
    return brotli_dictionary;
}

std::size_t brotli_dictionary_size()
{
    return sizeof brotli_dictionary;
}

}
