#include "pdf/Image.h"

#include "core/Jpeg.h"
#include "pdf/ColorSpace.h"
#include "pdf/Filters.h"
#include "pdf/ImageCodecs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace sashfold::pdf {

namespace {

// Samples as decoded: `components` values per pixel, each `bits` wide,
// rows padded to whole bytes — or, from a codec, 8 bits always.
struct Samples {
    int width = 0;
    int height = 0;
    int components = 1;
    int bits = 8;
    std::vector<std::uint8_t> data;
    std::shared_ptr<ColorSpace const> codec_space; // what a codec said the samples are, when the dictionary does not
    std::vector<std::uint8_t> codec_alpha; // a JPEG 2000's own alpha (SMaskInData)

    std::size_t row_bytes() const
    {
        return (static_cast<std::size_t>(width) * static_cast<std::size_t>(components) * static_cast<std::size_t>(bits) + 7) / 8;
    }
    // Sample `c` of pixel (x, y) as the raw integer.
    unsigned at(int x, int y, int c) const
    {
        std::size_t const index = static_cast<std::size_t>(x) * static_cast<std::size_t>(components) + static_cast<std::size_t>(c);
        std::size_t const row = static_cast<std::size_t>(y) * row_bytes();
        if (bits == 8) {
            std::size_t const at_byte = row + index;
            return at_byte < data.size() ? data[at_byte] : 0;
        }
        if (bits == 16) {
            std::size_t const at_byte = row + index * 2;
            return at_byte + 1 < data.size() ? (static_cast<unsigned>(data[at_byte]) << 8) | data[at_byte + 1] : 0;
        }
        std::size_t const bit = index * static_cast<std::size_t>(bits);
        std::size_t const at_byte = row + bit / 8;
        if (at_byte >= data.size())
            return 0;
        int const shift = 8 - bits - static_cast<int>(bit % 8);
        return (data[at_byte] >> shift) & ((1u << bits) - 1);
    }
};

std::vector<float> number_list(Document const& doc, Object const& object)
{
    std::vector<float> out;
    for (Object const& item : doc.resolve(object).as_array())
        out.push_back(static_cast<float>(doc.resolve(item).as_number()));
    return out;
}

Object get(Document const& doc, Dict const& dict, char const* key, char const* abbreviation = nullptr)
{
    Object const* found = dict.find(key);
    if (!found && abbreviation)
        found = dict.find(abbreviation);
    return found ? doc.resolve(*found) : Object {};
}

std::optional<Samples> samples_of(Document const& doc, Dict const& dict, ImageBytes const& bytes, bool stencil)
{
    Samples s;
    s.width = static_cast<int>(get(doc, dict, "Width", "W").as_int(0));
    s.height = static_cast<int>(get(doc, dict, "Height", "H").as_int(0));
    s.bits = stencil ? 1 : static_cast<int>(get(doc, dict, "BitsPerComponent", "BPC").as_int(8));
    std::string const& codec = bytes.codec;
    if (codec == "DCTDecode") {
        std::optional<JpegSamples> jpeg = decode_jpeg_samples(bytes.bytes, k_max_image_pixels);
        if (!jpeg)
            return std::nullopt;
        s.width = jpeg->width;
        s.height = jpeg->height;
        s.components = jpeg->components;
        s.bits = 8;
        s.data = std::move(jpeg->samples);
        s.codec_space = s.components == 1 ? ColorSpace::device_gray()
            : s.components == 3           ? ColorSpace::device_rgb()
                                          : ColorSpace::device_cmyk();
        return s;
    }
    if (codec == "JPXDecode") {
        std::optional<CodecPicture> jpx = decode_jpx(bytes.bytes, k_max_image_pixels);
        if (!jpx)
            return std::nullopt;
        s.width = jpx->width;
        s.height = jpx->height;
        s.components = jpx->components;
        s.bits = 8;
        s.data = std::move(jpx->samples);
        s.codec_alpha = std::move(jpx->alpha);
        s.codec_space = s.components == 1 ? ColorSpace::device_gray()
            : s.components == 3           ? ColorSpace::device_rgb()
                                          : ColorSpace::device_cmyk();
        return s;
    }
    if (s.width <= 0 || s.height <= 0 || static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height) > k_max_image_pixels)
        return std::nullopt;
    if (codec == "CCITTFaxDecode") {
        std::optional<std::vector<std::uint8_t>> bits = decode_ccitt(bytes.bytes, bytes.codec_params, s.width, s.height);
        if (!bits)
            return std::nullopt;
        s.components = 1;
        s.bits = 1;
        s.data = std::move(*bits);
        return s;
    }
    if (codec == "JBIG2Decode") {
        std::vector<std::uint8_t> globals;
        Object const& globals_ref = bytes.codec_params.get("JBIG2Globals");
        if (Stream const* g = doc.resolve(globals_ref).as_stream())
            globals = doc.decode(*g);
        std::optional<std::vector<std::uint8_t>> bits = decode_jbig2(bytes.bytes, globals, s.width, s.height);
        if (!bits)
            return std::nullopt;
        s.components = 1;
        s.bits = 1;
        s.data = std::move(*bits);
        return s;
    }
    if (!codec.empty())
        return std::nullopt;
    if (s.bits != 1 && s.bits != 2 && s.bits != 4 && s.bits != 8 && s.bits != 16)
        return std::nullopt;
    s.data = bytes.bytes;
    return s;
}

// A mask (stencil or soft) as one byte of coverage per pixel at its own size.
struct Coverage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> values;
    std::array<float, 32> matte {};
    bool has_matte = false;
};

std::uint8_t sample_coverage(Coverage const& mask, int x, int y, int width, int height)
{
    // Nearest sample of the mask for pixel (x, y) of a width x height grid.
    int const mx = std::min(mask.width - 1, static_cast<int>((static_cast<long long>(x) * mask.width + mask.width / 2) / width));
    int const my = std::min(mask.height - 1, static_cast<int>((static_cast<long long>(y) * mask.height + mask.height / 2) / height));
    return mask.values[static_cast<std::size_t>(my) * static_cast<std::size_t>(mask.width) + static_cast<std::size_t>(mx)];
}

std::optional<Coverage> stencil_coverage(Document const& doc, Stream const& stream, int depth)
{
    if (depth > 2)
        return std::nullopt;
    ImageBytes const bytes = doc.decode_image(stream);
    std::optional<Samples> const s = samples_of(doc, stream.dict, bytes, true);
    if (!s)
        return std::nullopt;
    std::vector<float> const decode = number_list(doc, stream.dict.get("Decode"));
    bool const inverted = decode.size() >= 2 && decode[0] > decode[1];
    Coverage c;
    c.width = s->width;
    c.height = s->height;
    c.values.resize(static_cast<std::size_t>(c.width) * static_cast<std::size_t>(c.height));
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            // An explicit mask: sample 1 masks out (with /Decode [0 1]); its pixel is not painted.
            bool const masked = (s->at(x, y, 0) != 0) != inverted;
            c.values[static_cast<std::size_t>(y) * static_cast<std::size_t>(c.width) + static_cast<std::size_t>(x)] = masked ? 0 : 255;
        }
    }
    return c;
}

std::optional<Coverage> soft_coverage(Document const& doc, Stream const& stream, int depth)
{
    if (depth > 2)
        return std::nullopt;
    ImageBytes const bytes = doc.decode_image(stream);
    std::optional<Samples> const s = samples_of(doc, stream.dict, bytes, false);
    if (!s || s->components != 1)
        return std::nullopt;
    std::vector<float> const decode = number_list(doc, stream.dict.get("Decode"));
    float const low = decode.size() >= 2 ? decode[0] : 0;
    float const high = decode.size() >= 2 ? decode[1] : 1;
    float const max = static_cast<float>((1u << s->bits) - 1);
    Coverage c;
    c.width = s->width;
    c.height = s->height;
    c.values.resize(static_cast<std::size_t>(c.width) * static_cast<std::size_t>(c.height));
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            float const v = low + static_cast<float>(s->at(x, y, 0)) / max * (high - low);
            c.values[static_cast<std::size_t>(y) * static_cast<std::size_t>(c.width) + static_cast<std::size_t>(x)]
                = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
        }
    }
    std::vector<float> const matte = number_list(doc, stream.dict.get("Matte"));
    if (!matte.empty() && matte.size() <= 32) {
        c.has_matte = true;
        std::copy(matte.begin(), matte.end(), c.matte.begin());
    }
    return c;
}

} // namespace

std::string expand_inline_image_key(std::string const& key)
{
    static std::map<std::string, std::string> const names = {
        { "BPC", "BitsPerComponent" }, { "CS", "ColorSpace" }, { "D", "Decode" }, { "DP", "DecodeParms" },
        { "F", "Filter" }, { "H", "Height" }, { "IM", "ImageMask" }, { "I", "Interpolate" }, { "W", "Width" },
        { "L", "Length" },
    };
    auto const it = names.find(key);
    return it == names.end() ? key : it->second;
}

Object expand_inline_image_value(std::string const& key, Object const& value)
{
    auto const expand_name = [&](Object const& v) -> Object {
        if (!v.is_name())
            return v;
        std::string const& n = v.as_name();
        if (key == "Filter")
            return Object::name(expand_filter_name(n));
        if (key == "ColorSpace") {
            if (n == "G")
                return Object::name("DeviceGray");
            if (n == "RGB")
                return Object::name("DeviceRGB");
            if (n == "CMYK")
                return Object::name("DeviceCMYK");
            if (n == "I")
                return Object::name("Indexed");
        }
        return v;
    };
    if (value.is_array()) {
        Array items;
        for (Object const& item : value.as_array())
            items.push_back(expand_name(item));
        return Object::array(std::move(items));
    }
    return expand_name(value);
}

std::optional<DecodedImage> decode_image(Document const& doc, Dict const& dict, ImageBytes const& bytes, Dict const* resources,
    int depth)
{
    if (depth > 2)
        return std::nullopt;
    bool const stencil = get(doc, dict, "ImageMask", "IM").as_bool();
    std::optional<Samples> samples = samples_of(doc, dict, bytes, stencil);
    if (!samples || samples->width <= 0 || samples->height <= 0)
        return std::nullopt;
    Samples const& s = *samples;
    std::vector<float> decode = number_list(doc, get(doc, dict, "Decode", "D").is_null() ? Object {} : get(doc, dict, "Decode", "D"));

    DecodedImage out;
    out.stencil = stencil;
    out.interpolate = get(doc, dict, "Interpolate", "I").as_bool();

    if (stencil) {
        bool const inverted = decode.size() >= 2 && decode[0] > decode[1];
        out.surface = canvas::Surface(s.width, s.height);
        for (int y = 0; y < s.height; ++y) {
            for (int x = 0; x < s.width; ++x) {
                // Sample 0 paints (with /Decode [0 1]).
                bool const paints = (s.at(x, y, 0) == 0) != inverted;
                out.surface.pixels[out.surface.offset(x, y) + 3] = paints ? 255 : 0;
            }
        }
        return out;
    }

    std::shared_ptr<ColorSpace const> space;
    Object const space_object = get(doc, dict, "ColorSpace", "CS");
    if (!space_object.is_null())
        space = ColorSpace::parse(doc, space_object, resources);
    if (!space || (bytes.codec == "JPXDecode" && space->components() != s.components))
        space = s.codec_space;
    if (!space) {
        space = s.components == 1 ? ColorSpace::device_gray()
            : s.components == 3   ? ColorSpace::device_rgb()
                                  : s.components == 4 ? ColorSpace::device_cmyk()
                                                      : nullptr;
    }
    if (!space || space->components() < 1 || space->family() == ColorSpace::Family::Pattern)
        return std::nullopt;
    Samples raw = std::move(*samples);
    int const n = space->components();
    if (raw.components != n) {
        // A codec gave more or fewer components than the space takes: the
        // codec's own space then (a JPEG's CMYK under a mislabeled RGB).
        if (raw.codec_space && raw.codec_space->components() == raw.components) {
            space = raw.codec_space;
        } else if (bytes.codec.empty()) {
            raw.components = n; // the samples are read as the space says
        } else {
            return std::nullopt;
        }
    }
    int const comps = space->components();
    float const max = static_cast<float>((1u << raw.bits) - 1);
    std::array<float, 64> lows {};
    std::array<float, 64> highs {};
    for (int c = 0; c < comps; ++c) {
        if (static_cast<int>(decode.size()) >= 2 * comps) {
            lows[static_cast<std::size_t>(c)] = decode[static_cast<std::size_t>(2 * c)];
            highs[static_cast<std::size_t>(c)] = decode[static_cast<std::size_t>(2 * c + 1)];
        } else {
            space->default_decode(raw.bits, c, lows[static_cast<std::size_t>(c)], highs[static_cast<std::size_t>(c)]);
        }
    }

    // Masks first: a color key over the raw samples, or an explicit or soft mask.
    std::vector<int> color_key;
    std::optional<Coverage> mask;
    Object const mask_object = get(doc, dict, "Mask");
    if (mask_object.is_array()) {
        for (Object const& v : mask_object.as_array())
            color_key.push_back(static_cast<int>(doc.resolve(v).as_int(-1)));
        if (static_cast<int>(color_key.size()) < 2 * comps)
            color_key.clear();
    } else if (Stream const* mask_stream = mask_object.as_stream()) {
        mask = stencil_coverage(doc, *mask_stream, depth + 1);
    }
    Object const soft = get(doc, dict, "SMask");
    if (Stream const* soft_stream = soft.as_stream())
        mask = soft_coverage(doc, *soft_stream, depth + 1);
    bool const alpha_in_data = !raw.codec_alpha.empty() && get(doc, dict, "SMaskInData").as_int(0) != 0 && !mask;

    // The output at the finer of the image's and the mask's sizes.
    int const width = mask ? std::max(raw.width, mask->width) : raw.width;
    int const height = mask ? std::max(raw.height, mask->height) : raw.height;
    if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > k_max_image_pixels)
        return std::nullopt;
    out.surface = canvas::Surface(width, height);

    // A lookup for one component of eight bits or fewer: every possible
    // sample turned to RGB once. Otherwise per pixel, with the last color kept.
    bool const lookup = comps == 1 && raw.bits <= 8;
    std::vector<std::array<std::uint8_t, 3>> table;
    if (lookup) {
        table.resize(static_cast<std::size_t>(1u << raw.bits));
        for (std::size_t v = 0; v < table.size(); ++v) {
            float const in = lows[0] + static_cast<float>(v) / max * (highs[0] - lows[0]);
            float rgb[3];
            space->to_rgb(&in, rgb);
            for (int k = 0; k < 3; ++k)
                table[v][static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::lround(rgb[k] * 255));
        }
    }
    bool const plain_rgb = space->family() == ColorSpace::Family::RGB && raw.bits == 8 && lows[0] == 0 && highs[0] == 1
        && lows[1] == 0 && highs[1] == 1 && lows[2] == 0 && highs[2] == 1;
    std::array<unsigned, 32> last_raw {};
    std::array<std::uint8_t, 3> last_rgb {};
    bool have_last = false;
    for (int y = 0; y < height; ++y) {
        int const sy = height == raw.height ? y : std::min(raw.height - 1, static_cast<int>(static_cast<long long>(y) * raw.height / height));
        for (int x = 0; x < width; ++x) {
            int const sx = width == raw.width ? x : std::min(raw.width - 1, static_cast<int>(static_cast<long long>(x) * raw.width / width));
            std::array<std::uint8_t, 3> rgb {};
            std::array<unsigned, 32> values {};
            for (int c = 0; c < comps; ++c)
                values[static_cast<std::size_t>(c)] = raw.at(sx, sy, c);
            if (lookup) {
                rgb = table[values[0]];
            } else if (plain_rgb) {
                rgb = { static_cast<std::uint8_t>(values[0]), static_cast<std::uint8_t>(values[1]), static_cast<std::uint8_t>(values[2]) };
            } else if (have_last && std::equal(values.begin(), values.begin() + comps, last_raw.begin())) {
                rgb = last_rgb;
            } else {
                std::array<float, 32> in {};
                for (int c = 0; c < comps; ++c)
                    in[static_cast<std::size_t>(c)] = lows[static_cast<std::size_t>(c)]
                        + static_cast<float>(values[static_cast<std::size_t>(c)]) / max * (highs[static_cast<std::size_t>(c)] - lows[static_cast<std::size_t>(c)]);
                float out_rgb[3];
                space->to_rgb(in.data(), out_rgb);
                for (int k = 0; k < 3; ++k)
                    rgb[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::lround(std::clamp(out_rgb[k], 0.0f, 1.0f) * 255));
                last_raw = values;
                last_rgb = rgb;
                have_last = true;
            }
            int alpha = 255;
            if (!color_key.empty()) {
                bool inside = true;
                for (int c = 0; c < comps && inside; ++c) {
                    int const v = static_cast<int>(values[static_cast<std::size_t>(c)]);
                    inside = v >= color_key[static_cast<std::size_t>(2 * c)] && v <= color_key[static_cast<std::size_t>(2 * c + 1)];
                }
                if (inside)
                    alpha = 0;
            }
            if (mask) {
                alpha = alpha * sample_coverage(*mask, x, y, width, height) / 255;
                if (mask->has_matte && alpha > 0 && alpha < 255) {
                    // The colors were premultiplied against the matte: undo it.
                    float a = static_cast<float>(alpha) / 255;
                    float matte_rgb[3] = { 0, 0, 0 };
                    space->to_rgb(mask->matte.data(), matte_rgb);
                    for (int k = 0; k < 3; ++k) {
                        float const c = static_cast<float>(rgb[static_cast<std::size_t>(k)]) / 255;
                        float const unmatted = matte_rgb[k] + (c - matte_rgb[k]) / a;
                        rgb[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::lround(std::clamp(unmatted, 0.0f, 1.0f) * 255));
                    }
                }
            } else if (alpha_in_data) {
                std::size_t const at = static_cast<std::size_t>(sy) * static_cast<std::size_t>(raw.width) + static_cast<std::size_t>(sx);
                if (at < raw.codec_alpha.size())
                    alpha = alpha * raw.codec_alpha[at] / 255;
            }
            std::uint8_t* const pixel = out.surface.pixels.data() + out.surface.offset(x, y);
            pixel[0] = static_cast<std::uint8_t>((rgb[0] * alpha + 127) / 255);
            pixel[1] = static_cast<std::uint8_t>((rgb[1] * alpha + 127) / 255);
            pixel[2] = static_cast<std::uint8_t>((rgb[2] * alpha + 127) / 255);
            pixel[3] = static_cast<std::uint8_t>(alpha);
        }
    }
    return out;
}

}
