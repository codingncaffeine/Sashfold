#pragma once
// Small helpers the security-key tests share: hex both ways, and a random
// source a test can make deterministic.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::test {

inline std::vector<std::uint8_t> from_hex(std::string_view hex)
{
    std::vector<std::uint8_t> out;
    auto const nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    int high = -1;
    for (char const c : hex) {
        int const n = nibble(c);
        if (n < 0)
            continue; // spaces and line breaks in long literals
        if (high < 0) {
            high = n;
        } else {
            out.push_back(static_cast<std::uint8_t>(high << 4 | n));
            high = -1;
        }
    }
    return out;
}

inline std::string to_hex(std::span<std::uint8_t const> bytes)
{
    static char const digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t const b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

inline std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// A deterministic generator (xorshift64*) for fuzz loops.
struct TestRandom {
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    std::uint64_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545f4914f6cdd1dull;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

}
