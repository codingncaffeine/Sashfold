#include "platform/linux/AtspiText.h"

#include <algorithm>

namespace sashfold::platform::atspi {

namespace {

bool continuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }

std::u32string decode(std::string_view text)
{
    std::u32string out;
    std::size_t i = 0;
    while (i < text.size()) {
        auto const lead = static_cast<unsigned char>(text[i]);
        std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : 4;
        char32_t code = length == 1 ? lead : length == 2 ? (lead & 0x1f) : length == 3 ? (lead & 0x0f) : (lead & 0x07);
        std::size_t k = 1;
        for (; k < length && i + k < text.size() && continuation(static_cast<unsigned char>(text[i + k])); ++k)
            code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        out += code;
        i += k;
    }
    return out;
}

// A character that is part of a word: letters and digits (every
// non-ASCII character but spaces, punctuation blocks and the embedded
// object character counts as one).
bool word_character(char32_t c)
{
    if (c < 0x80)
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (c == 0xa0 || c == 0xfffc || (c >= 0x2000 && c <= 0x206f) || (c >= 0x3000 && c <= 0x303f) || (c >= 0xff00 && c <= 0xff0f))
        return false;
    return true;
}

bool space(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0xa0 || c == 0xfffc; }
bool sentence_end(char32_t c) { return c == '.' || c == '!' || c == '?' || c == 0x3002; }

std::vector<std::size_t> boundaries(std::u32string const& text, std::vector<Range> const& lines, std::uint32_t boundary)
{
    std::size_t const length = text.size();
    std::vector<std::size_t> at;
    switch (boundary) {
    case 1: // word start
        for (std::size_t i = 0; i < length; ++i) {
            if (word_character(text[i]) && (i == 0 || !word_character(text[i - 1])))
                at.push_back(i);
        }
        break;
    case 2: // word end
        for (std::size_t i = 1; i <= length; ++i) {
            if (word_character(text[i - 1]) && (i == length || !word_character(text[i])))
                at.push_back(i);
        }
        break;
    case 3: // sentence start
    case 4: { // sentence end
        std::size_t i = 0;
        while (i < length) {
            if (sentence_end(text[i]) || text[i] == '\n') {
                std::size_t end = i + 1;
                while (end < length && sentence_end(text[end]))
                    ++end;
                std::size_t next = end;
                while (next < length && space(text[next]))
                    ++next;
                if (end == length || space(text[end]) || text[i] == '\n')
                    at.push_back(boundary == 3 ? next : end);
                i = next > i ? next : i + 1;
            } else {
                ++i;
            }
        }
        break;
    }
    case 5: // line start
        for (Range const& line : lines)
            at.push_back(line.start);
        break;
    case 6: // line end
        for (Range const& line : lines)
            at.push_back(line.end);
        break;
    default: // characters
        for (std::size_t i = 0; i <= length; ++i)
            at.push_back(i);
        break;
    }
    at.push_back(0);
    at.push_back(length);
    std::ranges::sort(at);
    auto const repeated = std::ranges::unique(at);
    at.erase(repeated.begin(), repeated.end());
    return at;
}

Range around(std::vector<std::size_t> const& at, std::size_t offset)
{
    // The last boundary at or before the offset, and the first after it.
    auto const after = std::ranges::upper_bound(at, offset);
    std::size_t const start = after == at.begin() ? 0 : *(after - 1);
    std::size_t const end = after == at.end() ? at.back() : *after;
    return { start, std::max(start, end) };
}

}

std::size_t code_point_count(std::string_view text)
{
    std::size_t count = 0;
    for (char const c : text) {
        if (!continuation(static_cast<unsigned char>(c)))
            ++count;
    }
    return count;
}

std::size_t byte_of(std::string_view text, std::size_t index)
{
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (continuation(static_cast<unsigned char>(text[i])))
            continue;
        if (seen == index)
            return i;
        ++seen;
    }
    return text.size();
}

std::string slice(std::string_view text, std::size_t from, std::size_t to)
{
    std::size_t const start = byte_of(text, from);
    std::size_t const end = to <= from ? start : byte_of(text, to);
    return std::string(text.substr(start, end - start));
}

char32_t character_at(std::string_view text, std::size_t index)
{
    std::size_t const at = byte_of(text, index);
    if (at >= text.size())
        return 0;
    std::u32string const one = decode(text.substr(at, std::min<std::size_t>(4, text.size() - at)));
    return one.empty() ? 0 : one.front();
}

std::uint32_t code_points_before(std::string_view text, std::uint32_t utf16)
{
    std::uint32_t units = 0;
    std::uint32_t points = 0;
    for (char32_t const c : decode(text)) {
        if (units >= utf16)
            break;
        units += c >= 0x10000 ? 2 : 1;
        ++points;
    }
    return points;
}

std::uint32_t utf16_before(std::string_view text, std::uint32_t code_points)
{
    std::uint32_t units = 0;
    std::uint32_t points = 0;
    for (char32_t const c : decode(text)) {
        if (points >= code_points)
            break;
        units += c >= 0x10000 ? 2 : 1;
        ++points;
    }
    return units;
}

void FlatText::append_text(std::string_view run, a11y::NodeId node, std::size_t)
{
    std::size_t const count = code_point_count(run);
    pieces.push_back({ node, length, length + count, false });
    text += run;
    length += count;
}

void FlatText::append_object(a11y::NodeId node)
{
    pieces.push_back({ node, length, length + 1, true });
    text += embedded_object;
    length += 1;
}

FlatText::Piece const* FlatText::piece_at(std::size_t offset) const
{
    for (Piece const& piece : pieces) {
        if (offset >= piece.start && offset < piece.end)
            return &piece;
    }
    return nullptr;
}

std::vector<FlatText::Piece const*> FlatText::objects() const
{
    std::vector<Piece const*> found;
    for (Piece const& piece : pieces) {
        if (piece.object)
            found.push_back(&piece);
    }
    return found;
}

TextChange diff_text(std::string_view before, std::string_view after)
{
    std::u32string const a = decode(before);
    std::u32string const b = decode(after);
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
        ++suffix;
    TextChange change;
    change.start = prefix;
    change.removed_length = a.size() - prefix - suffix;
    change.inserted_length = b.size() - prefix - suffix;
    change.removed = slice(before, prefix, prefix + change.removed_length);
    change.inserted = slice(after, prefix, prefix + change.inserted_length);
    return change;
}

std::vector<Range> lines_by_newline(std::string_view text)
{
    std::u32string const characters = decode(text);
    std::vector<Range> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < characters.size(); ++i) {
        if (characters[i] == '\n') {
            lines.push_back({ start, i + 1 });
            start = i + 1;
        }
    }
    lines.push_back({ start, characters.size() });
    return lines;
}

Range boundary_at(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t boundary)
{
    std::u32string const characters = decode(text);
    offset = std::min(offset, characters.size());
    if (boundary == 0)
        return { offset, std::min(offset + 1, characters.size()) };
    return around(boundaries(characters, lines, boundary), offset);
}

Range boundary_before(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t boundary)
{
    Range const here = boundary_at(text, lines, offset, boundary);
    if (here.start == 0)
        return { 0, 0 };
    if (boundary == 0)
        return { here.start - 1, here.start };
    return boundary_at(text, lines, here.start - 1, boundary);
}

Range boundary_after(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t boundary)
{
    Range const here = boundary_at(text, lines, offset, boundary);
    std::size_t const length = code_point_count(text);
    if (here.end >= length)
        return { length, length };
    return boundary_at(text, lines, here.end, boundary);
}

Range granule_at(std::string_view text, std::vector<Range> const& lines, std::size_t offset, std::uint32_t granularity)
{
    std::u32string const characters = decode(text);
    offset = std::min(offset, characters.size());
    switch (granularity) {
    case 1: { // the word holding the offset, or the spaces between two
        if (offset < characters.size() && word_character(characters[offset])) {
            std::size_t start = offset;
            while (start > 0 && word_character(characters[start - 1]))
                --start;
            std::size_t end = offset;
            while (end < characters.size() && word_character(characters[end]))
                ++end;
            return { start, end };
        }
        std::size_t start = offset;
        while (start > 0 && !word_character(characters[start - 1]))
            --start;
        std::size_t end = offset;
        while (end < characters.size() && !word_character(characters[end]))
            ++end;
        return { start, end };
    }
    case 2: { // the sentence, without the spaces after it
        Range const sentence = boundary_at(text, lines, offset, 3);
        std::size_t end = sentence.end;
        while (end > sentence.start && space(characters[end - 1]))
            --end;
        return { sentence.start, end };
    }
    case 3: { // the line, without its newline
        Range const line = boundary_at(text, lines, offset, 5);
        std::size_t end = line.end;
        if (end > line.start && characters[end - 1] == '\n')
            --end;
        return { line.start, end };
    }
    case 4: { // the paragraph: between newlines
        std::vector<Range> const paragraphs = lines_by_newline(text);
        for (Range const& paragraph : paragraphs) {
            if (offset >= paragraph.start && (offset < paragraph.end || paragraph.end == characters.size())) {
                std::size_t end = paragraph.end;
                if (end > paragraph.start && characters[end - 1] == '\n')
                    --end;
                return { paragraph.start, end };
            }
        }
        return { offset, offset };
    }
    default:
        return { offset, std::min(offset + 1, characters.size()) };
    }
}

}
