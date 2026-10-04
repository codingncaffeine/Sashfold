#pragma once

// A JavaScript value (ECMA-262 §6.1): one of the seven language types, or
// the internal `empty` that marks an array hole and a binding not yet
// initialized. Eight bytes, trivially copyable, no ownership: strings,
// objects, symbols and bigints are heap cells the collector owns (Heap.h).
// A Value held in a C++ local across an allocation must be rooted — see
// the note on Interpreter::root.
//
// The encoding is JavaScriptCore's for 64-bit machines, one word:
//
//   cell      0000 pppp pppp pppp   the cell's own address (8-aligned,
//                                   the top 16 bits zero); the cell says
//                                   which kind it is (Cell::kind, Cell.h)
//   int32     fffe 0000 iiii iiii   NumberTag | the 32 bits
//   double    0002 … fff2 …         the IEEE bits + 2^49 (DoubleEncodeOffset)
//   empty     0000 0000 0000 0000   all-zero memory reads as empty
//   null      …02  false …06  true …07  undefined …0a
//
// A number is an int32 whenever it is one (an integral double in range
// that is not −0), so equal numbers have equal bits; every NaN is the
// canonical one, since an impure payload could decode as another tag.
// Nothing outside this file reads the bits.

#include "js/Cell.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <functional>

// The checks on a cell's address run in a debug build and under the
// address sanitizer, whose build defines NDEBUG too.
#if !defined(NDEBUG) || defined(__SANITIZE_ADDRESS__)
#define SASHFOLD_JS_VALUE_CHECKS 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SASHFOLD_JS_VALUE_CHECKS 1
#endif
#endif

namespace sashfold::js {

class JsString;
class Object;
class Symbol;
class BigInt;

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
        BigInt,
        Empty, // never observable from a script
    };

    // The encoding's constants (JavaScriptCore's names).
    static constexpr std::uint64_t NumberTag = 0xfffe000000000000ull;
    static constexpr std::uint64_t DoubleEncodeOffset = 1ull << 49;
    static constexpr std::uint64_t OtherTag = 0x2;
    static constexpr std::uint64_t BoolTag = 0x4;
    static constexpr std::uint64_t UndefinedTag = 0x8;
    static constexpr std::uint64_t NotCellMask = NumberTag | OtherTag;
    static constexpr std::uint64_t ValueEmpty = 0x0;
    static constexpr std::uint64_t ValueNull = OtherTag;
    static constexpr std::uint64_t ValueFalse = OtherTag | BoolTag;
    static constexpr std::uint64_t ValueTrue = OtherTag | BoolTag | 1;
    static constexpr std::uint64_t ValueUndefined = OtherTag | UndefinedTag;
    static constexpr std::uint64_t CanonicalNaN = 0x7ff8000000000000ull;

    constexpr Value() = default; // undefined

    static constexpr Value undefined() { return {}; }
    static constexpr Value null() { return from_bits(ValueNull); }
    static constexpr Value empty() { return from_bits(ValueEmpty); }
    static constexpr Value boolean(bool b) { return from_bits(b ? ValueTrue : ValueFalse); }
    static constexpr Value int32(std::int32_t i) { return from_bits(NumberTag | static_cast<std::uint32_t>(i)); }
    // An int32 when the double is one — integral, in range, not −0 — so a
    // number has one encoding; otherwise the double, its NaN canonical.
    // The range is checked before the cast: casting a double outside it is
    // undefined behaviour. A NaN fails both comparisons.
    static constexpr Value number(double d)
    {
        if (d >= -2147483648.0 && d <= 2147483647.0) {
            auto const i = static_cast<std::int32_t>(d);
            if (static_cast<double>(i) == d && (i != 0 || std::bit_cast<std::uint64_t>(d) == 0))
                return int32(i);
        }
        return encode_double(d);
    }
    static Value string(JsString* s) { return cell(s); } // s must not be null
    static Value object(Object* o) { return cell(o); } // o must not be null
    static Value symbol(Symbol* s) { return cell(s); }
    static Value bigint(BigInt* b) { return cell(b); } // b must not be null

    Type type() const
    {
        if (is_cell())
            return static_cast<Type>(cell_kind());
        if (is_number())
            return Type::Number;
        switch (m_bits) {
        case ValueNull:
            return Type::Null;
        case ValueFalse:
        case ValueTrue:
            return Type::Boolean;
        case ValueEmpty:
            return Type::Empty;
        default:
            return Type::Undefined;
        }
    }
    constexpr bool is_undefined() const { return m_bits == ValueUndefined; }
    constexpr bool is_null() const { return m_bits == ValueNull; }
    constexpr bool is_nullish() const { return (m_bits & ~UndefinedTag) == ValueNull; }
    constexpr bool is_boolean() const { return (m_bits & ~std::uint64_t { 1 }) == ValueFalse; }
    constexpr bool is_number() const { return (m_bits & NumberTag) != 0; }
    constexpr bool is_int32() const { return (m_bits & NumberTag) == NumberTag; }
    constexpr bool is_double() const { return is_number() && !is_int32(); }
    bool is_string() const { return is_cell() && cell_kind() == CellKind::String; }
    bool is_object() const { return is_cell() && cell_kind() == CellKind::Object; }
    bool is_symbol() const { return is_cell() && cell_kind() == CellKind::Symbol; }
    bool is_bigint() const { return is_cell() && cell_kind() == CellKind::BigInt; }
    constexpr bool is_empty() const { return m_bits == ValueEmpty; }
    constexpr bool is_cell() const { return (m_bits & NotCellMask) == 0 && m_bits != ValueEmpty; }

    constexpr bool as_boolean() const { return m_bits == ValueTrue; }
    constexpr std::int32_t as_int32() const { return static_cast<std::int32_t>(static_cast<std::uint32_t>(m_bits)); }
    constexpr double as_double() const { return std::bit_cast<double>(m_bits - DoubleEncodeOffset); }
    constexpr double as_number() const { return is_int32() ? static_cast<double>(as_int32()) : as_double(); }
    JsString* as_string() const { return reinterpret_cast<JsString*>(m_bits); }
    Object* as_object() const { return reinterpret_cast<Object*>(m_bits); }
    Symbol* as_symbol() const { return reinterpret_cast<Symbol*>(m_bits); }
    BigInt* as_bigint() const { return reinterpret_cast<BigInt*>(m_bits); }
    // The cell behind a string, object, symbol or bigint; null for the
    // rest (empty's all-zero bits are the null pointer). What the
    // collector traces.
    Cell* as_cell() const { return (m_bits & NotCellMask) == 0 ? reinterpret_cast<Cell*>(m_bits) : nullptr; }

    // An object's slot (Object.h, Shape.h) may hold two things no script
    // value is: an accessor's pair, a cell that is never a script value,
    // and the mark of a property there and not made — the address of a
    // record of the process's, 8-aligned, with OtherTag set and BoolTag
    // telling one kind of record from the other. No script value has
    // those bits (null is OtherTag alone; false, true and undefined are
    // all below 16), and neither is ever handed to a script.
    static Value slot_cell(Cell* cell) { return Value::cell(cell); }
    static Value lazy_mark(void const* record, bool second)
    {
        auto const bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record));
#ifdef SASHFOLD_JS_VALUE_CHECKS
        if (bits <= 0xf || (bits & 7u) != 0 || (bits & 0xffff000000000000ull) != 0)
            std::abort();
#endif
        return from_bits(bits | OtherTag | (second ? BoolTag : 0));
    }
    constexpr bool is_lazy_mark() const { return (m_bits & (NumberTag | 1)) == 0 && (m_bits & OtherTag) != 0 && m_bits > 0xf; }
    constexpr bool is_second_lazy_mark() const { return (m_bits & BoolTag) != 0; }
    void const* as_lazy_mark() const { return reinterpret_cast<void const*>(static_cast<std::uintptr_t>(m_bits & ~std::uint64_t { 7 })); }

    // The encoded word itself, for the tests of the encoding; the engine
    // asks the questions above instead.
    constexpr std::uint64_t bits() const { return m_bits; }

    // Identity of the representation: the same bits. Two NaNs compare
    // equal here (there is one NaN), +0 and −0 do not (−0 is a double, +0
    // an int32), and 1 and 1.0 do (both are the int32). The language's
    // SameValue / strict equality are on the Interpreter.
    constexpr bool operator==(Value const& other) const { return m_bits == other.m_bits; }

private:
    static constexpr Value from_bits(std::uint64_t bits)
    {
        Value v;
        v.m_bits = bits;
        return v;
    }
    static constexpr Value encode_double(double d)
    {
        std::uint64_t const bits = d != d ? CanonicalNaN : std::bit_cast<std::uint64_t>(d);
        return from_bits(bits + DoubleEncodeOffset);
    }
    static Value cell(void const* pointer)
    {
        auto const bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
#ifdef SASHFOLD_JS_VALUE_CHECKS
        // A cell is 8-aligned and its address leaves the top 16 bits clear,
        // or it would read as a number; null would read as empty.
        if (bits == 0 || (bits & 7u) != 0 || (bits & 0xffff000000000000ull) != 0)
            std::abort();
#endif
        return from_bits(bits);
    }
    // Cell.h is included above, so every file that reads a kind has the
    // definition: none can link against another file's copy instead.
    CellKind cell_kind() const { return as_cell()->kind(); }

    std::uint64_t m_bits = ValueUndefined;
};

static_assert(sizeof(Value) == 8 && alignof(Value) == 8);
static_assert(static_cast<int>(CellKind::String) == static_cast<int>(Value::Type::String)
    && static_cast<int>(CellKind::Object) == static_cast<int>(Value::Type::Object)
    && static_cast<int>(CellKind::Symbol) == static_cast<int>(Value::Type::Symbol)
    && static_cast<int>(CellKind::BigInt) == static_cast<int>(Value::Type::BigInt));
static_assert(sizeof(void*) == 8, "the value encoding keeps a cell's address in 48 bits of a 64-bit word");
static_assert(Value().is_undefined() && Value::empty().bits() == 0);

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
