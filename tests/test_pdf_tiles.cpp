// A zoomed PDF page drawn a tile at a time: the tile math at 6400% on an
// A0 page (a picture of 152576 x 215680 pixels, which no bitmap holds), the
// tiles drawn with only their own pixels, and the viewer's tile cache
// letting the least recently used go past its byte cap.

#include "Test.h"

#include "core/Bitmap.h"
#include "pdf/Document.h"
#include "pdf/Render.h"
#include "ui/PdfTileCache.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

std::vector<std::uint8_t> a0_file()
{
    // A0 (2384 x 3370 pt): a red point-square at (1000, 1000), a blue one in the top right corner.
    std::string const content = "1 0 0 rg 1000 1000 1 1 re f 0 0 1 rg 2383 3369 1 1 re f";
    std::vector<std::string> const objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 2384 3370] /Contents 4 0 R >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
    };
    std::string out = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    std::size_t const xref = out.size();
    out += "xref\n0 5\n0000000000 65535 f \n";
    for (std::size_t offset : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
        out += line;
    }
    out += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return { out.begin(), out.end() };
}

bool is(Bitmap const& b, int x, int y, int r, int g, int bl)
{
    Color const c = b.pixel(x, y);
    bool const ok = c.r == r && c.g == g && c.b == bl;
    if (!ok)
        std::fprintf(stderr, "  (%d, %d) is %d %d %d, wanted %d %d %d\n", x, y, c.r, c.g, c.b, r, g, bl);
    return ok;
}

void test_tile_math()
{
    pdf::Document::Status status {};
    auto const doc = pdf::Document::open(a0_file(), "", status);
    if (!CHECK(doc != nullptr))
        return;
    pdf::Page const& page = doc->page(0);
    pdf::LargePixelSize const size = pdf::page_pixel_extent(page, 64);
    CHECK_EQ(size.width, 152576);
    CHECK_EQ(size.height, 215680);
    // Turned a quarter: the sides swap.
    pdf::LargePixelSize const turned = pdf::page_pixel_extent(page, 64, 90);
    CHECK(turned.width == 215680 && turned.height == 152576);
    // 298 columns exactly; 421 whole rows and one of 128.
    auto const last_column = pdf::tile_rect(size, 512, 297, 0);
    CHECK(last_column && last_column->x == 152064 && last_column->width == 512 && last_column->height == 512);
    auto const last_row = pdf::tile_rect(size, 512, 0, 421);
    CHECK(last_row && last_row->y == 215552 && last_row->height == 128);
    CHECK(!pdf::tile_rect(size, 512, 298, 0));
    CHECK(!pdf::tile_rect(size, 512, 0, 422));
    CHECK(!pdf::tile_rect(size, 512, -1, 0));
    CHECK(!pdf::tile_rect(size, 0, 0, 0));
    CHECK(!pdf::tile_rect(size, 512, std::int64_t { 1 } << 60, 0));
    // The whole picture is far too large to draw; a tile of it is not.
    pdf::RenderOptions options;
    options.scale = 64;
    CHECK(!pdf::render_page(*doc, 0, options).has_value());
    // The red square: x from 64000 (column 125), y from (3370 - 1001) * 64 = 151616 (row 296, 64 down).
    auto const start = std::chrono::steady_clock::now();
    std::optional<Bitmap> const red_tile = pdf::render_page_region(*doc, 0, options, *pdf::tile_rect(size, 512, 125, 296));
    double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (CHECK(red_tile.has_value())) {
        CHECK(red_tile->width() == 512 && red_tile->height() == 512);
        CHECK(is(*red_tile, 10, 100, 255, 0, 0));
        CHECK(is(*red_tile, 63, 127, 255, 0, 0));
        CHECK(is(*red_tile, 64, 100, 255, 255, 255));
        CHECK(is(*red_tile, 10, 63, 255, 255, 255));
        CHECK(is(*red_tile, 10, 128, 255, 255, 255));
    }
    std::fprintf(stderr, "  a tile at 6400%% of A0: %.1f ms\n", ms);
    // The blue square in the last column's first row, at its right edge.
    std::optional<Bitmap> const blue_tile = pdf::render_page_region(*doc, 0, options, *last_column);
    if (CHECK(blue_tile.has_value())) {
        CHECK(is(*blue_tile, 500, 10, 0, 0, 255));
        CHECK(is(*blue_tile, 440, 10, 255, 255, 255));
        CHECK(is(*blue_tile, 500, 70, 255, 255, 255));
    }
    // The same tile drawn whole at 100% agrees with the tiles: the red square at (1000, 2369).
    options.scale = 1;
    std::optional<Bitmap> const whole = pdf::render_page(*doc, 0, options);
    CHECK(whole && is(*whole, 1000, 2369, 255, 0, 0) && is(*whole, 1001, 2369, 255, 255, 255));
}

void test_tile_cache()
{
    ui::PdfTileCache cache(100);
    auto const bytes = [](std::size_t n) { return std::make_shared<std::vector<std::uint8_t> const>(n, std::uint8_t { 7 }); };
    cache.insert("a", bytes(40));
    cache.insert("b", bytes(40));
    CHECK(cache.find("a") != nullptr); // a is now the most recent
    cache.insert("c", bytes(40)); // over 100: b, the least recently used, goes
    CHECK(cache.find("b") == nullptr);
    CHECK(cache.find("a") != nullptr && cache.find("c") != nullptr);
    CHECK_EQ(cache.bytes(), std::size_t { 80 });
    cache.insert("c", bytes(10)); // replaced, counted again
    CHECK_EQ(cache.bytes(), std::size_t { 50 });
    cache.insert("huge", bytes(101)); // larger than the cap: not kept, nothing let go
    CHECK(cache.find("huge") == nullptr);
    CHECK_EQ(cache.count(), std::size_t { 2 });
    for (int i = 0; i < 1000; ++i)
        cache.insert("t" + std::to_string(i), bytes(30));
    CHECK(cache.bytes() <= 100);
    CHECK(cache.find("t999") != nullptr && cache.find("t0") == nullptr);
}

}

int main()
{
    test_tile_math();
    test_tile_cache();
    return sashfold::test::report("test_pdf_tiles");
}
