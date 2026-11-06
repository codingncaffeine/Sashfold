#pragma once
// Word boundaries (UAX #29 §4): where a word starts and ends, as editors
// move by words, a double click selects one, and spelling finds them.
#include "core/WordBreakData.h"

#include <string_view>
#include <vector>

namespace sashfold {

WordBreakClass word_break_class(char32_t code_point);
bool is_extended_pictographic(char32_t code_point);

// One flag per boundary of the text: [i] is whether a word boundary falls
// before character i; [0] and [size] are always boundaries (WB1, WB2).
std::vector<bool> word_boundaries(std::u32string_view text);

}
