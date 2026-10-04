#pragma once

// Feedback (js-JIT-DESIGN.md §4.5, js-SHAPES-DESIGN.md §5): what each site
// of a code block has seen, one record per site, laid out as data machine
// code will read. A property site (a named read or write, a global name)
// keeps an inline cache — up to four (shape, where, kind) answers, then the
// interpreter's stub cache — which the interpreter executes: a hit is a
// compare of the object's shape and a load of a slot. A call site keeps
// the one target it saw, or that it saw many; an arithmetic or comparison
// site the kinds of operands it saw; an element site the kinds of
// elements. The vector is made at the block's first run.
//
// An answer is good while what it was made from holds: the receiver's
// shape (a shared shape is immutable; a dictionary's version moves with
// every change to it), and for an answer from up the prototype chain — a
// prototype's property, a property no link has, a property added to the
// receiver past the chain's setters — the heap's prototype epoch, which
// every change to a prototype moves (js/Shape.h). Shapes and the objects an
// answer names are held weakly: after marking, what was not reached is
// dropped from every vector, and the stub cache is emptied.

#include "js/Cell.h"
#include "js/Value.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace sashfold::js {

class Heap;
class Object;
class Shape;
class JsString;

// What an answer is.
enum class CacheKind : std::uint8_t {
    None,
    OwnData, // the receiver's slot
    OwnAccessor, // the receiver's slot holds the accessor's pair
    ProtoData, // `other` (a prototype) holds the value in its slot
    ProtoAccessor, // … its accessor's pair
    Absent, // no link of the chain has the key (a read answers undefined)
    StoreAdd, // a write that adds the key: the receiver moves to `other` (a shape), the value to the slot
    GlobalData, // the global object's own data property (`other` the global object)
    GlobalAccessor, // … its own accessor
    GlobalLexical, // a script's let/const/class: `other` the realm's global lexical environment, the slot its binding
    ArrayLength, // an array's length
};

// One shape's answer. `stamp` is the prototype epoch an answer from up
// the chain was made under (a global's: the realm's lexical generation).
// `version`, for a dictionary receiver (whose shape stays while it changes
// in place): the property's place in its table for an answer over its own
// property — good while that place still holds the key, the slot and the
// kind, whatever else the dictionary does — and its version for an answer
// from up its chain, which its own changes may shadow.
struct PropertyEntry {
    Shape* shape = nullptr;
    Cell* other = nullptr;
    std::uint32_t slot = 0;
    std::uint32_t stamp = 0;
    std::uint32_t version = 0;
    CacheKind kind = CacheKind::None;
};

// A property site: one answer inline, three more beside it once a second
// shape is seen, then megamorphic.
struct PropertySite {
    enum : std::uint8_t { Empty, Monomorphic, Polymorphic, Megamorphic };
    static constexpr std::uint8_t width = 4;

    PropertyEntry first;
    PropertyEntry* more = nullptr; // Polymorphic: `width` entries, `count` used (first among them)
    std::uint8_t state = Empty;
    std::uint8_t count = 0;
};

// Read by the compilers to come at these offsets.
static_assert(sizeof(PropertyEntry) == 32 && offsetof(PropertyEntry, shape) == 0 && offsetof(PropertyEntry, other) == 8
    && offsetof(PropertyEntry, slot) == 16 && offsetof(PropertyEntry, stamp) == 20 && offsetof(PropertyEntry, version) == 24
    && offsetof(PropertyEntry, kind) == 28);
static_assert(sizeof(PropertySite) == 48 && offsetof(PropertySite, first) == 0 && offsetof(PropertySite, more) == 32
    && offsetof(PropertySite, state) == 40 && offsetof(PropertySite, count) == 41);

// A call site: the one function it has called, or that it has called many.
struct CallSite {
    Object* target = nullptr;
    bool many = false;
};

// Operand kinds an arithmetic or comparison site has seen (bits).
enum OperandKind : std::uint8_t {
    OperandInt32 = 1,
    OperandDouble = 2,
    OperandString = 4,
    OperandBigInt = 8,
    OperandOther = 16,
};

// Element kinds an element site has seen (bits), and a typed array's type.
enum ElementKind : std::uint8_t {
    ElementDense = 1, // an array, an int32 index inside its dense storage
    ElementTyped = 2, // a typed array
    ElementGeneric = 4, // anything else
};
struct ElementSite {
    std::uint8_t kinds = 0;
    std::uint8_t typed = 0xff; // the typed array's element type when only one was seen
};

class FeedbackVector {
public:
    FeedbackVector(std::uint32_t property_sites, std::uint32_t call_sites, std::uint32_t operand_sites, std::uint32_t element_sites);
    ~FeedbackVector();
    FeedbackVector(FeedbackVector const&) = delete;
    FeedbackVector& operator=(FeedbackVector const&) = delete;

    PropertySite& property(std::uint32_t site) { return m_properties[site]; }
    CallSite& call(std::uint32_t site) { return m_calls[site]; }
    std::uint8_t& operands(std::uint32_t site) { return m_operands[site]; }
    ElementSite& element(std::uint32_t site) { return m_elements[site]; }
    std::uint32_t property_count() const { return m_property_count; }

    // After marking: every answer naming a cell the collection did not
    // reach is dropped.
    void clear_dead(Heap const&);
    std::size_t size_in_bytes() const;

private:
    std::unique_ptr<PropertySite[]> m_properties;
    std::unique_ptr<CallSite[]> m_calls;
    std::unique_ptr<std::uint8_t[]> m_operands;
    std::unique_ptr<ElementSite[]> m_elements;
    std::uint32_t m_property_count;
    std::uint32_t m_call_count;
};

// The interpreter's cache for the megamorphic sites: answers by (shape,
// name), one way each, emptied at every collection (V8's stub cache).
class StubCache {
public:
    static constexpr std::size_t size = 4096;
    PropertyEntry const* find(Shape const* shape, JsString const* name, bool store) const
    {
        Slot const& slot = m_slots[index(shape, name, store)];
        return slot.name == name && slot.entry.shape == shape && slot.store == store ? &slot.entry : nullptr;
    }
    void put(JsString const* name, bool store, PropertyEntry const& entry)
    {
        Slot& slot = m_slots[index(entry.shape, name, store)];
        slot.name = name;
        slot.store = store;
        slot.entry = entry;
    }
    void clear() { m_slots.fill(Slot {}); }

private:
    struct Slot {
        JsString const* name = nullptr;
        PropertyEntry entry;
        bool store = false;
    };
    static std::size_t index(Shape const* shape, JsString const* name, bool store)
    {
        auto const mixed = (reinterpret_cast<std::uintptr_t>(shape) >> 3) * 0x9e3779b97f4a7c15ull ^ (reinterpret_cast<std::uintptr_t>(name) >> 3) ^ (store ? 0x5bd1e995u : 0u);
        return static_cast<std::size_t>(mixed >> 20) & (size - 1);
    }
    std::array<Slot, size> m_slots {};
};

// The bits a site records of one operand.
inline std::uint8_t operand_kind(Value const& value)
{
    if (value.is_int32())
        return OperandInt32;
    if (value.is_number())
        return OperandDouble;
    if (value.is_string())
        return OperandString;
    if (value.is_bigint())
        return OperandBigInt;
    return OperandOther;
}

// Answers made from a lookup, the one the miss path ran (none of them runs
// script or makes anything): a read's answer for this receiver and key, or
// a write's (an own writable slot, a setter up the chain); `epoch` is the
// heap's when the lookup ran. A kind of None: not cacheable.
PropertyEntry answer_for_load(Object& receiver, PropertyKey const& key, std::uint32_t epoch);
PropertyEntry answer_for_store(Object& receiver, PropertyKey const& key, std::uint32_t epoch);
// The answer for a write that added the key to the receiver: its shape
// went from `before` to its present one by that one addition.
PropertyEntry answer_for_add(Object& receiver, Shape* before, PropertyKey const& key, std::uint32_t epoch);
// Records an answer at a site: monomorphic, then polymorphic, then
// megamorphic (the answer goes to the stub cache instead).
void record(PropertySite&, PropertyEntry const&, StubCache&, JsString const* name, bool store);

} // namespace sashfold::js
