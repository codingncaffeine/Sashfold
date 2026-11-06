#pragma once

// JPEG decoding, baseline and extended-sequential Huffman: quantization and
// Huffman tables, the entropy-coded scans with byte stuffing and restart
// markers, an integer inverse DCT, chroma upsampling, and YCbCr, grayscale
// and Adobe RGB color. Progressive and arithmetic-coded files are
// recognized and declined for now. Every table index and every size is
// bounded; a stream that ends early leaves the rest of the picture gray.

#include "core/Bitmap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sashfold {

// The decoded samples before they become pixels, for a caller with color
// rules of its own (PDF's color spaces and /Decode arrays): `components`
// interleaved bytes per pixel, upsampled; three-component YCbCr already
// turned to RGB and YCCK to CMYK, as the file's Adobe segment says. An
// Adobe CMYK file's inks are as stored (Photoshop writes them inverted).
struct JpegSamples {
    int width = 0;
    int height = 0;
    int components = 0;
    bool adobe = false; // an Adobe APP14 segment was seen
    std::vector<std::uint8_t> samples;
};
std::optional<JpegSamples> decode_jpeg_samples(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

// Pixels: gray, RGB, and CMYK as browsers show it.
std::optional<Bitmap> decode_jpeg(std::vector<std::uint8_t> const& bytes,
    std::size_t max_pixels = 32u * 1024u * 1024u);

bool looks_like_jpeg(std::vector<std::uint8_t> const& bytes);

}
