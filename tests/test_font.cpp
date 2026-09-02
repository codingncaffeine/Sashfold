#include "Test.h"

#include "core/Bitmap.h"
#include "text/SashfoldMono.h"

#include <initializer_list>

// Sashfold Mono beyond ASCII: letters with diacritics compose from a base
// glyph and combining marks, invisible code points draw nothing, spelling
// aliases share glyphs, and what the face lacks is an honest box — not a
// digit lookalike.

using namespace sashfold;

namespace {

constexpr float size = 24;
constexpr int baseline = 32;
Color const white = Color::rgb(255, 255, 255);

Bitmap render(char32_t code_point)
{
    Bitmap bitmap(32, 48, white);
    text::SashfoldMono::instance().draw_glyph(bitmap, code_point, 4, baseline, size,
        Color::rgb(0, 0, 0), false, false);
    return bitmap;
}

bool same(Bitmap const& a, Bitmap const& b)
{
    return a.pixels() == b.pixels();
}

bool has_ink(Bitmap const& bitmap, int first_row, int last_row)
{
    for (int y = first_row; y <= last_row; ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
            if (!(bitmap.pixel(x, y) == white))
                return true;
        }
    }
    return false;
}

bool rows_equal(Bitmap const& a, Bitmap const& b, int first_row, int last_row)
{
    for (int y = first_row; y <= last_row; ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!(a.pixel(x, y) == b.pixel(x, y)))
                return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    // Design grid rows at 24 px: baseline 32; x-height (grid 10) at 32 - 24 * 15 / 32 = 20.75;
    // cap top (grid 3) at 15.5.
    int const x_height_row = 21;
    int const cap_row = 15;

    Bitmap const blank = render(U' ');
    Bitmap const box = render(0xFFFD);
    Bitmap const e = render(U'e');
    Bitmap const e_acute = render(0x00E9);

    // --- Composition: base + mark ------------------------------------------------
    CHECK(!same(e_acute, e));
    CHECK(!same(e_acute, box));
    CHECK(rows_equal(e_acute, e, x_height_row, 47)); // the letter itself is untouched
    CHECK(has_ink(e_acute, 0, x_height_row - 3)); // the accent sits above it
    CHECK(!has_ink(e, 0, x_height_row - 3));

    // Capitals lift the accent above the cap line.
    CHECK(!has_ink(render(U'E'), 0, cap_row - 2));
    CHECK(has_ink(render(0x00C9), 0, cap_row - 2));
    CHECK(rows_equal(render(0x00C9), render(U'E'), cap_row + 1, 47));

    // Below-marks leave the letter alone above the baseline (the cedilla
    // attaches at the baseline, so its stroke touches the last row or two).
    Bitmap const c_cedilla = render(0x00E7);
    CHECK(rows_equal(c_cedilla, render(U'c'), 0, baseline - 3));
    CHECK(has_ink(c_cedilla, baseline + 1, 47));

    // The dot of i yields to an above-mark: í is the dotless stem plus the accent.
    Bitmap const i_acute = render(0x00ED);
    CHECK(!same(i_acute, render(U'i')));
    CHECK(rows_equal(i_acute, render(0x0131), x_height_row, 47));

    // Stacked marks: ế carries both the circumflex and the acute.
    Bitmap const e_circumflex = render(0x00EA);
    CHECK(!same(render(0x1EBF), e_circumflex));
    CHECK(!same(render(0x1EBF), box));
    CHECK(rows_equal(render(0x1EBF), e, x_height_row, 47));

    // --- Nothing drawn for invisible code points ---------------------------------
    for (char32_t const c : { 0x00A0u, 0x00ADu, 0x200Bu, 0x200Eu, 0x2009u, 0x202Fu, 0xFEFFu, 0x3000u })
        CHECK(same(render(c), blank));

    // --- Aliases -------------------------------------------------------------------
    CHECK(same(render(0x2010), render(U'-')));
    CHECK(same(render(0x2212), render(0x2013)));
    CHECK(same(render(0xFF21), render(U'A')));
    CHECK(same(render(0x2032), render(0x2019)));

    // --- What the face lacks is a box, and the box is not a zero ------------------
    CHECK(same(render(0x0430), box)); // Cyrillic a: waits for system fonts
    CHECK(same(render(0x65E5), box)); // CJK
    CHECK(!same(box, render(U'0')));
    CHECK(!same(box, render(U'O')));

    // --- Glyphs the reader web leans on exist ----------------------------------------
    for (char32_t const c : { 0x2191u, 0x2193u, 0x2194u, 0x00B7u, 0x00B0u, 0x00A9u, 0x00AEu,
             0x00ABu, 0x00BBu, 0x00DFu, 0x00E6u, 0x0153u, 0x00F8u, 0x0142u, 0x0111u, 0x20ACu,
             0x00A3u, 0x00BDu, 0x2122u, 0x2713u, 0x00F1u, 0x00FCu, 0x0161u, 0x010Du, 0x0105u,
             0x0151u, 0x1EA1u, 0x01B0u })
        CHECK(!same(render(c), box));

    return sashfold::test::report("font");
}
