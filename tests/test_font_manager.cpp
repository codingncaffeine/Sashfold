#include "Test.h"

#include "core/Bitmap.h"
#include "core/Png.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"
#include "text/TrueTypeWriter.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The font manager: with system fonts off every request is the built-in
// face; with them on, the machine's fonts answer by family, weight and
// slant, generic families find something, and a code point no listed face
// has falls back through the catalogue and finally to the built-in box.
// The system half runs against whatever the machine offers, so it asserts
// shape, not names — except where a font is known to exist on the OS.

using namespace sashfold;
using text::FontManager;
using text::FontRequest;
using text::FontStack;

namespace {

// A 4 px red square as a PNG: the picture the test's emoji font draws.
std::vector<std::uint8_t> core_png_4x4_red()
{
    return encode_png(Bitmap(4, 4, Color::rgb(255, 0, 0)));
}

}

int main()
{
    FontManager& manager = FontManager::instance();
    text::Face const& builtin = text::builtin_face();

    // --- Built-in only ---------------------------------------------------------------
    manager.set_system_fonts(false);
    CHECK(!manager.system_fonts());
    FontStack const& plain = manager.resolve(FontRequest { { "Arial", "sans-serif" }, 700, true });
    CHECK_EQ(plain.faces().size(), 1u);
    CHECK(&plain.primary() == &builtin);
    CHECK(&plain.face_for(U'A') == &builtin);
    CHECK(&plain.face_for(0x4E00) == &builtin);
    CHECK(&manager.resolve(FontRequest { { "Arial", "sans-serif" }, 700, true }) == &plain);
    CHECK(&manager.resolve(FontRequest { {}, 400, false }).primary() == &builtin);

    // --- Page fonts -------------------------------------------------------------------
    // A page's @font-face fonts answer their family with system fonts off
    // or on; the same bytes on the next page reuse the parsed face; the
    // stacks handed out before stay alive for the layouts that hold them.
    std::vector<std::uint8_t> const ttf = text::SashfoldMono::instance().to_truetype();
    manager.set_page_fonts({ text::PageFont { "Ahem", 400, false, ttf, 100, {} } });
    CHECK_EQ(manager.page_font_count(), 1u);
    FontStack const& page = manager.resolve(FontRequest { { "ahem", "serif" }, 400, false });
    CHECK_EQ(page.faces().size(), 2u);
    CHECK(&page.primary() != &builtin);
    CHECK(page.primary().glyph_index(U'A') != 0);
    CHECK(&page.face_for(U'A') == &page.primary());
    CHECK(page.faces().back() == &builtin);
    CHECK_EQ(plain.faces().size(), 1u); // retired, not destroyed
    CHECK(&manager.resolve(FontRequest { { "Arial", "sans-serif" }, 700, true }) != &plain);
    // The same set again changes nothing: the stacks stand.
    manager.set_page_fonts({ text::PageFont { "Ahem", 400, false, ttf, 100, {} } });
    CHECK(&manager.resolve(FontRequest { { "ahem", "serif" }, 400, false }) == &page);
    // Weight and slant pick among a family's faces; a face of the other
    // slant is the last resort.
    text::TrueTypeOptions bold_options;
    bold_options.bold = true;
    std::vector<std::uint8_t> const bold_ttf = text::SashfoldMono::instance().to_truetype(bold_options);
    manager.set_page_fonts({ text::PageFont { "Ahem", 400, false, ttf, 100, {} },
        text::PageFont { "Ahem", 700, false, bold_ttf, 100, {} }, text::PageFont { "Solo", 400, true, ttf, 100, {} } });
    CHECK_EQ(manager.page_font_count(), 3u);
    text::Face const& page_regular = manager.resolve(FontRequest { { "Ahem" }, 400, false }).primary();
    text::Face const& page_bold = manager.resolve(FontRequest { { "Ahem" }, 700, false }).primary();
    CHECK(&page_regular != &page_bold);
    CHECK(&manager.resolve(FontRequest { { "Ahem" }, 500, false }).primary() == &page_regular);
    CHECK(&manager.resolve(FontRequest { { "Ahem" }, 600, false }).primary() == &page_bold);
    CHECK(&manager.resolve(FontRequest { { "Ahem" }, 400, true }).primary() == &page_regular);
    CHECK(&manager.resolve(FontRequest { { "Solo" }, 400, false }).primary() != &builtin);
    // font-stretch picks among a family's faces: the nearest at or below a
    // normal-or-narrower request, the nearest at or above a wider one, and
    // the other side when nothing is on the right one.
    manager.set_page_fonts({ text::PageFont { "Wide", 400, false, ttf, 100, {} },
        text::PageFont { "Wide", 400, false, ttf, 200, {} } });
    {
        auto const face_at = [&](int stretch) {
            FontRequest request { { "Wide" }, 400, false };
            request.stretch = stretch;
            return &manager.resolve(request).primary();
        };
        text::Face const* const normal_face = face_at(100);
        text::Face const* const wide_face = face_at(200);
        CHECK(normal_face != wide_face);
        CHECK(normal_face != &builtin && wide_face != &builtin);
        CHECK(face_at(150) == wide_face);
        CHECK(face_at(125) == wide_face);
        CHECK(face_at(75) == normal_face);
        CHECK(face_at(50) == normal_face);
    }
    // unicode-range keeps a face to its code points: a family split into
    // pieces answers character by character, and the first available font
    // — the one the line's metrics come from — is the first whose range
    // has the space in it.
    manager.set_page_fonts({ text::PageFont { "Split", 400, false, ttf, 100, { { 0x41, 0x5A } } },
        text::PageFont { "Split", 400, false, ttf, 100, { { 0x61, 0x7A } } } });
    {
        FontStack const& split = manager.resolve(FontRequest { { "Split" }, 400, false });
        if (CHECK_EQ(split.faces().size(), 3u)) {
            CHECK(&split.face_for(U'A') == split.faces()[0]);
            CHECK(&split.face_for(U'a') == split.faces()[1]);
            CHECK(&split.face_for(U'0') == &builtin);
            CHECK_EQ(split.faces()[0]->glyph_index(U'a'), 0u);
            CHECK(split.faces()[0]->glyph_index(U'A') != 0);
            CHECK(&split.primary() == &builtin); // neither piece has a space
        }
    }
    manager.set_page_fonts({ text::PageFont { "Split", 400, false, ttf, 100, { { 0x41, 0x5A } } },
        text::PageFont { "Split", 400, false, ttf, 100, {} } });
    {
        FontStack const& split = manager.resolve(FontRequest { { "Split" }, 400, false });
        if (CHECK_EQ(split.faces().size(), 3u)) {
            CHECK(&split.primary() == split.faces()[1]); // the whole face has the space
            CHECK(&split.face_for(U'A') == split.faces()[0]);
            CHECK(&split.face_for(U'a') == split.faces()[1]);
        }
    }
    // Bytes that are not a font register nothing, and the family falls through.
    manager.set_page_fonts({ text::PageFont { "Junk", 400, false, { 1, 2, 3 }, 100, {} } });
    CHECK_EQ(manager.page_font_count(), 0u);
    CHECK(&manager.resolve(FontRequest { { "Junk", "Ahem" }, 400, false }).primary() == &builtin);
    manager.set_page_fonts({});
    CHECK(&manager.resolve(FontRequest { { "ahem", "serif" }, 400, false }).primary() == &builtin);

    // --- Kerning ------------------------------------------------------------------------
    // A page font that kerns AV measures the pair closer than its two
    // advances, unless kerning is turned off; a pair the font does not
    // name, and a pair spanning two faces, measure as their advances.
    {
        text::FontDescription description;
        description.family = "Kern";
        description.units_per_em = 2048;
        description.ascender = 1600;
        description.descender = -400;
        for (int i = 0; i < 5; ++i) {
            // A triangle each: a font with no outlines at all is not a font
            // the manager will register.
            text::WriterGlyph glyph;
            glyph.advance = 1000;
            glyph.outline.points = { { 0, 0, true }, { 500, 1000, true }, { 1000, 0, true } };
            glyph.outline.contour_ends = { 2 };
            description.glyphs.push_back(glyph);
        }
        description.mappings = { { U'A', 1 }, { U'V', 2 }, { U'T', 3 }, { U'o', 4 } };
        description.kerning = { { 1, 2, -100 }, { 3, 4, -80 } };
        description.kerning_table = text::WriterKerningTable::GposPairs;
        manager.set_page_fonts({ text::PageFont { "Kern", 400, false, text::write_truetype(description), 100, {} } });
        FontStack const& kerned = manager.resolve(FontRequest { { "Kern" }, 400, false });
        CHECK(kerned.faces().size() == 2 && kerned.faces()[0] != &builtin);
        CHECK(&kerned.primary() == kerned.faces()[0]); // no unicode-range: the first available font, space glyph or not
        CHECK_EQ(kerned.measure(U"A", 2048), 1000.0f);
        CHECK_EQ(kerned.measure(U"AV", 2048), 1900.0f);
        CHECK_EQ(kerned.measure(U"AV", 2048, false), 2000.0f);
        CHECK_EQ(kerned.measure(U"VA", 2048), 2000.0f);
        CHECK_EQ(kerned.measure(U"To", 1024), 960.0f);
        float const question = kerned.measure(U"?", 2048); // the built-in face's, the font having none
        CHECK_EQ(kerned.measure(U"A?", 2048), 1000.0f + question);
        manager.set_page_fonts({});
    }

    // --- Fonts on demand -----------------------------------------------------------------
    // A font a page declares and does not fetch stands in its family
    // without a glyph to its name; the first code point that gets as far as
    // it — in its range, with no face ahead of it to draw it — has the page
    // asked for it, and where the page has it at hand the stack carries it
    // from that moment on.
    {
        int calls = 0;
        std::string last_asked;
        std::map<std::string, text::PageFontLoad> answers; // by the first source; no entry: the font's bytes
        auto loader = std::make_shared<text::PageFontLoader>([&](text::PageFontWait const& wait) {
            ++calls;
            last_asked = wait.urls.empty() ? std::string() : wait.urls.front();
            if (auto const it = answers.find(last_asked); it != answers.end())
                return it->second;
            text::PageFontLoad load;
            load.outcome = text::PageFontLoad::Outcome::Loaded;
            load.bytes = ttf;
            return load;
        });
        auto const waiting = [&loader](std::string family, int weight, std::string url,
                                 std::vector<std::pair<char32_t, char32_t>> ranges = {}) {
            auto wait = std::make_shared<text::PageFontWait>();
            wait->urls = { std::move(url) };
            wait->loader = loader;
            return text::PageFont { std::move(family), weight, false, {}, 100, std::move(ranges), 0, 0, 0, std::move(wait) };
        };
        text::PageFontCensus const census_before = text::page_font_census();

        // Declared, set, resolved: nothing is asked for.
        std::vector<text::PageFont> declared { waiting("Wait", 400, "https://fonts.test/wait.ttf") };
        manager.set_page_fonts(declared);
        CHECK_EQ(manager.page_font_count(), 0u);
        CHECK_EQ(manager.page_fonts_waiting(), 1u);
        FontStack const& stack = manager.resolve(FontRequest { { "Wait" }, 400, false });
        CHECK_EQ(calls, 0);
        CHECK_EQ(stack.faces().size(), 1u); // the built-in face, so far
        CHECK(!stack.builtin_alone()); // and not a stack that will never have more
        // The first character of text asks, once, and is drawn in the font.
        text::Face const& came = stack.face_for(U'A');
        CHECK_EQ(calls, 1);
        CHECK_EQ(last_asked, std::string("https://fonts.test/wait.ttf"));
        CHECK(&came != &builtin);
        CHECK_EQ(stack.faces().size(), 2u);
        CHECK(&stack.primary() == &came);
        CHECK(&stack.face_for(U'B') == &came);
        CHECK(stack.measure(U"ABC", 16) > 0);
        CHECK_EQ(calls, 1);
        CHECK_EQ(manager.page_font_count(), 1u);
        CHECK_EQ(manager.page_fonts_waiting(), 0u);
        // The page's list knows what came: set again, as a page sets its
        // fonts before every layout, it is the same set and the stack stands;
        // and a copy of the list — a frame's view keeps one — has the font
        // without asking.
        manager.set_page_fonts(declared);
        CHECK(&manager.resolve(FontRequest { { "Wait" }, 400, false }) == &stack);
        std::vector<text::PageFont> const copy = declared;
        manager.set_page_fonts({});
        manager.set_page_fonts(copy);
        CHECK_EQ(manager.page_font_count(), 1u);
        CHECK(&manager.resolve(FontRequest { { "Wait" }, 400, false }).face_for(U'A') == &came);
        CHECK_EQ(calls, 1);

        // The line's metrics are the first available font's: read without a
        // character of text, they ask for it.
        declared = { waiting("Line", 400, "https://fonts.test/line.ttf") };
        manager.set_page_fonts(declared);
        FontStack const& line = manager.resolve(FontRequest { { "Line" }, 400, false });
        CHECK_EQ(calls, 1);
        CHECK(&line.primary() != &builtin);
        CHECK_EQ(calls, 2);
        text::Face const& line_face = line.face_for(U'A');
        CHECK(&line.primary() == &line_face);
        CHECK_EQ(calls, 2);

        // A family in unicode-range pieces is fetched piece by piece, each
        // when a code point in its range arrives; a code point in no range
        // asks for none, and neither do the metrics when no piece has the
        // space.
        declared = { waiting("Parts", 400, "https://fonts.test/upper.ttf", { { 0x41, 0x5A } }),
            waiting("Parts", 400, "https://fonts.test/lower.ttf", { { 0x61, 0x7A } }) };
        manager.set_page_fonts(declared);
        FontStack const& parts = manager.resolve(FontRequest { { "Parts" }, 400, false });
        CHECK(&parts.primary() == &builtin);
        CHECK(&parts.face_for(U'0') == &builtin);
        CHECK_EQ(calls, 2);
        text::Face const& upper = parts.face_for(U'A');
        CHECK(&upper != &builtin);
        CHECK_EQ(calls, 3);
        CHECK_EQ(last_asked, std::string("https://fonts.test/upper.ttf"));
        CHECK_EQ(manager.page_fonts_waiting(), 1u);
        CHECK(&parts.face_for(U'Z') == &upper);
        CHECK_EQ(calls, 3);
        text::Face const& lower = parts.face_for(U'a');
        CHECK(&lower != &builtin && &lower != &upper);
        CHECK_EQ(calls, 4);
        CHECK_EQ(last_asked, std::string("https://fonts.test/lower.ttf"));
        CHECK_EQ(manager.page_fonts_waiting(), 0u);

        // A face ahead that draws the code point keeps the waiting one
        // unasked: it is fetched for the first code point that face lacks.
        text::FontDescription few;
        few.family = "Few";
        few.units_per_em = 2048;
        few.ascender = 1600;
        few.descender = -400;
        for (int i = 0; i < 2; ++i) {
            text::WriterGlyph glyph;
            glyph.advance = 1000;
            glyph.outline.points = { { 0, 0, true }, { 500, 1000, true }, { 1000, 0, true } };
            glyph.outline.contour_ends = { 2 };
            few.glyphs.push_back(glyph);
        }
        few.mappings = { { U'A', 1 } };
        declared = { text::PageFont { "Few", 400, false, text::write_truetype(few), 100, {} },
            waiting("Rest", 400, "https://fonts.test/rest.ttf") };
        manager.set_page_fonts(declared);
        FontStack const& both = manager.resolve(FontRequest { { "Few", "Rest" }, 400, false });
        if (CHECK_EQ(both.faces().size(), 2u)) {
            text::Face const* const first = both.faces()[0];
            CHECK(&both.face_for(U'A') == first);
            CHECK(&both.primary() == first);
            CHECK_EQ(calls, 4);
            text::Face const& rest = both.face_for(U'z');
            CHECK(&rest != first && &rest != &builtin);
            CHECK_EQ(calls, 5);
            CHECK(&both.face_for(U'A') == first);
        }

        // A font that has to cross the network is on its way: the text takes
        // the next face it has, and the page is asked the once — whichever
        // stack's text came first, and however much text follows.
        answers["https://fonts.test/slow.ttf"].outcome = text::PageFontLoad::Outcome::Coming;
        declared = { waiting("Slow", 400, "https://fonts.test/slow.ttf") };
        manager.set_page_fonts(declared);
        FontStack const& slow = manager.resolve(FontRequest { { "Slow" }, 400, false });
        FontStack const& slow_bold = manager.resolve(FontRequest { { "Slow" }, 700, false }); // before either has asked
        CHECK(&slow.face_for(U'A') == &builtin);
        CHECK_EQ(calls, 6);
        CHECK(declared[0].waiting->coming);
        CHECK(!declared[0].waiting->came());
        CHECK(&slow.face_for(U'B') == &builtin);
        CHECK(&slow.primary() == &builtin);
        CHECK(&slow_bold.face_for(U'A') == &builtin);
        CHECK_EQ(calls, 6);
        CHECK(&manager.resolve(FontRequest { { "Slow" }, 300, false }).face_for(U'A') == &builtin); // and one resolved after
        CHECK_EQ(calls, 6);
        CHECK_EQ(manager.page_fonts_waiting(), 1u);
        CHECK_EQ(manager.page_font_count(), 0u);

        // A font that cannot be had is as if it had not been declared: the
        // family's other face answers in its place.
        answers["https://fonts.test/gone.ttf"].outcome = text::PageFontLoad::Outcome::Failed;
        declared = { text::PageFont { "Pair", 400, false, ttf, 100, {} }, waiting("Pair", 700, "https://fonts.test/gone.ttf") };
        manager.set_page_fonts(declared);
        FontStack const& bold = manager.resolve(FontRequest { { "Pair" }, 700, false });
        CHECK_EQ(bold.faces().size(), 1u); // the bold face is the family's answer, and it waits
        text::Face const& regular = manager.resolve(FontRequest { { "Pair" }, 400, false }).face_for(U'A');
        CHECK(&regular != &builtin);
        CHECK_EQ(calls, 6);
        CHECK(&bold.face_for(U'A') == &regular);
        CHECK_EQ(calls, 7);
        CHECK_EQ(manager.page_fonts_waiting(), 0u);
        CHECK_EQ(manager.page_font_count(), 1u);
        CHECK(&bold.face_for(U'B') == &regular);
        CHECK_EQ(calls, 7);
        // And so is one whose bytes are not a font.
        answers["https://fonts.test/junk.ttf"].outcome = text::PageFontLoad::Outcome::Loaded;
        answers["https://fonts.test/junk.ttf"].bytes = { 1, 2, 3 };
        declared = { waiting("Junk", 400, "https://fonts.test/junk.ttf") };
        manager.set_page_fonts(declared);
        CHECK(&manager.resolve(FontRequest { { "Junk" }, 400, false }).face_for(U'A') == &builtin);
        CHECK_EQ(calls, 8);
        CHECK_EQ(manager.page_fonts_waiting(), 0u);
        CHECK_EQ(manager.page_font_count(), 0u);

        // A stack set aside — another page's fonts are the thread's now —
        // answers the layout that holds it as it did, and asks for nothing.
        declared = { waiting("Stale", 400, "https://fonts.test/stale.ttf") };
        manager.set_page_fonts(declared);
        FontStack const& stale = manager.resolve(FontRequest { { "Stale" }, 400, false });
        manager.set_page_fonts({});
        CHECK(&stale.face_for(U'A') == &builtin);
        CHECK(&stale.primary() == &builtin);
        CHECK_EQ(calls, 8);

        // The census, since this block began: every time the page was asked,
        // and what came of it.
        text::PageFontCensus const census = text::page_font_census();
        CHECK_EQ(census.asked - census_before.asked, std::size_t { 8 });
        CHECK_EQ(census.loaded - census_before.loaded, std::size_t { 5 });
        CHECK_EQ(census.coming - census_before.coming, std::size_t { 1 });
        CHECK_EQ(census.failed - census_before.failed, std::size_t { 2 });

        // A page whose fetcher is gone empties the loader: what still waits
        // stays waiting, and the text has its fallback.
        declared = { waiting("Never", 400, "https://fonts.test/never.ttf") };
        manager.set_page_fonts(declared);
        *loader = nullptr;
        FontStack const& never = manager.resolve(FontRequest { { "Never" }, 400, false });
        CHECK(&never.face_for(U'A') == &builtin);
        CHECK(&never.primary() == &builtin);
        CHECK_EQ(calls, 8);
        CHECK_EQ(manager.page_fonts_waiting(), 1u);
        manager.set_page_fonts({});
    }

    // --- System fonts --------------------------------------------------------------------
    // A colour font handed over by name joins the catalogue, and an emoji
    // falls back to it before any face with a black-and-white glyph — on a
    // machine with a colour emoji font of its own, to that one or this.
    std::filesystem::path const picture_font = std::filesystem::temp_directory_path() / "sashfold-emoji-test.ttf";
    {
        text::FontDescription description;
        description.family = "Sashfold Test Emoji";
        description.units_per_em = 1000;
        description.ascender = 800;
        description.descender = -200;
        for (int i = 0; i < 2; ++i) {
            text::WriterGlyph glyph;
            glyph.advance = 1000;
            description.glyphs.push_back(glyph);
        }
        description.mappings = { { 0x1F600, 1 } };
        description.bitmap_glyphs = { { 1, core_png_4x4_red(), 0, 0, 4, 4 } };
        description.bitmap_ppem = 16;
        std::vector<std::uint8_t> const bytes = text::write_truetype(description);
        std::ofstream out(picture_font, std::ios::binary);
        out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    manager.add_font_file(picture_font.string());
    manager.set_system_fonts(true);
    CHECK(manager.system_fonts());
    std::vector<text::FaceInfo> const& catalogue = manager.catalogue();
    std::cout << "  catalogued " << catalogue.size() << " faces\n";
    for (text::FaceInfo const& info : catalogue) {
        CHECK(!info.family.empty());
        CHECK(info.has_outlines);
    }
    {
        FontStack const& text_face = manager.resolve(FontRequest { { "sans-serif" }, 400, false });
        text::Face const& smile = text_face.face_for(0x1F600);
        CHECK(&smile != &builtin);
        CHECK(smile.glyph_index(0x1F600) != 0);
        std::size_t colour_faces = 0;
        for (text::FaceInfo const& info : catalogue)
            colour_faces += info.color ? 1 : 0;
        CHECK(colour_faces >= 1);
        std::cout << "  U+1F600 falls back to " << smile.family() << " (" << colour_faces << " colour faces catalogued)\n";
    }
    std::filesystem::remove(picture_font);

    FontStack const& serif = manager.resolve(FontRequest { { "serif" }, 400, false });
    FontStack const& sans = manager.resolve(FontRequest { { "sans-serif" }, 400, false });
    FontStack const& mono = manager.resolve(FontRequest { { "monospace" }, 400, false });
    FontStack const& none = manager.resolve(FontRequest { { "No Such Family 1234" }, 400, false });
    CHECK(serif.faces().back() == &builtin);
    CHECK(&serif.face_for(0x10FFFD) == &builtin); // a private-use code point: nothing has it
    if (catalogue.empty()) {
        CHECK(&serif.primary() == &builtin);
        std::cout << "  no system fonts here: the built-in face answers everything\n";
        return test::report("font-manager");
    }
    std::cout << "  serif -> " << serif.primary().family() << ", sans-serif -> "
              << sans.primary().family() << ", monospace -> " << mono.primary().family()
              << ", unknown -> " << none.primary().family() << "\n";
    // An unknown family falls to the default, which is serif.
    CHECK(&none.primary() == &serif.primary());
    // A monospace generic yields a fixed-pitch face when the machine has one.
    if (&mono.primary() != &builtin)
        CHECK(mono.primary().is_monospace());

    // Weight and slant choose within a family; a bold request never returns
    // a lighter face when a heavier one exists.
    FontStack const& bold = manager.resolve(FontRequest { { "sans-serif" }, 700, false });
    FontStack const& italic = manager.resolve(FontRequest { { "sans-serif" }, 400, true });
    if (&sans.primary() != &builtin) {
        CHECK_EQ(bold.primary().family(), sans.primary().family());
        CHECK(!sans.primary().is_bold());
        CHECK(!sans.primary().is_italic());
        std::cout << "  bold -> " << bold.primary().family() << (bold.primary().is_bold() ? " (bold face)" : " (synthesized)")
                  << ", italic -> " << (italic.primary().is_italic() ? "italic face" : "synthesized") << "\n";
    }

    // Fonts known to ship with the OS resolve by name.
#ifdef _WIN32
    FontStack const& arial = manager.resolve(FontRequest { { "Arial" }, 400, false });
    CHECK_EQ(arial.primary().family(), std::string("Arial"));
    CHECK(manager.resolve(FontRequest { { "Arial" }, 700, false }).primary().is_bold());
    CHECK(manager.resolve(FontRequest { { "arial" }, 400, true }).primary().is_italic()); // case-insensitive
    CHECK(manager.resolve(FontRequest { { "Consolas" }, 400, false }).primary().is_monospace());
    // Arial lacks CJK; the fallback chain finds a face that has it.
    text::Face const& cjk = arial.face_for(0x4E00);
    CHECK(&cjk != &builtin);
    CHECK(cjk.glyph_index(0x4E00) != 0);
    std::cout << "  U+4E00 falls back to " << cjk.family() << "\n";
#endif
    // Fallback answers are remembered and stable.
    CHECK(&serif.face_for(0x4E00) == &serif.face_for(0x4E00));
    CHECK(manager.fallback_for(0x10FFFD) == nullptr);

    return test::report("font-manager");
}
