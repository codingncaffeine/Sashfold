// The first-strong rules of UAX #9 (P2 and P3), over the generated
// Bidi_Class tables. This is not the bidirectional algorithm — nothing here
// reorders anything. It answers one question: given a run of text, which
// direction does it read as? That is what `dir=auto` asks of an element's
// content and what `unicode-bidi: plaintext` asks of each paragraph.

#include "core/Unicode.h"

#include "core/BidiData.h"

#include <algorithm>
#include <iterator>

namespace sashfold {

namespace {

template <std::size_t N>
bool in_ranges(BidiRange const (&ranges)[N], char32_t code_point)
{
    auto const it = std::upper_bound(std::begin(ranges), std::end(ranges), code_point,
        [](char32_t value, BidiRange const& range) { return value < range.first; });
    if (it == std::begin(ranges))
        return false;
    return code_point <= std::prev(it)->last;
}

// The isolate initiators and their terminator (UAX #9 §2.1): P2 steps over
// everything between an initiator and its matching PDI, because an isolate's
// content is not allowed to decide the direction around it.
constexpr char32_t left_to_right_isolate = 0x2066;
constexpr char32_t right_to_left_isolate = 0x2067;
constexpr char32_t first_strong_isolate = 0x2068;
constexpr char32_t pop_directional_isolate = 0x2069;

}

StrongDirection strong_direction(char32_t code_point)
{
    if (in_ranges(strong_rtl_ranges, code_point))
        return StrongDirection::Rtl;
    if (in_ranges(strong_ltr_ranges, code_point))
        return StrongDirection::Ltr;
    return StrongDirection::None;
}

bool first_strong_is_rtl(std::u32string_view text)
{
    int isolated = 0;
    for (char32_t const code_point : text) {
        if (code_point == left_to_right_isolate || code_point == right_to_left_isolate
            || code_point == first_strong_isolate) {
            ++isolated;
            continue;
        }
        if (code_point == pop_directional_isolate) {
            // An unmatched PDI is not an error here: it simply ends nothing.
            isolated = std::max(0, isolated - 1);
            continue;
        }
        if (isolated > 0)
            continue;
        switch (strong_direction(code_point)) {
        case StrongDirection::Rtl:
            return true;
        case StrongDirection::Ltr:
            return false;
        case StrongDirection::None:
            break;
        }
    }
    return false; // P3: nothing strong, so left-to-right
}

}
