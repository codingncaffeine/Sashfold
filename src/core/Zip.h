#pragma once

// Reading a zip archive: the directory at its end, and an entry's bytes —
// stored, or deflated and unpacked by our own inflate — checked against the
// CRC-32 the directory gives. The archive is found from its end, as every
// reader finds it, so what stands before it does not matter: a Chrome
// extension's .crx is a zip behind a signed header, and reads as one. No
// zip64, no encryption, no spanning: what is asked of this is a browser
// theme's few files, and anything else is refused rather than guessed at.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold {

struct ZipEntry {
    std::string name; // as the archive spells it: forward slashes, no leading one
    std::uint16_t method = 0; // 0 stored, 8 deflated
    std::uint16_t flags = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t size = 0;
    std::uint64_t local_header = 0; // from the archive's first byte
    bool is_directory() const { return !name.empty() && name.back() == '/'; }
};

class ZipArchive {
public:
    // Nullopt for bytes that hold no directory this reads.
    static std::optional<ZipArchive> open(std::vector<std::uint8_t> bytes);

    std::vector<ZipEntry> const& entries() const { return m_entries; }
    // The entry of exactly that name, a leading "./" or "/" aside.
    ZipEntry const* find(std::string_view name) const;
    // The entry's bytes. Nullopt when they are packed in a way not read
    // here, are encrypted, would unpack past `max_size`, or do not match
    // the entry's length or CRC-32.
    std::optional<std::vector<std::uint8_t>> read(ZipEntry const& entry, std::size_t max_size) const;

private:
    std::vector<std::uint8_t> m_bytes;
    std::uint64_t m_base = 0; // where the archive begins inside the bytes
    std::vector<ZipEntry> m_entries;
};

}
