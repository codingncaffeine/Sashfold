#include "Test.h"

#include "core/Bidi.h"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// The bidirectional algorithm, against its own conformance data when that
// is present. BidiCharacterTest.txt is 7 MB, so it is fetched rather than
// vendored (tools/ucd-fetch.sh, as the WPT checkout is): given a path to it
// this runs every case, and given none it runs the hand-written ones alone
// and says so.

using namespace sashfold;

namespace {

std::vector<std::string> split(std::string const& text, char separator)
{
    std::vector<std::string> fields;
    std::stringstream stream(text);
    std::string field;
    while (std::getline(stream, field, separator))
        fields.push_back(field);
    return fields;
}

std::vector<std::string> words(std::string const& text)
{
    std::vector<std::string> pieces;
    std::stringstream stream(text);
    std::string piece;
    while (stream >> piece)
        pieces.push_back(piece);
    return pieces;
}

// One line of BidiCharacterTest.txt: the code points, the direction asked
// for (0 left-to-right, 1 right-to-left, 2 from the text itself), the level
// that came out, a level per character with `x` where the character is
// removed, and the visual order as indices into the input.
struct CharacterCase {
    std::u32string text;
    int direction = 0;
    int paragraph_level = 0;
    std::vector<int> levels; // -1 for a removed character
    std::vector<std::size_t> order;
};

bool parse_case(std::string const& line, CharacterCase& out)
{
    std::vector<std::string> const fields = split(line, ';');
    if (fields.size() < 5)
        return false;
    for (std::string const& code_point : words(fields[0]))
        out.text.push_back(static_cast<char32_t>(std::stoul(code_point, nullptr, 16)));
    out.direction = std::stoi(fields[1]);
    out.paragraph_level = std::stoi(fields[2]);
    for (std::string const& level : words(fields[3]))
        out.levels.push_back(level == "x" ? -1 : std::stoi(level));
    for (std::string const& index : words(fields[4]))
        out.order.push_back(static_cast<std::size_t>(std::stoul(index)));
    return true;
}

// The conformance file, run case by case. Only the first few failures are
// printed: a broken rule fails thousands of cases, and the first ones say
// as much as the last.
int run_character_test(std::string const& path)
{
    std::ifstream file(path);
    if (!file) {
        std::cerr << "test_bidi: cannot open " << path << "\n";
        return 1;
    }
    std::string line;
    std::size_t total = 0;
    std::size_t failed = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        CharacterCase test;
        if (!parse_case(line, test))
            continue;
        ++total;

        std::optional<std::uint8_t> const asked = test.direction == 2
            ? std::nullopt
            : std::optional<std::uint8_t>(static_cast<std::uint8_t>(test.direction));
        BidiParagraph const paragraph = bidi_resolve(test.text, asked);
        std::vector<std::size_t> const order = bidi_visual_order(paragraph);

        bool ok = paragraph.paragraph_level == test.paragraph_level;
        for (std::size_t i = 0; ok && i < test.text.size(); ++i) {
            if (test.levels[i] < 0)
                ok = paragraph.removed[i];
            else
                ok = !paragraph.removed[i] && paragraph.levels[i] == test.levels[i];
        }
        ok = ok && order == test.order;
        if (!ok && failed < 5)
            std::cerr << "test_bidi: FAIL " << line << "\n";
        failed += ok ? 0 : 1;
    }
    std::cout << "BidiCharacterTest: " << (total - failed) << " / " << total << " cases\n";
    return failed == 0 ? 0 : 1;
}

// BidiTest.txt names classes rather than characters, so it needs one code
// point per class to stand for it. None of these is a bracket: N0 would
// then fire on cases the file does not expect it to.
char32_t representative(std::string const& name)
{
    static std::map<std::string, char32_t> const of = {
        { "L", U'a' }, { "R", 0x05D0 }, { "AL", 0x0627 }, // strong
        { "EN", U'0' }, { "ES", U'+' }, { "ET", U'#' }, { "AN", 0x0660 }, { "CS", U',' },
        { "NSM", 0x0300 }, { "BN", 0x00AD }, // weak
        { "B", 0x2029 }, { "S", U'\t' }, { "WS", U' ' }, { "ON", U'!' }, // neutral
        { "LRE", 0x202A }, { "RLE", 0x202B }, { "PDF", 0x202C }, { "LRO", 0x202D },
        { "RLO", 0x202E }, // embeddings and overrides
        { "LRI", 0x2066 }, { "RLI", 0x2067 }, { "FSI", 0x2068 }, { "PDI", 0x2069 }, // isolates
    };
    auto const found = of.find(name);
    if (found == of.end()) {
        std::cerr << "test_bidi: BidiTest names a class with no stand-in: " << name << "\n";
        std::exit(1);
    }
    return found->second;
}

// BidiTest.txt: an @Levels and an @Reorder line set the expectation, and
// the data lines that follow give a class sequence and a bitset of the
// paragraph directions to try it in (1 automatic, 2 left-to-right, 4
// right-to-left).
int run_bidi_test(std::string const& path)
{
    std::ifstream file(path);
    if (!file) {
        std::cerr << "test_bidi: cannot open " << path << "\n";
        return 1;
    }
    std::vector<int> levels; // -1 where the file writes `x`
    std::vector<std::size_t> order;
    std::string line;
    std::size_t total = 0;
    std::size_t failed = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.starts_with("@Levels:")) {
            levels.clear();
            for (std::string const& level : words(line.substr(8)))
                levels.push_back(level == "x" ? -1 : std::stoi(level));
            continue;
        }
        if (line.starts_with("@Reorder:")) {
            order.clear();
            for (std::string const& index : words(line.substr(9)))
                order.push_back(static_cast<std::size_t>(std::stoul(index)));
            continue;
        }
        std::vector<std::string> const fields = split(line, ';');
        if (fields.size() < 2)
            continue;
        std::u32string text;
        for (std::string const& name : words(fields[0]))
            text.push_back(representative(name));
        int const wanted = std::stoi(words(fields[1]).front());
        for (int which = 0; which < 3; ++which) {
            if ((wanted & (1 << which)) == 0)
                continue;
            std::optional<std::uint8_t> const asked = which == 0
                ? std::nullopt
                : std::optional<std::uint8_t>(static_cast<std::uint8_t>(which == 1 ? 0 : 1));
            ++total;
            BidiParagraph const paragraph = bidi_resolve(text, asked);
            bool ok = true;
            for (std::size_t i = 0; ok && i < text.size(); ++i) {
                if (levels[i] < 0)
                    ok = paragraph.removed[i];
                else
                    ok = !paragraph.removed[i] && paragraph.levels[i] == levels[i];
            }
            ok = ok && bidi_visual_order(paragraph) == order;
            if (!ok && failed < 5)
                std::cerr << "test_bidi: FAIL " << line << " (direction " << which << ")\n";
            failed += ok ? 0 : 1;
        }
    }
    std::cout << "BidiTest: " << (total - failed) << " / " << total << " cases\n";
    return failed == 0 ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    // --- The strong directions, and the first of them --------------------------
    {
        CHECK(strong_direction(U'a') == StrongDirection::Ltr);
        CHECK(strong_direction(U'漢') == StrongDirection::Ltr); // Han is left-to-right
        CHECK(strong_direction(U'\u05D0') == StrongDirection::Rtl); // Hebrew alef, class R
        CHECK(strong_direction(U'\u0627') == StrongDirection::Rtl); // Arabic alef, class AL
        // An UNASSIGNED code point inside a block reserved for a right-to-left
        // script is R, not the L everything else defaults to — which is why
        // the table comes from DerivedBidiClass and not UnicodeData field 4.
        CHECK(strong_direction(U'\u0590') == StrongDirection::Rtl);
        // Neither: the digits, the punctuation, the spaces, the marks.
        CHECK(strong_direction(U'5') == StrongDirection::None);
        CHECK(strong_direction(U'.') == StrongDirection::None);
        CHECK(strong_direction(U' ') == StrongDirection::None);
        CHECK(strong_direction(U'́') == StrongDirection::None); // combining acute

        CHECK(!first_strong_is_rtl(U"alpha"));
        CHECK(first_strong_is_rtl(U"\u05D0\u05D1\u05D2"));
        CHECK(!first_strong_is_rtl(U"123 — alpha")); // it steps over what is not strong
        CHECK(first_strong_is_rtl(U"123 — \u05D0\u05D1"));
        CHECK(!first_strong_is_rtl(U"")); // P3: nothing strong reads left-to-right
        CHECK(!first_strong_is_rtl(U"12.34"));
        // P2 steps over everything inside an isolate, so the Hebrew here does
        // not answer for the text around it. U+2066 is the left-to-right
        // isolate, U+2067 the right-to-left one, U+2069 the terminator.
        // Escaped, not written out: an unpaired control in a literal is a
        // warning in its own right, and it would reorder this source too.
        CHECK(!first_strong_is_rtl(U"\u2066\u05D0\u2069alpha"));
        CHECK(first_strong_is_rtl(U"\u2066alpha\u2069\u05D0"));
        // An isolate with no terminator swallows the rest of the paragraph.
        CHECK(!first_strong_is_rtl(U"\u2067\u05D0"));
        // An unmatched PDI ends nothing, and is itself not strong.
        CHECK(first_strong_is_rtl(U"\u2069\u05D0"));
    }

    // --- The whole algorithm, on cases small enough to read --------------------
    {
        auto const visual = [](std::u32string_view text, std::optional<std::uint8_t> level) {
            BidiParagraph const paragraph = bidi_resolve(text, level);
            std::u32string drawn;
            for (std::size_t const position : bidi_visual_order(paragraph))
                drawn.push_back(text[position]);
            return drawn;
        };
        // Latin in a left-to-right paragraph is drawn as it is written.
        CHECK(visual(U"abc", 0) == U"abc");
        // Hebrew is drawn the other way round, whichever paragraph it is in.
        CHECK(visual(U"\u05D0\u05D1\u05D2", 0) == U"\u05D2\u05D1\u05D0");
        CHECK(visual(U"\u05D0\u05D1\u05D2", 1) == U"\u05D2\u05D1\u05D0");
        // A Latin run inside right-to-left text keeps its own order and goes
        // to the left of the words around it (I1, I2 and L2 together).
        CHECK(visual(U"\u05D0\u05D1abc\u05D2", 1) == U"\u05D2abc\u05D1\u05D0");
        // The digits of a number are read left to right inside Arabic text.
        CHECK(visual(U"\u05D012\u05D1", 1) == U"\u05D112\u05D0");
        // A space between two right-to-left words goes with them (N1); the
        // one at the end of a left-to-right line stays at the end (L1).
        CHECK(visual(U"\u05D0\u05D1 \u05D2\u05D3", 0) == U"\u05D3\u05D2 \u05D1\u05D0");
        // A bracket pair reads as the text around it unless what it holds
        // disagrees (N0), so in right-to-left text the two brackets swap
        // ends: the CLOSING parenthesis is the character that comes out
        // first. They still look right on the page \u2014 rule L4 draws a
        // mirrored glyph for a bracket that resolved right-to-left, and that
        // is a painting step, not part of the order.
        CHECK(visual(U"\u05D0(abc)\u05D1", 1) == U"\u05D1)abc(\u05D0");
        CHECK(visual(U"\u05D0(12)\u05D1", 1) == U"\u05D1)12(\u05D0");
        // An override draws text in one direction whatever it says: U+202E
        // is the right-to-left override, U+202C its pop, and both are
        // removed from the output by X9.
        CHECK(visual(U"\u202Eabc\u202C", 0) == U"cba");
        // An isolate keeps its content from answering for the text around
        // it: U+2067 is the right-to-left isolate, U+2069 its terminator.
        CHECK(visual(U"a\u2067\u05D1\u2069c", 0) == U"a\u2067\u05D1\u2069c");
    }

    // The conformance files, when the checkout is there: the first names
    // characters, the second names classes, so between them they hold both
    // the table and the rules to account.
    if (argc > 2) {
        int const failures = run_character_test(argv[1]) + run_bidi_test(argv[2]);
        if (failures != 0)
            return 1;
    } else {
        std::cout << "BidiCharacterTest and BidiTest: not run (no data paths given)\n";
    }
    return sashfold::test::report("bidi");
}
