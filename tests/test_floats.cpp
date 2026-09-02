#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "text/FontManager.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Floats: placement against the containing block and each other, lines
// shortened beside them, clearance, shrink-to-fit widths, the boxes that
// contain their floats and those that keep clear of them, and the page
// reaching around a float at its end. Sashfold Mono at 16px advances 10 px
// per glyph, so the arithmetic below is exact.

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
  div { width: 300px }
  p { margin: 0 }
  .f { float: left; width: 100px; height: 30px }
  .r { float: right; width: 100px; height: 30px }
  .wide { float: left; width: 150px; height: 30px }
  .s { float: left }
</style></head><body>)HTML";

std::string page_with(std::string_view body)
{
    return std::string(head) + std::string(body) + "</body></html>";
}

} // namespace

int main()
{
    text::FontManager::instance().set_system_fonts(false);

    // --- A left float, and the lines beside and below it ---------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><span class="f" id="f"></span>aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp</div>)"), 800);
        layout::Fragment const* f = find_box(page.result.root, "f");
        layout::Fragment const* c = find_box(page.result.root, "c");
        if (CHECK(f != nullptr) && CHECK(c != nullptr)) {
            CHECK_EQ(f->x, 0.0f);
            CHECK_EQ(f->y, 0.0f);
            CHECK_EQ(f->width, 100.0f);
            CHECK_EQ(f->height, 30.0f);
            CHECK(f->floating);
            CHECK_EQ(c->height, 60.0f); // three lines; the float adds nothing
        }
        // Lines at y 0 and 20 have 200 px beside the float: seven words each.
        layout::TextRun const* aa = find_run(page.result.root, U"aa");
        layout::TextRun const* hh = find_run(page.result.root, U"hh");
        layout::TextRun const* oo = find_run(page.result.root, U"oo");
        if (CHECK(aa && hh && oo)) {
            CHECK_EQ(aa->x, 100.0f);
            CHECK_EQ(hh->x, 100.0f);
            CHECK_EQ(oo->x, 0.0f); // the third line, at y 40, is below the float
            CHECK_EQ(oo->baseline_y - aa->baseline_y, 40.0f);
        }
    }

    // --- A right float ----------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><span class="r" id="r"></span>aa bb</div>)"), 800);
        layout::Fragment const* r = find_box(page.result.root, "r");
        layout::TextRun const* aa = find_run(page.result.root, U"aa");
        if (CHECK(r != nullptr) && CHECK(aa != nullptr)) {
            CHECK_EQ(r->x, 200.0f);
            CHECK_EQ(r->y, 0.0f);
            CHECK_EQ(aa->x, 0.0f);
        }
    }

    // --- Floats stack beside each other, and drop when they cannot fit ----------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><span class="f" id="a"></span><span class="f" id="b"></span><span class="wide" id="w"></span><span class="r" id="r"></span>text</div>)"), 800);
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* w = find_box(page.result.root, "w");
        layout::Fragment const* r = find_box(page.result.root, "r");
        if (CHECK(a && b && w && r)) {
            CHECK_EQ(a->x, 0.0f);
            CHECK_EQ(b->x, 100.0f);
            CHECK_EQ(b->y, 0.0f);
            CHECK_EQ(w->x, 0.0f); // 250 + 150 > 300: below the first two
            CHECK_EQ(w->y, 30.0f);
            CHECK_EQ(r->x, 200.0f); // no higher than the float placed before it
            CHECK_EQ(r->y, 30.0f);
        }
        layout::TextRun const* text = find_run(page.result.root, U"text");
        if (CHECK(text != nullptr)) {
            CHECK_EQ(text->x, 200.0f); // the first line, at y 0, beside a and b
        }
    }

    // --- Clearance --------------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><span class="f" id="f"></span><p id="left" style="clear:left">below</p><div id="none">beside</div></div>)"), 800);
        layout::Fragment const* below = find_box(page.result.root, "left");
        layout::Fragment const* none = find_box(page.result.root, "none");
        layout::TextRun const* beside = find_run(page.result.root, U"beside");
        if (CHECK(below && none && beside)) {
            CHECK_EQ(below->y, 30.0f);
            CHECK_EQ(none->y, 50.0f);
            CHECK_EQ(beside->x, 0.0f); // below the float by then
        }
        Page const other = lay_out(page_with(R"(<div id="c"><span class="r" id="r"></span><p id="p" style="clear:left">left only</p></div>)"), 800);
        layout::Fragment const* p = find_box(other.result.root, "p");
        if (CHECK(p != nullptr))
            CHECK_EQ(p->y, 0.0f); // clear: left ignores a right float
    }

    // --- <br clear> ---------------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><span class="f" id="f"></span>aa<br clear="all">bb</div>)"), 800);
        layout::TextRun const* aa = find_run(page.result.root, U"aa");
        layout::TextRun const* bb = find_run(page.result.root, U"bb");
        if (CHECK(aa && bb)) {
            CHECK_EQ(aa->x, 100.0f);
            CHECK_EQ(bb->x, 0.0f);
            CHECK_EQ(bb->baseline_y - aa->baseline_y, 30.0f); // from y 0 to the float's bottom
        }
    }

    // --- Who contains a float ---------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="plain"><span class="f"></span></div>
<div id="hidden" style="overflow:hidden;clear:both"><span class="f"></span></div>
<div id="root" style="display:flow-root;clear:both"><span class="f"></span></div>)"), 800);
        layout::Fragment const* plain = find_box(page.result.root, "plain");
        layout::Fragment const* hidden = find_box(page.result.root, "hidden");
        layout::Fragment const* root = find_box(page.result.root, "root");
        if (CHECK(plain && hidden && root)) {
            CHECK_EQ(plain->height, 0.0f); // a float adds nothing to a plain block
            CHECK_EQ(hidden->height, 30.0f); // a formatting context reaches around its floats
            CHECK_EQ(root->height, 30.0f);
            CHECK_EQ(hidden->y, 30.0f); // cleared below the first float
            CHECK_EQ(root->y, 60.0f);
        }
    }

    // --- Shrink-to-fit widths ----------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><span class="s" id="s">Hello world</span>after</div>
<div id="n" style="width:80px;clear:both"><span class="s" id="t">Hello world</span></div>)"), 800);
        layout::Fragment const* s = find_box(page.result.root, "s");
        layout::Fragment const* t = find_box(page.result.root, "t");
        layout::TextRun const* after = find_run(page.result.root, U"after");
        if (CHECK(s && t && after)) {
            CHECK_EQ(s->width, 110.0f); // everything on one line: 11 glyphs
            CHECK_EQ(s->height, 20.0f);
            CHECK_EQ(after->x, 110.0f);
            CHECK_EQ(t->width, 80.0f); // the room there is, above the widest word (50)
            CHECK_EQ(t->height, 40.0f); // so the two words wrap
        }
    }

    // --- A box with its own formatting context keeps clear of floats -------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><span class="f" id="f"></span><div id="bfc" style="overflow:hidden;width:auto">x</div><div id="fixed" style="overflow:hidden;width:250px">y</div></div>)"), 800);
        layout::Fragment const* bfc = find_box(page.result.root, "bfc");
        layout::Fragment const* fixed = find_box(page.result.root, "fixed");
        if (CHECK(bfc && fixed)) {
            CHECK_EQ(bfc->x, 100.0f); // beside the float, narrowed to the room left
            CHECK_EQ(bfc->width, 200.0f);
            CHECK_EQ(bfc->y, 0.0f);
            CHECK_EQ(fixed->y, 30.0f); // 250 does not fit beside it: below
            CHECK_EQ(fixed->x, 0.0f);
        }
    }

    // --- A floated picture ------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><img id="i" style="float:right" width="50" height="40">aa</div>)"), 800);
        layout::Fragment const* i = find_box(page.result.root, "i");
        layout::TextRun const* aa = find_run(page.result.root, U"aa");
        if (CHECK(i && aa)) {
            CHECK_EQ(i->x, 250.0f);
            CHECK_EQ(i->width, 50.0f);
            CHECK_EQ(i->height, 40.0f);
            CHECK(i->floating);
            CHECK_EQ(aa->x, 0.0f);
        }
    }

    // --- A float met mid-line goes at the line's top; the line makes room ------
    {
        Page const page = lay_out(page_with(R"(<div id="c">aa <span class="f" id="f"></span>bb</div>)"), 800);
        layout::Fragment const* f = find_box(page.result.root, "f");
        layout::TextRun const* aa = find_run(page.result.root, U"aa");
        layout::TextRun const* bb = find_run(page.result.root, U"bb");
        if (CHECK(f && aa && bb)) {
            CHECK_EQ(f->x, 0.0f);
            CHECK_EQ(f->y, 0.0f);
            CHECK_EQ(aa->x, 100.0f);
            CHECK_EQ(bb->x, 130.0f);
            CHECK_EQ(aa->baseline_y, bb->baseline_y);
        }
    }

    // --- A float between blocks leaves their margins collapsing ------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><p style="margin:0 0 20px">p1</p><span class="f" id="f"></span><p id="p2" style="margin:20px 0 0">p2</p></div>)"), 800);
        layout::Fragment const* f = find_box(page.result.root, "f");
        layout::Fragment const* p2 = find_box(page.result.root, "p2");
        layout::TextRun const* text = find_run(page.result.root, U"p2");
        if (CHECK(f && p2 && text)) {
            CHECK_EQ(f->y, 40.0f); // after p1's bottom margin
            CHECK_EQ(p2->y, 40.0f); // 20 and 20 collapse to 20
            CHECK_EQ(text->x, 100.0f); // its line flows beside the float
        }
    }

    // --- The page reaches around a float at its end ------------------------------
    {
        Page const page = lay_out(page_with(R"(<p>one</p><div id="c"><span class="f" id="f"></span></div>)"), 800);
        layout::Fragment const* f = find_box(page.result.root, "f");
        if (CHECK(f != nullptr)) {
            CHECK_EQ(f->y, 20.0f);
            CHECK_EQ(page.result.page_height, 50.0f); // the float's bottom, not the line's
        }
    }

    // --- Nothing changes for a page without floats -------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="c"><p>aa</p><p style="margin-top:10px">bb</p></div>)"), 800);
        layout::Fragment const* c = find_box(page.result.root, "c");
        if (CHECK(c != nullptr))
            CHECK_EQ(c->height, 50.0f);
    }

    return sashfold::test::report("floats");
}
