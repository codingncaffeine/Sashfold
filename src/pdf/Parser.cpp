#include "pdf/Parser.h"

#include <cmath>
#include <optional>

namespace sashfold::pdf {

namespace {

constexpr int k_max_depth = 256;

int hex_value(std::uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool is_regular(std::uint8_t c)
{
    return !is_pdf_whitespace(c) && !is_pdf_delimiter(c);
}

} // namespace

bool is_pdf_whitespace(std::uint8_t c)
{
    return c == 0 || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

bool is_pdf_delimiter(std::uint8_t c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/'
        || c == '%';
}

void Lexer::skip_whitespace()
{
    while (m_position < m_data.size()) {
        std::uint8_t const c = m_data[m_position];
        if (is_pdf_whitespace(c)) {
            ++m_position;
        } else if (c == '%') {
            while (m_position < m_data.size() && m_data[m_position] != '\r' && m_data[m_position] != '\n')
                ++m_position;
        } else {
            break;
        }
    }
}

Token Lexer::peek()
{
    std::size_t const saved = m_position;
    Token token = next();
    m_position = saved;
    return token;
}

Token Lexer::next()
{
    for (;;) {
        skip_whitespace();
        Token token;
        token.start = m_position;
        if (m_position >= m_data.size())
            return token;
        std::uint8_t const c = m_data[m_position];
        if (c == '[') {
            ++m_position;
            token.kind = Token::Kind::ArrayOpen;
            return token;
        }
        if (c == ']') {
            ++m_position;
            token.kind = Token::Kind::ArrayClose;
            return token;
        }
        if (c == '<' && m_position + 1 < m_data.size() && m_data[m_position + 1] == '<') {
            m_position += 2;
            token.kind = Token::Kind::DictOpen;
            return token;
        }
        if (c == '>' && m_position + 1 < m_data.size() && m_data[m_position + 1] == '>') {
            m_position += 2;
            token.kind = Token::Kind::DictClose;
            return token;
        }
        if (c == '<') {
            ++m_position;
            token.kind = Token::Kind::HexString;
            int pending = -1;
            while (m_position < m_data.size() && m_data[m_position] != '>') {
                int const v = hex_value(m_data[m_position++]);
                if (v < 0)
                    continue;
                if (pending < 0) {
                    pending = v;
                } else {
                    token.text.push_back(static_cast<char>(pending * 16 + v));
                    pending = -1;
                }
            }
            if (pending >= 0)
                token.text.push_back(static_cast<char>(pending * 16));
            if (m_position < m_data.size())
                ++m_position;
            return token;
        }
        if (c == '(') {
            ++m_position;
            token.kind = Token::Kind::String;
            int depth = 1;
            while (m_position < m_data.size()) {
                std::uint8_t ch = m_data[m_position++];
                if (ch == '(') {
                    ++depth;
                } else if (ch == ')') {
                    if (--depth == 0)
                        break;
                } else if (ch == '\\') {
                    if (m_position >= m_data.size())
                        break;
                    ch = m_data[m_position++];
                    switch (ch) {
                    case 'n': token.text.push_back('\n'); continue;
                    case 'r': token.text.push_back('\r'); continue;
                    case 't': token.text.push_back('\t'); continue;
                    case 'b': token.text.push_back('\b'); continue;
                    case 'f': token.text.push_back('\f'); continue;
                    case '\r':
                        if (m_position < m_data.size() && m_data[m_position] == '\n')
                            ++m_position;
                        continue;
                    case '\n': continue;
                    default: break;
                    }
                    if (ch >= '0' && ch <= '7') {
                        int value = ch - '0';
                        for (int k = 0; k < 2 && m_position < m_data.size() && m_data[m_position] >= '0'
                             && m_data[m_position] <= '7';
                             ++k)
                            value = value * 8 + (m_data[m_position++] - '0');
                        token.text.push_back(static_cast<char>(value & 0xFF));
                        continue;
                    }
                    // \( \) \\ and any other escaped character stand for themselves.
                } else if (ch == '\r') {
                    // An end of line in a string is a line feed, whichever it was.
                    if (m_position < m_data.size() && m_data[m_position] == '\n')
                        ++m_position;
                    ch = '\n';
                }
                token.text.push_back(static_cast<char>(ch));
            }
            return token;
        }
        if (c == '/') {
            ++m_position;
            token.kind = Token::Kind::Name;
            while (m_position < m_data.size() && is_regular(m_data[m_position])) {
                std::uint8_t ch = m_data[m_position++];
                if (ch == '#' && m_position + 1 < m_data.size()) {
                    int const hi = hex_value(m_data[m_position]);
                    int const lo = hex_value(m_data[m_position + 1]);
                    if (hi >= 0 && lo >= 0) {
                        ch = static_cast<std::uint8_t>(hi * 16 + lo);
                        m_position += 2;
                    }
                }
                token.text.push_back(static_cast<char>(ch));
            }
            return token;
        }
        if (c == ')' || c == '>' || c == '{' || c == '}') {
            // A stray delimiter: '{' '}' are PostScript calculator braces,
            // handed on as keywords; the others are skipped.
            ++m_position;
            if (c == '{' || c == '}') {
                token.kind = Token::Kind::Keyword;
                token.text.assign(1, static_cast<char>(c));
                return token;
            }
            continue;
        }
        // A number or a keyword: a run of regular characters.
        std::size_t const begin = m_position;
        while (m_position < m_data.size() && is_regular(m_data[m_position]))
            ++m_position;
        std::string_view const word(reinterpret_cast<char const*>(m_data.data() + begin), m_position - begin);
        // Numbers: [+-]digits[.digits]; readers also take "--5", "5-" and "1.2.3" leniently.
        bool numeric = !word.empty();
        bool has_dot = false;
        bool has_digit = false;
        for (std::size_t i = 0; i < word.size(); ++i) {
            char const ch = word[i];
            if (ch >= '0' && ch <= '9')
                has_digit = true;
            else if (ch == '.')
                has_dot = true;
            else if (!((ch == '-' || ch == '+')))
                numeric = false;
        }
        if (numeric && has_digit) {
            // Sign first (repeated signs collapse), then the digits up to a second sign or dot.
            std::size_t i = 0;
            bool negative = false;
            while (i < word.size() && (word[i] == '-' || word[i] == '+')) {
                negative = negative || word[i] == '-';
                ++i;
            }
            double value = 0;
            std::int64_t integer = 0;
            bool overflow = false;
            double scale = 0;
            bool seen_dot = false;
            for (; i < word.size(); ++i) {
                char const ch = word[i];
                if (ch == '.') {
                    if (seen_dot)
                        break;
                    seen_dot = true;
                    scale = 1;
                    continue;
                }
                if (ch == '-' || ch == '+')
                    break;
                int const digit = ch - '0';
                if (seen_dot) {
                    scale /= 10;
                    value += digit * scale;
                } else {
                    value = value * 10 + digit;
                    if (integer > (INT64_MAX - digit) / 10)
                        overflow = true;
                    else
                        integer = integer * 10 + digit;
                }
            }
            if (has_dot || overflow) {
                token.kind = Token::Kind::Real;
                token.real = negative ? -value : value;
            } else {
                token.kind = Token::Kind::Integer;
                token.integer = negative ? -integer : integer;
            }
            return token;
        }
        token.kind = Token::Kind::Keyword;
        token.text = std::string(word);
        if (token.text.empty()) {
            ++m_position; // cannot happen with the checks above; never loop
            continue;
        }
        return token;
    }
}

Object Parser::parse(std::string* keyword)
{
    return parse_from_depth(m_lexer.next(), keyword, 0);
}

Object Parser::parse_from(Token first, std::string* keyword)
{
    return parse_from_depth(std::move(first), keyword, 0);
}

Dict Parser::parse_dict_body()
{
    return parse_dict(1);
}

Dict Parser::parse_dict(int depth)
{
    Dict dict;
    for (;;) {
        Token key = m_lexer.next();
        if (key.kind == Token::Kind::Eof || key.kind == Token::Kind::DictClose)
            break;
        if (key.kind != Token::Kind::Name) {
            // A value with no key: the dictionary is malformed; readers
            // skip what is not a name and go on — up to an object's end.
            if (key.kind == Token::Kind::Keyword && (key.text == "endobj" || key.text == "stream")) {
                m_lexer.seek(key.start);
                break;
            }
            continue;
        }
        std::size_t const before = m_lexer.position();
        Token value = m_lexer.next();
        if (value.kind == Token::Kind::DictClose) {
            dict.set(std::move(key.text), Object {});
            break;
        }
        if (value.kind == Token::Kind::Keyword && (value.text == "endobj" || value.text == "stream")) {
            m_lexer.seek(before);
            break;
        }
        dict.set(std::move(key.text), parse_from_depth(std::move(value), nullptr, depth + 1));
    }
    return dict;
}

Object Parser::parse_from_depth(Token first, std::string* keyword, int depth)
{
    if (depth > k_max_depth)
        return {};
    switch (first.kind) {
    case Token::Kind::Eof:
        return {};
    case Token::Kind::Real:
        return Object::real(first.real);
    case Token::Kind::Integer: {
        if (m_allow_refs && first.integer >= 0 && first.integer <= INT32_MAX) {
            // "N G R" — look two tokens ahead.
            std::size_t const saved = m_lexer.position();
            Token second = m_lexer.next();
            if (second.kind == Token::Kind::Integer && second.integer >= 0 && second.integer <= 65535) {
                Token third = m_lexer.next();
                if (third.kind == Token::Kind::Keyword && third.text == "R")
                    return Object::ref(Ref { static_cast<int>(first.integer), static_cast<int>(second.integer) });
            }
            m_lexer.seek(saved);
        }
        return Object::integer(first.integer);
    }
    case Token::Kind::String:
    case Token::Kind::HexString: {
        std::string bytes = std::move(first.text);
        if (m_filter)
            bytes = m_filter(std::move(bytes));
        return Object::string(std::move(bytes), first.kind == Token::Kind::HexString);
    }
    case Token::Kind::Name:
        return Object::name(std::move(first.text));
    case Token::Kind::ArrayOpen: {
        Array items;
        for (;;) {
            Token token = m_lexer.next();
            if (token.kind == Token::Kind::Eof || token.kind == Token::Kind::ArrayClose)
                break;
            if (token.kind == Token::Kind::DictClose)
                continue;
            if (token.kind == Token::Kind::Keyword && (token.text == "endobj" || token.text == "stream")) {
                m_lexer.seek(token.start);
                break;
            }
            items.push_back(parse_from_depth(std::move(token), nullptr, depth + 1));
        }
        return Object::array(std::move(items));
    }
    case Token::Kind::DictOpen:
        return Object::dict(parse_dict(depth));
    case Token::Kind::ArrayClose:
    case Token::Kind::DictClose:
        return {};
    case Token::Kind::Keyword:
        if (first.text == "true")
            return Object::boolean(true);
        if (first.text == "false")
            return Object::boolean(false);
        if (first.text == "null")
            return {};
        if (keyword)
            *keyword = std::move(first.text);
        return {};
    }
    return {};
}

}
