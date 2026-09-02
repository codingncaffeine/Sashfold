#include "core/NormalizationData.h"
#include "core/Unicode.h"

#include <algorithm>
#include <iterator>

// NFC (UAX #15): canonical decomposition (pre-expanded by tools/gen-unicode,
// so one table lookup — Hangul is algorithmic), canonical ordering, then
// canonical composition. First consumer: UTS #46 domain processing.

namespace sashfold {

namespace {

// Hangul syllable arithmetic (UAX #15 §3.12).
constexpr char32_t hangul_s_base = 0xAC00;
constexpr char32_t hangul_l_base = 0x1100;
constexpr char32_t hangul_v_base = 0x1161;
constexpr char32_t hangul_t_base = 0x11A7;
constexpr std::uint32_t hangul_l_count = 19;
constexpr std::uint32_t hangul_v_count = 21;
constexpr std::uint32_t hangul_t_count = 28;
constexpr std::uint32_t hangul_n_count = hangul_v_count * hangul_t_count;
constexpr std::uint32_t hangul_s_count = hangul_l_count * hangul_n_count;

std::uint8_t combining_class(char32_t code_point)
{
    auto const it = std::upper_bound(std::begin(combining_class_ranges),
        std::end(combining_class_ranges), code_point,
        [](char32_t value, CombiningClassRange const& range) { return value < range.first; });
    if (it == std::begin(combining_class_ranges))
        return 0;
    CombiningClassRange const& range = *std::prev(it);
    return code_point <= range.last ? range.combining_class : 0;
}

DecompositionEntry const* find_decomposition(char32_t code_point)
{
    auto const it = std::lower_bound(std::begin(canonical_decompositions),
        std::end(canonical_decompositions), code_point,
        [](DecompositionEntry const& entry, char32_t value) { return entry.code_point < value; });
    if (it == std::end(canonical_decompositions) || it->code_point != code_point)
        return nullptr;
    return &*it;
}

// The primary composite for a pair, or 0 when none exists.
char32_t compose_pair(char32_t starter, char32_t combining)
{
    // Hangul first: L+V and LV+T compose algorithmically.
    if (starter >= hangul_l_base && starter < hangul_l_base + hangul_l_count
        && combining >= hangul_v_base && combining < hangul_v_base + hangul_v_count) {
        std::uint32_t const l_index = starter - hangul_l_base;
        std::uint32_t const v_index = combining - hangul_v_base;
        return hangul_s_base + (l_index * hangul_v_count + v_index) * hangul_t_count;
    }
    if (starter >= hangul_s_base && starter < hangul_s_base + hangul_s_count
        && (starter - hangul_s_base) % hangul_t_count == 0 && combining > hangul_t_base
        && combining < hangul_t_base + hangul_t_count) {
        return starter + (combining - hangul_t_base);
    }

    auto const it = std::lower_bound(std::begin(composition_pairs), std::end(composition_pairs),
        starter, [](CompositionPair const& pair, char32_t value) { return pair.starter < value; });
    for (auto entry = it; entry != std::end(composition_pairs) && entry->starter == starter;
        ++entry)
        if (entry->combining == combining)
            return entry->composed;
    return 0;
}

}

std::u32string_view canonical_decomposition(char32_t code_point)
{
    DecompositionEntry const* const entry = find_decomposition(code_point);
    if (!entry)
        return {};
    return std::u32string_view(decomposition_pool + entry->offset, entry->length);
}

bool is_combining_mark(char32_t code_point)
{
    auto const it = std::upper_bound(std::begin(combining_mark_ranges),
        std::end(combining_mark_ranges), code_point,
        [](char32_t value, CombiningMarkRange const& range) { return value < range.first; });
    if (it == std::begin(combining_mark_ranges))
        return false;
    return code_point <= std::prev(it)->last;
}

std::u32string nfc(std::u32string_view input)
{
    // 1. Canonical decomposition.
    std::u32string decomposed;
    decomposed.reserve(input.size());
    for (char32_t const code_point : input) {
        if (code_point >= hangul_s_base && code_point < hangul_s_base + hangul_s_count) {
            std::uint32_t const index = code_point - hangul_s_base;
            decomposed += static_cast<char32_t>(hangul_l_base + index / hangul_n_count);
            decomposed += static_cast<char32_t>(hangul_v_base + (index % hangul_n_count) / hangul_t_count);
            if (index % hangul_t_count != 0)
                decomposed += static_cast<char32_t>(hangul_t_base + index % hangul_t_count);
        } else if (DecompositionEntry const* const entry = find_decomposition(code_point)) {
            decomposed.append(decomposition_pool + entry->offset, entry->length);
        } else {
            decomposed += code_point;
        }
    }

    // 2. Canonical ordering: stable insertion sort of nonzero-class runs.
    for (std::size_t i = 1; i < decomposed.size(); ++i) {
        std::uint8_t const ccc = combining_class(decomposed[i]);
        if (ccc == 0)
            continue;
        std::size_t j = i;
        while (j > 0 && combining_class(decomposed[j - 1]) > ccc) {
            std::swap(decomposed[j - 1], decomposed[j]);
            --j;
        }
    }

    // 3. Canonical composition. A character composes with the last starter
    // when nothing between them blocks it (every in-between character has
    // nonzero class, so blocking means class >= ours).
    std::u32string output;
    output.reserve(decomposed.size());
    std::size_t starter = std::u32string::npos;
    std::uint8_t last_class = 0;
    for (char32_t const code_point : decomposed) {
        std::uint8_t const ccc = combining_class(code_point);
        if (starter != std::u32string::npos) {
            bool const blocked = output.size() - 1 != starter && last_class >= ccc;
            if (!blocked) {
                if (char32_t const composed = compose_pair(output[starter], code_point)) {
                    output[starter] = composed;
                    continue;
                }
            }
        }
        if (ccc == 0)
            starter = output.size();
        output += code_point;
        last_class = ccc;
    }
    return output;
}

}
