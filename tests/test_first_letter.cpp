#include "Test.h"

#include "core/Unicode.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "text/FontManager.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ::first-letter — the punctuation categories the run keeps, and the runs a
// page comes out with: what wears the pseudo-element's style and what is
// left behind. Sashfold Mono at 16px advances 10 px per glyph.

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

// The green a test page asks for, as the resolver computes it.
bool is_green(Color const& color)
{
    return color.r == 0 && color.g == 128 && color.b == 0 && color.a == 255;
}

}

int main()
{
    text::FontManager::instance().set_system_fonts(false);

    {
        // The categories CSS 2.1 names: opening and closing pairs (Ps, Pe),
        // the quotation marks that face in and out (Pi, Pf), and everything
        // else Unicode calls punctuation (Po). Checked against the general
        // categories in UnicodeData 16.
        CHECK(is_first_letter_punctuation(U'(')); // Ps
        CHECK(is_first_letter_punctuation(U')')); // Pe
        CHECK(is_first_letter_punctuation(U'[')); // Ps
        CHECK(is_first_letter_punctuation(U']')); // Pe
        CHECK(is_first_letter_punctuation(U'{')); // Ps
        CHECK(is_first_letter_punctuation(U'}')); // Pe
        CHECK(is_first_letter_punctuation(U'"')); // Po
        CHECK(is_first_letter_punctuation(U'\'')); // Po
        CHECK(is_first_letter_punctuation(U'.')); // Po
        CHECK(is_first_letter_punctuation(U',')); // Po
        CHECK(is_first_letter_punctuation(U'!')); // Po
        CHECK(is_first_letter_punctuation(U'\\')); // Po
        CHECK(is_first_letter_punctuation(U'¡')); // Po, inverted exclamation
        CHECK(is_first_letter_punctuation(U'‘')); // Pi, left single quotation mark
        CHECK(is_first_letter_punctuation(U'’')); // Pf, right single quotation mark
        CHECK(is_first_letter_punctuation(U'“')); // Pi, left double quotation mark
        CHECK(is_first_letter_punctuation(U'”')); // Pf, right double quotation mark
        CHECK(is_first_letter_punctuation(U'،')); // Po, Arabic comma
        CHECK(is_first_letter_punctuation(U'᠃')); // Po, Mongolian full stop
        CHECK(is_first_letter_punctuation(U'︶')); // Pe, vertical right parenthesis
        CHECK(is_first_letter_punctuation(U'﹊')); // Po, centreline overline
        CHECK(is_first_letter_punctuation(U'\U00010A52')); // Po, Kharoshthi punctuation circle

        // Not punctuation of these classes: letters, digits, the space, the
        // dashes (Pd) and the connectors (Pc), the maths symbols (Sm), and
        // the combining marks a letter carries.
        CHECK(!is_first_letter_punctuation(U'A'));
        CHECK(!is_first_letter_punctuation(U'z'));
        CHECK(!is_first_letter_punctuation(U'7'));
        CHECK(!is_first_letter_punctuation(U' '));
        CHECK(!is_first_letter_punctuation(U'-')); // Pd
        CHECK(!is_first_letter_punctuation(U'_')); // Pc
        CHECK(!is_first_letter_punctuation(U'+')); // Sm
        CHECK(!is_first_letter_punctuation(U'<')); // Sm
        CHECK(!is_first_letter_punctuation(U'$')); // Sc
        CHECK(!is_first_letter_punctuation(U'́')); // Mn, combining acute
        CHECK(!is_first_letter_punctuation(0));
        CHECK(!is_first_letter_punctuation(0x10FFFF));
    }

    {
        // The plain case: one letter leaves the word, the rest stays, and
        // only the letter wears what the rule asked for.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p { color: black } p::first-letter { color: green }
        </style><p>Text</p>)HTML");
        layout::TextRun const* letter = find_run(page, U"T");
        layout::TextRun const* rest = find_run(page, U"ext");
        CHECK(letter && rest);
        CHECK(is_green(letter->style->color));
        CHECK(!is_green(rest->style->color));
        // The two runs sit side by side on one line, in order.
        CHECK_EQ(letter->x, 8);
        CHECK_EQ(rest->x, 18);
        CHECK_EQ(letter->baseline_y, rest->baseline_y);
    }

    {
        // The one-colon spelling CSS 2.1 wrote is the same pseudo-element.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p:first-letter { color: green }
        </style><p>Text</p>)HTML");
        layout::TextRun const* letter = find_run(page, U"T");
        CHECK(letter && is_green(letter->style->color));
    }

    {
        // Punctuation on either side of the letter comes with it, and only
        // as far as the letter reaches: the "e" after it stays behind.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p::first-letter { color: green }
        </style><p>)T)est</p>)HTML");
        layout::TextRun const* letter = find_run(page, U")T)");
        layout::TextRun const* rest = find_run(page, U"est");
        CHECK(letter && rest);
        CHECK(is_green(letter->style->color));
        CHECK(!is_green(rest->style->color));
    }

    {
        // Punctuation in front but none behind: the run ends at the letter.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p::first-letter { color: green }
        </style><p>"This is text"</p>)HTML");
        layout::TextRun const* letter = find_run(page, U"\"T");
        CHECK(letter && is_green(letter->style->color));
        CHECK(find_run(page, U"his") != nullptr);
    }

    {
        // A letter with the marks that hang off it keeps them.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p::first-letter { color: green }
        </style><p>e&#x0301;tude</p>)HTML");
        layout::TextRun const* letter = find_run(page, U"é");
        CHECK(letter && is_green(letter->style->color));
        CHECK(find_run(page, U"tude") != nullptr);
    }

    {
        // The letter may sit inside an inline box: the first letter of the
        // block's first line is what counts, wherever it was written. The
        // rest of the span keeps the span's own colour.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            div::first-letter { color: green } span { color: blue }
        </style><div><span>Text</span></div>)HTML");
        layout::TextRun const* letter = find_run(page, U"T");
        layout::TextRun const* rest = find_run(page, U"ext");
        CHECK(letter && rest);
        CHECK(is_green(letter->style->color));
        CHECK(rest->style->color.b == 255);
    }

    {
        // A word of nothing but punctuation leads into the letter the next
        // word holds — an opening quotation mark written in its own element,
        // say — and the whole run wears the style.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            div::first-letter { color: green }
        </style><div><i>"</i><span>This is text</span></div>)HTML");
        layout::TextRun const* quote = find_run(page, U"\"");
        layout::TextRun const* letter = find_run(page, U"T");
        CHECK(quote && letter);
        CHECK(is_green(quote->style->color));
        CHECK(is_green(letter->style->color));
        CHECK(find_run(page, U"his") != nullptr);
    }

    {
        // Leading spaces are stepped over: they put nothing on a line.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p::first-letter { color: green }
        </style><p>   Text</p>)HTML");
        layout::TextRun const* letter = find_run(page, U"T");
        CHECK(letter && is_green(letter->style->color));
    }

    {
        // A bigger first letter is bigger: it measures through its own font
        // size, and the rest of the line through the block's.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p { font-size: 16px } p::first-letter { font-size: 32px }
        </style><p>Text</p>)HTML");
        layout::TextRun const* letter = find_run(page, U"T");
        layout::TextRun const* rest = find_run(page, U"ext");
        CHECK(letter && rest);
        CHECK_EQ(letter->style->font_size, 32);
        CHECK_EQ(letter->width, 20);
        CHECK_EQ(rest->style->font_size, 16);
        CHECK_EQ(rest->x, 28);
    }

    {
        // Only the first line of the block: a second paragraph's first
        // letter is not this one's.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            div::first-letter { color: green }
        </style><div>One<br>Two</div>)HTML");
        CHECK(is_green(find_run(page, U"O")->style->color));
        CHECK(!is_green(find_run(page, U"Two")->style->color));
    }

    {
        // When the block holds boxes rather than text, the first line is
        // inside the first of them and the style is handed down to it.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            #outer::first-letter { color: green }
        </style><div id=outer><div>Adivtest</div></div>)HTML");
        layout::TextRun const* letter = find_run(page, U"A");
        CHECK(letter && is_green(letter->style->color));
        CHECK(!is_green(find_run(page, U"divtest")->style->color));
    }

    {
        // It is handed no further than the box whose own content starts the
        // line: text of the block's own before the first box keeps it.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            #outer::first-letter { color: green }
        </style><div id=outer>Ahead<div>Below</div></div>)HTML");
        CHECK(is_green(find_run(page, U"A")->style->color));
        CHECK(!is_green(find_run(page, U"Below")->style->color));
    }

    {
        // A box of its own beats what its ancestor asked for.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            #outer::first-letter { color: green } #inner::first-letter { color: blue }
        </style><div id=outer><div id=inner>Text</div></div>)HTML");
        layout::TextRun const* letter = find_run(page, U"T");
        CHECK(letter && letter->style->color.b == 255);
    }

    {
        // Nothing addresses ::first-letter: the word stays whole, in one run.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><p>Text</p>)HTML");
        CHECK(find_run(page, U"Text") != nullptr);
        CHECK(find_run(page, U"T") == nullptr);
    }

    {
        // A block whose line starts with something that is not a word has no
        // first letter to dress: the picture is not one, and the text behind
        // it does not become one either.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p::first-letter { color: green }
        </style><p><img src=x.png width=20 height=20>Text</p>)HTML");
        CHECK(find_run(page, U"Text") != nullptr);
        CHECK(find_run(page, U"T") == nullptr);
    }

    {
        // A word of nothing but punctuation, with no letter behind it, is
        // left alone.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            p::first-letter { color: green }
        </style><p>"" ""</p>)HTML");
        layout::TextRun const* quotes = find_run(page, U"\"\"");
        CHECK(quotes && !is_green(quotes->style->color));
    }

    {
        // The categories a first letter steps over rather than selects: the
        // spaces of every width, the separators, the controls and the
        // formatting characters.
        CHECK(is_first_letter_skipped(U' ')); // Zs
        CHECK(is_first_letter_skipped(U' ')); // Zs, no-break space
        CHECK(is_first_letter_skipped(U' ')); // Zs, en space
        CHECK(is_first_letter_skipped(U' ')); // Zs, em space
        CHECK(is_first_letter_skipped(U' ')); // Zs, thin space
        CHECK(is_first_letter_skipped(U'　')); // Zs, ideographic space
        CHECK(is_first_letter_skipped(U' ')); // Zl
        CHECK(is_first_letter_skipped(U' ')); // Zp
        CHECK(is_first_letter_skipped(U'\t')); // Cc
        CHECK(is_first_letter_skipped(U'­')); // Cf, soft hyphen
        CHECK(is_first_letter_skipped(U'﻿')); // Cf, byte order mark
        CHECK(!is_first_letter_skipped(U'A'));
        CHECK(!is_first_letter_skipped(U'.'));
        CHECK(!is_first_letter_skipped(U'¡'));
    }

    {
        // A space the line kept — a no-break space, an en, em or thin one —
        // is not selected, and the letter behind it is not dressed either.
        for (std::u32string_view const space : { U" ", U" ", U" ", U" " }) {
            std::string html = R"HTML(<!DOCTYPE html><style>
                p::first-letter { color: green; font-size: 32px }
            </style><p>)HTML";
            for (char32_t const c : space)
                append_utf8(html, c);
            html += "A word</p>";
            Page const page = lay_out(html);
            layout::TextRun const* first = find_run(page, std::u32string(space) + U"A");
            CHECK(first != nullptr);
            CHECK(first && !is_green(first->style->color));
            CHECK(first && first->style->font_size == 16);
        }
    }

    {
        // ::first-letter addresses a block container's first line, so a flex
        // or grid container is passed by — it holds items, not lines.
        for (std::string_view const display : { "grid", "flex" }) {
            std::string html = R"HTML(<!DOCTYPE html><style>
                #box { display: )HTML";
            html += display;
            html += R"HTML(; color: black }
                #box::first-letter { color: green }
            </style><div id=box><div>Text</div></div>)HTML";
            Page const page = lay_out(html);
            CHECK(find_run(page, U"Text") != nullptr);
            CHECK(find_run(page, U"T") == nullptr);
        }
    }

    {
        // The fictional start tag goes inside the inline boxes around the
        // letter: an inherited property the rules did not set comes from the
        // box the letter was written in, not from the block. Here the size
        // is the pseudo-element's and the colour the span's.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            div { color: red } div::first-letter { font-size: 32px }
            span { color: green }
        </style><div><span>Text</span></div>)HTML");
        layout::TextRun const* letter = find_run(page, U"T");
        CHECK(letter);
        CHECK(letter && letter->style->font_size == 32);
        CHECK(letter && is_green(letter->style->color));
    }

    {
        // And a property the rules did set wins over the box's, in the run
        // that leads into the letter as well as on the letter itself.
        Page const page = lay_out(R"HTML(<!DOCTYPE html><style>
            div { color: red } div::first-letter { color: green }
            i { color: blue } span { color: blue }
        </style><div><i>"</i><span>Text</span></div>)HTML");
        CHECK(is_green(find_run(page, U"\"")->style->color));
        CHECK(is_green(find_run(page, U"T")->style->color));
        CHECK(find_run(page, U"ext")->style->color.b == 255);
    }

    return sashfold::test::report("first-letter");
}
