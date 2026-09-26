#pragma once

// HPACK (RFC 7541), the header compression HTTP/2 carries every request's
// and response's fields in: a static table of 61 common fields, a dynamic
// table each side grows as it sends, integers with a prefix, and string
// literals either as they are or in the Huffman code of Appendix B. The
// encoder and the decoder each keep a table that must stay in step with
// the peer's, so a connection has one of each and every header block goes
// through them in the order it is on the wire.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::net::hpack {

struct Field {
    std::string name;
    std::string value;
    // On the way out: this field is sent as never indexed (Section 6.2.3), so
    // no table on the path keeps it. On the way in: it arrived that way.
    bool never_indexed = false;

    bool operator==(Field const&) const = default;
};

// The representations of Section 6.
enum class Representation {
    Indexed,
    IncrementalIndexing,
    WithoutIndexing,
    NeverIndexed,
};

// Section 4.1: what an entry costs in a table's size.
constexpr std::size_t entry_size(std::string_view name, std::string_view value)
{
    return name.size() + value.size() + 32;
}

// Section 5.1: an integer with a prefix of `prefix_bits` (1 to 8) bits; the
// bits of the first octet above the prefix are `high_bits`.
void encode_integer(std::vector<std::uint8_t>& out, std::uint64_t value, int prefix_bits, std::uint8_t high_bits = 0);
// Reads one starting at `at`, which it advances. Nothing when it is cut
// short or larger than 32 bits hold, which no field a peer sends needs.
std::optional<std::uint64_t> decode_integer(std::span<std::uint8_t const> in, std::size_t& at, int prefix_bits);

// Section 5.2: a string literal, Huffman-coded or plain.
void encode_string(std::vector<std::uint8_t>& out, std::string_view text, bool huffman);
std::optional<std::string> decode_string(std::span<std::uint8_t const> in, std::size_t& at, std::size_t max_length);

// Appendix B.
std::size_t huffman_encoded_length(std::string_view text);
void huffman_encode(std::vector<std::uint8_t>& out, std::string_view text);
// Nothing when the code is broken: padding longer than seven bits or not
// all ones, the end-of-string symbol inside the string, or more than
// `max_length` octets decoded.
std::optional<std::string> huffman_decode(std::span<std::uint8_t const> in, std::size_t max_length);

// Appendix A: entries 1 to 61.
inline constexpr std::size_t static_table_size = 61;
struct StaticEntry {
    std::string_view name;
    std::string_view value;
};
// Nothing outside 1..61.
std::optional<StaticEntry> static_entry(std::size_t index);

// Section 2.3.2: the newest entry first, bounded by a size in octets.
class DynamicTable {
public:
    explicit DynamicTable(std::size_t max_size = 4096)
        : m_max_size(max_size)
    {
    }

    std::size_t size() const { return m_size; }
    std::size_t max_size() const { return m_max_size; }
    std::size_t count() const { return m_entries.size(); }
    // 0 is the newest entry.
    Field const& at(std::size_t index) const { return m_entries[index]; }

    // Section 4.3: evicts from the oldest end until the table fits.
    void set_max_size(std::size_t max_size);
    // Section 4.4: evicts to make room; an entry larger than the whole
    // table empties it and is not kept.
    void add(std::string name, std::string value);

private:
    void evict_to(std::size_t limit);

    std::deque<Field> m_entries;
    std::size_t m_size = 0;
    std::size_t m_max_size;
};

class Encoder {
public:
    explicit Encoder(std::size_t table_size = 4096)
        : m_table(table_size)
    {
    }

    // The peer's SETTINGS_HEADER_TABLE_SIZE. The table follows it, and the
    // next block starts with the size updates Section 4.2 asks for: the
    // smallest size it passed through, then the final one.
    void set_max_table_size(std::size_t size);
    // Strings are Huffman-coded when that is no longer than plain; off
    // sends every string plain.
    void set_huffman(bool huffman) { m_huffman = huffman; }

    // One header block. A field that matches a table entry whole is sent
    // as its index; any other is a literal, kept in the table (named by
    // index when a table has the name) unless it is sensitive (cookie,
    // authorization and proxy-authorization, or a field marked so), which
    // goes as never indexed, or is too big to be worth keeping.
    void encode(std::span<Field const> fields, std::vector<std::uint8_t>& out);
    std::vector<std::uint8_t> encode(std::span<Field const> fields);

    // One field in the representation given, for a caller that chooses.
    // Indexed needs a whole match in a table and writes nothing without one.
    void encode_field(Field const& field, Representation representation, std::vector<std::uint8_t>& out);

    DynamicTable const& table() const { return m_table; }

private:
    void flush_size_updates(std::vector<std::uint8_t>& out);

    DynamicTable m_table;
    bool m_huffman = true;
    std::optional<std::size_t> m_smallest_update;
    std::optional<std::size_t> m_final_update;
};

class Decoder {
public:
    // `max_table_size` is what this side advertised in its SETTINGS: the
    // bound on every size update. `max_list_size` bounds a block's fields
    // by Section 4.1's measure, which is SETTINGS_MAX_HEADER_LIST_SIZE's.
    explicit Decoder(std::size_t max_table_size = 4096, std::size_t max_list_size = 256 * 1024)
        : m_table(4096)
        , m_max_table_size(max_table_size)
        , m_max_list_size(max_list_size)
    {
    }

    void set_max_table_size(std::size_t size) { m_max_table_size = size; }

    // One whole header block. Nothing on any error, which HTTP/2 makes a
    // connection error (COMPRESSION_ERROR): the table is out of step with
    // the peer's from then on.
    std::optional<std::vector<Field>> decode(std::span<std::uint8_t const> block);
    std::string const& error() const { return m_error; }

    DynamicTable const& table() const { return m_table; }

private:
    bool lookup(std::uint64_t index, Field& out);
    std::optional<std::vector<Field>> fail(std::string reason);

    DynamicTable m_table;
    std::size_t m_max_table_size;
    std::size_t m_max_list_size;
    std::string m_error;
};

}
