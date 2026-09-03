#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/GridAlgorithm.h"
#include "layout/Layout.h"
#include "text/FontManager.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Grid: the placement and sizing algorithms on plain numbers, the value
// parsers through the cascade, and whole pages laid out — explicit and
// implicit tracks, fr, auto-repeat, areas and named lines, spans,
// alignment, auto margins, gaps, order, and an inline-grid's shrink-to-fit.
// Sashfold Mono at 16px advances 10 px per glyph and lines are 20 px tall.

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

css::ComputedStyle const* style_of(Page const& page, std::string_view id)
{
    dom::Element const* element = find_element(*page.document, id);
    if (!element)
        return nullptr;
    auto const it = page.styles.find(element);
    return it == page.styles.end() ? nullptr : &it->second;
}

constexpr std::string_view head = R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
</style></head><body>)HTML";

std::string page_with(std::string_view body)
{
    return std::string(head) + std::string(body) + "</body></html>";
}

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.01f;
}

css::GridLine line(int number, std::string name = "")
{
    css::GridLine out;
    out.kind = css::GridLine::Kind::Line;
    out.number = number;
    out.name = std::move(name);
    return out;
}

css::GridLine named(std::string name)
{
    css::GridLine out;
    out.kind = css::GridLine::Kind::Name;
    out.name = std::move(name);
    return out;
}

css::GridLine span(int number, std::string name = "")
{
    css::GridLine out;
    out.kind = css::GridLine::Kind::Span;
    out.number = number;
    out.name = std::move(name);
    return out;
}

layout::grid::Track fixed(float px)
{
    layout::grid::Track track;
    track.min = { layout::grid::Breadth::Kind::Fixed, px };
    track.max = { layout::grid::Breadth::Kind::Fixed, px };
    return track;
}

layout::grid::Track flex(float factor)
{
    layout::grid::Track track;
    track.min = { layout::grid::Breadth::Kind::Auto, 0 };
    track.max = { layout::grid::Breadth::Kind::Flex, factor };
    return track;
}

layout::grid::Track auto_track()
{
    return {};
}

layout::grid::Contribution item(int start, int end, float minimum, float min_content, float max_content)
{
    return { start, end, minimum, min_content, max_content };
}

} // namespace

int main()
{
    using namespace layout::grid;
    text::FontManager::instance().set_system_fonts(false);

    // --- Lines: numbers, names, spans -----------------------------------------
    {
        AxisLines lines;
        lines.names = { { "a" }, { "b" }, { "a" }, {} }; // three tracks
        css::GridLine const automatic;
        AxisPlacement p = resolve_lines(line(2), automatic, lines);
        CHECK(p.definite());
        CHECK_EQ(p.start.value_or(-9), 1);
        CHECK_EQ(p.end.value_or(-9), 2);
        p = resolve_lines(line(-1), automatic, lines);
        CHECK_EQ(p.start.value_or(-9), 3);
        p = resolve_lines(line(2, "a"), automatic, lines);
        CHECK_EQ(p.start.value_or(-9), 2);
        p = resolve_lines(named("a"), automatic, lines);
        CHECK_EQ(p.start.value_or(-9), 0);
        p = resolve_lines(named("zz"), automatic, lines); // no such line: the first implicit one
        CHECK_EQ(p.start.value_or(-9), 4);
        p = resolve_lines(line(3, "a"), automatic, lines); // only two lines named a: implicit ones count
        CHECK_EQ(p.start.value_or(-9), 4);
        p = resolve_lines(line(1), span(2), lines);
        CHECK_EQ(p.start.value_or(-9), 0);
        CHECK_EQ(p.end.value_or(-9), 2);
        p = resolve_lines(automatic, line(3), lines);
        CHECK_EQ(p.start.value_or(-9), 1);
        CHECK_EQ(p.end.value_or(-9), 2);
        p = resolve_lines(span(2), line(3), lines);
        CHECK_EQ(p.start.value_or(-9), 0);
        p = resolve_lines(automatic, span(3), lines);
        CHECK(!p.definite());
        CHECK_EQ(p.span, 3);
        p = resolve_lines(line(3), line(1), lines); // swapped
        CHECK_EQ(p.start.value_or(-9), 0);
        CHECK_EQ(p.end.value_or(-9), 2);
        p = resolve_lines(line(2), line(2), lines); // equal: one track
        CHECK_EQ(p.start.value_or(-9), 1);
        CHECK_EQ(p.end.value_or(-9), 2);
        p = resolve_lines(span(1, "b"), line(3), lines); // back to the line named b
        CHECK_EQ(p.start.value_or(-9), 1);
        p = resolve_lines(span(2), span(4), lines); // two spans: the end's goes
        CHECK_EQ(p.span, 2);
    }

    // --- Auto-placement ------------------------------------------------------------
    {
        AxisPlacement const automatic;
        std::vector<ItemPlacement> items(3, ItemPlacement { automatic, automatic });
        PlacedGrid grid = place_items(items, 0, 2, css::GridAutoFlow::Row);
        CHECK_EQ(grid.columns, 2);
        CHECK_EQ(grid.rows, 2);
        CHECK_EQ(grid.areas[0].row_start, 0);
        CHECK_EQ(grid.areas[0].column_start, 0);
        CHECK_EQ(grid.areas[1].column_start, 1);
        CHECK_EQ(grid.areas[2].row_start, 1);
        CHECK_EQ(grid.areas[2].column_start, 0);
        // A definite item in the way: the flow goes around it.
        AxisPlacement fixed_row;
        fixed_row.start = 0;
        fixed_row.end = 1;
        AxisPlacement fixed_column;
        fixed_column.start = 1;
        fixed_column.end = 2;
        items[0] = ItemPlacement { fixed_row, fixed_column };
        grid = place_items(items, 0, 2, css::GridAutoFlow::Row);
        CHECK_EQ(grid.areas[1].row_start, 0);
        CHECK_EQ(grid.areas[1].column_start, 0);
        CHECK_EQ(grid.areas[2].row_start, 1);
        CHECK_EQ(grid.areas[2].column_start, 0);
        // A span of two in two columns after one item: sparse skips the
        // hole, dense fills it.
        AxisPlacement two;
        two.span = 2;
        items = { ItemPlacement { automatic, automatic }, ItemPlacement { automatic, two },
            ItemPlacement { automatic, automatic } };
        grid = place_items(items, 0, 2, css::GridAutoFlow::Row);
        CHECK_EQ(grid.areas[1].row_start, 1);
        CHECK_EQ(grid.areas[1].column_end, 2);
        CHECK_EQ(grid.areas[2].row_start, 2);
        grid = place_items(items, 0, 2, css::GridAutoFlow::RowDense);
        CHECK_EQ(grid.areas[2].row_start, 0);
        CHECK_EQ(grid.areas[2].column_start, 1);
        // Column flow fills down first.
        items = std::vector<ItemPlacement>(3, ItemPlacement { automatic, automatic });
        grid = place_items(items, 2, 0, css::GridAutoFlow::Column);
        CHECK_EQ(grid.areas[1].row_start, 1);
        CHECK_EQ(grid.areas[1].column_start, 0);
        CHECK_EQ(grid.areas[2].row_start, 0);
        CHECK_EQ(grid.areas[2].column_start, 1);
        // A line before the explicit grid shifts everything.
        AxisPlacement early;
        early.start = -1;
        early.end = 1;
        items = { ItemPlacement { automatic, early }, ItemPlacement { automatic, automatic } };
        grid = place_items(items, 0, 2, css::GridAutoFlow::Row);
        CHECK_EQ(grid.column_offset, 1);
        CHECK_EQ(grid.columns, 3);
        CHECK_EQ(grid.areas[0].column_start, 0);
        CHECK_EQ(grid.areas[1].column_start, 2);
    }

    // --- Track sizing ----------------------------------------------------------------
    {
        SizingInput input;
        input.tracks = { fixed(100), auto_track(), flex(1) };
        input.items = { item(1, 2, 50, 50, 80) };
        input.available = 400;
        std::vector<float> sizes = size_tracks(input);
        CHECK_EQ(sizes.size(), std::size_t(3));
        CHECK(near(sizes[0], 100));
        CHECK(near(sizes[1], 80));
        CHECK(near(sizes[2], 220));
        // Two auto tracks stretch over the free space equally.
        input.tracks = { auto_track(), auto_track() };
        input.items = { item(0, 1, 50, 50, 100), item(1, 2, 60, 60, 60) };
        input.available = 300;
        sizes = size_tracks(input);
        CHECK(near(sizes[0], 170));
        CHECK(near(sizes[1], 130));
        // Without stretching they stay at their max-content.
        input.stretch = false;
        sizes = size_tracks(input);
        CHECK(near(sizes[0], 100));
        CHECK(near(sizes[1], 60));
        // Flex factors share what the fixed tracks leave, gaps included.
        input.tracks = { flex(1), flex(2), fixed(40) };
        input.items = {};
        input.available = 400;
        input.gap = 10;
        input.stretch = true;
        sizes = size_tracks(input);
        CHECK(near(sizes[0], 340.0f / 3));
        CHECK(near(sizes[1], 680.0f / 3));
        // A flex track never shrinks under its content's minimum.
        input.tracks = { flex(1), flex(1) };
        input.items = { item(0, 1, 300, 300, 300) };
        input.available = 400;
        input.gap = 0;
        sizes = size_tracks(input);
        CHECK(near(sizes[0], 300));
        CHECK(near(sizes[1], 100));
        // An item spanning two auto tracks grows them equally.
        input.tracks = { auto_track(), auto_track() };
        input.items = { item(0, 2, 200, 200, 200) };
        input.available = 500;
        input.stretch = false;
        sizes = size_tracks(input);
        CHECK(near(sizes[0], 100));
        CHECK(near(sizes[1], 100));
        // minmax(50px, 150px): the content grows it to its maximum, no further.
        {
            Track track;
            track.min = { Breadth::Kind::Fixed, 50 };
            track.max = { Breadth::Kind::Fixed, 150 };
            input.tracks = { track };
            input.items = { item(0, 1, 400, 400, 400) };
            input.available = 500;
            sizes = size_tracks(input);
            CHECK(near(sizes[0], 150));
        }
        // Indefinite space: tracks take their max-content, fr tracks their
        // content's size.
        input.tracks = { auto_track(), flex(1) };
        input.items = { item(0, 1, 30, 30, 90), item(1, 2, 20, 20, 70) };
        input.available = std::nullopt;
        sizes = size_tracks(input);
        CHECK(near(sizes[0], 90));
        CHECK(near(sizes[1], 70));
        input.constraint = Constraint::MinContent;
        sizes = size_tracks(input);
        CHECK(near(sizes[0], 30));
        CHECK(near(sizes[1], 20));
        // A collapsed track is nothing, and its gutter goes too.
        input.constraint = Constraint::Definite;
        input.tracks = { fixed(100), fixed(100), fixed(100) };
        input.tracks[1].collapsed = true;
        input.items = {};
        input.available = 300;
        input.gap = 10;
        sizes = size_tracks(input);
        CHECK(near(sizes[1], 0));
        TrackPositions positions = distribute_tracks(sizes, { false, true, false }, 10, 300, Distribution::Start);
        CHECK(near(positions.offsets[2], 110));
        CHECK(near(positions.extent, 210));
    }

    // --- Repetitions and distribution -------------------------------------------
    {
        CHECK_EQ(repetitions_that_fit(350, false, 0, 0, 100, 1, 0), 3);
        CHECK_EQ(repetitions_that_fit(350, false, 0, 0, 100, 1, 10), 3);
        CHECK_EQ(repetitions_that_fit(50, false, 0, 0, 100, 1, 0), 1);
        CHECK_EQ(repetitions_that_fit(250, true, 0, 0, 100, 1, 0), 3);
        CHECK_EQ(repetitions_that_fit(400, false, 100, 1, 100, 1, 0), 3);
        TrackPositions positions = distribute_tracks({ 100, 100 }, {}, 0, 300, Distribution::SpaceBetween);
        CHECK(near(positions.offsets[0], 0));
        CHECK(near(positions.offsets[1], 200));
        CHECK(near(positions.extent, 300));
        positions = distribute_tracks({ 100, 100 }, {}, 0, 300, Distribution::Center);
        CHECK(near(positions.offsets[0], 50));
        CHECK(near(positions.offsets[1], 150));
        positions = distribute_tracks({ 100, 100 }, {}, 0, 300, Distribution::SpaceEvenly);
        CHECK(near(positions.offsets[0], 100.0f / 3));
        positions = distribute_tracks({ 100, 100 }, {}, 20, 300, Distribution::End);
        CHECK(near(positions.offsets[0], 80));
        CHECK(near(positions.offsets[1], 200));
    }

    // --- The parsers, through the cascade ------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="t" style="grid-template-columns: [a] 100px [b c] repeat(2, [d] 50px) [e]; grid-template-rows: repeat(auto-fill, minmax(40px, 1fr)); grid-template-areas: 'x x' 'y .'; grid-auto-flow: column dense; grid-auto-rows: 30px 60px; gap: 5px 10%"></div>
<div id="s" style="grid: auto-flow dense 40px / 100px 1fr"></div>
<div id="p" style="grid-row: 2 / span 3; grid-column: foo; grid-area: a / b"></div>
<div id="q" style="grid-area: 1 / 2 / 3 / 4; place-self: center end"></div>
<div id="bad" style="grid-template-columns: 100px; grid-template-columns: [a]; grid-template-areas: 'x y' 'y x'; grid-row: 0"></div>)"));
        css::ComputedStyle const* t = style_of(page, "t");
        if (CHECK(t && t->grid_template_columns)) {
            css::GridTrackList const& columns = *t->grid_template_columns;
            CHECK_EQ(columns.tracks.size(), std::size_t(3));
            CHECK_EQ(columns.tracks[0].names, std::vector<std::string> { "a" });
            CHECK_EQ(columns.tracks[1].names, (std::vector<std::string> { "b", "c", "d" }));
            CHECK_EQ(columns.tracks[2].names, std::vector<std::string> { "d" });
            CHECK_EQ(columns.trailing_names, std::vector<std::string> { "e" });
            CHECK(columns.tracks[1].size.min.kind == css::TrackBreadth::Kind::Length);
            CHECK(near(columns.tracks[1].size.min.length.value, 50));
        }
        if (CHECK(t && t->grid_template_rows)) {
            css::GridTrackList const& rows = *t->grid_template_rows;
            CHECK(rows.tracks.empty());
            CHECK(rows.auto_repeat == css::GridTrackList::AutoRepeat::Fill);
            CHECK_EQ(rows.auto_repeat_tracks.size(), std::size_t(1));
            CHECK(rows.auto_repeat_tracks[0].size.max.kind == css::TrackBreadth::Kind::Flex);
        }
        if (CHECK(t && t->grid_template_areas)) {
            css::GridAreas const& areas = *t->grid_template_areas;
            CHECK_EQ(areas.rows, 2);
            CHECK_EQ(areas.columns, 2);
            CHECK_EQ(areas.areas.size(), std::size_t(2));
            CHECK_EQ(areas.areas[0].name, "x");
            CHECK_EQ(areas.areas[0].column_end, 3);
            CHECK_EQ(areas.areas[1].name, "y");
            CHECK_EQ(areas.areas[1].row_start, 2);
            CHECK_EQ(areas.areas[1].column_end, 2);
        }
        if (CHECK(t)) {
            CHECK(t->grid_auto_flow == css::GridAutoFlow::ColumnDense);
            CHECK(t->grid_auto_rows && t->grid_auto_rows->size() == 2);
            CHECK(near(t->row_gap.value, 5));
            CHECK(t->column_gap.kind == css::LengthPercent::Kind::Percent);
            CHECK(near(t->column_gap.value, 10));
        }
        css::ComputedStyle const* s = style_of(page, "s");
        if (CHECK(s)) {
            CHECK(s->grid_auto_flow == css::GridAutoFlow::RowDense);
            CHECK(s->grid_auto_rows && s->grid_auto_rows->size() == 1);
            CHECK(!s->grid_template_rows);
            CHECK(s->grid_template_columns && s->grid_template_columns->tracks.size() == 2);
        }
        css::ComputedStyle const* p = style_of(page, "p");
        if (CHECK(p)) {
            // grid-area came last: a / b = row a, column b, then the
            // -end sides copy the names.
            CHECK(p->grid_row_start.kind == css::GridLine::Kind::Name);
            CHECK_EQ(p->grid_row_start.name, "a");
            CHECK_EQ(p->grid_row_end.name, "a");
            CHECK_EQ(p->grid_column_start.name, "b");
            CHECK_EQ(p->grid_column_end.name, "b");
        }
        css::ComputedStyle const* q = style_of(page, "q");
        if (CHECK(q)) {
            CHECK(q->grid_row_start.kind == css::GridLine::Kind::Line);
            CHECK_EQ(q->grid_row_start.number, 1);
            CHECK_EQ(q->grid_column_start.number, 2);
            CHECK_EQ(q->grid_row_end.number, 3);
            CHECK_EQ(q->grid_column_end.number, 4);
            CHECK(q->align_self == css::AlignItems::Center);
            CHECK(q->justify_self == css::AlignItems::FlexEnd);
        }
        css::ComputedStyle const* bad = style_of(page, "bad");
        if (CHECK(bad)) {
            // The bad declarations are dropped: names alone, a non-rectangular
            // area, line zero.
            CHECK(bad->grid_template_columns && bad->grid_template_columns->tracks.size() == 1);
            CHECK(!bad->grid_template_areas);
            CHECK(bad->grid_row_start.is_auto());
        }
    }

    // --- Explicit columns, an fr, gaps, auto rows -----------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:300px; grid-template-columns: 100px 1fr; column-gap: 20px; row-gap: 10px"><div id="a">aa</div><div id="b">bb</div><div id="c">cc</div></div>)"));
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* c = find_box(page.result.root, "c");
        if (CHECK(g && a && b && c)) {
            CHECK(near(a->x, 0) && near(a->y, 0) && near(a->width, 100) && near(a->height, 20));
            CHECK(near(b->x, 120) && near(b->y, 0) && near(b->width, 180) && near(b->height, 20));
            CHECK(near(c->x, 0) && near(c->y, 30) && near(c->width, 100));
            CHECK(near(g->height, 50));
        }
    }

    // --- Spans, definite lines and auto-placement around them --------------------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:300px; grid-template-columns: repeat(3, 100px); grid-template-rows: 50px 50px"><div id="p" style="grid-column: 1 / 3; grid-row: 2">p</div><div id="q" style="grid-column: 3; grid-row: 1 / span 2">q</div><div id="r">r</div></div>)"));
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::Fragment const* p = find_box(page.result.root, "p");
        layout::Fragment const* q = find_box(page.result.root, "q");
        layout::Fragment const* r = find_box(page.result.root, "r");
        if (CHECK(g && p && q && r)) {
            CHECK(near(p->x, 0) && near(p->y, 50) && near(p->width, 200) && near(p->height, 50));
            CHECK(near(q->x, 200) && near(q->y, 0) && near(q->width, 100) && near(q->height, 100));
            CHECK(near(r->x, 0) && near(r->y, 0) && near(r->width, 100) && near(r->height, 50));
            CHECK(near(g->height, 100));
        }
    }

    // --- Areas and the names they give the lines --------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:300px; grid-template-areas: 'head head' 'nav main'; grid-template-columns: 100px 200px; grid-template-rows: 30px 70px"><div id="main" style="grid-area: main">m</div><div id="head" style="grid-area: head">h</div><div id="nav" style="grid-row: nav-start / nav-end; grid-column: nav">n</div></div>)"));
        layout::Fragment const* head_box = find_box(page.result.root, "head");
        layout::Fragment const* nav = find_box(page.result.root, "nav");
        layout::Fragment const* main_box = find_box(page.result.root, "main");
        if (CHECK(head_box && nav && main_box)) {
            CHECK(near(head_box->x, 0) && near(head_box->y, 0) && near(head_box->width, 300) && near(head_box->height, 30));
            CHECK(near(nav->x, 0) && near(nav->y, 30) && near(nav->width, 100) && near(nav->height, 70));
            CHECK(near(main_box->x, 100) && near(main_box->y, 30) && near(main_box->width, 200) && near(main_box->height, 70));
        }
    }

    // --- auto-fill and space-between ------------------------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:350px; grid-template-columns: repeat(auto-fill, 100px); justify-content: space-between"><div id="d1">1</div><div id="d2">2</div><div id="d3">3</div><div id="d4">4</div></div>)"));
        layout::Fragment const* d1 = find_box(page.result.root, "d1");
        layout::Fragment const* d2 = find_box(page.result.root, "d2");
        layout::Fragment const* d3 = find_box(page.result.root, "d3");
        layout::Fragment const* d4 = find_box(page.result.root, "d4");
        if (CHECK(d1 && d2 && d3 && d4)) {
            CHECK(near(d1->x, 0) && near(d2->x, 125) && near(d3->x, 250));
            CHECK(near(d4->x, 0) && near(d4->y, 20));
        }
    }

    // --- auto-fit collapses the empty repetitions --------------------------------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:350px; grid-template-columns: repeat(auto-fit, 100px); column-gap: 10px; justify-content: center"><div id="d1">1</div><div id="d2">2</div></div>)"));
        layout::Fragment const* d1 = find_box(page.result.root, "d1");
        layout::Fragment const* d2 = find_box(page.result.root, "d2");
        if (CHECK(d1 && d2)) {
            // Two tracks and one gutter: 210 wide, centered in 350.
            CHECK(near(d1->x, 70) && near(d2->x, 180));
        }
    }

    // --- Alignment in the area: auto margins, align-items, a stretched row -------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:300px; height:100px; grid-template-columns: 1fr; align-items: center"><div id="e" style="width: 50px; margin: 0 auto">e</div></div>
<div id="h" style="display:grid; width:300px; height:100px; justify-items: end; align-items: end"><div id="f" style="width: 50px">f</div></div>)"));
        layout::Fragment const* e = find_box(page.result.root, "e");
        layout::Fragment const* f = find_box(page.result.root, "f");
        if (CHECK(e && f)) {
            CHECK(near(e->x, 125) && near(e->y, 40) && near(e->width, 50) && near(e->height, 20));
            // The second grid sits under the first one's 100 px.
            CHECK(near(f->x, 250) && near(f->y, 180));
        }
    }

    // --- Auto columns from content; an inline-grid shrinks to fit ----------------
    {
        Page const page = lay_out(page_with(R"(<span id="ig" style="display:inline-grid; grid-template-columns: auto auto; column-gap: 10px"><span id="f1">abc</span><span id="f2">de</span></span>)"));
        layout::Fragment const* ig = find_box(page.result.root, "ig");
        layout::Fragment const* f1 = find_box(page.result.root, "f1");
        layout::Fragment const* f2 = find_box(page.result.root, "f2");
        if (CHECK(ig && f1 && f2)) {
            CHECK(near(ig->width, 60) && near(ig->height, 20));
            CHECK(near(f1->x, 0) && near(f1->width, 30));
            CHECK(near(f2->x, 40) && near(f2->width, 20));
        }
    }

    // --- The automatic minimum: a narrow grid cannot squeeze a word ---------------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:10px"><div id="w">ab cd</div></div>)"));
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::Fragment const* w = find_box(page.result.root, "w");
        if (CHECK(g && w)) {
            CHECK(near(w->width, 20) && near(w->height, 40));
            CHECK(near(g->width, 10) && near(g->height, 40));
        }
    }

    // --- Percentages of the area: padding, and a gap of the container ----------
    {
        Page const page = lay_out(page_with(R"(<div style="display:grid; width:300px; grid-template-columns: 100px 100px; column-gap: 10%"><div id="pp" style="padding-left: 10%">x</div><div id="pq">y</div></div>)"));
        layout::Fragment const* pp = find_box(page.result.root, "pp");
        layout::Fragment const* pq = find_box(page.result.root, "pq");
        layout::TextRun const* x = find_run(page.result.root, U"x");
        if (CHECK(pp && pq && x)) {
            CHECK(near(pp->width, 100));
            CHECK(near(x->x, 10));
            CHECK(near(pq->x, 130));
        }
    }

    // --- order, z-index, anonymous text, generated boxes ----------------------------
    {
        Page const page = lay_out(page_with(R"(<style>#gen::before { content: "B"; grid-column: 2 }</style><div id="g" style="display:grid; width:200px; grid-template-columns: 100px 100px"><div id="o1" style="order: 2; z-index: 3">1</div><div id="o2">2</div>loose</div>
<div id="gen" style="display:grid; width:200px; grid-template-columns: 100px 100px"><div id="k">k</div></div>)"));
        layout::Fragment const* o1 = find_box(page.result.root, "o1");
        layout::Fragment const* o2 = find_box(page.result.root, "o2");
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::TextRun const* loose = find_run(page.result.root, U"loose");
        layout::TextRun const* generated = find_run(page.result.root, U"B");
        layout::Fragment const* k = find_box(page.result.root, "k");
        if (CHECK(o1 && o2 && g && loose && generated && k)) {
            // The container's own text is an item of its own, at order 0:
            // it takes the second cell, and the order-2 item the third.
            CHECK(near(o2->x, 0) && near(o2->y, 0));
            CHECK(near(loose->x, 100));
            CHECK(near(o1->x, 0) && near(o1->y, 20));
            CHECK(o1->positioned && o1->stacking_context && o1->z_index == 3);
            CHECK(!o2->positioned);
            CHECK(near(g->height, 40));
            // The ::before box is an item placed by its own properties.
            CHECK(near(generated->x, 100));
            CHECK(near(k->x, 0));
        }
    }

    // --- Implicit tracks from grid-auto-rows, and a column flow --------------------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:200px; grid-template-columns: 100px 100px; grid-auto-rows: 30px 60px"><div id="a">a</div><div id="b">b</div><div id="c">c</div><div id="d">d</div><div id="e">e</div></div>
<div id="h" style="display:grid; width:200px; height: 60px; grid-auto-flow: column; grid-template-rows: 30px 30px; grid-auto-columns: 50px"><div id="p">p</div><div id="q">q</div><div id="r">r</div></div>)"));
        layout::Fragment const* g = find_box(page.result.root, "g");
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* e = find_box(page.result.root, "e");
        layout::Fragment const* q = find_box(page.result.root, "q");
        layout::Fragment const* r = find_box(page.result.root, "r");
        if (CHECK(g && c && e && q && r)) {
            CHECK(near(c->y, 30) && near(c->height, 60));
            CHECK(near(e->y, 90) && near(e->height, 30));
            CHECK(near(g->height, 120));
            // The second grid starts under the first's 120 px.
            CHECK(near(q->x, 0) && near(q->y, 150));
            CHECK(near(r->x, 50) && near(r->y, 120) && near(r->width, 50));
        }
    }

    // --- A grid container's own height stretches its rows; align-content ---------
    {
        Page const page = lay_out(page_with(R"(<div id="g" style="display:grid; width:200px; height:200px; grid-template-rows: 50px 50px; align-content: space-between"><div id="a">a</div><div id="b">b</div></div>
<div id="h" style="display:grid; width:200px; height:100px"><div id="c">c</div><div id="d">d</div></div>)"));
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* d = find_box(page.result.root, "d");
        if (CHECK(a && b && c && d)) {
            CHECK(near(a->y, 0) && near(b->y, 150));
            // Two auto rows share the 100 px: 50 each, the items stretched
            // (this grid starts under the first's 200 px).
            CHECK(near(c->height, 50) && near(d->y, 250) && near(d->height, 50));
        }
    }

    // --- Absolutely positioned children in their grid areas ------------------------------
    {
        // The container is the containing block, so each child's placement
        // names the area it is laid out in: with auto offsets it stands at
        // that area's corner, and an offset counts from the area's edge.
        Page const page = lay_out(page_with(R"HTML(<div id="g" style="display: grid; grid-template-columns: 200px 300px; grid-template-rows: 100px 150px; position: relative; width: 500px; height: 250px; border: 10px solid #000; padding: 5px">
<div id="a" style="position: absolute; grid-column: 1; grid-row: 1">A</div>
<div id="b" style="position: absolute; grid-column: 2; grid-row: 1">B</div>
<div id="c" style="position: absolute; grid-column: 1 / 2; grid-row: 2 / 3; left: 0; right: 0; top: 0; bottom: 0"></div>
<div id="d" style="position: absolute; grid-column: 2 / 3; grid-row: 2; left: 7px; top: 9px">D</div>
<div id="e" style="position: absolute">E</div>
<div id="f" style="position: absolute; grid-column: 2 / 3; left: 0; right: 0"></div>
</div>)HTML"));
        // The content box starts at 15 (10 of border, 5 of padding); the
        // padding box at 10.
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        layout::Fragment const* c = find_box(page.result.root, "c");
        layout::Fragment const* d = find_box(page.result.root, "d");
        layout::Fragment const* e = find_box(page.result.root, "e");
        layout::Fragment const* f = find_box(page.result.root, "f");
        if (CHECK(a && b && c && d && e && f)) {
            CHECK(near(a->x, 15) && near(a->y, 15));
            CHECK(near(b->x, 215) && near(b->y, 15));
            // Stretched between four offsets: the whole of its area.
            CHECK(near(c->x, 15) && near(c->y, 115));
            CHECK(near(c->width, 200) && near(c->height, 150));
            // The offsets count from the area's edges, not the padding box's.
            CHECK(near(d->x, 222) && near(d->y, 124));
            // No placement at all: it stands where it would have, in the
            // padding box.
            CHECK(near(e->x, 15) && near(e->y, 15));
            // One axis placed and the other left alone: the column is the
            // area's, and with no row named the box keeps the static
            // position a sole grid item would have had, at the content edge.
            CHECK(near(f->x, 215) && near(f->width, 300));
            CHECK(near(f->y, 15));
        }
    }
    {
        // A line the grid does not have counts as auto, so that edge stays
        // on the padding box; a container that is not a containing block
        // gives no areas at all.
        Page const page = lay_out(page_with(R"HTML(<div id="g" style="display: grid; grid-template-columns: 100px 100px; grid-template-rows: 50px; position: relative; width: 200px; height: 50px">
<div id="a" style="position: absolute; grid-column: 7 / 9; grid-row: 1; left: 0; right: 0"></div>
</div>
<div id="h" style="display: grid; grid-template-columns: 100px 100px; width: 200px; height: 50px">
<div id="b" style="position: absolute; grid-column: 2; grid-row: 1">B</div>
</div>)HTML"));
        layout::Fragment const* a = find_box(page.result.root, "a");
        layout::Fragment const* b = find_box(page.result.root, "b");
        if (CHECK(a && b)) {
            // Lines 7 and 9 are past the grid: the whole padding box.
            CHECK(near(a->x, 0) && near(a->width, 200));
            // The second grid contains nothing: b answers to the page.
            CHECK(near(b->x, 0));
        }
    }

    return sashfold::test::report("grid");
}
