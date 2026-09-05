#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace sashfold::html {

// The tokenizer's view of the document: a decoded code-point sequence with
// the spec's input-stream preprocessing applied (CRLF and CR normalize to
// LF). Wholly in memory for now; streaming can come later without changing
// this interface.
class InputStream {
public:
    explicit InputStream(std::u32string code_points);

    // Decodes UTF-8 (invalid sequences become U+FFFD) and strips a leading
    // byte-order mark, per the byte-stream layer.
    static InputStream from_utf8(std::string_view);

    std::optional<char32_t> next(); // nullopt at end of stream, repeatable

    std::size_t position() const { return m_position; }
    void seek(std::size_t position) { m_position = position; }

    bool has(std::size_t offset = 0) const { return m_position + offset < m_data.size(); }
    char32_t peek(std::size_t offset = 0) const { return m_data[m_position + offset]; }

    // Case-insensitive (ASCII) match of the upcoming characters; consumes nothing.
    bool lookahead_equals_ignoring_case(std::u32string_view) const;
    void advance(std::size_t count) { m_position += count; }

    std::u32string_view remaining() const
    {
        return std::u32string_view(m_data).substr(m_position);
    }

    // Puts `text` just before the next character to be consumed: the
    // insertion point document.write writes at while the parser runs.
    void insert(std::u32string_view text) { m_data.insert(m_position, text); }

private:
    std::u32string m_data;
    std::size_t m_position = 0;
};

}
