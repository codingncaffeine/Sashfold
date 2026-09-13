#include "text/Woff.h"

#include "core/Inflate.h"

namespace sashfold::text {

namespace {

constexpr std::uint32_t tag_woff = 0x774F4646u; // 'wOFF'
constexpr std::uint32_t tag_woff2 = 0x774F4632u; // 'wOF2'

std::uint32_t u32(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) << 24 | static_cast<std::uint32_t>(bytes[at + 1]) << 16
        | static_cast<std::uint32_t>(bytes[at + 2]) << 8 | static_cast<std::uint32_t>(bytes[at + 3]);
}

std::uint16_t u16(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[at]) << 8 | bytes[at + 1]);
}

void push_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void push_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    push_u16(out, static_cast<std::uint16_t>(value >> 16));
    push_u16(out, static_cast<std::uint16_t>(value));
}

} // namespace

bool is_woff(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= 4 && u32(bytes, 0) == tag_woff;
}

bool is_woff2(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= 4 && u32(bytes, 0) == tag_woff2;
}

std::optional<std::vector<std::uint8_t>> unwrap_woff(std::vector<std::uint8_t> const& bytes, std::size_t max_output)
{
    // The header: the signature, the sfnt's own version (its flavor), the
    // table count; the rest — lengths, metadata, private data — is not
    // needed to rebuild the font.
    if (!is_woff(bytes) || bytes.size() < 44)
        return std::nullopt;
    std::uint32_t const flavor = u32(bytes, 4);
    std::size_t const table_count = u16(bytes, 12);
    if (table_count == 0 || table_count > 512 || bytes.size() < 44 + table_count * 20)
        return std::nullopt;

    struct Entry {
        std::uint32_t tag;
        std::uint32_t offset;
        std::uint32_t compressed_length;
        std::uint32_t length;
        std::uint32_t checksum;
    };
    std::vector<Entry> entries;
    std::size_t total = 12 + table_count * 16;
    for (std::size_t i = 0; i < table_count; ++i) {
        std::size_t const at = 44 + i * 20;
        Entry const entry { u32(bytes, at), u32(bytes, at + 4), u32(bytes, at + 8), u32(bytes, at + 12),
            u32(bytes, at + 16) };
        if (entry.offset > bytes.size() || entry.compressed_length > bytes.size() - entry.offset
            || entry.compressed_length > entry.length)
            return std::nullopt;
        total += (static_cast<std::size_t>(entry.length) + 3) & ~static_cast<std::size_t>(3);
        if (total > max_output)
            return std::nullopt;
        entries.push_back(entry);
    }

    // The sfnt: its header and a directory with the offsets filled in as
    // the tables land, each inflated when it was compressed and 4-aligned.
    std::vector<std::uint8_t> out;
    out.reserve(total);
    push_u32(out, flavor);
    push_u16(out, static_cast<std::uint16_t>(table_count));
    std::uint16_t search_range = 16;
    std::uint16_t entry_selector = 0;
    while (search_range * 2 <= table_count * 16) {
        search_range = static_cast<std::uint16_t>(search_range * 2);
        ++entry_selector;
    }
    push_u16(out, search_range);
    push_u16(out, entry_selector);
    push_u16(out, static_cast<std::uint16_t>(table_count * 16 - search_range));
    std::size_t const directory = out.size();
    out.resize(directory + table_count * 16, 0);
    for (std::size_t i = 0; i < table_count; ++i) {
        Entry const& entry = entries[i];
        std::vector<std::uint8_t> const compressed(bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset + entry.compressed_length));
        std::vector<std::uint8_t> table;
        if (entry.compressed_length < entry.length) {
            std::optional<std::vector<std::uint8_t>> inflated = zlib_decompress(compressed, entry.length);
            if (!inflated || inflated->size() != entry.length)
                return std::nullopt;
            table = std::move(*inflated);
        } else {
            table = compressed;
        }
        std::size_t const record = directory + i * 16;
        auto const put = [&](std::size_t at, std::uint32_t value) {
            out[at] = static_cast<std::uint8_t>(value >> 24);
            out[at + 1] = static_cast<std::uint8_t>(value >> 16);
            out[at + 2] = static_cast<std::uint8_t>(value >> 8);
            out[at + 3] = static_cast<std::uint8_t>(value);
        };
        put(record, entry.tag);
        put(record + 4, entry.checksum);
        put(record + 8, static_cast<std::uint32_t>(out.size()));
        put(record + 12, entry.length);
        out.insert(out.end(), table.begin(), table.end());
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

}
