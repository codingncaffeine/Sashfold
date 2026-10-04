#include "js/Feedback.h"

#include "js/Heap.h"
#include "js/Object.h"
#include "js/Shape.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace sashfold::js {

FeedbackVector::FeedbackVector(std::uint32_t property_sites, std::uint32_t call_sites, std::uint32_t operand_sites,
    std::uint32_t element_sites)
    : m_properties(property_sites != 0 ? std::make_unique<PropertySite[]>(property_sites) : nullptr)
    , m_calls(call_sites != 0 ? std::make_unique<CallSite[]>(call_sites) : nullptr)
    , m_operands(operand_sites != 0 ? std::make_unique<std::uint8_t[]>(operand_sites) : nullptr)
    , m_elements(element_sites != 0 ? std::make_unique<ElementSite[]>(element_sites) : nullptr)
    , m_property_count(property_sites)
    , m_call_count(call_sites)
{
}

FeedbackVector::~FeedbackVector()
{
    for (std::uint32_t i = 0; i < m_property_count; ++i)
        delete[] m_properties[i].more;
}

namespace {

// Whether an answer over this object's shape may be taken for this key:
// never for an exotic object any of whose own properties may not be its
// storage's; for an array, a string, a typed array or an arguments object
// only for a name that is no index and no other numeric string, and for
// an array and a string not `length`.
bool cacheable(Object const& object, PropertyKey const& key, Heap const& heap)
{
    Shape const& shape = *object.shape();
    if (shape.is_uncacheable())
        return false;
    if (shape.is_indexed()) {
        if (key.is_index() || TypedArrayObject::numeric_index(key))
            return false;
        if (key.is_atom() && key.as_atom() == heap.atoms().length
            && (object.class_id() == Object::Class::Array || object.class_id() == Object::Class::String))
            return false;
    }
    return true;
}

// Whether the key names a function property the shape's objects have not
// given room yet (a lookup would give it room: nothing to cache).
bool names_pending(Shape const& shape, PropertyKey const& key, Heap const& heap)
{
    if (shape.pending() == 0 || !key.is_atom())
        return false;
    WellKnownAtoms const& atoms = heap.atoms();
    JsString const* const name = key.as_atom();
    return name == atoms.length || name == atoms.name || name == atoms.prototype;
}

std::uint32_t version_of(Shape const& shape)
{
    return shape.is_dictionary() ? shape.version() : 0;
}

}

PropertyEntry answer_for_load(Object& receiver, PropertyKey const& key, std::uint32_t epoch)
{
    Heap const& heap = *receiver.heap();
    Shape* const shape = receiver.shape();
    std::uint32_t const version = version_of(*shape);
    if (receiver.class_id() == Object::Class::Array && key.is_atom() && key.as_atom() == heap.atoms().length)
        return { shape, nullptr, 0, 0, version, CacheKind::ArrayLength };
    if (!cacheable(receiver, key, heap))
        return {};
    if (std::uint32_t const place = shape->position_of(key); place != ShapeTable::npos) {
        ShapeEntry const& entry = shape->at(place);
        if (receiver.slot_value(entry.slot).is_lazy_mark())
            return {};
        return { shape, nullptr, entry.slot, 0, shape->is_dictionary() ? place : 0, entry.accessor ? CacheKind::OwnAccessor : CacheKind::OwnData };
    }
    if (names_pending(*shape, key, heap))
        return {};
    for (Object* link = shape->prototype(); link != nullptr; link = link->prototype()) {
        Shape* const link_shape = link->shape();
        if (!cacheable(*link, key, heap))
            return {};
        if (ShapeEntry const* const entry = link_shape->find(key)) {
            if (link->slot_value(entry->slot).is_lazy_mark())
                return {};
            return { shape, link, entry->slot, epoch, version, entry->accessor ? CacheKind::ProtoAccessor : CacheKind::ProtoData };
        }
        if (names_pending(*link_shape, key, heap))
            return {};
    }
    return { shape, nullptr, 0, epoch, version, CacheKind::Absent };
}

PropertyEntry answer_for_store(Object& receiver, PropertyKey const& key, std::uint32_t epoch)
{
    Heap const& heap = *receiver.heap();
    Shape* const shape = receiver.shape();
    std::uint32_t const version = version_of(*shape);
    if (!cacheable(receiver, key, heap))
        return {};
    if (std::uint32_t const place = shape->position_of(key); place != ShapeTable::npos) {
        ShapeEntry const& entry = shape->at(place);
        std::uint32_t const own_place = shape->is_dictionary() ? place : 0;
        if (receiver.slot_value(entry.slot).is_lazy_mark())
            return {};
        if (entry.accessor)
            return { shape, nullptr, entry.slot, 0, own_place, CacheKind::OwnAccessor };
        if ((entry.attributes & Writable) == 0)
            return {};
        return { shape, nullptr, entry.slot, 0, own_place, CacheKind::OwnData };
    }
    if (names_pending(*shape, key, heap))
        return {};
    // A setter up the chain is called; anything else the write finds there
    // means an addition (answer_for_add, once it is made) or a refusal.
    for (Object* link = shape->prototype(); link != nullptr; link = link->prototype()) {
        Shape* const link_shape = link->shape();
        if (!cacheable(*link, key, heap))
            return {};
        if (ShapeEntry const* const entry = link_shape->find(key)) {
            if (!entry->accessor || link->slot_value(entry->slot).is_lazy_mark())
                return {};
            return { shape, link, entry->slot, epoch, version, CacheKind::ProtoAccessor };
        }
        if (names_pending(*link_shape, key, heap))
            return {};
    }
    return {};
}

PropertyEntry answer_for_add(Object& receiver, Shape* before, PropertyKey const& key, std::uint32_t epoch)
{
    Heap const& heap = *receiver.heap();
    Shape* const after = receiver.shape();
    if (before == nullptr || before->is_dictionary() || !before->is_extensible() || before->pending() != 0 || before->is_uncacheable())
        return {};
    if (before->is_indexed() && (key.is_index() || TypedArrayObject::numeric_index(key)))
        return {};
    if (after->is_dictionary() || after->parent() != before || after->count() != before->count() + 1)
        return {};
    ShapeEntry const& added = after->at(before->count());
    if (!(added.key == key) || added.accessor || added.attributes != default_attributes || added.slot != before->count())
        return {};
    // The chain had nothing the write met (no setter, nothing read-only),
    // and the epoch says it still has not.
    for (Object* link = before->prototype(); link != nullptr; link = link->prototype()) {
        if (!cacheable(*link, key, heap))
            return {};
    }
    return { before, after, added.slot, epoch, 0, CacheKind::StoreAdd };
}

void record(PropertySite& site, PropertyEntry const& entry, StubCache& stubs, JsString const* name, bool store)
{
    switch (site.state) {
    case PropertySite::Empty:
        site.first = entry;
        site.count = 1;
        site.state = PropertySite::Monomorphic;
        return;
    case PropertySite::Monomorphic:
        if (site.first.shape == entry.shape) {
            site.first = entry;
            return;
        }
        if (site.more == nullptr)
            site.more = new PropertyEntry[PropertySite::width];
        site.more[0] = site.first;
        site.more[1] = entry;
        site.count = 2;
        site.state = PropertySite::Polymorphic;
        return;
    case PropertySite::Polymorphic:
        for (std::uint8_t i = 0; i < site.count; ++i) {
            if (site.more[i].shape == entry.shape) {
                site.more[i] = entry;
                return;
            }
        }
        if (site.count < PropertySite::width) {
            site.more[site.count++] = entry;
            return;
        }
        site.state = PropertySite::Megamorphic;
        stubs.put(name, store, entry);
        return;
    case PropertySite::Megamorphic:
        stubs.put(name, store, entry);
        return;
    default:
        return;
    }
}

void FeedbackVector::clear_dead(Heap const& heap)
{
    auto const live = [&heap](PropertyEntry const& entry) {
        return heap.reached(entry.shape) && (entry.other == nullptr || heap.reached(entry.other));
    };
    for (std::uint32_t i = 0; i < m_property_count; ++i) {
        PropertySite& site = m_properties[i];
        if (site.state == PropertySite::Monomorphic) {
            if (!live(site.first)) {
                site.first = {};
                site.count = 0;
                site.state = PropertySite::Empty;
            }
        } else if (site.state == PropertySite::Polymorphic) {
            PropertyEntry* const end = std::remove_if(site.more, site.more + site.count, [&](PropertyEntry const& entry) { return !live(entry); });
            site.count = static_cast<std::uint8_t>(end - site.more);
            if (site.count == 0) {
                site.state = PropertySite::Empty;
                site.first = {};
            } else if (site.count == 1) {
                site.first = site.more[0];
                site.state = PropertySite::Monomorphic;
            }
        }
    }
    for (std::uint32_t i = 0; i < m_call_count; ++i) {
        CallSite& site = m_calls[i];
        if (site.target != nullptr && !heap.reached(site.target))
            site.target = nullptr;
    }
}

std::size_t FeedbackVector::size_in_bytes() const
{
    std::size_t bytes = sizeof(*this) + m_property_count * sizeof(PropertySite) + m_call_count * sizeof(CallSite);
    for (std::uint32_t i = 0; i < m_property_count; ++i) {
        if (m_properties[i].more != nullptr)
            bytes += PropertySite::width * sizeof(PropertyEntry);
    }
    return bytes;
}

} // namespace sashfold::js
