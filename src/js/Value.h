#pragma once

// A JavaScript value (ECMA-262 §6.1): one of the seven language types, or
// the internal `empty` that marks an array hole and a binding not yet
// initialized. Sixteen bytes, trivially copyable, no ownership: strings,
// objects and symbols are heap cells the collector owns (Heap.h). A Value
// held in a C++ local across an allocation must be rooted — see the note
// on Interpreter::root.

#include <cstdint>
#include <functional>

namespace sashfold::js {

class Cell;
class JsString;
class Object;
class Symbol;

class Value {
public:
    enum class Type : std::uint8_t {
        Undefined,
        Null,
        Boolean,
        Number,
        String,
        Object,
        Symbol,
        Empty, // never observable from a script
    };

    Value() = default; // undefined

    static Value undefined() { return {}; }
    static Value null()
    {
        Value v;
        v.m_type = Type::Null;
        return v;
    }
    static Value empty()
    {
        Value v;
        v.m_type = Type::Empty;
        return v;
    }
    static Value boolean(bool b)
    {
        Value v;
        v.m_type = Type::Boolean;
        v.m_boolean = b;
        return v;
    }
    static Value number(double d)
    {
        Value v;
        v.m_type = Type::Number;
        v.m_number = d;
        return v;
    }
    static Value string(JsString* s) // s must not be null
    {
        Value v;
        v.m_type = Type::String;
        v.m_cell = reinterpret_cast<Cell*>(s);
        return v;
    }
    static Value object(Object* o) // o must not be null
    {
        Value v;
        v.m_type = Type::Object;
        v.m_cell = reinterpret_cast<Cell*>(o);
        return v;
    }
    static Value symbol(Symbol* s)
    {
        Value v;
        v.m_type = Type::Symbol;
        v.m_cell = reinterpret_cast<Cell*>(s);
        return v;
    }

    Type type() const { return m_type; }
    bool is_undefined() const { return m_type == Type::Undefined; }
    bool is_null() const { return m_type == Type::Null; }
    bool is_nullish() const { return m_type == Type::Undefined || m_type == Type::Null; }
    bool is_boolean() const { return m_type == Type::Boolean; }
    bool is_number() const { return m_type == Type::Number; }
    bool is_string() const { return m_type == Type::String; }
    bool is_object() const { return m_type == Type::Object; }
    bool is_symbol() const { return m_type == Type::Symbol; }
    bool is_empty() const { return m_type == Type::Empty; }
    bool is_cell() const { return m_type == Type::String || m_type == Type::Object || m_type == Type::Symbol; }

    bool as_boolean() const { return m_boolean; }
    double as_number() const { return m_number; }
    JsString* as_string() const { return reinterpret_cast<JsString*>(m_cell); }
    Object* as_object() const { return reinterpret_cast<Object*>(m_cell); }
    Symbol* as_symbol() const { return reinterpret_cast<Symbol*>(m_cell); }
    // The cell behind a string, object or symbol; null for the rest. What
    // the collector traces.
    Cell* as_cell() const { return is_cell() ? m_cell : nullptr; }

    // Identity of the representation: same type and the same bits (two
    // NaNs compare equal here, +0 and -0 do not). The language's
    // SameValue / strict equality are on the Interpreter.
    bool operator==(Value const& other) const
    {
        if (m_type != other.m_type)
            return false;
        switch (m_type) {
        case Type::Boolean:
            return m_boolean == other.m_boolean;
        case Type::Number:
            return m_bits == other.m_bits;
        case Type::String:
        case Type::Object:
        case Type::Symbol:
            return m_cell == other.m_cell;
        default:
            return true;
        }
    }

private:
    Type m_type = Type::Undefined;
    union {
        bool m_boolean;
        double m_number;
        std::uint64_t m_bits;
        Cell* m_cell;
    };
};

// A property name (§6.1.7): an interned string, an array index (an integer
// 0 … 2^32 − 2 whose canonical numeric string is the name), or a symbol.
// The heap makes them — `Heap::key(…)` decides between atom and index — so
// that a name is never stored both ways.
class PropertyKey {
public:
    enum class Kind : std::uint8_t { Atom, Index, Symbol };

    PropertyKey() = default; // an empty key; never valid for lookup
    static PropertyKey atom(JsString* interned) // interned, not null
    {
        PropertyKey k;
        k.m_kind = Kind::Atom;
        k.m_cell = reinterpret_cast<Cell*>(interned);
        return k;
    }
    static PropertyKey index(std::uint32_t i) // i <= 0xFFFFFFFE
    {
        PropertyKey k;
        k.m_kind = Kind::Index;
        k.m_index = i;
        return k;
    }
    static PropertyKey symbol(Symbol* s)
    {
        PropertyKey k;
        k.m_kind = Kind::Symbol;
        k.m_cell = reinterpret_cast<Cell*>(s);
        return k;
    }

    Kind kind() const { return m_kind; }
    bool is_atom() const { return m_kind == Kind::Atom; }
    bool is_index() const { return m_kind == Kind::Index; }
    bool is_symbol() const { return m_kind == Kind::Symbol; }
    bool is_string() const { return m_kind != Kind::Symbol; } // an index is a string name too
    JsString* as_atom() const { return reinterpret_cast<JsString*>(m_cell); }
    std::uint32_t as_index() const { return m_index; }
    Symbol* as_symbol() const { return reinterpret_cast<Symbol*>(m_cell); }
    Cell* as_cell() const { return m_kind == Kind::Index ? nullptr : m_cell; }

    bool operator==(PropertyKey const& other) const
    {
        if (m_kind != other.m_kind)
            return false;
        return m_kind == Kind::Index ? m_index == other.m_index : m_cell == other.m_cell;
    }

    std::size_t hash() const
    {
        if (m_kind == Kind::Index)
            return std::hash<std::uint32_t> {}(m_index) * 3u + 1u;
        return std::hash<void const*> {}(m_cell) * 3u + (m_kind == Kind::Atom ? 0u : 2u);
    }

private:
    Kind m_kind = Kind::Atom;
    union {
        std::uint32_t m_index;
        Cell* m_cell = nullptr;
    };
};

struct PropertyKeyHash {
    std::size_t operator()(PropertyKey const& k) const { return k.hash(); }
};

}
