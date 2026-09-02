#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Layout through a TrueType face equals layout through the built-in face
// when that face is Sashfold Mono itself, read from the fixture: every run
// lands at the same place with the same width, the same words break, and
// the page is as tall — the proportional path measuring glyph by glyph
// agrees with the fixed-pitch shortcut. A paragraph asking for sans-serif
// gets a proportional face when the machine has one.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document;
    css::StyleMap styles;
    layout::LayoutResult result;
};

Page lay_out(std::string_view html, float width)
{
    Page page;
    page.document = html::parse_document(html);
    page.styles = css::resolve_styles(*page.document);
    page.result = layout::layout_document(*page.document, page.styles, width);
    return page;
}

void collect(layout::Fragment const& fragment, std::vector<layout::TextRun const*>& runs)
{
    for (layout::TextRun const& run : fragment.runs)
        runs.push_back(&run);
    for (layout::Fragment const& child : fragment.children)
        collect(child, runs);
}

constexpr std::string_view page_html = R"(<!doctype html>
<html><head><style>
  body { font-family: "Sashfold Mono"; font-size: 16px; margin: 8px }
  h1 { font-size: 32px }
  .narrow { width: 100px }
  .sans { font-family: sans-serif }
</style></head><body>
<h1>Heading text here</h1>
<p>The quick brown fox jumps over the lazy dog, again and again and again, until the line wraps twice or more.</p>
<p class="narrow">Supercalifragilisticexpialidocious</p>
<pre>code  block   kept</pre>
<ul><li>one item</li><li>two items</li></ul>
<p class="sans">Sans paragraph</p>
</body></html>)";

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: test_layout <SashfoldMono.ttf>\n";
        return 2;
    }
    text::FontManager& manager = text::FontManager::instance();
    text::Face const& builtin = text::builtin_face();

    manager.set_system_fonts(false);
    Page const by_builtin = lay_out(page_html, 400);
    std::vector<layout::TextRun const*> builtin_runs;
    collect(by_builtin.result.root, builtin_runs);
    CHECK(builtin_runs.size() > 20);
    for (layout::TextRun const* run : builtin_runs) {
        CHECK(run->fonts != nullptr);
        CHECK(run->fonts && &run->fonts->primary() == &builtin);
        CHECK(run->width > 0);
        CHECK(run->width == run->fonts->measure(run->text, run->style->font_size));
    }

    manager.set_system_fonts(true);
    manager.add_font_file(argv[1]);
    Page const by_truetype = lay_out(page_html, 400);
    std::vector<layout::TextRun const*> truetype_runs;
    collect(by_truetype.result.root, truetype_runs);
    CHECK_EQ(truetype_runs.size(), builtin_runs.size());
    CHECK_EQ(by_truetype.result.page_height, by_builtin.result.page_height);

    int compared = 0;
    int emergency_pieces = 0;
    std::size_t const count = std::min(truetype_runs.size(), builtin_runs.size());
    for (std::size_t i = 0; i < count; ++i) {
        layout::TextRun const& a = *builtin_runs[i];
        layout::TextRun const& b = *truetype_runs[i];
        if (!b.fonts || b.fonts->primary().family() != "Sashfold Mono")
            continue; // the sans paragraph, when the machine has a sans face
        ++compared;
        CHECK(&b.fonts->primary() != &builtin); // the TrueType face, not the shortcut
        CHECK(b.text == a.text);
        CHECK_EQ(b.x, a.x);
        CHECK_EQ(b.baseline_y, a.baseline_y);
        CHECK_EQ(b.width, a.width);
        if (a.text.size() == 10 && a.text[0] == U'S')
            ++emergency_pieces; // "Supercalif": a 100 px box holds ten 10 px glyphs
    }
    CHECK(compared > 20);
    CHECK_EQ(emergency_pieces, 1);

    // The sans paragraph: a proportional face when there is one; the
    // built-in face otherwise, with the same geometry as before.
    layout::TextRun const* sans = nullptr;
    for (layout::TextRun const* run : truetype_runs) {
        if (run->text == U"paragraph")
            sans = run;
    }
    if (CHECK(sans != nullptr) && sans->fonts) {
        text::Face const& face = sans->fonts->primary();
        bool const machine_has_sans
            = &manager.resolve(text::FontRequest { { "sans-serif" }, 400, false }).primary() != &builtin;
        CHECK_EQ(&face != &builtin, machine_has_sans);
        if (machine_has_sans) {
            CHECK(!face.is_monospace());
            std::cout << "  sans-serif -> " << face.family() << ", 'paragraph' is " << sans->width
                      << " px wide against " << 9 * 10 << " in the built-in face\n";
        }
        CHECK(sans->width == sans->fonts->measure(sans->text, sans->style->font_size));
    }

    return test::report("layout");
}
