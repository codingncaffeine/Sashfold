#include "core/Json.h"

#include "core/Ascii.h"
#include "core/Unicode.h"

#include <charconv>

namespace sashfold {

JsonValue const* JsonValue::get(std::string_view key) const
{
    if (m_type != Type::Object)
        return nullptr;
    for (Member const& member : m_object) {
        if (member.first == key)
            return &member.second;
    }
    return nullptr;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text)
        : m_text(text)
    {
    }

    std::optional<JsonValue> parse_document()
    {
        skip_whitespace();
        std::optional<JsonValue> value = parse_value();
        if (!value)
            return std::nullopt;
        skip_whitespace();
        if (m_pos != m_text.size())
            return std::nullopt; // trailing garbage
        return value;
    }

private:
    void skip_whitespace()
    {
        while (m_pos < m_text.size()) {
            char const c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++m_pos;
        }
    }

    bool consume(char expected)
    {
        if (m_pos < m_text.size() && m_text[m_pos] == expected) {
            ++m_pos;
            return true;
        }
        return false;
    }

    bool consume_literal(std::string_view literal)
    {
        if (m_text.substr(m_pos, literal.size()) != literal)
            return false;
        m_pos += literal.size();
        return true;
    }

    std::optional<JsonValue> parse_value()
    {
        if (m_pos >= m_text.size())
            return std::nullopt;

        char const c = m_text[m_pos];
        if (c == '{')
            return parse_object();
        if (c == '[')
            return parse_array();
        if (c == '"')
            return parse_string_value();
        if (c == 't') {
            if (!consume_literal("true"))
                return std::nullopt;
            JsonValue value;
            value.m_type = JsonValue::Type::Bool;
            value.m_bool = true;
            return value;
        }
        if (c == 'f') {
            if (!consume_literal("false"))
                return std::nullopt;
            JsonValue value;
            value.m_type = JsonValue::Type::Bool;
            value.m_bool = false;
            return value;
        }
        if (c == 'n') {
            if (!consume_literal("null"))
                return std::nullopt;
            return JsonValue {};
        }
        return parse_number();
    }

    std::optional<JsonValue> parse_object()
    {
        ++m_pos; // '{'
        JsonValue value;
        value.m_type = JsonValue::Type::Object;

        skip_whitespace();
        if (consume('}'))
            return value;

        while (true) {
            skip_whitespace();
            std::optional<std::string> key = parse_string();
            if (!key)
                return std::nullopt;
            skip_whitespace();
            if (!consume(':'))
                return std::nullopt;
            skip_whitespace();
            std::optional<JsonValue> member = parse_value();
            if (!member)
                return std::nullopt;
            value.m_object.emplace_back(std::move(*key), std::move(*member));
            skip_whitespace();
            if (consume(','))
                continue;
            if (consume('}'))
                return value;
            return std::nullopt;
        }
    }

    std::optional<JsonValue> parse_array()
    {
        ++m_pos; // '['
        JsonValue value;
        value.m_type = JsonValue::Type::Array;

        skip_whitespace();
        if (consume(']'))
            return value;

        while (true) {
            skip_whitespace();
            std::optional<JsonValue> element = parse_value();
            if (!element)
                return std::nullopt;
            value.m_array.push_back(std::move(*element));
            skip_whitespace();
            if (consume(','))
                continue;
            if (consume(']'))
                return value;
            return std::nullopt;
        }
    }

    std::optional<JsonValue> parse_string_value()
    {
        std::optional<std::string> text = parse_string();
        if (!text)
            return std::nullopt;
        JsonValue value;
        value.m_type = JsonValue::Type::String;
        value.m_string = std::move(*text);
        return value;
    }

    std::optional<unsigned> parse_hex4()
    {
        if (m_pos + 4 > m_text.size())
            return std::nullopt;
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            char32_t const c = static_cast<unsigned char>(m_text[m_pos + static_cast<std::size_t>(i)]);
            if (!is_ascii_hex_digit(c))
                return std::nullopt;
            result = result * 16 + hex_digit_value(c);
        }
        m_pos += 4;
        return result;
    }

    std::optional<std::string> parse_string()
    {
        if (!consume('"'))
            return std::nullopt;

        std::string out;
        while (true) {
            if (m_pos >= m_text.size())
                return std::nullopt;
            char const c = m_text[m_pos];
            if (c == '"') {
                ++m_pos;
                return out;
            }
            if (c == '\\') {
                ++m_pos;
                if (m_pos >= m_text.size())
                    return std::nullopt;
                char const escape = m_text[m_pos++];
                switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::optional<unsigned> unit = parse_hex4();
                    if (!unit)
                        return std::nullopt;
                    char32_t code_point = *unit;
                    // Combine a valid surrogate pair; a lone surrogate is
                    // kept as-is (WTF-8) for fixture round-trips.
                    if (code_point >= 0xD800 && code_point <= 0xDBFF
                        && m_pos + 1 < m_text.size() && m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u') {
                        std::size_t const saved = m_pos;
                        m_pos += 2;
                        std::optional<unsigned> low = parse_hex4();
                        if (low && *low >= 0xDC00 && *low <= 0xDFFF)
                            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (*low - 0xDC00);
                        else
                            m_pos = saved;
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    return std::nullopt;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return std::nullopt; // raw control character
            out.push_back(c);
            ++m_pos;
        }
    }

    std::optional<JsonValue> parse_number()
    {
        std::size_t const start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-')
            ++m_pos;
        while (m_pos < m_text.size()
            && (is_ascii_digit(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '.'
                || m_text[m_pos] == 'e' || m_text[m_pos] == 'E' || m_text[m_pos] == '+'
                || m_text[m_pos] == '-'))
            ++m_pos;
        if (m_pos == start)
            return std::nullopt;

        double parsed = 0;
        auto const [end, error] = std::from_chars(m_text.data() + start, m_text.data() + m_pos, parsed);
        if (error != std::errc {} || end != m_text.data() + m_pos)
            return std::nullopt;

        JsonValue value;
        value.m_type = JsonValue::Type::Number;
        value.m_number = parsed;
        return value;
    }

    std::string_view m_text;
    std::size_t m_pos = 0;
};

std::optional<JsonValue> JsonValue::parse(std::string_view text)
{
    return JsonParser(text).parse_document();
}

}
