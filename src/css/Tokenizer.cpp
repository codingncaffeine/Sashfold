#include "css/Tokenizer.h"

#include "core/Ascii.h"
#include "core/Unicode.h"

#include <charconv>

namespace sashfold::css {

namespace {

constexpr char32_t eof_sentinel = 0xFFFFFFFF;

bool is_css_whitespace(char32_t c)
{
    // After preprocessing, only LF, TAB, and SPACE remain.
    return c == U'\n' || c == U'\t' || c == U' ';
}

bool is_digit(char32_t c)
{
    return c >= U'0' && c <= U'9';
}

bool is_hex_digit(char32_t c)
{
    return is_digit(c) || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F');
}

bool is_letter(char32_t c)
{
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

// §4.2 non-ASCII ident code point — the enumerated set of the current draft.
bool is_non_ascii_ident(char32_t c)
{
    return c == 0xB7
        || (c >= 0xC0 && c <= 0xD6)
        || (c >= 0xD8 && c <= 0xF6)
        || (c >= 0xF8 && c <= 0x37D)
        || (c >= 0x37F && c <= 0x1FFF)
        || c == 0x200C || c == 0x200D || c == 0x203F || c == 0x2040
        || (c >= 0x2070 && c <= 0x218F)
        || (c >= 0x2C00 && c <= 0x2FEF)
        || (c >= 0x3001 && c <= 0xD7FF)
        || (c >= 0xF900 && c <= 0xFDCF)
        || (c >= 0xFDF0 && c <= 0xFFFD)
        || (c >= 0x10000 && c <= 0x10FFFF); // bounded: the EOF sentinel is not an ident
}

bool is_ident_start(char32_t c)
{
    return is_letter(c) || is_non_ascii_ident(c) || c == U'_';
}

bool is_ident(char32_t c)
{
    return is_ident_start(c) || is_digit(c) || c == U'-';
}

bool is_non_printable(char32_t c)
{
    return c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1F) || c == 0x7F;
}

bool is_valid_escape(char32_t first, char32_t second)
{
    return first == U'\\' && second != U'\n';
}

int hex_value(char32_t c)
{
    if (is_digit(c))
        return static_cast<int>(c - U'0');
    if (c >= U'a' && c <= U'f')
        return static_cast<int>(c - U'a' + 10);
    return static_cast<int>(c - U'A' + 10);
}

Token simple_token(Token::Type type)
{
    Token token;
    token.type = type;
    return token;
}

} // namespace

Tokenizer::Tokenizer(std::string_view utf8)
{
    // §3.3 preprocessing: CR / FF / CRLF -> LF; NULL and surrogates -> U+FFFD
    // (decode_utf8 already replaces surrogates and invalid bytes).
    std::u32string decoded = decode_utf8(utf8);
    m_data.reserve(decoded.size());
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        char32_t const c = decoded[i];
        if (c == U'\r') {
            m_data.push_back(U'\n');
            if (i + 1 < decoded.size() && decoded[i + 1] == U'\n')
                ++i;
        } else if (c == U'\f') {
            m_data.push_back(U'\n');
        } else if (c == U'\0') {
            m_data.push_back(replacement_character);
        } else {
            m_data.push_back(c);
        }
    }
}

char32_t Tokenizer::peek(std::size_t offset) const
{
    if (m_position + offset >= m_data.size())
        return eof_sentinel;
    return m_data[m_position + offset];
}

char32_t Tokenizer::consume()
{
    if (m_position >= m_data.size()) {
        ++m_position; // keep reconsume() an unconditional decrement
        return eof_sentinel;
    }
    return m_data[m_position++];
}

// §4.3.2. Consume comments.
void Tokenizer::consume_comments()
{
    while (peek(0) == U'/' && peek(1) == U'*') {
        m_position += 2;
        while (m_position < m_data.size()) {
            if (peek(0) == U'*' && peek(1) == U'/') {
                m_position += 2;
                break;
            }
            ++m_position;
        }
        // EOF inside a comment: parse error, comment just ends.
    }
}

// §4.3.8, applied at the next two input code points.
bool Tokenizer::next_is_valid_escape() const
{
    return is_valid_escape(peek(0), peek(1));
}

// §4.3.9 at the given offset from the next input code point.
bool Tokenizer::would_start_ident(std::size_t offset) const
{
    char32_t const first = peek(offset);
    if (first == U'-') {
        char32_t const second = peek(offset + 1);
        return is_ident_start(second) || second == U'-'
            || is_valid_escape(second, peek(offset + 2));
    }
    if (is_ident_start(first))
        return true;
    return is_valid_escape(first, peek(offset + 1));
}

// §4.3.10 at the given offset from the next input code point.
bool Tokenizer::would_start_number(std::size_t offset) const
{
    char32_t const first = peek(offset);
    if (first == U'+' || first == U'-') {
        if (is_digit(peek(offset + 1)))
            return true;
        return peek(offset + 1) == U'.' && is_digit(peek(offset + 2));
    }
    if (first == U'.')
        return is_digit(peek(offset + 1));
    return is_digit(first);
}

// §4.3.11: the current code point is U/u; true when the next two are
// "+" then a hex digit or "?".
bool Tokenizer::would_start_unicode_range() const
{
    return peek(0) == U'+' && (is_hex_digit(peek(1)) || peek(1) == U'?');
}

// §4.3.7. Consume an escaped code point ("\" already consumed, escape valid).
char32_t Tokenizer::consume_escaped()
{
    char32_t const c = consume();
    if (c == eof_sentinel)
        return replacement_character; // parse error
    if (!is_hex_digit(c))
        return c;
    char32_t value = static_cast<char32_t>(hex_value(c));
    for (int i = 0; i < 5 && is_hex_digit(peek(0)); ++i)
        value = value * 16 + static_cast<char32_t>(hex_value(consume()));
    if (is_css_whitespace(peek(0)))
        consume();
    if (value == 0 || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF)
        return replacement_character;
    return value;
}

// §4.3.12. Consume an ident sequence.
std::string Tokenizer::consume_ident_sequence()
{
    std::u32string result;
    while (true) {
        char32_t const c = consume();
        if (is_ident(c)) {
            result.push_back(c);
        } else if (is_valid_escape(c, peek(0))) {
            result.push_back(consume_escaped());
        } else {
            reconsume();
            return to_utf8(result);
        }
    }
}

// §4.3.13. Consume a number.
double Tokenizer::consume_number(Token::NumericType& type_out)
{
    type_out = Token::NumericType::Integer;
    std::string repr;
    if (peek(0) == U'+' || peek(0) == U'-')
        repr += static_cast<char>(consume());
    while (is_digit(peek(0)))
        repr += static_cast<char>(consume());
    if (peek(0) == U'.' && is_digit(peek(1))) {
        repr += static_cast<char>(consume());
        while (is_digit(peek(0)))
            repr += static_cast<char>(consume());
        type_out = Token::NumericType::Number;
    }
    if ((peek(0) == U'e' || peek(0) == U'E')
        && (is_digit(peek(1)) || ((peek(1) == U'+' || peek(1) == U'-') && is_digit(peek(2))))) {
        repr += static_cast<char>(consume());
        if (peek(0) == U'+' || peek(0) == U'-')
            repr += static_cast<char>(consume());
        while (is_digit(peek(0)))
            repr += static_cast<char>(consume());
        type_out = Token::NumericType::Number;
    }
    double value = 0;
    std::string_view view = repr;
    if (!view.empty() && view.front() == '+')
        view.remove_prefix(1); // from_chars rejects a leading plus
    std::from_chars(view.data(), view.data() + view.size(), value);
    return value;
}

// §4.3.3. Consume a numeric token.
Token Tokenizer::consume_numeric()
{
    Token token;
    token.numeric_value = consume_number(token.numeric_type);
    if (would_start_ident()) {
        token.type = Token::Type::Dimension;
        token.unit = consume_ident_sequence();
        return token;
    }
    if (peek(0) == U'%') {
        consume();
        token.type = Token::Type::Percentage;
        return token;
    }
    token.type = Token::Type::Number;
    return token;
}

// §4.3.5. Consume a string token (the opening quote already consumed).
Token Tokenizer::consume_string(char32_t ending)
{
    Token token;
    token.type = Token::Type::String;
    std::u32string value;
    while (true) {
        char32_t const c = consume();
        if (c == ending)
            break;
        if (c == eof_sentinel) {
            reconsume();
            break; // parse error, still a <string-token>
        }
        if (c == U'\n') {
            reconsume();
            token.type = Token::Type::BadString; // parse error
            token.value.clear();
            return token;
        }
        if (c == U'\\') {
            if (peek(0) == eof_sentinel)
                continue; // escape before EOF: nothing
            if (peek(0) == U'\n') {
                consume(); // escaped newline
                continue;
            }
            value.push_back(consume_escaped());
            continue;
        }
        value.push_back(c);
    }
    token.value = to_utf8(value);
    return token;
}

// §4.3.15. Consume the remnants of a bad url.
void Tokenizer::consume_bad_url_remnants()
{
    while (true) {
        char32_t const c = consume();
        if (c == U')' || c == eof_sentinel) {
            if (c == eof_sentinel)
                reconsume();
            return;
        }
        if (is_valid_escape(c, peek(0)))
            consume_escaped();
    }
}

// §4.3.6. Consume a url token ("url(" and any following whitespace of a
// quoteless url already handled by the caller consuming into position).
Token Tokenizer::consume_url()
{
    Token token;
    token.type = Token::Type::Url;
    std::u32string value;
    while (is_css_whitespace(peek(0)))
        consume();
    while (true) {
        char32_t const c = consume();
        if (c == U')')
            break;
        if (c == eof_sentinel) {
            reconsume();
            break; // parse error, still a <url-token>
        }
        if (is_css_whitespace(c)) {
            while (is_css_whitespace(peek(0)))
                consume();
            char32_t const after = peek(0);
            if (after == U')' || after == eof_sentinel) {
                consume();
                if (after == eof_sentinel)
                    reconsume();
                break;
            }
            consume_bad_url_remnants();
            token.type = Token::Type::BadUrl;
            return token;
        }
        if (c == U'"' || c == U'\'' || c == U'(' || is_non_printable(c)) {
            consume_bad_url_remnants();
            token.type = Token::Type::BadUrl; // parse error
            return token;
        }
        if (c == U'\\') {
            if (is_valid_escape(c, peek(0))) {
                value.push_back(consume_escaped());
                continue;
            }
            consume_bad_url_remnants();
            token.type = Token::Type::BadUrl; // parse error
            return token;
        }
        value.push_back(c);
    }
    token.value = to_utf8(value);
    return token;
}

// §4.3.4. Consume an ident-like token.
Token Tokenizer::consume_ident_like()
{
    std::string const name = consume_ident_sequence();
    if (peek(0) == U'(') {
        consume();
        if (name.size() == 3 && ascii_ci_equals(name, "url")) {
            // Peek past whitespace: a quote makes it a <function-token>.
            while (is_css_whitespace(peek(0)) && is_css_whitespace(peek(1)))
                consume();
            char32_t const first = peek(0);
            char32_t const second = peek(1);
            if (first == U'"' || first == U'\''
                || (is_css_whitespace(first) && (second == U'"' || second == U'\''))) {
                Token token;
                token.type = Token::Type::Function;
                token.value = name;
                return token;
            }
            return consume_url();
        }
        Token token;
        token.type = Token::Type::Function;
        token.value = name;
        return token;
    }
    Token token;
    token.type = Token::Type::Ident;
    token.value = name;
    return token;
}

// §4.3.14. Consume a unicode-range token (only under unicode ranges allowed;
// the initial U/u has been reconsumed by the caller).
Token Tokenizer::consume_unicode_range()
{
    consume(); // U or u
    consume(); // +
    char32_t start = 0;
    int consumed = 0;
    while (consumed < 6 && is_hex_digit(peek(0))) {
        start = start * 16 + static_cast<char32_t>(hex_value(consume()));
        ++consumed;
    }
    Token token;
    token.type = Token::Type::UnicodeRange;
    if (consumed < 6 && peek(0) == U'?') {
        char32_t end = start;
        while (consumed < 6 && peek(0) == U'?') {
            consume();
            start = start * 16;
            end = end * 16 + 0xF;
            ++consumed;
        }
        token.range_start = start;
        token.range_end = end;
        return token;
    }
    token.range_start = start;
    token.range_end = start;
    if (peek(0) == U'-' && is_hex_digit(peek(1))) {
        consume();
        char32_t end = 0;
        int end_consumed = 0;
        while (end_consumed < 6 && is_hex_digit(peek(0))) {
            end = end * 16 + static_cast<char32_t>(hex_value(consume()));
            ++end_consumed;
        }
        token.range_end = end;
    }
    return token;
}

// §4.3.1. Consume a token.
Token Tokenizer::next(bool unicode_ranges_allowed)
{
    consume_comments();
    char32_t const c = consume();

    if (c == eof_sentinel) {
        reconsume(); // stay at EOF for repeated calls
        return simple_token(Token::Type::EndOfFile);
    }

    if (is_css_whitespace(c)) {
        while (is_css_whitespace(peek(0)))
            consume();
        return simple_token(Token::Type::Whitespace);
    }

    switch (c) {
    case U'"':
    case U'\'':
        return consume_string(c);
    case U'#':
        if (is_ident(peek(0)) || next_is_valid_escape()) {
            Token token;
            token.type = Token::Type::Hash;
            if (would_start_ident())
                token.hash_type = Token::HashType::Id;
            token.value = consume_ident_sequence();
            return token;
        }
        break;
    case U'(':
        return simple_token(Token::Type::OpenParen);
    case U')':
        return simple_token(Token::Type::CloseParen);
    case U'+':
        reconsume();
        if (would_start_number(0))
            return consume_numeric();
        consume();
        break;
    case U',':
        return simple_token(Token::Type::Comma);
    case U'-':
        reconsume();
        if (would_start_number(0))
            return consume_numeric();
        if (peek(1) == U'-' && peek(2) == U'>') {
            m_position += 3;
            return simple_token(Token::Type::CDC);
        }
        if (would_start_ident(0))
            return consume_ident_like();
        consume();
        break;
    case U'.':
        reconsume();
        if (would_start_number(0))
            return consume_numeric();
        consume();
        break;
    case U':':
        return simple_token(Token::Type::Colon);
    case U';':
        return simple_token(Token::Type::Semicolon);
    case U'<':
        if (peek(0) == U'!' && peek(1) == U'-' && peek(2) == U'-') {
            m_position += 3;
            return simple_token(Token::Type::CDO);
        }
        break;
    case U'@':
        if (would_start_ident(0)) {
            Token token;
            token.type = Token::Type::AtKeyword;
            token.value = consume_ident_sequence();
            return token;
        }
        break;
    case U'[':
        return simple_token(Token::Type::OpenSquare);
    case U'\\':
        if (is_valid_escape(c, peek(0))) {
            reconsume();
            return consume_ident_like();
        }
        break; // parse error -> delim
    case U']':
        return simple_token(Token::Type::CloseSquare);
    case U'{':
        return simple_token(Token::Type::OpenBrace);
    case U'}':
        return simple_token(Token::Type::CloseBrace);
    case U'U':
    case U'u':
        if (unicode_ranges_allowed && would_start_unicode_range()) {
            reconsume();
            return consume_unicode_range();
        }
        reconsume();
        return consume_ident_like();
    default:
        break;
    }

    if (is_digit(c)) {
        reconsume();
        return consume_numeric();
    }
    if (is_ident_start(c)) {
        reconsume();
        return consume_ident_like();
    }

    Token token;
    token.type = Token::Type::Delim;
    token.delim = c;
    return token;
}

std::vector<Token> Tokenizer::tokenize(std::string_view utf8, bool unicode_ranges_allowed)
{
    Tokenizer tokenizer(utf8);
    std::vector<Token> tokens;
    while (true) {
        Token token = tokenizer.next(unicode_ranges_allowed);
        if (token.type == Token::Type::EndOfFile)
            return tokens;
        tokens.push_back(std::move(token));
    }
}

}
