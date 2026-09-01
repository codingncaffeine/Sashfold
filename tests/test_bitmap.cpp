#include "Test.h"

#include "core/Bitmap.h"

using namespace sashfold;

int main()
{
    // Construction fills every pixel.
    Bitmap bitmap(4, 3, Color::rgb(10, 20, 30));
    CHECK_EQ(bitmap.width(), 4);
    CHECK_EQ(bitmap.height(), 3);
    CHECK_EQ(bitmap.pixels().size(), std::size_t { 4 * 3 * 4 });
    CHECK(bitmap.pixel(0, 0) == Color::rgb(10, 20, 30));
    CHECK(bitmap.pixel(3, 2) == Color::rgb(10, 20, 30));

    // Bounds.
    CHECK(bitmap.contains(0, 0));
    CHECK(bitmap.contains(3, 2));
    CHECK(!bitmap.contains(4, 0));
    CHECK(!bitmap.contains(0, 3));
    CHECK(!bitmap.contains(-1, 0));

    // Out-of-bounds writes are dropped rather than corrupting a neighbour.
    bitmap.set_pixel(-1, 0, Color::rgb(1, 2, 3));
    bitmap.set_pixel(4, 0, Color::rgb(1, 2, 3));
    bitmap.set_pixel(0, -1, Color::rgb(1, 2, 3));
    CHECK(bitmap.pixel(0, 0) == Color::rgb(10, 20, 30));
    CHECK(bitmap.pixel(3, 0) == Color::rgb(10, 20, 30));

    // fill_rect clips to the surface instead of wrapping.
    Bitmap clipped(4, 4, Color::rgb(0, 0, 0));
    clipped.fill_rect(Rect { 2, 2, 10, 10 }, Color::rgb(255, 255, 255));
    CHECK(clipped.pixel(3, 3) == Color::rgb(255, 255, 255));
    CHECK(clipped.pixel(2, 2) == Color::rgb(255, 255, 255));
    CHECK(clipped.pixel(1, 1) == Color::rgb(0, 0, 0));

    clipped.fill_rect(Rect { -2, -2, 4, 4 }, Color::rgb(9, 9, 9));
    CHECK(clipped.pixel(0, 0) == Color::rgb(9, 9, 9));
    CHECK(clipped.pixel(1, 1) == Color::rgb(9, 9, 9));
    CHECK(clipped.pixel(2, 2) == Color::rgb(255, 255, 255));

    // Degenerate rects paint nothing.
    Bitmap untouched(2, 2, Color::rgb(7, 7, 7));
    untouched.fill_rect(Rect { 0, 0, 0, 5 }, Color::rgb(1, 1, 1));
    untouched.fill_rect(Rect { 0, 0, 5, -1 }, Color::rgb(1, 1, 1));
    CHECK(untouched.pixel(0, 0) == Color::rgb(7, 7, 7));

    // Source-over compositing.
    Bitmap blended(3, 1, Color::rgb(255, 255, 255));

    blended.blend_pixel(0, 0, Color::rgba(255, 0, 0, 0)); // fully transparent: no change
    CHECK(blended.pixel(0, 0) == Color::rgb(255, 255, 255));

    blended.blend_pixel(1, 0, Color::rgba(255, 0, 0, 255)); // fully opaque: replace
    CHECK(blended.pixel(1, 0) == Color::rgb(255, 0, 0));

    // Half-transparent red over opaque white lands midway on green and blue and
    // leaves the surface opaque.
    blended.blend_pixel(2, 0, Color::rgba(255, 0, 0, 128));
    Color const mixed = blended.pixel(2, 0);
    CHECK_EQ(static_cast<int>(mixed.a), 255);
    CHECK_EQ(static_cast<int>(mixed.r), 255);
    CHECK_EQ(static_cast<int>(mixed.g), 127);
    CHECK_EQ(static_cast<int>(mixed.b), 127);

    // Compositing onto a fully transparent surface preserves the source colour
    // rather than dragging it toward the cleared black underneath.
    Bitmap transparent(1, 1, Color::rgba(0, 0, 0, 0));
    transparent.blend_pixel(0, 0, Color::rgba(200, 100, 50, 128));
    Color const over_nothing = transparent.pixel(0, 0);
    CHECK_EQ(static_cast<int>(over_nothing.a), 128);
    CHECK_EQ(static_cast<int>(over_nothing.r), 200);
    CHECK_EQ(static_cast<int>(over_nothing.g), 100);
    CHECK_EQ(static_cast<int>(over_nothing.b), 50);

    return sashfold::test::report("bitmap");
}
