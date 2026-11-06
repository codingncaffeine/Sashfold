#pragma once

// PDF's objects (ISO 32000-2 §7.3): null, booleans, integers, reals,
// strings, names, arrays, dictionaries, streams and indirect references.
// Arrays, dictionaries and streams are shared and never changed once
// parsed, so an object is cheap to copy and safe to read from any thread.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sashfold::pdf {

struct Ref {
    int num = 0;
    int gen = 0;
    friend constexpr bool operator==(Ref const&, Ref const&) = default;
    friend constexpr bool operator<(Ref const& a, Ref const& b)
    {
        return a.num != b.num ? a.num < b.num : a.gen < b.gen;
    }
};

struct Object;
class Dict;
struct Stream;

using Array = std::vector<Object>;

// A string's bytes, as written (literal or hexadecimal), already decrypted.
struct String {
    std::string bytes;
    bool hex = false;
};
// A name without its slash, #xx escapes decoded.
struct Name {
    std::string text;
};

struct Object {
    using Value = std::variant<std::monostate, bool, std::int64_t, double, String, Name,
        std::shared_ptr<Array const>, std::shared_ptr<Dict const>, std::shared_ptr<Stream const>, Ref>;
    Value value;

    Object() = default;
    explicit Object(Value v)
        : value(std::move(v))
    {
    }
    static Object boolean(bool b) { return Object { Value { b } }; }
    static Object integer(std::int64_t i) { return Object { Value { i } }; }
    static Object real(double r) { return Object { Value { r } }; }
    static Object string(std::string bytes, bool hex = false) { return Object { Value { String { std::move(bytes), hex } } }; }
    static Object name(std::string text) { return Object { Value { Name { std::move(text) } } }; }
    static Object array(Array items) { return Object { Value { std::make_shared<Array const>(std::move(items)) } }; }
    static Object dict(Dict d);
    static Object stream(Stream s);
    static Object ref(Ref r) { return Object { Value { r } }; }

    bool is_null() const { return std::holds_alternative<std::monostate>(value); }
    bool is_bool() const { return std::holds_alternative<bool>(value); }
    bool is_int() const { return std::holds_alternative<std::int64_t>(value); }
    bool is_number() const { return is_int() || std::holds_alternative<double>(value); }
    bool is_string() const { return std::holds_alternative<String>(value); }
    bool is_name() const { return std::holds_alternative<Name>(value); }
    bool is_name(std::string_view text) const { return is_name() && std::get<Name>(value).text == text; }
    bool is_array() const { return std::holds_alternative<std::shared_ptr<Array const>>(value); }
    bool is_dict() const { return std::holds_alternative<std::shared_ptr<Dict const>>(value); }
    bool is_stream() const { return std::holds_alternative<std::shared_ptr<Stream const>>(value); }
    bool is_ref() const { return std::holds_alternative<Ref>(value); }

    bool as_bool(bool fallback = false) const { return is_bool() ? std::get<bool>(value) : fallback; }
    std::int64_t as_int(std::int64_t fallback = 0) const;
    double as_number(double fallback = 0) const;
    std::string const& as_string() const; // empty for anything else
    std::string const& as_name() const; // empty for anything else
    Array const& as_array() const; // empty for anything else
    Dict const& as_dict() const; // a stream's dictionary too; empty for anything else
    Stream const* as_stream() const;
    Ref as_ref() const { return is_ref() ? std::get<Ref>(value) : Ref {}; }
};

// Keys in the order written; the last of a repeated key stands, as in
// every reader.
class Dict {
public:
    void set(std::string key, Object value);
    Object const* find(std::string_view key) const;
    // The value, or null.
    Object const& get(std::string_view key) const;
    bool has(std::string_view key) const { return find(key) != nullptr; }
    std::vector<std::pair<std::string, Object>> const& entries() const { return m_entries; }
    bool empty() const { return m_entries.empty(); }
    std::size_t size() const { return m_entries.size(); }

private:
    std::vector<std::pair<std::string, Object>> m_entries;
};

// A stream: its dictionary and where its raw bytes lie — a slice of the
// file, still encoded and (in an encrypted file) still encrypted. `owner`
// is the indirect object it is, which the decryption key is made from.
struct Stream {
    Dict dict;
    std::shared_ptr<std::vector<std::uint8_t> const> source;
    std::size_t start = 0;
    std::size_t length = 0;
    Ref owner;
    std::vector<std::uint8_t> raw() const;
};

inline Object Object::dict(Dict d)
{
    return Object { Value { std::make_shared<Dict const>(std::move(d)) } };
}

inline Object Object::stream(Stream s)
{
    return Object { Value { std::make_shared<Stream const>(std::move(s)) } };
}

// A text string (§7.9.2.2) as UTF-8: UTF-16BE or UTF-8 with their byte
// order marks, else PDFDocEncoding.
std::string text_string_to_utf8(std::string const& bytes);

}
