#include "Test.h"

#include "core/WordBreak.h"
#include "ui/FieldEdit.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Word boundaries (UAX #29), against WordBreakTest.txt when given its path
// (fetched by tools/ucd-fetch.sh, not vendored), and the moves a text
// field makes by them: Ctrl+Left/Right, the word a double click selects.

using namespace sashfold;

namespace {

struct Case {
    std::u32string text;
    std::vector<bool> breaks;
};

bool parse_case(std::string const& line, Case& out)
{
    std::istringstream stream(line.substr(0, line.find('#')));
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
    std::size_t number = 0;
    while (std::getline(file, line)) {
        ++number;
        if (line.empty() || line[0] == '#')
            continue;
        Case one;
        if (!parse_case(line, one))
            continue;
        ++cases;
        if (word_boundaries(one.text) == one.breaks)
            continue;
        ++failures;
        if (failures <= 12)
            std::cerr << "line " << number << ": " << line.substr(0, line.find('#')) << "\n";
    }
    std::cout << "WordBreakTest.txt: " << (cases - failures) << " / " << cases << " cases\n";
    CHECK(cases > 1800);
    CHECK_EQ(failures, std::size_t { 0 });
}

} // namespace

int main(int argc, char** argv)
{
    CHECK(word_break_class(U'a') == WordBreakClass::ALetter);
    CHECK(word_break_class(U'7') == WordBreakClass::Numeric);
    CHECK(word_break_class(U' ') == WordBreakClass::WSegSpace);
    CHECK(word_break_class(U'\'') == WordBreakClass::Single_Quote);
    CHECK(word_break_class(U'_') == WordBreakClass::ExtendNumLet);

    // "can't stop" keeps its apostrophe inside the word (WB6, WB7);
    // 3.14 is one number (WB11, WB12).
    std::vector<bool> const cant = word_boundaries(U"can't stop");
    CHECK(!cant[3] && !cant[4]);
    CHECK(cant[5] && cant[6]);
    std::vector<bool> const pi = word_boundaries(U"x 3.14 y");
    CHECK(!pi[3] && !pi[4] && !pi[5]);

    using namespace ui::field_edit;
    std::u32string const text = U"hello, big world";
    CHECK_EQ(next_word_end(text, 0), std::size_t { 5 });
    CHECK_EQ(next_word_end(text, 5), std::size_t { 10 }); // past ", " to the end of "big"
    CHECK_EQ(next_word_end(text, 16), std::size_t { 16 });
    CHECK_EQ(previous_word_start(text, 16), std::size_t { 11 });
    CHECK_EQ(previous_word_start(text, 11), std::size_t { 7 });
    CHECK_EQ(previous_word_start(text, 9), std::size_t { 7 }); // inside "big": its start
    CHECK_EQ(previous_word_start(text, 0), std::size_t { 0 });
    CHECK(word_at(text, 8) == std::make_pair(std::size_t { 7 }, std::size_t { 10 }));
    CHECK(word_at(text, 5) == std::make_pair(std::size_t { 5 }, std::size_t { 6 })); // the comma alone
    CHECK(word_at(text, 16) == std::make_pair(std::size_t { 11 }, std::size_t { 16 })); // at the end: the last word

    // A caret never stands between a letter and the accent on it.
    std::u32string const accented = U"ét";
    CHECK_EQ(next_character(accented, 0), std::size_t { 2 });
    CHECK_EQ(previous_character(accented, 2), std::size_t { 0 });

    CHECK(line_at(U"one\ntwo\nthree", 5) == std::make_pair(std::size_t { 4 }, std::size_t { 7 }));

    // maxlength counts UTF-16 code units of what stays.
    CHECK(within_max_length(U"abc", 1, 2, U"XYZ", 4) == U"XY");
    CHECK(within_max_length(U"abc", 0, 0, U"\U0001F600", 4) == U"");
    CHECK(within_max_length(U"abc", 0, 0, U"\U0001F600", 5) == U"\U0001F600");
    CHECK(within_max_length(U"abc", 0, 0, U"XYZ", std::nullopt) == U"XYZ");

    if (argc > 1)
        run_conformance(argv[1]);
    else
        std::cout << "WordBreakTest.txt not given: the hand-written cases alone (tools/ucd-fetch.sh fetches it)\n";
    return sashfold::test::report("word_break");
}
