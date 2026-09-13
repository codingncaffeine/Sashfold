#include "Test.h"

#include "core/LineBreak.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Line breaking (UAX #14), against its own conformance data when that is
// present: LineBreakTest.txt is fetched rather than vendored
// (tools/ucd-fetch.sh); given a path to it this runs every case and holds
// the algorithm to all of them, and given none it runs the hand-written
// cases alone and says so.

using namespace sashfold;

namespace {

// The breaks of a text as "×" and "÷" between the characters, the way the
// conformance file writes them: "× a × b ÷ c ÷".
std::string picture(std::u32string const& text, std::uint8_t tailoring = 0)
{
    std::vector<std::uint8_t> const flags(text.size(), tailoring);
    std::vector<LineBreak> const breaks = line_break_opportunities(text, flags);
    std::string out;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i > 0)
            out += ' ';
        out += breaks[i] == LineBreak::None ? "\xC3\x97" : "\xC3\xB7"; // × or ÷
        if (i < text.size()) {
            out += ' ';
            char32_t const c = text[i];
            if (c == U' ')
                out += '_';
            else if (c == U'\n')
                out += "LF";
            else if (c < 0x80)
                out += static_cast<char>(c);
            else
                out += "U+" + std::to_string(static_cast<unsigned>(c));
        }
    }
    return out;
}

// One line of LineBreakTest.txt: marks and hex code points alternating,
// "×" for no break and "÷" for a break, the last mark being the end.
struct Case {
    std::u32string text;
    std::vector<bool> breaks; // one per boundary, [0] and [size] included
};

bool parse_case(std::string const& line, Case& out)
{
    std::string const body = line.substr(0, line.find('#'));
    std::istringstream stream(body);
    std::string token;
    bool expect_mark = true;
    while (stream >> token) {
        if (expect_mark) {
            if (token == "\xC3\x97")
                out.breaks.push_back(false);
            else if (token == "\xC3\xB7")
                out.breaks.push_back(true);
            else
                return false;
        } else {
            out.text.push_back(static_cast<char32_t>(std::stoul(token, nullptr, 16)));
        }
        expect_mark = !expect_mark;
    }
    return !out.text.empty() && out.breaks.size() == out.text.size() + 1;
}

void run_conformance(std::string const& path)
{
    std::ifstream file(path);
    if (!file) {
        std::cerr << "cannot read " << path << "\n";
        CHECK(false);
        return;
    }
    std::string line;
    std::size_t cases = 0;
    std::size_t failures = 0;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#')
            continue;
        Case one;
        if (!parse_case(line, one))
            continue;
        ++cases;
        std::vector<LineBreak> const got = line_break_opportunities(one.text);
        bool same = got.size() == one.breaks.size();
        for (std::size_t i = 0; same && i < got.size(); ++i)
            same = (got[i] != LineBreak::None) == one.breaks[i];
        if (same)
            continue;
        ++failures;
        if (failures <= 12)
            std::cerr << "line " << line_number << ": " << line.substr(0, line.find('#')) << "\n   got: " << picture(one.text) << "\n";
    }
    std::cout << "LineBreakTest.txt: " << (cases - failures) << " / " << cases << " cases\n";
    CHECK(cases > 7000);
    CHECK_EQ(failures, std::size_t { 0 });
}

} // namespace

int main(int argc, char** argv)
{
    // The classes, resolved as LB1 has them.
    CHECK(line_break_class(U' ') == LineBreakClass::SP);
    CHECK(line_break_class(U'a') == LineBreakClass::AL);
    CHECK(line_break_class(U'1') == LineBreakClass::NU);
    CHECK(line_break_class(U'-') == LineBreakClass::HY);
    CHECK(line_break_class(U'/') == LineBreakClass::SY);
    CHECK(line_break_class(U'\n') == LineBreakClass::LF);
    CHECK(line_break_class(U'あ') == LineBreakClass::ID); // HIRAGANA A
    CHECK(line_break_class(U'、') == LineBreakClass::CL); // IDEOGRAPHIC COMMA
    CHECK(line_break_class(U'ぁ') == LineBreakClass::CJ); // HIRAGANA SMALL A, left for the style
    CHECK(line_break_class(U'ก') == LineBreakClass::SA); // THAI KO KAI, left for the dictionary question
    CHECK(line_break_class(U'ั') == LineBreakClass::CM); // THAI MAI HAN-AKAT, SA resolved to a mark
    CHECK(line_break_class(U'\U0001F600') == LineBreakClass::ID);
    CHECK(line_break_class(U'\U000E0080') == LineBreakClass::AL); // unassigned: XX resolved
    CHECK(line_break_class(U'\xA0') == LineBreakClass::GL); // NO-BREAK SPACE
    CHECK((line_break_flags(U'あ') & line_break_east_asian) != 0);
    CHECK((line_break_flags(U'a') & line_break_east_asian) == 0);
    CHECK((line_break_flags(U'“') & line_break_initial_quote) != 0);
    CHECK((line_break_flags(U'”') & line_break_final_quote) != 0);

    // The everyday cases: a space is a break after it, a hyphen a break
    // after it, a word never breaks inside, an ideograph breaks anywhere,
    // a URL breaks after its slashes, a newline is a hard break.
    CHECK_EQ(picture(U"ab cd"), "× a × b × _ \xC3\xB7 c × d \xC3\xB7");
    CHECK_EQ(picture(U"ab-cd"), "× a × b × - \xC3\xB7 c × d \xC3\xB7");
    CHECK_EQ(picture(U"日本語"), "× U+26085 \xC3\xB7 U+26412 \xC3\xB7 U+35486 \xC3\xB7");
    CHECK_EQ(picture(U"a/b"), "× a × / \xC3\xB7 b \xC3\xB7");
    CHECK_EQ(picture(U"3.14"), "× 3 × . × 1 × 4 \xC3\xB7");
    CHECK_EQ(picture(U"a\nb"), "× a × LF \xC3\xB7 b \xC3\xB7");
    CHECK_EQ(picture(U"a\xA0" "b"), "× a × U+160 × b \xC3\xB7"); // a no-break space glues
    CHECK_EQ(picture(U"(a)"), "× ( × a × ) \xC3\xB7");
    CHECK_EQ(picture(U"a “b” c"), "× a × _ \xC3\xB7 U+8220 × b × U+8221 × _ \xC3\xB7 c \xC3\xB7");
    CHECK_EQ(picture(U"$1,000"), "× $ × 1 × , × 0 × 0 × 0 \xC3\xB7");
    CHECK_EQ(picture(U""), "\xC3\xB7");
    CHECK_EQ(line_break_opportunities(U"ab").size(), std::size_t { 3 });
    CHECK(line_break_opportunities(U"a\nb")[2] == LineBreak::Mandatory);
    CHECK(line_break_opportunities(U"a b")[2] == LineBreak::Allowed);
    CHECK(line_break_opportunities(U"ab")[1] == LineBreak::None);

    // A small kana (CJ) is a non-starter unless the style says otherwise;
    // line-break: normal and loose also let an iteration mark start a line
    // and a line end between two inseparable characters, and loose alone
    // lets a hyphen start a line after an ideograph.
    CHECK_EQ(picture(U"アー"), "× U+12450 × U+12540 \xC3\xB7");
    CHECK_EQ(picture(U"アー", line_break_tailor_normal), "× U+12450 \xC3\xB7 U+12540 \xC3\xB7");
    CHECK_EQ(picture(U"字々"), "× U+23383 × U+12293 \xC3\xB7");
    CHECK_EQ(picture(U"字々", line_break_tailor_normal), "× U+23383 \xC3\xB7 U+12293 \xC3\xB7");
    CHECK_EQ(picture(U"a……"), "× a × U+8230 × U+8230 \xC3\xB7");
    CHECK_EQ(picture(U"a……", line_break_tailor_normal), "× a × U+8230 \xC3\xB7 U+8230 \xC3\xB7");
    CHECK_EQ(picture(U"文文‐文"), "× U+25991 \xC3\xB7 U+25991 × U+8208 \xC3\xB7 U+25991 \xC3\xB7");
    CHECK_EQ(picture(U"文文‐文", line_break_tailor_normal), "× U+25991 \xC3\xB7 U+25991 × U+8208 \xC3\xB7 U+25991 \xC3\xB7");
    CHECK_EQ(picture(U"文文‐文", line_break_tailor_normal | line_break_tailor_loose),
        "× U+25991 \xC3\xB7 U+25991 \xC3\xB7 U+8208 \xC3\xB7 U+25991 \xC3\xB7");
    CHECK_EQ(picture(U"aa‐a", line_break_tailor_loose), "× a × a × U+8208 \xC3\xB7 a \xC3\xB7"); // no ideograph in front of it
    // The CSS tailorings: break-all lets letters and digits break like
    // ideographs but leaves the punctuation rules alone, keep-all keeps
    // letters and ideographs together and nothing else, anywhere breaks
    // around every character, a joiner's included, but for the joined
    // pictographs that make one; break-spaces breaks after every space.
    CHECK_EQ(picture(U"abc 12", line_break_tailor_break_all), "× a \xC3\xB7 b \xC3\xB7 c × _ \xC3\xB7 1 \xC3\xB7 2 \xC3\xB7");
    CHECK_EQ(picture(U"a.b", line_break_tailor_break_all), "× a × . \xC3\xB7 b \xC3\xB7");
    CHECK_EQ(picture(U"日本語 日本語", line_break_tailor_keep_all),
        "× U+26085 × U+26412 × U+35486 × _ \xC3\xB7 U+26085 × U+26412 × U+35486 \xC3\xB7");
    CHECK_EQ(picture(U"ab-cd", line_break_tailor_keep_all), "× a × b × - \xC3\xB7 c × d \xC3\xB7");
    CHECK_EQ(picture(U"a.b c", line_break_tailor_anywhere), "× a \xC3\xB7 . \xC3\xB7 b \xC3\xB7 _ \xC3\xB7 c \xC3\xB7");
    CHECK_EQ(picture(U"XX‍XX", line_break_tailor_anywhere), "× X \xC3\xB7 X × U+8205 \xC3\xB7 X \xC3\xB7 X \xC3\xB7");
    CHECK_EQ(picture(U"\U0001F602‍\U0001F62D", line_break_tailor_anywhere), "× U+128514 × U+8205 × U+128557 \xC3\xB7");
    CHECK_EQ(picture(U"ab", line_break_tailor_break_all | line_break_tailor_keep_all), "× a × b \xC3\xB7");
    CHECK_EQ(picture(U"a  b"), "× a × _ × _ \xC3\xB7 b \xC3\xB7");
    CHECK_EQ(picture(U"a  b", line_break_tailor_break_spaces), "× a × _ \xC3\xB7 _ \xC3\xB7 b \xC3\xB7");
    CHECK_EQ(picture(U"あ　　あ"), "× U+12354 × U+12288 × U+12288 \xC3\xB7 U+12354 \xC3\xB7");
    CHECK_EQ(picture(U"あ　　あ", line_break_tailor_break_spaces),
        "× U+12354 × U+12288 \xC3\xB7 U+12288 \xC3\xB7 U+12354 \xC3\xB7");
    // Thai and its neighbours (SA) are whole from space to space by the
    // rules alone, which expect a dictionary; without one a line may end
    // between clusters, a leading vowel staying with the letter after it
    // and a following vowel with the one before.
    CHECK_EQ(picture(U"การ"), "× U+3585 × U+3634 × U+3619 \xC3\xB7");
    CHECK_EQ(picture(U"การ", line_break_tailor_clusters), "× U+3585 × U+3634 \xC3\xB7 U+3619 \xC3\xB7");
    CHECK_EQ(picture(U"เกิด", line_break_tailor_clusters), "× U+3648 × U+3585 × U+3636 \xC3\xB7 U+3604 \xC3\xB7");
    CHECK(is_other_space_separator(U'　'));
    CHECK(!is_other_space_separator(U' '));
    CHECK(!is_other_space_separator(U'\xA0'));

    if (argc > 1)
        run_conformance(argv[1]);
    else
        std::cout << "LineBreakTest.txt not given: the hand-written cases alone\n";
    return sashfold::test::report("line_break");
}
