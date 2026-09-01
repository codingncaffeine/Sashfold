#pragma once

// A deliberately tiny assertion harness. A browser engine earns its keep on
// conformance tests, and those need to run everywhere with no fetch step, so
// the test support here stays dependency-free.

#include <concepts>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

namespace sashfold::test {

inline int g_checks = 0;
inline int g_failures = 0;

template<typename T>
concept Streamable = requires(std::ostream& os, T const& value) { os << value; };

template<typename T>
std::string describe(T const& value)
{
    if constexpr (Streamable<T>) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return "<not printable>";
    }
}

inline void fail(std::string const& message, char const* file, int line)
{
    ++g_failures;
    std::cerr << "FAIL " << file << ":" << line << "  " << message << "\n";
}

inline bool check(bool condition, char const* expression, char const* file, int line)
{
    ++g_checks;
    if (!condition)
        fail(std::string("expected true: ") + expression, file, line);
    return condition;
}

template<typename A, typename B>
bool check_eq(A const& actual, B const& expected, char const* actual_text, char const* expected_text,
    char const* file, int line)
{
    ++g_checks;
    if (actual == expected)
        return true;
    fail(std::string(actual_text) + " == " + expected_text
            + "\n       actual:   " + describe(actual)
            + "\n       expected: " + describe(expected),
        file, line);
    return false;
}

inline int report(char const* suite)
{
    if (g_failures == 0) {
        std::cout << "PASS " << suite << " (" << g_checks << " checks)\n";
        return 0;
    }
    std::cerr << "FAILED " << suite << ": " << g_failures << " of " << g_checks << " checks\n";
    return 1;
}

}

#define CHECK(expr) ::sashfold::test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
    ::sashfold::test::check_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)
