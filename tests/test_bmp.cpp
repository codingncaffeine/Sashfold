#include "Test.h"

#include "core/Bitmap.h"
#include "core/Bmp.h"
#include "core/Png.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The BMP and ICO decoder over pictures written byte by byte here: every
// depth, both row orders, the core and the info headers, the masks, both
// run-length forms, an icon directory of DIB and PNG entries chosen by
// size, the AND mask, and input cut short or bent out of shape.

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(Bytes& out, std::uint32_t v)
{
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

void append(Bytes& out, Bytes const& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

// A BITMAPINFOHEADER (40 bytes) with the given fields, or a core header (12).
Bytes info_header(int width, int height, int bpp, std::uint32_t compression, std::uint32_t colors_used = 0)
{
    Bytes out;
    put32(out, 40);
    put32(out, static_cast<std::uint32_t>(width));
    put32(out, static_cast<std::uint32_t>(height));
    put16(out, 1);
    put16(out, static_cast<std::uint32_t>(bpp));
    put32(out, compression);
    put32(out, 0);
    put32(out, 2835);
    put32(out, 2835);
    put32(out, colors_used);
    put32(out, 0);
    return out;
}

Bytes core_header(int width, int height, int bpp)
{
    Bytes out;
    put32(out, 12);
    put16(out, static_cast<std::uint32_t>(width));
    put16(out, static_cast<std::uint32_t>(height));
    put16(out, 1);
    put16(out, static_cast<std::uint32_t>(bpp));
    return out;
}

// A BMP file around a DIB: the file header names where the pixels start.
Bytes bmp_file(Bytes const& dib_header, Bytes const& after_header, Bytes const& pixels)
{
    Bytes out;
    out.push_back('B');
    out.push_back('M');
    std::uint32_t const pixels_at = 14 + static_cast<std::uint32_t>(dib_header.size() + after_header.size());
    put32(out, pixels_at + static_cast<std::uint32_t>(pixels.size()));
    put32(out, 0);
    put32(out, pixels_at);
    append(out, dib_header);
    append(out, after_header);
    append(out, pixels);
    return out;
}

Bytes palette(std::vector<Color> const& colors, bool core = false)
{
    Bytes out;
    for (Color const& c : colors) {
        out.push_back(c.b);
        out.push_back(c.g);
        out.push_back(c.r);
        if (!core)
            out.push_back(0);
    }
    return out;
}

Bytes padded_rows(std::vector<Bytes> const& rows)
{
    Bytes out;
    for (Bytes const& row : rows) {
        append(out, row);
        while (out.size() % 4 != 0)
            out.push_back(0);
    }
    return out;
}

std::string pixel(Bitmap const& bitmap, int x, int y)
{
    Color const c = bitmap.pixel(x, y);
    return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a);
}

Color const red = Color::rgb(255, 0, 0);
Color const green = Color::rgb(0, 255, 0);
Color const blue = Color::rgb(0, 0, 255);
Color const white = Color::rgb(255, 255, 255);
Color const black = Color::rgb(0, 0, 0);

void test_depths_and_orders()
{
    // 24-bit, bottom-up: the file's first row is the picture's last.
    Bytes const bottom_up = bmp_file(info_header(2, 2, 24, 0), {},
        padded_rows({ { 0, 0, 255, 0, 255, 0 }, { 255, 0, 0, 255, 255, 255 } }));
    CHECK(looks_like_bmp(bottom_up));
    std::optional<Bitmap> picture = decode_bmp(bottom_up);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(picture->width(), 2);
        CHECK_EQ(picture->height(), 2);
        CHECK_EQ(pixel(*picture, 0, 0), "0,0,255,255");
        CHECK_EQ(pixel(*picture, 1, 0), "255,255,255,255");
        CHECK_EQ(pixel(*picture, 0, 1), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 1, 1), "0,255,0,255");
    }
    // The same rows top-down (a negative height) read the other way round.
    Bytes const top_down = bmp_file(info_header(2, -2, 24, 0), {},
        padded_rows({ { 0, 0, 255, 0, 255, 0 }, { 255, 0, 0, 255, 255, 255 } }));
    picture = decode_bmp(top_down);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 1, 0), "0,255,0,255");
        CHECK_EQ(pixel(*picture, 0, 1), "0,0,255,255");
        CHECK_EQ(pixel(*picture, 1, 1), "255,255,255,255");
    }
    // 8-bit through a palette; 4-bit two to a byte; 1-bit eight to a byte.
    Bytes const eight = bmp_file(info_header(4, 1, 8, 0, 4), palette({ black, white, red, blue }), padded_rows({ { 0, 1, 2, 3 } }));
    picture = decode_bmp(eight);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "0,0,0,255");
        CHECK_EQ(pixel(*picture, 1, 0), "255,255,255,255");
        CHECK_EQ(pixel(*picture, 2, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 3, 0), "0,0,255,255");
    }
    Bytes const four = bmp_file(info_header(3, 1, 4, 0), palette({ black, white, red, blue, green, green, green, green, green, green, green, green, green, green, green, green }),
        padded_rows({ { 0x12, 0x30 } }));
    picture = decode_bmp(four);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,255,255,255");
        CHECK_EQ(pixel(*picture, 1, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 2, 0), "0,0,255,255");
    }
    Bytes const one = bmp_file(info_header(8, 1, 1, 0), palette({ black, white }), padded_rows({ { 0xA5 } }));
    picture = decode_bmp(one);
    CHECK(picture.has_value());
    if (picture) {
        std::string bits;
        for (int x = 0; x < 8; ++x)
            bits += picture->pixel(x, 0) == white ? '1' : '0';
        CHECK_EQ(bits, "10100101");
    }
    // 16-bit with the default 5-5-5 layout: each channel's top value is 255.
    Bytes sixteen_rows;
    put16(sixteen_rows, 0x7C00);
    put16(sixteen_rows, 0x03E0);
    put16(sixteen_rows, 0x001F);
    Bytes const sixteen = bmp_file(info_header(3, 1, 16, 0), {}, padded_rows({ sixteen_rows }));
    picture = decode_bmp(sixteen);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 1, 0), "0,255,0,255");
        CHECK_EQ(pixel(*picture, 2, 0), "0,0,255,255");
    }
    // The core header, 24-bit, one pixel.
    Bytes const core = bmp_file(core_header(1, 1, 24), {}, padded_rows({ { 0, 0, 255 } }));
    picture = decode_bmp(core);
    CHECK(picture.has_value());
    if (picture)
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,255");
}

void test_masks_and_alpha()
{
    // 32-bit with BITFIELDS and an alpha mask (compression 6): the masks say
    // where each channel lives, and the alpha is read.
    Bytes masks;
    put32(masks, 0x00FF0000);
    put32(masks, 0x0000FF00);
    put32(masks, 0x000000FF);
    put32(masks, 0xFF000000);
    Bytes row;
    put32(row, 0x80FF0000);
    put32(row, 0xFF00FF00);
    Bytes const masked = bmp_file(info_header(2, -1, 32, 6), masks, padded_rows({ row }));
    std::optional<Bitmap> picture = decode_bmp(masked);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,128");
        CHECK_EQ(pixel(*picture, 1, 0), "0,255,0,255");
    }
    // 32-bit BI_RGB whose fourth bytes are all zero is opaque: a writer that
    // never set alpha did not mean an invisible picture.
    Bytes plain_row;
    put32(plain_row, 0x00FF0000);
    put32(plain_row, 0x000000FF);
    Bytes const plain = bmp_file(info_header(2, 1, 32, 0), {}, padded_rows({ plain_row }));
    picture = decode_bmp(plain);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 1, 0), "0,0,255,255");
    }
    // ... while one with any alpha set keeps its alpha.
    Bytes alpha_row;
    put32(alpha_row, 0x40FF0000);
    put32(alpha_row, 0x000000FF);
    Bytes const with_alpha = bmp_file(info_header(2, 1, 32, 0), {}, padded_rows({ alpha_row }));
    picture = decode_bmp(with_alpha);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,64");
        CHECK_EQ(pixel(*picture, 1, 0), "0,0,255,0");
    }
    // 16-bit BITFIELDS in 5-6-5.
    Bytes masks565;
    put32(masks565, 0xF800);
    put32(masks565, 0x07E0);
    put32(masks565, 0x001F);
    Bytes row565;
    put16(row565, 0x07E0);
    Bytes const rgb565 = bmp_file(info_header(1, 1, 16, 3), masks565, padded_rows({ row565 }));
    picture = decode_bmp(rgb565);
    CHECK(picture.has_value());
    if (picture)
        CHECK_EQ(pixel(*picture, 0, 0), "0,255,0,255");
}

void test_run_lengths()
{
    // RLE8, 4 by 2, bottom-up: a run, a literal, an end of line, an absolute
    // run padded to a word, an end of bitmap.
    Bytes encoded = { 3, 1, 1, 2, 0, 0, 0, 4, 1, 2, 3, 0, 0, 0, 0, 1 };
    Bytes const rle8 = bmp_file(info_header(4, 2, 8, 1, 4), palette({ black, white, red, blue }), encoded);
    std::optional<Bitmap> picture = decode_bmp(rle8);
    CHECK(picture.has_value());
    if (picture) {
        // The first encoded row is the bottom one.
        CHECK_EQ(pixel(*picture, 0, 1), "255,255,255,255");
        CHECK_EQ(pixel(*picture, 2, 1), "255,255,255,255");
        CHECK_EQ(pixel(*picture, 3, 1), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 0, 0), "255,255,255,255");
        CHECK_EQ(pixel(*picture, 1, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 2, 0), "0,0,255,255");
        CHECK_EQ(pixel(*picture, 3, 0), "0,0,0,255");
    }
    // RLE4: a run of two indices alternating, then a delta that skips a pixel
    // (left at index 0), then the end.
    Bytes encoded4 = { 4, 0x12, 0, 2, 1, 0, 1, 0x30, 0, 1 };
    Bytes const rle4 = bmp_file(info_header(6, 1, 4, 2, 4), palette({ black, white, red, blue }), encoded4);
    picture = decode_bmp(rle4);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,255,255,255");
        CHECK_EQ(pixel(*picture, 1, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 3, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 4, 0), "0,0,0,255");
        CHECK_EQ(pixel(*picture, 5, 0), "0,0,255,255");
    }
}

// An icon directory over the given entries: each its dimensions, its bit
// depth and its bytes.
struct IconEntry {
    int width;
    int height;
    int bpp;
    Bytes data;
};

Bytes ico_file(std::vector<IconEntry> const& entries)
{
    Bytes out;
    put16(out, 0);
    put16(out, 1);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    std::uint32_t offset = 6 + 16 * static_cast<std::uint32_t>(entries.size());
    for (IconEntry const& entry : entries) {
        out.push_back(static_cast<std::uint8_t>(entry.width == 256 ? 0 : entry.width));
        out.push_back(static_cast<std::uint8_t>(entry.height == 256 ? 0 : entry.height));
        out.push_back(0);
        out.push_back(0);
        put16(out, 1);
        put16(out, static_cast<std::uint32_t>(entry.bpp));
        put32(out, static_cast<std::uint32_t>(entry.data.size()));
        put32(out, offset);
        offset += static_cast<std::uint32_t>(entry.data.size());
    }
    for (IconEntry const& entry : entries)
        append(out, entry.data);
    return out;
}

// A 32-bit DIB entry of one colour, `size` square, with the given pixel
// made transparent through its alpha, and an AND mask marking nothing.
Bytes dib_entry_32(int size, Color color, int clear_x, int clear_y)
{
    Bytes out = info_header(size, size * 2, 32, 0);
    for (int file_row = 0; file_row < size; ++file_row) {
        int const y = size - 1 - file_row;
        for (int x = 0; x < size; ++x) {
            std::uint32_t const alpha = (x == clear_x && y == clear_y) ? 0u : 255u;
            put32(out, (alpha << 24) | (static_cast<std::uint32_t>(color.r) << 16) | (static_cast<std::uint32_t>(color.g) << 8) | color.b);
        }
    }
    std::size_t const mask_stride = ((static_cast<std::size_t>(size) + 31) / 32) * 4;
    for (int i = 0; i < size; ++i)
        for (std::size_t b = 0; b < mask_stride; ++b)
            out.push_back(0);
    return out;
}

void test_icons()
{
    // Two entries: a 16-pixel DIB with an alpha hole, and a 32-pixel PNG.
    Bitmap big(32, 32, green);
    Bytes const png_entry = encode_png(big);
    Bytes const ico = ico_file({ { 16, 16, 32, dib_entry_32(16, red, 3, 5) }, { 32, 32, 32, png_entry } });
    CHECK(looks_like_ico(ico));
    CHECK(!looks_like_bmp(ico));
    std::optional<Bitmap> picture = decode_ico(ico, 16);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(picture->width(), 16);
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 3, 5), "255,0,0,0");
    }
    // The size asked for picks the entry: exact, the smallest wider one, or
    // the largest when nothing is wide enough; zero takes the largest.
    picture = decode_ico(ico, 32);
    CHECK(picture.has_value() && picture->width() == 32);
    if (picture)
        CHECK_EQ(pixel(*picture, 10, 10), "0,255,0,255");
    picture = decode_ico(ico, 20);
    CHECK(picture.has_value() && picture->width() == 32);
    picture = decode_ico(ico, 64);
    CHECK(picture.has_value() && picture->width() == 32);
    picture = decode_ico(ico, 0);
    CHECK(picture.has_value() && picture->width() == 32);
    picture = decode_ico(ico, 8);
    CHECK(picture.has_value() && picture->width() == 16);

    // A 24-bit entry has no alpha: the AND mask says which pixels are clear.
    Bytes entry = info_header(2, 4, 24, 0);
    append(entry, padded_rows({ { 0, 0, 255, 0, 0, 255 }, { 0, 0, 255, 0, 0, 255 } }));
    entry.push_back(0x40); // the bottom file row: pixel 1 (the picture's (1, 1)) is masked
    entry.push_back(0);
    entry.push_back(0);
    entry.push_back(0);
    entry.push_back(0x00);
    entry.push_back(0);
    entry.push_back(0);
    entry.push_back(0);
    picture = decode_ico(ico_file({ { 2, 2, 24, entry } }), 16);
    CHECK(picture.has_value());
    if (picture) {
        CHECK_EQ(pixel(*picture, 0, 0), "255,0,0,255");
        CHECK_EQ(pixel(*picture, 1, 1), "0,0,0,0");
        CHECK_EQ(pixel(*picture, 0, 1), "255,0,0,255");
    }
    // An entry whose bytes lie outside the file is skipped for one that is inside.
    Bytes lying = ico_file({ { 16, 16, 32, dib_entry_32(16, blue, -1, -1) } });
    lying[6 + 12] = 0xFF; // the first entry's offset, sent past the end
    lying[6 + 13] = 0xFF;
    CHECK(!decode_ico(lying).has_value());
}

void test_malformed()
{
    CHECK(!decode_bmp({}).has_value());
    CHECK(!looks_like_bmp({ 'B', 'M' }));
    Bytes const good = bmp_file(info_header(2, 2, 24, 0), {}, padded_rows({ { 0, 0, 255, 0, 255, 0 }, { 255, 0, 0, 255, 255, 255 } }));
    // Cut short anywhere: nothing.
    for (std::size_t cut : { std::size_t(13), std::size_t(20), std::size_t(53), good.size() - 1 })
        CHECK(!decode_bmp(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut))).has_value());
    // A depth that is not one, a width of zero, a header size no one uses, a
    // pixel offset before the palette, a picture past the limit.
    CHECK(!decode_bmp(bmp_file(info_header(2, 2, 12, 0), {}, Bytes(16, 0))).has_value());
    CHECK(!decode_bmp(bmp_file(info_header(0, 2, 24, 0), {}, Bytes(16, 0))).has_value());
    Bytes odd_header = info_header(2, 2, 24, 0);
    odd_header[0] = 41;
    CHECK(!decode_bmp(bmp_file(odd_header, {}, Bytes(16, 0))).has_value());
    Bytes early = bmp_file(info_header(4, 1, 8, 0, 4), palette({ black, white, red, blue }), padded_rows({ { 0, 1, 2, 3 } }));
    early[10] = 14;
    CHECK(!decode_bmp(early).has_value());
    CHECK(!decode_bmp(good, 3).has_value());
    CHECK(decode_bmp(good, 4).has_value());
    // A run-length picture that ends early stands as far as it got.
    Bytes const short_rle = bmp_file(info_header(4, 2, 8, 1, 4), palette({ black, white, red, blue }), { 2, 2 });
    std::optional<Bitmap> const partial = decode_bmp(short_rle);
    CHECK(partial.has_value());
    if (partial) {
        CHECK_EQ(pixel(*partial, 0, 1), "255,0,0,255");
        CHECK_EQ(pixel(*partial, 2, 1), "0,0,0,255");
    }
    CHECK(!looks_like_ico({ 0, 0, 3, 0, 1, 0 }));
    CHECK(!decode_ico({ 0, 0, 1, 0, 1, 0 }).has_value());
}

} // namespace

int main()
{
    test_depths_and_orders();
    test_masks_and_alpha();
    test_run_lengths();
    test_icons();
    test_malformed();
    return sashfold::test::report("bmp");
}
