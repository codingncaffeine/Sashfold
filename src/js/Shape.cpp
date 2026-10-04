#include "js/Shape.h"

#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Object.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace sashfold::js {

// ------------------------------------------------------------ ShapeTable

void ShapeTable::append(ShapeEntry const& entry, Heap* heap)
{
    std::size_t const room_before = m_entries.capacity();
    m_entries.push_back(entry);
    if (entry.key.is_index())
        ++m_index_keys;
    if (size() == linear_limit + 1)
        rebuild_index();
    else if (size() > linear_limit + 1)
        place(size() - 1);
    if (heap != nullptr && m_entries.capacity() > room_before)
        heap->grew((m_entries.capacity() - room_before) * sizeof(ShapeEntry));
}

void ShapeTable::reserve(std::uint32_t count, Heap* heap)
{
    std::size_t const room_before = m_entries.capacity();
    m_entries.reserve(count);
    if (heap != nullptr && m_entries.capacity() > room_before)
        heap->grew((m_entries.capacity() - room_before) * sizeof(ShapeEntry));
}

void ShapeTable::copy_from(ShapeTable const& other, std::uint32_t count, Heap* heap)
{
    reserve(count + 1, heap);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!other.m_entries[i].is_hole())
            append(other.m_entries[i], heap);
    }
}

// The entry at `position` given its spot in the index, which grows (and
// sheds what deletes left) once it is half full.
void ShapeTable::place(std::uint32_t position)
{
    if ((m_index_used + 1) * 2 > m_index.size()) {
        rebuild_index();
        return;
    }
    auto const mask = static_cast<std::uint32_t>(m_index.size()) - 1;
    std::uint32_t spot = spot_of(m_entries[position].key, mask);
    while (m_index[spot] != 0)
        spot = (spot + 1) & mask;
    m_index[spot] = position + 1;
    ++m_index_used;
}

void ShapeTable::erase(std::uint32_t position)
{
    ShapeEntry& entry = m_entries[position];
    if (!m_index.empty()) {
        auto const mask = static_cast<std::uint32_t>(m_index.size()) - 1;
        for (std::uint32_t spot = spot_of(entry.key, mask);; spot = (spot + 1) & mask) {
            if (m_index[spot] == position + 1) {
                m_index[spot] = gone;
                break;
            }
        }
    }
    if (entry.key.is_index())
        --m_index_keys;
    entry.key = PropertyKey();
    ++m_holes;
}

void ShapeTable::compact()
{
    std::erase_if(m_entries, [](ShapeEntry const& entry) { return entry.is_hole(); });
    m_holes = 0;
    rebuild_index();
}

void ShapeTable::rebuild_index()
{
    m_index.clear();
    m_index_used = 0;
    std::uint32_t const live = size() - m_holes;
    if (size() <= linear_limit)
        return;
    std::uint32_t room = 16;
    while (room < live * 4)
        room *= 2;
    m_index.assign(room, 0);
    for (std::uint32_t i = 0; i < size(); ++i) {
        if (!m_entries[i].is_hole())
            place(i);
    }
}

std::size_t ShapeTable::size_in_bytes() const
{
    return sizeof(*this) + m_entries.capacity() * sizeof(ShapeEntry) + m_index.capacity() * sizeof(std::uint32_t);
}

// ----------------------------------------------------------------- Shape

namespace {

// Which of a shape's pending function properties a key names: the bit, or 0.
std::uint8_t pending_bit(Heap const& heap, std::uint8_t flags, PropertyKey const& key)
{
    if ((flags & Shape::PendingMask) == 0 || !key.is_atom())
        return 0;
    WellKnownAtoms const& atoms = heap.atoms();
    JsString const* const name = key.as_atom();
    std::uint8_t const bit = name == atoms.length ? 1 : name == atoms.name ? 2 : name == atoms.prototype ? 4 : 0;
    return static_cast<std::uint8_t>(bit & flags);
}

// Past this many transitions a shape finds one by key.
constexpr std::size_t transition_index_from = 12;

}

Shape::Shape(Shape* parent, ShapeTable* table, Object* prototype, std::uint32_t count, std::uint8_t flags)
    : m_parent(parent)
    , m_table(table)
    , m_prototype(prototype)
    , m_count(count)
    , m_flags(flags)
{
    if (m_table != nullptr)
        m_table->retain();
}

Shape::~Shape()
{
    if (m_table != nullptr)
        m_table->release();
}

Shape* Shape::root(Heap& heap, Object* prototype, std::uint8_t flags)
{
    if (prototype == nullptr) {
        for (auto const& [held_flags, held] : heap.m_null_roots) {
            if (held_flags == flags)
                return held;
        }
        Heap::NoCollect const no_collect(heap);
        Shape* const made = heap.allocate<Shape>(nullptr, nullptr, nullptr, 0u, flags);
        heap.m_null_roots.emplace_back(flags, made);
        return made;
    }
    // An object that is the prototype of another becomes a dictionary,
    // which knows the roots of the objects that inherit from it — weakly,
    // as a shape knows its transitions: a root no object uses goes at the
    // next collection (a prototype made a dictionary as soon as it is made
    // used its parent's root for an instant).
    if (!prototype->shape()->is_dictionary())
        prototype->become_dictionary();
    Shape& holder = *prototype->shape();
    for (auto const& [held_flags, held] : holder.info().child_roots) {
        if (held_flags == flags)
            return held;
    }
    Heap::NoCollect const no_collect(heap);
    Shape* const made = heap.allocate<Shape>(nullptr, nullptr, prototype, 0u, flags);
    holder.info().child_roots.emplace_back(flags, made);
    holder.hold_weakly(heap);
    return made;
}

Shape* Shape::find_transition(Kind kind, PropertyKey const& key, std::uint8_t attributes, bool accessor) const
{
    if (m_transitions == nullptr)
        return nullptr;
    auto const matches = [&](Transition const& transition) {
        return transition.kind == kind && transition.key == key && transition.attributes == attributes && transition.accessor == accessor;
    };
    std::vector<Transition> const& list = m_transitions->list;
    if (!m_transitions->index.empty()) {
        auto [at, end] = m_transitions->index.equal_range(key);
        for (; at != end; ++at) {
            if (matches(list[at->second]))
                return list[at->second].child;
        }
        return nullptr;
    }
    for (Transition const& transition : list) {
        if (matches(transition))
            return transition.child;
    }
    return nullptr;
}

void Shape::add_transition(Heap& heap, Kind kind, PropertyKey const& key, std::uint8_t attributes, bool accessor, Shape* child)
{
    if (m_transitions == nullptr) {
        m_transitions = std::make_unique<Transitions>();
        hold_weakly(heap);
    }
    std::vector<Transition>& list = m_transitions->list;
    list.push_back(Transition { key, child, kind, attributes, accessor });
    if (list.size() == transition_index_from + 1) {
        for (std::uint32_t i = 0; i < list.size(); ++i)
            m_transitions->index.emplace(list[i].key, i);
    } else if (list.size() > transition_index_from + 1) {
        m_transitions->index.emplace(key, static_cast<std::uint32_t>(list.size() - 1));
    }
}

Shape* Shape::adding(Heap& heap, PropertyKey const& key, std::uint8_t attributes, bool accessor)
{
    if (Shape* const known = find_transition(Kind::Add, key, attributes, accessor))
        return known;
    Heap::NoCollect const no_collect(heap);
    // The table's last shape adds in place; any other copies what it sees.
    ShapeTable* table = m_table;
    bool const fresh = table == nullptr || table->size() != m_count;
    if (fresh) {
        table = new ShapeTable();
        if (m_table != nullptr)
            table->copy_from(*m_table, m_count, &heap);
    }
    table->append(ShapeEntry { key, m_count, attributes, accessor }, &heap);
    auto const flags = static_cast<std::uint8_t>(m_flags & ~pending_bit(heap, m_flags, key));
    Shape* const child = heap.allocate<Shape>(this, table, m_prototype, m_count + 1, flags);
    child->m_owns_table = fresh;
    child->m_has_index_keys = m_has_index_keys || key.is_index();
    add_transition(heap, Kind::Add, key, attributes, accessor, child);
    return child;
}

Shape* Shape::reconfiguring(Heap& heap, std::uint32_t position, std::uint8_t attributes, bool accessor)
{
    PropertyKey const key = at(position).key;
    if (Shape* const known = find_transition(Kind::Reconfigure, key, attributes, accessor))
        return known;
    Heap::NoCollect const no_collect(heap);
    auto* const table = new ShapeTable();
    table->copy_from(*m_table, m_count, &heap);
    (*table)[position].attributes = attributes;
    (*table)[position].accessor = accessor;
    Shape* const child = heap.allocate<Shape>(this, table, m_prototype, m_count, m_flags);
    child->m_owns_table = true;
    child->m_has_index_keys = m_has_index_keys;
    add_transition(heap, Kind::Reconfigure, key, attributes, accessor, child);
    return child;
}

Shape* Shape::with_flags(Heap& heap, std::uint8_t flags)
{
    if (flags == m_flags)
        return this;
    // An object with no properties starts again from the root those flags have.
    if (m_count == 0)
        return root(heap, m_prototype, flags);
    if (Shape* const known = find_transition(Kind::Flags, PropertyKey(), flags, false))
        return known;
    Heap::NoCollect const no_collect(heap);
    Shape* const child = heap.allocate<Shape>(this, m_table, m_prototype, m_count, flags);
    child->m_has_index_keys = m_has_index_keys;
    add_transition(heap, Kind::Flags, PropertyKey(), flags, false, child);
    return child;
}

Shape* Shape::with_prototype(Heap& heap, Object* prototype)
{
    // The same properties, added again in their order from the new
    // prototype's root: an object of this shape keeps its slots.
    Heap::NoCollect const no_collect(heap);
    Shape* shape = root(heap, prototype, static_cast<std::uint8_t>(m_flags & (PendingMask | Uncacheable)));
    for (std::uint32_t i = 0; i < m_count; ++i) {
        ShapeEntry const& entry = at(i);
        shape = shape->adding(heap, entry.key, entry.attributes, entry.accessor);
    }
    return shape;
}

Shape* Shape::to_dictionary(Heap& heap) const
{
    Heap::NoCollect const no_collect(heap);
    auto* const table = new ShapeTable();
    if (m_table != nullptr)
        table->copy_from(*m_table, count(), &heap);
    // No cell of the heap's: the object owns it (and deletes it), and
    // traces it as the collector traces a cell that belongs to no heap.
    auto* const dictionary = new Shape(nullptr, table, m_prototype, 0u, static_cast<std::uint8_t>(m_flags | Dictionary));
    dictionary->m_owns_table = true;
    return dictionary;
}

void Shape::trace(Tracer& tracer)
{
    tracer.visit(m_parent);
    tracer.visit(m_prototype);
    // A dictionary keeps its keys; a shared shape the one it added, its
    // parent the ones before it (a table copied for a branch holds only
    // those, and a table extended past this shape holds later shapes').
    if (m_table != nullptr) {
        if (is_dictionary()) {
            for (std::uint32_t i = 0; i < m_table->size(); ++i)
                tracer.visit((*m_table)[i].key);
        } else if (m_count != 0) {
            tracer.visit((*m_table)[m_count - 1].key);
        }
    }
    if (m_info != nullptr)
        tracer.visit(m_info->realm);
    // The transitions and a prototype's roots are weak: clear_weak() drops
    // the ones that died.
}

void Shape::hold_weakly(Heap& heap)
{
    if (m_weakly_held)
        return;
    m_weakly_held = true;
    heap.hold_weakly(this);
}

void Shape::clear_weak()
{
    if (m_info != nullptr)
        std::erase_if(m_info->child_roots, [](auto const& entry) { return !entry.second->marked(); });
    if (m_transitions == nullptr)
        return;
    std::vector<Transition>& list = m_transitions->list;
    std::size_t const before = list.size();
    std::erase_if(list, [](Transition const& transition) { return !transition.child->marked(); });
    if (list.size() == before || m_transitions->index.empty())
        return;
    m_transitions->index.clear();
    if (list.size() > transition_index_from) {
        for (std::uint32_t i = 0; i < list.size(); ++i)
            m_transitions->index.emplace(list[i].key, i);
    }
}

std::size_t Shape::size_in_bytes() const
{
    std::size_t bytes = sizeof(*this);
    if (m_owns_table)
        bytes += m_table->size_in_bytes();
    if (m_transitions != nullptr)
        bytes += sizeof(Transitions) + m_transitions->list.capacity() * sizeof(Transition) + m_transitions->index.size() * 4 * sizeof(void*);
    if (m_info != nullptr)
        bytes += sizeof(Info) + m_info->child_roots.capacity() * sizeof(m_info->child_roots[0]) + m_info->free_slots.capacity() * sizeof(std::uint32_t);
    return bytes;
}

// ---------------------------------------------------------- AccessorPair

void AccessorPair::trace(Tracer& tracer)
{
    tracer.visit(getter);
    tracer.visit(setter);
}

} // namespace sashfold::js
