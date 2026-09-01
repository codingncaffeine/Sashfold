#pragma once

// A small JSON reader. First consumer: the html5lib conformance fixtures in
// the test suite; later consumers: bookmarks/history storage. Strings are
// stored as UTF-8 (WTF-8-permissive, so fixture files that smuggle lone
// surrogates through \uXXXX escapes survive a round trip).

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<JsonValue>;
    using Member = std::pair<std::string, JsonValue>;
    using Object = std::vector<Member>; // insertion order preserved

    // Returns nullopt on malformed input (trailing garbage included).
    static std::optional<JsonValue> parse(std::string_view);

    JsonValue() = default;

    Type type() const { return m_type; }
    bool is_null() const { return m_type == Type::Null; }
    bool is_bool() const { return m_type == Type::Bool; }
    bool is_number() const { return m_type == Type::Number; }
    bool is_string() const { return m_type == Type::String; }
    bool is_array() const { return m_type == Type::Array; }
    bool is_object() const { return m_type == Type::Object; }

    bool as_bool() const { return m_bool; }
    double as_number() const { return m_number; }
    std::string const& as_string() const { return m_string; }
    Array const& as_array() const { return m_array; }
    Object const& as_object() const { return m_object; }

    // Object lookup by key; nullptr when absent or not an object.
    JsonValue const* get(std::string_view key) const;

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    Array m_array;
    Object m_object;

    friend class JsonParser;
};

}
