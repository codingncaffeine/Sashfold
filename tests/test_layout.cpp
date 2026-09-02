#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "paint/Painter.h"
#include "text/Face.h"
#include "text/FontManager.h"

#include <algorithm>
#include <functional>
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

    // --- Images: on the baseline inline, as blocks, sized by CSS, attributes, or themselves ---
    {
        constexpr std::string_view image_html = R"HTML(<!doctype html>
<html><head><style>
  body { font-family: "Sashfold Mono"; font-size: 16px; margin: 0; width: 200px }
  .block { display: block }
  .half { width: 50% }
</style></head><body>
<p id="p1">ab<img id="i1" src="a.png">cd</p>
<p id="p2"><img id="i2" src="a.png" width="40" height="10"></p>
<p id="p3"><img id="i3" class="block" src="a.png"></p>
<p id="p4"><img id="i4" class="half" src="a.png"></p>
<p id="p5"><img id="i5" src="missing.png" alt="gone"></p>
<p id="p6"><img id="i6" src="a.png" width="400"></p>
<p id="p7"><img id="i7" src="missing.png" width="30" height="20" alt="late"></p>
</body></html>)HTML";
        manager.set_system_fonts(false);
        Page page;
        page.document = html::parse_document(image_html);
        page.styles = css::resolve_styles(*page.document);
        auto const picture = std::make_shared<Bitmap const>(Bitmap(20, 30, Color::rgb(255, 0, 0)));
        layout::ImageMap images;
        std::vector<dom::Element const*> imgs;
        std::function<void(dom::Node const&)> const gather = [&](dom::Node const& node) {
            if (node.is_element()) {
                auto const& element = static_cast<dom::Element const&>(node);
                if (element.is_html("img")) {
                    imgs.push_back(&element);
                    if (dom::Attr const* src = element.find_attribute("src"); src && src->value == "a.png")
                        images.emplace(&element, layout::PageImage { picture, 1.0f });
                }
            }
            for (dom::Node const* child : node.children())
                gather(*child);
        };
        gather(*page.document);
        CHECK_EQ(imgs.size(), 7u);
        page.result = layout::layout_document(*page.document, page.styles, 200, &images);

        std::vector<layout::Fragment const*> boxes;
        std::function<void(layout::Fragment const&)> const collect_boxes = [&](layout::Fragment const& f) {
            if (f.image)
                boxes.push_back(&f);
            for (layout::Fragment const& child : f.children)
                collect_boxes(child);
        };
        collect_boxes(page.result.root);
        auto const box_of = [&](std::string_view id) -> layout::Fragment const* {
            for (layout::Fragment const* box : boxes) {
                dom::Attr const* attribute = box->element ? box->element->find_attribute("id") : nullptr;
                if (attribute && attribute->value == id)
                    return box;
            }
            return nullptr;
        };
        CHECK_EQ(boxes.size(), 6u); // every img but the one with neither picture nor size
        if (layout::Fragment const* i1 = box_of("i1"); CHECK(i1 != nullptr)) {
            CHECK_EQ(i1->width, 20.0f);
            CHECK_EQ(i1->height, 30.0f);
            CHECK_EQ(i1->x, 20.0f); // after "ab"
            CHECK(i1->image->bitmap == picture);
            // The picture sits on the baseline, which it lifted to its height.
            std::vector<layout::TextRun const*> runs;
            collect(page.result.root, runs);
            layout::TextRun const* cd = nullptr;
            for (layout::TextRun const* run : runs) {
                if (run->text == U"cd")
                    cd = run;
            }
            if (CHECK(cd != nullptr)) {
                CHECK_EQ(cd->x, 40.0f);
                CHECK_EQ(cd->baseline_y, i1->y + i1->height);
            }
        }
        if (layout::Fragment const* i2 = box_of("i2"); CHECK(i2 != nullptr)) {
            CHECK_EQ(i2->width, 40.0f);
            CHECK_EQ(i2->height, 10.0f);
        }
        if (layout::Fragment const* i3 = box_of("i3"); CHECK(i3 != nullptr)) {
            CHECK_EQ(i3->x, 0.0f);
            CHECK_EQ(i3->width, 20.0f);
            CHECK_EQ(i3->height, 30.0f);
        }
        if (layout::Fragment const* i4 = box_of("i4"); CHECK(i4 != nullptr)) {
            CHECK_EQ(i4->width, 100.0f); // 50% of 200
            CHECK_EQ(i4->height, 150.0f); // the ratio kept
        }
        CHECK(box_of("i5") == nullptr);
        if (layout::Fragment const* i6 = box_of("i6"); CHECK(i6 != nullptr)) {
            CHECK_EQ(i6->width, 200.0f); // 400 asked, shrunk to the container
            CHECK_EQ(i6->height, 300.0f);
        }
        if (layout::Fragment const* i7 = box_of("i7"); CHECK(i7 != nullptr)) {
            CHECK_EQ(i7->width, 30.0f); // reserved for a picture that has not arrived
            CHECK(i7->image->bitmap == nullptr);
        }
        // Alt text stands in only when nothing sizes the box.
        {
            std::vector<layout::TextRun const*> runs;
            collect(page.result.root, runs);
            bool gone = false;
            bool late = false;
            for (layout::TextRun const* run : runs) {
                gone = gone || run->text == U"[gone]";
                late = late || run->text == U"[late]";
            }
            CHECK(gone);
            CHECK(!late);
        }
        // Painting: the pictures land red where their boxes are, scaled.
        Bitmap canvas(200, static_cast<int>(page.result.page_height + 0.5f), Color::rgb(255, 255, 255));
        paint::paint_page(canvas, page.result);
        if (layout::Fragment const* i4 = box_of("i4")) {
            CHECK(canvas.pixel(static_cast<int>(i4->x) + 50, static_cast<int>(i4->y) + 75) == Color::rgb(255, 0, 0));
            CHECK(canvas.pixel(static_cast<int>(i4->x) + 99, static_cast<int>(i4->y) + 149) == Color::rgb(255, 0, 0));
            CHECK(canvas.pixel(static_cast<int>(i4->x) + 100, static_cast<int>(i4->y) + 75) == Color::rgb(255, 255, 255));
        }
        if (layout::Fragment const* i1 = box_of("i1"))
            CHECK(canvas.pixel(static_cast<int>(i1->x) + 10, static_cast<int>(i1->y) + 15) == Color::rgb(255, 0, 0));
        if (layout::Fragment const* i7 = box_of("i7"))
            CHECK(canvas.pixel(static_cast<int>(i7->x) + 15, static_cast<int>(i7->y) + 10) == Color::rgb(255, 255, 255));
    }

    // --- Scaling: shrinking averages, growing repeats ------------------------------------
    {
        Bitmap source(4, 2, Color::rgb(0, 0, 0));
        source.set_pixel(0, 0, Color::rgb(255, 255, 255));
        source.set_pixel(1, 0, Color::rgb(255, 255, 255));
        source.set_pixel(0, 1, Color::rgb(255, 255, 255));
        source.set_pixel(1, 1, Color::rgb(255, 255, 255));
        Bitmap small(2, 1, Color::rgb(0, 128, 0));
        small.draw_scaled(source, Rect { 0, 0, 2, 1 });
        CHECK(small.pixel(0, 0) == Color::rgb(255, 255, 255));
        CHECK(small.pixel(1, 0) == Color::rgb(0, 0, 0));
        Bitmap mixed(1, 1, Color::rgb(0, 128, 0));
        mixed.draw_scaled(source, Rect { 0, 0, 1, 1 });
        CHECK(mixed.pixel(0, 0) == Color::rgb(127, 127, 127)); // half white, half black
        Bitmap large(8, 4, Color::rgb(0, 128, 0));
        large.draw_scaled(source, Rect { 0, 0, 8, 4 });
        CHECK(large.pixel(3, 3) == Color::rgb(255, 255, 255));
        CHECK(large.pixel(4, 0) == Color::rgb(0, 0, 0));
        // Transparent source pixels leave the ground alone.
        Bitmap clear(2, 2, Color::rgba(0, 0, 0, 0));
        Bitmap ground(2, 2, Color::rgb(9, 9, 9));
        ground.draw_scaled(clear, Rect { 0, 0, 2, 2 });
        CHECK(ground.pixel(1, 1) == Color::rgb(9, 9, 9));
        Bitmap clipped(3, 3, Color::rgb(9, 9, 9));
        clipped.draw_scaled(source, Rect { -2, -1, 4, 2 }); // partly off the canvas
        CHECK(clipped.pixel(0, 0) == Color::rgb(0, 0, 0));
        CHECK(clipped.pixel(2, 2) == Color::rgb(9, 9, 9));
    }

    return test::report("layout");
}
