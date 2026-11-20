#include "Test.h"

#include "core/Bitmap.h"
#include "layout/RunGlyphs.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/ShapedText.h"
#include "text/Shaper.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Text as a page sets it (text/ShapedText.h): a string in a style's faces
// becomes glyphs with faces, clusters and places. With the built-in face
// alone the result is the fixed-pitch one every reference picture is made
// with; with fonts from the machine (handed over as a page's fonts, so the
// test does not depend on what else is installed) the glyphs are exactly
// what the shaper gives each piece, the pieces split by level, script and
// face, joined with their context, spaced per cluster.

using namespace sashfold;
using text::FontManager;
using text::FontRequest;
using text::FontStack;
using text::ShapedText;
using text::TextShaping;

namespace {

std::optional<std::vector<std::uint8_t>> font_bytes(std::string const& name)
{
    for (char const* dir : { "/usr/share/fonts/noto/", "/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/noto/",
             "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/opentype/noto/" }) {
        std::string const path = std::string(dir) + name;
        if (!std::filesystem::exists(path))
            continue;
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::cerr << "SKIP " << name << ": not installed\n";
    return std::nullopt;
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

// The shaper's own answer for a run in one face: its advance in px.
float shaper_width(text::Face const& face, std::u32string_view text, text::TextDirection direction, float size,
    text::ScriptCode script, std::u32string_view before = {}, std::u32string_view after = {})
{
    text::ShapeRequest request;
    request.text = text;
    request.before = before;
    request.after = after;
    request.direction = direction;
    request.script = script;
    request.beginning_of_text = before.empty();
    request.end_of_text = after.empty();
    text::ShapedRun const run = text::shape(face, request);
    return static_cast<float>(run.advance()) * size / static_cast<float>(run.units_per_em);
}

}

int main()
{
    FontManager& manager = FontManager::instance();
    manager.set_system_fonts(false);

    // --- The built-in face alone: a glyph per code point, count times advance ----------
    {
        FontStack const& stack = manager.resolve(FontRequest { { "serif" }, 400, false });
        CHECK(stack.builtin_alone());
        float const advance = stack.faces()[0]->advance(0, 16);
        TextShaping shaping;
        shaping.size = 16;
        auto const plain = stack.shape(U"office", shaping);
        CHECK_EQ(plain->glyphs.size(), 6u);
        CHECK(plain->width == 6.0f * advance);
        CHECK_EQ(plain->glyphs[1].glyph, static_cast<std::uint32_t>(U'f'));
        CHECK_EQ(plain->cluster_starts.size(), 6u);
        // A run of one right-to-left level: drawn last character first,
        // its brackets mirrored.
        shaping.direction = text::TextDirection::RightToLeft;
        auto const turned = stack.shape(U"(ab", shaping);
        CHECK_EQ(turned->glyphs[0].glyph, static_cast<std::uint32_t>(U'b'));
        CHECK_EQ(turned->glyphs[2].glyph, static_cast<std::uint32_t>(U')'));
        CHECK_EQ(turned->glyphs[0].cluster, 2u);
        // Spacing: a letter's worth after each character, a word's worth
        // after each word separator, the no-break space among them.
        shaping.direction = text::TextDirection::LeftToRight;
        shaping.letter_spacing = 2;
        shaping.word_spacing = 5;
        auto const spaced = stack.shape(U"a b c", shaping);
        CHECK(spaced->width == 5.0f * advance + 2.0f * 5 + 5 + 5);
        CHECK(near(spaced->pen_end, spaced->width));
        CHECK(stack.measure(U"office", 16) == 6.0f * advance);
    }

    std::optional<std::vector<std::uint8_t>> const latin = font_bytes("DejaVuSans.ttf");
    std::optional<std::vector<std::uint8_t>> const arabic = font_bytes("NotoNaskhArabic-Regular.ttf");
    std::optional<std::vector<std::uint8_t>> const hebrew = font_bytes("NotoSansHebrew-Regular.ttf");
    if (!latin || !arabic || !hebrew)
        return test::report("shaped_text");
    manager.set_page_fonts({ text::PageFont { "TestLatin", 400, false, *latin, 100, {} },
        text::PageFont { "TestArabic", 400, false, *arabic, 100, {} },
        text::PageFont { "TestHebrew", 400, false, *hebrew, 100, {} } });
    // The Latin face (which has Arabic and Hebrew letters of its own) last,
    // so the others answer their scripts.
    FontStack const& stack = manager.resolve(FontRequest { { "TestArabic", "TestHebrew", "TestLatin" }, 400, false });
    CHECK_EQ(stack.faces().size(), 4u);
    text::Face const& arabic_face = *stack.faces()[0];
    text::Face const& hebrew_face = *stack.faces()[1];
    text::Face const& latin_face = *stack.faces()[2];
    float const size = 32;

    // --- Latin: the shaper's glyphs, ligatures as clusters ------------------------------
    {
        TextShaping shaping;
        shaping.size = size;
        auto const office = stack.shape(U"office", shaping);
        CHECK(near(office->width, shaper_width(latin_face, U"office", text::TextDirection::LeftToRight, size, text::scripts::latin)));
        // DejaVu Sans sets "ffi" as one glyph: one cluster, its room shared
        // evenly among its three characters.
        std::cout << "  office: " << office->glyphs.size() << " glyphs, " << office->cluster_starts.size() << " clusters\n";
        CHECK(office->glyphs.size() < 6u);
        CHECK(!office->starts_cluster(2));
        CHECK_EQ(office->cluster_start_of(2), 1u);
        std::vector<float> left;
        std::vector<float> right;
        office->character_edges(left, right);
        CHECK(near(right[1] - left[1], right[2] - left[2]));
        CHECK(near(right[1], left[2]));
        // A caret inside the ligature stands a third of the way through it.
        std::vector<float> const carets = layout::caret_stops(*office);
        CHECK_EQ(carets.size(), 7u);
        CHECK(near(carets[2] - carets[1], (carets[4] - carets[1]) / 3));
        CHECK(near(carets[6], office->width));
        // Parts of a word shaped whole: the ligature goes with the part its
        // cluster starts in, and the parts' widths add up to the whole's.
        auto const head = text::part_of(*office, 0, 2);
        auto const middle = text::part_of(*office, 2, 3);
        auto const tail = text::part_of(*office, 3, 6);
        CHECK(near(head->width + middle->width + tail->width, office->width));
        CHECK(middle->glyphs.empty());
        // letter-spacing: optional ligatures off, a letter's worth after
        // each cluster, so six of them; per glyph would be fewer.
        shaping.letter_spacing = 3;
        text::shaping_features_for_css(text::CssFontFeatures { text::CssFontFeatures::Kerning::Auto, true, false, false, true, true },
            shaping.features);
        auto const spaced = stack.shape(U"office", shaping);
        CHECK_EQ(spaced->glyphs.size(), 6u);
        TextShaping unspaced;
        unspaced.size = size;
        unspaced.features = shaping.features;
        CHECK(near(spaced->width, stack.shape(U"office", unspaced)->width + 6 * 3));
        // font-kerning: none comes through as the feature turned off.
        TextShaping kerned;
        kerned.size = size;
        TextShaping flat = kerned;
        flat.features.push_back(text::FeatureSetting { text::feature_tag('k', 'e', 'r', 'n'), 0 });
        CHECK(stack.shape(U"AVAV", kerned)->width < stack.shape(U"AVAV", flat)->width);
    }

    // --- Arabic: right to left, joined, context across the edge -------------------------
    {
        std::u32string const word = U"سلام"; // salam
        TextShaping rtl;
        rtl.size = size;
        rtl.direction = text::TextDirection::RightToLeft;
        auto const shaped = stack.shape(word, rtl);
        CHECK(near(shaped->width, shaper_width(arabic_face, word, text::TextDirection::RightToLeft, size, text::scripts::arabic)));
        CHECK(shaped->glyphs.front().face == &arabic_face);
        // Visual order: the first glyph on the left is the word's end.
        CHECK(shaped->glyphs.front().cluster >= 2u);
        CHECK_EQ(shaped->glyphs.back().cluster, 0u);
        CHECK_EQ(shaped->right_to_left.size(), word.size());
        // A caret before the first letter stands at the word's right edge,
        // after the last at its left.
        std::vector<float> const carets = layout::caret_stops(*shaped);
        CHECK(near(carets.front(), shaped->width));
        CHECK(near(carets.back(), 0));
        // A letter alone takes its isolated form; with the letters around
        // it as context, its medial one.
        std::u32string const lam = U"ل";
        auto const alone = stack.shape(lam, rtl);
        auto const joined = stack.shape(lam, rtl, U"س", U"م");
        CHECK(alone->glyphs.size() == 1u && joined->glyphs.size() == 1u);
        CHECK(alone->glyphs[0].glyph != joined->glyphs[0].glyph);
        CHECK(near(joined->width, shaper_width(arabic_face, lam, text::TextDirection::RightToLeft, size, text::scripts::arabic,
                                      U"س", U"م")));
        // letter-spacing goes after a cluster, not after each glyph in it:
        // a letter with its vowel mark takes one letter's worth.
        TextShaping spaced = rtl;
        spaced.letter_spacing = 3;
        auto const marked = stack.shape(U"بَ", rtl);
        CHECK(marked->glyphs.size() >= 2u);
        CHECK_EQ(marked->cluster_starts.size(), 1u);
        CHECK(near(stack.shape(U"بَ", spaced)->width, marked->width + 3));
        // The cache keeps them apart.
        CHECK(stack.shape(lam, rtl)->glyphs[0].glyph == alone->glyphs[0].glyph);
    }

    // --- A run against its script's direction (bidi-override) --------------------------
    {
        // Latin at an odd level reads right to left: "abcd" drawn "dcba";
        // Hebrew at an even level left to right, as written.
        TextShaping rtl;
        rtl.size = size;
        rtl.direction = text::TextDirection::RightToLeft;
        auto const latin_rtl = stack.shape(U"abcd", rtl);
        CHECK_EQ(latin_rtl->glyphs.front().cluster, 3u);
        auto const hebrew_rtl = stack.shape(U"אבגד", rtl);
        CHECK_EQ(hebrew_rtl->glyphs.front().cluster, 3u);
        TextShaping ltr;
        ltr.size = size;
        auto const hebrew_ltr = stack.shape(U"אבגד", ltr);
        CHECK_EQ(hebrew_ltr->glyphs.front().cluster, 0u);
    }

    // --- A paragraph: levels resolved, faces by cluster, fallback ------------------------
    {
        TextShaping paragraph;
        paragraph.size = size;
        paragraph.paragraph = true;
        std::u32string const mixed = U"ab אב cd";
        auto const shaped = stack.shape(mixed, paragraph);
        CHECK_EQ(shaped->right_to_left.size(), mixed.size());
        CHECK(shaped->right_to_left[3] && shaped->right_to_left[4] && !shaped->right_to_left[0]);
        // Hebrew in the Hebrew face, reversed in place: alef right of bet.
        std::size_t alef = 0;
        std::size_t bet = 0;
        for (std::size_t i = 0; i < shaped->glyphs.size(); ++i) {
            if (shaped->glyphs[i].cluster == 3)
                alef = i;
            if (shaped->glyphs[i].cluster == 4)
                bet = i;
        }
        CHECK(alef > bet);
        CHECK(shaped->glyphs[alef].face == &hebrew_face);
        // A base and its mark stay in one face: a Hebrew point after a
        // Latin letter goes with the letter's face.
        auto const marked = stack.shape(U"aָ", paragraph);
        CHECK(marked->glyphs.front().face == &latin_face);
        // Nothing has it: the built-in face draws the box.
        auto const missing = stack.shape(U"a一", paragraph);
        CHECK(missing->glyphs.back().face == &text::builtin_face());
        CHECK_EQ(missing->glyphs.back().glyph, 0x4E00u);
    }

    // --- Threads: one stack, the faces' glyph caches, shaped and drawn at once -----------
    {
        std::atomic<int> wrong { 0 };
        float const expected = stack.measure(U"Shaped text, drawn سلام", 20);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                Bitmap target(64, 64, Color::rgba(0, 0, 0, 0));
                for (int i = 0; i < 300; ++i) {
                    TextShaping shaping;
                    shaping.size = 10.0f + static_cast<float>((i + t) % 17);
                    shaping.paragraph = true;
                    auto const shaped = stack.shape(U"Shaped text, drawn سلام", shaping);
                    for (text::PlacedGlyph const& g : shaped->glyphs)
                        g.face->draw_glyph(target, g.glyph, 4, 40, shaping.size, Color::rgb(0, 0, 0), false, false);
                    if (stack.measure(U"Shaped text, drawn سلام", 20) != expected)
                        ++wrong;
                }
            });
        }
        for (std::thread& thread : threads)
            thread.join();
        CHECK_EQ(wrong.load(), 0);
    }

    return test::report("shaped_text");
}
