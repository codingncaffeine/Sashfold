#pragma once

// The CSS tokenizer, css-syntax-3 §3.3 (preprocessing) + §4 (tokenization),
// implemented from the current editor's draft — including the gated
// unicode-range consumption (§4.3.1 "unicode ranges allowed").

#include "css/Token.h"

#include <string>
#include <string_view>
#include <vector>

namespace sashfold::css {

class Tokenizer {
public:
    // Takes UTF-8; decoding errors become U+FFFD. Preprocessing (§3.3):
    // CR / FF / CRLF -> LF, NULL and surrogates -> U+FFFD.
    explicit Tokenizer(std::string_view utf8);

    // §4.3.1. Consume a token.
    Token next(bool unicode_ranges_allowed = false);

    bool at_end() const { return m_position >= m_data.size(); }

    // Convenience: the whole stream, EOF token excluded.
    static std::vector<Token> tokenize(std::string_view utf8, bool unicode_ranges_allowed = false);

private:
    char32_t peek(std::size_t offset = 0) const; // 0 = next input code point
    char32_t consume(); // advances; returns the consumed code point
    void reconsume() { --m_position; }
    bool has(std::size_t count) const { return m_position + count <= m_data.size(); }

    void consume_comments(); // §4.3.2
    Token consume_numeric(); // §4.3.3
    Token consume_ident_like(); // §4.3.4
    Token consume_string(char32_t ending); // §4.3.5
    Token consume_url(); // §4.3.6
    char32_t consume_escaped(); // §4.3.7
    Token consume_unicode_range(); // §4.3.14
    void consume_bad_url_remnants(); // §4.3.15

    bool next_is_valid_escape() const; // §4.3.8 at the next two code points
    bool would_start_ident(std::size_t offset = 0) const; // §4.3.9 at offset
    bool would_start_number(std::size_t offset = 0) const; // §4.3.10 at offset
    bool would_start_unicode_range() const; // §4.3.11 (current code point = U/u)

    std::string consume_ident_sequence(); // §4.3.12, returns UTF-8
    double consume_number(Token::NumericType& type_out); // §4.3.13

    std::u32string m_data;
    std::size_t m_position = 0;
};

}
