#pragma once

// What the syllable-based shapers (Indic, the Universal Shaping Engine,
// Khmer, Myanmar) share: a small pattern language over character
// categories that their specifications' cluster grammars are written in,
// matched longest-first the way the grammars are meant; syllable numbering
// for the per-syllable features; the dotted circle a broken cluster gets;
// and the question which forms a font has (would a feature substitute
// these glyphs). Not for use outside src/text.

#include "text/ShaperInternal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::text::shaping {

// A cluster grammar's rule, compiled to a nondeterministic automaton
// (Thompson's construction) over category numbers below 64. The pattern
// is written with category names, whitespace for sequence, `|` for
// alternatives, `( )` for grouping, the postfix `?`, `*` and `+`, and
// `[A B C]` for one of several categories.
class CategoryPattern {
public:
    using Names = std::span<std::pair<std::string_view, std::uint8_t> const>;
    CategoryPattern(std::string_view pattern, Names names);

    // The length of the longest non-empty match at `start` of the
    // categories `category(i)` for i in [start, end), or 0.
    template<typename Category>
    std::size_t longest(std::size_t start, std::size_t end, Category const& category) const
    {
        thread_local Lists lists;
        if (lists.mark.size() < m_states.size())
            lists.mark.resize(m_states.size(), 0);
        lists.clear_current();
        add(lists, lists.current, m_start);
        std::size_t best = 0;
        for (std::size_t i = start; i < end && !lists.current.empty(); ++i) {
            std::uint64_t const bit = 1ull << (category(i) & 63);
            lists.clear_next();
            for (int s : lists.current) {
                State const& state = m_states[static_cast<std::size_t>(s)];
                if (state.kind == Kind::Symbol && (state.set & bit))
                    add(lists, lists.next, state.out1);
            }
            lists.swap();
            if (lists.accepting)
                best = i + 1 - start;
        }
        return best;
    }

private:
    enum class Kind : std::uint8_t {
        Symbol,
        Split,
        Accept,
    };
    struct State {
        Kind kind = Kind::Accept;
        std::uint64_t set = 0;
        int out1 = -1;
        int out2 = -1;
    };
    struct Lists {
        std::vector<int> current;
        std::vector<int> next;
        std::vector<unsigned> mark;
        unsigned generation = 1;
        bool accepting = false;
        bool next_accepting = false;
        void clear_current()
        {
            current.clear();
            ++generation;
            accepting = false;
        }
        void clear_next()
        {
            next.clear();
            ++generation;
            next_accepting = false;
        }
        void swap()
        {
            current.swap(next);
            accepting = next_accepting;
        }
    };
    void add(Lists& lists, std::vector<int>& list, int s) const
    {
        // The epsilon closure, with an explicit stack: a split's outs.
        // A state is pushed once per generation, so the stack never holds
        // more than all of them.
        constexpr std::size_t stack_size = 512;
        int stack[stack_size];
        std::size_t depth = 0;
        stack[depth++] = s;
        while (depth) {
            int const k = stack[--depth];
            if (k < 0 || lists.mark[static_cast<std::size_t>(k)] == lists.generation)
                continue;
            lists.mark[static_cast<std::size_t>(k)] = lists.generation;
            State const& state = m_states[static_cast<std::size_t>(k)];
            if (state.kind == Kind::Split) {
                if (depth + 2 <= stack_size) {
                    stack[depth++] = state.out2;
                    stack[depth++] = state.out1;
                }
            } else if (state.kind == Kind::Accept) {
                if (&list == &lists.current)
                    lists.accepting = true;
                else
                    lists.next_accepting = true;
            } else {
                list.push_back(k);
            }
        }
    }

    struct Fragment {
        int start = -1;
        std::vector<std::pair<int, int>> outs; // (state, 1 or 2) still to be connected
    };
    struct Parser;
    std::vector<State> m_states;
    int m_start = -1;
};

// Numbers syllables for the per-syllable features: the low four bits the
// syllable's type, the high four a serial that differs between neighbors
// (never zero, so that every syllable restricts its lookups).
inline std::uint8_t syllable_value(unsigned serial, unsigned type)
{
    return static_cast<std::uint8_t>(((serial % 15 + 1) << 4) | (type & 0x0F));
}
inline unsigned syllable_type(GlyphInfo const& g) { return g.syllable & 0x0F; }

// The end of the syllable that starts at `start`.
inline std::size_t syllable_end(Buffer const& b, std::size_t start)
{
    std::size_t end = start + 1;
    while (end < b.len() && b.info[end].syllable == b.info[start].syllable)
        ++end;
    return end;
}

// Splits the buffer into syllables by trying each rule at each position and
// taking the longest match (the first rule listed wins a tie); where no rule
// matches, one character is a syllable of type `other`. `category` reads a
// glyph's category. The grammars allow syllables of any length; one longer
// than `max_syllable_length` (no writing system has one) is cut there, so
// that hostile text cannot make the reordering of one syllable quadratic in
// the length of the text.
inline constexpr std::size_t max_syllable_length = 256;
struct SyllableRule {
    CategoryPattern const* pattern;
    unsigned type;
};
template<typename Category>
void find_syllables(Buffer& b, std::span<SyllableRule const> rules, unsigned other, Category const& category)
{
    unsigned serial = 0;
    std::size_t const count = b.len();
    auto category_at = [&](std::size_t i) { return category(b.info[i]); };
    for (std::size_t i = 0; i < count;) {
        std::size_t best = 0;
        unsigned type = other;
        std::size_t const limit = std::min(count, i + max_syllable_length);
        for (SyllableRule const& rule : rules) {
            std::size_t const length = rule.pattern->longest(i, limit, category_at);
            if (length > best) {
                best = length;
                type = rule.type;
            }
        }
        if (best == 0)
            best = 1;
        std::uint8_t const value = syllable_value(serial++, type);
        for (std::size_t k = i; k < i + best; ++k)
            b.info[k].syllable = value;
        i += best;
    }
}

// Puts a dotted circle (U+25CC) at the start of every syllable of type
// `broken` that does not start with a repha (`is_repha` says which glyph
// is one; the circle then goes after it), so that its marks have a base to
// sit on — when the font has the circle. The circle takes the category
// `circle_category` and `circle_position` (written into GlyphInfo::shaper
// and shaper2), and the cluster and features of what it stands before.
template<typename IsRepha>
void insert_dotted_circles(ShapeFont const& font, Buffer& b, unsigned broken, std::uint8_t circle_category,
    std::uint8_t circle_position, std::uint32_t mask, IsRepha const& is_repha)
{
    bool any = false;
    for (GlyphInfo const& g : b.info)
        any = any || syllable_type(g) == broken;
    if (!any)
        return;
    std::uint32_t const glyph = font.nominal_glyph(0x25CC);
    if (!glyph)
        return;
    GlyphInfo circle;
    circle.codepoint = 0x25CC;
    set_unicode_props(circle);
    circle.glyph = glyph;
    circle.shaper = circle_category;
    circle.shaper2 = circle_position;
    circle.mask = mask;
    circle.props = font.tables.gdef.has_glyph_classes() ? font.tables.gdef.props_of(glyph) : otl::glyph_props::base_glyph;
    b.clear_output();
    b.idx = 0;
    std::uint8_t last = 0;
    while (b.idx < b.len() && !b.failed) {
        std::uint8_t const syllable = b.cur().syllable;
        if (syllable != last && syllable_type(b.cur()) == broken && b.output_allowed()) {
            last = syllable;
            while (b.idx < b.len() && b.cur().syllable == syllable && is_repha(b.cur()))
                b.next_glyph();
            GlyphInfo g = circle;
            g.syllable = syllable;
            if (b.idx < b.len()) {
                g.cluster = b.cur().cluster;
                g.mask = b.cur().mask;
            } else if (!b.out.empty()) {
                g.cluster = b.out.back().cluster;
            }
            b.out.push_back(g);
            continue;
        }
        last = syllable;
        b.next_glyph();
    }
    b.sync();
}

// Draws a dotted circle into every sequence of the script the
// specifications prohibit (an independent vowel followed by a dependent
// vowel that together look like another independent vowel:
// VowelConstraintsData.h), before the sequence's last character, which it
// takes the cluster and features of — when the font has the circle. For
// the text before normalization (a shaper's preprocess_text).
void insert_vowel_constraint_circles(ScriptCode, ShapeFont const&, Buffer&);

// The lookups of one feature as the plan selected it (its language
// system's feature, with the feature variations at the default instance),
// to ask whether they substitute exactly some glyphs with no context: the
// font has the form the feature makes of them.
struct FeatureLookups {
    FeatureLookups() = default;
    FeatureLookups(Plan const&, ShapeFont const&, std::uint32_t tag);
    bool would_substitute(ShapeFont const&, std::span<std::uint32_t const> glyphs) const;
    bool empty() const { return lookups.empty(); }
    std::vector<std::uint16_t> lookups;
};

}
