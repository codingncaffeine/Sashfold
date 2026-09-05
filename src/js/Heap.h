#pragma once

// The script heap: every string, symbol, object and scope a script can
// reach is a Cell the Heap owns, and a precise, non-moving mark-and-sweep
// collector reclaims what nothing reaches. Roots are explicit: the
// interpreter's root stack, every Persistent, the atom table, and whatever
// RootProvider the bindings register (the connected DOM tree, per ADR 0001).
//
// Collection runs inside allocate() once enough has been allocated since
// the last one — so any Value a C++ frame holds across an allocation must
// be rooted, or it may be swept under it. Stress mode collects at EVERY
// allocation and is how the tests find a missing root deterministically.
// A NoCollect scope defers collection for a native that builds several
// cells before it can root them.

#include "js/Value.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::js {

class Heap;
class Tracer;

class Cell {
public:
    virtual ~Cell() = default;
    // Visit every cell this one keeps alive. Not recursive: the tracer
    // queues what it is shown, so a long prototype or scope chain costs
    // stack for nothing.
    virtual void trace(Tracer&) { }
    // An estimate the collector's threshold is fed with; exactness is not
    // required, monotonic reasonableness is.
    virtual std::size_t size_in_bytes() const { return sizeof(*this); }

    bool marked() const { return m_marked; }
    void set_marked(bool marked) { m_marked = marked; } // the collector's
    // The heap that adopted this cell; null for a cell that belongs to no
    // heap. How an exotic object reaches its realm's atoms.
    Heap* heap() const { return m_heap; }

private:
    friend class Heap;
    Heap* m_heap = nullptr;
    bool m_marked = false;
};

class Tracer {
public:
    void visit(Cell* cell); // null is fine; marks and queues an unmarked cell
    void visit(Value const& value) { visit(value.as_cell()); }
    void visit(PropertyKey const& key) { visit(key.as_cell()); }

private:
    friend class Heap;
    std::vector<Cell*> m_worklist;
};

// An immutable sequence of UTF-16 code units, which is what a JavaScript
// string is (§6.1.4). Lone surrogates are legal and preserved. An atom is
// a string the heap has interned: two atoms with the same contents are the
// same cell, so property names compare by pointer. Atoms are never
// collected.
class JsString : public Cell {
public:
    explicit JsString(std::u16string data)
        : m_data(std::move(data))
    {
    }

    std::u16string const& data() const { return m_data; }
    std::u16string_view view() const { return m_data; }
    std::size_t length() const { return m_data.size(); }
    bool is_empty() const { return m_data.empty(); }
    bool is_atom() const { return m_atom; }
    bool equals(std::u16string_view other) const { return m_data == other; }
    bool equals(JsString const& other) const
    {
        return this == &other || (!(m_atom && other.m_atom) && m_data == other.m_data);
    }
    // WTF-8: a lone surrogate comes out as its three-byte form, for
    // internal use only (the DOM stores WTF-8 too).
    std::string to_utf8() const;
    // The array index this string names, when it is the canonical numeric
    // string of an integer 0 … 2^32 − 2; computed once.
    std::optional<std::uint32_t> as_array_index() const;
    std::size_t hash() const; // computed once

    std::size_t size_in_bytes() const override { return sizeof(*this) + m_data.size() * 2; }

private:
    friend class Heap;
    std::u16string m_data;
    bool m_atom = false;
    mutable bool m_index_known = false;
    mutable bool m_is_index = false;
    mutable std::uint32_t m_index = 0;
    mutable std::size_t m_hash = 0;
    mutable bool m_hash_known = false;
};

class Symbol : public Cell {
public:
    explicit Symbol(JsString* description) // may be null: Symbol()
        : m_description(description)
    {
    }
    JsString* description() const { return m_description; }
    void trace(Tracer& tracer) override { tracer.visit(m_description); }

private:
    JsString* m_description;
};

// Something outside the heap that holds cells and must say so at every
// collection: the interpreter (its root stack), the bindings (the
// connected tree's wrappers).
class RootProvider {
public:
    virtual ~RootProvider() = default;
    virtual void trace_roots(Tracer&) = 0;
};

// The names the runtime looks up on every other operation, interned once
// at heap construction. Add here rather than calling atom("…") in a hot
// path.
struct WellKnownAtoms {
    JsString* empty = nullptr; // ""
    JsString* length = nullptr;
    JsString* prototype = nullptr;
    JsString* constructor = nullptr;
    JsString* name = nullptr;
    JsString* message = nullptr;
    JsString* cause = nullptr;
    JsString* stack = nullptr;
    JsString* value = nullptr;
    JsString* get = nullptr;
    JsString* set = nullptr;
    JsString* writable = nullptr;
    JsString* enumerable = nullptr;
    JsString* configurable = nullptr;
    JsString* to_string = nullptr; // "toString"
    JsString* value_of = nullptr; // "valueOf"
    JsString* to_json = nullptr; // "toJSON"
    JsString* arguments = nullptr;
    JsString* callee = nullptr;
    JsString* caller = nullptr;
    JsString* index = nullptr;
    JsString* input = nullptr;
    JsString* groups = nullptr;
    JsString* last_index = nullptr; // "lastIndex"
    JsString* source = nullptr;
    JsString* flags = nullptr;
    JsString* global = nullptr;
    JsString* undefined = nullptr;
    JsString* null = nullptr;
    JsString* true_ = nullptr;
    JsString* false_ = nullptr;
    JsString* object = nullptr;
    JsString* function = nullptr;
    JsString* number = nullptr;
    JsString* string = nullptr;
    JsString* boolean = nullptr;
    JsString* symbol = nullptr;
    JsString* nan = nullptr; // "NaN"
    JsString* infinity = nullptr; // "Infinity"
    JsString* negative_infinity = nullptr; // "-Infinity"
    JsString* zero = nullptr; // "0"
    JsString* eval = nullptr;
    JsString* default_ = nullptr;
    JsString* proto = nullptr; // "__proto__"
    JsString* anonymous = nullptr; // "anonymous"
    JsString* object_object = nullptr; // "[object Object]"
    JsString* error = nullptr; // "Error"
    JsString* done = nullptr;
    JsString* next = nullptr;
    JsString* description = nullptr;
    JsString* raw = nullptr;
    JsString* comma = nullptr; // ","

    // Well-known symbols (§6.1.5.1) the runtime consults.
    Symbol* symbol_to_primitive = nullptr;
    Symbol* symbol_to_string_tag = nullptr;
    Symbol* symbol_iterator = nullptr;
    Symbol* symbol_has_instance = nullptr;
    Symbol* symbol_is_concat_spreadable = nullptr;
    Symbol* symbol_match = nullptr;
    Symbol* symbol_match_all = nullptr;
    Symbol* symbol_replace = nullptr;
    Symbol* symbol_search = nullptr;
    Symbol* symbol_split = nullptr;
    Symbol* symbol_species = nullptr;
    Symbol* symbol_unscopables = nullptr;
    Symbol* symbol_async_iterator = nullptr;
};

class Persistent;

class Heap {
public:
    Heap();
    ~Heap();
    Heap(Heap const&) = delete;
    Heap& operator=(Heap const&) = delete;

    // Makes a cell the heap owns. May collect first (never the new cell).
    template<typename T, typename... Args>
    T* allocate(Args&&... args)
    {
        maybe_collect();
        auto cell = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = cell.get();
        adopt(std::move(cell));
        return raw;
    }

    JsString* string(std::u16string data);
    JsString* string(std::u16string_view data) { return string(std::u16string(data)); }
    JsString* string(std::string_view utf8); // WTF-8 in, so lone surrogates survive
    JsString* string(char16_t code_unit);

    // Interning. The atom with these contents, made if absent; permanent.
    JsString* atom(std::u16string_view);
    JsString* atom(std::string_view utf8);
    JsString* atom(JsString*); // the atom for an existing string's contents

    // Property keys: an index when the name is a canonical array index,
    // otherwise the atom.
    PropertyKey key(std::u16string_view);
    PropertyKey key(std::string_view utf8);
    PropertyKey key(JsString*);
    PropertyKey key(double); // an index when integral and in range, else the atom of Number::toString
    PropertyKey key(std::uint32_t index); // an index unless it is 2^32 − 1, which is a string name
    // The string form of a key (§7.1.17 on an index); a symbol has none.
    JsString* key_to_string(PropertyKey const&);

    Symbol* symbol(JsString* description);

    WellKnownAtoms const& atoms() const { return m_well_known; }

    void add_root_provider(RootProvider*);
    void remove_root_provider(RootProvider*);

    void collect();
    // Collect at every allocation. For tests; finds unrooted values.
    void set_stress(bool stress) { m_stress = stress; }
    bool stress() const { return m_stress; }

    std::size_t cell_count() const { return m_cells.size(); }
    std::size_t bytes_allocated() const { return m_bytes; }
    std::size_t collections() const { return m_collections; }

    // No collection runs while one of these is alive; nests.
    class NoCollect {
    public:
        explicit NoCollect(Heap& heap)
            : m_heap(heap)
        {
            ++m_heap.m_no_collect;
        }
        ~NoCollect() { --m_heap.m_no_collect; }
        NoCollect(NoCollect const&) = delete;
        NoCollect& operator=(NoCollect const&) = delete;

    private:
        Heap& m_heap;
    };

private:
    friend class Persistent;
    void adopt(std::unique_ptr<Cell>);
    void maybe_collect();
    void intern_well_known();

    std::vector<std::unique_ptr<Cell>> m_cells;
    // Keys view the atom's own data; cells never move, so the views hold.
    std::unordered_map<std::u16string_view, JsString*> m_atoms;
    std::vector<RootProvider*> m_root_providers;
    std::unordered_set<Persistent*> m_persistents;
    WellKnownAtoms m_well_known;
    std::size_t m_bytes = 0; // estimated live + garbage since the last collection
    std::size_t m_threshold = 8u * 1024u * 1024u;
    std::size_t m_collections = 0;
    int m_no_collect = 0;
    bool m_stress = false;
    bool m_collecting = false;
};

// A value kept alive from outside the heap for as long as the handle
// exists: a timer's callback, a listener the shell holds, a wrapper the
// bindings hand to C++ code.
class Persistent {
public:
    explicit Persistent(Heap& heap, Value value = {});
    ~Persistent();
    Persistent(Persistent&&) noexcept;
    Persistent& operator=(Persistent&&) noexcept;
    Persistent(Persistent const&) = delete;
    Persistent& operator=(Persistent const&) = delete;

    Value const& value() const { return m_value; }
    void set(Value value) { m_value = value; }
    Heap& heap() const { return *m_heap; }

private:
    friend class Heap;
    Heap* m_heap;
    Value m_value;
};

}
