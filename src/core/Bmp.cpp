#include "core/Bmp.h"

#include "core/Png.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace sashfold {

namespace {

using View = std::span<std::uint8_t const>;

std::uint16_t read16(View bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

std::uint32_t read32(View bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

bool has(View bytes, std::size_t at, std::size_t count)
{
    return at <= bytes.size() && count <= bytes.size() - at;
}

enum Compression : std::uint32_t { rgb = 0, rle8 = 1, rle4 = 2, bitfields = 3, alpha_bitfields = 6 };

struct Dib {
    bool core = false; // a BITMAPCOREHEADER: 16-bit dimensions, 3-byte palette entries
    int width = 0;
    int height = 0; // the picture's; an ICO entry's header says twice this
    bool top_down = false;
    int bpp = 0;
    std::uint32_t compression = rgb;
    std::uint32_t colors_used = 0;
    std::uint32_t masks[4] = { 0, 0, 0, 0 }; // red, green, blue, alpha; zero = none
    std::size_t palette_at = 0; // after the header and any masks
};

// One channel out of a masked pixel, scaled to eight bits.
struct Channel {
    std::uint32_t mask = 0;
    int shift = 0;
    int bits = 0;

    explicit Channel(std::uint32_t m)
        : mask(m)
    {
        if (m == 0)
            return;
        while (((m >> shift) & 1) == 0)
            ++shift;
        std::uint32_t rest = m >> shift;
        while ((rest & 1) != 0) {
            ++bits;
            rest >>= 1;
        }
    }

    std::uint8_t of(std::uint32_t pixel) const
    {
        if (mask == 0 || bits == 0)
            return 0;
        std::uint32_t const value = (pixel & mask) >> shift;
        if (bits >= 8)
            return static_cast<std::uint8_t>(value >> (bits - 8));
        std::uint32_t const full = (1u << bits) - 1;
        return static_cast<std::uint8_t>((value * 255 + full / 2) / full);
    }
};

std::optional<Dib> read_dib(View bytes, std::size_t at, bool ico_entry)
{
    if (!has(bytes, at, 4))
        return std::nullopt;
    std::uint32_t const header_size = read32(bytes, at);
    if (!has(bytes, at, header_size))
        return std::nullopt;
    Dib dib;
    std::int64_t height = 0;
    if (header_size == 12) {
        dib.core = true;
        dib.width = read16(bytes, at + 4);
        height = read16(bytes, at + 6);
        dib.bpp = read16(bytes, at + 10);
        dib.palette_at = at + 12;
    } else if (header_size == 40 || header_size == 52 || header_size == 56 || header_size == 108 || header_size == 124) {
        dib.width = static_cast<std::int32_t>(read32(bytes, at + 4));
        height = static_cast<std::int32_t>(read32(bytes, at + 8));
        dib.bpp = read16(bytes, at + 14);
        dib.compression = read32(bytes, at + 16);
        dib.colors_used = read32(bytes, at + 32);
        std::size_t masks_at = at + 40;
        int mask_count = 0;
        if (header_size >= 52) {
            // V2 and later carry the masks inside the header (V3 and later the alpha too).
            mask_count = header_size >= 56 ? 4 : 3;
        } else if (dib.compression == bitfields || dib.compression == alpha_bitfields) {
            // A 40-byte header with BITFIELDS puts three (or four) masks right after it.
            mask_count = dib.compression == alpha_bitfields ? 4 : 3;
        }
        if (mask_count > 0) {
            if (!has(bytes, masks_at, static_cast<std::size_t>(mask_count) * 4))
                return std::nullopt;
            for (int i = 0; i < mask_count; ++i)
                dib.masks[i] = read32(bytes, masks_at + static_cast<std::size_t>(i) * 4);
        }
        dib.palette_at = header_size >= 52 ? at + header_size : masks_at + static_cast<std::size_t>(mask_count) * 4;
    } else {
        return std::nullopt;
    }
    if (height < 0) {
        dib.top_down = true;
        height = -height;
    }
    if (ico_entry) {
        if (height % 2 != 0)
            return std::nullopt;
        height /= 2;
    }
    if (dib.width <= 0 || height <= 0 || dib.width > 32767 || height > 32767)
        return std::nullopt;
    dib.height = static_cast<int>(height);
    switch (dib.bpp) {
    case 1:
    case 4:
    case 8:
    case 16:
    case 24:
    case 32:
        break;
    default:
        return std::nullopt;
    }
    if (dib.compression == rle8 && dib.bpp != 8)
        return std::nullopt;
    if (dib.compression == rle4 && dib.bpp != 4)
        return std::nullopt;
    if ((dib.compression == bitfields || dib.compression == alpha_bitfields) && dib.bpp != 16 && dib.bpp != 32)
        return std::nullopt;
    if (dib.compression != rgb && dib.compression != rle8 && dib.compression != rle4 && dib.compression != bitfields
        && dib.compression != alpha_bitfields)
        return std::nullopt;
    return dib;
}

std::size_t row_stride(int width, int bpp)
{
    return ((static_cast<std::size_t>(width) * static_cast<std::size_t>(bpp) + 31) / 32) * 4;
}

// The run-length forms, into one index per pixel (unwritten pixels are 0).
bool decode_rle(View bytes, std::size_t at, Dib const& dib, std::vector<std::uint8_t>& indices)
{
    std::size_t const width = static_cast<std::size_t>(dib.width);
    std::size_t const height = static_cast<std::size_t>(dib.height);
    indices.assign(width * height, 0);
    std::size_t x = 0;
    std::size_t y = 0; // file rows: bottom-up unless top-down
    auto const put = [&](std::uint8_t index) {
        if (x < width && y < height) {
            std::size_t const row = dib.top_down ? y : height - 1 - y;
            indices[row * width + x] = index;
        }
        ++x;
    };
    bool const four = dib.compression == rle4;
    std::size_t p = at;
    while (has(bytes, p, 2)) {
        std::uint8_t const count = bytes[p];
        std::uint8_t const value = bytes[p + 1];
        p += 2;
        if (count > 0) {
            for (std::uint8_t i = 0; i < count; ++i) {
                if (four)
                    put((i % 2 == 0) ? static_cast<std::uint8_t>(value >> 4) : static_cast<std::uint8_t>(value & 0x0F));
                else
                    put(value);
            }
            continue;
        }
        if (value == 0) { // end of line
            x = 0;
            ++y;
            continue;
        }
        if (value == 1) // end of bitmap
            return true;
        if (value == 2) { // delta
            if (!has(bytes, p, 2))
                return false;
            x += bytes[p];
            y += bytes[p + 1];
            p += 2;
            continue;
        }
        // Absolute mode: `value` pixels follow, padded to a 16-bit boundary.
        std::size_t const run_bytes = four ? (static_cast<std::size_t>(value) + 1) / 2 : value;
        std::size_t const padded = (run_bytes + 1) & ~std::size_t(1);
        if (!has(bytes, p, padded))
            return false;
        for (std::size_t i = 0; i < value; ++i) {
            if (four) {
                std::uint8_t const pair = bytes[p + i / 2];
                put((i % 2 == 0) ? static_cast<std::uint8_t>(pair >> 4) : static_cast<std::uint8_t>(pair & 0x0F));
            } else {
                put(bytes[p + i]);
            }
        }
        p += padded;
        if (y >= height && x >= width)
            return true;
    }
    return true; // ran out of bytes: what was written stands, as the readers of the world have it
}

// The pixels of a DIB whose header has been read, from `pixels_at`, into a
// bitmap. For an ICO entry the AND mask follows the pixels and makes the
// pixels it marks transparent when the picture has no alpha of its own.
std::optional<Bitmap> decode_pixels(View bytes, Dib const& dib, std::size_t pixels_at, bool ico_entry, std::size_t max_pixels)
{
    std::size_t const width = static_cast<std::size_t>(dib.width);
    std::size_t const height = static_cast<std::size_t>(dib.height);
    if (width * height > max_pixels)
        return std::nullopt;
    // The palette, for the depths that have one.
    std::vector<Color> palette;
    if (dib.bpp <= 8) {
        std::size_t const entry_size = dib.core ? 3 : 4;
        std::size_t count = dib.colors_used != 0 ? dib.colors_used : (std::size_t(1) << dib.bpp);
        count = std::min(count, std::size_t(1) << dib.bpp);
        if (!has(bytes, dib.palette_at, count * entry_size))
            return std::nullopt;
        palette.resize(std::size_t(1) << dib.bpp, Color::rgb(0, 0, 0));
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t const e = dib.palette_at + i * entry_size;
            palette[i] = Color::rgb(bytes[e + 2], bytes[e + 1], bytes[e]);
        }
    }
    Bitmap out(dib.width, dib.height, Color::rgba(0, 0, 0, 0));
    bool has_alpha = false;
    if (dib.compression == rle8 || dib.compression == rle4) {
        std::vector<std::uint8_t> indices;
        if (!decode_rle(bytes, pixels_at, dib, indices))
            return std::nullopt;
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
                out.set_pixel(static_cast<int>(x), static_cast<int>(y), palette[indices[y * width + x] & ((1u << dib.bpp) - 1)]);
    } else {
        std::size_t const stride = row_stride(dib.width, dib.bpp);
        if (!has(bytes, pixels_at, stride * height))
            return std::nullopt;
        Channel red(0), green(0), blue(0), alpha(0);
        if (dib.bpp == 16 || dib.bpp == 32) {
            bool const masked = dib.compression == bitfields || dib.compression == alpha_bitfields || dib.masks[0] != 0;
            red = Channel(masked ? dib.masks[0] : dib.bpp == 16 ? 0x7C00u : 0x00FF0000u);
            green = Channel(masked ? dib.masks[1] : dib.bpp == 16 ? 0x03E0u : 0x0000FF00u);
            blue = Channel(masked ? dib.masks[2] : dib.bpp == 16 ? 0x001Fu : 0x000000FFu);
            // A 32-bit picture with no alpha mask named still carries a fourth
            // byte, which icons and many writers fill with alpha: it is read
            // as alpha when any pixel has one, else the picture is opaque.
            alpha = Channel(masked && dib.masks[3] != 0 ? dib.masks[3] : dib.bpp == 32 ? 0xFF000000u : 0u);
            has_alpha = alpha.mask != 0;
        }
        bool any_alpha = false;
        for (std::size_t file_row = 0; file_row < height; ++file_row) {
            std::size_t const y = dib.top_down ? file_row : height - 1 - file_row;
            std::size_t const row = pixels_at + file_row * stride;
            for (std::size_t x = 0; x < width; ++x) {
                Color color;
                switch (dib.bpp) {
                case 1:
                    color = palette[(bytes[row + x / 8] >> (7 - x % 8)) & 1];
                    break;
                case 4:
                    color = palette[(x % 2 == 0) ? (bytes[row + x / 2] >> 4) : (bytes[row + x / 2] & 0x0F)];
                    break;
                case 8:
                    color = palette[bytes[row + x]];
                    break;
                case 16: {
                    std::uint32_t const pixel = read16(bytes, row + x * 2);
                    color = Color::rgba(red.of(pixel), green.of(pixel), blue.of(pixel), has_alpha ? alpha.of(pixel) : 255);
                    break;
                }
                case 24:
                    color = Color::rgb(bytes[row + x * 3 + 2], bytes[row + x * 3 + 1], bytes[row + x * 3]);
                    break;
                default: {
                    std::uint32_t const pixel = read32(bytes, row + x * 4);
                    color = Color::rgba(red.of(pixel), green.of(pixel), blue.of(pixel), has_alpha ? alpha.of(pixel) : 255);
                    break;
                }
                }
                if (color.a != 0)
                    any_alpha = true;
                out.set_pixel(static_cast<int>(x), static_cast<int>(y), color);
            }
        }
        if (has_alpha && !any_alpha && dib.bpp == 32) {
            // Every alpha byte is zero: a 32-bit picture written without alpha,
            // which is opaque (an icon's AND mask, below, says where it is not).
            has_alpha = false;
            for (std::size_t y = 0; y < height; ++y)
                for (std::size_t x = 0; x < width; ++x) {
                    Color color = out.pixel(static_cast<int>(x), static_cast<int>(y));
                    color.a = 255;
                    out.set_pixel(static_cast<int>(x), static_cast<int>(y), color);
                }
        }
        pixels_at += stride * height;
    }
    if (ico_entry && !has_alpha) {
        // The AND mask: one bit per pixel, rows padded to four bytes,
        // bottom-up like the pixels; a set bit is a transparent pixel. A
        // mask cut short marks nothing.
        std::size_t const mask_stride = row_stride(dib.width, 1);
        if (has(bytes, pixels_at, mask_stride * height)) {
            for (std::size_t file_row = 0; file_row < height; ++file_row) {
                std::size_t const y = dib.top_down ? file_row : height - 1 - file_row;
                std::size_t const row = pixels_at + file_row * mask_stride;
                for (std::size_t x = 0; x < width; ++x) {
                    if (((bytes[row + x / 8] >> (7 - x % 8)) & 1) != 0)
                        out.set_pixel(static_cast<int>(x), static_cast<int>(y), Color::rgba(0, 0, 0, 0));
                }
            }
        }
    }
    return out;
}

} // namespace

bool looks_like_bmp(std::vector<std::uint8_t> const& bytes)
{
    return bytes.size() >= 14 && bytes[0] == 'B' && bytes[1] == 'M';
}

bool looks_like_ico(std::vector<std::uint8_t> const& bytes)
{
    if (bytes.size() < 6)
        return false;
    View const view(bytes);
    std::uint16_t const type = read16(view, 2);
    std::uint16_t const count = read16(view, 4);
    return read16(view, 0) == 0 && (type == 1 || type == 2) && count >= 1 && count <= 256;
}

std::optional<Bitmap> decode_bmp(std::vector<std::uint8_t> const& bytes, std::size_t max_pixels)
{
    if (!looks_like_bmp(bytes))
        return std::nullopt;
    View const view(bytes);
    std::size_t const pixels_at = read32(view, 10);
    std::optional<Dib> const dib = read_dib(view, 14, false);
    if (!dib)
        return std::nullopt;
    // The file header names where the pixels start; a header that points
    // before the palette's end is not one this decoder reads.
    if (pixels_at < dib->palette_at)
        return std::nullopt;
    return decode_pixels(view, *dib, pixels_at, false, max_pixels);
}

std::optional<Bitmap> decode_ico(std::vector<std::uint8_t> const& bytes, int wanted, std::size_t max_pixels)
{
    if (!looks_like_ico(bytes))
        return std::nullopt;
    View const view(bytes);
    std::uint16_t const count = read16(view, 4);
    if (!has(view, 6, static_cast<std::size_t>(count) * 16))
        return std::nullopt;
    // The entry nearest the size wanted, among the ones the bytes hold.
    std::size_t chosen = count;
    int chosen_width = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t const e = 6 + i * 16;
        int const width = view[e] == 0 ? 256 : view[e];
        std::uint32_t const size = read32(view, e + 8);
        std::uint32_t const offset = read32(view, e + 12);
        if (size < 8 || !has(view, offset, size))
            continue;
        bool better = chosen == count;
        if (!better && wanted > 0) {
            bool const chosen_fits = chosen_width >= wanted;
            bool const fits = width >= wanted;
            better = fits && (!chosen_fits || width < chosen_width);
            if (!fits && !chosen_fits)
                better = width > chosen_width;
        } else if (!better) {
            better = width > chosen_width;
        }
        if (better) {
            chosen = i;
            chosen_width = width;
        }
    }
    if (chosen == count)
        return std::nullopt;
    std::size_t const e = 6 + chosen * 16;
    std::uint32_t const size = read32(view, e + 8);
    std::uint32_t const offset = read32(view, e + 12);
    std::vector<std::uint8_t> const entry(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
    if (looks_like_png(entry))
        return decode_png(entry, max_pixels);
    View const entry_view(entry);
    std::optional<Dib> const dib = read_dib(entry_view, 0, true);
    if (!dib)
        return std::nullopt;
    std::size_t pixels_at = dib->palette_at;
    if (dib->bpp <= 8) {
        std::size_t const entries = dib->colors_used != 0 ? std::min<std::size_t>(dib->colors_used, std::size_t(1) << dib->bpp) : (std::size_t(1) << dib->bpp);
        pixels_at += entries * (dib->core ? 3 : 4);
    }
    return decode_pixels(entry_view, *dib, pixels_at, true, max_pixels);
}

}
