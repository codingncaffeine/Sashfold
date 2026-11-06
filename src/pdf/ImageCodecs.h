#pragma once

// The picture codecs PDF's scanned files are made of, each written here:
// CCITT Group 3 and Group 4 fax (ITU-T T.4/T.6, the /CCITTFaxDecode filter),
// JBIG2 (ITU-T T.88, /JBIG2Decode) and JPEG 2000 (ITU-T T.800, /JPXDecode).
// The bilevel codecs give rows of 1-bit samples, 1 = black unless the
// parameters say otherwise, as PDF's /DeviceGray image with /Decode [0 1]
// reads them: CCITT gives 0 for black by default (BlackIs1 false), JBIG2 1.

#include "pdf/Object.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold::pdf {

// Rows of (width + 7) / 8 bytes, as an image's samples at 1 bit; the
// /DecodeParms say K, Columns, Rows, BlackIs1, EncodedByteAlign and
// EndOfBlock. Width and height come from the image when the parameters
// leave them out.
std::optional<std::vector<std::uint8_t>> decode_ccitt(std::vector<std::uint8_t> const& data, Dict const& params, int width,
    int height);

// The page region of an embedded JBIG2 stream (with its global segments),
// in the same layout: 1 = black, which PDF's image sees as sample 1.
std::optional<std::vector<std::uint8_t>> decode_jbig2(std::vector<std::uint8_t> const& data,
    std::vector<std::uint8_t> const& globals, int width, int height);

// A JPEG 2000 codestream or JP2 file, as 8-bit interleaved components
// (1 gray, 3 RGB already through the codestream's color transform, 4 CMYK)
// and its alpha channel apart, when it has one.
struct CodecPicture {
    int width = 0;
    int height = 0;
    int components = 0;
    std::vector<std::uint8_t> samples;
    std::vector<std::uint8_t> alpha;
};
std::optional<CodecPicture> decode_jpx(std::vector<std::uint8_t> const& data, std::size_t max_pixels);

}
