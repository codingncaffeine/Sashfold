#include "core/Base64.h"

#include <string>

namespace sashfold {

namespace {

int digit_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

bool is_ascii_whitespace(char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

}

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view input)
{
    // The forgiving-base64 preamble: strip whitespace, then padding.
    std::string data;
    data.reserve(input.size());
    for (char const c : input)
        if (!is_ascii_whitespace(c))
            data += c;
    if (data.size() % 4 == 0) {
        if (data.ends_with("=="))
            data.resize(data.size() - 2);
        else if (data.ends_with("="))
            data.resize(data.size() - 1);
    }
    if (data.size() % 4 == 1)
        return std::nullopt;

    std::vector<std::uint8_t> output;
    output.reserve(data.size() / 4 * 3 + 2);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char const c : data) {
        int const value = digit_value(c);
        if (value < 0)
            return std::nullopt;
        buffer = buffer << 6 | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits == 24) {
            output.push_back(static_cast<std::uint8_t>(buffer >> 16));
            output.push_back(static_cast<std::uint8_t>(buffer >> 8));
            output.push_back(static_cast<std::uint8_t>(buffer));
            buffer = 0;
            bits = 0;
        }
    }
    if (bits == 12) {
        output.push_back(static_cast<std::uint8_t>(buffer >> 4));
    } else if (bits == 18) {
        output.push_back(static_cast<std::uint8_t>(buffer >> 10));
        output.push_back(static_cast<std::uint8_t>(buffer >> 2));
    }
    return output;
}

}
