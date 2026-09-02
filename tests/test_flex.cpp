#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "text/FontManager.h"

#include <cmath>
#include <memory>
#include <string>
#include <string_view>

// Flexbox: rows and columns, growing and shrinking with the automatic
// minimum, wrapping, main- and cross-axis alignment, gaps, order, anonymous
// text items, reversed directions, and a container beside a float. Sashfold
// Mono at 16px advances 10 px per glyph and lines are 20 px tall, so the
// numbers below are exact.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document;
    css::StyleMap styles;
    layout::LayoutResult result;
};

Page lay_out(std::string_view html)
{
    Page page;
    page.document = html::parse_document(html);
    page.styles = css::resolve_styles(*page.document);
    page.result = layout::layout_document(*page.document, page.styles, 800);
    return page;
}

layout::Fragment const* find_box(layout::Fragment const& fragment, std::string_view id)
{
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
}

layout::TextRun const* find_run(layout::Fragment const& fragment, std::u32string_view text)
{
    for (layout::TextRun const& run : fragment.runs) {
        if (run.text == text)
            return &run;
    }
    for (layout::Fragment const& child : fragment.children) {
        if (layout::TextRun const* found = find_run(child, text))
            return found;
    }
    return nullptr;
}

constexpr std::string_view head = R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  .c { display: flex; width: 300px }
  .w50 { width: 50px }
  .w60 { width: 60px }
  .w70 { width: 70px }
</style></head><body>)HTML";

std::string page_with(std::string_view body)
{
    return std::string(head) + std::string(body) + "</body></html>";
}

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.01f;
}

} // namespace

int main()
{
    text::FontManager::instance().set_system_fonts(false);

    // --- A row of items with written widths -------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" id="c"><div id="a" class="w50">a</div><div id="b" class="w60">b</div><div id="d" class="w70">d</div></div>)"));
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* d = find_box(page.result.root, "d");
        if (CHECK(c && a && b && d)) {
            CHECK_EQ(a->x, 0.0f);
            CHECK_EQ(b->x, 50.0f);
            CHECK_EQ(d->x, 110.0f);
            CHECK_EQ(a->width, 50.0f);
            CHECK_EQ(a->height, 20.0f); // stretched to the line, one line tall
            CHECK_EQ(c->height, 20.0f);
            CHECK_EQ(c->width, 300.0f);
        }
    }

    // --- flex: 1 shares the line equally; weights share it in proportion ------
    {
        Page const page = lay_out(page_with(R"(<div class="c"><div id="a" style="flex:1">a</div><div id="b" style="flex:1">b</div><div id="d" style="flex:1">d</div></div>
<div class="c"><div id="e" class="w50" style="flex-grow:1">e</div><div id="f" class="w50" style="flex-grow:2">f</div><div id="g" class="w50" style="flex-grow:1">g</div></div>
<div class="c"><div id="h" style="flex:none;width:100px">h</div><div id="i" style="flex:1">i</div><div id="j" style="flex:1">j</div></div>)"));
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* d = find_box(page.result.root, "d");
        if (CHECK(a && b && d)) {
            CHECK_EQ(a->width, 100.0f);
            CHECK_EQ(b->x, 100.0f);
            CHECK_EQ(d->x, 200.0f);
        }
        layout::Fragment const* e = find_box(page.result.root, "e");
        layout::Fragment const* f = find_box(page.result.root, "f");
        layout::Fragment const* g = find_box(page.result.root, "g");
        if (CHECK(e && f && g)) {
            CHECK_EQ(e->width, 87.5f); // 150 free: 37.5 + 75 + 37.5
            CHECK_EQ(f->width, 125.0f);
            CHECK_EQ(g->width, 87.5f);
            CHECK_EQ(g->x, 212.5f);
        }
        layout::Fragment const* h = find_box(page.result.root, "h");
        layout::Fragment const* i = find_box(page.result.root, "i");
        if (CHECK(h && i)) {
            CHECK_EQ(h->width, 100.0f); // flex: none keeps its width
            CHECK_EQ(i->width, 100.0f); // the other two share 200
        }
    }

    // --- Shrinking, and the automatic minimum ---------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c"><div id="a" style="width:200px">a</div><div id="b" style="width:200px">b</div></div>
<div class="c"><div id="d" style="width:200px">abcdefghijklmnopqrstuvwxyz</div><div id="e" style="width:200px">e</div></div>)"));
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        if (CHECK(a && b)) {
            CHECK_EQ(a->width, 150.0f); // 100 over, shared by base size
            CHECK_EQ(b->width, 150.0f);
            CHECK_EQ(b->x, 150.0f);
        }
        layout::Fragment const* d = find_box(page.result.root, "d");
        layout::Fragment const* e = find_box(page.result.root, "e");
        if (CHECK(d && e)) {
            CHECK_EQ(d->width, 200.0f); // its word is 260 wide: its minimum is its written 200
            CHECK_EQ(e->width, 100.0f); // so the other takes the whole cut
        }
    }

    // --- Wrapping ---------------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" id="c" style="flex-wrap:wrap"><div id="a" style="width:100px">a</div><div id="b" style="width:100px">b</div><div id="d" style="width:100px">d</div><div id="e" style="width:100px">e</div></div>)"));
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* d = find_box(page.result.root, "d");
        layout::Fragment const* e = find_box(page.result.root, "e");
        if (CHECK(c && d && e)) {
            CHECK_EQ(d->x, 200.0f);
            CHECK_EQ(d->y, 0.0f);
            CHECK_EQ(e->x, 0.0f); // the fourth starts the second line
            CHECK_EQ(e->y, 20.0f);
            CHECK_EQ(c->height, 40.0f);
        }
    }

    // --- justify-content --------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" style="justify-content:center"><div id="a1" class="w50">a</div><div id="b1" class="w50">b</div></div>
<div class="c" style="justify-content:space-between"><div id="a2" class="w50">a</div><div id="b2" class="w50">b</div></div>
<div class="c" style="justify-content:space-around"><div id="a3" class="w50">a</div><div id="b3" class="w50">b</div></div>
<div class="c" style="justify-content:space-evenly"><div id="a4" class="w50">a</div><div id="b4" class="w50">b</div></div>
<div class="c" style="justify-content:flex-end"><div id="a5" class="w50">a</div><div id="b5" class="w50">b</div></div>)"));
        auto const x_of = [&](std::string_view id) {
            layout::Fragment const* box = find_box(page.result.root, id);
            return box ? box->x : -1.0f;
        };
        CHECK_EQ(x_of("a1"), 100.0f);
        CHECK_EQ(x_of("b1"), 150.0f);
        CHECK_EQ(x_of("a2"), 0.0f);
        CHECK_EQ(x_of("b2"), 250.0f);
        CHECK_EQ(x_of("a3"), 50.0f);
        CHECK_EQ(x_of("b3"), 200.0f);
        CHECK(near(x_of("a4"), 200.0f / 3.0f));
        CHECK(near(x_of("b4"), 200.0f / 3.0f * 2.0f + 50.0f));
        CHECK_EQ(x_of("a5"), 200.0f);
        CHECK_EQ(x_of("b5"), 250.0f);
    }

    // --- align-items in a tall row ---------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" style="height:100px"><div id="a" class="w50">a</div><div id="b" class="w50" style="align-self:center">b</div><div id="d" class="w50" style="align-self:flex-end">d</div><div id="e" class="w50" style="align-self:flex-start">e</div></div>
<div class="c" id="c2" style="height:100px;align-items:center"><div id="f" class="w50">f</div></div>)"));
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* d = find_box(page.result.root, "d");
        layout::Fragment const* e = find_box(page.result.root, "e");
        layout::Fragment const* f = find_box(page.result.root, "f");
        if (CHECK(a && b && d && e && f)) {
            CHECK_EQ(a->height, 100.0f); // stretch
            CHECK_EQ(b->y, 40.0f);
            CHECK_EQ(b->height, 20.0f);
            CHECK_EQ(d->y, 80.0f);
            CHECK_EQ(e->y, 0.0f);
            CHECK_EQ(e->height, 20.0f);
            CHECK_EQ(f->y, 140.0f); // the second container starts at 100
        }
    }

    // --- Columns ------------------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" id="c" style="flex-direction:column"><div id="a">a</div><div id="b">b</div><div id="d">d</div></div>
<div class="c" style="flex-direction:column;align-items:flex-start"><div id="e">e</div></div>
<div class="c" style="flex-direction:column;height:300px"><div id="f" style="flex:1">f</div><div id="g" style="flex:1">g</div><div id="h" style="flex:1">h</div></div>)"));
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* d = find_box(page.result.root, "d");
        layout::Fragment const* e = find_box(page.result.root, "e");
        if (CHECK(c && a && b && d && e)) {
            CHECK_EQ(a->y, 0.0f);
            CHECK_EQ(b->y, 20.0f);
            CHECK_EQ(d->y, 40.0f);
            CHECK_EQ(a->width, 300.0f); // stretched across
            CHECK_EQ(c->height, 60.0f);
            CHECK_EQ(e->width, 10.0f); // flex-start: shrink-to-fit around "e"
        }
        layout::Fragment const* f = find_box(page.result.root, "f");
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::Fragment const* h = find_box(page.result.root, "h");
        if (CHECK(f && g && h)) {
            CHECK_EQ(f->height, 100.0f);
            CHECK_EQ(g->y, f->y + 100.0f);
            CHECK_EQ(h->y, f->y + 200.0f);
        }
    }

    // --- Gaps, order, margins ----------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" style="gap:10px"><div id="a" class="w50">a</div><div id="b" class="w60">b</div><div id="d" class="w70">d</div></div>
<div class="c"><div id="e" class="w50" style="order:2">e</div><div id="f" class="w50" style="order:1">f</div></div>
<div class="c" id="c3"><div id="g" class="w50" style="margin:10px">g</div><div id="h" class="w50" style="margin:10px">h</div></div>)"));
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* d = find_box(page.result.root, "d");
        if (CHECK(b && d)) {
            CHECK_EQ(b->x, 60.0f);
            CHECK_EQ(d->x, 130.0f);
        }
        layout::Fragment const* e = find_box(page.result.root, "e");
        layout::Fragment const* f = find_box(page.result.root, "f");
        if (CHECK(e && f)) {
            CHECK_EQ(f->x, 0.0f); // order 1 comes first
            CHECK_EQ(e->x, 50.0f);
        }
        layout::Fragment const* c3 = find_box(page.result.root, "c3");
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::Fragment const* h = find_box(page.result.root, "h");
        if (CHECK(c3 && g && h)) {
            CHECK_EQ(g->x, 10.0f);
            CHECK_EQ(g->y, c3->y + 10.0f);
            CHECK_EQ(h->x, 80.0f);
            CHECK_EQ(c3->height, 40.0f); // the margins are part of the line
        }
    }

    // --- Text of the container's own becomes an item ---------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" id="c">Hello<span id="s">World</span></div>)"));
        layout::TextRun const* hello = find_run(page.result.root, U"Hello");
        layout::Fragment const* s = find_box(page.result.root, "s");
        if (CHECK(hello && s)) {
            CHECK_EQ(hello->x, 0.0f);
            CHECK_EQ(s->x, 50.0f);
            CHECK_EQ(s->width, 50.0f); // a span is blockified into an item of its content's width
        }
    }

    // --- Reversed rows, and a basis in percent ---------------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" style="flex-direction:row-reverse"><div id="a" class="w50">a</div><div id="b" class="w60">b</div></div>
<div class="c"><div id="d" style="flex-basis:50%">d</div><div id="e" class="w50">e</div></div>)"));
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        if (CHECK(a && b)) {
            CHECK_EQ(a->x, 250.0f); // the first item sits at the main end
            CHECK_EQ(b->x, 190.0f);
        }
        layout::Fragment const* d = find_box(page.result.root, "d");
        if (CHECK(d != nullptr))
            CHECK_EQ(d->width, 150.0f);
    }

    // --- Nested containers, and one beside a float -----------------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c"><div id="inner" class="c" style="width:auto;flex:1"><div id="n1" style="flex:1">1</div><div id="n2" style="flex:1">2</div></div></div>
<div style="width:300px"><span style="float:left;width:100px;height:30px"></span><div class="c" id="beside" style="width:auto"><div id="x" style="flex:1">x</div></div></div>)"));
        layout::Fragment const* inner = find_box(page.result.root, "inner");
        layout::Fragment const* n1 = find_box(page.result.root, "n1");
        layout::Fragment const* n2 = find_box(page.result.root, "n2");
        if (CHECK(inner && n1 && n2)) {
            CHECK_EQ(inner->width, 300.0f);
            CHECK_EQ(n1->width, 150.0f);
            CHECK_EQ(n2->x, 150.0f);
        }
        layout::Fragment const* beside = find_box(page.result.root, "beside");
        layout::Fragment const* x = find_box(page.result.root, "x");
        if (CHECK(beside && x)) {
            CHECK_EQ(beside->x, 100.0f); // a flex container keeps clear of the float
            CHECK_EQ(beside->width, 200.0f);
            CHECK_EQ(x->width, 200.0f);
        }
    }

    // --- align-content over wrapped lines in a tall container -------------------
    {
        Page const page = lay_out(page_with(R"(<div class="c" style="flex-wrap:wrap;height:100px"><div id="a" style="width:200px">a</div><div id="b" style="width:200px">b</div></div>
<div class="c" style="flex-wrap:wrap;height:100px;align-content:flex-end"><div id="d" style="width:200px">d</div><div id="e" style="width:200px">e</div></div>)"));
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* d = find_box(page.result.root, "d");
        layout::Fragment const* e = find_box(page.result.root, "e");
        if (CHECK(a && b && d && e)) {
            CHECK_EQ(a->height, 50.0f); // stretch: two lines share the 100
            CHECK_EQ(b->y, 50.0f);
            CHECK_EQ(d->y, 160.0f); // flex-end: the lines sit at the bottom (container at 100)
            CHECK_EQ(d->height, 20.0f);
            CHECK_EQ(e->y, 180.0f);
        }
    }

    // --- A wrapping container whose items fit one line keeps that line its items' size
    {
        Page const page = lay_out(page_with(R"(<div class="c" style="flex-wrap:wrap;height:100px;align-content:flex-start"><div id="fits" style="width:100px">f</div></div>
<div class="c" style="flex-wrap:wrap;height:100px"><div id="stretched" style="width:100px">t</div></div>
<div class="c" style="height:100px"><div id="single" style="width:100px">s</div></div>)"));
        layout::Fragment const* fits = find_box(page.result.root, "fits");
        layout::Fragment const* stretched = find_box(page.result.root, "stretched");
        layout::Fragment const* single = find_box(page.result.root, "single");
        if (CHECK(fits && stretched && single)) {
            CHECK_EQ(fits->height, 20.0f); // multi-line: the line is as tall as its item, which stretches to it
            CHECK_EQ(stretched->height, 100.0f); // the same line stretched to the container by align-content
            CHECK_EQ(single->height, 100.0f); // single-line: the line is the container's height
        }
    }

    return sashfold::test::report("flex");
}
