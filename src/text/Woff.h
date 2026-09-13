#pragma once

// WOFF, the web font wrapper (W3C WOFF File Format 1.0): an sfnt whose
// tables sit behind a directory of their own, each zlib-compressed or
// stored as it was. Unwrapping rebuilds the sfnt — the directory in the
// wrapper's order, every table inflated and 4-aligned — for the TrueType
// reader, which then reads it as any other font. WOFF 2.0 (one brotli
// stream, the glyf and loca tables transformed) is not read yet.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold::text {

bool is_woff(std::vector<std::uint8_t> const& bytes); // begins with 'wOFF'
bool is_woff2(std::vector<std::uint8_t> const& bytes); // begins with 'wOF2'

// The sfnt inside a WOFF 1.0 file; nullopt for anything malformed — a
// directory past the end, a table that does not inflate to the length it
// claims — or larger than `max_output` once unwrapped.
std::optional<std::vector<std::uint8_t>> unwrap_woff(std::vector<std::uint8_t> const& bytes,
    std::size_t max_output = 64u * 1024u * 1024u);

}
