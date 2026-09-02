#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "layout/TableStructure.h"
#include "layout/TableWidths.h"
#include "text/FontManager.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tables: the structure a table's children make (rows, cells, spans, the
// anonymous rows and cells around loose content), the column width
// algorithm on plain numbers, and whole pages — automatic widths, gutters
// and padding, spans, vertical alignment, captions and the header/footer
// order, presentational attributes, an anonymous table around loose cells,
// and an inline-table on a line. Sashfold Mono at 16px advances 10 px per
// glyph and lines are 20 px tall.

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
    if (fragment.element && fragment.style) {
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

dom::Element const* find_element(dom::Node const& node, std::string_view id)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (dom::Attr const* attribute = element.find_attribute("id"); attribute && attribute->value == id)
            return &element;
    }
    for (dom::Node const* child : node.children()) {
        if (dom::Element const* found = find_element(*child, id))
            return found;
    }
    return nullptr;
}

constexpr std::string_view head = R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  td { padding: 0 }
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
    using namespace layout::table;
    text::FontManager::instance().set_system_fonts(false);

    // --- Column widths on plain numbers -------------------------------------------
    {
        WidthInput input;
        input.columns = { { 10, 50, std::nullopt, std::nullopt }, { 20, 20, std::nullopt, std::nullopt } };
        input.available = 100;
        WidthResult result = compute_widths(input);
        CHECK(near(result.width, 70) && near(result.columns[0], 50) && near(result.columns[1], 20));
        CHECK(near(result.min, 30) && near(result.max, 70));
        // Less room than the maximum: the columns share it between their measures.
        input.available = 40;
        result = compute_widths(input);
        CHECK(near(result.width, 40) && near(result.columns[0], 20) && near(result.columns[1], 20));
        // A written width: the extra goes out in proportion to the maximums.
        input.width = 100;
        result = compute_widths(input);
        CHECK(near(result.width, 100));
        CHECK(near(result.columns[0], 50 + 30.0f * 50 / 70) && near(result.columns[1], 20 + 30.0f * 20 / 70));
        // A fixed column holds its width; the other takes the rest.
        input.columns[0].fixed = 30;
        result = compute_widths(input);
        CHECK(near(result.columns[0], 30) && near(result.columns[1], 70));
        // A spanning cell widens the columns it spans.
        input = WidthInput {};
        input.columns = { { 10, 10, std::nullopt, std::nullopt }, { 10, 10, std::nullopt, std::nullopt } };
        input.spans = { { 0, 2, 60, 60, std::nullopt, std::nullopt } };
        input.available = 800;
        result = compute_widths(input);
        CHECK(near(result.width, 60) && near(result.columns[0], 30) && near(result.columns[1], 30));
        // Gutters and edges count in the table's width; the spanning cell's
        // 60 covers the gutter between its columns.
        input.spacing = 4;
        input.edges = 2;
        result = compute_widths(input);
        CHECK(near(result.width, 60 + 8 + 2) && near(result.columns[0], 28));
        // Fixed layout: the first row's widths hold, the rest share equally.
        input = WidthInput {};
        input.columns = { { 50, 200, 40, std::nullopt }, { 50, 200, std::nullopt, std::nullopt },
            { 50, 200, std::nullopt, std::nullopt } };
        input.width = 100;
        input.fixed_layout = true;
        result = compute_widths(input);
        CHECK(near(result.columns[0], 40) && near(result.columns[1], 30) && near(result.columns[2], 30));
    }

    // --- The structure: spans, groups, and the anonymous boxes -----------------------
    {
        Page const page = lay_out(page_with(R"(<table id="t"><thead><tr><td>h</td></tr></thead><tr><td>1</td><td>2</td></tr><tr><td colspan="2">3</td></tr></table>
<div id="u" style="display:table"><div style="display:table-row">loose<div style="display:table-cell">x</div></div><div style="display:table-cell">cell</div>text</div>)"));
        dom::Element const* t = find_element(*page.document, "t");
        dom::Element const* u = find_element(*page.document, "u");
        if (CHECK(t && u)) {
            auto const style_of = [&](dom::Element const& element) -> css::ComputedStyle const* {
                auto const it = page.styles.find(&element);
                return it == page.styles.end() ? nullptr : &it->second;
            };
            std::vector<dom::Node const*> const t_children(t->children().begin(), t->children().end());
            Structure const s = build_structure(t_children, *style_of(*t), style_of);
            CHECK_EQ(s.groups.size(), std::size_t(2));
            CHECK(s.groups[0].kind == RowGroup::Kind::Header);
            CHECK_EQ(s.rows.size(), std::size_t(3));
            CHECK_EQ(s.column_count, 2);
            CHECK_EQ(s.cells.size(), std::size_t(4));
            CHECK_EQ(s.cells[3].column_span, 2);
            CHECK_EQ(s.cell_at(2, 1), 3);
            CHECK_EQ(s.cell_at(0, 1), -1);
            std::vector<dom::Node const*> const u_children(u->children().begin(), u->children().end());
            Structure const a = build_structure(u_children, *style_of(*u), style_of);
            // "loose" and "x" in the row; then a row of its own around the
            // cell and the text, the text in an anonymous cell.
            CHECK_EQ(a.rows.size(), std::size_t(2));
            CHECK_EQ(a.rows[0].cells.size(), std::size_t(2));
            CHECK(a.cells[a.rows[0].cells[0]].anonymous());
            CHECK_EQ(a.cells[a.rows[0].cells[0]].nodes.size(), std::size_t(1));
            CHECK(!a.cells[a.rows[0].cells[1]].anonymous());
            CHECK(a.rows[1].element == nullptr);
            CHECK_EQ(a.rows[1].cells.size(), std::size_t(2));
            CHECK(a.cells[a.rows[1].cells[1]].anonymous());
        }
    }

    // --- Automatic widths from the content -------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<table id="t" style="border-spacing:0"><tr><td id="a">aa</td><td id="b">bbbb</td></tr><tr><td id="c">c</td><td id="d">dd</td></tr></table>)"));
        layout::Fragment const* t = find_box(page.result.root, "t");
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* d = find_box(page.result.root, "d");
        if (CHECK(t && a && b && c && d)) {
            CHECK(near(t->width, 60) && near(t->height, 40));
            CHECK(near(a->x, 0) && near(a->y, 0) && near(a->width, 20) && near(a->height, 20));
            CHECK(near(b->x, 20) && near(b->width, 40));
            CHECK(near(c->x, 0) && near(c->y, 20) && near(c->width, 20));
            CHECK(near(d->x, 20) && near(d->y, 20) && near(d->width, 40));
        }
    }

    // --- Gutters, padding, a written width ---------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<table id="t" style="border-spacing:4px; width:200px"><tr><td id="e" style="padding:2px">e</td><td id="f" style="padding:2px">f</td></tr></table>)"));
        layout::Fragment const* t = find_box(page.result.root, "t");
        layout::Fragment const* e = find_box(page.result.root, "e");
        layout::Fragment const* f = find_box(page.result.root, "f");
        layout::TextRun const* run = find_run(page.result.root, U"e");
        if (CHECK(t && e && f && run)) {
            CHECK(near(t->width, 200) && near(t->height, 32));
            CHECK(near(e->x, 4) && near(e->y, 4) && near(e->width, 94) && near(e->height, 24));
            CHECK(near(f->x, 102) && near(f->width, 94));
            CHECK(near(run->x, 6));
        }
    }

    // --- colspan and rowspan ------------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<table id="t" style="border-spacing:0"><tr><td id="g" colspan="2">gggggg</td><td id="h" rowspan="2">h</td></tr><tr><td id="i">i</td><td id="j">j</td></tr></table>)"));
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::Fragment const* h = find_box(page.result.root, "h");
        layout::Fragment const* i = find_box(page.result.root, "i");
        layout::Fragment const* j = find_box(page.result.root, "j");
        layout::Fragment const* t = find_box(page.result.root, "t");
        if (CHECK(g && h && i && j && t)) {
            CHECK(near(t->width, 70));
            CHECK(near(g->x, 0) && near(g->width, 60) && near(g->height, 20));
            CHECK(near(h->x, 60) && near(h->width, 10) && near(h->height, 40));
            CHECK(near(i->x, 0) && near(i->y, 20) && near(i->width, 30));
            CHECK(near(j->x, 30) && near(j->y, 20) && near(j->width, 30));
        }
    }

    // --- Vertical alignment: the row group's middle, and a bottom --------------------
    {
        Page const page = lay_out(page_with(R"(<table id="t" style="border-spacing:0"><tr><td id="k">k<br>k<br>k</td><td id="l">l</td><td id="m" style="vertical-align: bottom">m</td><td id="n" style="vertical-align: top">n</td></tr></table>)"));
        layout::Fragment const* l = find_box(page.result.root, "l");
        layout::TextRun const* k = find_run(page.result.root, U"k");
        layout::TextRun const* run_l = find_run(page.result.root, U"l");
        layout::TextRun const* run_m = find_run(page.result.root, U"m");
        layout::TextRun const* run_n = find_run(page.result.root, U"n");
        if (CHECK(l && k && run_l && run_m && run_n)) {
            CHECK(near(l->height, 60));
            CHECK(near(run_l->baseline_y - k->baseline_y, 20));
            CHECK(near(run_m->baseline_y - k->baseline_y, 40));
            CHECK(near(run_n->baseline_y - k->baseline_y, 0));
        }
    }

    // --- Captions, and the header and footer groups in their places -------------------
    {
        Page const page = lay_out(page_with(R"(<table id="t" style="border-spacing:0"><caption id="cap" style="margin:0">Cap</caption><tfoot><tr><td id="foot">foot</td></tr></tfoot><tbody><tr><td id="body">body</td></tr></tbody><thead><tr><td id="head">head</td></tr></thead></table>)"));
        layout::Fragment const* t = find_box(page.result.root, "t");
        layout::Fragment const* cap = find_box(page.result.root, "cap");
        layout::Fragment const* head_cell = find_box(page.result.root, "head");
        layout::Fragment const* body_cell = find_box(page.result.root, "body");
        layout::Fragment const* foot_cell = find_box(page.result.root, "foot");
        layout::TextRun const* cap_run = find_run(page.result.root, U"Cap");
        if (CHECK(t && cap && head_cell && body_cell && foot_cell && cap_run)) {
            CHECK(near(cap->y, 0) && near(cap->width, 40) && near(cap->height, 20));
            CHECK(near(cap_run->x, 5));
            CHECK(near(t->y, 20) && near(t->height, 60));
            CHECK(near(head_cell->y, 20) && near(body_cell->y, 40) && near(foot_cell->y, 60));
        }
    }

    // --- Presentational attributes, and a table around loose cells --------------------
    {
        Page const page = lay_out(page_with(R"(<div id="wrap" style="width:300px"><div id="m" style="display:table-cell">mm</div><div id="n" style="display:table-cell">nn</div></div>
<table id="t" cellpadding="3" cellspacing="0" border="1" width="100"><tr><td id="o">o</td></tr></table>)"));
        layout::Fragment const* m = find_box(page.result.root, "m");
        layout::Fragment const* n = find_box(page.result.root, "n");
        layout::Fragment const* wrap = find_box(page.result.root, "wrap");
        layout::Fragment const* t = find_box(page.result.root, "t");
        layout::Fragment const* o = find_box(page.result.root, "o");
        layout::TextRun const* run = find_run(page.result.root, U"o");
        if (CHECK(m && n && wrap && t && o && run)) {
            CHECK(near(m->x, 0) && near(m->y, 0) && near(m->width, 20));
            CHECK(near(n->x, 20) && near(n->width, 20));
            CHECK(near(wrap->height, 20));
            // border=1 puts a 1px border on the table and its cells;
            // cellpadding=3 loses to the page's own td { padding: 0 }.
            CHECK(near(t->y, 20) && near(t->width, 100) && near(t->height, 24));
            CHECK(near(o->x, 1) && near(o->width, 98) && near(o->height, 22));
            CHECK(near(run->x, 2));
        }
    }

    // --- An inline-table on a line --------------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<p id="p">a <span id="it" style="display:inline-table; border-spacing:0"><span style="display:table-cell">xy</span></span> b</p>)"));
        layout::Fragment const* it = find_box(page.result.root, "it");
        layout::TextRun const* a = find_run(page.result.root, U"a");
        layout::TextRun const* b = find_run(page.result.root, U"b");
        layout::TextRun const* xy = find_run(page.result.root, U"xy");
        if (CHECK(it && a && b && xy)) {
            CHECK(near(a->x, 0));
            CHECK(near(it->x, 20) && near(it->width, 20) && near(it->height, 20));
            CHECK(near(xy->x, 20));
            CHECK(near(b->x, 50));
        }
    }

    return sashfold::test::report("table");
}
