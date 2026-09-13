#include "core/LineBreak.h"

#include <algorithm>
#include <iterator>
#include <span>

namespace sashfold {

namespace {

using C = LineBreakClass;

LineBreakRange const* range_of(char32_t code_point)
{
    auto const it = std::upper_bound(std::begin(line_break_ranges), std::end(line_break_ranges), code_point,
        [](char32_t c, LineBreakRange const& range) { return c < range.first; });
    if (it == std::begin(line_break_ranges))
        return nullptr;
    LineBreakRange const& range = *std::prev(it);
    return code_point >= range.first && code_point <= range.last ? &range : nullptr;
}

constexpr char32_t dotted_circle = 0x25CC;

// What a Southeast Asian letter is to the line breaker when a line may
// end between clusters (see line_break_tailor_clusters).
C southeast_asian_cluster_class(char32_t c)
{
    bool const leading_vowel = (c >= 0x0E40 && c <= 0x0E44) || (c >= 0x0EC0 && c <= 0x0EC4);
    bool const following = c == 0x0E30 || c == 0x0E32 || c == 0x0E33 || c == 0x0E45 || c == 0x0E46
        || c == 0x0EB0 || c == 0x0EB2 || c == 0x0EB3 || c == 0x0EC6;
    if (leading_vowel)
        return C::BB;
    if (following)
        return C::CM;
    return C::ID;
}

// The text as the rules see it: each character's class after LB1, LB9
// and LB10, with the combining marks and joiners that LB9 folds into the
// character before them marked, so the rules step over them; and each
// character's flags, a folded mark wearing its base's.
struct Prepared {
    std::vector<C> klass;
    std::vector<std::uint8_t> flags;
    std::vector<char32_t> code_point;
    std::vector<bool> absorbed; // an LB9 mark: part of the character before it
    std::vector<C> raw; // the class before LB9 and LB10, for LB8a
    std::span<std::uint8_t const> tailor; // CSS's flags per character, or nothing

    std::uint8_t tailoring(std::size_t i) const { return tailor.empty() ? 0 : tailor[i]; }
    bool east_asian(std::size_t i) const { return (flags[i] & line_break_east_asian) != 0; }
    bool initial_quote(std::size_t i) const { return (flags[i] & line_break_initial_quote) != 0; }
    bool final_quote(std::size_t i) const { return (flags[i] & line_break_final_quote) != 0; }
    bool unassigned_pictographic(std::size_t i) const { return (flags[i] & line_break_unassigned_pictographic) != 0; }
    // AK, AS, or the dotted circle: what LB28a calls a base of a Brahmic
    // conjunct.
    bool aksara_like(std::size_t i) const
    {
        return klass[i] == C::AK || klass[i] == C::AS || code_point[i] == dotted_circle;
    }
};

Prepared prepare(std::u32string_view text, std::span<std::uint8_t const> tailoring)
{
    Prepared p;
    p.tailor = tailoring;
    p.klass.reserve(text.size());
    p.flags.reserve(text.size());
    p.code_point.assign(text.begin(), text.end());
    p.absorbed.assign(text.size(), false);
    p.raw.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        C klass = line_break_class(text[i]);
        std::uint8_t const tailor = p.tailoring(i);
        // CJ, which LB1 leaves to the style: NS under strict, and by
        // default; ID under loose and normal.
        if (klass == C::CJ)
            klass = (tailor & line_break_tailor_normal) ? C::ID : C::NS;
        // SA, which LB1 takes as AL where a dictionary finds the words.
        // Without one a line may end between clusters instead: a letter is
        // ID; a leading vowel (Thai, Lao) stays with the letter after it,
        // a vowel written after its letter, or a repetition mark, with the
        // one before; a virama or coeng (Myanmar, Khmer) holds both sides.
        if (klass == C::SA)
            klass = (tailor & line_break_tailor_clusters) ? southeast_asian_cluster_class(text[i]) : C::AL;
        if ((tailor & line_break_tailor_clusters) && (text[i] == 0x1039 || text[i] == 0x17D2))
            klass = C::GL;
        // word-break: break-all — a letter or a digit breaks like an ideograph.
        if ((tailor & line_break_tailor_break_all) && (klass == C::AL || klass == C::HL || klass == C::NU))
            klass = C::ID;
        std::uint8_t flags = line_break_flags(text[i]);
        p.raw.push_back(klass);
        if (klass == C::CM || klass == C::ZWJ) {
            // LB9: X (CM | ZWJ)* is X, unless X is BK, CR, LF, NL, SP or ZW;
            // LB10: any other CM or ZWJ is AL.
            if (i > 0) {
                C const base = p.klass[i - 1];
                if (base != C::BK && base != C::CR && base != C::LF && base != C::NL && base != C::SP
                    && base != C::ZW) {
                    p.klass.push_back(base);
                    p.flags.push_back(p.flags[i - 1]);
                    p.absorbed[i] = true;
                    continue;
                }
            }
            klass = C::AL;
        }
        p.klass.push_back(klass);
        p.flags.push_back(flags);
    }
    return p;
}

// The iteration marks a line may start with under line-break: normal
// and loose (css-text-3 §5.3): 々 〻 ゝ ゞ ヽ ヾ.
bool is_iteration_mark(char32_t c)
{
    return c == 0x3005 || c == 0x303B || c == 0x309D || c == 0x309E || c == 0x30FD || c == 0x30FE;
}

// What word-break: keep-all keeps together: letters and digits of any
// script, and ideographs — the classes of the typographic letter units.
bool is_letter_like(C c)
{
    switch (c) {
    case C::AL:
    case C::HL:
    case C::NU:
    case C::ID:
    case C::H2:
    case C::H3:
    case C::JL:
    case C::JV:
    case C::JT:
    case C::AK:
    case C::AS:
        return true;
    default:
        return false;
    }
}

// Rules over the boundary before character `b`; `a` is the character
// before it, both bases (an absorbed mark never reaches here). Positions
// are read through the bases: the one before a base skips its marks.
struct Rules {
    Prepared const& p;
    std::size_t n;

    // The base before `i`, or npos at the start of the text.
    std::size_t before(std::size_t i) const
    {
        while (i > 0) {
            --i;
            if (!p.absorbed[i])
                return i;
        }
        return npos;
    }
    // The base after `i`, or npos at the end.
    std::size_t after(std::size_t i) const
    {
        for (std::size_t j = i + 1; j < n; ++j)
            if (!p.absorbed[j])
                return j;
        return npos;
    }
    C at(std::size_t i) const { return i == npos ? C::BK : p.klass[i]; } // BK stands for "nothing there"
    bool is(std::size_t i, C c) const { return i != npos && p.klass[i] == c; }
    bool in(std::size_t i, std::initializer_list<C> set) const
    {
        if (i == npos)
            return false;
        for (C const c : set)
            if (p.klass[i] == c)
                return true;
        return false;
    }
    // Back over the spaces before `i` (SP*): the base before them, or `i`
    // itself when it is not a space.
    std::size_t before_spaces(std::size_t i) const
    {
        while (i != npos && is(i, C::SP))
            i = before(i);
        return i;
    }
    // Back over (SY | IS)* before `i`, for LB25.
    std::size_t before_number_tail(std::size_t i) const
    {
        while (i != npos && in(i, { C::SY, C::IS }))
            i = before(i);
        return i;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    LineBreak decide(std::size_t a, std::size_t b) const
    {
        C const prev = at(a);
        C const next = at(b);
        std::size_t const before_a = before(a);
        std::size_t const after_b = after(b);

        // LB4, LB5, LB6
        if (prev == C::BK)
            return LineBreak::Mandatory;
        if (prev == C::CR && next == C::LF)
            return LineBreak::None;
        if (prev == C::CR || prev == C::LF || prev == C::NL)
            return LineBreak::Mandatory;
        if (next == C::BK || next == C::CR || next == C::LF || next == C::NL)
            return LineBreak::None;
        // white-space: break-spaces — a break after every preserved space
        // and every other space separator, between two of them included
        // (css-text-3 §3).
        if ((p.tailoring(a) & line_break_tailor_break_spaces)
            && (prev == C::SP || is_other_space_separator(p.code_point[a])))
            return LineBreak::Allowed;
        // LB7
        if (next == C::SP || next == C::ZW)
            return LineBreak::None;
        // LB8: ZW SP* ÷
        if (is(before_spaces(a), C::ZW))
            return LineBreak::Allowed;
        // LB8a: ZWJ ×, the joiner itself, folded or not.
        if (p.raw[b - 1] == C::ZWJ)
            return LineBreak::None;
        // LB11
        if (next == C::WJ || prev == C::WJ)
            return LineBreak::None;
        // LB12
        if (prev == C::GL)
            return LineBreak::None;
        // LB12a
        if (next == C::GL && prev != C::SP && prev != C::BA && prev != C::HY)
            return LineBreak::None;
        // LB13
        if (next == C::CL || next == C::CP || next == C::EX || next == C::SY)
            return LineBreak::None;
        // LB14: OP SP* ×
        if (is(before_spaces(a), C::OP))
            return LineBreak::None;
        // LB15a: (sot | BK | CR | LF | NL | OP | QU | GL | SP | ZW) [\p{Pi}&QU] SP* ×
        {
            std::size_t const quote = before_spaces(a);
            if (is(quote, C::QU) && p.initial_quote(quote)) {
                std::size_t const opener = before(quote);
                if (opener == npos || in(opener, { C::BK, C::CR, C::LF, C::NL, C::OP, C::QU, C::GL, C::SP, C::ZW }))
                    return LineBreak::None;
            }
        }
        // LB15b: × [\p{Pf}&QU] ( SP | GL | WJ | CL | QU | CP | EX | IS | SY | BK | CR | LF | NL | ZW | eot )
        if (next == C::QU && p.final_quote(b)) {
            if (after_b == npos
                || in(after_b, { C::SP, C::GL, C::WJ, C::CL, C::QU, C::CP, C::EX, C::IS, C::SY, C::BK, C::CR, C::LF, C::NL, C::ZW }))
                return LineBreak::None;
        }
        // LB15c: SP ÷ IS NU
        if (prev == C::SP && next == C::IS && is(after_b, C::NU))
            return LineBreak::Allowed;
        // LB15d: × IS
        if (next == C::IS)
            return LineBreak::None;
        // LB16: (CL | CP) SP* × NS
        if (next == C::NS && in(before_spaces(a), { C::CL, C::CP }))
            return LineBreak::None;
        // LB17: B2 SP* × B2
        if (next == C::B2 && is(before_spaces(a), C::B2))
            return LineBreak::None;
        // LB18: SP ÷
        if (prev == C::SP)
            return LineBreak::Allowed;
        // LB19: × [QU - \p{Pi}]; [QU - \p{Pf}] ×
        if (next == C::QU && !p.initial_quote(b))
            return LineBreak::None;
        if (prev == C::QU && !p.final_quote(a))
            return LineBreak::None;
        // LB19a: unless surrounded by East Asian characters, no break either side of a quotation mark.
        if (next == C::QU) {
            if (!p.east_asian(a))
                return LineBreak::None;
            if (after_b == npos || !p.east_asian(after_b))
                return LineBreak::None;
        }
        if (prev == C::QU) {
            if (!p.east_asian(b))
                return LineBreak::None;
            if (before_a == npos || !p.east_asian(before_a))
                return LineBreak::None;
        }
        // LB20: ÷ CB; CB ÷
        if (next == C::CB || prev == C::CB)
            return LineBreak::Allowed;
        // LB20a: (sot | BK | CR | LF | NL | SP | ZW | CB | GL) (HY | U+2010) × AL
        if (next == C::AL && (prev == C::HY || p.code_point[a] == 0x2010)) {
            if (before_a == npos || in(before_a, { C::BK, C::CR, C::LF, C::NL, C::SP, C::ZW, C::CB, C::GL }))
                return LineBreak::None;
        }
        // line-break: loose — a hyphen may start a line after an ideograph;
        // normal and loose — so may an iteration mark (css-text-3 §5.3).
        if ((p.tailoring(a) & line_break_tailor_loose) && (p.tailoring(b) & line_break_tailor_loose) && prev == C::ID
            && (p.code_point[b] == 0x2010 || p.code_point[b] == 0x2013))
            return LineBreak::Allowed;
        if ((p.tailoring(a) & line_break_tailor_normal) && (p.tailoring(b) & line_break_tailor_normal)
            && is_iteration_mark(p.code_point[b]))
            return LineBreak::Allowed;
        // LB21: × BA; × HY; × NS; BB ×
        if (next == C::BA || next == C::HY || next == C::NS || prev == C::BB)
            return LineBreak::None;
        // LB21a: HL (HY | [BA - $EastAsian]) × [^HL]
        if ((prev == C::HY || (prev == C::BA && !p.east_asian(a))) && is(before_a, C::HL) && next != C::HL)
            return LineBreak::None;
        // LB21b: SY × HL
        if (prev == C::SY && next == C::HL)
            return LineBreak::None;
        // LB22: × IN — but under line-break: normal and loose a line may
        // end between two inseparable characters (css-text-3 §5.3).
        if (next == C::IN) {
            bool const relaxed = prev == C::IN && (p.tailoring(a) & line_break_tailor_normal)
                && (p.tailoring(b) & line_break_tailor_normal);
            if (!relaxed)
                return LineBreak::None;
        }
        // LB23: (AL | HL) × NU; NU × (AL | HL)
        if ((prev == C::AL || prev == C::HL) && next == C::NU)
            return LineBreak::None;
        if (prev == C::NU && (next == C::AL || next == C::HL))
            return LineBreak::None;
        // LB23a: PR × (ID | EB | EM); (ID | EB | EM) × PO
        if (prev == C::PR && (next == C::ID || next == C::EB || next == C::EM))
            return LineBreak::None;
        if ((prev == C::ID || prev == C::EB || prev == C::EM) && next == C::PO)
            return LineBreak::None;
        // LB24: (PR | PO) × (AL | HL); (AL | HL) × (PR | PO)
        if ((prev == C::PR || prev == C::PO) && (next == C::AL || next == C::HL))
            return LineBreak::None;
        if ((prev == C::AL || prev == C::HL) && (next == C::PR || next == C::PO))
            return LineBreak::None;
        // LB25: numbers, as Unicode 16 spells them out.
        if (next == C::PO || next == C::PR) {
            // NU (SY | IS)* (CL | CP)? × (PO | PR)
            std::size_t tail = a;
            if (in(tail, { C::CL, C::CP }))
                tail = before(tail);
            if (is(before_number_tail(tail), C::NU))
                return LineBreak::None;
        }
        if (prev == C::PO || prev == C::PR) {
            // (PO | PR) × OP NU; (PO | PR) × OP IS NU; (PO | PR) × NU
            if (next == C::OP) {
                if (is(after_b, C::NU) || (is(after_b, C::IS) && is(after(after_b), C::NU)))
                    return LineBreak::None;
            }
            if (next == C::NU)
                return LineBreak::None;
        }
        if ((prev == C::HY || prev == C::IS) && next == C::NU)
            return LineBreak::None;
        if (next == C::NU && is(before_number_tail(a), C::NU))
            return LineBreak::None;
        // LB26: Korean syllable blocks
        if (prev == C::JL && (next == C::JL || next == C::JV || next == C::H2 || next == C::H3))
            return LineBreak::None;
        if ((prev == C::JV || prev == C::H2) && (next == C::JV || next == C::JT))
            return LineBreak::None;
        if ((prev == C::JT || prev == C::H3) && next == C::JT)
            return LineBreak::None;
        // LB27
        if ((prev == C::JL || prev == C::JV || prev == C::JT || prev == C::H2 || prev == C::H3) && next == C::PO)
            return LineBreak::None;
        if (prev == C::PR && (next == C::JL || next == C::JV || next == C::JT || next == C::H2 || next == C::H3))
            return LineBreak::None;
        // LB28: (AL | HL) × (AL | HL)
        if ((prev == C::AL || prev == C::HL) && (next == C::AL || next == C::HL))
            return LineBreak::None;
        // LB28a: Brahmic conjuncts
        if (prev == C::AP && p.aksara_like(b))
            return LineBreak::None;
        if (p.aksara_like(a) && (next == C::VF || next == C::VI))
            return LineBreak::None;
        if (prev == C::VI && before_a != npos && p.aksara_like(before_a) && (next == C::AK || p.code_point[b] == dotted_circle))
            return LineBreak::None;
        if (p.aksara_like(a) && p.aksara_like(b) && is(after_b, C::VF))
            return LineBreak::None;
        // LB29: IS × (AL | HL)
        if (prev == C::IS && (next == C::AL || next == C::HL))
            return LineBreak::None;
        // LB30: (AL | HL | NU) × [OP - $EastAsian]; [CP - $EastAsian] × (AL | HL | NU)
        if ((prev == C::AL || prev == C::HL || prev == C::NU) && next == C::OP && !p.east_asian(b))
            return LineBreak::None;
        if (prev == C::CP && !p.east_asian(a) && (next == C::AL || next == C::HL || next == C::NU))
            return LineBreak::None;
        // LB30a: regional indicators pair up from the start of their run.
        if (prev == C::RI && next == C::RI) {
            std::size_t count = 0;
            for (std::size_t i = a; i != npos && is(i, C::RI); i = before(i))
                ++count;
            if (count % 2 == 1)
                return LineBreak::None;
        }
        // LB30b: EB × EM; [\p{Extended_Pictographic}&\p{Cn}] × EM
        if (next == C::EM && (prev == C::EB || p.unassigned_pictographic(a)))
            return LineBreak::None;
        // word-break: keep-all — no implicit break between two letters,
        // digits or ideographs (css-text-3 §5.2), where both sides ask.
        if ((p.tailoring(a) & line_break_tailor_keep_all) && (p.tailoring(b) & line_break_tailor_keep_all)
            && is_letter_like(prev) && is_letter_like(next))
            return LineBreak::None;
        // LB31
        return LineBreak::Allowed;
    }
};

// The classes an emoji sequence is made of: what a joiner glues into one
// character, as far as the line breaker can tell without the grapheme
// cluster rules.
bool is_pictograph_like(C c)
{
    return c == C::ID || c == C::EB || c == C::EM;
}

} // namespace

bool is_emoji_presentation(char32_t code_point)
{
    return (line_break_flags(code_point) & line_break_emoji_presentation) != 0;
}

bool is_other_space_separator(char32_t code_point)
{
    return code_point == 0x1680 || (code_point >= 0x2000 && code_point <= 0x200A) || code_point == 0x202F
        || code_point == 0x205F || code_point == 0x3000;
}

LineBreakClass line_break_class(char32_t code_point)
{
    LineBreakRange const* const range = range_of(code_point);
    return range ? range->klass : LineBreakClass::AL;
}

std::uint8_t line_break_flags(char32_t code_point)
{
    LineBreakRange const* const range = range_of(code_point);
    return range ? range->flags : 0;
}

std::vector<LineBreak> line_break_opportunities(std::u32string_view text, std::span<std::uint8_t const> tailoring)
{
    std::vector<LineBreak> out(text.size() + 1, LineBreak::None);
    if (text.empty()) {
        out[0] = LineBreak::Mandatory;
        return out;
    }
    Prepared const p = prepare(text, tailoring);
    Rules const rules { p, text.size() };
    std::size_t a = 0;
    for (std::size_t b = 1; b < text.size(); ++b) {
        if (p.absorbed[b])
            continue; // LB9: no break inside X (CM | ZWJ)*
        out[b] = rules.decide(a, b);
        // line-break: anywhere — a break around every character, whatever
        // the rules forbade, a joiner's included (css-text-3 §5.3), short
        // of parting a CR from its LF (LB5) or one pictograph from another
        // it is joined to, which make one character between them.
        if (out[b] == LineBreak::None && (p.tailoring(a) & line_break_tailor_anywhere)
            && (p.tailoring(b) & line_break_tailor_anywhere) && !(p.klass[a] == C::CR && p.klass[b] == C::LF)
            && !(p.raw[b - 1] == C::ZWJ && is_pictograph_like(p.klass[a]) && is_pictograph_like(p.klass[b])))
            out[b] = LineBreak::Allowed;
        a = b;
    }
    out[text.size()] = LineBreak::Mandatory; // LB3
    return out;
}

}
