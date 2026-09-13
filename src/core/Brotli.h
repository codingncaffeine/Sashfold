#pragma once

// Brotli decoding (RFC 7932): the Content-Encoding a browser asks for beside
// gzip, and the compression inside a WOFF2 font. Simple and complex prefix
// codes, block switching, context modeling, the static dictionary with its
// 121 word transforms, and the hard output cap Inflate.h has, so a hostile
// stream cannot balloon.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

// A whole brotli stream. nullopt on any malformed input, on a stream that
// ends early, or when the output would exceed `max_output`.
std::optional<std::vector<std::uint8_t>> brotli_decompress(std::uint8_t const* data, std::size_t size,
    std::size_t max_output = 256u * 1024u * 1024u);

inline std::optional<std::vector<std::uint8_t>> brotli_decompress(std::vector<std::uint8_t> const& data,
    std::size_t max_output = 256u * 1024u * 1024u)
{
    return brotli_decompress(data.data(), data.size(), max_output);
}

// Exposed for tests, which check them against the values RFC 7932 gives:
// the word transforms laid out as Appendix B describes (each prefix and a
// zero byte, the transform's number, the suffix and a zero byte), one of
// the three context lookup tables of §7.1 (256 bytes), and the static
// dictionary.
std::vector<std::uint8_t> brotli_transform_bytes();
std::uint8_t const* brotli_context_lookup(int table);
std::uint8_t const* brotli_dictionary_bytes();
std::size_t brotli_dictionary_size();

}
