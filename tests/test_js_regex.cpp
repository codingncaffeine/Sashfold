#include "Test.h"

#include "js/Regex.h"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sashfold;
using namespace sashfold::js;

namespace {

// A group's code-unit span; {-1, -1} is a group that did not participate.
struct Span {
    long start;
    long end;
};
constexpr Span none { -1, -1 };

struct MatchCase {
    std::u16string_view pattern;
    std::u16string_view flags;
    std::u16string_view input;
    std::size_t start;
    std::vector<Span> groups; // empty: no match at all
};

struct ErrorCase {
    std::u16string_view pattern;
    std::u16string_view flags;
    char const* message;
};

void expect(bool ok, std::string const& message, int line)
{
    ++sashfold::test::g_checks;
    if (!ok)
        sashfold::test::fail(message, __FILE__, line);
}

// Printable ASCII as itself, everything else as \uXXXX, so a failure
// names the exact code units involved.
std::string narrow(std::u16string_view text)
{
    std::string out;
    for (char16_t const c : text) {
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        char buffer[16];
        std::snprintf(buffer, sizeof buffer, "\\u%04X", static_cast<unsigned>(c));
        out += buffer;
    }
    return out;
}

std::string describe(std::optional<Regex::Match> const& match)
{
    if (!match)
        return "no match";
    std::string out;
    for (auto const& group : match->groups) {
        if (!out.empty())
            out += ' ';
        if (group)
            out += "[" + std::to_string(group->first) + "," + std::to_string(group->second) + "]";
        else
            out += "[-]";
    }
    return out;
}

std::string describe(std::vector<Span> const& spans)
{
    if (spans.empty())
        return "no match";
    std::string out;
    for (Span const& span : spans) {
        if (!out.empty())
            out += ' ';
        if (span.start < 0)
            out += "[-]";
        else
            out += "[" + std::to_string(span.start) + "," + std::to_string(span.end) + "]";
    }
    return out;
}

std::string label(std::u16string_view pattern, std::u16string_view flags)
{
    return "/" + narrow(pattern) + "/" + narrow(flags);
}

void run_match_case(MatchCase const& c, int line)
{
    std::string const name = label(c.pattern, c.flags) + " on \"" + narrow(c.input) + "\" from " + std::to_string(c.start);
    auto const flags = RegexFlags::parse(c.flags);
    if (!flags) {
        expect(false, name + ": flags did not parse", line);
        return;
    }
    Regex::CompileError error;
    auto const regex = Regex::compile(c.pattern, *flags, &error);
    if (!regex) {
        expect(false, name + ": did not compile: " + error.message, line);
        return;
    }
    bool exhausted = true;
    auto const match = regex->exec(c.input, c.start, &exhausted);
    std::string const actual = describe(match);
    std::string const expected = describe(c.groups);
    expect(!exhausted && actual == expected,
        name + "\n       actual:   " + actual + (exhausted ? " (budget exhausted)" : "") + "\n       expected: " + expected,
        line);
}

void run_error_case(ErrorCase const& c, int line)
{
    std::string const name = label(c.pattern, c.flags);
    auto const flags = RegexFlags::parse(c.flags);
    if (!flags) {
        expect(false, name + ": flags did not parse", line);
        return;
    }
    Regex::CompileError error;
    auto const regex = Regex::compile(c.pattern, *flags, &error);
    if (regex) {
        expect(false, name + ": compiled, expected \"" + c.message + "\"", line);
        return;
    }
    expect(error.message == c.message,
        name + "\n       actual:   " + error.message + "\n       expected: " + c.message, line);
}

}

int main()
{
    // Each entry: pattern, flags, input, start, expected groups (group 0
    // first; `none` for a group that did not participate; an empty list
    // for no match). Every expectation was taken from the specification's
    // semantics, and the annotated ones from its own worked examples.
    std::vector<MatchCase> const match_cases {
        // Literals and the start position.
        { u"abc", u"", u"xabcx", 0, { { 1, 4 } } },
        { u"abc", u"", u"ab", 0, {} },
        { u"", u"", u"xyz", 0, { { 0, 0 } } },
        { u"a", u"", u"bbb", 0, {} },
        { u"hello", u"", u"say hello", 4, { { 4, 9 } } },
        { u"a", u"", u"aaa", 1, { { 1, 2 } } },
        { u"a", u"", u"aaa", 3, {} },
        { u"", u"", u"ab", 2, { { 2, 2 } } },
        { u"", u"", u"ab", 5, {} }, // a start past the end is no match

        // Character classes: ranges, negation, escapes inside, Annex B forms.
        { u"[abc]+", u"", u"xxbcax", 0, { { 2, 5 } } },
        { u"[a-c]+", u"", u"dcbad", 0, { { 1, 4 } } },
        { u"[^a-c]+", u"", u"abcxyzabc", 0, { { 3, 6 } } },
        { u"[a-]", u"", u"-", 0, { { 0, 1 } } },
        { u"[-a]", u"", u"-", 0, { { 0, 1 } } },
        { u"[]", u"", u"abc", 0, {} },
        { u"[^]", u"", u"\n", 0, { { 0, 1 } } },
        { u"[\\d]+", u"", u"ab123", 0, { { 2, 5 } } },
        { u"[\\s\\S]+", u"", u"a\nb", 0, { { 0, 3 } } },
        { u"[a\\-z]", u"", u"-", 0, { { 0, 1 } } },
        { u"[\\b]", u"", u"\b", 0, { { 0, 1 } } }, // backspace inside a class
        { u"[.]", u"", u"a.b", 0, { { 1, 2 } } },
        { u"[a-z\\d]+", u"", u"x9y!", 0, { { 0, 3 } } },
        { u"[\\w-]+", u"", u"a-b c", 0, { { 0, 3 } } },
        { u"[\\w-a]", u"", u"-", 0, { { 0, 1 } } }, // B.1.2: a class escape cannot bound a range; the dash is literal
        { u"[a-b-c]", u"", u"-", 0, { { 0, 1 } } },
        { u"[\\c1]", u"", u"\x11", 0, { { 0, 1 } } }, // B.1.2 ClassControlLetter
        { u"[\\c]", u"", u"\\", 0, { { 0, 1 } } }, // B.1.2: \c with no letter keeps the backslash
        { u"[\\c]", u"", u"c", 0, { { 0, 1 } } },

        // Escapes.
        { u"\\d+", u"", u"abc123def", 0, { { 3, 6 } } },
        { u"\\D+", u"", u"123abc456", 0, { { 3, 6 } } },
        { u"\\w+", u"", u"  foo_bar9  ", 0, { { 2, 10 } } },
        { u"\\W+", u"", u"ab, cd", 0, { { 2, 4 } } },
        { u"\\s+", u"", u"a \t\n\u00A0b", 0, { { 1, 5 } } },
        { u"\\s", u"", u"\u2028", 0, { { 0, 1 } } },
        { u"\\s", u"", u"\uFEFF", 0, { { 0, 1 } } },
        { u"\\S+", u"", u"  ab  ", 0, { { 2, 4 } } },
        { u"\\t\\n\\v\\f\\r", u"", u"\t\n\v\f\r", 0, { { 0, 5 } } },
        { u"\\cJ", u"", u"\n", 0, { { 0, 1 } } },
        { u"\\x41", u"", u"A", 0, { { 0, 1 } } },
        { u"\\u0041", u"", u"A", 0, { { 0, 1 } } },
        { u"\\u{1F600}", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u"\\u{41}", u"u", u"A", 0, { { 0, 1 } } },
        { u"\\0", u"", std::u16string_view(u"\0", 1), 0, { { 0, 1 } } },
        { u"\\.", u"", u"a.b", 0, { { 1, 2 } } },
        { u"\\/", u"", u"/", 0, { { 0, 1 } } },
        // Annex B.1.2 outside `u`: identity escapes, \c without a letter,
        // incomplete hex escapes, legacy octal, \8 \9, \p, \k, lone brackets.
        { u"\\a", u"", u"a", 0, { { 0, 1 } } },
        { u"\\c", u"", u"\\c", 0, { { 0, 2 } } },
        { u"\\c1", u"", u"\\c1", 0, { { 0, 3 } } },
        { u"\\x4", u"", u"x4", 0, { { 0, 2 } } },
        { u"\\u12", u"", u"u12", 0, { { 0, 3 } } },
        { u"\\101", u"", u"A", 0, { { 0, 1 } } },
        { u"\\1", u"", u"\x01", 0, { { 0, 1 } } },
        { u"\\8", u"", u"8", 0, { { 0, 1 } } },
        { u"\\08", u"", std::u16string_view(u"\0" u"8", 2), 0, { { 0, 2 } } },
        { u"\\400", u"", u" 0", 0, { { 0, 2 } } }, // 4–7 take one more digit at most
        { u"\\377", u"", u"\u00FF", 0, { { 0, 1 } } },
        { u"\\p{L}", u"", u"p{L}", 0, { { 0, 4 } } },
        { u"\\k", u"", u"k", 0, { { 0, 1 } } },
        { u"]", u"", u"]", 0, { { 0, 1 } } },
        { u"}", u"", u"}", 0, { { 0, 1 } } },
        { u"{", u"", u"{", 0, { { 0, 1 } } },
        { u"a{", u"", u"a{", 0, { { 0, 2 } } },
        { u"a{1,2", u"", u"a{1,2", 0, { { 0, 5 } } },
        { u"a{,3}", u"", u"a{,3}", 0, { { 0, 5 } } },
        { u"\\u{1F600}", u"", u"u{1F600}", 0, { { 0, 8 } } }, // no `u`: the letter u, then literal braces

        // Quantifier forms.
        { u"x{2}", u"", u"xxx", 0, { { 0, 2 } } },
        { u"a{2,}", u"", u"aaaa", 0, { { 0, 4 } } },
        { u"a{1,3}", u"", u"aaaaa", 0, { { 0, 3 } } },
        { u"a{1,3}?", u"", u"aaaaa", 0, { { 0, 1 } } },
        { u"a{0}", u"", u"aaa", 0, { { 0, 0 } } },
        { u"a{0,0}b", u"", u"aab", 0, { { 2, 3 } } },
        { u"(?:ab){2}", u"", u"ababab", 0, { { 0, 4 } } },
        { u"(?:ab){1,2}?c", u"", u"ababc", 0, { { 0, 5 } } },
        { u"a{3}", u"", u"aa", 0, {} },

        // Anchors and word boundaries, with and without multiline.
        { u"^abc", u"", u"abc", 0, { { 0, 3 } } },
        { u"^abc", u"", u"xabc", 0, {} },
        { u"abc$", u"", u"xabc", 0, { { 1, 4 } } },
        { u"abc$", u"", u"abcx", 0, {} },
        { u"^$", u"", u"", 0, { { 0, 0 } } },
        { u"^b", u"m", u"a\nb", 0, { { 2, 3 } } },
        { u"a$", u"m", u"a\nb", 0, { { 0, 1 } } },
        { u"^b", u"", u"a\nb", 0, {} },
        { u"a$", u"m", u"a\u2028b", 0, { { 0, 1 } } },
        { u"^b", u"m", u"a\rb", 0, { { 2, 3 } } },
        { u"\\bfoo\\b", u"", u"a foo b", 0, { { 2, 5 } } },
        { u"\\bfoo\\b", u"", u"afoo", 0, {} },
        { u"\\Bfoo", u"", u"afoo", 0, { { 1, 4 } } },
        { u"\\b", u"", u"", 0, {} },
        { u"\\B", u"", u"", 0, { { 0, 0 } } },
        { u"\\b", u"", u"  a", 0, { { 2, 2 } } },

        // Sticky: only the start position is tried.
        { u"^", u"y", u"ab", 1, {} },
        { u"b", u"y", u"ab", 0, {} },
        { u"b", u"y", u"ab", 1, { { 1, 2 } } },
        { u"b", u"y", u"abb", 2, { { 2, 3 } } },
        { u"^b", u"my", u"a\nb", 2, { { 2, 3 } } },

        // Dot, line terminators, dotAll, and code points under `u`.
        { u"a.c", u"", u"abc", 0, { { 0, 3 } } },
        { u"a.c", u"", u"a\nc", 0, {} },
        { u"a.c", u"s", u"a\nc", 0, { { 0, 3 } } },
        { u".", u"", u"\r", 0, {} },
        { u".", u"", u"\u2029", 0, {} },
        { u".", u"s", u"\n", 0, { { 0, 1 } } },
        { u".+", u"", u"ab\ncd", 0, { { 0, 2 } } },
        { u".", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u".", u"", u"\U0001F600", 0, { { 0, 1 } } },
        { u"^.$", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u"^.$", u"", u"\U0001F600", 0, {} },
        { u"^.$", u"u", u"\xD83D", 0, { { 0, 1 } } }, // a lone surrogate is one character

        // Alternation, including backtracking into an earlier alternative.
        { u"a|b|c", u"", u"xc", 0, { { 1, 2 } } },
        { u"ab|abc", u"", u"abc", 0, { { 0, 2 } } }, // the leftmost alternative wins
        { u"(ab|abc)d", u"", u"abcd", 0, { { 0, 4 }, { 0, 3 } } },
        { u"a|", u"", u"b", 0, { { 0, 0 } } },
        { u"|a", u"", u"a", 0, { { 0, 0 } } },
        { u"(a|ab)(c|bcd)(d*)", u"", u"abcd", 0, { { 0, 4 }, { 0, 1 }, { 1, 4 }, { 4, 4 } } }, // §22.2.2.3 note
        { u"x(?:a|b)+y", u"", u"xababy", 0, { { 0, 6 } } },

        // Greedy against lazy.
        { u"a+", u"", u"aaa", 0, { { 0, 3 } } },
        { u"a+?", u"", u"aaa", 0, { { 0, 1 } } },
        { u"a*", u"", u"bbb", 0, { { 0, 0 } } },
        { u"a*?b", u"", u"aab", 0, { { 0, 3 } } },
        { u"a??b", u"", u"ab", 0, { { 0, 2 } } },
        { u"a??", u"", u"a", 0, { { 0, 0 } } },
        { u"x*?", u"", u"xxx", 0, { { 0, 0 } } },
        { u"<.*>", u"", u"<a><b>", 0, { { 0, 6 } } },
        { u"<.*?>", u"", u"<a><b>", 0, { { 0, 3 } } },
        { u"(a+)(a+)", u"", u"aaaa", 0, { { 0, 4 }, { 0, 3 }, { 3, 4 } } },
        { u"(a+?)(a+)", u"", u"aaaa", 0, { { 0, 4 }, { 0, 1 }, { 1, 4 } } },
        { u"a{2,3}?a", u"", u"aaaa", 0, { { 0, 3 } } },
        { u"(a|b)*", u"", u"abab", 0, { { 0, 4 }, { 3, 4 } } },
        { u"(a|b)*?c", u"", u"abc", 0, { { 0, 3 }, { 1, 2 } } },
        { u"a*b", u"", u"aaaaaaaaaac", 0, {} },
        { u"(?:ab)*?c", u"", u"ababc", 0, { { 0, 5 } } },

        // Nested groups: captures reset at every iteration (§22.2.2.3.1).
        { u"(?:(a)|b)+", u"", u"ab", 0, { { 0, 2 }, none } },
        { u"((a)|(b))+", u"", u"ab", 0, { { 0, 2 }, { 1, 2 }, none, { 1, 2 } } }, // §22.2.2.3.1 note 3
        { u"(a*)*", u"", u"b", 0, { { 0, 0 }, none } },
        { u"(a*)+", u"", u"b", 0, { { 0, 0 }, { 0, 0 } } },
        { u"(?:)*", u"", u"a", 0, { { 0, 0 } } },
        { u"(a*)*", u"", u"aa", 0, { { 0, 2 }, { 0, 2 } } },
        { u"(a|b)*", u"", u"", 0, { { 0, 0 }, none } },
        { u"(a)|(b)", u"", u"b", 0, { { 0, 1 }, none, { 0, 1 } } },
        { u"((a)b)*", u"", u"abab", 0, { { 0, 4 }, { 2, 4 }, { 2, 3 } } },
        { u"(z)((a+)?(b+)?(c))*", u"", u"zaacbbbcac", 0,
            { { 0, 10 }, { 0, 1 }, { 8, 10 }, { 8, 9 }, none, { 9, 10 } } }, // §22.2.2.3.1 note 4
        { u"(a?)*", u"", u"aa", 0, { { 0, 2 }, { 1, 2 } } },
        { u"(|a)*", u"", u"aa", 0, { { 0, 2 }, { 1, 2 } } },
        { u"(?:a*)*", u"", u"b", 0, { { 0, 0 } } },
        { u"(?:a?)*b", u"", u"aab", 0, { { 0, 3 } } },
        { u"(a){2}", u"", u"aa", 0, { { 0, 2 }, { 1, 2 } } },
        { u"(a)?", u"", u"b", 0, { { 0, 0 }, none } },
        { u"(?:(a)|(b))*", u"", u"ab", 0, { { 0, 2 }, none, { 1, 2 } } },
        { u"(a*|b)*", u"", u"b", 0, { { 0, 1 }, { 0, 1 } } },

        // Backreferences, numbered and named, forward and unmatched.
        { u"(a)\\1", u"", u"aa", 0, { { 0, 2 }, { 0, 1 } } },
        { u"(a)\\1", u"", u"ab", 0, {} },
        { u"(a*)\\1", u"", u"aaaa", 0, { { 0, 4 }, { 0, 2 } } },
        { u"(a)|\\1b", u"", u"b", 0, { { 0, 1 }, none } }, // an unmatched group matches empty
        { u"\\1(a)", u"", u"a", 0, { { 0, 1 }, { 0, 1 } } }, // a forward reference: the group is unset there
        { u"(a)\\2", u"", u"a\x02", 0, { { 0, 2 }, { 0, 1 } } }, // B.1.2: past the group count, so octal
        { u"(a)\\10", u"", u"a\b", 0, { { 0, 2 }, { 0, 1 } } }, // \10 is octal 8 here
        { u"(?<n>a)\\k<n>", u"", u"aa", 0, { { 0, 2 }, { 0, 1 } } },
        { u"(?<x>a)|\\k<x>b", u"", u"b", 0, { { 0, 1 }, none } },
        { u"\\k<n>(?<n>a)", u"", u"a", 0, { { 0, 1 }, { 0, 1 } } },
        { u"(a)\\1", u"i", u"aA", 0, { { 0, 2 }, { 0, 1 } } },
        { u"(?:(a)|b)\\1", u"", u"b", 0, { { 0, 1 }, none } },
        { u"(a)\\1", u"u", u"aa", 0, { { 0, 2 }, { 0, 1 } } },
        { u"(.)\\1", u"iu", u"\u00DF\u1E9E", 0, { { 0, 2 }, { 0, 1 } } },
        { u"(\\w+)\\s\\1", u"", u"hello hello world", 0, { { 0, 11 }, { 0, 5 } } },

        // Named groups, including a name shared across alternatives
        // (§22.2.1.1 MightBothParticipate): \k<name> is whichever matched.
        { u"(?<year>\\d{4})-(?<month>\\d{2})", u"", u"2024-06", 0, { { 0, 7 }, { 0, 4 }, { 5, 7 } } },
        { u"(?<a>x)|(?<b>y)", u"", u"y", 0, { { 0, 1 }, none, { 0, 1 } } },
        { u"(?<a>x)|(?<a>y)", u"", u"y", 0, { { 0, 1 }, none, { 0, 1 } } },
        { u"(?:(?<a>x)|(?<a>y))\\k<a>", u"", u"yy", 0, { { 0, 2 }, none, { 0, 1 } } },
        { u"(?:(?<a>x)|(?<a>y))\\k<a>", u"", u"xx", 0, { { 0, 2 }, { 0, 1 }, none } },
        { u"(?:(?<a>x)|(?<a>y))\\k<a>", u"", u"xy", 0, {} },
        { u"\\k<a>(?:(?<a>x)|(?<a>y))", u"", u"y", 0, { { 0, 1 }, none, { 0, 1 } } },
        { u"(?:(?<a>x)|(?<a>y))*\\k<a>", u"", u"xyy", 0, { { 0, 3 }, none, { 1, 2 } } },
        { u"(?<a>x)|(?:(?<a>y)|(?<a>z))", u"", u"z", 0, { { 0, 1 }, none, none, { 0, 1 } } },

        // Lookahead: positive keeps its captures, negative discards them,
        // both are atomic, and either may sit inside an alternation.
        { u"a(?=b)", u"", u"ab", 0, { { 0, 1 } } },
        { u"a(?=b)", u"", u"ac", 0, {} },
        { u"a(?!b)", u"", u"ac", 0, { { 0, 1 } } },
        { u"a(?!b)", u"", u"ab", 0, {} },
        { u"(?=(a))a", u"", u"a", 0, { { 0, 1 }, { 0, 1 } } },
        { u"(?!(a))b", u"", u"b", 0, { { 0, 1 }, none } },
        { u"(?=a|b)b", u"", u"b", 0, { { 0, 1 } } },
        { u"(?:(?=a)a|b)", u"", u"b", 0, { { 0, 1 } } },
        { u"(?:x(?!y)|xy)", u"", u"xy", 0, { { 0, 2 } } },
        { u"(?=(a+))a*b\\1", u"", u"baaabac", 0, { { 3, 6 }, { 3, 4 } } }, // §22.2.2.4 note 2: no backtracking into the lookahead
        { u"(?=a)*", u"", u"a", 0, { { 0, 0 } } }, // B.1.2 QuantifiableAssertion
        { u"^(?:(?=x)x|y)+$", u"", u"xyx", 0, { { 0, 3 } } },
        { u"(?!a)b", u"", u"ab", 0, { { 1, 2 } } },
        { u"a(?=(b))(?!\\1c)", u"", u"abd", 0, { { 0, 1 }, { 1, 2 } } },
        { u"(?=(a))(?!\\1)", u"", u"a", 0, {} },
        { u"(?=(?=a)a)a", u"", u"a", 0, { { 0, 1 } } },

        // ignoreCase: Canonicalize (§22.2.2.7.3) with and without `u`.
        { u"abc", u"i", u"xABCx", 0, { { 1, 4 } } },
        { u"[a-z]+", u"i", u"ABC", 0, { { 0, 3 } } },
        { u"[^a-z]", u"i", u"A", 0, {} },
        { u"A", u"i", u"a", 0, { { 0, 1 } } },
        { u"[A-Z]", u"i", u"q", 0, { { 0, 1 } } },
        { u"a.c", u"i", u"AXC", 0, { { 0, 3 } } },
        { u"\\x41", u"i", u"a", 0, { { 0, 1 } } },
        { u"\\u00e9", u"i", u"\u00C9", 0, { { 0, 1 } } }, // é / É
        { u"(?:\u00E9)+", u"i", u"\u00C9\u00E9", 0, { { 0, 2 } } },
        { u"[\u00E0-\u00E5]", u"i", u"\u00C4", 0, { { 0, 1 } } },
        { u"\u00B5", u"i", u"\u039C", 0, { { 0, 1 } } }, // µ uppercases to Greek capital mu
        { u"\u03A3", u"i", u"\u03C2", 0, { { 0, 1 } } }, // final sigma
        { u"\u017F", u"i", u"s", 0, {} }, // ſ would fold to ASCII: stays itself without `u`
        { u"\u017F", u"iu", u"s", 0, { { 0, 1 } } },
        { u"\\u212a", u"i", u"k", 0, {} }, // Kelvin sign likewise
        { u"\\u212a", u"iu", u"k", 0, { { 0, 1 } } },
        { u"k", u"iu", u"\u212A", 0, { { 0, 1 } } },
        { u"\u00DF", u"i", u"\u1E9E", 0, {} }, // ß / ẞ only fold together under `u`
        { u"\u00DF", u"iu", u"\u1E9E", 0, { { 0, 1 } } },
        { u"\\u0130", u"iu", u"i", 0, {} }, // İ has no simple folding
        // CaseFolding.txt status S without any simple case mapping.
        { u"\\u1FD3", u"iu", u"\u0390", 0, { { 0, 1 } } },
        { u"[\\u03B8-\\u1FFF]", u"iu", u"\u0390", 0, { { 0, 1 } } },
        { u"\\u1FE3", u"iu", u"\u03B0", 0, { { 0, 1 } } },
        { u"\\uFB06", u"iu", u"\uFB05", 0, { { 0, 1 } } },
        { u"\\u1FD3", u"i", u"\u0390", 0, {} }, // no uppercase mapping: unrelated without `u`
        { u"\\u1F80", u"i", u"\u1F88", 0, {} }, // full uppercase is two units: Canonicalize keeps U+1F80
        { u"\\u1F80", u"iu", u"\u1F88", 0, { { 0, 1 } } },
        // WordCharacters (§22.2.2.9.3) under `u` + `i` include ſ and K.
        { u"\\w", u"iu", u"\u017F", 0, { { 0, 1 } } },
        { u"\\W", u"iu", u"\u017F", 0, {} },
        { u"\\w", u"i", u"\u017F", 0, {} },
        { u"\\W", u"i", u"\u017F", 0, { { 0, 1 } } },
        { u"[\\W]", u"iu", u"S", 0, {} },
        { u"[^\\W]", u"iu", u"\u017F", 0, { { 0, 1 } } },
        { u"\\b\u017F", u"iu", u"a \u017F", 0, { { 2, 3 } } },

        // Surrogate pairs: one character under `u`, two code units without.
        { u"^[\U0001F600]$", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u"^[\U0001F600]$", u"", u"\U0001F600", 0, {} },
        { u"\U0001F600+", u"u", u"\U0001F600\U0001F600", 0, { { 0, 4 } } },
        { u"\U0001F600+", u"", u"\U0001F600\U0001F600", 0, { { 0, 2 } } }, // the + binds the trail unit only
        { u"\\uD83D\\uDE00", u"u", u"\U0001F600", 0, { { 0, 2 } } }, // two escapes join into one code point
        { u"[\\uD83D\\uDE00-\\uD83D\\uDE02]", u"u", u"\U0001F601", 0, { { 0, 2 } } },
        { u"[\\u{1F600}]", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u"[^a]", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u".", u"u", u"\U0001F600", 1, { { 0, 2 } } }, // §22.2.7.2: a start inside a pair backs up to its lead
        { u"\\uDE00", u"u", u"\U0001F600", 0, {} }, // no lone trail surrogate exists in the code-point view
        { u"\\uDE00", u"", u"\U0001F600", 0, { { 1, 2 } } },
        { u"^.$", u"u", u"\xDE00", 0, { { 0, 1 } } },
        { u"[\\u{10000}-\\u{10FFFF}]", u"u", u"a\U0001F600", 0, { { 1, 3 } } },
        { u"\U0001F600{2}", u"u", u"\U0001F600\U0001F600\U0001F600", 0, { { 0, 4 } } },
        { u"(?:\U0001F600)*?x", u"u", u"\U0001F600\U0001F600x", 0, { { 0, 5 } } },
        { u"a.*\U0001F600", u"u", u"a\U0001F600\U0001F600", 0, { { 0, 5 } } }, // greedy gives back a whole pair
        { u"\\S", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u"\\S", u"", u"\U0001F600", 0, { { 0, 1 } } },
    };
    for (MatchCase const& c : match_cases)
        run_match_case(c, __LINE__);

    std::vector<ErrorCase> const error_cases {
        { u"*a", u"", "Nothing to repeat" },
        { u"a**", u"", "Nothing to repeat" },
        { u"+", u"", "Nothing to repeat" },
        { u"?", u"", "Nothing to repeat" },
        { u"{1}", u"", "Nothing to repeat" },
        { u"{1,2}", u"", "Nothing to repeat" },
        { u"(?=a)*", u"u", "Nothing to repeat" },
        { u"^*", u"", "Nothing to repeat" },
        { u"\\b+", u"", "Nothing to repeat" },
        { u"a*??", u"", "Nothing to repeat" },
        { u"a{2}{3}", u"", "Nothing to repeat" },
        { u"[abc", u"", "Unterminated character class" },
        { u"[", u"", "Unterminated character class" },
        { u"[a-", u"", "Unterminated character class" },
        { u"(abc", u"", "Unterminated group" },
        { u"(?:a", u"", "Unterminated group" },
        { u"(?=a", u"", "Unterminated group" },
        { u"(?<a>", u"", "Unterminated group" },
        { u"a)", u"", "Unmatched ')'" },
        { u")", u"", "Unmatched ')'" },
        { u"(?<=a)b", u"", "Lookbehind assertions are not supported" },
        { u"(?<!a)", u"u", "Lookbehind assertions are not supported" },
        { u"\\p{L}", u"u", "Unicode property escapes (\\p{...}) are not supported" },
        { u"\\P{L}", u"u", "Unicode property escapes (\\p{...}) are not supported" },
        { u"[\\p{L}]", u"u", "Unicode property escapes (\\p{...}) are not supported" },
        { u"[z-a]", u"", "Range out of order in character class" },
        { u"[z-a]", u"u", "Range out of order in character class" },
        { u"a{70000}", u"", "Quantifier bound too large (above 65535)" },
        { u"a{1,70000}", u"", "Quantifier bound too large (above 65535)" },
        { u"a{65536}", u"", "Quantifier bound too large (above 65535)" },
        { u"a{99999999999999999999}", u"", "Quantifier bound too large (above 65535)" },
        { u"a{2,1}", u"", "numbers out of order in {} quantifier" },
        { u"(?<n>a)(?<n>b)", u"", "Duplicate capture group name" },
        { u"(?:(?<a>x)|y)(?:z|(?<a>w))", u"", "Duplicate capture group name" }, // both can participate
        { u"(?<a>x)(?:y|(?<a>z))", u"", "Duplicate capture group name" },
        { u"(?:(?<a>x)|(?<a>y))(?<a>z)", u"", "Duplicate capture group name" },
        { u"(?<a>x)\\k<b>", u"", "Invalid named capture referenced" },
        { u"\\k<a>", u"u", "Invalid named capture referenced" },
        { u"(?<1>a)", u"", "Invalid capture group name" },
        { u"(?<>a)", u"", "Invalid capture group name" },
        { u"(?x)", u"", "Invalid group" },
        { u"(?", u"", "Invalid group" },
        { u"\\1", u"u", "Invalid escape" },
        { u"(a)\\2", u"u", "Invalid escape" },
        { u"\\a", u"u", "Invalid escape" },
        { u"\\-", u"u", "Invalid escape" },
        { u"\\x1", u"u", "Invalid escape" },
        { u"(?<a>x)[\\k]", u"", "Invalid escape" },
        { u"\\u{110000}", u"u", "Invalid Unicode escape" },
        { u"\\u{}", u"u", "Invalid Unicode escape" },
        { u"\\u12", u"u", "Invalid Unicode escape" },
        { u"]", u"u", "Lone quantifier brackets" },
        { u"{", u"u", "Lone quantifier brackets" },
        { u"a{", u"u", "Incomplete quantifier" },
        { u"[\\d-a]", u"u", "Invalid character class" },
        { u"\\c", u"u", "Invalid unicode escape" },
        { u"(?<a>x)\\k", u"", "Invalid named reference" },
        { u"\\01", u"u", "Invalid decimal escape" },
        { u"[\\1]", u"u", "Invalid class escape" },
        { u"\\", u"", "\\ at end of pattern" },
    };
    for (ErrorCase const& c : error_cases)
        run_error_case(c, __LINE__);

    // The error carries the offset of what went wrong.
    {
        Regex::CompileError error;
        CHECK(!Regex::compile(u"a)", RegexFlags {}, &error));
        CHECK_EQ(error.offset, std::size_t { 1 });
        CHECK(!Regex::compile(u"a**", RegexFlags {}, &error));
        CHECK_EQ(error.offset, std::size_t { 2 });
        CHECK(!Regex::compile(u"[z-a]", RegexFlags {}, &error));
        CHECK_EQ(error.offset, std::size_t { 1 });
        CHECK(!Regex::compile(u"x(?<=a)b", RegexFlags {}, &error));
        CHECK_EQ(error.offset, std::size_t { 1 });
    }

    // Nesting is capped: a pattern of a hundred and fifty nested groups is
    // refused before the parser has recursed anywhere near the stack.
    {
        std::u16string deep;
        for (int i = 0; i < 150; ++i)
            deep += u'(';
        deep += u'a';
        for (int i = 0; i < 150; ++i)
            deep += u')';
        Regex::CompileError error;
        CHECK(!Regex::compile(deep, RegexFlags {}, &error));
        CHECK_EQ(error.message, std::string("Regular expression too deeply nested"));
        // Fifty deep is fine.
        std::u16string shallow;
        for (int i = 0; i < 50; ++i)
            shallow += u'(';
        shallow += u'a';
        for (int i = 0; i < 50; ++i)
            shallow += u')';
        auto const regex = Regex::compile(shallow, RegexFlags {}, &error);
        CHECK(regex.has_value());
        CHECK_EQ(regex->group_count(), std::size_t { 50 });
        auto const match = regex->exec(u"a", 0);
        CHECK(match.has_value() && match->groups.size() == 51 && match->groups[50] == std::make_pair(std::size_t { 0 }, std::size_t { 1 }));
    }

    // Group bookkeeping: count, and names in group order.
    {
        auto const regex = Regex::compile(u"(?<year>\\d{4})-(?<month>\\d{2})(x)?", RegexFlags {});
        CHECK(regex.has_value());
        CHECK_EQ(regex->group_count(), std::size_t { 3 });
        auto const& names = regex->group_names();
        CHECK_EQ(names.size(), std::size_t { 2 });
        CHECK(names.size() == 2 && names[0].first == u"year" && names[0].second == 1);
        CHECK(names.size() == 2 && names[1].first == u"month" && names[1].second == 2);
        auto const mixed = Regex::compile(u"(a)(?<n>b)", RegexFlags {});
        CHECK(mixed.has_value());
        CHECK(mixed->group_names().size() == 1 && mixed->group_names()[0].first == u"n" && mixed->group_names()[0].second == 2);
        auto const plain = Regex::compile(u"(a)(b)", RegexFlags {});
        CHECK(plain.has_value() && plain->group_names().empty() && plain->group_count() == 2);
        // A shared name lists every group that carries it, in group order.
        auto const shared = Regex::compile(u"(?<a>x)|(?<a>y)|(?<b>z)", RegexFlags {});
        CHECK(shared.has_value());
        auto const& shared_names = shared->group_names();
        CHECK_EQ(shared_names.size(), std::size_t { 3 });
        CHECK(shared_names.size() == 3 && shared_names[0].first == u"a" && shared_names[0].second == 1
            && shared_names[1].first == u"a" && shared_names[1].second == 2
            && shared_names[2].first == u"b" && shared_names[2].second == 3);
    }

    // Flags: any order in, canonical order out, repeats and strangers refused.
    {
        auto const all = RegexFlags::parse(u"gimsuy");
        CHECK(all.has_value());
        CHECK(all->global && all->ignore_case && all->multiline && all->dot_all && all->unicode && all->sticky);
        CHECK(all->to_string() == u"gimsuy");
        auto const reordered = RegexFlags::parse(u"yg");
        CHECK(reordered.has_value() && reordered->to_string() == u"gy");
        auto const empty = RegexFlags::parse(u"");
        CHECK(empty.has_value() && empty->to_string().empty() && !empty->global && !empty->unicode);
        CHECK(!RegexFlags::parse(u"gg").has_value());
        CHECK(!RegexFlags::parse(u"x").has_value());
        CHECK(!RegexFlags::parse(u"gimsuyg").has_value());
        auto const regex = Regex::compile(u"a", *RegexFlags::parse(u"iy"));
        CHECK(regex.has_value());
        CHECK(regex->flags().ignore_case && regex->flags().sticky && !regex->flags().global);
        CHECK(regex->flags().to_string() == u"iy");
    }

    // The step budget: (a+)+b on a run of a's with no b is exponential;
    // with the budget lowered it gives up and says so, and a benign match
    // reports the flag clear.
    {
        CHECK_EQ(Regex::step_budget(), std::size_t { 20'000'000 });
        auto const regex = Regex::compile(u"(a+)+b", RegexFlags {});
        CHECK(regex.has_value());
        bool exhausted = false;
        auto const benign = regex->exec(u"xaaab", 0, &exhausted);
        CHECK(benign.has_value() && !exhausted);
        CHECK(benign.has_value() && benign->groups[0] == std::make_pair(std::size_t { 1 }, std::size_t { 5 }));
        Regex::set_step_budget(100'000);
        CHECK_EQ(Regex::step_budget(), std::size_t { 100'000 });
        std::u16string const hostile(30, u'a');
        auto const attack = regex->exec(hostile, 0, &exhausted);
        CHECK(!attack.has_value());
        CHECK(exhausted);
        // Without the flag pointer the call is still safe.
        CHECK(!regex->exec(hostile, 0).has_value());
        Regex::set_step_budget(20'000'000);
        // A short input finishes inside the default budget with a definite no.
        auto const small = regex->exec(u"aaaa", 0, &exhausted);
        CHECK(!small.has_value() && !exhausted);
    }

    // A moved-from or default Regex answers without a program.
    {
        Regex empty;
        CHECK(!empty.exec(u"a", 0).has_value());
        CHECK_EQ(empty.group_count(), std::size_t { 0 });
        CHECK(empty.group_names().empty());
        auto compiled = Regex::compile(u"(a)", RegexFlags {});
        CHECK(compiled.has_value());
        Regex moved = std::move(*compiled);
        CHECK(moved.exec(u"a", 0).has_value());
        CHECK_EQ(moved.group_count(), std::size_t { 1 });
    }

    // Review cases: the behaviours a backtracking engine most often gets
    // wrong, each expectation derived from the algorithms of §22.2.2 (and
    // Annex B.1.2 outside `u`) and cross-checked against V8 where the two
    // agree. run_match_case also insists the budget was not exhausted, so
    // the termination cases prove the empty-iteration rule rather than the
    // step limit.
    std::vector<MatchCase> const review_cases {
        // The empty check (§22.2.2.3.1 step 2.b): an iteration past the
        // minimum that consumes nothing fails, so these finish with a
        // definite answer. Sixteen a's keep (a*)*b, which is exponential in
        // any backtracker, well inside the default budget.
        { u"(a*)*b", u"", u"aaaaaaaaaaaaaaaac", 0, {} },
        { u"(a?)*b", u"", u"aaaaaaaaaaaaaaaaaaaaaaaac", 0, {} },
        { u"(a*)*b", u"", u"aab", 0, { { 0, 3 }, { 0, 2 } } },
        { u"(a?)*b", u"", u"aab", 0, { { 0, 3 }, { 1, 2 } } },
        { u"(a*|b)*c", u"", u"aabbabc", 0, { { 0, 7 }, { 5, 6 } } },
        // Captures reset at every iteration (§22.2.2.3.1 step 4): the
        // group that matched in an earlier iteration is undefined after.
        { u"((a)|b)+", u"", u"ab", 0, { { 0, 2 }, { 1, 2 }, none } },
        { u"(?:(a)|b)+", u"", u"ab", 0, { { 0, 2 }, none } },
        { u"(?:(a)|b)+\\1", u"", u"abb", 0, { { 0, 3 }, none } },
        // Lazy quantifiers take the fewest iterations that let the rest match.
        { u"(?:ab)+?", u"", u"ababab", 0, { { 0, 2 } } },
        { u"<(.+?)>", u"", u"<a><bc>", 0, { { 0, 3 }, { 1, 2 } } },
        { u"a{2,}?", u"", u"aaaa", 0, { { 0, 2 } } },
        // A backreference inside its own group sees the group still
        // undefined (§22.2.2.7.2: an undefined capture matches empty).
        { u"(a\\1)", u"", u"aa", 0, { { 0, 1 }, { 0, 1 } } },
        { u"(a\\1)+", u"", u"aaa", 0, { { 0, 3 }, { 2, 3 } } },
        { u"(?<n>a\\k<n>)", u"", u"aa", 0, { { 0, 1 }, { 0, 1 } } },
        // \b at the edges of the input next to non-word characters.
        { u"\\b", u"", u" ", 0, {} },
        { u"\\b", u"", u"a", 1, { { 1, 1 } } },
        { u"\\B", u"", u"!", 0, { { 0, 0 } } },
        { u"\\bb", u"", u"a b", 0, { { 2, 3 } } },
        { u"\\b$", u"", u"a!", 0, {} },
        // Canonicalize (§22.2.2.7.3): without `u` a non-ASCII character
        // whose uppercase is ASCII keeps itself, so KELVIN SIGN matches
        // neither [a-z] nor k; with `u` simple case folding takes it to k.
        { u"[a-z]", u"i", u"\u212A", 0, {} },
        { u"[a-z]", u"iu", u"\u212A", 0, { { 0, 1 } } },
        { u"\\W", u"i", u"\u212A", 0, { { 0, 1 } } },
        { u"\\W", u"iu", u"\u212A", 0, {} },
        { u"[^a-z]", u"i", u"K", 0, {} },
        { u"\u03C3", u"i", u"\u03C2", 0, { { 0, 1 } } }, // σ and ς share the uppercase Σ
        { u"\u2126", u"i", u"\u03C9", 0, {} }, // OHM SIGN uppercases to itself, ω to Ω
        { u"\u2126", u"iu", u"\u03C9", 0, { { 0, 1 } } }, // but folds to ω
        { u"[\\u{10400}-\\u{10427}]", u"iu", u"\U00010428", 0, { { 0, 2 } } },
        { u"[\u13A0]", u"iu", u"\uAB70", 0, { { 0, 1 } } }, // Cherokee folds upward
        { u"\u0131", u"i", u"I", 0, {} }, // dotless i uppercases to ASCII: keeps itself
        { u"I", u"iu", u"\u0131", 0, {} }, // and has no simple folding
        // Multiline ^ after each LineTerminator (§22.2.2.4).
        { u"^b", u"m", u"a\u2028b", 0, { { 2, 3 } } },
        { u"^b", u"m", u"a\u2029b", 0, { { 2, 3 } } },
        { u"^b", u"m", u"a\r\nb", 0, { { 3, 4 } } },
        { u"^", u"m", u"a\n", 2, { { 2, 2 } } },
        { u"^b", u"", u"a\u2028b", 0, {} },
        // Sticky tries only the start position (§22.2.7.2 step 13.d.i).
        { u"a", u"y", u"ba", 1, { { 1, 2 } } },
        { u"a", u"y", u"ba", 0, {} },
        { u"\\b", u"y", u"a b", 1, { { 1, 1 } } },
        { u".", u"uy", u"\U0001F600", 1, { { 0, 2 } } },
        // . never matches LS or PS outside dotAll.
        { u".", u"", u"\u2028", 0, {} },
        { u"a.b", u"", u"a\u2028b", 0, {} },
        { u".", u"s", u"\u2028", 0, { { 0, 1 } } },
        // Under `u` a surrogate pair is one character everywhere; a lone
        // surrogate in the input is one character too.
        { u"^[\U0001F600]$", u"u", u"\U0001F600", 0, { { 0, 2 } } },
        { u"^[\U0001F600]$", u"u", u"\xD83D", 0, {} },
        { u"^.{2}$", u"u", u"\U0001F600\U0001F600", 0, { { 0, 4 } } },
        { u"^.{2}$", u"", u"\U0001F600\U0001F600", 0, {} },
        { u"^.{2}$", u"", u"\U0001F600", 0, { { 0, 2 } } },
        { u"\\u{1F600}", u"u", u"x\U0001F600", 0, { { 1, 3 } } },
        { u"^..$", u"u", u"\xD83D" u"a", 0, { { 0, 2 } } },
        { u"^..$", u"u", u"\xDE00\xD83D", 0, { { 0, 2 } } },
        { u"[\\uD800-\\uDFFF]", u"u", u"\U0001F600", 0, {} },
        { u"[\\uD800-\\uDFFF]", u"u", u"\xD83D", 0, { { 0, 1 } } },
        { u"(.)\\1", u"iu", u"\U00010400\U00010428", 0, { { 0, 4 }, { 0, 2 } } },
        // Annex B.1.2 outside `u`: \u{ is the letter u and a brace that
        // opens no quantifier, } and ] are themselves, {,5} is four literals.
        { u"\\u{2}", u"", u"uu", 0, { { 0, 2 } } }, // the brace quantifies the u
        { u"\\u{", u"", u"u{", 0, { { 0, 2 } } },
        { u"x{,5}", u"", u"x{,5}", 0, { { 0, 5 } } },
        { u"{,5}", u"", u"{,5}", 0, { { 0, 4 } } },
        { u"x{1}}", u"", u"x}", 0, { { 0, 2 } } },
        { u"{a}", u"", u"{a}", 0, { { 0, 3 } } },
        { u"x{ 1}", u"", u"x{ 1}", 0, { { 0, 5 } } },
        // Annex B.1.2 QuantifiableAssertion: a quantified lookahead runs
        // through RepeatMatcher, so a zero-width iteration past the
        // minimum fails and its captures are discarded, while iterations
        // inside the minimum keep them.
        { u"(?=(a)){2}", u"", u"a", 0, { { 0, 0 }, { 0, 1 } } },
        { u"(?=(a)){1}", u"", u"a", 0, { { 0, 0 }, { 0, 1 } } },
        { u"(?=(a))*", u"", u"a", 0, { { 0, 0 }, none } },
        { u"(?=(a))?", u"", u"a", 0, { { 0, 0 }, none } },
        { u"(?!b)+a", u"", u"a", 0, { { 0, 1 } } },
        // Nested lookahead: captures of a positive body stay visible
        // after it, those of a negative body never are (§22.2.2.4).
        { u"(?=(a)(?=(b)))ab", u"", u"ab", 0, { { 0, 2 }, { 0, 1 }, { 1, 2 } } },
        { u"(?=(?=(a))a)a", u"", u"a", 0, { { 0, 1 }, { 0, 1 } } },
        { u"(?=(a)b)(?!(a)c)a", u"", u"ab", 0, { { 0, 1 }, { 0, 1 }, none } },
        { u"(?!(?!(a)))a", u"", u"a", 0, { { 0, 1 }, none } },
        { u"(?=(a))\\1b", u"", u"ab", 0, { { 0, 2 }, { 0, 1 } } },
        { u"(?=a*(b))a*c", u"", u"aab", 0, {} }, // atomic: a* is not retried
        // Starting at the end of the input still tries the empty match
        // there; past the end nothing is tried.
        { u"$", u"", u"ab", 2, { { 2, 2 } } },
        { u"\\b", u"", u"ab", 2, { { 2, 2 } } },
        { u"a", u"", u"ab", 3, {} },
        { u"$", u"", u"ab", 3, {} },
        // A real U+0000 after the dash still opens a range.
        { std::u16string_view(u"[\0-a]", 5), u"", u"-", 0, { { 0, 1 } } },
    };
    for (MatchCase const& c : review_cases)
        run_match_case(c, __LINE__);

    std::vector<ErrorCase> const review_errors {
        // Under `u` no assertion is quantifiable (§22.2.1 Term[+U]).
        { u"(?=a)+", u"u", "Nothing to repeat" },
        { u"(?=a)?", u"u", "Nothing to repeat" },
        { u"(?!a)*", u"u", "Nothing to repeat" },
        // Outside `u` only lookahead is (Annex B.1.2 QuantifiableAssertion).
        { u"$+", u"", "Nothing to repeat" },
        { u"\\B{2}", u"", "Nothing to repeat" },
        { u"\\b*", u"u", "Nothing to repeat" },
        // A real U+0000 after the dash opens a range, out of order here.
        { std::u16string_view(u"[a-\0]", 5), u"", "Range out of order in character class" },
        { std::u16string_view(u"[a-\0]", 5), u"u", "Range out of order in character class" },
    };
    for (ErrorCase const& c : review_errors)
        run_error_case(c, __LINE__);

    // Group bookkeeping through quantified and asserted groups.
    {
        auto const quantified = Regex::compile(u"((a)|b)+", RegexFlags {});
        CHECK(quantified.has_value() && quantified->group_count() == 2 && quantified->group_names().empty());
        auto const asserted = Regex::compile(u"(?:a)(?=(b))", RegexFlags {});
        CHECK(asserted.has_value() && asserted->group_count() == 1);
        auto const shared = Regex::compile(u"(?<a>x)|(?<a>y)", RegexFlags {});
        CHECK(shared.has_value() && shared->group_count() == 2);
        CHECK(shared.has_value() && shared->group_names().size() == 2 && shared->group_names()[0].first == u"a"
            && shared->group_names()[0].second == 1 && shared->group_names()[1].second == 2);
    }

    // A tiny budget is exhausted by a plain linear scan, reported through
    // the flag with a nullopt result, and a later exec with the budget
    // restored is unaffected.
    {
        auto const regex = Regex::compile(u"a*b", RegexFlags {});
        CHECK(regex.has_value());
        std::u16string const run(200, u'a');
        Regex::set_step_budget(50);
        bool exhausted = false;
        CHECK(!regex->exec(run, 0, &exhausted).has_value());
        CHECK(exhausted);
        Regex::set_step_budget(20'000'000);
        CHECK(!regex->exec(run, 0, &exhausted).has_value());
        CHECK(!exhausted);
        auto const found = regex->exec(run + u"b", 0, &exhausted);
        CHECK(found.has_value() && !exhausted && found->groups[0] == std::make_pair(std::size_t { 0 }, std::size_t { 201 }));
    }

    return sashfold::test::report("js_regex");
}
