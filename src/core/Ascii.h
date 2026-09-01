#pragma once

namespace sashfold {

constexpr bool is_ascii_digit(char32_t c) { return c >= U'0' && c <= U'9'; }
constexpr bool is_ascii_upper_alpha(char32_t c) { return c >= U'A' && c <= U'Z'; }
constexpr bool is_ascii_lower_alpha(char32_t c) { return c >= U'a' && c <= U'z'; }
constexpr bool is_ascii_alpha(char32_t c) { return is_ascii_upper_alpha(c) || is_ascii_lower_alpha(c); }
constexpr bool is_ascii_alphanumeric(char32_t c) { return is_ascii_alpha(c) || is_ascii_digit(c); }

constexpr bool is_ascii_hex_digit(char32_t c)
{
    return is_ascii_digit(c) || (c >= U'A' && c <= U'F') || (c >= U'a' && c <= U'f');
}

constexpr char32_t to_ascii_lowercase(char32_t c)
{
    return is_ascii_upper_alpha(c) ? c + 0x20 : c;
}

constexpr unsigned hex_digit_value(char32_t c)
{
    if (is_ascii_digit(c))
        return static_cast<unsigned>(c - U'0');
    return static_cast<unsigned>(to_ascii_lowercase(c) - U'a') + 10u;
}

// The tokenizer's whitespace set: TAB, LF, FF, SPACE. (CR never reaches the
// tokenizer; the input stream normalizes newlines first.)
constexpr bool is_tokenizer_whitespace(char32_t c)
{
    return c == U'\t' || c == U'\n' || c == U'\f' || c == U' ';
}

}
