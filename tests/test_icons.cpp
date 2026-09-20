// The chrome's icons are drawn in whole numbers, so that every machine draws
// the same bytes: this holds the rasterizer to coverages worked out by hand,
// which no floating point could have been trusted to reach alike on three
// operating systems.
//
// At 16 px a grid unit is a pixel, a stroke reaches 0.75 px either side of
// its spine, and a pixel has 8 x 8 sub-samples at the odd sixteenths. The
// menu's middle bar runs along y = 8: it covers y 7.25 to 8.75, which is the
// lower six of row 7's eight rows of samples and the upper six of row 8's —
// 48 of 64, 191 of 255 — and none of row 6. Its top bar runs along y = 4.5,
// covering 3.75 to 5.25: all of row 4, and the two rows of samples nearest
// it in rows 3 and 5 — 16 of 64, 63 of 255.

#include "Test.h"

#include "ui/Icons.h"

using namespace sashfold;
using namespace sashfold::ui;

int main()
{
    // --- A bar, by its rows -----------------------------------------------------
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 8, 6)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 8, 7)), 191);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 8, 8)), 191);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 8, 9)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 8, 3)), 63);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 8, 4)), 255);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 8, 5)), 63);
    // Its ends are round: the bar runs from x = 3 to 13, so the column that
    // holds the cap's tip (2.25 to 3) is lightly covered and the next is not
    // at all.
    CHECK(icon_coverage(Icon::Menu, 16, 2, 4) > 0);
    CHECK(icon_coverage(Icon::Menu, 16, 2, 4) < icon_coverage(Icon::Menu, 16, 3, 4));
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 1, 4)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 14, 4)), 0);

    // --- Twice the size is twice the picture ------------------------------------
    // At 32 px the same bar covers y 14.5 to 17.5: rows 15 and 16 whole, half
    // of rows 14 and 17.
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 32, 16, 13)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 32, 16, 14)), 127);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 32, 16, 15)), 255);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 32, 16, 16)), 255);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 32, 16, 17)), 127);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 32, 16, 18)), 0);

    // --- Forward is Back in a mirror, to the sample ------------------------------
    for (int const size : { 10, 16, 20, 32, 48 }) {
        int differing = 0;
        int inked = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                differing += icon_coverage(Icon::Back, size, x, y) != icon_coverage(Icon::Forward, size, size - 1 - x, y);
                inked += icon_coverage(Icon::Back, size, x, y) != 0;
            }
        }
        CHECK_EQ(differing, 0);
        CHECK(inked > size); // and something was drawn to compare
    }

    // --- A cross is the same turned over its diagonals ----------------------------
    {
        int differing = 0;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                differing += icon_coverage(Icon::Close, 16, x, y) != icon_coverage(Icon::Close, 16, y, x);
                differing += icon_coverage(Icon::Close, 16, x, y) != icon_coverage(Icon::Close, 16, 15 - x, y);
            }
        }
        CHECK_EQ(differing, 0);
        CHECK_EQ(static_cast<int>(icon_coverage(Icon::Close, 16, 7, 7)), 255); // where the strokes cross
        CHECK_EQ(static_cast<int>(icon_coverage(Icon::Close, 16, 8, 2)), 0);
    }

    // --- The reload's ring is open where its arrowhead is ---------------------------
    // The ring is five units about (8, 8), reaching from 4.25 to 5.75 of its
    // middle. Of pixel (3, 7)'s eight columns of samples the six to the left
    // are on it whole; the seventh, 4.1875 from the middle across, is on it
    // only in the two rows far enough up (0.8125 and 0.9375) to be 4.25 away;
    // the eighth is inside the ring's hole: 6 x 8 + 2 = 50 of 64, 199. And
    // the ring is left open from half past one to a little past three, which
    // is where (12, 7) lies: nothing of the ring, the head or its ends there.
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Reload, 16, 3, 7)), 199);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Reload, 16, 3, 8)), 199); // and the same below the middle
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Reload, 16, 12, 7)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Reload, 16, 8, 8)), 0); // and nothing at its middle
    CHECK(icon_coverage(Icon::Reload, 16, 13, 4) > 200); // the head's upright arm, x 12.35 to 13.85

    // --- Out of bounds is nothing, never a read past the mask -----------------------
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, -1, 4)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 16, 16, 4)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 0, 0, 0)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Menu, 1000, 0, 0)), 0);

    // --- Drawn, it is the color through its coverage ---------------------------------
    {
        Bitmap canvas(20, 20, Color::rgb(0, 0, 0));
        draw_icon(canvas, Icon::Menu, Rect { 0, 0, 20, 20 }, 16, Color::rgb(255, 255, 255));
        // Centered: the icon's (8, 4) is the canvas's (10, 6).
        CHECK(canvas.pixel(10, 6) == Color::rgb(255, 255, 255));
        CHECK(canvas.pixel(10, 5) == Color::rgb(63, 63, 63));
        CHECK(canvas.pixel(10, 9) == Color::rgb(191, 191, 191));
        CHECK(canvas.pixel(0, 0) == Color::rgb(0, 0, 0));
    }

    // --- The page: a sheet with its corner folded --------------------------------
    // At 16 px its left edge runs down x = 4.5 and covers 3.75 to 5.25: all
    // of column 4, and the two columns of samples nearest it in columns 3
    // and 5 (16 of 64, 63 of 255). Its right edge (x = 11.5), its foot
    // (y = 13.5) and its top (y = 2.5) likewise, by their rows.
    auto const page = [](int x, int y) { return static_cast<int>(icon_coverage(Icon::Page, 16, x, y)); };
    CHECK_EQ(page(3, 8), 63);
    CHECK_EQ(page(4, 8), 255);
    CHECK_EQ(page(5, 8), 63);
    CHECK_EQ(page(6, 8), 0);
    CHECK_EQ(page(10, 8), 63);
    CHECK_EQ(page(11, 8), 255);
    CHECK_EQ(page(12, 8), 63);
    CHECK_EQ(page(13, 8), 0);
    CHECK_EQ(page(8, 12), 63);
    CHECK_EQ(page(8, 13), 255);
    CHECK_EQ(page(8, 14), 63);
    CHECK_EQ(page(8, 15), 0);
    CHECK_EQ(page(6, 1), 63);
    CHECK_EQ(page(6, 2), 255);
    CHECK_EQ(page(6, 3), 63);
    // Its top ends at x = 9, where the corner is cut along a diagonal: the
    // corner's own pixel, which the reader's outline fills to the last
    // sample, has nothing in it — the nearest sample is 1.15 px from the cut.
    CHECK_EQ(page(11, 2), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Reader, 16, 11, 2)), 255);
    // The fold's own edges are there, inside the cut.
    CHECK(page(9, 4) > 0);
    CHECK(page(10, 5) > 0);
    // And nothing is written on it: the reader's middle line is here.
    CHECK_EQ(page(8, 8), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Reader, 16, 8, 8)), 191);

    // The bookmarks' icons. A star is the same on its left as on its right,
    // about x = 8: the pixel columns 7 and 8 mirror each other, 6 and 9, and
    // so on. Its tip is inked, its corners are not, and the middle is empty
    // in the outline and inked in the filled one.
    for (Icon const star : { Icon::Star, Icon::StarFilled }) {
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 8; ++x)
                CHECK_EQ(static_cast<int>(icon_coverage(star, 16, x, y)), static_cast<int>(icon_coverage(star, 16, 15 - x, y)));
        }
        CHECK(icon_coverage(star, 16, 7, 3) > 0);
        CHECK_EQ(static_cast<int>(icon_coverage(star, 16, 0, 0)), 0);
        CHECK_EQ(static_cast<int>(icon_coverage(star, 16, 15, 15)), 0);
    }
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Star, 16, 7, 8)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::StarFilled, 16, 7, 8)), 255);
    // Two chevrons, each the same above y = 8 as below it, the second the
    // first moved 4.5 to the right; nothing at the far left.
    for (int x = 0; x < 16; ++x) {
        for (int y = 0; y < 8; ++y)
            CHECK_EQ(static_cast<int>(icon_coverage(Icon::Chevrons, 16, x, y)), static_cast<int>(icon_coverage(Icon::Chevrons, 16, x, 15 - y)));
    }
    CHECK(icon_coverage(Icon::Chevrons, 16, 7, 7) > 0);
    CHECK(icon_coverage(Icon::Chevrons, 16, 11, 7) > 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Chevrons, 16, 1, 8)), 0);
    // A folder: its foot along y = 12.5, its tab at the top left and nothing
    // where the tab is not, its inside empty.
    CHECK(icon_coverage(Icon::Folder, 16, 8, 12) > 0);
    CHECK(icon_coverage(Icon::Folder, 16, 4, 4) > 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Folder, 16, 11, 3)), 0);
    CHECK_EQ(static_cast<int>(icon_coverage(Icon::Folder, 16, 8, 9)), 0);

    return sashfold::test::report("icons");
}
