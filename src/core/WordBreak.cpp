#include "core/WordBreak.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace sashfold {

namespace {

WordBreakRange const* range_of(char32_t c)
{
    auto const it = std::upper_bound(std::begin(word_break_ranges), std::end(word_break_ranges), c,
        [](char32_t value, WordBreakRange const& range) { return value < range.first; });
    if (it == std::begin(word_break_ranges))
        return nullptr;
    WordBreakRange const& range = *std::prev(it);
    return c <= range.last ? &range : nullptr;
}

using C = WordBreakClass;

bool newline(C c) { return c == C::Newline || c == C::CR || c == C::LF; }
bool ignorable(C c) { return c == C::Extend || c == C::Format || c == C::ZWJ; }
bool ah_letter(C c) { return c == C::ALetter || c == C::Hebrew_Letter; }
bool mid_num_let_q(C c) { return c == C::MidNumLet || c == C::Single_Quote; }

}

WordBreakClass word_break_class(char32_t code_point)
{
    WordBreakRange const* const range = range_of(code_point);
    return range ? range->klass : C::Other;
}

bool is_extended_pictographic(char32_t code_point)
{
    WordBreakRange const* const range = range_of(code_point);
    return range && (range->flags & word_break_pictographic) != 0;
}

std::vector<bool> word_boundaries(std::u32string_view text)
{
    std::size_t const n = text.size();
    std::vector<bool> breaks(n + 1, false);
    if (n == 0) {
        breaks[0] = true;
        return breaks;
    }
    breaks[0] = true; // WB1
    breaks[n] = true; // WB2
    std::vector<C> classes(n);
    for (std::size_t i = 0; i < n; ++i)
        classes[i] = word_break_class(text[i]);
    // WB4: the character a run of Extend, Format and ZWJ follows stands for
    // the run — but not after a line break, which they do not attach to.
    // `base[i]` is the index of the character standing for i.
    std::vector<std::size_t> base(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0 && ignorable(classes[i]) && !newline(classes[base[i - 1]]))
            base[i] = base[i - 1];
        else
            base[i] = i;
    }
    // The effective character before a boundary at i, and the one before
    // that; the effective one after (skipping what WB4 ignores).
    auto const before = [&](std::size_t i) -> std::ptrdiff_t { return static_cast<std::ptrdiff_t>(base[i - 1]); };
    auto const previous_of = [&](std::ptrdiff_t at) -> std::ptrdiff_t { return at > 0 ? static_cast<std::ptrdiff_t>(base[static_cast<std::size_t>(at) - 1]) : -1; };
    auto const next_after = [&](std::size_t i) -> std::ptrdiff_t {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!ignorable(classes[j]))
                return static_cast<std::ptrdiff_t>(j);
        }
        return -1;
    };
    auto const cls = [&](std::ptrdiff_t at) { return at < 0 ? C::Other : classes[static_cast<std::size_t>(at)]; };
    for (std::size_t i = 1; i < n; ++i) {
        C const a = classes[i - 1];
        C const b = classes[i];
        if (a == C::CR && b == C::LF) // WB3
            continue;
        if (newline(a) || newline(b)) { // WB3a, WB3b
            breaks[i] = true;
            continue;
        }
        if (a == C::ZWJ && is_extended_pictographic(text[i])) // WB3c
            continue;
        if (a == C::WSegSpace && b == C::WSegSpace) // WB3d
            continue;
        if (ignorable(b)) // WB4
            continue;
        std::ptrdiff_t const p = before(i);
        C const left = cls(p);
        C const left2 = cls(previous_of(p));
        C const right = b;
        C const right2 = cls(next_after(i));
        bool keep = false;
        if (ah_letter(left) && ah_letter(right)) // WB5
            keep = true;
        else if (ah_letter(left) && (right == C::MidLetter || mid_num_let_q(right)) && ah_letter(right2)) // WB6
            keep = true;
        else if (ah_letter(left2) && (left == C::MidLetter || mid_num_let_q(left)) && ah_letter(right)) // WB7
            keep = true;
        else if (left == C::Hebrew_Letter && right == C::Single_Quote) // WB7a
            keep = true;
        else if (left == C::Hebrew_Letter && right == C::Double_Quote && right2 == C::Hebrew_Letter) // WB7b
            keep = true;
        else if (left2 == C::Hebrew_Letter && left == C::Double_Quote && right == C::Hebrew_Letter) // WB7c
            keep = true;
        else if (left == C::Numeric && right == C::Numeric) // WB8
            keep = true;
        else if (ah_letter(left) && right == C::Numeric) // WB9
            keep = true;
        else if (left == C::Numeric && ah_letter(right)) // WB10
            keep = true;
        else if (left2 == C::Numeric && (left == C::MidNum || mid_num_let_q(left)) && right == C::Numeric) // WB11
            keep = true;
        else if (left == C::Numeric && (right == C::MidNum || mid_num_let_q(right)) && right2 == C::Numeric) // WB12
            keep = true;
        else if (left == C::Katakana && right == C::Katakana) // WB13
            keep = true;
        else if ((ah_letter(left) || left == C::Numeric || left == C::Katakana || left == C::ExtendNumLet) && right == C::ExtendNumLet) // WB13a
            keep = true;
        else if (left == C::ExtendNumLet && (ah_letter(right) || right == C::Numeric || right == C::Katakana)) // WB13b
            keep = true;
        else if (left == C::Regional_Indicator && right == C::Regional_Indicator) {
            // WB15, WB16: flags pair up — no break inside a pair.
            std::size_t count = 0;
            for (std::ptrdiff_t at = p; at >= 0 && cls(at) == C::Regional_Indicator; at = previous_of(at))
                ++count;
            keep = count % 2 == 1;
        }
        breaks[i] = !keep; // WB999
    }
    return breaks;
}

}
