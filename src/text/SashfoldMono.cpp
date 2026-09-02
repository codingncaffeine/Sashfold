#include "text/SashfoldMono.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "text/TrueTypeWriter.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sashfold::text {

namespace {

// One stroke of a glyph, on the 20x32 grid.
//   V: centerline from (a,b) to (c,d), thickened horizontally
//   H: centerline from (a,b) to (c,d), thickened vertically
//   B: axis-aligned box with corners (a,b)-(c,d), drawn as-is
struct Seg {
    char kind;
    std::int8_t a, b, c, d;
};

struct GlyphDef {
    char32_t code_point;
    std::vector<Seg> segments;
};

Seg V(int a, int b, int c, int d) { return { 'V', static_cast<std::int8_t>(a), static_cast<std::int8_t>(b), static_cast<std::int8_t>(c), static_cast<std::int8_t>(d) }; }
Seg H(int a, int b, int c, int d) { return { 'H', static_cast<std::int8_t>(a), static_cast<std::int8_t>(b), static_cast<std::int8_t>(c), static_cast<std::int8_t>(d) }; }
Seg B(int a, int b, int c, int d) { return { 'B', static_cast<std::int8_t>(a), static_cast<std::int8_t>(b), static_cast<std::int8_t>(c), static_cast<std::int8_t>(d) }; }

// The face. Key ys: cap top 3, x-height top 10, baseline 25, descender 31.
// Key xs: left 4, right 16, center 10 (advance 20).
std::vector<GlyphDef> build_glyphs()
{
    std::vector<GlyphDef> defs;
    auto add = [&](char32_t code_point, std::vector<Seg> segments) {
        defs.push_back(GlyphDef { code_point, std::move(segments) });
    };

    // Uppercase.
    add(U'A', { V(10, 3, 4, 25), V(10, 3, 16, 25), H(6, 18, 14, 18) });
    add(U'B', { V(4, 3, 4, 25), H(5, 4, 14, 4), H(5, 14, 14, 14), H(5, 24, 15, 24), V(15, 5, 15, 13), V(16, 15, 16, 23) });
    add(U'C', { V(4, 5, 4, 23), H(6, 4, 16, 4), H(6, 24, 16, 24) });
    add(U'D', { V(4, 3, 4, 25), H(5, 4, 13, 4), H(5, 24, 13, 24), V(16, 6, 16, 22) });
    add(U'E', { V(4, 3, 4, 25), H(6, 4, 17, 4), H(6, 14, 14, 14), H(6, 24, 17, 24) });
    add(U'F', { V(4, 3, 4, 25), H(6, 4, 17, 4), H(6, 14, 14, 14) });
    add(U'G', { V(4, 5, 4, 23), H(6, 4, 16, 4), H(6, 24, 15, 24), V(16, 15, 16, 23), H(11, 15, 15, 15) });
    add(U'H', { V(4, 3, 4, 25), V(16, 3, 16, 25), H(6, 14, 14, 14) });
    add(U'I', { V(10, 3, 10, 25), H(5, 4, 15, 4), H(5, 24, 15, 24) });
    add(U'J', { V(15, 3, 15, 22), H(6, 24, 13, 24), V(4, 19, 4, 23) });
    add(U'K', { V(4, 3, 4, 25), V(16, 3, 6, 14), V(8, 15, 16, 25) });
    add(U'L', { V(4, 3, 4, 25), H(6, 24, 17, 24) });
    add(U'M', { V(3, 3, 3, 25), V(17, 3, 17, 25), V(3, 3, 10, 16), V(17, 3, 10, 16) });
    add(U'N', { V(4, 3, 4, 25), V(16, 3, 16, 25), V(4, 3, 16, 25) });
    add(U'O', { V(4, 5, 4, 23), V(16, 5, 16, 23), H(6, 4, 14, 4), H(6, 24, 14, 24) });
    add(U'P', { V(4, 3, 4, 25), H(5, 4, 14, 4), H(5, 15, 14, 15), V(16, 5, 16, 14) });
    add(U'Q', { V(4, 5, 4, 23), V(16, 5, 16, 23), H(6, 4, 14, 4), H(6, 24, 14, 24), V(12, 19, 17, 28) });
    add(U'R', { V(4, 3, 4, 25), H(5, 4, 14, 4), H(5, 15, 14, 15), V(16, 5, 16, 14), V(9, 15, 16, 25) });
    add(U'S', { H(6, 4, 16, 4), V(4, 5, 4, 13), H(6, 14, 14, 14), V(16, 15, 16, 23), H(4, 24, 14, 24) });
    add(U'T', { H(3, 4, 17, 4), V(10, 5, 10, 25) });
    add(U'U', { V(4, 3, 4, 23), V(16, 3, 16, 23), H(6, 24, 14, 24) });
    add(U'V', { V(4, 3, 10, 25), V(16, 3, 10, 25) });
    add(U'W', { V(3, 3, 7, 25), V(10, 8, 7, 25), V(10, 8, 13, 25), V(17, 3, 13, 25) });
    add(U'X', { V(4, 3, 16, 25), V(16, 3, 4, 25) });
    add(U'Y', { V(4, 3, 10, 14), V(16, 3, 10, 14), V(10, 14, 10, 25) });
    add(U'Z', { H(4, 4, 16, 4), V(16, 5, 4, 22), H(4, 24, 16, 24) });

    // Lowercase.
    add(U'a', { H(6, 11, 15, 11), V(16, 12, 16, 24), H(6, 24, 14, 24), V(4, 17, 4, 23), H(6, 16, 14, 16) });
    add(U'b', { V(4, 3, 4, 25), H(6, 11, 14, 11), V(16, 13, 16, 23), H(6, 24, 14, 24) });
    add(U'c', { H(6, 11, 16, 11), V(4, 13, 4, 23), H(6, 24, 16, 24) });
    add(U'd', { V(16, 3, 16, 25), H(6, 11, 14, 11), V(4, 13, 4, 23), H(6, 24, 14, 24) });
    add(U'e', { H(6, 11, 14, 11), V(16, 12, 16, 16), H(6, 17, 16, 17), V(4, 13, 4, 23), H(6, 24, 15, 24) });
    add(U'f', { H(9, 4, 15, 4), V(7, 5, 7, 25), H(3, 11, 13, 11) });
    add(U'g', { H(6, 11, 14, 11), V(4, 13, 4, 21), H(6, 22, 14, 22), V(16, 11, 16, 28), H(5, 30, 14, 30) });
    add(U'h', { V(4, 3, 4, 25), H(6, 11, 14, 11), V(16, 13, 16, 25) });
    add(U'i', { V(10, 10, 10, 25), H(9, 5, 11, 5) });
    add(U'j', { V(12, 10, 12, 28), H(4, 30, 10, 30), H(11, 5, 13, 5) });
    add(U'k', { V(4, 3, 4, 25), V(15, 10, 6, 17), V(8, 16, 15, 25) });
    add(U'l', { V(10, 3, 10, 25) });
    add(U'm', { V(3, 10, 3, 25), H(4, 11, 16, 11), V(10, 12, 10, 25), V(17, 12, 17, 25) });
    add(U'n', { V(4, 10, 4, 25), H(6, 11, 14, 11), V(16, 13, 16, 25) });
    add(U'o', { H(6, 11, 14, 11), H(6, 24, 14, 24), V(4, 13, 4, 23), V(16, 13, 16, 23) });
    add(U'p', { V(4, 10, 4, 31), H(6, 11, 14, 11), V(16, 13, 16, 23), H(6, 24, 14, 24) });
    add(U'q', { V(16, 10, 16, 31), H(6, 11, 14, 11), V(4, 13, 4, 23), H(6, 24, 14, 24) });
    add(U'r', { V(4, 10, 4, 25), H(6, 11, 16, 11) });
    add(U's', { H(6, 11, 16, 11), V(4, 12, 4, 16), H(6, 17, 14, 17), V(16, 18, 16, 22), H(4, 24, 14, 24) });
    add(U't', { V(7, 4, 7, 25), H(3, 10, 13, 10) });
    add(U'u', { V(4, 10, 4, 23), V(16, 10, 16, 25), H(6, 24, 14, 24) });
    add(U'v', { V(4, 10, 10, 25), V(16, 10, 10, 25) });
    add(U'w', { V(3, 10, 7, 25), V(10, 14, 7, 25), V(10, 14, 13, 25), V(17, 10, 13, 25) });
    add(U'x', { V(4, 10, 16, 25), V(16, 10, 4, 25) });
    add(U'y', { V(4, 10, 10, 22), V(16, 10, 8, 31) });
    add(U'z', { H(4, 11, 16, 11), V(15, 12, 5, 22), H(4, 24, 16, 24) });

    // Digits.
    add(U'0', { V(4, 5, 4, 23), V(16, 5, 16, 23), H(6, 4, 14, 4), H(6, 24, 14, 24), V(14, 7, 6, 21) });
    add(U'1', { V(10, 3, 10, 25), V(10, 3, 6, 7), H(5, 24, 15, 24) });
    add(U'2', { H(4, 4, 15, 4), V(16, 5, 16, 12), V(15, 13, 4, 22), H(4, 24, 17, 24) });
    add(U'3', { H(4, 4, 15, 4), V(16, 5, 16, 12), H(8, 14, 15, 14), V(16, 16, 16, 22), H(4, 24, 15, 24) });
    add(U'4', { V(13, 3, 3, 17), H(3, 18, 17, 18), V(13, 3, 13, 25) });
    add(U'5', { H(4, 4, 17, 4), V(4, 5, 4, 13), H(6, 13, 14, 13), V(16, 15, 16, 22), H(4, 24, 14, 24) });
    add(U'6', { V(4, 5, 4, 23), H(6, 4, 16, 4), H(6, 14, 14, 14), V(16, 16, 16, 22), H(6, 24, 14, 24) });
    add(U'7', { H(3, 4, 17, 4), V(17, 5, 8, 25) });
    add(U'8', { H(6, 4, 14, 4), V(4, 5, 4, 12), V(16, 5, 16, 12), H(6, 14, 14, 14), V(4, 16, 4, 23), V(16, 16, 16, 23), H(6, 24, 14, 24) });
    add(U'9', { H(6, 4, 14, 4), V(4, 6, 4, 12), V(16, 6, 16, 23), H(6, 14, 14, 14), H(6, 24, 14, 24) });

    // ASCII punctuation.
    add(U'!', { V(10, 3, 10, 18), H(9, 23, 11, 23) });
    add(U'"', { V(7, 3, 7, 9), V(13, 3, 13, 9) });
    add(U'#', { V(8, 4, 6, 24), V(14, 4, 12, 24), H(3, 10, 17, 10), H(3, 18, 17, 18) });
    add(U'$', { H(6, 5, 16, 5), V(4, 6, 4, 13), H(6, 14, 14, 14), V(16, 15, 16, 22), H(4, 23, 14, 23), V(10, 1, 10, 27) });
    add(U'%', { B(3, 3, 8, 8), B(12, 20, 17, 25), V(16, 4, 4, 24) });
    add(U'&', { H(6, 4, 12, 4), V(4, 6, 4, 11), V(13, 6, 13, 11), V(13, 11, 4, 19), V(4, 13, 4, 23), H(6, 24, 13, 24), V(9, 15, 17, 25) });
    add(U'\'', { V(10, 3, 10, 9) });
    add(U'(', { V(13, 3, 10, 10), V(10, 9, 10, 19), V(10, 18, 13, 25) });
    add(U')', { V(7, 3, 10, 10), V(10, 9, 10, 19), V(10, 18, 7, 25) });
    add(U'*', { V(10, 4, 10, 14), V(5, 6, 15, 12), V(15, 6, 5, 12) });
    add(U'+', { V(10, 9, 10, 21), H(4, 15, 16, 15) });
    add(U',', { H(9, 23, 11, 23), V(10, 25, 8, 29) });
    add(U'-', { H(5, 15, 15, 15) });
    add(U'.', { H(9, 23, 11, 23) });
    add(U'/', { V(15, 3, 5, 27) });
    add(U':', { H(9, 11, 11, 11), H(9, 23, 11, 23) });
    add(U';', { H(9, 11, 11, 11), H(9, 23, 11, 23), V(10, 25, 8, 29) });
    add(U'<', { V(15, 5, 5, 15), V(5, 15, 15, 25) });
    add(U'=', { H(4, 11, 16, 11), H(4, 19, 16, 19) });
    add(U'>', { V(5, 5, 15, 15), V(15, 15, 5, 25) });
    add(U'?', { H(4, 4, 14, 4), V(16, 5, 16, 10), V(15, 11, 10, 16), V(10, 15, 10, 18), H(9, 23, 11, 23) });
    add(U'@', { H(5, 4, 15, 4), V(3, 6, 3, 22), H(5, 24, 16, 24), V(17, 6, 17, 16), V(13, 10, 13, 17), H(9, 10, 12, 10), V(8, 11, 8, 16), H(9, 17, 14, 17) });
    add(U'[', { V(7, 3, 7, 28), H(8, 4, 13, 4), H(8, 27, 13, 27) });
    add(U'\\', { V(5, 3, 15, 27) });
    add(U']', { V(13, 3, 13, 28), H(7, 4, 12, 4), H(7, 27, 12, 27) });
    add(U'^', { V(10, 3, 5, 10), V(10, 3, 15, 10) });
    add(U'_', { H(2, 29, 18, 29) });
    add(U'`', { V(8, 3, 11, 7) });
    add(U'{', { V(12, 4, 9, 9), V(9, 8, 9, 13), H(5, 15, 9, 15), V(9, 17, 9, 22), V(9, 21, 12, 26) });
    add(U'|', { V(10, 3, 10, 29) });
    add(U'}', { V(8, 4, 11, 9), V(11, 8, 11, 13), H(11, 15, 15, 15), V(11, 17, 11, 22), V(11, 21, 8, 26) });
    add(U'~', { V(4, 15, 8, 12), V(8, 12, 12, 15), V(12, 15, 16, 12) });

    // Specials.
    add(0xFFFD, { H(4, 4, 16, 4), H(4, 24, 16, 24), V(3, 5, 3, 23), V(17, 5, 17, 23) }); // missing glyph: an honest empty box, unlike a zero
    add(0x2022, { B(6, 12, 14, 20) }); // bullet
    add(0x25E6, { H(7, 13, 13, 13), H(7, 19, 13, 19), V(6, 14, 6, 18), V(14, 14, 14, 18) }); // white bullet
    add(0x25AA, { B(6, 13, 14, 21) }); // small black square
    add(0x2013, { H(3, 15, 17, 15) }); // en dash
    add(0x2014, { H(1, 15, 19, 15) }); // em dash
    add(0x2018, { V(10, 3, 8, 9) }); // quotes
    add(0x2019, { V(8, 3, 10, 9) });
    add(0x201C, { V(9, 3, 7, 9), V(14, 3, 12, 9) });
    add(0x201D, { V(7, 3, 9, 9), V(12, 3, 14, 9) });
    add(0x2026, { H(3, 23, 5, 23), H(9, 23, 11, 23), H(15, 23, 17, 23) }); // ellipsis

    // Chrome glyphs: back, forward, reload, close.
    add(0x2190, { H(2, 14, 18, 14), V(9, 7, 2, 14), V(2, 14, 9, 21) }); // left arrow
    add(0x2192, { H(2, 14, 18, 14), V(11, 7, 18, 14), V(18, 14, 11, 21) }); // right arrow
    add(0x21BB, { H(7, 4, 11, 4), V(11, 4, 7, 0), V(11, 4, 7, 8), V(19, 9, 19, 19), V(19, 19, 13, 25), H(7, 25, 13, 25), V(7, 25, 1, 19), V(1, 19, 1, 9), V(1, 9, 7, 4) }); // clockwise open circle arrow: a ring open at the upper right, the arrowhead at the top pointing into the gap
    add(0x00D7, { V(5, 9, 15, 19), V(15, 9, 5, 19) }); // multiplication sign
    add(0x00A0, {}); // nbsp draws nothing

    // Arrows and marks the reader web leans on (Wikipedia's reference
    // back-links are U+2191).
    add(0x2191, { V(10, 4, 10, 26), V(10, 4, 4, 10), V(10, 4, 16, 10) }); // up arrow
    add(0x2193, { V(10, 4, 10, 26), V(10, 26, 4, 20), V(10, 26, 16, 20) }); // down arrow
    add(0x2194, { H(2, 14, 18, 14), V(9, 7, 2, 14), V(2, 14, 9, 21), V(11, 7, 18, 14), V(18, 14, 11, 21) }); // left right arrow
    add(0x2713, { V(4, 15, 8, 23), V(8, 23, 17, 5) }); // check mark
    add(0x2020, { V(10, 3, 10, 25), H(5, 9, 15, 9) }); // dagger
    add(0x2021, { V(10, 3, 10, 25), H(5, 9, 15, 9), H(5, 19, 15, 19) }); // double dagger
    add(0x2039, { V(12, 10, 8, 15), V(8, 15, 12, 20) }); // single left guillemet
    add(0x203A, { V(8, 10, 12, 15), V(12, 15, 8, 20) }); // single right guillemet
    add(0x2122, { H(1, 3, 7, 3), V(4, 4, 4, 11), V(9, 3, 9, 11), V(9, 3, 12, 8), V(15, 3, 12, 8), V(15, 3, 15, 11) }); // trade mark

    // Latin-1 punctuation and symbols.
    add(0x00A1, { B(9, 10, 11, 12), V(10, 15, 10, 31) }); // inverted exclamation
    add(0x00A2, { H(6, 11, 16, 11), V(4, 13, 4, 23), H(6, 24, 16, 24), V(10, 7, 10, 29) }); // cent
    add(0x00A3, { H(10, 4, 15, 4), V(9, 5, 9, 23), H(5, 14, 13, 14), H(4, 24, 16, 24) }); // pound
    add(0x00A4, { H(7, 9, 13, 9), H(7, 19, 13, 19), V(6, 10, 6, 18), V(14, 10, 14, 18), V(3, 6, 6, 9), V(17, 6, 14, 9), V(3, 22, 6, 19), V(17, 22, 14, 19) }); // currency
    add(0x00A5, { V(4, 3, 10, 14), V(16, 3, 10, 14), V(10, 14, 10, 25), H(5, 16, 15, 16), H(5, 20, 15, 20) }); // yen
    add(0x00A6, { V(10, 3, 10, 13), V(10, 18, 10, 29) }); // broken bar
    add(0x00A7, { H(7, 4, 15, 4), V(5, 5, 5, 9), H(7, 10, 13, 10), V(15, 11, 15, 15), H(7, 16, 13, 16), V(5, 17, 5, 21), H(5, 22, 13, 22), V(15, 23, 15, 27), H(5, 28, 13, 28) }); // section
    add(0x00A8, { B(6, 5, 8, 7), B(12, 5, 14, 7) }); // diaeresis, spacing
    add(0x00A9, { H(7, 4, 13, 4), V(13, 4, 17, 8), V(17, 8, 17, 20), V(17, 20, 13, 24), H(7, 24, 13, 24), V(7, 24, 3, 20), V(3, 20, 3, 8), V(3, 8, 7, 4), H(8, 10, 12, 10), V(7, 11, 7, 17), H(8, 18, 12, 18) }); // copyright
    add(0x00AA, { H(6, 3, 12, 3), V(13, 4, 13, 10), H(6, 11, 13, 11), V(5, 7, 5, 10), H(6, 7, 12, 7) }); // feminine ordinal
    add(0x00AB, { V(9, 10, 5, 15), V(5, 15, 9, 20), V(15, 10, 11, 15), V(11, 15, 15, 20) }); // left guillemet
    add(0x00AC, { H(4, 14, 16, 14), V(16, 14, 16, 20) }); // not sign
    add(0x00AE, { H(7, 4, 13, 4), V(13, 4, 17, 8), V(17, 8, 17, 20), V(17, 20, 13, 24), H(7, 24, 13, 24), V(7, 24, 3, 20), V(3, 20, 3, 8), V(3, 8, 7, 4), V(7, 10, 7, 18), H(8, 10, 12, 10), H(8, 14, 12, 14), V(13, 11, 13, 13), V(9, 14, 13, 18) }); // registered
    add(0x00AF, { H(4, 5, 16, 5) }); // macron, spacing
    add(0x00B0, { H(8, 3, 12, 3), H(8, 8, 12, 8), V(7, 4, 7, 7), V(13, 4, 13, 7) }); // degree
    add(0x00B1, { V(10, 6, 10, 18), H(4, 12, 16, 12), H(4, 22, 16, 22) }); // plus-minus
    add(0x00B2, { H(6, 3, 12, 3), V(13, 4, 13, 7), V(12, 8, 6, 12), H(6, 13, 14, 13) }); // superscript two
    add(0x00B3, { H(6, 3, 12, 3), V(13, 4, 13, 6), H(9, 7, 12, 7), V(13, 8, 13, 11), H(6, 12, 12, 12) }); // superscript three
    add(0x00B4, { V(12, 4, 9, 8) }); // acute, spacing
    add(0x00B5, { V(4, 10, 4, 31), V(16, 10, 16, 25), H(6, 24, 14, 24) }); // micro
    add(0x00B6, { B(5, 4, 11, 13), V(11, 3, 11, 25), V(15, 3, 15, 25), H(5, 4, 16, 4) }); // pilcrow
    add(0x00B7, { B(9, 13, 11, 15) }); // middle dot
    add(0x00B8, { V(10, 25, 10, 28), H(7, 29, 10, 29) }); // cedilla, spacing
    add(0x00B9, { V(10, 3, 10, 12), V(10, 3, 8, 5), H(8, 12, 12, 12) }); // superscript one
    add(0x00BA, { H(7, 3, 13, 3), H(7, 10, 13, 10), V(6, 4, 6, 9), V(14, 4, 14, 9), H(6, 13, 14, 13) }); // masculine ordinal
    add(0x00BB, { V(5, 10, 9, 15), V(9, 15, 5, 20), V(11, 10, 15, 15), V(15, 15, 11, 20) }); // right guillemet
    add(0x00BC, { V(4, 3, 4, 12), V(4, 3, 2, 5), V(17, 3, 3, 25), V(15, 14, 11, 21), H(11, 22, 18, 22), V(15, 14, 15, 25) }); // one quarter
    add(0x00BD, { V(4, 3, 4, 12), V(4, 3, 2, 5), V(17, 3, 3, 25), H(11, 14, 16, 14), V(17, 15, 17, 18), V(16, 19, 11, 23), H(11, 24, 18, 24) }); // one half
    add(0x00BE, { H(2, 3, 6, 3), V(7, 4, 7, 6), H(4, 7, 6, 7), V(7, 8, 7, 11), H(2, 12, 6, 12), V(17, 3, 3, 25), V(15, 14, 11, 21), H(11, 22, 18, 22), V(15, 14, 15, 25) }); // three quarters
    add(0x00BF, { B(9, 10, 11, 12), V(10, 15, 10, 19), V(10, 19, 5, 23), V(5, 23, 5, 27), V(5, 27, 10, 31), V(10, 31, 15, 27) }); // inverted question mark
    add(0x00F7, { H(4, 15, 16, 15), B(9, 8, 11, 10), B(9, 20, 11, 22) }); // division
    add(0x20AC, { H(7, 4, 16, 4), V(5, 5, 5, 23), H(7, 24, 16, 24), H(3, 12, 14, 12), H(3, 17, 14, 17) }); // euro

    // Latin letters that are not base + mark.
    add(0x00DF, { V(4, 5, 4, 25), H(6, 4, 13, 4), V(15, 5, 15, 10), H(9, 12, 14, 12), V(16, 13, 16, 23), H(9, 24, 14, 24) }); // sharp s
    add(0x00C6, { V(8, 3, 2, 25), V(8, 3, 8, 25), H(3, 18, 8, 18), H(9, 4, 18, 4), H(9, 14, 15, 14), H(9, 24, 18, 24) }); // AE
    add(0x00E6, { H(2, 11, 8, 11), V(9, 12, 9, 24), V(1, 17, 1, 23), H(2, 16, 8, 16), H(2, 24, 8, 24), H(11, 11, 17, 11), V(18, 12, 18, 16), H(10, 17, 18, 17), V(10, 18, 10, 23), H(11, 24, 18, 24) }); // ae
    add(0x0152, { H(3, 4, 8, 4), H(3, 24, 8, 24), V(2, 5, 2, 23), V(9, 5, 9, 23), H(10, 4, 18, 4), H(10, 14, 16, 14), H(10, 24, 18, 24) }); // OE
    add(0x0153, { H(3, 11, 8, 11), H(3, 24, 8, 24), V(2, 13, 2, 23), V(9, 13, 9, 23), H(11, 11, 16, 11), V(17, 12, 17, 16), H(10, 17, 17, 17), V(10, 18, 10, 23), H(11, 24, 17, 24) }); // oe
    add(0x00D8, { V(4, 5, 4, 23), V(16, 5, 16, 23), H(6, 4, 14, 4), H(6, 24, 14, 24), V(18, 2, 2, 26) }); // O with stroke
    add(0x00F8, { H(6, 11, 14, 11), H(6, 24, 14, 24), V(4, 13, 4, 23), V(16, 13, 16, 23), V(17, 9, 3, 26) }); // o with stroke
    add(0x00D0, { V(4, 3, 4, 25), H(5, 4, 13, 4), H(5, 24, 13, 24), V(16, 6, 16, 22), H(1, 14, 9, 14) }); // Eth
    add(0x00F0, { H(6, 11, 14, 11), H(6, 24, 14, 24), V(4, 13, 4, 23), V(16, 13, 16, 23), V(16, 12, 9, 3), H(6, 6, 14, 6) }); // eth
    add(0x00DE, { V(4, 3, 4, 25), H(5, 8, 14, 8), H(5, 20, 14, 20), V(16, 9, 16, 19) }); // Thorn
    add(0x00FE, { V(4, 3, 4, 31), H(6, 11, 14, 11), V(16, 13, 16, 23), H(6, 24, 14, 24) }); // thorn
    add(0x0110, { V(4, 3, 4, 25), H(5, 4, 13, 4), H(5, 24, 13, 24), V(16, 6, 16, 22), H(1, 14, 9, 14) }); // D with stroke
    add(0x0111, { V(16, 3, 16, 25), H(6, 11, 14, 11), V(4, 13, 4, 23), H(6, 24, 14, 24), H(12, 7, 19, 7) }); // d with stroke
    add(0x0126, { V(4, 3, 4, 25), V(16, 3, 16, 25), H(6, 14, 14, 14), H(1, 7, 19, 7) }); // H with stroke
    add(0x0127, { V(4, 3, 4, 25), H(6, 11, 14, 11), V(16, 13, 16, 25), H(1, 7, 8, 7) }); // h with stroke
    add(0x0131, { V(10, 10, 10, 25) }); // dotless i
    add(0x0141, { V(4, 3, 4, 25), H(6, 24, 17, 24), V(8, 11, 1, 18) }); // L with stroke
    add(0x0142, { V(10, 3, 10, 25), V(13, 10, 7, 17) }); // l with stroke
    add(0x0166, { H(3, 4, 17, 4), V(10, 5, 10, 25), H(6, 15, 14, 15) }); // T with stroke
    add(0x0167, { V(7, 4, 7, 25), H(3, 10, 13, 10), H(3, 17, 11, 17) }); // t with stroke
    add(0x0237, { V(12, 10, 12, 28), H(4, 30, 10, 30) }); // dotless j

    // Combining marks, designed over an x-height base; find() composes them
    // onto letters and lifts the above-marks over capitals and ascenders.
    add(0x0300, { V(8, 4, 11, 8) }); // grave
    add(0x0301, { V(12, 4, 9, 8) }); // acute
    add(0x0302, { V(10, 4, 7, 8), V(10, 4, 13, 8) }); // circumflex
    add(0x0303, { H(5, 7, 8, 5), H(8, 5, 12, 7), H(12, 7, 15, 5) }); // tilde
    add(0x0304, { H(6, 6, 14, 6) }); // macron
    add(0x0306, { V(6, 4, 7, 7), H(7, 7, 13, 7), V(14, 4, 13, 7) }); // breve
    add(0x0307, { B(9, 5, 11, 7) }); // dot above
    add(0x0308, { B(6, 5, 8, 7), B(12, 5, 14, 7) }); // diaeresis
    add(0x0309, { H(8, 4, 12, 4), V(12, 4, 12, 6), V(12, 6, 9, 8) }); // hook above
    add(0x030A, { H(8, 3, 12, 3), H(8, 8, 12, 8), V(7, 4, 7, 7), V(13, 4, 13, 7) }); // ring above
    add(0x030B, { V(11, 4, 8, 8), V(15, 4, 12, 8) }); // double acute
    add(0x030C, { V(7, 4, 10, 8), V(13, 4, 10, 8) }); // caron
    add(0x031B, { V(16, 9, 18, 6) }); // horn, attached at the upper right
    add(0x0323, { B(9, 27, 11, 29) }); // dot below
    add(0x0327, { V(10, 25, 10, 28), H(7, 29, 10, 29) }); // cedilla
    add(0x0328, { V(10, 25, 9, 28), H(9, 29, 13, 29) }); // ogonek
    add(0x0331, { H(6, 28, 14, 28) }); // macron below

    return defs;
}

struct Point {
    std::int64_t x, y; // 1/4-subpixel fixed point
};

using Quad = std::array<Point, 4>;

// Expands a segment to its quad on the design grid, in grid units x4
// (so half-unit offsets stay integral).
Quad expand(Seg const& seg, int weight)
{
    auto const gp = [](int gx, int gy) { return Point { gx * 4, gy * 4 }; };
    int const half = 2 * weight; // stroke half-width in quarter-units (weight 3 -> 6)
    switch (seg.kind) {
    case 'V': {
        Point const a = gp(seg.a, seg.b);
        Point const b = gp(seg.c, seg.d);
        return Quad { Point { a.x - half, a.y }, Point { a.x + half, a.y },
            Point { b.x + half, b.y }, Point { b.x - half, b.y } };
    }
    case 'H': {
        Point const a = gp(seg.a, seg.b);
        Point const b = gp(seg.c, seg.d);
        return Quad { Point { a.x, a.y - half }, Point { b.x, b.y - half },
            Point { b.x, b.y + half }, Point { a.x, a.y + half } };
    }
    default: { // 'B'
        Point const a = gp(seg.a, seg.b);
        Point const b = gp(seg.c, seg.d);
        return Quad { Point { a.x, a.y }, Point { b.x, a.y }, Point { b.x, b.y },
            Point { a.x, b.y } };
    }
    }
}

// Even-odd point-in-polygon, integer arithmetic only.
bool inside(Quad const& quad, std::int64_t px, std::int64_t py)
{
    bool in = false;
    for (std::size_t i = 0, j = 3; i < 4; j = i++) {
        Point const& a = quad[i];
        Point const& b = quad[j];
        if ((a.y > py) == (b.y > py))
            continue;
        // x of the edge at height py, compared without division:
        // px < a.x + (b.x-a.x)*(py-a.y)/(b.y-a.y)
        std::int64_t const dy = b.y - a.y;
        std::int64_t const lhs = (px - a.x) * dy;
        std::int64_t const rhs = (b.x - a.x) * (py - a.y);
        if (dy > 0 ? lhs < rhs : lhs > rhs)
            in = !in;
    }
    return in;
}

struct GlyphMask {
    int left = 0; // px offset from the glyph origin
    int top = 0; // px offset from the TOP of the em box (origin y - ascent)
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
};

struct MaskKey {
    char32_t code_point;
    int size_q; // size in quarter px
    bool bold;
    bool italic;
    bool operator==(MaskKey const&) const = default;
};

struct MaskKeyHash {
    std::size_t operator()(MaskKey const& key) const
    {
        std::size_t hash = static_cast<std::size_t>(key.code_point);
        hash = hash * 1315423911u ^ static_cast<std::size_t>(key.size_q);
        hash = hash * 1315423911u ^ (static_cast<std::size_t>(key.bold) << 1
                                        | static_cast<std::size_t>(key.italic));
        return hash;
    }
};

class Rasterizer {
public:
    std::unordered_map<char32_t, std::vector<Seg> const*> index;
    std::vector<GlyphDef> glyphs = build_glyphs();
    std::unordered_map<MaskKey, GlyphMask, MaskKeyHash> cache;
    std::deque<std::vector<Seg>> composed; // stable addresses for the index

    Rasterizer()
    {
        for (GlyphDef const& glyph : glyphs)
            index.emplace(glyph.code_point, &glyph.segments);
    }

    // Combining marks are designed over an x-height base; above-marks lift
    // by this much over capitals and ascenders, and stack by 5 more each.
    static constexpr int tall_lift = -6;

    static bool is_below_mark(char32_t mark)
    {
        return mark == 0x0323 || mark == 0x0327 || mark == 0x0328 || mark == 0x0331;
    }

    static bool is_side_mark(char32_t mark) { return mark == 0x031B; }

    static bool is_tall_base(char32_t base)
    {
        return is_ascii_upper_alpha(base) || base == U'b' || base == U'd' || base == U'f'
            || base == U'h' || base == U'k' || base == U'l' || base == U't' || base == 0x00DF
            || base == 0x00FE;
    }

    // Spellings that share another glyph outright.
    static char32_t alias_of(char32_t code_point)
    {
        switch (code_point) {
        case 0x2010: case 0x2011: case 0x2043: return U'-'; // hyphens
        case 0x2012: case 0x2212: return 0x2013; // figure dash, minus
        case 0x2015: return 0x2014; // horizontal bar
        case 0x2032: case 0x02BC: return 0x2019; // prime, modifier apostrophe
        case 0x2033: return 0x201D; // double prime
        case 0x2035: return 0x2018; // reversed prime
        case 0x2044: return U'/'; // fraction slash
        case 0x2717: return 0x00D7; // ballot x
        default: break;
        }
        if (code_point >= 0xFF01 && code_point <= 0xFF5E)
            return code_point - 0xFF01 + 0x21; // fullwidth ASCII
        return 0;
    }

    // How a precomposed letter is built: the base glyph, then each mark with
    // its vertical lift on the grid (negative is up).
    struct Composition {
        std::vector<Seg> const* base;
        struct Mark {
            std::vector<Seg> const* segments;
            int lift;
        };
        std::vector<Mark> marks;
    };

    // Letters with diacritics: the base glyph plus the marks of the
    // canonical decomposition (fully expanded by tools/gen-unicode).
    std::optional<Composition> compose(char32_t code_point)
    {
        std::u32string_view const parts = canonical_decomposition(code_point);
        if (parts.size() < 2)
            return std::nullopt;
        char32_t base = parts[0];
        bool has_above = false;
        for (char32_t const mark : parts.substr(1))
            has_above = has_above || (!is_below_mark(mark) && !is_side_mark(mark));
        if (has_above && base == U'i')
            base = 0x0131; // the dot yields to the mark
        if (has_above && base == U'j')
            base = 0x0237;
        std::vector<Seg> const* const base_segments = find(base);
        if (!base_segments)
            return std::nullopt;
        Composition composition { base_segments, {} };
        int above_count = 0;
        for (char32_t const mark : parts.substr(1)) {
            auto const mark_it = index.find(mark);
            if (mark_it == index.end())
                return std::nullopt;
            int lift = 0;
            if (!is_below_mark(mark) && !is_side_mark(mark)) {
                lift = (is_tall_base(base) ? tall_lift : 0) - 5 * above_count;
                ++above_count;
            }
            composition.marks.push_back(Composition::Mark { mark_it->second, lift });
        }
        return composition;
    }

    std::vector<Seg> const* find(char32_t code_point)
    {
        if (auto const it = index.find(code_point); it != index.end())
            return it->second;
        if (char32_t const alias = alias_of(code_point)) {
            if (std::vector<Seg> const* const segments = find(alias)) {
                index.emplace(code_point, segments);
                return segments;
            }
        }
        std::optional<Composition> const composition = compose(code_point);
        if (!composition)
            return nullptr;
        std::vector<Seg> glyph = *composition->base;
        for (Composition::Mark const& mark : composition->marks) {
            for (Seg seg : *mark.segments) {
                seg.b = static_cast<std::int8_t>(seg.b + mark.lift);
                seg.d = static_cast<std::int8_t>(seg.d + mark.lift);
                glyph.push_back(seg);
            }
        }
        composed.push_back(std::move(glyph));
        index.emplace(code_point, &composed.back());
        return &composed.back();
    }

    GlyphMask const& mask_for(char32_t code_point, int size_q, bool bold, bool italic)
    {
        MaskKey const key { code_point, size_q, bold, italic };
        if (auto const it = cache.find(key); it != cache.end())
            return it->second;

        std::vector<Seg> const* segments = find(code_point);
        if (!segments)
            segments = find(0xFFFD);

        GlyphMask mask;
        // Scale: pixels = grid * size / 32. In fixed point: the glyph grid is
        // held in quarter-units; one grid unit = size_q/32 quarter-pixels.
        // A point at grid-quarter g maps to quarter-pixels g * size_q / 128.
        std::int64_t const size_quarters = size_q;
        auto const to_quarter_px = [&](std::int64_t grid_quarters) {
            return grid_quarters * size_quarters / 128;
        };

        int const weight = bold ? 4 : 3;
        std::vector<Quad> quads;
        for (Seg const& seg : *segments) {
            Quad quad = expand(seg, weight);
            for (Point& point : quad) {
                if (italic) {
                    // Oblique shear: x grows as y rises above the baseline.
                    std::int64_t const baseline = 25 * 4;
                    point.x += (baseline - point.y) / 4;
                }
                point.x = to_quarter_px(point.x);
                point.y = to_quarter_px(point.y);
            }
            quads.push_back(quad);
        }

        // Pixel bounds of the em box, padded for stroke overhang, AA bleed,
        // and the italic shear.
        int const em_px = (size_q + 3) / 4;
        mask.left = -2;
        mask.top = -8; // room for stacked marks over a capital
        mask.width = em_px * 20 / 32 + 5 + (italic ? em_px / 4 + 1 : 0);
        mask.height = em_px + 11;
        mask.alpha.assign(static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height), 0);

        // Sample points double once so subcell centers stay integral.
        std::vector<Quad> doubled(quads.size());
        for (std::size_t i = 0; i < quads.size(); ++i) {
            for (std::size_t j = 0; j < 4; ++j)
                doubled[i][j] = Point { quads[i][j].x * 2, quads[i][j].y * 2 };
        }

        for (int py = 0; py < mask.height; ++py) {
            for (int px = 0; px < mask.width; ++px) {
                // 4x4 subsamples at quarter-pixel centers.
                int hits = 0;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sx = 0; sx < 4; ++sx) {
                        std::int64_t const qx = (static_cast<std::int64_t>(px) + mask.left) * 4 + sx;
                        std::int64_t const qy = (static_cast<std::int64_t>(py) + mask.top) * 4 + sy;
                        for (Quad const& quad : doubled) {
                            if (inside(quad, qx * 2 + 1, qy * 2 + 1)) {
                                ++hits;
                                break;
                            }
                        }
                    }
                }
                mask.alpha[static_cast<std::size_t>(py) * static_cast<std::size_t>(mask.width)
                    + static_cast<std::size_t>(px)]
                    = static_cast<std::uint8_t>(hits * 255 / 16);
            }
        }

        auto const [it, inserted] = cache.emplace(key, std::move(mask));
        (void)inserted;
        return it->second;
    }
};

// Draws nothing: spaces of every width and the default-ignorable code points
// (soft hyphen, zero-width joiners, bidi marks, variation selectors).
bool is_blank(char32_t code_point)
{
    return code_point == U' ' || code_point == 0x00A0
        || (code_point >= 0x2000 && code_point <= 0x200A) || code_point == 0x2028
        || code_point == 0x2029 || code_point == 0x202F || code_point == 0x205F
        || code_point == 0x3000 || is_default_ignorable(code_point);
}

Rasterizer& rasterizer()
{
    static Rasterizer instance;
    return instance;
}

} // namespace

SashfoldMono const& SashfoldMono::instance()
{
    static SashfoldMono font;
    return font;
}

void SashfoldMono::draw_glyph(Bitmap& target, char32_t code_point, float x, float baseline_y,
    float size, Color color, bool bold, bool italic) const
{
    if (code_point < 0x21 || is_blank(code_point))
        return;
    int const size_q = static_cast<int>(size * 4.0f + 0.5f);
    if (size_q <= 0)
        return;
    GlyphMask const& mask = rasterizer().mask_for(code_point, size_q, bold, italic);

    float const ascent = design_ascent * size / static_cast<float>(units_per_em);
    int const origin_x = static_cast<int>(x + 0.5f) + mask.left;
    int const origin_y = static_cast<int>(baseline_y - ascent + 0.5f) + mask.top;
    for (int py = 0; py < mask.height; ++py) {
        for (int px = 0; px < mask.width; ++px) {
            std::uint8_t const alpha = mask.alpha[static_cast<std::size_t>(py)
                    * static_cast<std::size_t>(mask.width)
                + static_cast<std::size_t>(px)];
            if (alpha == 0)
                continue;
            Color shaded = color;
            shaded.a = static_cast<std::uint8_t>(static_cast<int>(color.a) * alpha / 255);
            target.blend_pixel(origin_x + px, origin_y + py, shaded);
        }
    }
}

std::vector<std::uint8_t> SashfoldMono::to_truetype(TrueTypeOptions const& options) const
{
    Rasterizer& faces = rasterizer();
    // 2048 units per em: one grid unit is 64, one quarter-unit 16.
    constexpr int unit = 16;
    constexpr int baseline_quarters = design_ascent * 4;
    int const weight = options.bold ? 4 : 3;
    auto const advance = static_cast<std::uint16_t>(design_advance * 64);
    auto const wanted = [&](char32_t code_point) {
        return options.only.empty() || options.only.find(code_point) != std::u32string_view::npos;
    };

    FontDescription font;
    font.family = "Sashfold Mono";
    font.subfamily = options.bold ? (options.italic ? "Bold Italic" : "Bold")
                                  : (options.italic ? "Italic" : "Regular");
    font.units_per_em = 2048;
    font.ascender = design_ascent * 64;
    font.descender = -design_descent * 64;
    font.line_gap = 0;
    font.x_height = (design_ascent - 10) * 64;
    font.cap_height = (design_ascent - 3) * 64;
    font.weight_class = options.bold ? 700 : 400;
    font.italic = options.italic;
    font.fixed_pitch = true;
    font.long_loca = options.long_loca;

    // Each stroke becomes one clockwise quad; overlapping quads fill as a
    // union under nonzero winding, which is how the stroke rasterizer sees
    // them too.
    auto const outline_of = [&](std::vector<Seg> const& segments) {
        GlyphOutline outline;
        for (Seg const& seg : segments) {
            Quad const quad = expand(seg, weight);
            std::array<GlyphPoint, 4> corners;
            for (std::size_t i = 0; i < 4; ++i) {
                Point point = quad[i];
                if (options.italic)
                    point.x += (baseline_quarters - point.y) / 4;
                corners[i] = GlyphPoint { static_cast<std::int16_t>(point.x * unit),
                    static_cast<std::int16_t>((baseline_quarters - point.y) * unit), true };
            }
            long long area = 0; // shoelace, y up: clockwise is negative
            for (std::size_t i = 0; i < 4; ++i) {
                GlyphPoint const& a = corners[i];
                GlyphPoint const& b = corners[(i + 1) % 4];
                area += static_cast<long long>(a.x) * b.y - static_cast<long long>(b.x) * a.y;
            }
            if (area == 0)
                continue; // a zero-length stroke draws nothing
            if (area > 0)
                std::reverse(corners.begin(), corners.end());
            for (GlyphPoint const& corner : corners)
                outline.points.push_back(corner);
            outline.contour_ends.push_back(static_cast<std::uint16_t>(outline.points.size() - 1));
        }
        return outline;
    };

    std::map<std::vector<Seg> const*, std::uint16_t> glyph_of;
    auto const glyph_for = [&](std::vector<Seg> const* segments) {
        if (auto const it = glyph_of.find(segments); it != glyph_of.end())
            return it->second;
        auto const id = static_cast<std::uint16_t>(font.glyphs.size());
        font.glyphs.push_back(WriterGlyph { outline_of(*segments), {}, advance });
        glyph_of.emplace(segments, id);
        return id;
    };

    // Glyph 0 draws the box; glyph 1 is the space, and every blank maps to it.
    std::vector<Seg> const* const box = faces.find(0xFFFD);
    font.glyphs.push_back(WriterGlyph { box ? outline_of(*box) : GlyphOutline {}, {}, advance });
    font.glyphs.push_back(WriterGlyph { GlyphOutline {}, {}, advance });
    std::vector<char32_t> blanks { 0x20, 0xA0, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000 };
    for (char32_t c = 0x2000; c <= 0x200A; ++c)
        blanks.push_back(c);
    for (char32_t const blank : blanks) {
        if (wanted(blank))
            font.mappings.emplace_back(blank, 1);
    }

    // The designed glyphs, then every alias and composed letter the face can
    // reach: aliases share the glyph, compositions become composites.
    std::unordered_set<char32_t> designed;
    for (GlyphDef const& def : faces.glyphs) {
        designed.insert(def.code_point);
        if (wanted(def.code_point))
            font.mappings.emplace_back(def.code_point, glyph_for(&def.segments));
    }
    for (char32_t c = 0x21; c <= 0xFFFF; ++c) {
        if (!wanted(c) || designed.contains(c))
            continue;
        if (char32_t const alias = Rasterizer::alias_of(c)) {
            if (std::vector<Seg> const* const target = faces.find(alias))
                font.mappings.emplace_back(c, glyph_for(target));
            continue;
        }
        std::optional<Rasterizer::Composition> const composition = faces.compose(c);
        if (!composition)
            continue;
        WriterGlyph composite;
        composite.advance = advance;
        composite.components.push_back(WriterComponent { glyph_for(composition->base), 0, 0 });
        for (Rasterizer::Composition::Mark const& mark : composition->marks) {
            composite.components.push_back(WriterComponent { glyph_for(mark.segments), 0,
                static_cast<std::int16_t>(-mark.lift * 64) });
        }
        font.mappings.emplace_back(c, static_cast<std::uint16_t>(font.glyphs.size()));
        font.glyphs.push_back(std::move(composite));
    }
    return write_truetype(font);
}

}
