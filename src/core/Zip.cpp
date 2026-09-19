#include "core/Zip.h"

#include "core/Inflate.h"

#include <algorithm>

namespace sashfold {

namespace {

constexpr std::uint32_t end_of_directory = 0x06054b50u;
constexpr std::uint32_t directory_entry = 0x02014b50u;
constexpr std::uint32_t local_entry = 0x04034b50u;
constexpr std::size_t end_record_size = 22;
constexpr std::size_t directory_entry_size = 46;
constexpr std::size_t local_entry_size = 30;
constexpr std::size_t longest_name = 1024;

std::uint16_t u16(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

std::uint32_t u32(std::vector<std::uint8_t> const& bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

std::string_view without_lead(std::string_view name)
{
    while (name.starts_with("./"))
        name.remove_prefix(2);
    while (name.starts_with("/"))
        name.remove_prefix(1);
    return name;
}

}

std::optional<ZipArchive> ZipArchive::open(std::vector<std::uint8_t> bytes)
{
    if (bytes.size() < end_record_size)
        return std::nullopt;
    // The end record is the last thing in the file but for a comment of at
    // most 65,535 bytes: looked for from the end backwards.
    std::size_t const earliest = bytes.size() - std::min(bytes.size(), end_record_size + std::size_t { 65535 });
    std::optional<std::size_t> end;
    for (std::size_t at = bytes.size() - end_record_size + 1; at-- > earliest;) {
        if (u32(bytes, at) == end_of_directory && at + end_record_size + u16(bytes, at + 20) <= bytes.size()) {
            end = at;
            break;
        }
    }
    if (!end)
        return std::nullopt;
    if (u16(bytes, *end + 4) != 0 || u16(bytes, *end + 6) != 0)
        return std::nullopt; // an archive in several parts
    std::uint16_t const count = u16(bytes, *end + 10);
    std::uint64_t const directory_size = u32(bytes, *end + 12);
    std::uint64_t const directory_offset = u32(bytes, *end + 16);
    if (count == 0xFFFFu || directory_size == 0xFFFFFFFFu || directory_offset == 0xFFFFFFFFu)
        return std::nullopt; // zip64
    // The directory ends where the end record begins; whatever the offsets
    // leave unaccounted for before it stands in front of the archive.
    if (directory_size > *end || directory_offset > *end - directory_size)
        return std::nullopt;
    ZipArchive archive;
    archive.m_base = *end - directory_size - directory_offset;
    std::size_t at = static_cast<std::size_t>(archive.m_base + directory_offset);
    for (std::uint16_t i = 0; i < count; ++i) {
        if (at + directory_entry_size > *end || u32(bytes, at) != directory_entry)
            return std::nullopt;
        ZipEntry entry;
        entry.flags = u16(bytes, at + 8);
        entry.method = u16(bytes, at + 10);
        entry.crc32 = u32(bytes, at + 16);
        entry.compressed_size = u32(bytes, at + 20);
        entry.size = u32(bytes, at + 24);
        std::size_t const name_length = u16(bytes, at + 28);
        std::size_t const extra_length = u16(bytes, at + 30);
        std::size_t const comment_length = u16(bytes, at + 32);
        entry.local_header = u32(bytes, at + 42);
        if (name_length > longest_name || at + directory_entry_size + name_length + extra_length + comment_length > *end)
            return std::nullopt;
        if (entry.compressed_size == 0xFFFFFFFFu || entry.size == 0xFFFFFFFFu || entry.local_header == 0xFFFFFFFFu)
            return std::nullopt; // zip64
        entry.name.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + directory_entry_size),
            bytes.begin() + static_cast<std::ptrdiff_t>(at + directory_entry_size + name_length));
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        archive.m_entries.push_back(std::move(entry));
        at += directory_entry_size + name_length + extra_length + comment_length;
    }
    archive.m_bytes = std::move(bytes);
    return archive;
}

ZipEntry const* ZipArchive::find(std::string_view name) const
{
    std::string_view const wanted = without_lead(name);
    for (ZipEntry const& entry : m_entries) {
        if (without_lead(entry.name) == wanted)
            return &entry;
    }
    return nullptr;
}

std::optional<std::vector<std::uint8_t>> ZipArchive::read(ZipEntry const& entry, std::size_t max_size) const
{
    if ((entry.flags & 1u) != 0 || (entry.method != 0 && entry.method != 8) || entry.size > max_size)
        return std::nullopt;
    std::uint64_t const header = m_base + entry.local_header;
    if (header + local_entry_size > m_bytes.size())
        return std::nullopt;
    std::size_t const at = static_cast<std::size_t>(header);
    if (u32(m_bytes, at) != local_entry)
        return std::nullopt;
    // The sizes are the directory's — a local header written before its
    // data was packed holds zeros — and only the lengths of its own name
    // and extra field are taken from it.
    std::uint64_t const data = header + local_entry_size + u16(m_bytes, at + 26) + u16(m_bytes, at + 28);
    if (data + entry.compressed_size > m_bytes.size())
        return std::nullopt;
    std::vector<std::uint8_t> packed(m_bytes.begin() + static_cast<std::ptrdiff_t>(data),
        m_bytes.begin() + static_cast<std::ptrdiff_t>(data + entry.compressed_size));
    std::optional<std::vector<std::uint8_t>> unpacked;
    if (entry.method == 0)
        unpacked = std::move(packed);
    else
        unpacked = inflate(packed, max_size);
    if (!unpacked || unpacked->size() != entry.size || crc32_of(*unpacked) != entry.crc32)
        return std::nullopt;
    return unpacked;
}

}
