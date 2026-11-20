#include "text/ShaperSyllabic.h"

#include "text/VowelConstraintsData.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace sashfold::text::shaping {

// A recursive-descent parser of the pattern into Thompson fragments:
//   alternation := sequence ('|' sequence)*
//   sequence    := repeat+
//   repeat      := atom ('?' | '*' | '+')*
//   atom        := name | '[' name+ ']' | '(' alternation ')'
// The patterns are the shapers' own constants: a malformed one is a
// programming error and stops the program at its first use.
struct CategoryPattern::Parser {
    CategoryPattern& p;
    std::string_view text;
    Names names;
    std::size_t at = 0;

    [[noreturn]] static void fail() { std::abort(); }

    void skip_space()
    {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
    }
    int new_state(Kind kind, std::uint64_t set = 0)
    {
        State s;
        s.kind = kind;
        s.set = set;
        p.m_states.push_back(s);
        return static_cast<int>(p.m_states.size() - 1);
    }
    void patch(std::vector<std::pair<int, int>> const& outs, int to)
    {
        for (auto [state, which] : outs) {
            if (which == 1)
                p.m_states[static_cast<std::size_t>(state)].out1 = to;
            else
                p.m_states[static_cast<std::size_t>(state)].out2 = to;
        }
    }
    std::uint64_t name()
    {
        std::size_t const begin = at;
        while (at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_'))
            ++at;
        std::string_view const word = text.substr(begin, at - begin);
        for (auto const& [n, value] : names) {
            if (n == word)
                return 1ull << (value & 63);
        }
        fail();
    }
    Fragment atom()
    {
        skip_space();
        if (at >= text.size())
            fail();
        if (text[at] == '(') {
            ++at;
            Fragment f = alternation();
            skip_space();
            if (at >= text.size() || text[at] != ')')
                fail();
            ++at;
            return f;
        }
        std::uint64_t set = 0;
        if (text[at] == '[') {
            ++at;
            for (;;) {
                skip_space();
                if (at < text.size() && text[at] == ']') {
                    ++at;
                    break;
                }
                set |= name();
            }
        } else {
            set = name();
        }
        int const s = new_state(Kind::Symbol, set);
        return Fragment { s, { { s, 1 } } };
    }
    Fragment repeat()
    {
        Fragment f = atom();
        for (;;) {
            skip_space();
            if (at >= text.size())
                return f;
            char const op = text[at];
            if (op != '?' && op != '*' && op != '+')
                return f;
            ++at;
            int const split = new_state(Kind::Split);
            p.m_states[static_cast<std::size_t>(split)].out1 = f.start;
            if (op == '?') {
                f.outs.push_back({ split, 2 });
                f.start = split;
            } else if (op == '*') {
                patch(f.outs, split);
                f = Fragment { split, { { split, 2 } } };
            } else {
                patch(f.outs, split);
                f.outs = { { split, 2 } };
            }
        }
    }
    Fragment sequence()
    {
        Fragment f = repeat();
        for (;;) {
            skip_space();
            if (at >= text.size() || text[at] == '|' || text[at] == ')')
                return f;
            Fragment next = repeat();
            patch(f.outs, next.start);
            f.outs = std::move(next.outs);
        }
    }
    Fragment alternation()
    {
        Fragment f = sequence();
        for (;;) {
            skip_space();
            if (at >= text.size() || text[at] != '|')
                return f;
            ++at;
            Fragment other = sequence();
            int const split = new_state(Kind::Split);
            p.m_states[static_cast<std::size_t>(split)].out1 = f.start;
            p.m_states[static_cast<std::size_t>(split)].out2 = other.start;
            f.start = split;
            f.outs.insert(f.outs.end(), other.outs.begin(), other.outs.end());
        }
    }
};

CategoryPattern::CategoryPattern(std::string_view pattern, Names names)
{
    Parser parser { *this, pattern, names };
    Fragment f = parser.alternation();
    parser.skip_space();
    if (parser.at != pattern.size())
        Parser::fail();
    int const accept = parser.new_state(Kind::Accept);
    parser.patch(f.outs, accept);
    m_start = f.start;
}

void insert_vowel_constraint_circles(ScriptCode script, ShapeFont const& font, Buffer& b)
{
    using vowel_constraints::sequences;
    auto const first_of = [](char32_t c) {
        return std::lower_bound(sequences.begin(), sequences.end(), c,
            [](std::array<char32_t, 3> const& s, char32_t v) { return s[0] < v; });
    };
    auto const starts_one = [&](char32_t c) {
        auto const it = first_of(c);
        return it != sequences.end() && (*it)[0] == c;
    };
    bool any = false;
    for (GlyphInfo const& g : b.info)
        any = any || starts_one(g.codepoint);
    if (!any || !font.has_glyph(0x25CC))
        return;
    b.clear_output();
    b.idx = 0;
    while (b.idx < b.len() && !b.failed) {
        char32_t const c = b.cur().codepoint;
        // The shortest prohibited sequence that starts here.
        std::size_t matched = 0;
        if (starts_one(c) && script_of(c) == script) {
            for (auto it = first_of(c); it != sequences.end() && (*it)[0] == c; ++it) {
                std::size_t const length = (*it)[2] ? 3 : 2;
                if (b.idx + length > b.len() || (matched && length >= matched))
                    continue;
                bool same = true;
                for (std::size_t k = 1; k < length; ++k)
                    same = same && b.info[b.idx + k].codepoint == (*it)[k];
                if (same)
                    matched = length;
            }
        }
        if (!matched || !b.output_allowed()) {
            b.next_glyph();
            continue;
        }
        for (std::size_t k = 0; k + 1 < matched; ++k)
            b.next_glyph();
        // The last character can start another sequence: it is not consumed.
        GlyphInfo circle = b.cur();
        circle.codepoint = 0x25CC;
        set_unicode_props(circle);
        b.out.push_back(circle);
    }
    b.sync();
}

FeatureLookups::FeatureLookups(Plan const& plan, ShapeFont const& font, std::uint32_t tag)
{
    FeatureMap const* f = plan.feature(tag);
    if (!f || f->index[0] < 0)
        return;
    otl::LayoutTable const& gsub = font.tables.gsub;
    lookups = gsub.feature_lookups(static_cast<std::uint16_t>(f->index[0]), gsub.feature_variation_at_default());
}

bool FeatureLookups::would_substitute(ShapeFont const& font, std::span<std::uint32_t const> glyphs) const
{
    for (std::uint16_t index : lookups) {
        if (otl::would_substitute(font.tables, index, glyphs))
            return true;
    }
    return false;
}

}
