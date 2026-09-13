#pragma once

// WOFF, the web font wrapper, in both its versions. WOFF 1.0: an sfnt whose
// tables sit behind a directory of their own, each zlib-compressed or
// stored as it was; unwrapping rebuilds the sfnt, the directory in the
// wrapper's order and every table inflated and 4-aligned. WOFF 2.0: every
// table in one brotli stream, glyf, loca and hmtx perhaps transformed, the
// file perhaps a whole collection; unwrapping rebuilds those tables and
// the sfnt (or the TTC) around them. Either way the TrueType reader then
// reads the result as any other font.

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

// The sfnt, or the font collection, inside a WOFF 2.0 file; nullopt for
// anything malformed — a directory or a stream past the end, a stream that
// does not decompress to exactly the tables' lengths, a transformed table
// that does not rebuild — or larger than `max_output` once unwrapped.
std::optional<std::vector<std::uint8_t>> unwrap_woff2(std::vector<std::uint8_t> const& bytes,
    std::size_t max_output = 64u * 1024u * 1024u);

}
