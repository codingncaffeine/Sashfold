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

    // A video's frame, scaled smoothly: at its own size it is copied as it is.
    Bitmap frame(3, 2, Color::rgb(0, 0, 0));
    frame.set_pixel(0, 0, Color::rgb(10, 20, 30));
    frame.set_pixel(1, 0, Color::rgb(40, 50, 60));
    frame.set_pixel(2, 1, Color::rgb(70, 80, 90));
    Bitmap copy(3, 2, Color::rgb(255, 255, 255));
    copy.draw_scaled_opaque(frame, Rect { 0, 0, 3, 2 });
    CHECK(copy.pixels() == frame.pixels());

    // At twice the size each pixel mixes the two its centre falls between:
    // black and white become black, a quarter, three quarters, white.
    Bitmap pair(2, 1, Color::rgb(0, 0, 0));
    pair.set_pixel(1, 0, Color::rgb(255, 255, 255));
    Bitmap wide(4, 1, Color::rgb(255, 0, 0));
    wide.draw_scaled_opaque(pair, Rect { 0, 0, 4, 1 });
    CHECK(wide.pixel(0, 0) == Color::rgb(0, 0, 0));
    CHECK(wide.pixel(1, 0) == Color::rgb(64, 64, 64));
    CHECK(wide.pixel(2, 0) == Color::rgb(191, 191, 191));
    CHECK(wide.pixel(3, 0) == Color::rgb(255, 255, 255));

    // Nothing outside the clip is written.
    Bitmap through(4, 1, Color::rgb(255, 0, 0));
    through.set_clip(Rect { 1, 0, 2, 1 });
    through.draw_scaled_opaque(pair, Rect { 0, 0, 4, 1 });
    CHECK(through.pixel(0, 0) == Color::rgb(255, 0, 0));
    CHECK(through.pixel(1, 0) == Color::rgb(64, 64, 64));
    CHECK(through.pixel(3, 0) == Color::rgb(255, 0, 0));

    // Under a rounded clip the inside is written, the corner outside the
    // curve is left, and a pixel the curve crosses is blended by how much
    // of it the shape covers.
    Bitmap rounded(10, 10, Color::rgb(255, 0, 0));
    RoundedRect corners = RoundedRect::of(Rect { 0, 0, 10, 10 });
    corners.top_left_x = corners.top_left_y = corners.top_right_x = corners.top_right_y = 5;
    corners.bottom_right_x = corners.bottom_right_y = corners.bottom_left_x = corners.bottom_left_y = 5;
    rounded.push_round_clip(corners);
    rounded.draw_scaled_opaque(Bitmap(1, 1, Color::rgb(255, 255, 255)), Rect { 0, 0, 10, 10 });
    CHECK(rounded.pixel(0, 0) == Color::rgb(255, 0, 0));
    CHECK(rounded.pixel(5, 5) == Color::rgb(255, 255, 255));
    CHECK(rounded.pixel(5, 0) == Color::rgb(255, 255, 255));
    Color const edge = rounded.pixel(1, 1);
    CHECK(edge.r == 255 && edge.g > 0 && edge.g < 255);

    return sashfold::test::report("bitmap");
}
