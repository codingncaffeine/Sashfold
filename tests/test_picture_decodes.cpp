#include "Test.h"

#include "core/Bitmap.h"
#include "core/Png.h"
#include "ui/PictureDecodes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Pictures decoded off the page's thread: a picture offered under its name
// is handed back for exactly those bytes and no others, a vector picture is
// left to the page, and offers from several threads at once all land.

using namespace sashfold;

namespace {

std::vector<std::uint8_t> png_of(int width, int height, Color color)
{
    return encode_png(Bitmap(width, height, color));
}

}

int main()
{
    ui::PictureDecodes decodes;
    std::vector<std::uint8_t> const red = png_of(7, 5, Color::rgb(200, 10, 10));
    decodes.offer("https://a.example/red.png", red);

    // The bytes offered, under the name offered: the picture they hold.
    std::optional<ui::PictureDecodes::Decoded> const found = decodes.find("https://a.example/red.png", red);
    CHECK(found.has_value());
    if (found && found->image) {
        CHECK_EQ(found->image->width(), 7);
        CHECK_EQ(found->image->height(), 5);
        CHECK(found->image->pixel(3, 2) == Color::rgb(200, 10, 10));
    } else {
        CHECK(false);
    }
    // Other bytes under that name (a response the page's own request got
    // differently), or the bytes under another name: nothing.
    CHECK(!decodes.find("https://a.example/red.png", png_of(7, 5, Color::rgb(0, 0, 200))).has_value());
    CHECK(!decodes.find("https://a.example/other.png", red).has_value());
    // The first offer of a name stands.
    decodes.offer("https://a.example/red.png", png_of(2, 2, Color::rgb(0, 0, 0)));
    CHECK(decodes.find("https://a.example/red.png", red).has_value());
    CHECK_EQ(decodes.decoded(), std::size_t { 1 });

    // A vector picture is drawn by the page, in its fonts: never decoded here.
    std::string const svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="4" height="4"/></svg>)";
    std::vector<std::uint8_t> const svg_bytes(svg.begin(), svg.end());
    CHECK(!ui::is_raster_picture(svg_bytes));
    decodes.offer("https://a.example/v.svg", svg_bytes);
    CHECK(!decodes.find("https://a.example/v.svg", svg_bytes).has_value());

    // Bytes that say PNG and are not one: found, with no picture, so the
    // page does not decode them again either.
    std::vector<std::uint8_t> broken(red.begin(), red.begin() + 20);
    decodes.offer("https://a.example/broken.png", broken);
    std::optional<ui::PictureDecodes::Decoded> const failed = decodes.find("https://a.example/broken.png", broken);
    CHECK(failed.has_value());
    CHECK(failed && !failed->image);

    // Offered from the loader's threads together.
    ui::PictureDecodes many;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&many, t] {
            for (int i = 0; i < 16; ++i)
                many.offer("p" + std::to_string(t * 16 + i), png_of(3 + i, 3, Color::rgb(static_cast<std::uint8_t>(t * 30), 0, 0)));
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    CHECK_EQ(many.decoded(), std::size_t { 128 });
    std::optional<ui::PictureDecodes::Decoded> const last = many.find("p127", png_of(18, 3, Color::rgb(210, 0, 0)));
    CHECK(last && last->image && last->image->width() == 18);

    return test::report("picture decodes");
}
