#pragma once

// PDF's sampled images (ISO 32000-2 §8.9) made into pictures: samples of
// 1 to 16 bits through the /Decode array and the color space, or through a
// picture codec (JPEG, and the fax, JBIG2 and JPEG 2000 codecs of the scanned
// files); stencil masks (/ImageMask), color-key masks (/Mask array),
// explicit masks (/Mask stream) and soft masks (/SMask, with /Matte).
// Where a mask is finer than its image both are taken at the finer size,
// so a sharp mask over a coarse picture keeps its edge.

#include "paint/Canvas2D.h"
#include "pdf/Document.h"

#include <optional>

namespace sashfold::pdf {

struct DecodedImage {
    canvas::Surface surface; // premultiplied RGBA; a stencil's color is black
    bool stencil = false; // painted with the fill color through its alpha
    bool interpolate = false;
};

inline constexpr std::size_t k_max_image_pixels = 64u * 1024u * 1024u;

// An image XObject's stream, or an inline image's dictionary (keys
// expanded already) and data. `resources` resolves a named color space.
std::optional<DecodedImage> decode_image(Document const& doc, Dict const& dict, ImageBytes const& bytes,
    Dict const* resources, int depth = 0);

// The full key for an inline image's abbreviated one ("W" → "Width").
std::string expand_inline_image_key(std::string const& key);
// An inline image's abbreviated values ("RGB" → "DeviceRGB", "Fl" → "FlateDecode").
Object expand_inline_image_value(std::string const& key, Object const& value);

}
