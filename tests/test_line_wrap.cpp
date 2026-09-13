#include "Test.h"

#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "text/FontManager.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Where lines end: the soft wrap opportunities UAX #14 finds inside a
// word — after a hyphen, after a slash, between ideographs — and the ones
// it withholds, so that a word cut by an inline box moves to the next line
// whole; the characters that only steer the breaker; the spaces that
// hang past a line's end; and the tailorings word-break and line-break
// ask for. Sashfold Mono at 16px advances 10 px per glyph, a glyph it
// lacks included, so every width below is a count of characters.

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

void collect(layout::Fragment const& fragment, std::vector<layout::TextRun const*>& runs)
{
    for (layout::TextRun const& run : fragment.runs)
        runs.push_back(&run);
    for (layout::Fragment const& child : fragment.children)
        collect(child, runs);
}

std::vector<layout::TextRun const*> runs_of(Page const& page)
{
    std::vector<layout::TextRun const*> runs;
    collect(page.result.root, runs);
    return runs;
}

layout::TextRun const* find_run(Page const& page, std::u32string_view text)
{
    for (layout::TextRun const* run : runs_of(page)) {
        if (run->text == text)
            return run;
    }
    return nullptr;
}

// How many lines the page's text takes: one per distinct baseline.
std::size_t lines_of(Page const& page)
{
    std::set<float> baselines;
    for (layout::TextRun const* run : runs_of(page))
        baselines.insert(run->baseline_y);
    return baselines.size();
}

bool same_line(layout::TextRun const* a, layout::TextRun const* b)
{
    return a && b && a->baseline_y == b->baseline_y;
}

bool below(layout::TextRun const* lower, layout::TextRun const* upper)
{
    return lower && upper && lower->baseline_y > upper->baseline_y;
}

}

int main()
{
    text::FontManager::instance().set_system_fonts(false);

    {
        // A line may end after a hyphen and after a slash, and nowhere
        // else inside a word: "aaa-bbb" in 60 px is "aaa-" over "bbb".
        Page const page = lay_out(R"HTML(<!DOCTYPE html><div style="width: 60px">aaa-bbb</div>)HTML");
        CHECK(below(find_run(page, U"bbb"), find_run(page, U"aaa-")));
        CHECK_EQ(lines_of(page), std::size_t { 2 });
        Page const slash = lay_out(R"HTML(<!DOCTYPE html><div style="width: 60px">aaa/bbb</div>)HTML");
        CHECK(below(find_run(slash, U"bbb"), find_run(slash, U"aaa/")));
        // A period is not such a place: the word is one piece, wider than
        // the line, and it overflows — unless overflow-wrap has the line
        // slice it where it can.
        Page const period = lay_out(R"HTML(<!DOCTYPE html><div style="width: 60px">aaa.bbb</div>)HTML");
        CHECK(find_run(period, U"aaa.bbb") != nullptr);
        CHECK_EQ(lines_of(period), std::size_t { 1 });
        Page const sliced = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 60px; overflow-wrap: anywhere">aaa.bbb</div>)HTML");
        CHECK(find_run(sliced, U"aaa.bb") != nullptr);
        CHECK(below(find_run(sliced, U"b"), find_run(sliced, U"aaa.bb")));
        Page const legacy = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 60px; word-break: break-word">aaa.bbb</div>)HTML");
        CHECK(find_run(legacy, U"aaa.bb") != nullptr);
    }

    {
        // Ideographs break between any two, so a run of them wraps like
        // words: three in 25 px take two lines.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><div style="width: 25px">日本語</div>)HTML");
        CHECK_EQ(lines_of(page), std::size_t { 2 });
        CHECK(below(find_run(page, U"語"), find_run(page, U"日")));
        CHECK(same_line(find_run(page, U"日"), find_run(page, U"本")));
    }

    {
        // The edge of an inline box is no place to end a line: when what
        // is inside the box does not fit, the line ends where it last
        // could, and the word moves over whole. 120 px holds "aaaa aaaa "
        // and "un", but not "believable" behind them; the second line is
        // the whole word, and "end" takes a third.
        Page const page = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 120px">aaaa aaaa un<b>believable</b> end</div>)HTML");
        CHECK(same_line(find_run(page, U"un"), find_run(page, U"believable")));
        CHECK(below(find_run(page, U"un"), find_run(page, U"aaaa")));
        CHECK(below(find_run(page, U"end"), find_run(page, U"un")));
        CHECK_EQ(lines_of(page), std::size_t { 3 });
    }

    {
        // A zero width space is a place to end a line, and a word joiner
        // takes one away; neither is measured or drawn. 200 px holds the
        // x's, the space and "aaa-", and not "bbb" behind them.
        Page const zero_width = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 40px">aaa&#x200B;bbb</div>)HTML");
        CHECK(below(find_run(zero_width, U"bbb"), find_run(zero_width, U"aaa")));
        Page const joined = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 200px">xxxxxxxxxxxxx aaa-&#x2060;bbb</div>)HTML");
        CHECK(below(find_run(joined, U"aaa-bbb"), find_run(joined, U"xxxxxxxxxxxxx")));
        Page const unjoined = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 200px">xxxxxxxxxxxxx aaa-bbb</div>)HTML");
        CHECK(same_line(find_run(unjoined, U"aaa-"), find_run(unjoined, U"xxxxxxxxxxxxx")));
        CHECK(below(find_run(unjoined, U"bbb"), find_run(unjoined, U"aaa-")));
    }

    {
        // Under pre-wrap a preserved space at a line's end hangs past it:
        // "aaaa " fits 40 px, its space outside, and a right-aligned line
        // is aligned without it. Under break-spaces the space takes room:
        // the line is aligned with it, and where "aaaa " does not fit even
        // an empty line the space overflows rather than being sliced off
        // or moved, since no line may start with it. An ideographic space
        // hangs the same way under normal.
        Page const wrap = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 40px; white-space: pre-wrap">aaaa bbbb</div>)HTML");
        CHECK(below(find_run(wrap, U"bbbb"), find_run(wrap, U"aaaa ")));
        CHECK_EQ(lines_of(wrap), std::size_t { 2 });
        Page const wrap_right = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 60px; white-space: pre-wrap; text-align: right">aaaa bbbb</div>)HTML");
        CHECK(find_run(wrap_right, U"aaaa ") && find_run(wrap_right, U"aaaa ")->x == 28);
        Page const spaces_right = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 60px; white-space: break-spaces; text-align: right">aaaa bbbb</div>)HTML");
        CHECK(find_run(spaces_right, U"aaaa ") && find_run(spaces_right, U"aaaa ")->x == 18);
        Page const spaces = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 45px; white-space: break-spaces">aaaa bbbb</div>)HTML");
        CHECK(find_run(spaces, U"aaaa ") != nullptr);
        CHECK(below(find_run(spaces, U"bbbb"), find_run(spaces, U"aaaa ")));
        CHECK_EQ(lines_of(spaces), std::size_t { 2 });
        // With overflow-wrap, the spaces that would overflow go over to the
        // next line whole instead, a break in front of the first of them.
        Page const spaces_over = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 40px; white-space: break-spaces; overflow-wrap: break-word">XXXX　XX</div>)HTML");
        CHECK(find_run(spaces_over, U"XXXX") != nullptr);
        CHECK(same_line(find_run(spaces_over, U"　"), find_run(spaces_over, U"XX")));
        CHECK(below(find_run(spaces_over, U"　"), find_run(spaces_over, U"XXXX")));
        Page const ideographic = lay_out(R"HTML(<!DOCTYPE html><div style="width: 40px">aaaa　bbbb</div>)HTML");
        CHECK(find_run(ideographic, U"aaaa　") != nullptr);
        CHECK(below(find_run(ideographic, U"bbbb"), find_run(ideographic, U"aaaa　")));
        CHECK_EQ(lines_of(ideographic), std::size_t { 2 });
        // Before a forced break the spaces hang only when they overflow:
        // " 0 " centred in 50 px is centred whole, its "0 " at 20 px in.
        Page const centred = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: 50px; white-space: pre-wrap; text-align: center"> 0 </div>)HTML");
        CHECK(find_run(centred, U"0 ") && find_run(centred, U"0 ")->x == 28);
    }

    {
        // word-break: break-all cuts between letters, so a word's narrowest
        // line is one letter; keep-all keeps ideographs together, so
        // theirs is the run; line-break: anywhere cuts around punctuation
        // too. Each shows in a min-content box.
        Page const normal = lay_out(R"HTML(<!DOCTYPE html><div style="width: min-content">aaaa</div>)HTML");
        CHECK_EQ(lines_of(normal), std::size_t { 1 });
        Page const break_all = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: min-content; word-break: break-all">aaaa</div>)HTML");
        CHECK_EQ(lines_of(break_all), std::size_t { 4 });
        Page const ideographs = lay_out(R"HTML(<!DOCTYPE html><div style="width: min-content">日本語</div>)HTML");
        CHECK_EQ(lines_of(ideographs), std::size_t { 3 });
        Page const keep_all = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: min-content; word-break: keep-all">日本語</div>)HTML");
        CHECK_EQ(lines_of(keep_all), std::size_t { 1 });
        CHECK(find_run(keep_all, U"日本語") != nullptr);
        Page const glued = lay_out(R"HTML(<!DOCTYPE html><div style="width: min-content">a.b</div>)HTML");
        CHECK_EQ(lines_of(glued), std::size_t { 1 });
        Page const anywhere = lay_out(
            R"HTML(<!DOCTYPE html><div style="width: min-content; line-break: anywhere">a.b</div>)HTML");
        CHECK_EQ(lines_of(anywhere), std::size_t { 3 });
    }

    {
        // text-indent is part of the first line, so a box sized to its
        // content makes room for it: at max-content "12 456 89012" stays
        // on one line, indented; at min-content the first line holds the
        // indent and "12", the widest piece "89012" the third.
        Page const max = lay_out(
            R"HTML(<!DOCTYPE html><div style="text-indent: 50px; width: max-content">12 456 89012</div>)HTML");
        CHECK_EQ(lines_of(max), std::size_t { 1 });
        CHECK(find_run(max, U"12") && find_run(max, U"12")->x == 58);
        Page const min = lay_out(
            R"HTML(<!DOCTYPE html><div style="text-indent: 50px; width: min-content">12 456 89012</div>)HTML");
        CHECK_EQ(lines_of(min), std::size_t { 3 });
        CHECK(find_run(min, U"12") && find_run(min, U"12")->x == 58);
        CHECK(find_run(min, U"456") && find_run(min, U"456")->x == 8);
    }

    return sashfold::test::report("line_wrap");
}
