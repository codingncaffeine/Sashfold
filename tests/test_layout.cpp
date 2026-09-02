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

    // --- A block inside inline content takes lines of its own ------------------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p { margin: 0 }
</style></head><body><p>aa <span>bb<div>cc</div>dd</span> ee</p></body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const run_of = [&](std::u32string_view text) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return candidate;
            }
            return nullptr;
        };
        layout::TextRun const* aa = run_of(U"aa");
        layout::TextRun const* bb = run_of(U"bb");
        layout::TextRun const* cc = run_of(U"cc");
        layout::TextRun const* dd = run_of(U"dd");
        layout::TextRun const* ee = run_of(U"ee");
        if (CHECK(aa && bb && cc && dd && ee)) {
            CHECK_EQ(aa->baseline_y, bb->baseline_y);
            CHECK_EQ(cc->baseline_y - aa->baseline_y, 20.0f); // the block starts a line of its own
            CHECK_EQ(dd->baseline_y - cc->baseline_y, 20.0f); // and what follows starts another
            CHECK_EQ(dd->baseline_y, ee->baseline_y);
            CHECK_EQ(cc->x, 0.0f);
        }
    }

    // --- A block-level picture inside inline content is a line of its own -------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page page;
        page.document = html::parse_document(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p { margin: 0 }
  img { display: block; width: 40px; height: 30px }
</style></head><body><p>aa <a href="#"><img src="a.png"></a> bb</p></body></html>)HTML");
        page.styles = css::resolve_styles(*page.document);
        auto const picture = std::make_shared<Bitmap const>(Bitmap(20, 30, Color::rgb(255, 0, 0)));
        layout::ImageMap images;
        std::function<void(dom::Node const&)> const gather = [&](dom::Node const& node) {
            if (node.is_element() && static_cast<dom::Element const&>(node).is_html("img"))
                images.emplace(static_cast<dom::Element const*>(&node), layout::PageImage { picture, 1.0f });
            for (dom::Node const* child : node.children())
                gather(*child);
        };
        gather(*page.document);
        page.result = layout::layout_document(*page.document, page.styles, 400, &images);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        layout::Fragment const* box = nullptr;
        std::function<void(layout::Fragment const&)> const find_image = [&](layout::Fragment const& f) {
            if (f.image)
                box = &f;
            for (layout::Fragment const& child : f.children)
                find_image(child);
        };
        find_image(page.result.root);
        layout::TextRun const* aa = nullptr;
        layout::TextRun const* bb = nullptr;
        for (layout::TextRun const* const run : runs) {
            if (run->text == U"aa")
                aa = run;
            if (run->text == U"bb")
                bb = run;
        }
        if (CHECK(box && box->image) && CHECK(aa && bb)) {
            CHECK_EQ(box->image->width, 40.0f); // the picture is there, at its written size
            CHECK_EQ(box->image->height, 30.0f);
            CHECK_EQ(box->image->x, 0.0f); // on a line of its own, at the left
            CHECK_EQ(box->image->y, 20.0f); // the line after aa's
            CHECK(bb->baseline_y >= 50.0f); // and bb on a line after the picture's
            CHECK(bb->baseline_y < 100.0f);
        }
    }

    // --- Generated content: ::before and ::after boxes ---------------------------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p, div { margin: 0 }
  #inline::before { content: "B " }
  #inline::after { content: " A" }
  #blk::after { content: "Z"; display: block; margin-top: 10px }
  #fix::after { content: ""; display: block; clear: both }
  .fl { float: left; width: 30px; height: 50px }
</style></head><body><p id="inline">aa</p><div id="blk">bb</div><div id="fix"><div class="fl"></div>cc</div><p id="tail">dd</p></body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const run_of = [&](std::u32string_view text) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return candidate;
            }
            return nullptr;
        };
        layout::TextRun const* b = run_of(U"B");
        layout::TextRun const* aa = run_of(U"aa");
        layout::TextRun const* a = run_of(U"A");
        layout::TextRun const* bb = run_of(U"bb");
        layout::TextRun const* z = run_of(U"Z");
        layout::TextRun const* cc = run_of(U"cc");
        layout::TextRun const* dd = run_of(U"dd");
        if (CHECK(b && aa && a && bb && z && cc && dd)) {
            CHECK_EQ(b->baseline_y, aa->baseline_y); // inline boxes share the line
            CHECK_EQ(aa->baseline_y, a->baseline_y);
            CHECK_EQ(b->x, 0.0f);
            CHECK(b->x < aa->x && aa->x < a->x);
            CHECK_EQ(z->baseline_y - bb->baseline_y, 30.0f); // a line of its own, after its 10 px margin
            CHECK_EQ(z->x, 0.0f);
            // The clearfix: the empty cleared box stands below the float, so
            // the container reaches around it and what follows starts there.
            CHECK(dd->baseline_y - cc->baseline_y >= 50.0f);
        }
    }

    // --- Margins collapsing through parents and empty boxes (CSS 2.1 §8.3.1) ------
    {
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 8px; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p { margin: 16px 0 }
  .bordered { border-top: 1px solid black }
  .padded { padding-bottom: 5px }
  .empty { margin: 30px 0 }
  .neg { margin-top: -10px }
</style></head><body>
<div id="wrap"><p id="first">first</p><p id="second">second</p></div>
<div id="bordered" class="bordered"><p id="inbordered">bordered</p></div>
<div id="empty" class="empty"></div>
<p id="after">after</p>
<div id="padded" class="padded"><p id="inpadded">padded</p></div>
<p id="last" class="neg">last</p>
</body></html>)HTML", 400);
        std::function<layout::Fragment const*(layout::Fragment const&, std::string_view)> find_box
            = [&](layout::Fragment const& fragment, std::string_view id) -> layout::Fragment const* {
            if (fragment.element) {
                if (dom::Attr const* attribute = fragment.element->find_attribute("id");
                    attribute && attribute->value == id)
                    return &fragment;
            }
            for (layout::Fragment const& child : fragment.children) {
                if (layout::Fragment const* found = find_box(child, id))
                    return found;
            }
            return nullptr;
        };
        auto const top_of = [&](std::string_view id) {
            layout::Fragment const* box = find_box(page.result.root, id);
            return box ? box->y : -1.0f;
        };
        auto const height_of = [&](std::string_view id) {
            layout::Fragment const* box = find_box(page.result.root, id);
            return box ? box->height : -1.0f;
        };
        // body's 8 and the first paragraph's 16 meet through the wrapper: 16.
        CHECK_EQ(top_of("wrap"), 16.0f);
        CHECK_EQ(top_of("first"), 16.0f);
        CHECK_EQ(top_of("second"), 52.0f);
        // The last paragraph's bottom margin reaches out of the wrapper.
        CHECK_EQ(height_of("wrap"), 56.0f);
        CHECK_EQ(top_of("bordered"), 88.0f);
        // A top border stops the collapse: the margin stays inside.
        CHECK_EQ(top_of("inbordered"), 105.0f);
        CHECK_EQ(height_of("bordered"), 37.0f);
        // An empty box's margins meet with its neighbours': one margin of 30.
        CHECK_EQ(height_of("empty"), 0.0f);
        CHECK_EQ(top_of("after"), 155.0f);
        CHECK_EQ(top_of("padded"), 191.0f);
        CHECK_EQ(top_of("inpadded"), 191.0f);
        // Bottom padding keeps the last child's margin inside.
        CHECK_EQ(height_of("padded"), 41.0f);
        // A negative margin adds to a positive one.
        CHECK_EQ(top_of("last"), 222.0f);
        // The page ends with the collapsed body/paragraph bottom margin.
        CHECK_EQ(page.result.page_height, 258.0f);
    }

    // --- min-width, max-width, min-height, max-height -------------------------------
    {
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  div { margin: 0 }
  #capped { max-width: 100px }
  #floored { width: 50px; min-width: 200px }
  #pct { max-width: 50% }
  #tall { height: 10px; min-height: 30px }
  #short { height: 100px; max-height: 40px }
  #auto { max-height: 5px }
  #fl { float: left; max-width: 60px }
  #row { display: flex; width: 400px }
  #item { flex: 1; max-width: 120px }
  #none { max-width: none; min-width: auto }
</style></head><body>
<div id="capped">x</div>
<div id="floored">x</div>
<div id="pct">x</div>
<div id="tall">x</div>
<div id="short">x</div>
<div id="auto">x</div>
<div id="fl">a b c d e f g h i j k l</div>
<div id="row"><div id="item">x</div></div>
<div id="none">x</div>
</body></html>)HTML", 400);
        std::function<layout::Fragment const*(layout::Fragment const&, std::string_view)> find_box
            = [&](layout::Fragment const& fragment, std::string_view id) -> layout::Fragment const* {
            if (fragment.element) {
                if (dom::Attr const* attribute = fragment.element->find_attribute("id");
                    attribute && attribute->value == id)
                    return &fragment;
            }
            for (layout::Fragment const& child : fragment.children) {
                if (layout::Fragment const* found = find_box(child, id))
                    return found;
            }
            return nullptr;
        };
        auto const width_of = [&](std::string_view id) {
            layout::Fragment const* box = find_box(page.result.root, id);
            return box ? box->width : -1.0f;
        };
        auto const height_of = [&](std::string_view id) {
            layout::Fragment const* box = find_box(page.result.root, id);
            return box ? box->height : -1.0f;
        };
        CHECK_EQ(width_of("capped"), 100.0f); // an auto width, held by its maximum
        CHECK_EQ(width_of("floored"), 200.0f); // a written width, raised to its minimum
        CHECK_EQ(width_of("pct"), 200.0f); // a percentage against the container
        CHECK_EQ(height_of("tall"), 30.0f);
        CHECK_EQ(height_of("short"), 40.0f);
        CHECK_EQ(height_of("auto"), 5.0f); // an auto height, held by its maximum
        CHECK_EQ(width_of("fl"), 60.0f); // a float's shrink-to-fit, held by its maximum
        CHECK_EQ(width_of("item"), 120.0f); // a growing flex item, held by its maximum
        CHECK_EQ(width_of("none"), 400.0f);
    }

    return test::report("layout");
}
