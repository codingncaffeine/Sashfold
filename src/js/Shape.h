#pragma once

// Shapes (js-SHAPES-DESIGN.md §2–§4, js-JIT-DESIGN.md §4.4): what an
// object's own properties are — each key, its attributes, whether it is an
// accessor, and which of the object's slots holds what it holds — kept
// apart from the values, which the object keeps in its slots. Every object
// that came by the same properties in the same order from the same
// prototype has the same shape: a shape is immutable once made, and adding
// a property, changing one's attributes or making the object
// non-extensible moves the object to another shape (a transition), which
// is made once and found again by the next object to do the same.
//
// The properties of a chain of shapes sit in one table, V8's descriptor
// array: a shape sees the first `count` entries of its table, and the
// shape that sees all of them may add the next in place; a second way out
// of a shape copies the entries it sees. Entries are in creation order and
// a shared shape's slot of a property is its place in that order.
//
// An object can leave the shared shapes for a dictionary shape of its own
// (V8's slow mode), whose table it changes in place: after a delete, past
// 64 properties, as the global object, as soon as it is the prototype of
// another object (V8 normalizes a prototype the same way), and once it
// holds a property not yet made (a native kept as its description: its
// realm is its dictionary's). A dictionary never goes back.
//
// The collector: an object holds its shape, a shape its parent, its table
// and its prototype; the shapes made from a shape are held weakly (Heap's
// weak pass drops a transition whose shape died); a prototype's dictionary
// holds the roots of the objects that inherit from it, and the heap the
// roots of the objects with none.

#include "js/Cell.h"
#include "js/Value.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::js {

class Heap;
class Object;
class RealmRecord;
class Tracer;
struct NativeSpec;

// One property as its shape knows it.
struct ShapeEntry {
    PropertyKey key; // the empty key marks a dictionary's deleted property
    std::uint32_t slot = 0;
    std::uint8_t attributes = 0;
    bool accessor = false;

    bool is_hole() const { return key.is_atom() && key.as_atom() == nullptr; }
};

static_assert(sizeof(ShapeEntry) == 24);

// The entries of a chain of shapes, or of one dictionary: kept by the
// shapes that use it (a count of them; the last to go deletes it), and no
// cell of the heap's. Its keys are kept alive by the shapes too: each
// shared shape traces the key it added and its parent the ones before, and
// a dictionary traces all of its own.
class ShapeTable {
public:
    static constexpr std::uint32_t npos = 0xffffffffu;
    // Up to this many entries a lookup is a scan; past it the table keeps
    // an index, open-addressed: the place plus one of each live entry, by
    // the key's hash (0 an empty spot, `gone` one a delete emptied).
    static constexpr std::uint32_t linear_limit = 8;

    ShapeTable() = default;
    ShapeTable(ShapeTable const&) = delete;
    ShapeTable& operator=(ShapeTable const&) = delete;
    void retain() { ++m_users; }
    void release()
    {
        if (--m_users == 0)
            delete this;
    }

    std::uint32_t size() const { return static_cast<std::uint32_t>(m_entries.size()); }
    ShapeEntry const& operator[](std::uint32_t position) const { return m_entries[position]; }
    ShapeEntry& operator[](std::uint32_t position) { return m_entries[position]; }
    // The place of `key` among the first `count` entries, or npos.
    std::uint32_t find(PropertyKey const& key, std::uint32_t count) const
    {
        if (count <= linear_limit) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (m_entries[i].key == key)
                    return i;
            }
            return npos;
        }
        std::uint32_t const mask = static_cast<std::uint32_t>(m_index.size()) - 1;
        for (std::uint32_t spot = spot_of(key, mask);; spot = (spot + 1) & mask) {
            std::uint32_t const held = m_index[spot];
            if (held == 0)
                return npos;
            if (held != gone && m_entries[held - 1].key == key)
                return held - 1 < count ? held - 1 : npos;
        }
    }
    // An entry at the end, the table growing as it must (and the heap told).
    void append(ShapeEntry const&, Heap*);
    void reserve(std::uint32_t count, Heap*);
    // A copy of the first `count` entries, holes left out (their places
    // close up; the slots stay as they were).
    void copy_from(ShapeTable const&, std::uint32_t count, Heap*);
    // A dictionary's delete: a hole where the entry was.
    void erase(std::uint32_t position);
    std::uint32_t holes() const { return m_holes; }
    // A dictionary's holes squeezed out; the entries keep their order and
    // their slots, and only their places move.
    void compact();
    // How many entries are array indices: what the dense fast paths ask of
    // every link of an array's prototype chain.
    std::uint32_t index_keys() const { return m_index_keys; }
    std::size_t size_in_bytes() const;

private:
    static constexpr std::uint32_t gone = 0xffffffffu;
    static std::uint32_t spot_of(PropertyKey const& key, std::uint32_t mask)
    {
        // Fibonacci hashing: a cell's address has no entropy in its low bits.
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(key.hash()) * 0x9e3779b97f4a7c15ull) >> 32) & mask;
    }
    void place(std::uint32_t position);
    void rebuild_index();

    std::vector<ShapeEntry> m_entries;
    std::vector<std::uint32_t> m_index; // empty, or a power of two in size
    std::uint32_t m_index_used = 0; // live and gone spots
    std::uint32_t m_users = 0;
    std::uint32_t m_holes = 0;
    std::uint32_t m_index_keys = 0;
};

class Shape final : public Cell {
public:
    // The flags. The low three are the function properties an object has
    // by its nature and has not given room yet (Object's Pending bits):
    // two objects of one shape owe the same ones.
    static constexpr std::uint8_t PendingMask = 7;
    static constexpr std::uint8_t Dictionary = 8;
    static constexpr std::uint8_t NotExtensible = 16;
    // An exotic object any of whose own properties may not be its
    // storage's (a proxy's every key, a host's named properties): a cache
    // keyed on the shape must never take the storage's word for it.
    static constexpr std::uint8_t Uncacheable = 32;
    // An exotic object whose own properties beyond its storage are only
    // indices, `length` and other canonical numeric strings (an array, a
    // string wrapper, a typed array, an arguments object): a cache may
    // take the storage's word for any other name.
    static constexpr std::uint8_t Indexed = 64;
    // A dictionary that is some object's prototype: any change to it moves
    // its heap's prototype epoch, which every cached lookup up a chain is
    // made under.
    static constexpr std::uint8_t Prototype = 128;
    // Past this many properties an object becomes a dictionary.
    static constexpr std::uint32_t dictionary_threshold = 64;

    Shape(Shape* parent, ShapeTable* table, Object* prototype, std::uint32_t count, std::uint8_t flags);
    ~Shape() override;

    Object* prototype() const { return m_prototype; }
    ShapeTable* table() const { return m_table; }
    Shape* parent() const { return m_parent; }
    std::uint8_t flags() const { return m_flags; }
    std::uint8_t pending() const { return static_cast<std::uint8_t>(m_flags & PendingMask); }
    bool is_dictionary() const { return (m_flags & Dictionary) != 0; }
    bool is_extensible() const { return (m_flags & NotExtensible) == 0; }
    bool is_uncacheable() const { return (m_flags & Uncacheable) != 0; }
    bool is_indexed() const { return (m_flags & Indexed) != 0; }
    bool is_prototype() const { return (m_flags & Prototype) != 0; }
    // A dictionary's count of the changes made to what it has (a property
    // added, deleted or reconfigured, its flags or its prototype changed):
    // a cache that keys on a dictionary keys on this too, since the shape
    // itself stays.
    std::uint32_t version() const { return m_version; }
    // The entries this shape has, holes included: the first `count` of its
    // table. A dictionary has all of its own.
    std::uint32_t count() const
    {
        if (is_dictionary())
            return m_table->size();
        return m_count;
    }
    // Its properties: the entries less the holes.
    std::uint32_t property_count() const { return count() - (is_dictionary() ? m_table->holes() : 0); }
    std::uint32_t position_of(PropertyKey const& key) const
    {
        return m_table == nullptr ? ShapeTable::npos : m_table->find(key, count());
    }
    ShapeEntry const* find(PropertyKey const& key) const
    {
        std::uint32_t const position = position_of(key);
        return position == ShapeTable::npos ? nullptr : &(*m_table)[position];
    }
    ShapeEntry const& at(std::uint32_t position) const { return (*m_table)[position]; }
    bool has_index_keys() const { return is_dictionary() ? m_table->index_keys() != 0 : m_has_index_keys; }

    // The shape an object with this prototype and these flags starts
    // from. Allocates without collecting: an object's constructor asks.
    static Shape* root(Heap&, Object* prototype, std::uint8_t flags);

    // A shared shape's transitions (none of them collects): the shape an
    // object of this one moves to when it adds a property — giving up the
    // pending flag the key names, if it names one — when one of its
    // properties takes other attributes or kind, when its flags change, and
    // when its prototype does (the properties replayed from the new root).
    Shape* adding(Heap&, PropertyKey const&, std::uint8_t attributes, bool accessor);
    Shape* reconfiguring(Heap&, std::uint32_t position, std::uint8_t attributes, bool accessor);
    Shape* with_flags(Heap&, std::uint8_t flags);
    Shape* with_prototype(Heap&, Object* prototype);
    // A dictionary of this shape's properties, for one object, which owns
    // it: no cell of the heap's, deleted with the object and traced through
    // it (the collector traces a cell no heap adopted and never frees it).
    Shape* to_dictionary(Heap&) const;

    // A dictionary's own, changed in place (the caller says it changed:
    // Object::changed).
    void set_flags(std::uint8_t flags) { m_flags = flags; }
    void set_prototype(Object* prototype) { m_prototype = prototype; }
    void bump_version() { ++m_version; }
    // The realm its properties not yet made are made in (null until one is
    // put), and the slots its deleted properties gave back.
    RealmRecord* realm() const { return m_info != nullptr ? m_info->realm : nullptr; }
    void set_realm(RealmRecord* realm) { info().realm = realm; }
    std::vector<std::uint32_t>& free_slots() { return info().free_slots; }

    void trace(Tracer&) override;
    void clear_weak() override;
    std::size_t size_in_bytes() const override;

private:
    enum class Kind : std::uint8_t { Add, Reconfigure, Flags };
    struct Transition {
        PropertyKey key; // Add: the key; Reconfigure: the property's key; Flags: none
        Shape* child;
        Kind kind;
        std::uint8_t attributes; // Flags: the new flags
        bool accessor;
    };
    struct Transitions {
        std::vector<Transition> list;
        // Past a dozen, by key: a popular prototype's root sees every
        // object literal's first key.
        std::unordered_multimap<PropertyKey, std::uint32_t, PropertyKeyHash> index;
    };
    // What only a dictionary has.
    struct Info {
        RealmRecord* realm = nullptr;
        std::vector<std::pair<std::uint8_t, Shape*>> child_roots;
        std::vector<std::uint32_t> free_slots;
    };
    Info& info()
    {
        if (m_info == nullptr)
            m_info = std::make_unique<Info>();
        return *m_info;
    }
    Shape* find_transition(Kind, PropertyKey const&, std::uint8_t attributes, bool accessor) const;
    void add_transition(Heap&, Kind, PropertyKey const&, std::uint8_t attributes, bool accessor, Shape* child);
    // Told by the heap at every collection from now on (clear_weak).
    void hold_weakly(Heap&);
    friend class Heap;
    friend struct MachineLayout; // the offsets the machine code reads (Vm.cpp)

    Shape* m_parent;
    ShapeTable* m_table;
    Object* m_prototype;
    std::uint32_t m_count;
    std::uint8_t m_flags;
    bool m_has_index_keys = false;
    bool m_owns_table = false; // made it: the heap counts it in this shape
    bool m_weakly_held = false;
    std::uint32_t m_version = 0;
    std::unique_ptr<Transitions> m_transitions;
    std::unique_ptr<Info> m_info;
};

// An accessor property's two functions, in the slot that holds the
// property: one pair per property, changed in place when either is.
class AccessorPair final : public Cell {
public:
    AccessorPair(Object* getter_, Object* setter_)
        : getter(getter_)
        , setter(setter_)
    {
    }
    Object* getter;
    Object* setter;
    void trace(Tracer&) override;
};

} // namespace sashfold::js
