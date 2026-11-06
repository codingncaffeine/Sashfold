#include "ui/FieldEdit.h"

#include "core/Unicode.h"
#include "core/WordBreak.h"

#include <algorithm>

namespace sashfold::ui::field_edit {

namespace {

// A character that is part of a word, for the moves: one of a word's
// classes (letters, digits, kana, the connector punctuation).
bool wordy(char32_t c)
{
    WordBreakClass const k = word_break_class(c);
    return k == WordBreakClass::ALetter || k == WordBreakClass::Hebrew_Letter || k == WordBreakClass::Numeric
        || k == WordBreakClass::Katakana || k == WordBreakClass::ExtendNumLet || is_extended_pictographic(c)
        || (k == WordBreakClass::Other && c > 0x2E7F && !is_combining_mark(c)); // ideographs and the like: words of their own
}

// Whether the segment [from, to) between two word boundaries is a word.
bool segment_is_word(std::u32string_view text, std::size_t from, std::size_t to)
{
    for (std::size_t i = from; i < to; ++i) {
        if (wordy(text[i]))
            return true;
    }
    return false;
}

}

std::size_t previous_character(std::u32string_view text, std::size_t at)
{
    at = std::min(at, text.size());
    if (at == 0)
        return 0;
    --at;
    while (at > 0 && is_combining_mark(text[at]))
        --at;
    return at;
}

std::size_t next_character(std::u32string_view text, std::size_t at)
{
    if (at >= text.size())
        return text.size();
    ++at;
    while (at < text.size() && is_combining_mark(text[at]))
        ++at;
    return at;
}

std::size_t previous_word_start(std::u32string_view text, std::size_t at)
{
    at = std::min(at, text.size());
    std::vector<bool> const breaks = word_boundaries(text);
    // Back over boundaries to the start of the first word segment found.
    std::size_t end = at;
    while (end > 0) {
        std::size_t start = end - 1;
        while (start > 0 && !breaks[start])
            --start;
        if (segment_is_word(text, start, end))
            return start;
        end = start;
    }
    return 0;
}

std::size_t next_word_end(std::u32string_view text, std::size_t at)
{
    at = std::min(at, text.size());
    std::vector<bool> const breaks = word_boundaries(text);
    std::size_t start = at;
    while (start < text.size()) {
        std::size_t end = start + 1;
        while (end < text.size() && !breaks[end])
            ++end;
        if (segment_is_word(text, start, end))
            return end;
        start = end;
    }
    return text.size();
}

std::pair<std::size_t, std::size_t> word_at(std::u32string_view text, std::size_t at)
{
    if (text.empty())
        return { 0, 0 };
    at = std::min(at, text.size());
    std::vector<bool> const breaks = word_boundaries(text);
    // At a boundary the segment after it, unless it is no word and the one
    // before is (a double click just past a word's end takes the word), or
    // the end of the text.
    auto const segment_from = [&](std::size_t i) {
        std::size_t start = i;
        while (start > 0 && !breaks[start])
            --start;
        std::size_t end = i + 1;
        while (end < text.size() && !breaks[end])
            ++end;
        return std::make_pair(start, end);
    };
    if (at == text.size())
        return segment_from(at - 1);
    std::pair<std::size_t, std::size_t> const here = segment_from(at);
    if (breaks[at] && at > 0 && !segment_is_word(text, here.first, here.second)) {
        std::pair<std::size_t, std::size_t> const before = segment_from(at - 1);
        if (segment_is_word(text, before.first, before.second) && (text[at] == U' ' || text[at] == U'\n'))
            return before;
    }
    return here;
}

std::pair<std::size_t, std::size_t> line_at(std::u32string_view text, std::size_t at)
{
    at = std::min(at, text.size());
    std::size_t start = at;
    while (start > 0 && text[start - 1] != U'\n')
        --start;
    std::size_t end = at;
    while (end < text.size() && text[end] != U'\n')
        ++end;
    return { start, end };
}

dom::forms::TextSelection moved(dom::forms::TextSelection const& from, std::size_t to, bool extend)
{
    using Direction = dom::forms::TextSelection::Direction;
    dom::forms::TextSelection out;
    if (!extend) {
        out.start = out.end = to;
        out.direction = Direction::None;
        return out;
    }
    std::size_t const anchor = from.anchor();
    out.start = std::min(anchor, to);
    out.end = std::max(anchor, to);
    out.direction = to < anchor ? Direction::Backward : Direction::Forward;
    return out;
}

Edit replace(std::u32string_view text, std::size_t start, std::size_t end, std::u32string_view with)
{
    start = std::min(start, text.size());
    end = std::clamp(end, start, text.size());
    Edit out;
    out.text.reserve(text.size() - (end - start) + with.size());
    out.text.append(text.substr(0, start));
    out.text.append(with);
    out.text.append(text.substr(end));
    out.caret = start + with.size();
    return out;
}

std::u32string_view within_max_length(std::u32string_view text, std::size_t start, std::size_t end,
    std::u32string_view inserted, std::optional<std::size_t> limit)
{
    if (!limit)
        return inserted;
    auto const units = [](std::u32string_view part) {
        std::size_t count = 0;
        for (char32_t const c : part)
            count += c > 0xFFFF ? 2 : 1;
        return count;
    };
    start = std::min(start, text.size());
    end = std::clamp(end, start, text.size());
    std::size_t const kept = units(text.substr(0, start)) + units(text.substr(end));
    if (kept >= *limit)
        return inserted.substr(0, 0);
    std::size_t room = *limit - kept;
    std::size_t taken = 0;
    while (taken < inserted.size()) {
        std::size_t const width = inserted[taken] > 0xFFFF ? 2 : 1;
        if (width > room)
            break;
        room -= width;
        ++taken;
    }
    return inserted.substr(0, taken);
}

}
