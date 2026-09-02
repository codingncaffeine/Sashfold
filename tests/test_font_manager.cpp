#include "Test.h"

#include "text/Face.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"

#include <cstdint>
#include <iostream>
#include <string>
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
    manager.set_page_fonts({ text::PageFont { "Ahem", 400, false, ttf } });
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
    manager.set_page_fonts({ text::PageFont { "Ahem", 400, false, ttf } });
    CHECK(&manager.resolve(FontRequest { { "ahem", "serif" }, 400, false }) == &page);
    // Weight and slant pick among a family's faces; a face of the other
    // slant is the last resort.
    text::TrueTypeOptions bold_options;
    bold_options.bold = true;
    std::vector<std::uint8_t> const bold_ttf = text::SashfoldMono::instance().to_truetype(bold_options);
    manager.set_page_fonts({ text::PageFont { "Ahem", 400, false, ttf },
        text::PageFont { "Ahem", 700, false, bold_ttf }, text::PageFont { "Solo", 400, true, ttf } });
    CHECK_EQ(manager.page_font_count(), 3u);
    text::Face const& page_regular = manager.resolve(FontRequest { { "Ahem" }, 400, false }).primary();
    text::Face const& page_bold = manager.resolve(FontRequest { { "Ahem" }, 700, false }).primary();
    CHECK(&page_regular != &page_bold);
    CHECK(&manager.resolve(FontRequest { { "Ahem" }, 500, false }).primary() == &page_regular);
    CHECK(&manager.resolve(FontRequest { { "Ahem" }, 600, false }).primary() == &page_bold);
    CHECK(&manager.resolve(FontRequest { { "Ahem" }, 400, true }).primary() == &page_regular);
    CHECK(&manager.resolve(FontRequest { { "Solo" }, 400, false }).primary() != &builtin);
    // Bytes that are not a font register nothing, and the family falls through.
    manager.set_page_fonts({ text::PageFont { "Junk", 400, false, { 1, 2, 3 } } });
    CHECK_EQ(manager.page_font_count(), 0u);
    CHECK(&manager.resolve(FontRequest { { "Junk", "Ahem" }, 400, false }).primary() == &builtin);
    manager.set_page_fonts({});
    CHECK(&manager.resolve(FontRequest { { "ahem", "serif" }, 400, false }).primary() == &builtin);

    // --- System fonts --------------------------------------------------------------------
    manager.set_system_fonts(true);
    CHECK(manager.system_fonts());
    std::vector<text::FaceInfo> const& catalogue = manager.catalogue();
    std::cout << "  catalogued " << catalogue.size() << " faces\n";
    for (text::FaceInfo const& info : catalogue) {
        CHECK(!info.family.empty());
        CHECK(info.has_outlines);
    }

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
