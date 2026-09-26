#include "net/Hpack.h"

#include <array>
#include <utility>

namespace sashfold::net::hpack {

namespace {

// Appendix A, in its order.
constexpr std::array<StaticEntry, static_table_size> static_table { {
    { ":authority", "" },
    { ":method", "GET" },
    { ":method", "POST" },
    { ":path", "/" },
    { ":path", "/index.html" },
    { ":scheme", "http" },
    { ":scheme", "https" },
    { ":status", "200" },
    { ":status", "204" },
    { ":status", "206" },
    { ":status", "304" },
    { ":status", "400" },
    { ":status", "404" },
    { ":status", "500" },
    { "accept-charset", "" },
    { "accept-encoding", "gzip, deflate" },
    { "accept-language", "" },
    { "accept-ranges", "" },
    { "accept", "" },
    { "access-control-allow-origin", "" },
    { "age", "" },
    { "allow", "" },
    { "authorization", "" },
    { "cache-control", "" },
    { "content-disposition", "" },
    { "content-encoding", "" },
    { "content-language", "" },
    { "content-length", "" },
    { "content-location", "" },
    { "content-range", "" },
    { "content-type", "" },
    { "cookie", "" },
    { "date", "" },
    { "etag", "" },
    { "expect", "" },
    { "expires", "" },
    { "from", "" },
    { "host", "" },
    { "if-match", "" },
    { "if-modified-since", "" },
    { "if-none-match", "" },
    { "if-range", "" },
    { "if-unmodified-since", "" },
    { "last-modified", "" },
    { "link", "" },
    { "location", "" },
    { "max-forwards", "" },
    { "proxy-authenticate", "" },
    { "proxy-authorization", "" },
    { "range", "" },
    { "referer", "" },
    { "refresh", "" },
    { "retry-after", "" },
    { "server", "" },
    { "set-cookie", "" },
    { "strict-transport-security", "" },
    { "transfer-encoding", "" },
    { "user-agent", "" },
    { "vary", "" },
    { "via", "" },
    { "www-authenticate", "" },
} };

struct Code {
    std::uint32_t bits; // right-aligned
    std::uint8_t length;
};

// Appendix B: the code of every octet, then of the end-of-string symbol.
constexpr std::array<Code, 257> huffman_codes { {
    { 0x1ff8, 13 }, { 0x7fffd8, 23 }, { 0xfffffe2, 28 }, { 0xfffffe3, 28 },
    { 0xfffffe4, 28 }, { 0xfffffe5, 28 }, { 0xfffffe6, 28 }, { 0xfffffe7, 28 },
    { 0xfffffe8, 28 }, { 0xffffea, 24 }, { 0x3ffffffc, 30 }, { 0xfffffe9, 28 },
    { 0xfffffea, 28 }, { 0x3ffffffd, 30 }, { 0xfffffeb, 28 }, { 0xfffffec, 28 },
    { 0xfffffed, 28 }, { 0xfffffee, 28 }, { 0xfffffef, 28 }, { 0xffffff0, 28 },
    { 0xffffff1, 28 }, { 0xffffff2, 28 }, { 0x3ffffffe, 30 }, { 0xffffff3, 28 },
    { 0xffffff4, 28 }, { 0xffffff5, 28 }, { 0xffffff6, 28 }, { 0xffffff7, 28 },
    { 0xffffff8, 28 }, { 0xffffff9, 28 }, { 0xffffffa, 28 }, { 0xffffffb, 28 },
    { 0x14, 6 }, { 0x3f8, 10 }, { 0x3f9, 10 }, { 0xffa, 12 }, // ' ' ! " #
    { 0x1ff9, 13 }, { 0x15, 6 }, { 0xf8, 8 }, { 0x7fa, 11 }, // $ % & '
    { 0x3fa, 10 }, { 0x3fb, 10 }, { 0xf9, 8 }, { 0x7fb, 11 }, // ( ) * +
    { 0xfa, 8 }, { 0x16, 6 }, { 0x17, 6 }, { 0x18, 6 }, // , - . /
    { 0x0, 5 }, { 0x1, 5 }, { 0x2, 5 }, { 0x19, 6 }, // 0 1 2 3
    { 0x1a, 6 }, { 0x1b, 6 }, { 0x1c, 6 }, { 0x1d, 6 }, // 4 5 6 7
    { 0x1e, 6 }, { 0x1f, 6 }, { 0x5c, 7 }, { 0xfb, 8 }, // 8 9 : ;
    { 0x7ffc, 15 }, { 0x20, 6 }, { 0xffb, 12 }, { 0x3fc, 10 }, // < = > ?
    { 0x1ffa, 13 }, { 0x21, 6 }, { 0x5d, 7 }, { 0x5e, 7 }, // @ A B C
    { 0x5f, 7 }, { 0x60, 7 }, { 0x61, 7 }, { 0x62, 7 }, // D E F G
    { 0x63, 7 }, { 0x64, 7 }, { 0x65, 7 }, { 0x66, 7 }, // H I J K
    { 0x67, 7 }, { 0x68, 7 }, { 0x69, 7 }, { 0x6a, 7 }, // L M N O
    { 0x6b, 7 }, { 0x6c, 7 }, { 0x6d, 7 }, { 0x6e, 7 }, // P Q R S
    { 0x6f, 7 }, { 0x70, 7 }, { 0x71, 7 }, { 0x72, 7 }, // T U V W
    { 0xfc, 8 }, { 0x73, 7 }, { 0xfd, 8 }, { 0x1ffb, 13 }, // X Y Z [
    { 0x7fff0, 19 }, { 0x1ffc, 13 }, { 0x3ffc, 14 }, { 0x22, 6 }, // backslash ] ^ _
    { 0x7ffd, 15 }, { 0x3, 5 }, { 0x23, 6 }, { 0x4, 5 }, // ` a b c
    { 0x24, 6 }, { 0x5, 5 }, { 0x25, 6 }, { 0x26, 6 }, // d e f g
    { 0x27, 6 }, { 0x6, 5 }, { 0x74, 7 }, { 0x75, 7 }, // h i j k
    { 0x28, 6 }, { 0x29, 6 }, { 0x2a, 6 }, { 0x7, 5 }, // l m n o
    { 0x2b, 6 }, { 0x76, 7 }, { 0x2c, 6 }, { 0x8, 5 }, // p q r s
    { 0x9, 5 }, { 0x2d, 6 }, { 0x77, 7 }, { 0x78, 7 }, // t u v w
    { 0x79, 7 }, { 0x7a, 7 }, { 0x7b, 7 }, { 0x7ffe, 15 }, // x y z {
    { 0x7fc, 11 }, { 0x3ffd, 14 }, { 0x1ffd, 13 }, { 0xffffffc, 28 }, // | } ~ DEL
    { 0xfffe6, 20 }, { 0x3fffd2, 22 }, { 0xfffe7, 20 }, { 0xfffe8, 20 },
    { 0x3fffd3, 22 }, { 0x3fffd4, 22 }, { 0x3fffd5, 22 }, { 0x7fffd9, 23 },
    { 0x3fffd6, 22 }, { 0x7fffda, 23 }, { 0x7fffdb, 23 }, { 0x7fffdc, 23 },
    { 0x7fffdd, 23 }, { 0x7fffde, 23 }, { 0xffffeb, 24 }, { 0x7fffdf, 23 },
    { 0xffffec, 24 }, { 0xffffed, 24 }, { 0x3fffd7, 22 }, { 0x7fffe0, 23 },
    { 0xffffee, 24 }, { 0x7fffe1, 23 }, { 0x7fffe2, 23 }, { 0x7fffe3, 23 },
    { 0x7fffe4, 23 }, { 0x1fffdc, 21 }, { 0x3fffd8, 22 }, { 0x7fffe5, 23 },
    { 0x3fffd9, 22 }, { 0x7fffe6, 23 }, { 0x7fffe7, 23 }, { 0xffffef, 24 },
    { 0x3fffda, 22 }, { 0x1fffdd, 21 }, { 0xfffe9, 20 }, { 0x3fffdb, 22 },
    { 0x3fffdc, 22 }, { 0x7fffe8, 23 }, { 0x7fffe9, 23 }, { 0x1fffde, 21 },
    { 0x7fffea, 23 }, { 0x3fffdd, 22 }, { 0x3fffde, 22 }, { 0xfffff0, 24 },
    { 0x1fffdf, 21 }, { 0x3fffdf, 22 }, { 0x7fffeb, 23 }, { 0x7fffec, 23 },
    { 0x1fffe0, 21 }, { 0x1fffe1, 21 }, { 0x3fffe0, 22 }, { 0x1fffe2, 21 },
    { 0x7fffed, 23 }, { 0x3fffe1, 22 }, { 0x7fffee, 23 }, { 0x7fffef, 23 },
    { 0xfffea, 20 }, { 0x3fffe2, 22 }, { 0x3fffe3, 22 }, { 0x3fffe4, 22 },
    { 0x7ffff0, 23 }, { 0x3fffe5, 22 }, { 0x3fffe6, 22 }, { 0x7ffff1, 23 },
    { 0x3ffffe0, 26 }, { 0x3ffffe1, 26 }, { 0xfffeb, 20 }, { 0x7fff1, 19 },
    { 0x3fffe7, 22 }, { 0x7ffff2, 23 }, { 0x3fffe8, 22 }, { 0x1ffffec, 25 },
    { 0x3ffffe2, 26 }, { 0x3ffffe3, 26 }, { 0x3ffffe4, 26 }, { 0x7ffffde, 27 },
    { 0x7ffffdf, 27 }, { 0x3ffffe5, 26 }, { 0xfffff1, 24 }, { 0x1ffffed, 25 },
    { 0x7fff2, 19 }, { 0x1fffe3, 21 }, { 0x3ffffe6, 26 }, { 0x7ffffe0, 27 },
    { 0x7ffffe1, 27 }, { 0x3ffffe7, 26 }, { 0x7ffffe2, 27 }, { 0xfffff2, 24 },
    { 0x1fffe4, 21 }, { 0x1fffe5, 21 }, { 0x3ffffe8, 26 }, { 0x3ffffe9, 26 },
    { 0xffffffd, 28 }, { 0x7ffffe3, 27 }, { 0x7ffffe4, 27 }, { 0x7ffffe5, 27 },
    { 0xfffec, 20 }, { 0xfffff3, 24 }, { 0xfffed, 20 }, { 0x1fffe6, 21 },
    { 0x3fffe9, 22 }, { 0x1fffe7, 21 }, { 0x1fffe8, 21 }, { 0x7ffff3, 23 },
    { 0x3fffea, 22 }, { 0x3fffeb, 22 }, { 0x1ffffee, 25 }, { 0x1ffffef, 25 },
    { 0xfffff4, 24 }, { 0xfffff5, 24 }, { 0x3ffffea, 26 }, { 0x7ffff4, 23 },
    { 0x3ffffeb, 26 }, { 0x7ffffe6, 27 }, { 0x3ffffec, 26 }, { 0x3ffffed, 26 },
    { 0x7ffffe7, 27 }, { 0x7ffffe8, 27 }, { 0x7ffffe9, 27 }, { 0x7ffffea, 27 },
    { 0x7ffffeb, 27 }, { 0xffffffe, 28 }, { 0x7ffffec, 27 }, { 0x7ffffed, 27 },
    { 0x7ffffee, 27 }, { 0x7ffffef, 27 }, { 0x7fffff0, 27 }, { 0x3ffffee, 26 },
    { 0x3fffffff, 30 }, // end of string
} };

constexpr int end_of_string = 256;

// The code as a binary tree, walked a bit at a time by the decoder: node 0
// is the root; a child below zero is the leaf of symbol -(child + 1).
struct HuffmanTree {
    struct Node {
        std::array<std::int16_t, 2> child { 0, 0 };
    };
    std::vector<Node> nodes;

    HuffmanTree()
    {
        nodes.reserve(256);
        nodes.emplace_back();
        for (int symbol = 0; symbol < 257; ++symbol) {
            Code const code = huffman_codes[static_cast<std::size_t>(symbol)];
            std::size_t node = 0;
            for (int bit = code.length - 1; bit >= 0; --bit) {
                std::size_t const branch = (code.bits >> bit) & 1u;
                if (bit == 0) {
                    nodes[node].child[branch] = static_cast<std::int16_t>(-(symbol + 1));
                    break;
                }
                if (nodes[node].child[branch] == 0) {
                    nodes[node].child[branch] = static_cast<std::int16_t>(nodes.size());
                    nodes.emplace_back();
                }
                node = static_cast<std::size_t>(nodes[node].child[branch]);
            }
        }
    }
};

HuffmanTree const& huffman_tree()
{
    static HuffmanTree const tree;
    return tree;
}

bool is_sensitive(std::string_view name)
{
    return name == "cookie" || name == "authorization" || name == "proxy-authorization";
}

} // namespace

std::optional<StaticEntry> static_entry(std::size_t index)
{
    if (index < 1 || index > static_table_size)
        return std::nullopt;
    return static_table[index - 1];
}

void encode_integer(std::vector<std::uint8_t>& out, std::uint64_t value, int prefix_bits, std::uint8_t high_bits)
{
    std::uint64_t const limit = (std::uint64_t { 1 } << prefix_bits) - 1;
    if (value < limit) {
        out.push_back(static_cast<std::uint8_t>(high_bits | value));
        return;
    }
    out.push_back(static_cast<std::uint8_t>(high_bits | limit));
    value -= limit;
    while (value >= 128) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

std::optional<std::uint64_t> decode_integer(std::span<std::uint8_t const> in, std::size_t& at, int prefix_bits)
{
    if (at >= in.size())
        return std::nullopt;
    std::uint64_t const limit = (std::uint64_t { 1 } << prefix_bits) - 1;
    std::uint64_t value = in[at++] & limit;
    if (value < limit)
        return value;
    int shift = 0;
    while (true) {
        if (at >= in.size() || shift > 28)
            return std::nullopt;
        std::uint8_t const octet = in[at++];
        value += static_cast<std::uint64_t>(octet & 0x7f) << shift;
        shift += 7;
        if (value > 0xffffffffu)
            return std::nullopt;
        if ((octet & 0x80) == 0)
            return value;
    }
}

std::size_t huffman_encoded_length(std::string_view text)
{
    std::size_t bits = 0;
    for (char const c : text)
        bits += huffman_codes[static_cast<unsigned char>(c)].length;
    return (bits + 7) / 8;
}

void huffman_encode(std::vector<std::uint8_t>& out, std::string_view text)
{
    std::uint64_t pending = 0;
    int pending_bits = 0;
    for (char const c : text) {
        Code const code = huffman_codes[static_cast<unsigned char>(c)];
        pending = (pending << code.length) | code.bits;
        pending_bits += code.length;
        while (pending_bits >= 8) {
            pending_bits -= 8;
            out.push_back(static_cast<std::uint8_t>(pending >> pending_bits));
        }
    }
    // Section 5.2: the last octet is filled with the high bits of the
    // end-of-string code, which are all ones.
    if (pending_bits > 0) {
        int const fill = 8 - pending_bits;
        out.push_back(static_cast<std::uint8_t>((pending << fill) | ((1u << fill) - 1)));
    }
}

std::optional<std::string> huffman_decode(std::span<std::uint8_t const> in, std::size_t max_length)
{
    HuffmanTree const& tree = huffman_tree();
    std::string out;
    std::size_t node = 0;
    int bits_since_symbol = 0;
    bool all_ones = true;
    for (std::uint8_t const octet : in) {
        for (int bit = 7; bit >= 0; --bit) {
            unsigned const branch = (octet >> bit) & 1u;
            std::int16_t const next = tree.nodes[node].child[branch];
            if (next < 0) {
                int const symbol = -(next + 1);
                if (symbol == end_of_string)
                    return std::nullopt;
                if (out.size() >= max_length)
                    return std::nullopt;
                out += static_cast<char>(symbol);
                node = 0;
                bits_since_symbol = 0;
                all_ones = true;
                continue;
            }
            node = static_cast<std::size_t>(next);
            ++bits_since_symbol;
            all_ones = all_ones && branch == 1;
        }
    }
    // What is left must be padding: a prefix of the end-of-string code, so
    // all ones, and shorter than an octet.
    if (bits_since_symbol > 7 || !all_ones)
        return std::nullopt;
    return out;
}

void encode_string(std::vector<std::uint8_t>& out, std::string_view text, bool huffman)
{
    if (huffman) {
        std::size_t const coded = huffman_encoded_length(text);
        if (coded <= text.size()) {
            encode_integer(out, coded, 7, 0x80);
            huffman_encode(out, text);
            return;
        }
    }
    encode_integer(out, text.size(), 7, 0);
    out.insert(out.end(), text.begin(), text.end());
}

std::optional<std::string> decode_string(std::span<std::uint8_t const> in, std::size_t& at, std::size_t max_length)
{
    if (at >= in.size())
        return std::nullopt;
    bool const huffman = (in[at] & 0x80) != 0;
    std::optional<std::uint64_t> const length = decode_integer(in, at, 7);
    if (!length || *length > in.size() - at)
        return std::nullopt;
    std::span<std::uint8_t const> const bytes = in.subspan(at, static_cast<std::size_t>(*length));
    at += bytes.size();
    if (huffman)
        return huffman_decode(bytes, max_length);
    if (bytes.size() > max_length)
        return std::nullopt;
    return std::string(bytes.begin(), bytes.end());
}

void DynamicTable::evict_to(std::size_t limit)
{
    while (m_size > limit && !m_entries.empty()) {
        m_size -= entry_size(m_entries.back().name, m_entries.back().value);
        m_entries.pop_back();
    }
}

void DynamicTable::set_max_size(std::size_t max_size)
{
    m_max_size = max_size;
    evict_to(max_size);
}

void DynamicTable::add(std::string name, std::string value)
{
    std::size_t const size = entry_size(name, value);
    if (size > m_max_size) {
        evict_to(0);
        return;
    }
    evict_to(m_max_size - size);
    m_entries.push_front(Field { std::move(name), std::move(value), false });
    m_size += size;
}

void Encoder::set_max_table_size(std::size_t size)
{
    if (!m_smallest_update || size < *m_smallest_update)
        m_smallest_update = size;
    m_final_update = size;
    m_table.set_max_size(size);
}

void Encoder::flush_size_updates(std::vector<std::uint8_t>& out)
{
    if (!m_final_update)
        return;
    if (m_smallest_update && *m_smallest_update < *m_final_update)
        encode_integer(out, *m_smallest_update, 5, 0x20);
    encode_integer(out, *m_final_update, 5, 0x20);
    m_smallest_update.reset();
    m_final_update.reset();
}

void Encoder::encode_field(Field const& field, Representation representation, std::vector<std::uint8_t>& out)
{
    flush_size_updates(out);
    // The index of a whole match and of a name match, the static table
    // first: its entries never move, so its index is the one a peer's
    // table cannot have lost.
    std::size_t whole = 0;
    std::size_t name_only = 0;
    for (std::size_t i = 0; i < static_table.size(); ++i) {
        if (static_table[i].name != field.name)
            continue;
        if (name_only == 0)
            name_only = i + 1;
        if (static_table[i].value == field.value) {
            whole = i + 1;
            break;
        }
    }
    for (std::size_t i = 0; i < m_table.count() && whole == 0; ++i) {
        Field const& entry = m_table.at(i);
        if (entry.name != field.name)
            continue;
        if (name_only == 0)
            name_only = static_table_size + 1 + i;
        if (entry.value == field.value)
            whole = static_table_size + 1 + i;
    }

    switch (representation) {
    case Representation::Indexed:
        if (whole != 0)
            encode_integer(out, whole, 7, 0x80);
        return;
    case Representation::IncrementalIndexing:
        encode_integer(out, name_only, 6, 0x40);
        break;
    case Representation::WithoutIndexing:
        encode_integer(out, name_only, 4, 0x00);
        break;
    case Representation::NeverIndexed:
        encode_integer(out, name_only, 4, 0x10);
        break;
    }
    if (name_only == 0)
        encode_string(out, field.name, m_huffman);
    encode_string(out, field.value, m_huffman);
    if (representation == Representation::IncrementalIndexing)
        m_table.add(field.name, field.value);
}

void Encoder::encode(std::span<Field const> fields, std::vector<std::uint8_t>& out)
{
    flush_size_updates(out);
    for (Field const& field : fields) {
        bool const sensitive = field.never_indexed || is_sensitive(field.name);
        if (sensitive) {
            encode_field(field, Representation::NeverIndexed, out);
            continue;
        }
        std::size_t const before = out.size();
        encode_field(field, Representation::Indexed, out);
        if (out.size() != before)
            continue;
        // An entry that would take most of the table pushes out everything
        // that was worth keeping for one field that may never come again.
        bool const worth_keeping = entry_size(field.name, field.value) * 4 <= m_table.max_size() * 3;
        encode_field(field, worth_keeping ? Representation::IncrementalIndexing : Representation::WithoutIndexing, out);
    }
}

std::vector<std::uint8_t> Encoder::encode(std::span<Field const> fields)
{
    std::vector<std::uint8_t> out;
    encode(fields, out);
    return out;
}

std::optional<std::vector<Field>> Decoder::fail(std::string reason)
{
    m_error = std::move(reason);
    return std::nullopt;
}

bool Decoder::lookup(std::uint64_t index, Field& out)
{
    if (index == 0)
        return false;
    if (index <= static_table_size) {
        StaticEntry const entry = static_table[static_cast<std::size_t>(index - 1)];
        out.name = std::string(entry.name);
        out.value = std::string(entry.value);
        return true;
    }
    std::uint64_t const dynamic = index - static_table_size - 1;
    if (dynamic >= m_table.count())
        return false;
    out.name = m_table.at(static_cast<std::size_t>(dynamic)).name;
    out.value = m_table.at(static_cast<std::size_t>(dynamic)).value;
    return true;
}

std::optional<std::vector<Field>> Decoder::decode(std::span<std::uint8_t const> block)
{
    std::vector<Field> fields;
    std::size_t list_size = 0;
    std::size_t at = 0;
    bool field_seen = false;
    while (at < block.size()) {
        std::uint8_t const first = block[at];
        if ((first & 0xe0) == 0x20) {
            // Section 4.2: a size update belongs at the start of a block.
            if (field_seen)
                return fail("a dynamic table size update after a field");
            std::optional<std::uint64_t> const size = decode_integer(block, at, 5);
            if (!size)
                return fail("a size update that does not decode");
            if (*size > m_max_table_size)
                return fail("a size update above the advertised limit");
            m_table.set_max_size(static_cast<std::size_t>(*size));
            continue;
        }
        field_seen = true;
        Field field;
        if ((first & 0x80) != 0) {
            std::optional<std::uint64_t> const index = decode_integer(block, at, 7);
            if (!index || !lookup(*index, field))
                return fail("an index outside both tables");
        } else {
            int const prefix = (first & 0x40) != 0 ? 6 : 4;
            bool const indexing = (first & 0x40) != 0;
            field.never_indexed = !indexing && (first & 0x10) != 0;
            std::optional<std::uint64_t> const index = decode_integer(block, at, prefix);
            if (!index)
                return fail("a literal whose name index does not decode");
            std::size_t const remaining = m_max_list_size - list_size;
            if (*index != 0) {
                Field named;
                if (!lookup(*index, named))
                    return fail("a name index outside both tables");
                field.name = std::move(named.name);
            } else {
                std::optional<std::string> name = decode_string(block, at, remaining);
                if (!name)
                    return fail("a literal name that does not decode");
                field.name = std::move(*name);
            }
            std::optional<std::string> value = decode_string(block, at, remaining);
            if (!value)
                return fail("a literal value that does not decode");
            field.value = std::move(*value);
            if (indexing)
                m_table.add(field.name, field.value);
        }
        list_size += entry_size(field.name, field.value);
        if (list_size > m_max_list_size)
            return fail("a header list larger than the advertised limit");
        fields.push_back(std::move(field));
    }
    return fields;
}

}
