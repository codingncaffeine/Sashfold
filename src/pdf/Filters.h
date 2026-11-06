#pragma once

// PDF's standard filters (ISO 32000-2 §7.4): FlateDecode and LZWDecode
// with their PNG and TIFF predictors, ASCIIHexDecode, ASCII85Decode and
// RunLengthDecode. The picture codecs (DCTDecode, JPXDecode, JBIG2Decode,
// CCITTFaxDecode) end a chain: what reaches them is handed to the picture
// code with its parameters. Every decoder is lenient the way readers are —
// a stream that breaks yields what came before the break — and bounded.

#include "pdf/Object.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sashfold::pdf {

inline constexpr std::size_t k_max_stream_bytes = 512u * 1024u * 1024u;

// The filters that end a chain because a picture decoder takes their bytes.
bool is_image_filter(std::string const& name);

// One filter by its name (long or the inline-image abbreviation) over the
// data, with its decode parameters (already resolved). An unknown filter
// gives nothing back, as the stream cannot be read.
std::vector<std::uint8_t> apply_filter(std::string const& name, std::vector<std::uint8_t> const& data,
    Dict const& params, std::size_t max_output = k_max_stream_bytes);

std::vector<std::uint8_t> ascii_hex_decode(std::vector<std::uint8_t> const& data);
std::vector<std::uint8_t> ascii85_decode(std::vector<std::uint8_t> const& data);
std::vector<std::uint8_t> run_length_decode(std::vector<std::uint8_t> const& data, std::size_t max_output);
std::vector<std::uint8_t> lzw_decode(std::vector<std::uint8_t> const& data, bool early_change, std::size_t max_output);
// The predictor named in `params` undone (none for Predictor 1 or absent).
std::vector<std::uint8_t> unpredict(std::vector<std::uint8_t> data, Dict const& params);

// The full name for an inline image's abbreviation ("Fl" → "FlateDecode").
std::string expand_filter_name(std::string const& name);

}
