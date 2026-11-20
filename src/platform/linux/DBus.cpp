#include "platform/linux/DBus.h"

#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace sashfold::platform::dbus {

namespace {

void pad(std::vector<std::uint8_t>& bytes, std::size_t alignment)
{
    while (bytes.size() % alignment != 0)
        bytes.push_back(0);
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void set32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t get32(std::span<std::uint8_t const> bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8
        | static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

// The header's fields, by the numbers the specification gives them.
enum Field : std::uint8_t {
    Path = 1,
    InterfaceField = 2, // named apart from dbus::Interface, the service's
    Member = 3,
    ErrorName = 4,
    ReplySerial = 5,
    Destination = 6,
    Sender = 7,
    Signature = 8,
    UnixFds = 9,
};

// The longest message a bus passes on (2^27 bytes), and the longest array.
constexpr std::size_t longest_message = 1u << 27;
constexpr std::size_t longest_array = 1u << 26;

// The nesting the specification allows ("Valid Signatures"): 32 arrays and
// 32 structs (a dict entry counts as a struct), 64 containers in all once
// variants are counted.
constexpr int deepest_arrays = 32;
constexpr int deepest_structs = 32;
constexpr int deepest_total = 64;
constexpr std::size_t longest_signature = 255;

bool basic_type(char code) { return code != 0 && std::strchr("ybnqiuxtdsogh", code) != nullptr; }

constexpr std::size_t no_type = static_cast<std::size_t>(-1);

// The end of the complete type at `at`, or no_type when none is there or
// it nests too deep.
std::size_t parse_complete(std::string_view signature, std::size_t at, int arrays, int structs)
{
    if (at >= signature.size())
        return no_type;
    char const code = signature[at];
    if (basic_type(code) || code == 'v')
        return at + 1;
    if (code == 'a') {
        if (arrays + 1 > deepest_arrays)
            return no_type;
        if (at + 1 < signature.size() && signature[at + 1] == '{') {
            if (structs + 1 > deepest_structs)
                return no_type;
            std::size_t const key = at + 2;
            if (key >= signature.size() || !basic_type(signature[key]))
                return no_type;
            std::size_t const value_end = parse_complete(signature, key + 1, arrays + 1, structs + 1);
            if (value_end == no_type || value_end >= signature.size() || signature[value_end] != '}')
                return no_type;
            return value_end + 1;
        }
        return parse_complete(signature, at + 1, arrays + 1, structs);
    }
    if (code == '(') {
        if (structs + 1 > deepest_structs)
            return no_type;
        std::size_t next = at + 1;
        if (next < signature.size() && signature[next] == ')')
            return no_type; // a struct has at least one field
        while (next < signature.size() && signature[next] != ')') {
            next = parse_complete(signature, next, arrays, structs + 1);
            if (next == no_type)
                return no_type;
        }
        return next < signature.size() ? next + 1 : no_type;
    }
    return no_type;
}

// A path element, an interface's or a bus name's element: [A-Za-z0-9_],
// and '-' too in a bus name.
bool name_character(char c, bool dash)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (dash && c == '-');
}

}

bool valid_signature(std::string_view signature)
{
    if (signature.size() > longest_signature)
        return false;
    std::size_t at = 0;
    while (at < signature.size()) {
        at = parse_complete(signature, at, 0, 0);
        if (at == no_type)
            return false;
    }
    return true;
}

bool single_complete_type(std::string_view signature)
{
    return !signature.empty() && signature.size() <= longest_signature && parse_complete(signature, 0, 0, 0) == signature.size();
}

std::size_t complete_type_length(std::string_view signature, std::size_t at)
{
    std::size_t const end = parse_complete(signature, at, 0, 0);
    return end == no_type ? 0 : end - at;
}

std::size_t alignment_of(char code)
{
    switch (code) {
    case 'y':
    case 'g':
    case 'v':
        return 1;
    case 'n':
    case 'q':
        return 2;
    case 'x':
    case 't':
    case 'd':
    case '(':
    case '{':
        return 8;
    default:
        return 4; // b i u h s o a
    }
}

bool valid_object_path(std::string_view path)
{
    if (path.empty() || path[0] != '/')
        return false;
    if (path.size() == 1)
        return true;
    if (path.back() == '/')
        return false;
    bool element_empty = true;
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') {
            if (element_empty)
                return false;
            element_empty = true;
        } else if (!name_character(path[i], false)) {
            return false;
        } else {
            element_empty = false;
        }
    }
    return true;
}

namespace {

// Dotted elements, each of name characters and not starting with a digit
// (`digits_lead` lets a unique bus name's do).
bool dotted_name(std::string_view name, bool dash, bool digits_lead, std::size_t least_elements)
{
    if (name.empty() || name.size() > 255)
        return false;
    std::size_t elements = 0;
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t end = name.find('.', start);
        if (end == std::string_view::npos)
            end = name.size();
        std::string_view const element = name.substr(start, end - start);
        if (element.empty() || (!digits_lead && element[0] >= '0' && element[0] <= '9'))
            return false;
        for (char const c : element) {
            if (!name_character(c, dash))
                return false;
        }
        ++elements;
        start = end + 1;
    }
    return elements >= least_elements;
}

}

bool valid_interface_name(std::string_view name) { return dotted_name(name, false, false, 2); }

bool valid_member_name(std::string_view name)
{
    if (name.empty() || name.size() > 255 || (name[0] >= '0' && name[0] <= '9'))
        return false;
    for (char const c : name) {
        if (!name_character(c, false))
            return false;
    }
    return true;
}

bool valid_bus_name(std::string_view name)
{
    if (name.starts_with(':'))
        return dotted_name(name.substr(1), true, true, 2);
    return dotted_name(name, true, false, 2);
}

bool valid_utf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        auto const byte = static_cast<unsigned char>(text[i]);
        if (byte == 0)
            return false;
        if (byte < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code = 0;
        if ((byte & 0xe0) == 0xc0) {
            length = 2;
            code = byte & 0x1f;
        } else if ((byte & 0xf0) == 0xe0) {
            length = 3;
            code = byte & 0x0f;
        } else if ((byte & 0xf8) == 0xf0) {
            length = 4;
            code = byte & 0x07;
        } else {
            return false;
        }
        if (i + length > text.size())
            return false;
        for (std::size_t k = 1; k < length; ++k) {
            auto const next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        // The shortest form only, no surrogate, nothing past U+10FFFF.
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000)
            || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff)
            return false;
        i += length;
    }
    return true;
}

// --- Values ---------------------------------------------------------------------------

namespace {

Value number_value(char const* type, std::uint64_t bits)
{
    Value value;
    value.type = type;
    value.number = bits;
    return value;
}

}

Value Value::byte(std::uint8_t v) { return number_value("y", v); }
Value Value::boolean(bool v) { return number_value("b", v ? 1 : 0); }
Value Value::int16(std::int16_t v) { return number_value("n", static_cast<std::uint64_t>(static_cast<std::int64_t>(v))); }
Value Value::uint16(std::uint16_t v) { return number_value("q", v); }
Value Value::int32(std::int32_t v) { return number_value("i", static_cast<std::uint64_t>(static_cast<std::int64_t>(v))); }
Value Value::uint32(std::uint32_t v) { return number_value("u", v); }
Value Value::int64(std::int64_t v) { return number_value("x", static_cast<std::uint64_t>(v)); }
Value Value::uint64(std::uint64_t v) { return number_value("t", v); }

Value Value::float64(double v)
{
    Value value;
    value.type = "d";
    value.real = v;
    return value;
}

Value Value::string(std::string v)
{
    Value value;
    value.type = "s";
    value.text = std::move(v);
    return value;
}

Value Value::object_path(std::string v)
{
    Value value;
    value.type = "o";
    value.text = std::move(v);
    return value;
}

Value Value::signature(std::string v)
{
    Value value;
    value.type = "g";
    value.text = std::move(v);
    return value;
}

Value Value::variant(Value inner)
{
    Value value;
    value.type = "v";
    value.items.push_back(std::move(inner));
    return value;
}

Value Value::array(std::string element_type, std::vector<Value> elements)
{
    Value value;
    value.type = "a" + element_type;
    value.items = std::move(elements);
    return value;
}

Value Value::structure(std::vector<Value> fields)
{
    Value value;
    value.type = "(";
    for (Value const& field : fields)
        value.type += field.type;
    value.type += ")";
    value.items = std::move(fields);
    return value;
}

Value Value::dict_entry(Value key, Value value_of)
{
    Value value;
    value.type = "{" + key.type + value_of.type + "}";
    value.items.push_back(std::move(key));
    value.items.push_back(std::move(value_of));
    return value;
}

Value const* Value::find(std::string_view key) const
{
    if (!type.starts_with("a{"))
        return nullptr;
    for (Value const& entry : items) {
        if (entry.items.size() == 2 && entry.items[0].text == key)
            return &entry.items[1];
    }
    return nullptr;
}

// --- Writing --------------------------------------------------------------------------

void Writer::byte(std::uint8_t value) { m_bytes.push_back(value); }

void Writer::boolean(bool value) { uint32(value ? 1 : 0); }

void Writer::int16(std::int16_t value) { uint16(static_cast<std::uint16_t>(value)); }

void Writer::uint16(std::uint16_t value)
{
    pad(m_bytes, 2);
    m_bytes.push_back(static_cast<std::uint8_t>(value));
    m_bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void Writer::int32(std::int32_t value) { uint32(static_cast<std::uint32_t>(value)); }

void Writer::uint32(std::uint32_t value)
{
    pad(m_bytes, 4);
    put32(m_bytes, value);
}

void Writer::int64(std::int64_t value) { uint64(static_cast<std::uint64_t>(value)); }

void Writer::uint64(std::uint64_t value)
{
    pad(m_bytes, 8);
    for (int shift = 0; shift < 64; shift += 8)
        m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void Writer::float64(double value) { uint64(std::bit_cast<std::uint64_t>(value)); }

void Writer::reference(std::string_view bus_name, std::string_view path)
{
    align(8);
    string(bus_name);
    string(path);
}

bool Writer::variant(Value const& value)
{
    if (!single_complete_type(value.type))
        return false;
    signature(value.type);
    return this->value(value);
}

bool Writer::value(Value const& value)
{
    if (value.type.empty())
        return false;
    switch (value.type[0]) {
    case 'y': byte(static_cast<std::uint8_t>(value.number)); return value.type.size() == 1;
    case 'b': boolean(value.number != 0); return value.type.size() == 1;
    case 'n': int16(static_cast<std::int16_t>(value.number)); return value.type.size() == 1;
    case 'q': uint16(static_cast<std::uint16_t>(value.number)); return value.type.size() == 1;
    case 'i': int32(static_cast<std::int32_t>(value.number)); return value.type.size() == 1;
    case 'u':
    case 'h': uint32(static_cast<std::uint32_t>(value.number)); return value.type.size() == 1;
    case 'x': int64(static_cast<std::int64_t>(value.number)); return value.type.size() == 1;
    case 't': uint64(value.number); return value.type.size() == 1;
    case 'd': float64(value.real); return value.type.size() == 1;
    case 's': string(value.text); return value.type.size() == 1;
    case 'o': string(value.text); return value.type.size() == 1 && valid_object_path(value.text);
    case 'g': signature(value.text); return value.type.size() == 1 && valid_signature(value.text);
    case 'v': return value.type.size() == 1 && value.items.size() == 1 && variant(value.items[0]);
    case 'a': {
        std::string_view const element = std::string_view(value.type).substr(1);
        if (!single_complete_type(value.type))
            return false;
        bool fine = true;
        array(alignment_of(element[0]), [&] {
            for (Value const& item : value.items)
                fine = item.type == element && this->value(item) && fine;
        });
        return fine;
    }
    case '(':
    case '{': {
        align(8);
        std::string inner;
        bool fine = true;
        for (Value const& item : value.items) {
            inner += item.type;
            fine = this->value(item) && fine;
        }
        return fine && value.type.size() == inner.size() + 2 && std::string_view(value.type).substr(1, inner.size()) == inner;
    }
    default:
        return false;
    }
}

void Writer::string(std::string_view text)
{
    uint32(static_cast<std::uint32_t>(text.size()));
    m_bytes.insert(m_bytes.end(), text.begin(), text.end());
    m_bytes.push_back(0);
}

void Writer::signature(std::string_view text)
{
    m_bytes.push_back(static_cast<std::uint8_t>(text.size()));
    m_bytes.insert(m_bytes.end(), text.begin(), text.end());
    m_bytes.push_back(0);
}

void Writer::byte_array(std::span<std::uint8_t const> values)
{
    uint32(static_cast<std::uint32_t>(values.size()));
    m_bytes.insert(m_bytes.end(), values.begin(), values.end());
}

void Writer::align(std::size_t alignment) { pad(m_bytes, alignment); }

std::size_t Writer::begin_array(std::size_t element_alignment)
{
    pad(m_bytes, 4);
    std::size_t const length_at = m_bytes.size();
    put32(m_bytes, 0);
    // The padding before the first element is there even when the array is
    // empty, and is not counted in its length.
    pad(m_bytes, element_alignment);
    return length_at;
}

void Writer::end_array(std::size_t length_at, std::size_t element_alignment)
{
    std::size_t first = length_at + 4;
    while (first % element_alignment != 0)
        ++first;
    set32(m_bytes, length_at, static_cast<std::uint32_t>(m_bytes.size() - first));
}

void Writer::variant_string(std::string_view text)
{
    signature("s");
    string(text);
}

void Writer::variant_boolean(bool value)
{
    signature("b");
    boolean(value);
}

void Writer::variant_byte(std::uint8_t value)
{
    signature("y");
    byte(value);
}

void Writer::variant_uint32(std::uint32_t value)
{
    signature("u");
    uint32(value);
}

bool Reader::align(std::size_t alignment)
{
    while (m_at % alignment != 0)
        ++m_at;
    if (m_at > m_bytes.size())
        m_failed = true;
    return !m_failed;
}

bool Reader::skip(std::size_t count)
{
    if (m_failed || count > m_bytes.size() - m_at) {
        m_failed = true;
        return false;
    }
    m_at += count;
    return true;
}

std::optional<std::uint8_t> Reader::byte()
{
    if (m_failed || m_at >= m_bytes.size()) {
        m_failed = true;
        return std::nullopt;
    }
    return m_bytes[m_at++];
}

std::optional<std::uint32_t> Reader::uint32()
{
    if (m_failed || !align(4) || m_bytes.size() - m_at < 4) {
        m_failed = true;
        return std::nullopt;
    }
    std::uint32_t const value = get32(m_bytes, m_at);
    m_at += 4;
    return value;
}

template <typename T>
std::optional<T> Reader::fixed(std::size_t size)
{
    if (m_failed || !align(size) || m_bytes.size() - m_at < size) {
        m_failed = true;
        return std::nullopt;
    }
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < size; ++i)
        bits |= static_cast<std::uint64_t>(m_bytes[m_at + i]) << (8 * i);
    m_at += size;
    return static_cast<T>(bits);
}

std::optional<bool> Reader::boolean()
{
    std::optional<std::uint32_t> const value = uint32();
    if (!value || *value > 1) {
        m_failed = true;
        return std::nullopt;
    }
    return *value == 1;
}

std::optional<std::int16_t> Reader::int16() { return fixed<std::int16_t>(2); }
std::optional<std::uint16_t> Reader::uint16() { return fixed<std::uint16_t>(2); }
std::optional<std::int32_t> Reader::int32() { return fixed<std::int32_t>(4); }
std::optional<std::int64_t> Reader::int64() { return fixed<std::int64_t>(8); }
std::optional<std::uint64_t> Reader::uint64() { return fixed<std::uint64_t>(8); }

std::optional<double> Reader::float64()
{
    std::optional<std::uint64_t> const bits = uint64();
    if (!bits)
        return std::nullopt;
    return std::bit_cast<double>(*bits);
}

std::optional<std::string> Reader::string()
{
    std::optional<std::uint32_t> const length = uint32();
    // Its bytes and the NUL after them, which must be one.
    if (!length || *length >= m_bytes.size() - m_at || m_bytes[m_at + *length] != 0) {
        m_failed = true;
        return std::nullopt;
    }
    std::string text(reinterpret_cast<char const*>(m_bytes.data() + m_at), *length);
    if (!valid_utf8(text)) {
        m_failed = true;
        return std::nullopt;
    }
    m_at += *length + 1;
    return text;
}

std::optional<std::string> Reader::object_path()
{
    std::optional<std::string> path = string();
    if (path && !valid_object_path(*path)) {
        m_failed = true;
        return std::nullopt;
    }
    return path;
}

std::optional<std::string> Reader::signature()
{
    std::optional<std::uint8_t> const length = byte();
    if (!length || *length >= m_bytes.size() - m_at || m_bytes[m_at + *length] != 0) {
        m_failed = true;
        return std::nullopt;
    }
    std::string text(reinterpret_cast<char const*>(m_bytes.data() + m_at), *length);
    if (!valid_signature(text)) {
        m_failed = true;
        return std::nullopt;
    }
    m_at += static_cast<std::size_t>(*length) + 1;
    return text;
}

std::optional<Value> Reader::value(std::string_view type)
{
    if (!single_complete_type(type)) {
        m_failed = true;
        return std::nullopt;
    }
    return value_at(type, 0, 0, 0);
}

std::optional<std::vector<Value>> Reader::values(std::string_view signature)
{
    if (!valid_signature(signature)) {
        m_failed = true;
        return std::nullopt;
    }
    std::vector<Value> values;
    std::size_t at = 0;
    while (at < signature.size()) {
        std::size_t const length = complete_type_length(signature, at);
        std::optional<Value> one = value_at(signature.substr(at, length), 0, 0, 0);
        if (!one)
            return std::nullopt;
        values.push_back(std::move(*one));
        at += length;
    }
    return values;
}

std::optional<Value> Reader::value_at(std::string_view type, int arrays, int structs, int depth)
{
    auto const fail = [this]() -> std::optional<Value> {
        m_failed = true;
        return std::nullopt;
    };
    if (m_failed || type.empty())
        return fail();
    Value value;
    value.type = std::string(type);
    switch (type[0]) {
    case 'y': {
        std::optional<std::uint8_t> const v = byte();
        if (!v)
            return fail();
        value.number = *v;
        return value;
    }
    case 'b': {
        std::optional<bool> const v = boolean();
        if (!v)
            return fail();
        value.number = *v ? 1 : 0;
        return value;
    }
    case 'n': {
        std::optional<std::int16_t> const v = int16();
        if (!v)
            return fail();
        value.number = static_cast<std::uint64_t>(static_cast<std::int64_t>(*v));
        return value;
    }
    case 'q': {
        std::optional<std::uint16_t> const v = uint16();
        if (!v)
            return fail();
        value.number = *v;
        return value;
    }
    case 'i': {
        std::optional<std::int32_t> const v = int32();
        if (!v)
            return fail();
        value.number = static_cast<std::uint64_t>(static_cast<std::int64_t>(*v));
        return value;
    }
    case 'u':
    case 'h': {
        std::optional<std::uint32_t> const v = uint32();
        if (!v)
            return fail();
        value.number = *v;
        return value;
    }
    case 'x':
    case 't': {
        std::optional<std::uint64_t> const v = uint64();
        if (!v)
            return fail();
        value.number = *v;
        return value;
    }
    case 'd': {
        std::optional<double> const v = float64();
        if (!v)
            return fail();
        value.real = *v;
        return value;
    }
    case 's':
    case 'o':
    case 'g': {
        std::optional<std::string> v = type[0] == 's' ? string() : type[0] == 'o' ? object_path() : signature();
        if (!v)
            return fail();
        value.text = std::move(*v);
        return value;
    }
    case 'v': {
        if (depth + 1 > deepest_total)
            return fail();
        std::optional<std::string> const inner = signature();
        if (!inner || !single_complete_type(*inner))
            return fail();
        std::optional<Value> held = value_at(*inner, arrays, structs, depth + 1);
        if (!held)
            return fail();
        value.items.push_back(std::move(*held));
        return value;
    }
    case 'a': {
        if (arrays + 1 > deepest_arrays || depth + 1 > deepest_total)
            return fail();
        std::optional<std::uint32_t> const length = uint32();
        std::string_view const element = type.substr(1);
        // The padding before the first element is there even when the
        // array is empty, and is not counted in its length.
        if (!length || *length > longest_array || !align(alignment_of(element[0])) || *length > m_bytes.size() - m_at)
            return fail();
        std::size_t const end = m_at + *length;
        while (m_at < end) {
            std::optional<Value> item = value_at(element, arrays + 1, structs, depth + 1);
            if (!item || m_at > end)
                return fail();
            value.items.push_back(std::move(*item));
        }
        return value;
    }
    case '(':
    case '{': {
        if (structs + 1 > deepest_structs || depth + 1 > deepest_total || !align(8))
            return fail();
        std::size_t at = 1;
        while (at < type.size() - 1) {
            std::size_t const length = complete_type_length(type, at);
            if (length == 0)
                return fail();
            std::optional<Value> field = value_at(type.substr(at, length), arrays, structs + 1, depth + 1);
            if (!field)
                return fail();
            value.items.push_back(std::move(*field));
            at += length;
        }
        return value;
    }
    default:
        return fail();
    }
}

std::optional<std::vector<std::string>> Reader::string_array()
{
    std::optional<std::uint32_t> const length = uint32();
    if (!length || *length > longest_array || *length > m_bytes.size() - m_at) {
        m_failed = true;
        return std::nullopt;
    }
    std::size_t const end = m_at + *length;
    std::vector<std::string> strings;
    while (m_at < end) {
        std::optional<std::string> text = string();
        if (!text || m_at > end) {
            m_failed = true;
            return std::nullopt;
        }
        strings.push_back(std::move(*text));
    }
    return strings;
}

std::vector<std::uint8_t> encode(Message const& message)
{
    Writer header;
    header.byte('l');
    header.byte(static_cast<std::uint8_t>(message.type));
    header.byte(message.no_reply_expected ? 1 : 0);
    header.byte(1); // the protocol's version
    header.uint32(static_cast<std::uint32_t>(message.body.size()));
    header.uint32(message.serial);
    header.array(8, [&] {
        auto const text_field = [&header](Field field, std::string_view kind, std::string const& value) {
            if (value.empty())
                return;
            header.align(8);
            header.byte(field);
            header.signature(kind);
            if (kind == "g")
                header.signature(value);
            else
                header.string(value);
        };
        text_field(Path, "o", message.path);
        text_field(InterfaceField, "s", message.interface);
        text_field(Member, "s", message.member);
        text_field(ErrorName, "s", message.error_name);
        if (message.reply_serial != 0) {
            header.align(8);
            header.byte(ReplySerial);
            header.variant_uint32(message.reply_serial);
        }
        text_field(Destination, "s", message.destination);
        text_field(Sender, "s", message.sender);
        text_field(Signature, "g", message.signature);
        if (!message.fds.empty()) {
            header.align(8);
            header.byte(UnixFds);
            header.variant_uint32(static_cast<std::uint32_t>(message.fds.size()));
        }
    });
    header.align(8);
    std::vector<std::uint8_t> bytes = header.take();
    bytes.insert(bytes.end(), message.body.begin(), message.body.end());
    return bytes;
}

std::optional<Message> decode(std::span<std::uint8_t const> bytes, std::size_t& used, bool& malformed)
{
    used = 0;
    malformed = false;
    if (bytes.size() < 16)
        return std::nullopt;
    if ((bytes[0] != 'l' && bytes[0] != 'B') || bytes[1] < 1 || bytes[1] > 4 || bytes[3] != 1) {
        malformed = true;
        return std::nullopt;
    }
    bool const little = bytes[0] == 'l';
    auto const number = [&](std::size_t at) {
        std::uint32_t const value = get32(bytes, at);
        return little ? value : __builtin_bswap32(value);
    };
    std::size_t const body_length = number(4);
    std::size_t const fields_length = number(12);
    if (body_length > longest_message || fields_length > longest_array) {
        malformed = true;
        return std::nullopt;
    }
    std::size_t header_length = 16 + fields_length;
    while (header_length % 8 != 0)
        ++header_length;
    std::size_t const whole = header_length + body_length;
    if (whole > longest_message) {
        malformed = true;
        return std::nullopt;
    }
    if (bytes.size() < whole)
        return std::nullopt;
    used = whole;
    // Every bus and every service on this machine writes in the machine's
    // order; a message in the other is passed over.
    if (!little)
        return std::nullopt;

    Message message;
    message.type = static_cast<MessageType>(bytes[1]);
    message.no_reply_expected = (bytes[2] & 1) != 0;
    message.serial = number(8);
    Reader fields(bytes.first(16 + fields_length));
    fields.skip(16);
    while (fields.position() < 16 + fields_length) {
        if (!fields.align(8))
            return std::nullopt;
        if (fields.position() >= 16 + fields_length)
            break;
        std::optional<std::uint8_t> const field = fields.byte();
        std::optional<std::string> const kind = fields.signature();
        if (!field || !kind)
            return std::nullopt;
        if (*kind == "s" || *kind == "o") {
            std::optional<std::string> value = fields.string();
            if (!value)
                return std::nullopt;
            switch (*field) {
            case Path: message.path = std::move(*value); break;
            case InterfaceField: message.interface = std::move(*value); break;
            case Member: message.member = std::move(*value); break;
            case ErrorName: message.error_name = std::move(*value); break;
            case Destination: message.destination = std::move(*value); break;
            case Sender: message.sender = std::move(*value); break;
            default: break;
            }
        } else if (*kind == "g") {
            std::optional<std::string> value = fields.signature();
            if (!value)
                return std::nullopt;
            if (*field == Signature)
                message.signature = std::move(*value);
        } else if (*kind == "u") {
            std::optional<std::uint32_t> const value = fields.uint32();
            if (!value)
                return std::nullopt;
            if (*field == ReplySerial)
                message.reply_serial = *value;
        } else if (!single_complete_type(*kind) || !fields.value(*kind)) {
            // A field of another kind (one a later version may add) is
            // passed over; one that does not parse ends the header.
            return std::nullopt;
        }
    }
    message.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(header_length),
        bytes.begin() + static_cast<std::ptrdiff_t>(whole));
    return message;
}

std::optional<SocketAddress> parse_address(std::string_view address)
{
    while (!address.empty()) {
        std::size_t const end = address.find(';');
        std::string_view one = address.substr(0, end);
        address = end == std::string_view::npos ? std::string_view() : address.substr(end + 1);
        if (!one.starts_with("unix:"))
            continue;
        one.remove_prefix(5);
        while (!one.empty()) {
            std::size_t const comma = one.find(',');
            std::string_view const pair = one.substr(0, comma);
            one = comma == std::string_view::npos ? std::string_view() : one.substr(comma + 1);
            bool const abstract = pair.starts_with("abstract=");
            if (!abstract && !pair.starts_with("path="))
                continue;
            // A value's bytes outside the unreserved set are written %xx.
            std::string_view const escaped = pair.substr(pair.find('=') + 1);
            std::string value;
            for (std::size_t i = 0; i < escaped.size(); ++i) {
                auto const hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                if (escaped[i] == '%' && i + 2 < escaped.size() && hex(escaped[i + 1]) >= 0 && hex(escaped[i + 2]) >= 0) {
                    value += static_cast<char>(hex(escaped[i + 1]) * 16 + hex(escaped[i + 2]));
                    i += 2;
                } else {
                    value += escaped[i];
                }
            }
            if (!value.empty())
                return SocketAddress { std::move(value), abstract };
        }
    }
    return std::nullopt;
}

std::optional<SocketAddress> session_bus_address()
{
    if (char const* const address = std::getenv("DBUS_SESSION_BUS_ADDRESS"); address != nullptr && *address)
        return parse_address(address);
    if (char const* const runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime) {
        std::string path = std::string(runtime) + "/bus";
        if (access(path.c_str(), F_OK) == 0)
            return SocketAddress { std::move(path), false };
    }
    return std::nullopt;
}

struct Connection::Lock {
    std::mutex mutex;
};

namespace {

bool said(std::string* why_not, std::string reason)
{
    if (why_not != nullptr)
        *why_not = std::move(reason);
    return false;
}

// The first bytes of a message with its file descriptors beside them
// (SCM_RIGHTS, all on one sendmsg); how many bytes went, -1 on failure.
ssize_t send_with_fds(int fd, std::uint8_t const* bytes, std::size_t count, std::vector<int> const& fds)
{
    std::vector<std::uint8_t> control(CMSG_SPACE(sizeof(int) * fds.size()));
    iovec io { const_cast<std::uint8_t*>(bytes), count };
    msghdr message {};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* const cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
    while (true) {
        ssize_t const wrote = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote < 0 && errno == EAGAIN) {
            pollfd waiting { fd, POLLOUT, 0 };
            poll(&waiting, 1, 1000);
            continue;
        }
        return wrote;
    }
}

bool write_all(int fd, std::uint8_t const* bytes, std::size_t count)
{
    while (count > 0) {
        ssize_t const wrote = send(fd, bytes, count, MSG_NOSIGNAL);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                pollfd waiting { fd, POLLOUT, 0 };
                poll(&waiting, 1, 1000);
                continue;
            }
            return false;
        }
        bytes += wrote;
        count -= static_cast<std::size_t>(wrote);
    }
    return true;
}

// A line of the handshake, up to its "\r\n", waiting at most `timeout_ms`.
std::optional<std::string> read_line(int fd, int timeout_ms)
{
    std::string line;
    while (line.size() < 4096) {
        pollfd waiting { fd, POLLIN, 0 };
        if (poll(&waiting, 1, timeout_ms) <= 0)
            return std::nullopt;
        char c = 0;
        ssize_t const got = recv(fd, &c, 1, 0);
        if (got <= 0)
            return std::nullopt;
        line += c;
        if (line.ends_with("\r\n")) {
            line.resize(line.size() - 2);
            return line;
        }
    }
    return std::nullopt;
}

}

std::unique_ptr<Connection> Connection::open(SocketAddress const& address, std::string* why_not)
{
    sockaddr_un name {};
    name.sun_family = AF_UNIX;
    if (address.path.size() + 1 >= sizeof name.sun_path) {
        said(why_not, "the bus's socket name is too long");
        return nullptr;
    }
    // An abstract name starts with a NUL and has no NUL after it.
    std::memcpy(name.sun_path + (address.abstract ? 1 : 0), address.path.data(), address.path.size());
    socklen_t const name_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address.path.size() + (address.abstract ? 1 : 0));
    int const fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        said(why_not, std::string("no socket: ") + std::strerror(errno));
        return nullptr;
    }
    std::unique_ptr<Connection> connection(new Connection);
    connection->m_fd = fd;
    connection->m_send_lock = std::make_unique<Lock>();
    if (connect(fd, reinterpret_cast<sockaddr const*>(&name), name_length) != 0) {
        said(why_not, "the bus at " + address.path + " cannot be reached: " + std::strerror(errno));
        return nullptr;
    }
    // The handshake: a NUL, then who this is — the user's number, as the
    // socket's own credentials will bear out — and the binary protocol begins.
    std::string const uid = std::to_string(getuid());
    std::string hex;
    for (char const c : uid) {
        static constexpr char digits[] = "0123456789abcdef";
        hex += digits[static_cast<unsigned char>(c) >> 4];
        hex += digits[static_cast<unsigned char>(c) & 15];
    }
    std::string const hello = std::string(1, '\0') + "AUTH EXTERNAL " + hex + "\r\n";
    if (!write_all(fd, reinterpret_cast<std::uint8_t const*>(hello.data()), hello.size())) {
        said(why_not, "the bus closed the socket");
        return nullptr;
    }
    std::optional<std::string> const answer = read_line(fd, 5000);
    if (!answer || !answer->starts_with("OK")) {
        said(why_not, "the bus did not accept this user" + (answer ? ": " + *answer : std::string()));
        return nullptr;
    }
    // File descriptors in messages (a portal's print takes the file as
    // one): asked for; a bus that says no still carries everything else.
    static constexpr char negotiate[] = "NEGOTIATE_UNIX_FD\r\n";
    if (write_all(fd, reinterpret_cast<std::uint8_t const*>(negotiate), sizeof negotiate - 1)) {
        std::optional<std::string> const agreed = read_line(fd, 5000);
        connection->m_passes_fds = agreed && agreed->starts_with("AGREE_UNIX_FD");
    }
    static constexpr char begin[] = "BEGIN\r\n";
    if (!write_all(fd, reinterpret_cast<std::uint8_t const*>(begin), sizeof begin - 1)) {
        said(why_not, "the bus closed the socket");
        return nullptr;
    }
    // Hello: the bus names the connection, and passes nothing on before.
    Message call;
    call.destination = "org.freedesktop.DBus";
    call.path = "/org/freedesktop/DBus";
    call.interface = "org.freedesktop.DBus";
    call.member = "Hello";
    std::uint32_t const serial = connection->send(std::move(call));
    for (int waited = 0; waited < 50 && serial != 0; ++waited) {
        std::optional<Message> const reply = connection->receive(100);
        if (connection->closed())
            break;
        if (!reply || reply->reply_serial != serial)
            continue;
        if (reply->type == MessageType::MethodReturn) {
            Reader body(reply->body);
            if (std::optional<std::string> name_given = body.string()) {
                connection->m_unique_name = std::move(*name_given);
                return connection;
            }
        }
        break;
    }
    said(why_not, "the bus did not answer the hello");
    return nullptr;
}

Connection::~Connection()
{
    if (m_fd >= 0)
        close(m_fd);
}

std::uint32_t Connection::send(Message message)
{
    std::lock_guard const lock(m_send_lock->mutex);
    if (m_closed)
        return 0;
    message.serial = m_next_serial++;
    if (m_next_serial == 0)
        m_next_serial = 1;
    std::vector<std::uint8_t> const bytes = encode(message);
    std::size_t sent = 0;
    if (!message.fds.empty()) {
        if (!m_passes_fds)
            return 0; // the bus will not carry them: not sent
        ssize_t const first = send_with_fds(m_fd, bytes.data(), bytes.size(), message.fds);
        if (first < 0) {
            m_closed = true;
            return 0;
        }
        sent = static_cast<std::size_t>(first);
    }
    if (!write_all(m_fd, bytes.data() + sent, bytes.size() - sent)) {
        m_closed = true;
        return 0;
    }
    return message.serial;
}

std::optional<Message> Connection::receive(int timeout_ms, int also_fd)
{
    for (;;) {
        std::size_t used = 0;
        bool malformed = false;
        std::optional<Message> message = decode(m_pending, used, malformed);
        if (malformed) {
            m_closed = true;
            return std::nullopt;
        }
        if (used != 0) {
            m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(used));
            if (message)
                return message;
            continue; // one that could not be read: the next
        }
        if (m_closed)
            return std::nullopt;
        pollfd waiting[2] = { { m_fd, POLLIN, 0 }, { also_fd, POLLIN, 0 } };
        int const ready = poll(waiting, also_fd >= 0 ? 2 : 1, timeout_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (waiting[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            return std::nullopt;
        std::uint8_t buffer[4096];
        ssize_t const got = recv(m_fd, buffer, sizeof buffer, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (got <= 0) {
            m_closed = true;
            return std::nullopt;
        }
        m_pending.insert(m_pending.end(), buffer, buffer + got);
    }
}

// --- Being a service --------------------------------------------------------------

Reply Reply::failure(std::string name, std::string text)
{
    Reply reply;
    reply.error = std::move(name);
    reply.error_text = std::move(text);
    return reply;
}

namespace {

constexpr std::string_view properties_interface = "org.freedesktop.DBus.Properties";
constexpr std::string_view introspectable_interface = "org.freedesktop.DBus.Introspectable";
constexpr std::string_view peer_interface = "org.freedesktop.DBus.Peer";

// The standard interfaces' own introspection ("Standard Interfaces").
constexpr std::string_view standard_introspection
    = "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
      "    <method name=\"Introspect\"><arg name=\"xml_data\" type=\"s\" direction=\"out\"/></method>\n"
      "  </interface>\n"
      "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
      "    <method name=\"Ping\"/>\n"
      "    <method name=\"GetMachineId\"><arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/></method>\n"
      "  </interface>\n"
      "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
      "    <method name=\"Get\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
      "<arg name=\"property_name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"out\"/></method>\n"
      "    <method name=\"GetAll\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
      "<arg name=\"props\" type=\"a{sv}\" direction=\"out\"/></method>\n"
      "    <method name=\"Set\"><arg name=\"interface_name\" type=\"s\" direction=\"in\"/>"
      "<arg name=\"property_name\" type=\"s\" direction=\"in\"/><arg name=\"value\" type=\"v\" direction=\"in\"/></method>\n"
      "    <signal name=\"PropertiesChanged\"><arg name=\"interface_name\" type=\"s\"/>"
      "<arg name=\"changed_properties\" type=\"a{sv}\"/><arg name=\"invalidated_properties\" type=\"as\"/></signal>\n"
      "  </interface>\n";

// The machine's id, as the bus has it (Peer.GetMachineId).
std::string machine_id()
{
    for (char const* const path : { "/etc/machine-id", "/var/lib/dbus/machine-id" }) {
        std::ifstream in(path);
        std::string id;
        if (in && std::getline(in, id) && id.size() == 32)
            return id;
    }
    return std::string(32, '0');
}

}

Service::Service(Connection& connection)
    : m_connection(&connection)
{
}

void Service::add_object(std::string path, std::vector<std::shared_ptr<Interface const>> interfaces)
{
    m_objects[std::move(path)] = std::move(interfaces);
}

void Service::add_subtree(std::string prefix, ObjectResolver resolver)
{
    m_subtrees.emplace_back(std::move(prefix), std::move(resolver));
}

void Service::remove_object(std::string const& path) { m_objects.erase(path); }

std::optional<std::vector<std::shared_ptr<Interface const>>> Service::interfaces_at(std::string_view path) const
{
    if (auto const found = m_objects.find(path); found != m_objects.end())
        return found->second;
    for (auto const& [prefix, resolver] : m_subtrees) {
        if (path.starts_with(prefix) && path.size() > prefix.size())
            return resolver(path);
    }
    return std::nullopt;
}

std::string Service::introspect(std::string const& path) const
{
    std::string xml = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
                      "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n";
    xml += standard_introspection;
    if (std::optional<std::vector<std::shared_ptr<Interface const>>> const interfaces = interfaces_at(path)) {
        for (std::shared_ptr<Interface const> const& interface : *interfaces)
            xml += interface->introspection;
    }
    // The objects below this one, by the next element of their paths.
    std::string const below = path == "/" ? "/" : path + "/";
    std::string last;
    for (auto const& [object, interfaces] : m_objects) {
        if (!object.starts_with(below) || object.size() == below.size())
            continue;
        std::string const child = object.substr(below.size(), object.find('/', below.size()) - below.size());
        if (child != last)
            xml += "  <node name=\"" + child + "\"/>\n";
        last = child;
    }
    return xml + "</node>\n";
}

void Service::answer(Message const& call, Reply reply)
{
    if (call.no_reply_expected)
        return;
    Message message;
    message.reply_serial = call.serial;
    message.destination = call.sender;
    if (!reply.error.empty()) {
        message.type = MessageType::Error;
        message.error_name = std::move(reply.error);
        message.signature = "s";
        Writer body;
        body.string(reply.error_text);
        message.body = body.take();
    } else {
        message.type = MessageType::MethodReturn;
        message.signature = std::move(reply.signature);
        message.body = reply.body.take();
    }
    m_connection->send(std::move(message));
}

bool Service::dispatch(Message const& call)
{
    if (call.type != MessageType::MethodCall)
        return false;
    std::string const& path = call.path;
    if (call.interface == peer_interface || (call.interface.empty() && (call.member == "Ping" || call.member == "GetMachineId"))) {
        Reply reply;
        if (call.member == "GetMachineId") {
            reply.signature = "s";
            reply.body.string(machine_id());
        } else if (call.member != "Ping") {
            reply = Reply::failure(std::string(error_unknown_method), "Peer has no method " + call.member);
        }
        answer(call, std::move(reply));
        return true;
    }
    if (call.interface == introspectable_interface || (call.interface.empty() && call.member == "Introspect")) {
        Reply reply;
        if (call.member == "Introspect") {
            reply.signature = "s";
            reply.body.string(introspect(path));
        } else {
            reply = Reply::failure(std::string(error_unknown_method), "Introspectable has no method " + call.member);
        }
        answer(call, std::move(reply));
        return true;
    }
    std::optional<std::vector<std::shared_ptr<Interface const>>> const interfaces = interfaces_at(path);
    if (!interfaces) {
        answer(call, Reply::failure(std::string(error_unknown_object), "No object at " + path));
        return true;
    }
    auto const interface_named = [&](std::string_view name) -> Interface const* {
        for (std::shared_ptr<Interface const> const& interface : *interfaces) {
            if (interface->name == name)
                return interface.get();
        }
        return nullptr;
    };
    if (call.interface == properties_interface) {
        Reader body(call.body);
        std::optional<std::vector<Value>> const args = body.values(call.signature);
        bool const get = call.member == "Get" && call.signature == "ss";
        bool const get_all = call.member == "GetAll" && call.signature == "s";
        bool const set = call.member == "Set" && call.signature == "ssv";
        if (!get && !get_all && !set) {
            answer(call, Reply::failure(std::string(call.member == "Get" || call.member == "GetAll" || call.member == "Set" ? error_invalid_args : error_unknown_method),
                             "Properties." + call.member + " with arguments (" + call.signature + ")"));
            return true;
        }
        if (!args) {
            answer(call, Reply::failure(std::string(error_invalid_args), "The arguments do not parse"));
            return true;
        }
        Interface const* const interface = interface_named((*args)[0].text);
        if (!interface) {
            answer(call, Reply::failure(std::string(error_unknown_interface), "No interface " + (*args)[0].text + " at " + path));
            return true;
        }
        Reply reply;
        if (get_all) {
            reply.signature = "a{sv}";
            reply.body.array(8, [&] {
                for (Interface::Property const& property : interface->properties) {
                    std::optional<Value> const value = interface->get ? interface->get(path, property.name) : std::nullopt;
                    if (!value)
                        continue;
                    reply.body.align(8);
                    reply.body.string(property.name);
                    reply.body.variant(*value);
                }
            });
            answer(call, std::move(reply));
            return true;
        }
        std::string const& name = (*args)[1].text;
        Interface::Property const* property = nullptr;
        for (Interface::Property const& candidate : interface->properties) {
            if (candidate.name == name)
                property = &candidate;
        }
        if (!property) {
            answer(call, Reply::failure(std::string(error_unknown_property), "No property " + name + " in " + interface->name));
            return true;
        }
        if (get) {
            std::optional<Value> const value = interface->get ? interface->get(path, name) : std::nullopt;
            if (!value) {
                answer(call, Reply::failure(std::string(error_unknown_property), "No property " + name + " here"));
                return true;
            }
            reply.signature = "v";
            reply.body.variant(*value);
            answer(call, std::move(reply));
            return true;
        }
        if (!property->writable || !interface->set) {
            answer(call, Reply::failure(std::string(error_property_read_only), name + " cannot be set"));
            return true;
        }
        Value const& given = (*args)[2].items.front();
        if (given.type != property->signature) {
            answer(call, Reply::failure(std::string(error_invalid_args), name + " is of type " + property->signature));
            return true;
        }
        if (std::optional<Reply> refused = interface->set(path, name, given)) {
            answer(call, std::move(*refused));
            return true;
        }
        answer(call, Reply {});
        return true;
    }
    if (!call.interface.empty()) {
        Interface const* const interface = interface_named(call.interface);
        if (!interface) {
            answer(call, Reply::failure(std::string(error_unknown_interface), "No interface " + call.interface + " at " + path));
            return true;
        }
        std::optional<Reply> reply = interface->call ? interface->call(call) : std::nullopt;
        if (!reply)
            reply = Reply::failure(std::string(error_unknown_method), call.interface + " has no method " + call.member + "(" + call.signature + ")");
        answer(call, std::move(*reply));
        return true;
    }
    // No interface named: the first that has the member answers.
    for (std::shared_ptr<Interface const> const& interface : *interfaces) {
        if (std::optional<Reply> reply = interface->call ? interface->call(call) : std::nullopt) {
            answer(call, std::move(*reply));
            return true;
        }
    }
    answer(call, Reply::failure(std::string(error_unknown_method), "No method " + call.member + " at " + path));
    return true;
}

std::optional<Message> Service::call(Message message, int timeout_ms)
{
    std::uint32_t const serial = m_connection->send(std::move(message));
    if (serial == 0)
        return std::nullopt;
    auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return std::nullopt;
        std::optional<Message> got = m_connection->receive(static_cast<int>(left));
        if (!got) {
            if (m_connection->closed())
                return std::nullopt;
            continue;
        }
        if (got->reply_serial == serial && (got->type == MessageType::MethodReturn || got->type == MessageType::Error))
            return got;
        m_kept.push_back(std::move(*got));
    }
}

std::optional<Message> Service::next(int timeout_ms, int also_fd)
{
    if (!m_kept.empty()) {
        Message message = std::move(m_kept.front());
        m_kept.pop_front();
        return message;
    }
    return m_connection->receive(timeout_ms, also_fd);
}

std::uint32_t Service::emit(std::string path, std::string interface, std::string member, std::string signature, Writer body,
    std::string destination)
{
    Message message;
    message.type = MessageType::Signal;
    message.no_reply_expected = true;
    message.path = std::move(path);
    message.interface = std::move(interface);
    message.member = std::move(member);
    message.signature = std::move(signature);
    message.destination = std::move(destination);
    message.body = body.take();
    return m_connection->send(std::move(message));
}

Message Service::method_call(std::string destination, std::string path, std::string interface, std::string member,
    std::string signature, Writer body)
{
    Message message;
    message.destination = std::move(destination);
    message.path = std::move(path);
    message.interface = std::move(interface);
    message.member = std::move(member);
    message.signature = std::move(signature);
    message.body = body.take();
    return message;
}

namespace {

constexpr char const* bus_name = "org.freedesktop.DBus";
constexpr char const* bus_path = "/org/freedesktop/DBus";

}

bool Service::request_name(std::string const& name, std::uint32_t flags, int timeout_ms)
{
    Writer body;
    body.string(name);
    body.uint32(flags);
    std::optional<Message> const reply = call(method_call(bus_name, bus_path, bus_name, "RequestName", "su", std::move(body)), timeout_ms);
    if (!reply || reply->type != MessageType::MethodReturn)
        return false;
    Reader answer(reply->body);
    std::optional<std::uint32_t> const result = answer.uint32();
    return result && (*result == 1 || *result == 4); // the primary owner now, or already
}

bool Service::add_match(std::string const& rule, int timeout_ms)
{
    Writer body;
    body.string(rule);
    std::optional<Message> const reply = call(method_call(bus_name, bus_path, bus_name, "AddMatch", "s", std::move(body)), timeout_ms);
    return reply && reply->type == MessageType::MethodReturn;
}

bool Service::remove_match(std::string const& rule, int timeout_ms)
{
    Writer body;
    body.string(rule);
    std::optional<Message> const reply = call(method_call(bus_name, bus_path, bus_name, "RemoveMatch", "s", std::move(body)), timeout_ms);
    return reply && reply->type == MessageType::MethodReturn;
}

}
