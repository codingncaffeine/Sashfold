#pragma once

// DEFLATE decoding (RFC 1951) with the zlib (RFC 1950) and gzip (RFC 1952)
// containers — the decompression half of the web: Content-Encoding gzip and
// deflate now, PNG IDAT later. All three block types, integrity checks
// (Adler-32 / CRC-32 / ISIZE), and a hard output cap so hostile streams
// cannot balloon.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

// Raw DEFLATE stream. nullopt on any malformed input or when the output
// would exceed `max_output`.
std::optional<std::vector<std::uint8_t>> inflate(std::vector<std::uint8_t> const& data,
    std::size_t max_output = 256u * 1024u * 1024u);

// zlib wrapper: header checks + Adler-32 verification.
std::optional<std::vector<std::uint8_t>> zlib_decompress(std::vector<std::uint8_t> const& data,
    std::size_t max_output = 256u * 1024u * 1024u);

// gzip wrapper: header parse (flags honored), CRC-32 + ISIZE verification.
std::optional<std::vector<std::uint8_t>> gzip_decompress(std::vector<std::uint8_t> const& data,
    std::size_t max_output = 256u * 1024u * 1024u);

}
