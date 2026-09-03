// The Unicode bidirectional algorithm (UAX #9), rules P2 through L2, over
// the generated Bidi_Class and paired-bracket tables. The rule numbers in
// the comments are the specification's; where a rule reads oddly out of
// context, the comment says what it is for rather than restating it.
//
// The shape follows the specification's own: resolve the explicit levels
// (X1-X8), take out the characters X9 removes, cut what is left into
// isolating run sequences (X10), resolve the weak types (W1-W7), the paired
// brackets and the neutrals (N0-N2) and the implicit levels (I1-I2) inside
// each sequence, then settle the line (L1) and read off the order (L2).

#include "core/Bidi.h"

#include "core/BidiData.h"

#include <algorithm>
#include <iterator>

namespace sashfold {

namespace {

// The deepest embedding the algorithm allows (BD2); past it, an embedding
// or an isolate overflows and is counted rather than pushed.
constexpr std::uint8_t max_depth = 125;

BidiClass class_of(char32_t code_point)
{
    auto const it = std::upper_bound(std::begin(bidi_class_ranges), std::end(bidi_class_ranges),
        code_point, [](char32_t value, BidiRange const& range) { return value < range.first; });
    if (it == std::begin(bidi_class_ranges))
        return BidiClass::L;
    BidiRange const& range = *std::prev(it);
    return code_point <= range.last ? range.klass : BidiClass::L;
}

BidiBracket const* bracket_of(char32_t code_point)
{
    auto const it = std::lower_bound(std::begin(bidi_brackets), std::end(bidi_brackets), code_point,
        [](BidiBracket const& entry, char32_t value) { return entry.code_point < value; });
    if (it == std::end(bidi_brackets) || it->code_point != code_point)
        return nullptr;
    return it;
}

bool is_isolate_initiator(BidiClass k)
{
    return k == BidiClass::LRI || k == BidiClass::RLI || k == BidiClass::FSI;
}

// X9: the embeddings, the overrides, their pop, and the boundary-neutrals
// are taken out. They are drawn as nothing and take no part in the order.
bool is_removed(BidiClass k)
{
    return k == BidiClass::RLE || k == BidiClass::LRE || k == BidiClass::RLO
        || k == BidiClass::LRO || k == BidiClass::PDF || k == BidiClass::BN;
}

// BD13's "NI": the neutrals and the isolate formatting characters, which
// N1 and N2 settle together.
bool is_neutral_or_isolate(BidiClass k)
{
    return k == BidiClass::B || k == BidiClass::S || k == BidiClass::WS || k == BidiClass::ON
        || k == BidiClass::PDI || is_isolate_initiator(k);
}

std::uint8_t next_odd(std::uint8_t level) { return static_cast<std::uint8_t>((level + 1) | 1); }
std::uint8_t next_even(std::uint8_t level) { return static_cast<std::uint8_t>((level + 2) & ~1); }

// U+2329 and U+3008 are canonically equivalent, and so are U+232A and
// U+3009, so a bracket written one way closes one written the other (BD16
// matches on canonical equivalence). These two pairs are the only ones in
// the table for which that arises.
char32_t canonical_bracket(char32_t code_point)
{
    if (code_point == 0x3008)
        return 0x2329;
    if (code_point == 0x3009)
        return 0x232A;
    return code_point;
}

// BD9: the PDI that matches an isolate initiator, or npos when it has none.
std::vector<std::size_t> matching_isolates(std::vector<BidiClass> const& classes)
{
    std::vector<std::size_t> match(classes.size(), std::u32string_view::npos);
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        if (is_isolate_initiator(classes[i])) {
            open.push_back(i);
        } else if (classes[i] == BidiClass::PDI && !open.empty()) {
            match[open.back()] = i;
            open.pop_back();
        }
    }
    return match;
}

// One isolating run sequence (BD13): the positions it covers, in order,
// and the directions its two ends look out onto (X10's sos and eos).
struct RunSequence {
    std::vector<std::size_t> positions;
    std::uint8_t level = 0;
    BidiClass sos = BidiClass::L;
    BidiClass eos = BidiClass::L;
};

BidiClass direction_of_level(std::uint8_t level) { return (level & 1) ? BidiClass::R : BidiClass::L; }

}

StrongDirection strong_direction(char32_t code_point)
{
    switch (class_of(code_point)) {
    case BidiClass::L:
        return StrongDirection::Ltr;
    case BidiClass::R:
    case BidiClass::AL:
        return StrongDirection::Rtl;
    default:
        return StrongDirection::None;
    }
}

bool first_strong_is_rtl(std::u32string_view text)
{
    int isolated = 0;
    for (char32_t const code_point : text) {
        BidiClass const klass = class_of(code_point);
        if (is_isolate_initiator(klass)) {
            ++isolated;
            continue;
        }
        if (klass == BidiClass::PDI) {
            // An unmatched PDI is not an error here: it ends nothing.
            isolated = std::max(0, isolated - 1);
            continue;
        }
        if (isolated > 0)
            continue;
        if (klass == BidiClass::L)
            return false;
        if (klass == BidiClass::R || klass == BidiClass::AL)
            return true;
    }
    return false; // P3: nothing strong, so left-to-right
}

BidiParagraph bidi_resolve(std::u32string_view text, std::optional<std::uint8_t> paragraph_level)
{
    std::size_t const n = text.size();
    BidiParagraph result;
    result.levels.assign(n, 0);
    result.removed.assign(n, false);

    std::vector<BidiClass> original(n);
    for (std::size_t i = 0; i < n; ++i)
        original[i] = class_of(text[i]);
    std::vector<BidiClass> types = original;
    std::vector<std::size_t> const match = matching_isolates(original);
    // Which PDIs close an isolate: those join the sequence that opened
    // them, so they never begin one of their own.
    std::vector<bool> matched_pdi(n, false);
    for (std::size_t const closing : match) {
        if (closing != std::u32string_view::npos)
            matched_pdi[closing] = true;
    }

    std::uint8_t const base = paragraph_level ? *paragraph_level : (first_strong_is_rtl(text) ? 1 : 0);
    result.paragraph_level = base;
    for (std::size_t i = 0; i < n; ++i)
        result.removed[i] = is_removed(original[i]);

    // --- X1-X8: the explicit levels ------------------------------------------
    struct Status {
        std::uint8_t level;
        BidiClass override_status; // ON when the embedding does not override
        bool isolate;
    };
    std::vector<Status> stack { { base, BidiClass::ON, false } };
    int overflow_isolates = 0;
    int overflow_embeddings = 0;
    int valid_isolates = 0;

    for (std::size_t i = 0; i < n; ++i) {
        BidiClass const klass = original[i];
        switch (klass) {
        case BidiClass::RLE:
        case BidiClass::LRE:
        case BidiClass::RLO:
        case BidiClass::LRO: {
            // X2-X5. The character itself is removed by X9; it keeps the
            // level it was written at so a run around it stays contiguous.
            result.levels[i] = stack.back().level;
            bool const rtl = klass == BidiClass::RLE || klass == BidiClass::RLO;
            std::uint8_t const level
                = rtl ? next_odd(stack.back().level) : next_even(stack.back().level);
            BidiClass const override_status = klass == BidiClass::RLO ? BidiClass::R
                : klass == BidiClass::LRO                             ? BidiClass::L
                                                                      : BidiClass::ON;
            if (level <= max_depth && overflow_isolates == 0 && overflow_embeddings == 0)
                stack.push_back({ level, override_status, false });
            else if (overflow_isolates == 0)
                ++overflow_embeddings;
            break;
        }
        case BidiClass::RLI:
        case BidiClass::LRI:
        case BidiClass::FSI: {
            // X5a-X5c. An isolate initiator is drawn (as nothing visible)
            // and belongs to the run around it, so it takes the current
            // level and the current override before the push.
            bool rtl = klass == BidiClass::RLI;
            if (klass == BidiClass::FSI) {
                // X5c: which way the isolated text reads decides.
                std::size_t const end = match[i] == std::u32string_view::npos ? n : match[i];
                rtl = first_strong_is_rtl(text.substr(i + 1, end - (i + 1)));
            }
            result.levels[i] = stack.back().level;
            if (stack.back().override_status != BidiClass::ON)
                types[i] = stack.back().override_status;
            std::uint8_t const level
                = rtl ? next_odd(stack.back().level) : next_even(stack.back().level);
            if (level <= max_depth && overflow_isolates == 0 && overflow_embeddings == 0) {
                ++valid_isolates;
                stack.push_back({ level, BidiClass::ON, true });
            } else {
                ++overflow_isolates;
            }
            break;
        }
        case BidiClass::PDI: {
            // X6a. The pop happens first, so the PDI reads at the level of
            // the text it returns to — which is what keeps it with the run
            // outside its isolate rather than the one inside.
            if (overflow_isolates > 0) {
                --overflow_isolates;
            } else if (valid_isolates > 0) {
                overflow_embeddings = 0;
                while (!stack.back().isolate)
                    stack.pop_back();
                stack.pop_back();
                --valid_isolates;
            }
            result.levels[i] = stack.back().level;
            if (stack.back().override_status != BidiClass::ON)
                types[i] = stack.back().override_status;
            break;
        }
        case BidiClass::PDF: {
            // X7. Removed by X9, and it closes the embedding it can.
            result.levels[i] = stack.back().level;
            if (overflow_isolates > 0) {
                // An isolate is still open: this pop is not ours to make.
            } else if (overflow_embeddings > 0) {
                --overflow_embeddings;
            } else if (!stack.back().isolate && stack.size() >= 2) {
                stack.pop_back();
            }
            break;
        }
        case BidiClass::B: {
            // X8. A paragraph separator ends everything: it reads at the
            // paragraph's own level and the state starts again.
            stack.assign(1, { base, BidiClass::ON, false });
            overflow_isolates = 0;
            overflow_embeddings = 0;
            valid_isolates = 0;
            result.levels[i] = base;
            break;
        }
        default: {
            // X6, and the boundary-neutrals X9 removes, which keep a level
            // only so the runs around them stay contiguous.
            result.levels[i] = stack.back().level;
            if (klass != BidiClass::BN && stack.back().override_status != BidiClass::ON)
                types[i] = stack.back().override_status;
            break;
        }
        }
    }

    // --- X9, X10: what is left, cut into isolating run sequences -------------
    std::vector<std::size_t> kept;
    kept.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!result.removed[i])
            kept.push_back(i);
    }

    // The level runs, in order: maximal stretches of one level among what
    // X9 left.
    std::vector<std::vector<std::size_t>> level_runs;
    for (std::size_t k = 0; k < kept.size(); ++k) {
        if (k > 0 && result.levels[kept[k]] == result.levels[kept[k - 1]])
            level_runs.back().push_back(kept[k]);
        else
            level_runs.push_back({ kept[k] });
    }
    // Which run each kept position starts, so an isolate initiator can find
    // the run its matching PDI begins.
    std::vector<std::size_t> run_starting_at(n, std::u32string_view::npos);
    for (std::size_t r = 0; r < level_runs.size(); ++r)
        run_starting_at[level_runs[r].front()] = r;

    std::vector<bool> used(level_runs.size(), false);
    std::vector<RunSequence> sequences;
    for (std::size_t r = 0; r < level_runs.size(); ++r) {
        if (used[r])
            continue;
        // BD13: a sequence starts at a run whose first character is not a
        // PDI that matches an isolate initiator — those are joined onto the
        // sequence that opened them instead.
        std::size_t const first = level_runs[r].front();
        if (original[first] == BidiClass::PDI && matched_pdi[first])
            continue;
        RunSequence sequence;
        sequence.level = result.levels[first];
        std::size_t current = r;
        for (;;) {
            used[current] = true;
            for (std::size_t const position : level_runs[current])
                sequence.positions.push_back(position);
            std::size_t const last = level_runs[current].back();
            if (!is_isolate_initiator(original[last]) || match[last] == std::u32string_view::npos)
                break;
            std::size_t const next = run_starting_at[match[last]];
            if (next == std::u32string_view::npos || used[next])
                break;
            current = next;
        }
        sequences.push_back(std::move(sequence));
    }

    // X10's sos and eos: what the sequence's two ends look out onto. Each is
    // the direction of the higher of the sequence's level and the level of
    // the character on that side (the paragraph's own where there is none).
    auto const level_before = [&](std::size_t position) {
        for (std::size_t i = position; i-- > 0;) {
            if (!result.removed[i])
                return result.levels[i];
        }
        return base;
    };
    auto const level_after = [&](std::size_t position) {
        for (std::size_t i = position + 1; i < n; ++i) {
            if (!result.removed[i])
                return result.levels[i];
        }
        return base;
    };
    for (RunSequence& sequence : sequences) {
        std::size_t const first = sequence.positions.front();
        std::size_t const last = sequence.positions.back();
        sequence.sos = direction_of_level(std::max(sequence.level, level_before(first)));
        // A sequence ending at an isolate initiator with no matching PDI
        // looks out onto the paragraph, not onto whatever follows it.
        bool const unmatched_isolate
            = is_isolate_initiator(original[last]) && match[last] == std::u32string_view::npos;
        std::uint8_t const after = unmatched_isolate ? base : level_after(last);
        sequence.eos = direction_of_level(std::max(sequence.level, after));
    }

    // --- W1-W7, N0-N2, I1-I2, inside each sequence ---------------------------
    for (RunSequence const& sequence : sequences) {
        std::vector<std::size_t> const& at = sequence.positions;
        std::size_t const length = at.size();
        BidiClass const e = direction_of_level(sequence.level);
        BidiClass const o = e == BidiClass::L ? BidiClass::R : BidiClass::L;

        // W1: a nonspacing mark takes the type of what it sits on; after an
        // isolate initiator or a PDI it has nothing to sit on and is ON.
        for (std::size_t k = 0; k < length; ++k) {
            if (types[at[k]] != BidiClass::NSM)
                continue;
            if (k == 0) {
                types[at[k]] = sequence.sos;
                continue;
            }
            BidiClass const previous = types[at[k - 1]];
            types[at[k]] = is_isolate_initiator(previous) || previous == BidiClass::PDI
                ? BidiClass::ON
                : previous;
        }
        // W2: a European number after an Arabic letter is an Arabic number.
        {
            BidiClass strong = sequence.sos;
            for (std::size_t k = 0; k < length; ++k) {
                BidiClass const klass = types[at[k]];
                if (klass == BidiClass::L || klass == BidiClass::R || klass == BidiClass::AL)
                    strong = klass;
                else if (klass == BidiClass::EN && strong == BidiClass::AL)
                    types[at[k]] = BidiClass::AN;
            }
        }
        // W3: and the Arabic letter itself is simply right-to-left.
        for (std::size_t k = 0; k < length; ++k) {
            if (types[at[k]] == BidiClass::AL)
                types[at[k]] = BidiClass::R;
        }
        // W4: a single separator between two numbers of a kind joins them.
        for (std::size_t k = 1; k + 1 < length; ++k) {
            BidiClass const klass = types[at[k]];
            BidiClass const before = types[at[k - 1]];
            BidiClass const after = types[at[k + 1]];
            if (klass == BidiClass::ES && before == BidiClass::EN && after == BidiClass::EN)
                types[at[k]] = BidiClass::EN;
            else if (klass == BidiClass::CS && before == after
                && (before == BidiClass::EN || before == BidiClass::AN))
                types[at[k]] = before;
        }
        // W5: a run of terminators beside a European number joins it.
        for (std::size_t k = 0; k < length; ++k) {
            if (types[at[k]] != BidiClass::ET)
                continue;
            std::size_t end = k;
            while (end < length && types[at[end]] == BidiClass::ET)
                ++end;
            bool const before = k > 0 && types[at[k - 1]] == BidiClass::EN;
            bool const after = end < length && types[at[end]] == BidiClass::EN;
            if (before || after) {
                for (std::size_t j = k; j < end; ++j)
                    types[at[j]] = BidiClass::EN;
            }
            k = end - 1;
        }
        // W6: every separator and terminator still standing is neutral.
        for (std::size_t k = 0; k < length; ++k) {
            BidiClass const klass = types[at[k]];
            if (klass == BidiClass::ES || klass == BidiClass::ET || klass == BidiClass::CS)
                types[at[k]] = BidiClass::ON;
        }
        // W7: a European number in left-to-right text reads as that text.
        {
            BidiClass strong = sequence.sos;
            for (std::size_t k = 0; k < length; ++k) {
                BidiClass const klass = types[at[k]];
                if (klass == BidiClass::L || klass == BidiClass::R)
                    strong = klass;
                else if (klass == BidiClass::EN && strong == BidiClass::L)
                    types[at[k]] = BidiClass::L;
            }
        }

        // N0: the paired brackets. BD16 pairs them off with a stack that
        // holds at most 63 openings; past that the rule gives up on the rest
        // of the sequence rather than guessing.
        {
            struct Opening {
                char32_t closing;
                std::size_t index; // into `at`
            };
            std::vector<Opening> open;
            std::vector<std::pair<std::size_t, std::size_t>> pairs;
            for (std::size_t k = 0; k < length; ++k) {
                if (types[at[k]] != BidiClass::ON)
                    continue;
                BidiBracket const* const bracket = bracket_of(text[at[k]]);
                if (!bracket)
                    continue;
                if (bracket->opening) {
                    if (open.size() == 63)
                        break;
                    open.push_back({ canonical_bracket(bracket->paired), k });
                    continue;
                }
                for (std::size_t s = open.size(); s-- > 0;) {
                    if (open[s].closing != canonical_bracket(text[at[k]]))
                        continue;
                    pairs.push_back({ open[s].index, k });
                    open.resize(s);
                    break;
                }
            }
            std::sort(pairs.begin(), pairs.end());

            auto const strength = [&](BidiClass klass) {
                // For N0 and N1 a number counts as right-to-left, since that
                // is the direction the digits of one are read within.
                if (klass == BidiClass::EN || klass == BidiClass::AN)
                    return BidiClass::R;
                if (klass == BidiClass::L || klass == BidiClass::R)
                    return klass;
                return BidiClass::ON;
            };
            for (auto const& [opening, closing] : pairs) {
                bool embedding_inside = false;
                bool opposite_inside = false;
                for (std::size_t k = opening + 1; k < closing; ++k) {
                    BidiClass const inside = strength(types[at[k]]);
                    embedding_inside = embedding_inside || inside == e;
                    opposite_inside = opposite_inside || inside == o;
                }
                std::optional<BidiClass> settled;
                if (embedding_inside) {
                    settled = e; // b: the brackets read as the text around them
                } else if (opposite_inside) {
                    // c: unless the text before the pair reads the other way
                    // too, in which case the pair goes with it.
                    BidiClass preceding = sequence.sos;
                    for (std::size_t k = opening; k-- > 0;) {
                        BidiClass const before = strength(types[at[k]]);
                        if (before != BidiClass::ON) {
                            preceding = before;
                            break;
                        }
                    }
                    settled = preceding == o ? o : e;
                }
                if (!settled)
                    continue; // d: nothing strong inside, so they stay neutral
                types[at[opening]] = *settled;
                types[at[closing]] = *settled;
                // A mark that followed a bracket goes with it, since W1 gave
                // it the bracket's old type and the bracket has just changed.
                for (std::size_t const bracket : { opening, closing }) {
                    for (std::size_t k = bracket + 1; k < length; ++k) {
                        if (original[at[k]] != BidiClass::NSM)
                            break;
                        types[at[k]] = *settled;
                    }
                }
            }
        }

        // N1: a stretch of neutrals between two sides that agree takes their
        // direction; N2: whatever is left takes the embedding's.
        for (std::size_t k = 0; k < length; ++k) {
            if (!is_neutral_or_isolate(types[at[k]]))
                continue;
            std::size_t end = k;
            while (end < length && is_neutral_or_isolate(types[at[end]]))
                ++end;
            auto const side = [&](BidiClass klass) {
                if (klass == BidiClass::EN || klass == BidiClass::AN)
                    return BidiClass::R;
                return klass;
            };
            BidiClass const before = k == 0 ? sequence.sos : side(types[at[k - 1]]);
            BidiClass const after = end == length ? sequence.eos : side(types[at[end]]);
            BidiClass const settled = before == after && (before == BidiClass::L || before == BidiClass::R)
                ? before
                : e;
            for (std::size_t j = k; j < end; ++j)
                types[at[j]] = settled;
            k = end - 1;
        }

        // I1 and I2: the levels the resolved types imply.
        for (std::size_t k = 0; k < length; ++k) {
            BidiClass const klass = types[at[k]];
            std::uint8_t& level = result.levels[at[k]];
            if ((sequence.level & 1) == 0) {
                if (klass == BidiClass::R)
                    level = static_cast<std::uint8_t>(level + 1);
                else if (klass == BidiClass::AN || klass == BidiClass::EN)
                    level = static_cast<std::uint8_t>(level + 2);
            } else if (klass == BidiClass::L || klass == BidiClass::AN || klass == BidiClass::EN) {
                level = static_cast<std::uint8_t>(level + 1);
            }
        }
    }

    // --- L1: the separators and the whitespace at the ends of a line ---------
    // On the ORIGINAL types, as the rule says: a segment or paragraph
    // separator returns to the paragraph's level, and so does any whitespace
    // that runs up to one, or up to the end of the line. What X9 removed
    // goes with the whitespace it sits in rather than breaking the run.
    {
        auto const resets = [&](BidiClass klass) {
            return klass == BidiClass::WS || is_isolate_initiator(klass) || klass == BidiClass::PDI
                || is_removed(klass);
        };
        bool trailing = true;
        for (std::size_t i = n; i-- > 0;) {
            BidiClass const klass = original[i];
            if (klass == BidiClass::S || klass == BidiClass::B) {
                result.levels[i] = base;
                trailing = true;
            } else if (trailing && resets(klass)) {
                result.levels[i] = base;
            } else {
                trailing = false;
            }
        }
    }
    return result;
}

std::vector<std::size_t> bidi_visual_order(BidiParagraph const& paragraph)
{
    std::vector<std::size_t> order;
    order.reserve(paragraph.levels.size());
    for (std::size_t i = 0; i < paragraph.levels.size(); ++i) {
        if (!paragraph.removed[i])
            order.push_back(i);
    }
    if (order.empty())
        return order;

    // L2: from the highest level down to the lowest odd one, reverse every
    // stretch at or above that level. Each pass folds one nesting depth.
    std::uint8_t highest = 0;
    std::uint8_t lowest_odd = max_depth + 1;
    for (std::size_t const position : order) {
        std::uint8_t const level = paragraph.levels[position];
        highest = std::max(highest, level);
        if (level & 1)
            lowest_odd = std::min(lowest_odd, level);
    }
    for (std::uint8_t level = highest; level >= lowest_odd && level > 0; --level) {
        for (std::size_t k = 0; k < order.size(); ++k) {
            if (paragraph.levels[order[k]] < level)
                continue;
            std::size_t end = k;
            while (end < order.size() && paragraph.levels[order[end]] >= level)
                ++end;
            std::reverse(order.begin() + static_cast<std::ptrdiff_t>(k),
                order.begin() + static_cast<std::ptrdiff_t>(end));
            k = end;
        }
    }
    return order;
}

}
