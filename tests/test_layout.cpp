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
  body { font-family: "Sashfold Mono"; font-size: 16px; margin: 8px; line-height: 20px }
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

    // --- Positioning: relative shifts, absolute boxes in a containing block ------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p, div { margin: 0 }
  #rel { position: relative; left: 10px; top: 5px }
  #cb { position: relative; width: 300px; height: 100px; padding: 10px; border: 2px solid black }
  #abs { position: absolute; top: 20px; left: 30px; width: 50px; height: 40px }
  #br { position: absolute; right: 0; bottom: 0; width: 60px; height: 20px }
  #auto { position: absolute }
  #fix { position: fixed; top: 100px; left: 200px; width: 20px; height: 20px }
</style></head><body><p id="rel">rr</p><div id="cb"><div id="abs"></div>cc<div id="auto">au</div><div id="br"></div></div><p id="after">dd</p><div id="fix"></div></body></html>)HTML", 400);
        std::function<layout::Fragment const*(layout::Fragment const&, std::string_view)> find_box
            = [&](layout::Fragment const& f, std::string_view id) -> layout::Fragment const* {
            if (f.element) {
                dom::Attr const* attribute = f.element->find_attribute("id");
                if (attribute && attribute->value == id && f.style)
                    return &f;
            }
            for (layout::Fragment const& child : f.children) {
                if (layout::Fragment const* found = find_box(child, id))
                    return found;
            }
            return nullptr;
        };
        layout::Fragment const* rel = find_box(page.result.root, "rel");
        layout::Fragment const* cb = find_box(page.result.root, "cb");
        layout::Fragment const* abs = find_box(page.result.root, "abs");
        layout::Fragment const* br = find_box(page.result.root, "br");
        layout::Fragment const* automatic = find_box(page.result.root, "auto");
        layout::Fragment const* after = find_box(page.result.root, "after");
        layout::Fragment const* fix = find_box(page.result.root, "fix");
        if (CHECK(rel && cb && abs && br && automatic && after && fix)) {
            CHECK_EQ(rel->x, 10.0f); // shifted right and down, still taking its room in flow
            CHECK_EQ(rel->y, 5.0f);
            CHECK(rel->positioned);
            CHECK(!rel->runs.empty() && rel->runs[0].x == 10.0f);
            CHECK_EQ(cb->y, 20.0f); // the shifted box left its 20 px in the flow
            CHECK_EQ(cb->width, 324.0f);
            CHECK_EQ(cb->height, 124.0f);
            CHECK_EQ(abs->x, 32.0f); // the padding box's left plus left: 30px
            CHECK_EQ(abs->y, 42.0f);
            CHECK_EQ(abs->width, 50.0f);
            CHECK_EQ(abs->height, 40.0f);
            CHECK(abs->positioned);
            CHECK_EQ(br->x, 262.0f); // anchored at the padding box's right and bottom
            CHECK_EQ(br->y, 122.0f);
            CHECK_EQ(automatic->x, 12.0f); // its static position: below the line of "cc"
            CHECK_EQ(automatic->y, 52.0f);
            CHECK_EQ(automatic->width, 20.0f); // shrink-to-fit around "au"
            CHECK_EQ(after->y, 144.0f); // the absolute boxes took no room
            CHECK_EQ(fix->x, 200.0f); // fixed: against the initial containing block
            CHECK_EQ(fix->y, 100.0f);
        }
    }

    // --- Translate transforms: moved after layout, the flow untouched ------------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  div { margin: 0; width: 100px; height: 20px }
  p { margin: 0 }
  #t1 { transform: translate(10px, 5px) }
  #t2 { transform: translateX(-100%) }
  #t3 { transform: translate3d(50%, 25%, 0) rotate(45deg) }
  #t4 { translate: 3px 4px }
  #t5 { transform: none }
</style></head><body><div id="t1"></div><div id="t2"></div><div id="t3"></div><div id="t4"></div><div id="t5"></div><p id="after">x</p></body></html>)HTML", 400);
        std::function<layout::Fragment const*(layout::Fragment const&, std::string_view)> find_box
            = [&](layout::Fragment const& f, std::string_view id) -> layout::Fragment const* {
            if (f.element) {
                dom::Attr const* attribute = f.element->find_attribute("id");
                if (attribute && attribute->value == id && f.style)
                    return &f;
            }
            for (layout::Fragment const& child : f.children) {
                if (layout::Fragment const* found = find_box(child, id))
                    return found;
            }
            return nullptr;
        };
        layout::Fragment const* t1 = find_box(page.result.root, "t1");
        layout::Fragment const* t2 = find_box(page.result.root, "t2");
        layout::Fragment const* t3 = find_box(page.result.root, "t3");
        layout::Fragment const* t4 = find_box(page.result.root, "t4");
        layout::Fragment const* t5 = find_box(page.result.root, "t5");
        layout::Fragment const* after = find_box(page.result.root, "after");
        if (CHECK(t1 && t2 && t3 && t4 && t5 && after)) {
            CHECK_EQ(t1->x, 10.0f); // moved by its translation
            CHECK_EQ(t1->y, 5.0f);
            CHECK(t1->stacking_context);
            CHECK_EQ(t2->x, -100.0f); // a full width to the left: off the page
            CHECK_EQ(t2->y, 20.0f);
            CHECK_EQ(t3->x, 50.0f); // half its width, a quarter of its height; the rotation is passed over
            CHECK_EQ(t3->y, 45.0f);
            CHECK_EQ(t4->x, 3.0f); // the translate property
            CHECK_EQ(t4->y, 64.0f);
            CHECK_EQ(t5->x, 0.0f);
            CHECK(!t5->stacking_context);
            CHECK_EQ(after->y, 100.0f); // the flow never moved
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

    // --- inline-block: an atomic box on the line, laid out as a block inside ------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p { margin: 0 }
  .ib { display: inline-block; width: 50px; padding: 5px; border: 1px solid black; margin: 2px 4px }
  .bare { display: inline-block }
  .clip { overflow: hidden; height: 10px }
  #fl { float: left }
  #abs { position: absolute }
</style></head><body>
<p>aa <span id="one" class="ib">bb</span> cc</p>
<p>dd <span id="two" class="bare">ee<br>ff</span> gg</p>
<p>hh <span id="three" class="ib" style="width: auto">ii jj</span> kk</p>
<p><span id="four" class="ib"></span>ll</p>
<div>mm <span id="five" class="bare"><span>nn</span><div id="abs">oo</div></span></div>
<div id="fl"><span id="six" class="ib">pp</span></div>
<p>qq <span id="seven" class="ib clip">rr<br>ss</span> tt</p>
</body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const run_of = [&](std::u32string_view text) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return candidate;
            }
            return nullptr;
        };
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
        layout::TextRun const* aa = run_of(U"aa");
        layout::TextRun const* bb = run_of(U"bb");
        layout::TextRun const* cc = run_of(U"cc");
        layout::Fragment const* one = find_box(page.result.root, "one");
        if (CHECK(aa && bb && cc && one)) {
            float const g = aa->width / 2; // one glyph of the fixed-pitch face
            CHECK_EQ(one->x, 3 * g + 4); // after "aa" and a space, past its left margin
            CHECK_EQ(one->y, 2.0f); // its top margin
            CHECK_EQ(one->width, 62.0f); // 50 + padding + border
            CHECK_EQ(one->height, 32.0f); // one 20 px line + padding + border
            CHECK_EQ(bb->x, 3 * g + 10); // inside the border and padding
            CHECK_EQ(bb->baseline_y, aa->baseline_y); // its line's baseline is the line's
            CHECK_EQ(cc->x, 4 * g + 70); // past the margin box and a space
        }
        layout::TextRun const* dd = run_of(U"dd");
        layout::TextRun const* ee = run_of(U"ee");
        layout::TextRun const* ff = run_of(U"ff");
        layout::TextRun const* gg = run_of(U"gg");
        layout::Fragment const* two = find_box(page.result.root, "two");
        if (CHECK(aa && dd && ee && ff && gg && two)) {
            float const g = aa->width / 2;
            CHECK_EQ(two->y, 36.0f); // the first paragraph was 36 px tall: the box's margin box
            CHECK_EQ(two->width, 2 * g); // shrink-to-fit: the wider of its two lines
            CHECK_EQ(two->height, 40.0f);
            CHECK_EQ(ff->baseline_y, dd->baseline_y); // the last line's baseline is the line's
            CHECK_EQ(gg->baseline_y, dd->baseline_y);
            CHECK_EQ(dd->baseline_y - ee->baseline_y, 20.0f);
            CHECK_EQ(dd->baseline_y - aa->baseline_y, 48.0f); // 36 down, and the baseline 12 lower in a 40 px line
        }
        layout::TextRun const* hh = run_of(U"hh");
        layout::TextRun const* ii = run_of(U"ii");
        layout::TextRun const* jj = run_of(U"jj");
        layout::TextRun const* kk = run_of(U"kk");
        layout::Fragment const* three = find_box(page.result.root, "three");
        if (CHECK(aa && hh && ii && jj && kk && three)) {
            float const g = aa->width / 2;
            CHECK_EQ(three->width, 5 * g + 12); // an auto width shrinks to its content
            CHECK_EQ(ii->baseline_y, hh->baseline_y);
            CHECK_EQ(jj->baseline_y, hh->baseline_y);
            CHECK_EQ(kk->x, 9 * g + 20);
        }
        layout::TextRun const* ll = run_of(U"ll");
        layout::Fragment const* four = find_box(page.result.root, "four");
        if (CHECK(ll && four)) {
            CHECK_EQ(four->x, 4.0f);
            CHECK_EQ(four->width, 62.0f);
            CHECK_EQ(four->height, 12.0f); // no lines: the edges alone
            CHECK_EQ(four->y + four->height + 2, ll->baseline_y); // no line box: the bottom margin edge sits on the baseline
        }
        layout::TextRun const* nn = run_of(U"nn");
        layout::TextRun const* oo = run_of(U"oo");
        if (CHECK(nn && oo)) {
            // The absolutely positioned block inside stands at its static
            // position: the line below its sibling's, moved with the box.
            CHECK_EQ(oo->x, nn->x);
            CHECK_EQ(oo->baseline_y - nn->baseline_y, 20.0f);
        }
        layout::Fragment const* fl = find_box(page.result.root, "fl");
        layout::Fragment const* six = find_box(page.result.root, "six");
        if (CHECK(fl && six)) {
            CHECK_EQ(fl->width, 70.0f); // the float shrinks to the inline-block's margin box
            CHECK_EQ(six->x, fl->x + 4);
        }
        layout::TextRun const* qq = run_of(U"qq");
        layout::TextRun const* ss = run_of(U"ss");
        layout::TextRun const* tt = run_of(U"tt");
        layout::Fragment const* seven = find_box(page.result.root, "seven");
        if (CHECK(qq && ss && tt && seven)) {
            CHECK_EQ(qq->x, 70.0f); // beside the float
            CHECK_EQ(seven->height, 22.0f); // the written height, edges around
            // Clipped, with its last line below its bottom margin edge: the
            // higher of the two, the margin edge, sits on the baseline.
            CHECK_EQ(seven->y + seven->height + 2, qq->baseline_y);
            CHECK(ss->baseline_y > qq->baseline_y);
            CHECK_EQ(tt->baseline_y, qq->baseline_y);
        }
    }

    // --- vertical-align: where a box sits on its line (CSS 2.1 §10.8) ---------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p, div { margin: 0 }
  .box { display: inline-block; width: 10px; height: 36px }
  .top { vertical-align: top }
  .bottom { vertical-align: bottom }
  .middle { vertical-align: middle }
  .up { vertical-align: 6px }
  .pct { vertical-align: -50% }
  .own { vertical-align: 10px }
</style></head><body>
<p>ff<span id="t" class="box top"></span><span id="b" class="box bottom"></span><span id="m" class="box middle"></span>gg</p>
<div class="own">hh</div>
<p>zz</p>
<p>ii<span class="up">jj <span>kk</span></span></p>
<p>aa<sup>bb</sup><sub>cc</sub><span class="up">dd</span><span class="pct">ee</span></p>
</body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const run_of = [&](std::u32string_view text) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return candidate;
            }
            return nullptr;
        };
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
        layout::TextRun const* aa = run_of(U"aa");
        layout::TextRun const* bb = run_of(U"bb");
        layout::TextRun const* cc = run_of(U"cc");
        layout::TextRun const* dd = run_of(U"dd");
        layout::TextRun const* ee = run_of(U"ee");
        if (CHECK(aa && bb && cc && dd && ee)) {
            CHECK(std::abs(aa->baseline_y - bb->baseline_y - 16.0f / 3.0f) < 0.01f); // super: a third of the parent's em up
            CHECK(std::abs(cc->baseline_y - aa->baseline_y - 16.0f / 5.0f) < 0.01f); // sub: a fifth down
            CHECK_EQ(aa->baseline_y - dd->baseline_y, 6.0f); // a length raises by itself
            CHECK_EQ(ee->baseline_y - aa->baseline_y, 10.0f); // a percentage of the box's own line height, 20 px
            CHECK(bb->style->font_size < aa->style->font_size); // sup and sub are smaller
        }
        layout::TextRun const* ff = run_of(U"ff");
        layout::TextRun const* gg = run_of(U"gg");
        layout::Fragment const* t = find_box(page.result.root, "t");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* m = find_box(page.result.root, "m");
        if (CHECK(ff && gg && t && b && m)) {
            // The first paragraph's line holds three 36 px boxes: the
            // middle one reaches 18 + 4 above the baseline and 18 − 4
            // below, more than the text's own ascent and descent, so the
            // line is 22 + 14 = 36 tall and the top and bottom boxes fit
            // within it.
            CHECK_EQ(t->y, 0.0f); // top-aligned: at the line's top
            CHECK_EQ(b->y + b->height, 36.0f); // bottom-aligned: at the line's bottom
            CHECK_EQ(ff->baseline_y, 22.0f); // the baseline 22 below the line's top
            CHECK_EQ(gg->baseline_y, ff->baseline_y);
            CHECK_EQ(m->y + m->height / 2, ff->baseline_y - 4); // its midpoint 4 px above the baseline
            CHECK_EQ(m->y, 0.0f); // so a 36 px box tops out at the line's top
        }
        layout::TextRun const* hh = run_of(U"hh");
        layout::TextRun const* zz = run_of(U"zz");
        layout::TextRun const* ii = run_of(U"ii");
        layout::TextRun const* jj = run_of(U"jj");
        layout::TextRun const* kk = run_of(U"kk");
        if (CHECK(hh && zz && ii && jj && kk)) {
            // A block's own vertical-align does not move its text: the
            // division's line is a plain 20 px line like the paragraph's
            // after it.
            CHECK_EQ(zz->baseline_y - hh->baseline_y, 20.0f);
            // A run nested in a raised span rides with it.
            CHECK_EQ(ii->baseline_y - jj->baseline_y, 6.0f);
            CHECK_EQ(ii->baseline_y - kk->baseline_y, 6.0f);
        }
    }

    // --- A block inside an inline box splits it: the block is a block child ----
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p { margin: 16px 0 }
  #card { border: 2px solid black; padding: 5px; margin: 4px 0; background: #eee }
</style></head><body>
<div id="host">aa <a href="#"><span>bb</span><div id="card">cc<p>dd</p></div>ee</a> ff</div>
</body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const run_of = [&](std::u32string_view text) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return candidate;
            }
            return nullptr;
        };
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
        layout::TextRun const* aa = run_of(U"aa");
        layout::TextRun const* bb = run_of(U"bb");
        layout::TextRun const* cc = run_of(U"cc");
        layout::TextRun const* dd = run_of(U"dd");
        layout::TextRun const* ee = run_of(U"ee");
        layout::TextRun const* ff = run_of(U"ff");
        layout::Fragment const* card = find_box(page.result.root, "card");
        layout::Fragment const* host = find_box(page.result.root, "host");
        if (CHECK(aa && bb && cc && dd && ee && ff && card && host)) {
            float const g = aa->width / 2;
            CHECK_EQ(bb->baseline_y, aa->baseline_y); // the inline content before the block stays on its line
            CHECK_EQ(bb->x, 3 * g);
            CHECK_EQ(card->x, 0.0f); // the block is a child of the containing block: full width, its own edges
            CHECK_EQ(card->width, 400.0f);
            CHECK_EQ(card->y, 24.0f); // below the 20 px line, past its 4 px margin
            CHECK_EQ(cc->x, 7.0f); // inside border and padding
            CHECK_EQ(cc->baseline_y - aa->baseline_y, 31.0f); // 24 down plus border and padding
            CHECK_EQ(dd->baseline_y - cc->baseline_y, 36.0f); // the paragraph's margin inside the card holds
            CHECK_EQ(card->height, 86.0f); // 7 + 20 + 16 + 20 + 16 + 7
            CHECK_EQ(ee->baseline_y - aa->baseline_y, 114.0f); // the content after the block on a new line
            CHECK_EQ(ee->x, 0.0f);
            CHECK_EQ(ff->baseline_y, ee->baseline_y);
            CHECK_EQ(host->height, 134.0f); // 20 + 4 + 86 + 4 + 20
        }
    }

    // --- Lines and formatting-context boxes keep clear of floats along their height
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  .f1 { float: left; width: 50px; height: 75px }
  .f2 { float: left; clear: left; width: 100px; height: 75px }
  .ib { display: inline-block; vertical-align: top; width: 200px; height: 50px }
  .tall { overflow: hidden; height: 100px }
  .wide { overflow: hidden; height: 100px; width: 350px }
</style></head><body>
<div id="a" style="width: 400px; overflow: hidden"><div class="f1"></div><div class="f2"></div><span id="s1" class="ib"></span><span id="s2" class="ib"></span></div>
<div id="b" style="width: 400px; overflow: hidden"><div class="f1"></div><div class="f2"></div><div id="box" class="tall"></div></div>
<div id="c" style="width: 400px; overflow: hidden"><div class="f1"></div><div class="f2"></div><div id="wide" class="wide"></div></div>
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
        layout::Fragment const* s1 = find_box(page.result.root, "s1");
        layout::Fragment const* s2 = find_box(page.result.root, "s2");
        layout::Fragment const* a = find_box(page.result.root, "a");
        if (CHECK(s1 && s2 && a)) {
            CHECK_EQ(s1->x, 50.0f); // beside the first float
            CHECK_EQ(s1->y, 0.0f);
            // The second line spans 50 to 100: the second float (75 to 150)
            // narrows it, and the box fits beside it.
            CHECK_EQ(s2->x, 100.0f);
            CHECK_EQ(s2->y, 50.0f);
            CHECK_EQ(a->height, 150.0f); // the container reaches around its floats
        }
        layout::Fragment const* box = find_box(page.result.root, "box");
        layout::Fragment const* b = find_box(page.result.root, "b");
        if (CHECK(box && b)) {
            // A box of its own context, 100 tall from the top, meets both
            // floats along its height: laid out again in the narrower room.
            CHECK_EQ(box->y, b->y);
            CHECK_EQ(box->x, 100.0f);
            CHECK_EQ(box->width, 300.0f);
        }
        layout::Fragment const* wide = find_box(page.result.root, "wide");
        layout::Fragment const* c = find_box(page.result.root, "c");
        if (CHECK(wide && c)) {
            // A written width that fits beside the first float alone but not
            // both goes below them.
            CHECK_EQ(wide->x, c->x);
            CHECK_EQ(wide->y, c->y + 150.0f);
        }
    }

    // --- Percentage heights resolve against a definite containing height ---------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  div { margin: 0 }
</style></head><body>
<div id="outer" style="height: 200px"><div id="half" style="height: 50%"></div><div id="auto"><div id="deep" style="height: 25%">x</div></div><div id="capped" style="height: 80%; max-height: 10%"></div></div>
<div id="unknown"><div id="none" style="height: 50%">y</div></div>
<div id="flexc" style="display: flex; height: 100px"><div id="fi" style="height: 50%; width: 20px"></div></div>
<div id="rep" style="height: 100px"><iframe id="fr" height="50%"></iframe></div>
<div id="cb" style="position: relative; height: 120px"><img id="ai" style="position: absolute; height: 50%; width: 10px" src="x.png"></div>
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
        auto const height_of = [&](std::string_view id) {
            layout::Fragment const* box = find_box(page.result.root, id);
            return box ? box->height : -1.0f;
        };
        CHECK_EQ(height_of("half"), 100.0f); // half of a written 200
        CHECK_EQ(height_of("deep"), 20.0f); // its parent's height is auto: the percentage is auto too
        CHECK_EQ(height_of("capped"), 20.0f); // max-height in percent holds it
        CHECK_EQ(height_of("none"), 20.0f); // no definite base anywhere: auto
        CHECK_EQ(height_of("fi"), 50.0f); // a flex item's percentage against the container's definite height
        CHECK_EQ(height_of("fr"), 50.0f); // a replaced box's height attribute in percent
        CHECK_EQ(height_of("ai"), 60.0f); // an absolutely positioned picture against its containing block
    }

    // --- Table cells stand side by side as top-aligned inline-blocks, until tables land
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  td, th { padding: 0 }
</style></head><body>
<table><tr><th id="h">aa</th><td id="c">bb<div>cc</div></td><td id="d">dd</td></tr></table>
</body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const run_of = [&](std::u32string_view text) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return candidate;
            }
            return nullptr;
        };
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
        layout::TextRun const* aa = run_of(U"aa");
        layout::TextRun const* bb = run_of(U"bb");
        layout::TextRun const* cc = run_of(U"cc");
        layout::TextRun const* dd = run_of(U"dd");
        layout::Fragment const* h = find_box(page.result.root, "h");
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* d = find_box(page.result.root, "d");
        if (CHECK(aa && bb && cc && dd && h && c && d)) {
            // A table: 2px of border-spacing around each cell, the columns
            // as wide as their content, every cell as tall as its row.
            float const g = aa->width / 2;
            CHECK_EQ(h->x, 2.0f);
            CHECK_EQ(h->width, 2 * g); // each column shrinks to its content
            CHECK_EQ(c->x, 2 * g + 4); // the next cell past the gutter
            CHECK_EQ(c->height, 40.0f); // its block inside stays inside: two lines
            CHECK_EQ(h->height, 40.0f); // the row's height is every cell's
            CHECK_EQ(cc->x, c->x); // the block is the cell's, full width of the cell
            CHECK_EQ(d->x, 4 * g + 6);
            CHECK_EQ(h->y, c->y); // the cells start at the row's top
            CHECK_EQ(d->y, c->y);
            CHECK_EQ(aa->baseline_y, dd->baseline_y); // both centered in the row
            CHECK(aa->style->bold()); // th is bold
        }
    }

    // --- Replaced boxes on a line carry their edges; the embedded kinds are 300 by 150
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  p { margin: 0 }
  #pic { width: 20px; height: 10px; margin: 1px 4px; border: 2px solid black; padding: 3px }
  #box { width: 30px; margin-left: 5px }
</style></head><body>
<p>aa<img id="pic" src="x.png">bb</p>
<p><iframe id="frame"></iframe></p>
<p><canvas id="canvas" width="100" height="50"></canvas></p>
<p>cc<input id="box" type="checkbox">dd</p>
</body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const run_of = [&](std::u32string_view text) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return candidate;
            }
            return nullptr;
        };
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
        layout::TextRun const* aa = run_of(U"aa");
        layout::TextRun const* bb = run_of(U"bb");
        layout::Fragment const* pic = find_box(page.result.root, "pic");
        if (CHECK(aa && bb && pic && pic->image)) {
            float const g = aa->width / 2;
            CHECK_EQ(pic->x, 2 * g + 4); // past its left margin
            CHECK_EQ(pic->width, 30.0f); // border box: 20 + padding and border each side
            CHECK_EQ(pic->height, 20.0f);
            CHECK_EQ(pic->image->x, pic->x + 5); // the picture inside border and padding
            CHECK_EQ(pic->image->width, 20.0f);
            CHECK_EQ(pic->y + pic->height + 1, aa->baseline_y); // the bottom margin edge on the baseline
            CHECK_EQ(bb->x, 2 * g + 38); // margin, border box, margin
        }
        layout::Fragment const* frame = find_box(page.result.root, "frame");
        layout::Fragment const* canvas = find_box(page.result.root, "canvas");
        if (CHECK(frame && canvas)) {
            CHECK_EQ(frame->width, 300.0f); // no picture, no size written: CSS 2.1 §10.3.2
            CHECK_EQ(frame->height, 150.0f);
            CHECK_EQ(canvas->width, 100.0f); // its attributes size it
            CHECK_EQ(canvas->height, 50.0f);
        }
        layout::TextRun const* cc = run_of(U"cc");
        layout::TextRun const* dd = run_of(U"dd");
        layout::Fragment const* box = find_box(page.result.root, "box");
        if (CHECK(cc && dd && box && box->control)) {
            float const g = cc->width / 2;
            CHECK_EQ(box->x, 2 * g + 5); // a control's margin goes around its own box
            CHECK_EQ(box->width, 30.0f);
            CHECK_EQ(dd->x, 2 * g + 35 + 3); // the UA sheet's 3 px right margin on a checkbox
        }
    }

    // box-sizing: border-box — a written width, its bounds and a flex basis
    // name the border box, so the padding and the borders come off them; the
    // content floors at zero when they are wider than the written size. The
    // content-box box beside the first one is the control.
    {
        constexpr std::string_view html = R"(<!doctype html>
<html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  div { box-sizing: border-box }
  #w, #cb { width: 200px; padding: 0 10px; border-left: 5px solid; border-right: 5px solid }
  #cb { box-sizing: content-box }
  #mx { max-width: 200px; padding: 0 20px }
  #mn { width: 50px; min-width: 200px; padding: 0 20px }
  #pc { width: 50%; padding: 0 10px }
  #sm { width: 10px; padding: 0 20px }
  #h { height: 100px; padding: 20px 0; border-top: 5px solid; border-bottom: 5px solid }
  #flex { display: flex }
  #flex > div { flex: 0 0 100px; padding: 0 10px; border-left: 5px solid; border-right: 5px solid }
</style></head><body>
<div id=w><div id=wi>x</div></div>
<div id=cb><div id=cbi>x</div></div>
<div id=mx><div id=mxi>x</div></div>
<div id=mn><div id=mni>x</div></div>
<div id=pc><div id=pci>x</div></div>
<div id=sm><div id=smi>x</div></div>
<div id=h></div>
<div id=flex><div id=f1>a</div><div id=f2>b</div></div>
</body></html>)";
        Page const page = lay_out(html, 400);
        std::function<layout::Fragment const*(layout::Fragment const&, std::string_view)> find
            = [&](layout::Fragment const& f, std::string_view id) -> layout::Fragment const* {
            if (f.element) {
                dom::Attr const* attribute = f.element->find_attribute("id");
                if (attribute && attribute->value == id)
                    return &f;
            }
            for (layout::Fragment const& child : f.children) {
                if (layout::Fragment const* found = find(child, id))
                    return found;
            }
            return nullptr;
        };
        auto const box_of = [&](std::string_view id) { return find(page.result.root, id); };

        // A written width is the border box: 200 across, 170 of content.
        if (layout::Fragment const* w = box_of("w"); CHECK(w != nullptr)) {
            CHECK_EQ(w->width, 200.0f);
            if (layout::Fragment const* inner = box_of("wi"); CHECK(inner != nullptr))
                CHECK_EQ(inner->width, 170.0f); // 200 - 20 padding - 10 border
        }
        // The control: content-box puts the edges outside the 200.
        if (layout::Fragment const* cb = box_of("cb"); CHECK(cb != nullptr)) {
            CHECK_EQ(cb->width, 230.0f);
            if (layout::Fragment const* inner = box_of("cbi"); CHECK(inner != nullptr))
                CHECK_EQ(inner->width, 200.0f);
        }
        // A maximum names the border box: the auto width stops at 200.
        if (layout::Fragment const* mx = box_of("mx"); CHECK(mx != nullptr)) {
            CHECK_EQ(mx->width, 200.0f);
            if (layout::Fragment const* inner = box_of("mxi"); CHECK(inner != nullptr))
                CHECK_EQ(inner->width, 160.0f);
        }
        // So does a minimum: it lifts the border box from 50 to 200.
        if (layout::Fragment const* mn = box_of("mn"); CHECK(mn != nullptr)) {
            CHECK_EQ(mn->width, 200.0f);
            if (layout::Fragment const* inner = box_of("mni"); CHECK(inner != nullptr))
                CHECK_EQ(inner->width, 160.0f);
        }
        // A percentage resolves first, then the edges come off it.
        if (layout::Fragment const* pc = box_of("pc"); CHECK(pc != nullptr)) {
            CHECK_EQ(pc->width, 200.0f); // 50% of 400
            if (layout::Fragment const* inner = box_of("pci"); CHECK(inner != nullptr))
                CHECK_EQ(inner->width, 180.0f);
        }
        // Edges wider than the written width: the content floors at zero and
        // the border box is the edges themselves.
        if (layout::Fragment const* sm = box_of("sm"); CHECK(sm != nullptr)) {
            CHECK_EQ(sm->width, 40.0f);
            if (layout::Fragment const* inner = box_of("smi"); CHECK(inner != nullptr))
                CHECK_EQ(inner->width, 0.0f);
        }
        // A written height is the border box too.
        if (layout::Fragment const* h = box_of("h"); CHECK(h != nullptr))
            CHECK_EQ(h->height, 100.0f); // 50 of content, 40 padding, 10 border
        // A flex basis names the border box: each item is 100 across, the
        // second starting where the first ends.
        if (layout::Fragment const* f1 = box_of("f1"); CHECK(f1 != nullptr))
            CHECK_EQ(f1->width, 100.0f);
        if (layout::Fragment const* f2 = box_of("f2"); CHECK(f2 != nullptr)) {
            CHECK_EQ(f2->width, 100.0f);
            CHECK_EQ(f2->x, 100.0f);
        }
    }

    // --- List markers: outside in the padding, inside on the line --------------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  ol, ul { margin: 0; padding: 0 }
  #out { list-style-position: outside }
  #in { list-style-position: inside }
  #empty { list-style: square inside }
</style></head><body>
  <ol id="out"><li>a</li></ol>
  <ol id="in"><li>b</li></ol>
  <ul id="empty"><li></li></ul>
</body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        std::vector<layout::Fragment const*> boxes;
        auto const collect_boxes = [&](auto&& self, layout::Fragment const& f) -> void {
            if (f.element)
                boxes.push_back(&f);
            for (layout::Fragment const& child : f.children)
                self(self, child);
        };
        collect_boxes(collect_boxes, page.result.root);
        auto const box_of = [&](std::string_view id) -> layout::Fragment const* {
            for (layout::Fragment const* box : boxes) {
                dom::Attr const* attribute = box->element ? box->element->find_attribute("id") : nullptr;
                if (attribute && attribute->value == id)
                    return box;
            }
            return nullptr;
        };
        auto const run_of = [&](std::u32string_view text, bool negative) -> layout::TextRun const* {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text && (candidate->x < 0) == negative)
                    return candidate;
            }
            return nullptr;
        };
        // An outside marker hangs to the left of the content box, which is
        // why a list with its padding zeroed loses it off the page edge.
        layout::TextRun const* out_marker = run_of(U"1.", true);
        layout::TextRun const* a = run_of(U"a", false);
        if (CHECK(out_marker && a)) {
            CHECK(out_marker->x < 0);
            CHECK_EQ(a->x, 0.0f); // the content starts at the box, marker or no marker
        }
        // An inside marker is the first thing on the item's own first line,
        // so it starts at the content edge and the text follows it.
        layout::TextRun const* in_marker = run_of(U"1.", false);
        layout::TextRun const* b = run_of(U"b", false);
        if (CHECK(in_marker && b)) {
            CHECK_EQ(in_marker->x, 0.0f);
            CHECK(b->x > in_marker->x); // a space stands between the marker and the text
            CHECK_EQ(in_marker->baseline_y, b->baseline_y); // one line, not two
        }
        // An item holding nothing but an inside marker is not an empty box:
        // it makes a line, and its list is as tall as that line.
        layout::TextRun const* square = run_of(U"▪", false);
        layout::Fragment const* empty_list = box_of("empty");
        if (CHECK(square && empty_list)) {
            CHECK_EQ(square->x, 0.0f);
            CHECK_EQ(empty_list->height, 20.0f);
        }
    }

    // --- A marker's number is the list-item counter's ---------------------------
    {
        text::FontManager::instance().set_system_fonts(false);
        Page const page = lay_out(R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  ol { margin: 0 }
</style></head><body>
  <ol start="5"><li>a</li><li>b</li><li value="20">c</li><li>d</li></ol>
  <ol type="a"><li>e</li></ol>
  <ol><li>f</li></ol>
</body></html>)HTML", 400);
        std::vector<layout::TextRun const*> runs;
        collect(page.result.root, runs);
        auto const has = [&](std::u32string_view text) {
            for (layout::TextRun const* const candidate : runs) {
                if (candidate->text == text)
                    return true;
            }
            return false;
        };
        CHECK(has(U"5.") && has(U"6.")); // start puts the counter one below the first number
        CHECK(has(U"20.") && has(U"21.")); // value writes it, and the next item carries on
        CHECK(has(U"a.")); // type names the counter style
        CHECK(has(U"1.")); // a list of its own resets the counter it shares
        CHECK(!has(U"7.")); // the item after value=20 is not 7
    }

    return test::report("layout");
}
