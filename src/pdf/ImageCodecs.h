#pragma once

// The picture codecs PDF's scanned files are made of, each written here:
// CCITT Group 3 and Group 4 fax (ITU-T T.4/T.6, the /CCITTFaxDecode filter),
// JBIG2 (ITU-T T.88, /JBIG2Decode) and JPEG 2000 (ITU-T T.800, /JPXDecode).
// The bilevel codecs give rows of 1-bit samples as PDF's /DeviceGray image
// reads them, 0 = black: CCITT's by default (BlackIs1 false), and JBIG2's
// always, the filter turning the coding's 1 = black around (ISO 32000-2
// §7.4.7), as poppler's and pdf.js's do.

#include "pdf/Object.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::pdf {

// Rows of (width + 7) / 8 bytes, as an image's samples at 1 bit; the
// /DecodeParms say K, Columns, Rows, BlackIs1, EncodedByteAlign and
// EndOfBlock. Width and height come from the image when the parameters
// leave them out.
std::optional<std::vector<std::uint8_t>> decode_ccitt(std::vector<std::uint8_t> const& data, Dict const& params, int width,
    int height);

// T.6 (MMR) coding read bitmap by bitmap from one stream, as JBIG2 codes
// a generic region, a collective bitmap or the bitplanes of a grayscale
// image (T.88 6.2.6, C.5): each bitmap starts against a white reference
// line, the data carries on from where the last one ended. `pixels` takes
// width * height bytes, 1 = black; false when the data ended or broke
// before the last row (what was decoded stands, the rest white).
class MmrDecoder {
public:
    explicit MmrDecoder(std::span<std::uint8_t const> data);
    bool decode(int width, int height, std::uint8_t* pixels);
    // An EOFB (two end-of-lines) where the data stands, if there is one,
    // and the rest of its byte: the next bitmap starts on a byte.
    void skip_end_of_block();
    // The first byte not yet read.
    std::size_t byte_position() const;

private:
    std::span<std::uint8_t const> m_data;
    std::size_t m_bit_position = 0;
};

// The page of an embedded JBIG2 stream (with its global segments) at the
// image's size, as rows of (width + 7) / 8 bytes, 0 = black.
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
