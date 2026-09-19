// gen-font: writes Sashfold Mono as a TrueType file. The regular face is the
// fixture the reader's tests read back (tests/fixtures/fonts/SashfoldMono.ttf);
// test_truetype regenerates it in memory, so the committed bytes can never
// drift from the face. With --seed it writes a small face for the fuzzing
// corpus instead; with --test-sans, a face that is not Sashfold Mono at all.
//
//   gen_font <out.ttf> [--bold] [--italic] [--seed | --test-sans]
//
// Sashfold Test Sans is a measuring stick for what is set in a proportional
// face — the chrome's words, where the machine has fonts. Its letters are
// plain boxes, so that a pixel inside one is the ink's color exactly, and
// they are of three widths on a thousand units to the em: f, i, j, l, r and
// t and the space 250, m and w 900, the rest of a to z 500, each box 50 in
// from either side and 700 tall; it rises 800 and falls 200; and a before
// m is kerned 100 closer. At 20 px those are 5, 18 and 10 px, a box 14 px
// tall, and 2 px of kerning: whole pixels, which a test can count.

#include "text/SashfoldMono.h"
#include "text/TrueTypeWriter.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int usage()
{
    std::cerr << "usage: gen_font <out.ttf> [--bold] [--italic] [--seed | --test-sans]\n";
    return 2;
}

std::vector<std::uint8_t> test_sans()
{
    using namespace sashfold::text;
    FontDescription font;
    font.family = "Sashfold Test Sans";
    font.units_per_em = 1000;
    font.ascender = 800;
    font.descender = -200;
    font.x_height = 700;
    font.cap_height = 700;
    auto const box = [](std::uint16_t advance) {
        WriterGlyph glyph;
        glyph.advance = advance;
        auto const right = static_cast<std::int16_t>(advance - 50);
        glyph.outline.points = { { 50, 0, true }, { 50, 700, true }, { right, 700, true }, { right, 0, true } };
        glyph.outline.contour_ends = { 3 };
        glyph.outline.x_min = 50;
        glyph.outline.y_min = 0;
        glyph.outline.x_max = right;
        glyph.outline.y_max = 700;
        return glyph;
    };
    WriterGlyph space;
    space.advance = 250;
    // Glyph 0 is the one for what the face lacks; then the space and the three widths.
    font.glyphs = { box(500), space, box(250), box(500), box(900) };
    font.mappings.emplace_back(U' ', std::uint16_t { 1 });
    for (char32_t letter = U'a'; letter <= U'z'; ++letter) {
        bool const narrow = letter == U'i' || letter == U'j' || letter == U'l' || letter == U'f' || letter == U't'
            || letter == U'r';
        bool const wide = letter == U'm' || letter == U'w';
        font.mappings.emplace_back(letter, static_cast<std::uint16_t>(narrow ? 2 : wide ? 4 : 3));
    }
    // A pair is of glyphs: every letter of the middle width before every
    // wide one, a before m among them.
    font.kerning.push_back({ 3, 4, -100 });
    return write_truetype(font);
}

int main(int argc, char** argv)
{
    std::string output;
    sashfold::text::TrueTypeOptions options;
    bool seed = false;
    bool sans = false;
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--bold")
            options.bold = true;
        else if (arg == "--italic")
            options.italic = true;
        else if (arg == "--seed")
            seed = true;
        else if (arg == "--test-sans")
            sans = true;
        else if (arg.starts_with("-"))
            return usage();
        else
            output = arg;
    }
    if (output.empty())
        return usage();

    // The seed keeps one of everything: a capital, a lowercase, a composed
    // letter over each, an alias, the box, and the space.
    std::u32string seed_set = U" AEae-";
    for (char32_t const c : { 0x00C9u, 0x00E9u, 0x2010u, 0xFF21u, 0xFFFDu })
        seed_set.push_back(c);
    if (seed)
        options.only = seed_set;

    std::vector<std::uint8_t> const bytes
        = sans ? test_sans() : sashfold::text::SashfoldMono::instance().to_truetype(options);
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        std::cerr << "error: could not write " << output << "\n";
        return 1;
    }
    std::cout << "wrote " << output << " (" << bytes.size() << " bytes)\n";
    return 0;
}
