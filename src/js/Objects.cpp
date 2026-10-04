#include "js/Object.h"

#include "js/Ast.h"
#include "js/Interpreter.h"
#include "js/MissCensus.h"
#include "js/Module.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {

// How far past the dense storage an index may land and still grow it
// with holes rather than becoming an ordinary (sparse) property.
constexpr std::uint32_t dense_growth_limit = 1024;

constexpr std::u16string_view length_name = u"length";

// SameValue (§7.2.10) without the interpreter: NaN equals NaN, +0 and −0
// differ, strings compare by contents, the rest by identity. It is what
// ValidateAndApplyPropertyDescriptor compares with.
bool same_value(Value const& a, Value const& b)
{
    if (a.type() != b.type())
        return false;
    if (a.is_number()) {
        double const x = a.as_number();
        double const y = b.as_number();
        if (std::isnan(x) && std::isnan(y))
            return true;
        return x == y && std::signbit(x) == std::signbit(y);
    }
    if (a.is_string())
        return a.as_string()->equals(*b.as_string());
    return a == b;
}

void set_flag(std::uint8_t& attributes, Attribute flag, bool on)
{
    if (on)
        attributes = static_cast<std::uint8_t>(attributes | flag);
    else
        attributes = static_cast<std::uint8_t>(attributes & ~flag);
}

bool atom_is(JsString const* atom, std::u16string_view name)
{
    return atom->view() == name;
}

// Is this key the "length" atom? Compared by contents: an atom key that
// spells "length" is that atom in every heap.
bool is_length_key(PropertyKey const& key)
{
    return key.is_atom() && atom_is(key.as_atom(), length_name);
}

// The "length" atom of the realm `object` belongs to. An Array or String
// exotic object answers `length` from a member and never stores it, yet
// [[OwnPropertyKeys]] must list it, which takes the interned atom of the
// object's own heap.
JsString* length_atom_for(Object const& object)
{
    Heap const* heap = object.heap();
    return heap ? heap->atoms().length : nullptr;
}

// StringGetOwnProperty (§10.4.3.5) answers each code unit with a one-unit
// string, and get_own_property has no way to allocate one without the
// risk of a collection under its caller. The units are served from a
// table of cells that belong to no heap: immutable, never swept (a
// collector frees only what it adopted, and Tracer::visit marks a foreign
// cell and moves on), so the value stays correct wherever a script
// carries it.
JsString* code_unit_string(char16_t unit)
{
    thread_local std::deque<JsString> cells;
    thread_local std::unordered_map<char16_t, JsString*> table;
    auto const found = table.find(unit);
    if (found != table.end())
        return found->second;
    JsString* cell = &cells.emplace_back(std::u16string(1, unit));
    table.emplace(unit, cell);
    return cell;
}

// Steps 2 through 5 of ValidateAndApplyPropertyDescriptor (§10.1.6.3),
// which are also IsCompatiblePropertyDescriptor (§10.1.6.2): may `desc`
// be applied over `current` on an object of this extensibility? Nothing
// is changed here; the storage-specific callers apply what passes.
bool is_compatible(bool extensible, PropertyDescriptor const& desc, std::optional<PropertyDescriptor> const& current)
{
    if (!current)
        return extensible;
    // Step 4: a descriptor with no fields changes nothing.
    if (desc.is_generic() && !desc.enumerable && !desc.configurable)
        return true;
    if (current->configurable.value_or(false))
        return true;
    // Step 5: a non-configurable property admits only changes that leave
    // it observably the same, plus writable going false.
    if (desc.configurable.value_or(false))
        return false;
    if (desc.enumerable && *desc.enumerable != current->enumerable.value_or(false))
        return false;
    if (!desc.is_generic() && desc.is_accessor() != current->is_accessor())
        return false;
    if (current->is_accessor()) {
        if (desc.get && *desc.get != current->get.value_or(nullptr))
            return false;
        if (desc.set && *desc.set != current->set.value_or(nullptr))
            return false;
        return true;
    }
    if (!current->writable.value_or(false)) {
        if (desc.writable.value_or(false))
            return false;
        if (desc.value && !same_value(*desc.value, current->value.value_or(Value::undefined())))
            return false;
    }
    return true;
}

// Step 2.c–d of §10.1.6.3: the property a descriptor creates from
// nothing. Absent fields take their defaults (undefined and false).
Property property_from(PropertyKey const& key, PropertyDescriptor const& desc)
{
    Property property;
    property.key = key;
    property.attributes = 0;
    set_flag(property.attributes, Enumerable, desc.enumerable.value_or(false));
    set_flag(property.attributes, Configurable, desc.configurable.value_or(false));
    if (desc.is_accessor()) {
        property.accessor = true;
        property.getter = desc.get.value_or(nullptr);
        property.setter = desc.set.value_or(nullptr);
    } else {
        property.value = desc.value.value_or(Value::undefined());
        set_flag(property.attributes, Writable, desc.writable.value_or(false));
    }
    return property;
}

// Step 6 of §10.1.6.3, applied to an existing property that validation
// has already admitted: a change of kind keeps enumerable and
// configurable and resets the rest; otherwise each present field lands.
void apply_to(Property& property, PropertyDescriptor const& desc)
{
    if (!property.accessor && desc.is_accessor()) {
        property.accessor = true;
        property.value = Value::undefined();
        set_flag(property.attributes, Writable, false);
        property.getter = desc.get.value_or(nullptr);
        property.setter = desc.set.value_or(nullptr);
    } else if (property.accessor && desc.is_data()) {
        property.accessor = false;
        property.getter = nullptr;
        property.setter = nullptr;
        property.value = desc.value.value_or(Value::undefined());
        set_flag(property.attributes, Writable, desc.writable.value_or(false));
    } else {
        if (desc.value)
            property.value = *desc.value;
        if (desc.writable)
            set_flag(property.attributes, Writable, *desc.writable);
        if (desc.get)
            property.getter = *desc.get;
        if (desc.set)
            property.setter = *desc.set;
    }
    if (desc.enumerable)
        set_flag(property.attributes, Enumerable, *desc.enumerable);
    if (desc.configurable)
        set_flag(property.attributes, Configurable, *desc.configurable);
}

// Only a plain data property with the default attributes may live in an
// array's dense storage, which has no room for attributes.
bool is_dense_eligible(Property const& property)
{
    return !property.accessor && property.attributes == default_attributes;
}

// An integral Number in 0 … 2^32 − 1, as ArraySetLength expects once its
// caller has done ToUint32 and the RangeError check (§10.4.2.4 steps 3–5).
std::optional<std::uint32_t> validated_uint32(Value const& value)
{
    if (!value.is_number())
        return std::nullopt;
    double const number = value.as_number();
    if (!(number >= 0 && number <= 4294967295.0) || std::floor(number) != number)
        return std::nullopt;
    return static_cast<std::uint32_t>(number);
}

bool has_index_key(Object const& object)
{
    return object.shape()->has_index_keys();
}

// Does this object, on its own, answer any array index? What the dense
// fast paths must not find anywhere on an array's prototype chain. The
// class ids stand for the exotic classes the way is_array() already does.
bool has_indexed_properties(Object const& object)
{
    if (object.is_host())
        return true; // the bindings may answer indices the storage does not show
    if (object.class_id() == Object::Class::TypedArray)
        return true; // every index below its length, out of the buffer
    if (has_index_key(object))
        return true;
    if (object.is_array()) {
        for (Value const& element : static_cast<ArrayObject const&>(object).dense()) {
            if (!element.is_empty())
                return true;
        }
    }
    if (object.class_id() == Object::Class::String)
        return static_cast<StringObject const&>(object).string()->length() > 0;
    return false;
}

bool is_receiver(Value const& receiver, Object const* object)
{
    return receiver.is_object() && receiver.as_object() == object;
}

}

bool is_compatible_property_descriptor(bool extensible, PropertyDescriptor const& desc,
    std::optional<PropertyDescriptor> const& current)
{
    return is_compatible(extensible, desc, current);
}

// ---------------------------------------------------------------- Object

namespace {

// The exotic classes whose own properties are not all their storage's:
// an array's length and elements, a string's units, a typed array's
// numeric keys, a mapped arguments object's parameters, a namespace's
// bindings, a proxy's every key.
std::uint8_t root_flags(Object::Class class_id)
{
    switch (class_id) {
    case Object::Class::Array:
    case Object::Class::String:
    case Object::Class::Arguments:
    case Object::Class::TypedArray:
    case Object::Class::ModuleNamespace:
    case Object::Class::Proxy:
        return Shape::Uncacheable;
    default:
        return 0;
    }
}

}

Object::Object(Object* prototype, Class class_id)
    : Cell(CellKind::Object)
    , m_slots(m_inline_slots)
    , m_class(class_id)
{
    // Every object is made by a heap's allocate(), which names itself
    // while the object is constructed.
    m_shape = Shape::root(*Heap::constructing(), prototype, root_flags(class_id));
    if (class_id == Class::Global)
        become_dictionary();
}

Object::~Object()
{
    if (m_slots != m_inline_slots)
        std::free(m_slots);
    // A dictionary's shape is the object's own (Shape::to_dictionary).
    if (m_owns_shape)
        delete m_shape;
}

Heap& Object::owner() const
{
    Heap* const adopted = heap();
    return adopted != nullptr ? *adopted : *Heap::constructing();
}

// The lookup every property read in the engine goes through, inlined into
// each of this file's internal methods: what is put off is made out of
// line, so a read that finds an ordinary property pays one test of its
// slot for it, and a read that finds none one test of the shape's flags.
[[gnu::always_inline]] inline ShapeEntry const* Object::whole_entry(PropertyKey const& key) const
{
    ShapeEntry const* const entry = m_shape->find(key);
    if (entry != nullptr) {
        if (m_slots[entry->slot].is_lazy_mark()) [[unlikely]]
            return const_cast<Object*>(this)->find_lazy(key);
        return entry;
    }
    if (m_shape->pending() != 0) [[unlikely]]
        return const_cast<Object*>(this)->find_pending(key);
    return nullptr;
}

// A lazy property made whole, and the property as it then stands: a
// native's function is made in place; a value's maker may have defined
// other properties, this one among them, so the property is looked for
// again — it is gone if the maker removed it, and still to be made if the
// maker put a native in its place. Making what was put off changes
// nothing a caller can tell from its having been there all along, which
// is what lets a const lookup do it.
[[gnu::noinline]] ShapeEntry const* Object::find_lazy(PropertyKey const& key)
{
    for (;;) {
        ShapeEntry const* const entry = m_shape->find(key);
        if (entry == nullptr || !m_slots[entry->slot].is_lazy_mark())
            return entry;
        make_lazy(key, *m_shape->realm());
    }
}

[[gnu::noinline]] ShapeEntry const* Object::find_pending(PropertyKey const& key)
{
    std::uint8_t const which = pending_named(key);
    if (which == 0)
        return nullptr;
    settle(which);
    return m_shape->find(key);
}

PropertyRef Object::find_own(PropertyKey const& key) const
{
    ShapeEntry const* const entry = whole_entry(key);
    return entry != nullptr ? PropertyRef(view_of(*entry)) : PropertyRef();
}

PropertyRef Object::peek_own(PropertyKey const& key) const
{
    ShapeEntry const* const entry = m_shape->find(key);
    return entry != nullptr ? PropertyRef(view_of(*entry)) : PropertyRef();
}

bool Object::write_own_value(PropertyKey const& key, Value const& value)
{
    ShapeEntry const* const entry = whole_entry(key);
    if (entry == nullptr || entry->accessor)
        return false;
    m_slots[entry->slot] = value;
    return true;
}

// The property as a reader outside the storage sees it.
Property Object::view_of(ShapeEntry const& entry) const
{
    Property view;
    view.key = entry.key;
    view.attributes = entry.attributes;
    view.accessor = entry.accessor;
    Value const held = m_slots[entry.slot];
    if (held.is_lazy_mark()) {
        view.lazy_realm = m_shape->realm();
        if (!held.is_second_lazy_mark()) {
            view.lazy = Property::LazyNative;
            view.lazy_get = static_cast<NativeSpec const*>(held.as_lazy_mark());
            view.lazy_set = nullptr;
            return view;
        }
        LazyRecord const& record = *static_cast<LazyRecord const*>(held.as_lazy_mark());
        if (record.make != nullptr) {
            view.lazy = Property::LazyValue;
            view.lazy_make = record.make;
            view.lazy_set = nullptr;
        } else {
            view.lazy = Property::LazyNative;
            view.lazy_get = record.get;
            view.lazy_set = record.set;
        }
        return view;
    }
    if (entry.accessor) {
        auto const* const pair = static_cast<AccessorPair const*>(held.as_cell());
        view.getter = pair->getter;
        view.setter = pair->setter;
    } else {
        view.value = held;
    }
    return view;
}

PropertyDescriptor Object::descriptor_of(ShapeEntry const& entry) const
{
    Value const held = m_slots[entry.slot];
    if (entry.accessor) {
        auto const* const pair = static_cast<AccessorPair const*>(held.as_cell());
        return PropertyDescriptor::accessor(pair->getter, pair->setter, entry.attributes);
    }
    return PropertyDescriptor::data(held, entry.attributes);
}

// The native a lazy property described, or the value it is made by, made
// now: its function is of the realm the property was defined in,
// whichever realm's code asks first, and no collection runs under a
// caller that only looked a property up.
[[gnu::noinline]] void Object::make_lazy(PropertyKey const& key, RealmRecord& realm, bool at_once)
{
    Heap& heap = owner();
    Heap::NoCollect const guard(heap);
    ShapeEntry const& entry = *m_shape->find(key);
    std::uint32_t const slot = entry.slot;
    bool const accessor = entry.accessor;
    Value const mark = m_slots[slot];
    NativeSpec const* get = nullptr;
    NativeSpec const* set = nullptr;
    if (mark.is_second_lazy_mark()) {
        LazyRecord const& record = *static_cast<LazyRecord const*>(mark.as_lazy_mark());
        if (record.make != nullptr) {
            // An ordinary property from here on, undefined until its maker
            // answers: whatever the maker does to this object — defining
            // other properties, this one included, or looking this one up
            // again — it does to a whole object. The maker may have changed
            // the object's shape, so the property is found again before its
            // answer is put.
            m_slots[slot] = Value::undefined();
            ++heap.lazy_census().values_made;
            Value const made = record.make(*this, key, realm);
            if (ShapeEntry const* const now = m_shape->find(key); now != nullptr && !now->accessor && m_slots[now->slot].is_undefined())
                m_slots[now->slot] = made;
            return;
        }
        get = record.get;
        set = record.set;
    } else {
        get = static_cast<NativeSpec const*>(mark.as_lazy_mark());
    }
    auto const make = [&](NativeSpec const& spec, NativeFunction::Role role) {
        auto* const function = heap.allocate<NativeFunction>(realm.intrinsics.function_prototype, spec, key, role, this);
        function->set_realm(&realm);
        if (at_once) {
            ++heap.lazy_census().natives_made_at_once;
            function->settle_pending();
        } else {
            ++heap.lazy_census().natives_made_later;
        }
        return function;
    };
    if (accessor) {
        Object* const getter = get != nullptr ? make(*get, NativeFunction::Role::Getter) : nullptr;
        Object* const setter = set != nullptr ? make(*set, NativeFunction::Role::Setter) : nullptr;
        m_slots[slot] = Value::slot_cell(heap.allocate<AccessorPair>(getter, setter));
    } else {
        m_slots[slot] = Value::object(make(*get, NativeFunction::Role::Method));
    }
}

void Object::set_pending(std::uint8_t which)
{
    auto const flags = static_cast<std::uint8_t>((m_shape->flags() & ~Shape::PendingMask) | which);
    if (m_shape->is_dictionary())
        m_shape->set_flags(flags);
    else
        m_shape = m_shape->with_flags(owner(), flags);
}

std::pair<Value, std::uint8_t> Object::make_pending(std::uint8_t)
{
    return { Value::undefined(), default_attributes };
}

// Which pending property a key names; 0 for a key that names none.
std::uint8_t Object::pending_named(PropertyKey const& key) const
{
    std::uint8_t const pending = m_shape->pending();
    if (pending == 0 || !key.is_atom())
        return 0;
    WellKnownAtoms const& atoms = owner().atoms();
    JsString const* const name = key.as_atom();
    std::uint8_t const which = name == atoms.length ? PendingLength : name == atoms.name ? PendingName : name == atoms.prototype ? PendingPrototype : 0;
    return static_cast<std::uint8_t>(which & pending);
}

// A pending property given its room, after those before it in the
// standard's order.
void Object::settle(std::uint8_t which)
{
    for (std::uint8_t const earlier : { PendingLength, PendingName }) {
        if (earlier < which && is_pending(earlier))
            settle_one(earlier);
    }
    if (is_pending(which))
        settle_one(which);
}

void Object::settle_one(std::uint8_t which)
{
    Heap& heap = owner();
    Heap::NoCollect const guard(heap);
    // The value first: making it may look this object's other properties up.
    auto const [value, attributes] = make_pending(which);
    if (!is_pending(which))
        return;
    WellKnownAtoms const& atoms = heap.atoms();
    JsString* const name = which == PendingLength ? atoms.length : which == PendingName ? atoms.name : atoms.prototype;
    std::uint32_t const slot = add_property(PropertyKey::atom(name), attributes, false);
    m_slots[slot] = value;
}

void Object::settle_all()
{
    for (std::uint8_t const which : { PendingLength, PendingName, PendingPrototype }) {
        if (is_pending(which))
            settle_one(which);
    }
}

void Object::settle_pending()
{
    if (m_shape->pending() != 0)
        settle_all();
}

std::size_t Object::own_property_count() const
{
    return m_shape->property_count() + static_cast<std::size_t>(std::popcount(static_cast<unsigned>(m_shape->pending())));
}

void Object::reserve_properties(std::size_t count)
{
    grow_slots(static_cast<std::uint32_t>(m_slot_count + count));
}

void Object::grow_slots(std::uint32_t needed)
{
    if (needed <= m_slot_capacity)
        return;
    std::uint32_t const capacity = std::max(needed, m_slot_capacity + m_slot_capacity / 2 + 2);
    bool const inline_before = m_slots == m_inline_slots;
    Value* block = nullptr;
    if (inline_before) {
        block = static_cast<Value*>(std::malloc(capacity * sizeof(Value)));
        if (block != nullptr)
            std::memcpy(static_cast<void*>(block), static_cast<void const*>(m_inline_slots), m_slot_count * sizeof(Value));
    } else {
        block = static_cast<Value*>(std::realloc(static_cast<void*>(m_slots), capacity * sizeof(Value)));
    }
    if (block == nullptr)
        std::abort();
    if (Heap* const adopted = heap())
        adopted->grew((capacity - (inline_before ? 0 : m_slot_capacity)) * sizeof(Value));
    m_slots = block;
    m_slot_capacity = capacity;
}

// A slot for a new property: a dictionary's freed one, or the next.
std::uint32_t Object::take_slot()
{
    if (m_shape->is_dictionary()) {
        std::vector<std::uint32_t>& freed = m_shape->free_slots();
        if (!freed.empty()) {
            std::uint32_t const slot = freed.back();
            freed.pop_back();
            return slot;
        }
    }
    grow_slots(m_slot_count + 1);
    m_slots[m_slot_count] = Value::undefined();
    return m_slot_count++;
}

// A property given room at the end of the order: what the object owes
// itself first is given its room first (the pending properties before the
// key, or all of them before any other key); a shared shape moves on by
// its transition and a dictionary adds in place. The slot it answers holds
// undefined until the caller fills it. Never collects.
bool Object::is_function_own(PropertyKey const& key) const
{
    if (!key.is_atom())
        return false;
    WellKnownAtoms const& atoms = owner().atoms();
    JsString const* const name = key.as_atom();
    return name == atoms.length || name == atoms.name || name == atoms.prototype;
}

std::uint32_t Object::add_property(PropertyKey const& key, std::uint8_t attributes, bool accessor)
{
    Heap& heap = owner();
    Heap::NoCollect const no_collect(heap);
    if (m_shape->pending() != 0) [[unlikely]] {
        std::uint8_t const which = pending_named(key);
        for (std::uint8_t const earlier : { PendingLength, PendingName, PendingPrototype }) {
            if (which != 0 && earlier >= which)
                break;
            if (is_pending(earlier))
                settle_one(earlier);
        }
    }
    if (m_shape->is_dictionary()) {
        std::uint8_t const which = pending_named(key);
        std::uint32_t const slot = take_slot();
        m_shape->table()->append(ShapeEntry { key, slot, attributes, accessor }, &heap);
        if (which != 0)
            m_shape->set_flags(static_cast<std::uint8_t>(m_shape->flags() & ~which));
        return slot;
    }
    // Past the threshold an object is a dictionary, and so is a function
    // given anything beyond its own length, name and prototype: the
    // statics and constants of a class or an interface, which no other
    // object adds in the same order, so that their shapes would be its alone.
    if (m_shape->count() >= Shape::dictionary_threshold || (m_class == Class::Function && !is_function_own(key))) {
        become_dictionary();
        return add_property(key, attributes, accessor);
    }
    Shape* const next = m_shape->adding(heap, key, attributes, accessor);
    std::uint32_t const slot = take_slot();
    m_shape = next;
    return slot;
}

void Object::reconfigure(std::uint32_t position, std::uint8_t attributes, bool accessor)
{
    if (m_shape->is_dictionary()) {
        ShapeEntry& entry = (*m_shape->table())[position];
        entry.attributes = attributes;
        entry.accessor = accessor;
        return;
    }
    m_shape = m_shape->reconfiguring(owner(), position, attributes, accessor);
}

void Object::set_accessor_slot(std::uint32_t slot, Object* getter, Object* setter, bool reuse)
{
    if (reuse) {
        auto* const pair = static_cast<AccessorPair*>(m_slots[slot].as_cell());
        pair->getter = getter;
        pair->setter = setter;
        return;
    }
    Heap& heap = owner();
    Heap::NoCollect const no_collect(heap);
    m_slots[slot] = Value::slot_cell(heap.allocate<AccessorPair>(getter, setter));
}

void Object::become_dictionary()
{
    if (m_shape->is_dictionary())
        return;
    m_shape = m_shape->to_dictionary(owner());
    m_owns_shape = true;
}

void Object::mark_uncacheable()
{
    auto const flags = static_cast<std::uint8_t>(m_shape->flags() | Shape::Uncacheable);
    if (m_shape->is_dictionary())
        m_shape->set_flags(flags);
    else
        m_shape = m_shape->with_flags(owner(), flags);
}

void Object::prevent_extensions()
{
    auto const flags = static_cast<std::uint8_t>(m_shape->flags() | Shape::NotExtensible);
    if (m_shape->is_dictionary())
        m_shape->set_flags(flags);
    else
        m_shape = m_shape->with_flags(owner(), flags);
}

void Object::put(PropertyKey const& key, Value const& value, std::uint8_t attributes)
{
    if (m_class == Class::Array && key.is_index()) {
        // put() is not virtual, and an array answers an index from its
        // dense storage before its ordinary storage (every index goes
        // through the array's own [[DefineOwnProperty]], §10.4.2.1): an
        // ordinary property at an index the block covers would shadow
        // the element, or be shadowed by it. So the value goes where
        // set_element puts it, and attributes the block has no room for
        // move the element — and everything above it, so that every
        // ordinary index stays at or past the dense size — out of it.
        auto& array = static_cast<ArrayObject&>(*this);
        std::uint32_t const index = key.as_index();
        array.set_element(index, value);
        ElementBlock& dense = array.dense();
        if (attributes != default_attributes && index < dense.size()) {
            for (std::uint32_t i = index; i < dense.size(); ++i) {
                if (!dense[i].is_empty())
                    put_data(PropertyKey::index(i), dense[i], default_attributes);
            }
            dense.resize(index);
        }
        if (own_entry(key) != nullptr)
            put_data(key, value, attributes);
        return;
    }
    put_data(key, value, attributes);
}

// What is there is overwritten whole, so a native not yet made is simply
// dropped, and a pending property takes its place in the order without
// its own value ever being made.
void Object::put_data(PropertyKey const& key, Value const& value, std::uint8_t attributes)
{
    Heap::NoCollect const no_collect(owner());
    if (std::uint32_t const position = m_shape->position_of(key); position != ShapeTable::npos) {
        ShapeEntry const& entry = m_shape->at(position);
        std::uint32_t const slot = entry.slot;
        if (entry.attributes != attributes || entry.accessor)
            reconfigure(position, attributes, false);
        m_slots[slot] = value;
        return;
    }
    std::uint32_t const slot = add_property(key, attributes, false);
    m_slots[slot] = value;
}

void Object::put_accessor(PropertyKey const& key, Object* getter, Object* setter, std::uint8_t attributes)
{
    Heap::NoCollect const no_collect(owner());
    auto const accessor_attributes = static_cast<std::uint8_t>(attributes & ~Writable);
    if (std::uint32_t const position = m_shape->position_of(key); position != ShapeTable::npos) {
        ShapeEntry const& entry = m_shape->at(position);
        std::uint32_t const slot = entry.slot;
        bool const reuse = entry.accessor && !m_slots[slot].is_lazy_mark();
        if (entry.attributes != accessor_attributes || !entry.accessor)
            reconfigure(position, accessor_attributes, true);
        set_accessor_slot(slot, getter, setter, reuse);
        return;
    }
    std::uint32_t const slot = add_property(key, accessor_attributes, true);
    set_accessor_slot(slot, getter, setter, false);
}

// A holder of properties not yet made is a dictionary, whose realm they
// are made in. False for a holder whose other such properties are another
// realm's: the caller then makes the property at once.
bool Object::ready_for_lazy(RealmRecord& realm)
{
    become_dictionary();
    if (m_shape->realm() == nullptr)
        m_shape->set_realm(&realm);
    return m_shape->realm() == &realm;
}

void Object::put_mark(PropertyKey const& key, Value const& mark, std::uint8_t attributes, bool accessor)
{
    if (std::uint32_t const position = m_shape->position_of(key); position != ShapeTable::npos) {
        ShapeEntry const& entry = m_shape->at(position);
        std::uint32_t const slot = entry.slot;
        if (entry.attributes != attributes || entry.accessor != accessor)
            reconfigure(position, attributes, accessor);
        m_slots[slot] = mark;
        return;
    }
    std::uint32_t const slot = add_property(key, attributes, accessor);
    m_slots[slot] = mark;
}

void Object::put_lazy(PropertyKey const& key, NativeSpec const& method, RealmRecord& realm, std::uint8_t attributes)
{
    Heap& heap = owner();
    Heap::NoCollect const no_collect(heap);
    bool const kept = ready_for_lazy(realm);
    put_mark(key, Value::lazy_mark(&method, false), attributes, false);
    if (!lazy_natives() || !kept)
        make_lazy(key, realm, true);
    else
        ++heap.lazy_census().natives_described;
}

void Object::put_lazy_accessor(PropertyKey const& key, NativeSpec const* getter, NativeSpec const* setter, RealmRecord& realm,
    std::uint8_t attributes)
{
    Heap& heap = owner();
    Heap::NoCollect const no_collect(heap);
    bool const kept = ready_for_lazy(realm);
    Value const mark = setter == nullptr && getter != nullptr
        ? Value::lazy_mark(getter, false)
        : Value::lazy_mark(&LazyRecord::intern(LazyRecord { getter, setter, nullptr }), true);
    put_mark(key, mark, static_cast<std::uint8_t>(attributes & ~Writable), true);
    if (!lazy_natives() || !kept)
        make_lazy(key, realm, true);
    else
        heap.lazy_census().natives_described += (getter != nullptr ? 1u : 0u) + (setter != nullptr ? 1u : 0u);
}

void Object::put_lazy_value(PropertyKey const& key, LazyValueMaker make, RealmRecord& realm, std::uint8_t attributes)
{
    Heap& heap = owner();
    Heap::NoCollect const no_collect(heap);
    bool const kept = ready_for_lazy(realm);
    put_mark(key, Value::lazy_mark(&LazyRecord::intern(LazyRecord { nullptr, nullptr, make }), true), attributes, false);
    ++heap.lazy_census().values_described;
    if (!lazy_natives() || !kept)
        make_lazy(key, realm, true);
}

bool Object::remove_own(PropertyKey const& key)
{
    if (m_class == Class::Array && key.is_index()) {
        // The dense side of the same routing as put(): an element the
        // block holds becomes a hole.
        ElementBlock& dense = static_cast<ArrayObject&>(*this).dense();
        std::uint32_t const index = key.as_index();
        if (index < dense.size()) {
            bool const present = !dense[index].is_empty();
            dense[index] = Value::empty();
            return present;
        }
    }
    if (m_shape->position_of(key) == ShapeTable::npos) {
        // A pending property gone before it was ever given room.
        if (std::uint8_t const which = pending_named(key)) {
            set_pending(static_cast<std::uint8_t>(m_shape->pending() & ~which));
            return true;
        }
        return false;
    }
    delete_entry(key);
    return true;
}

// A property gone: the object is a dictionary from here on, the entry a
// hole and its slot free for the next property.
void Object::delete_entry(PropertyKey const& key)
{
    Heap::NoCollect const no_collect(owner());
    become_dictionary();
    ShapeTable& table = *m_shape->table();
    std::uint32_t const position = m_shape->position_of(key);
    std::uint32_t const slot = table[position].slot;
    table.erase(position);
    m_slots[slot] = Value::undefined();
    m_shape->free_slots().push_back(slot);
    if (table.holes() > ShapeTable::linear_limit && table.holes() * 2 > table.size())
        table.compact();
}

bool Object::set_prototype(Object* proto)
{
    // OrdinarySetPrototypeOf (§10.1.2.1): the same prototype is always
    // fine, even on a non-extensible object; otherwise the object must be
    // extensible and the new chain must not run back into this object.
    if (proto == prototype())
        return true;
    if (!is_extensible())
        return false;
    for (Object const* link = proto; link != nullptr; link = link->prototype()) {
        if (link == this)
            return false;
    }
    Heap& heap = owner();
    Heap::NoCollect const no_collect(heap);
    if (m_shape->is_dictionary()) {
        // An object that is another's prototype is a dictionary (Shape.h).
        if (proto != nullptr)
            proto->become_dictionary();
        m_shape->set_prototype(proto);
        return true;
    }
    // A shared shape's properties are replayed from the new prototype's
    // root, so the pending ones go first if there are others.
    if (m_shape->pending() != 0 && m_shape->count() != 0)
        settle_all();
    m_shape = m_shape->with_prototype(heap, proto);
    return true;
}

std::optional<PropertyDescriptor> Object::get_own_property(PropertyKey const& key) const
{
    ShapeEntry const* const entry = whole_entry(key);
    if (entry == nullptr)
        return std::nullopt;
    return descriptor_of(*entry);
}

bool Object::define_own_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    // OrdinaryDefineOwnProperty (§10.1.6.1) over the ordinary storage.
    // The exotic subclasses handle their own keys before coming here, so
    // the current descriptor is read from the storage, not virtually.
    ShapeEntry const* const existing = whole_entry(key);
    std::optional<PropertyDescriptor> current;
    if (existing != nullptr)
        current = descriptor_of(*existing);
    if (!is_compatible(is_extensible(), desc, current))
        return false;
    if (existing != nullptr)
        apply_descriptor(*existing, desc);
    else
        create_property(key, desc);
    return true;
}

// Step 2.c–d of §10.1.6.3: the property a descriptor creates from
// nothing. Absent fields take their defaults (undefined and false).
void Object::create_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    Heap::NoCollect const no_collect(owner());
    std::uint8_t attributes = 0;
    set_flag(attributes, Enumerable, desc.enumerable.value_or(false));
    set_flag(attributes, Configurable, desc.configurable.value_or(false));
    if (desc.is_accessor()) {
        std::uint32_t const slot = add_property(key, attributes, true);
        set_accessor_slot(slot, desc.get.value_or(nullptr), desc.set.value_or(nullptr), false);
        return;
    }
    set_flag(attributes, Writable, desc.writable.value_or(false));
    std::uint32_t const slot = add_property(key, attributes, false);
    m_slots[slot] = desc.value.value_or(Value::undefined());
}

// Step 6 of §10.1.6.3, applied to an existing property that validation
// has already admitted: a change of kind keeps enumerable and
// configurable and resets the rest; otherwise each present field lands.
// The shape changes only when the attributes or the kind do.
void Object::apply_descriptor(ShapeEntry const& entry, PropertyDescriptor const& desc)
{
    Heap::NoCollect const no_collect(owner());
    std::uint32_t const position = m_shape->position_of(entry.key);
    std::uint32_t const slot = entry.slot;
    std::uint8_t const attributes_before = entry.attributes;
    bool const accessor_before = entry.accessor;
    std::uint8_t attributes = attributes_before;
    bool accessor = accessor_before;
    Value value = m_slots[slot];
    Object* getter = nullptr;
    Object* setter = nullptr;
    if (accessor_before) {
        auto const* const pair = static_cast<AccessorPair const*>(value.as_cell());
        getter = pair->getter;
        setter = pair->setter;
    }
    if (!accessor_before && desc.is_accessor()) {
        accessor = true;
        set_flag(attributes, Writable, false);
        getter = desc.get.value_or(nullptr);
        setter = desc.set.value_or(nullptr);
    } else if (accessor_before && desc.is_data()) {
        accessor = false;
        value = desc.value.value_or(Value::undefined());
        set_flag(attributes, Writable, desc.writable.value_or(false));
    } else {
        if (desc.value)
            value = *desc.value;
        if (desc.writable)
            set_flag(attributes, Writable, *desc.writable);
        if (desc.get)
            getter = *desc.get;
        if (desc.set)
            setter = *desc.set;
    }
    if (desc.enumerable)
        set_flag(attributes, Enumerable, *desc.enumerable);
    if (desc.configurable)
        set_flag(attributes, Configurable, *desc.configurable);
    if (attributes != attributes_before || accessor != accessor_before)
        reconfigure(position, attributes, accessor);
    if (accessor)
        set_accessor_slot(slot, getter, setter, accessor_before);
    else
        m_slots[slot] = value;
}

bool Object::has_property(PropertyKey const& key) const
{
    // OrdinaryHasProperty (§10.1.7.1), the recursion over the chain
    // unrolled. Each link answers through its own [[GetOwnProperty]], so
    // an exotic prototype's virtual keys count.
    for (Object const* link = this; link != nullptr; link = link->prototype()) {
        if (link->get_own_property(key))
            return true;
    }
    if (miss_census_on())
        note_miss(*this, key, true);
    return false;
}

std::optional<Value> Object::get(Interpreter& interpreter, PropertyKey const& key, Value const& receiver)
{
    // OrdinaryGet (§10.1.8.1), the recursion over the chain unrolled.
    for (Object* link = this; link != nullptr; link = link->prototype()) {
        // A proxy up the chain answers for itself (§10.5.8): only its own
        // [[Get]] can run the handler's trap, and the receiver it is
        // handed is the one the walk started with.
        if (link != this && link->is_proxy())
            return link->get(interpreter, key, receiver);
        std::optional<PropertyDescriptor> const desc = link->get_own_property(key);
        if (!desc)
            continue;
        if (!desc->is_accessor())
            return desc->value.value_or(Value::undefined());
        Object* getter = desc->get.value_or(nullptr);
        if (getter == nullptr)
            return Value::undefined();
        return interpreter.call(Value::object(getter), receiver, {});
    }
    if (miss_census_on())
        note_miss(*this, key, false);
    return Value::undefined();
}

std::optional<bool> Object::set(Interpreter& interpreter, PropertyKey const& key, Value const& value, Value const& receiver)
{
    // OrdinarySet (§10.1.9.1) with OrdinarySetWithOwnDescriptor
    // (§10.1.9.2) unrolled: the first own descriptor up the chain decides,
    // and a chain with none behaves as a writable data property would.
    std::optional<PropertyDescriptor> own;
    for (Object* link = this; link != nullptr; link = link->prototype()) {
        // A typed array up the chain answers for itself (§10.4.5.5): its
        // [[Set]] decides a numeric key with no look further up and no
        // property created on the receiver, so the walk hands over to it.
        // A proxy does too (§10.5.9): only its own [[Set]] runs the trap.
        if (link != this && (link->class_id() == Class::TypedArray || link->is_proxy()))
            return link->set(interpreter, key, value, receiver);
        own = link->get_own_property(key);
        if (own)
            break;
    }
    if (!own)
        own = PropertyDescriptor::data(Value::undefined(), default_attributes);
    if (own->is_accessor()) {
        Object* setter = own->set.value_or(nullptr);
        if (setter == nullptr)
            return false;
        Value const arguments[1] = { value };
        if (!interpreter.call(Value::object(setter), receiver, arguments))
            return std::nullopt;
        return true;
    }
    if (!own->writable.value_or(false))
        return false;
    if (!receiver.is_object())
        return false;
    // The write lands on the receiver: an existing data property there is
    // updated in place (only its value), a missing one is created as
    // assignment creates properties (CreateDataProperty), and an accessor
    // or a read-only property on the receiver refuses. A receiver whose
    // [[DefineOwnProperty]] converts the value by running script — an
    // array's length, a typed array's element — takes the interpreter's
    // wrapper; an ordinary receiver takes the virtual directly, and needs
    // no interpreter for it.
    Object* target = receiver.as_object();
    bool const converts = target->class_id() == Class::TypedArray || target->is_proxy() || (target->is_array() && is_length_key(key));
    auto const define = [&](PropertyDescriptor const& desc) -> std::optional<bool> {
        if (converts)
            return interpreter.define_own_property(*target, key, desc);
        return target->define_own_property(key, desc);
    };
    // A module namespace receiver reads its export's binding here, which
    // throws in its dead zone (§10.4.6.4), and a proxy receiver runs its
    // getOwnPropertyDescriptor trap: the interpreter's wrapper.
    std::optional<PropertyDescriptor> existing;
    if (target->class_id() == Class::ModuleNamespace || target->is_proxy()) {
        std::optional<std::optional<PropertyDescriptor>> const read = interpreter.get_own_property(*target, key);
        if (!read)
            return std::nullopt;
        existing = *read;
    } else {
        existing = target->get_own_property(key);
    }
    if (existing) {
        if (existing->is_accessor())
            return false;
        if (!existing->writable.value_or(false))
            return false;
        PropertyDescriptor value_only;
        value_only.value = value;
        return define(value_only);
    }
    return define(PropertyDescriptor::data(value, default_attributes));
}

bool Object::delete_property(PropertyKey const& key)
{
    // OrdinaryDelete (§10.1.10.1): a missing property deletes fine.
    ShapeEntry const* const entry = whole_entry(key);
    if (entry == nullptr)
        return true;
    if ((entry->attributes & Configurable) == 0)
        return false;
    delete_entry(key);
    return true;
}

std::vector<PropertyKey> Object::own_keys() const
{
    // OrdinaryOwnPropertyKeys (§10.1.11.1). Every key is listed, so what
    // the object still owes itself is given its room first.
    if (m_shape->pending() != 0)
        const_cast<Object*>(this)->settle_all();
    std::vector<std::uint32_t> indices;
    std::vector<PropertyKey> atoms;
    std::vector<PropertyKey> symbols;
    Shape const& shape = *m_shape;
    std::uint32_t const count = shape.count();
    atoms.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ShapeEntry const& entry = shape.at(i);
        if (entry.is_hole())
            continue;
        switch (entry.key.kind()) {
        case PropertyKey::Kind::Index:
            indices.push_back(entry.key.as_index());
            break;
        case PropertyKey::Kind::Atom:
            atoms.push_back(entry.key);
            break;
        case PropertyKey::Kind::Symbol:
            // A Private Name keys a class's `#x`: it is not a property key
            // the language can see (§6.2.10), so no key list has it.
            if (!entry.key.as_symbol()->is_private())
                symbols.push_back(entry.key);
            break;
        }
    }
    std::sort(indices.begin(), indices.end());
    std::vector<PropertyKey> keys;
    keys.reserve(indices.size() + atoms.size() + symbols.size());
    for (std::uint32_t const index : indices)
        keys.push_back(PropertyKey::index(index));
    keys.insert(keys.end(), atoms.begin(), atoms.end());
    keys.insert(keys.end(), symbols.begin(), symbols.end());
    return keys;
}

std::vector<Property> Object::properties() const
{
    std::vector<Property> listed;
    std::uint32_t const count = m_shape->count();
    listed.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ShapeEntry const& entry = m_shape->at(i);
        if (!entry.is_hole())
            listed.push_back(view_of(entry));
    }
    return listed;
}

void Object::trace(Tracer& tracer)
{
    // A slot holds a value, an accessor's pair, or the mark of a property
    // not yet made, which is no cell: what that keeps alive is the realm
    // it will be made in, which the object's dictionary holds.
    tracer.visit(m_shape);
    for (std::uint32_t i = 0; i < m_slot_count; ++i)
        tracer.visit(m_slots[i]);
}

std::size_t Object::size_in_bytes() const
{
    return sizeof(*this) + (m_slots != m_inline_slots ? m_slot_capacity * sizeof(Value) : 0)
        + (m_owns_shape ? m_shape->size_in_bytes() : 0);
}

// ---------------------------------------------------------- LazyRecord

namespace {

struct LazyRecordHash {
    std::size_t operator()(LazyRecord const& record) const
    {
        std::size_t hash = std::hash<void const*> {}(record.get);
        hash ^= std::hash<void const*> {}(record.set) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::uintptr_t> {}(reinterpret_cast<std::uintptr_t>(record.make)) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return hash;
    }
};

}

LazyRecord const& LazyRecord::intern(LazyRecord const& record)
{
    // As NativeSpec::intern: a thread's own view first, the records one
    // set for the process, never freed and never moved.
    thread_local std::unordered_map<LazyRecord, LazyRecord const*, LazyRecordHash> seen;
    if (auto const found = seen.find(record); found != seen.end())
        return *found->second;
    static std::mutex lock;
    static auto* const records = new std::unordered_set<LazyRecord, LazyRecordHash>();
    LazyRecord const* interned = nullptr;
    {
        std::lock_guard<std::mutex> const held(lock);
        interned = &*records->insert(record).first;
    }
    seen.emplace(record, interned);
    return *interned;
}

// ----------------------------------------------------------- ArrayObject

namespace {

// Does the ordinary storage hold no index in [from, to)? What decides
// whether the dense storage may grow over the gap.
bool no_index_between(Shape const& shape, std::uint32_t from, std::uint32_t to)
{
    if (!shape.has_index_keys())
        return true;
    for (std::uint32_t i = 0; i < shape.count(); ++i) {
        ShapeEntry const& entry = shape.at(i);
        if (!entry.is_hole() && entry.key.is_index() && entry.key.as_index() >= from && entry.key.as_index() < to)
            return false;
    }
    return true;
}

}

ElementBlock::ElementBlock(std::span<Value const> values)
{
    if (values.empty())
        return;
    reserve(values.size());
    std::memcpy(static_cast<void*>(m_data), static_cast<void const*>(values.data()), values.size() * sizeof(Value));
    m_length = static_cast<std::uint32_t>(values.size());
}

ElementBlock::~ElementBlock()
{
    std::free(static_cast<void*>(m_data));
}

void ElementBlock::reserve(std::size_t capacity)
{
    if (capacity <= m_capacity)
        return;
    auto* const block = static_cast<Value*>(std::realloc(static_cast<void*>(m_data), capacity * sizeof(Value)));
    if (block == nullptr)
        std::abort();
    m_data = block;
    m_capacity = static_cast<std::uint32_t>(capacity);
}

void ElementBlock::resize(std::size_t length, Value fill)
{
    if (length > m_length) {
        if (length > m_capacity)
            reserve(std::max<std::size_t>(length, m_capacity + m_capacity / 2));
        std::fill(m_data + m_length, m_data + length, fill);
    }
    m_length = static_cast<std::uint32_t>(length);
}

ArrayObject::ArrayObject(Object* prototype, std::span<Value const> elements)
    : Object(prototype, Class::Array)
    , m_elements(elements)
    , m_length(static_cast<std::uint32_t>(elements.size()))
{
}

// The storage invariant every method below keeps: an index below
// dense_size() is answered by the element block alone (a hole is absent),
// and every index held as an ordinary property is at or past
// dense_size(). So element() is exact, and the dense storage never grows
// over an ordinary index property.

bool ArrayObject::set_length(std::uint32_t new_length)
{
    if (new_length == m_length)
        return true;
    if (!m_length_writable)
        return false;
    if (new_length > m_length) {
        m_length = new_length;
        return true;
    }
    // ArraySetLength step 16 (§10.4.2.4): elements go from the top down
    // and the first that will not delete stops the truncation with the
    // length just above it. Dense elements are always configurable, so
    // only an ordinary index property can be the one that stops it.
    std::uint32_t stop = new_length;
    bool blocked = false;
    std::vector<PropertyKey> going;
    if (m_shape->has_index_keys()) {
        Shape const& shape = *m_shape;
        for (std::uint32_t i = 0; i < shape.count(); ++i) {
            ShapeEntry const& entry = shape.at(i);
            if (entry.is_hole() || !entry.key.is_index() || (entry.attributes & Configurable) != 0)
                continue;
            std::uint32_t const index = entry.key.as_index();
            if (index >= new_length && index + 1 > stop) {
                stop = index + 1;
                blocked = true;
            }
        }
        for (std::uint32_t i = 0; i < shape.count(); ++i) {
            ShapeEntry const& entry = shape.at(i);
            if (!entry.is_hole() && entry.key.is_index() && entry.key.as_index() >= stop)
                going.push_back(entry.key);
        }
    }
    for (PropertyKey const& key : going)
        delete_entry(key);
    if (m_elements.size() > stop)
        m_elements.resize(stop);
    m_length = stop;
    return !blocked;
}

Value ArrayObject::element(std::uint32_t index) const
{
    if (index < m_elements.size())
        return m_elements[index];
    ShapeEntry const* const entry = whole_entry(PropertyKey::index(index));
    // An accessor has no value without running its getter, which this
    // path never does; has_element() still reports it present.
    if (entry == nullptr || entry->accessor)
        return Value::empty();
    return slot(entry->slot);
}

bool ArrayObject::has_element(std::uint32_t index) const
{
    if (index < m_elements.size())
        return !m_elements[index].is_empty();
    return whole_entry(PropertyKey::index(index)) != nullptr;
}

void ArrayObject::set_element(std::uint32_t index, Value const& value)
{
    // The unchecked write: the value lands where the growth policy says
    // and the length follows the index with no regard to its writability.
    // An element that has left the dense storage keeps the attributes it
    // has — this path skips the checks of OrdinarySet (§10.1.9.2), but it
    // must not widen what they guard: a frozen element written here is
    // still frozen. An accessor has no value slot and becomes a plain
    // data element, as put() would make it.
    auto const size = static_cast<std::uint32_t>(m_elements.size());
    PropertyKey const key = PropertyKey::index(index);
    if (index < size) {
        m_elements[index] = value;
    } else if (ShapeEntry const* const existing = whole_entry(key)) {
        put_data(key, value, existing->accessor ? default_attributes : existing->attributes);
    } else if (index - size < dense_growth_limit && no_index_between(*m_shape, size, index)) {
        m_elements.resize(static_cast<std::size_t>(index) + 1, Value::empty());
        m_elements[index] = value;
        if (Heap* owner = heap())
            owner->grew((static_cast<std::size_t>(index) + 1 - size) * sizeof(Value));
    } else {
        put_data(key, value, default_attributes);
    }
    if (index >= m_length)
        m_length = index + 1;
}

void ArrayObject::push(Value const& value)
{
    // 2^32 − 1 is not an array index (§10.4.2.3 step 5 throws there); the
    // library checks before calling, so a full array is left as it is.
    if (m_length == 0xFFFFFFFFu)
        return;
    set_element(m_length, value);
}

bool ArrayObject::is_simple_dense() const
{
    if (m_length > m_elements.size())
        return false;
    for (std::uint32_t i = 0; i < m_length; ++i) {
        if (m_elements[i].is_empty())
            return false;
    }
    if (has_index_key(*this))
        return false;
    for (Object const* link = prototype(); link != nullptr; link = link->prototype()) {
        if (has_indexed_properties(*link))
            return false;
    }
    return true;
}

std::optional<PropertyDescriptor> ArrayObject::get_own_property(PropertyKey const& key) const
{
    if (is_length_key(key)) {
        return PropertyDescriptor::data(Value::number(static_cast<double>(m_length)),
            m_length_writable ? static_cast<std::uint8_t>(Writable) : frozen_attributes);
    }
    if (key.is_index()) {
        std::uint32_t const index = key.as_index();
        if (index < m_elements.size() && !m_elements[index].is_empty())
            return PropertyDescriptor::data(m_elements[index], default_attributes);
    }
    return Object::get_own_property(key);
}

bool ArrayObject::define_own_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    if (is_length_key(key)) {
        // ArraySetLength (§10.4.2.4) over the virtual length property,
        // whose descriptor is {m_length, m_length_writable, false, false}.
        std::optional<PropertyDescriptor> const current = get_own_property(key);
        if (!desc.value) {
            if (!is_compatible(is_extensible(), desc, current))
                return false;
            if (desc.writable && !*desc.writable)
                m_length_writable = false;
            return true;
        }
        // Steps 3–5 are the caller's: ToUint32 and the RangeError when the
        // value is not a valid length. What arrives must already be one;
        // anything else is refused rather than silently truncated.
        std::optional<std::uint32_t> const new_length = validated_uint32(*desc.value);
        if (!new_length)
            return false;
        PropertyDescriptor new_desc = desc;
        new_desc.value = Value::number(static_cast<double>(*new_length));
        if (*new_length >= m_length) {
            if (!is_compatible(is_extensible(), new_desc, current))
                return false;
            m_length = *new_length;
            if (new_desc.writable && !*new_desc.writable)
                m_length_writable = false;
            return true;
        }
        if (!m_length_writable)
            return false;
        // Step 13: writable false is deferred until the elements are gone,
        // and lands even when a truncation stops early (step 16.d).
        bool const new_writable = new_desc.writable.value_or(true);
        new_desc.writable = true;
        if (!is_compatible(is_extensible(), new_desc, current))
            return false;
        bool const truncated = set_length(*new_length);
        if (!new_writable)
            m_length_writable = false;
        return truncated;
    }
    if (!key.is_index())
        return Object::define_own_property(key, desc);

    // §10.4.2.1 step 2: an index at or past the length needs a writable
    // length, and defining it moves the length past it.
    std::uint32_t const index = key.as_index();
    if (index >= m_length && !m_length_writable)
        return false;
    std::optional<PropertyDescriptor> const current = get_own_property(key);
    if (!is_compatible(is_extensible(), desc, current))
        return false;
    auto const size = static_cast<std::uint32_t>(m_elements.size());
    bool const dense_now = index < size && !m_elements[index].is_empty();
    if (dense_now) {
        // A dense element carries the default attributes; if the result
        // still does, it stays. Otherwise it and everything above it move
        // to ordinary storage, which keeps every ordinary index at or past
        // the dense size.
        Property scratch;
        scratch.key = key;
        scratch.value = m_elements[index];
        apply_to(scratch, desc);
        if (is_dense_eligible(scratch)) {
            m_elements[index] = scratch.value;
        } else {
            for (std::uint32_t i = index; i < size; ++i) {
                if (!m_elements[i].is_empty())
                    put_data(PropertyKey::index(i), m_elements[i], default_attributes);
            }
            m_elements.resize(index);
            apply_descriptor(*whole_entry(key), desc);
        }
    } else if (current) {
        apply_descriptor(*whole_entry(key), desc);
    } else {
        Property const fresh = property_from(key, desc);
        bool const near = index < size || index - size < dense_growth_limit;
        bool const clear_run = no_index_between(*m_shape, size, index);
        if (is_dense_eligible(fresh) && near && clear_run) {
            if (index >= size) {
                m_elements.resize(static_cast<std::size_t>(index) + 1, Value::empty());
                if (Heap* owner = heap())
                    owner->grew((static_cast<std::size_t>(index) + 1 - size) * sizeof(Value));
            }
            m_elements[index] = fresh.value;
        } else {
            // An ordinary index inside the dense range would be shadowed
            // by the hole there, so the dense storage ends below it.
            if (index < size) {
                for (std::uint32_t i = index; i < size; ++i) {
                    if (!m_elements[i].is_empty())
                        put_data(PropertyKey::index(i), m_elements[i], default_attributes);
                }
                m_elements.resize(index);
            }
            create_property(key, desc);
        }
    }
    if (index >= m_length)
        m_length = index + 1;
    return true;
}

bool ArrayObject::has_property(PropertyKey const& key) const
{
    if (key.is_index()) {
        std::uint32_t const index = key.as_index();
        if (index < m_elements.size() && !m_elements[index].is_empty())
            return true;
    } else if (is_length_key(key)) {
        return true;
    }
    return Object::has_property(key);
}

std::optional<Value> ArrayObject::get(Interpreter& interpreter, PropertyKey const& key, Value const& receiver)
{
    if (key.is_index()) {
        std::uint32_t const index = key.as_index();
        if (index < m_elements.size() && !m_elements[index].is_empty())
            return m_elements[index];
    }
    return Object::get(interpreter, key, receiver);
}

std::optional<bool> ArrayObject::set(Interpreter& interpreter, PropertyKey const& key, Value const& value, Value const& receiver)
{
    if (key.is_index()) {
        std::uint32_t const index = key.as_index();
        if (index < m_elements.size() && !m_elements[index].is_empty() && is_receiver(receiver, this)) {
            m_elements[index] = value;
            return true;
        }
    } else if (is_length_key(key) && is_receiver(receiver, this)) {
        // The assignment `array.length = v`. OrdinarySet finds the length
        // writable (or refuses before touching v) and hands {value: v} to
        // [[DefineOwnProperty]], whose ArraySetLength coerces it — twice,
        // ToUint32 then ToNumber, both observable — and throws a
        // RangeError when they disagree (§10.4.2.4 steps 3–5). That throw
        // can only be raised here, where the interpreter is at hand.
        if (!m_length_writable)
            return false;
        std::optional<std::uint32_t> const new_length = interpreter.to_uint32(value);
        if (!new_length)
            return std::nullopt;
        std::optional<double> const number_length = interpreter.to_number(value);
        if (!number_length)
            return std::nullopt;
        if (static_cast<double>(*new_length) != *number_length)
            return interpreter.throw_range_error("Invalid array length");
        PropertyDescriptor length_desc;
        length_desc.value = Value::number(static_cast<double>(*new_length));
        return define_own_property(key, length_desc);
    }
    return Object::set(interpreter, key, value, receiver);
}

bool ArrayObject::delete_property(PropertyKey const& key)
{
    if (is_length_key(key))
        return false;
    if (key.is_index()) {
        std::uint32_t const index = key.as_index();
        if (index < m_elements.size()) {
            m_elements[index] = Value::empty();
            return true;
        }
    }
    return Object::delete_property(key);
}

std::vector<PropertyKey> ArrayObject::own_keys() const
{
    std::vector<std::uint32_t> indices;
    for (std::uint32_t i = 0; i < m_elements.size(); ++i) {
        if (!m_elements[i].is_empty())
            indices.push_back(i);
    }
    if (m_shape->has_index_keys()) {
        for (std::uint32_t i = 0; i < m_shape->count(); ++i) {
            ShapeEntry const& entry = m_shape->at(i);
            if (!entry.is_hole() && entry.key.is_index())
                indices.push_back(entry.key.as_index());
        }
    }
    std::sort(indices.begin(), indices.end());
    std::vector<PropertyKey> keys;
    keys.reserve(indices.size() + m_shape->count() + 1);
    for (std::uint32_t const index : indices)
        keys.push_back(PropertyKey::index(index));
    // `length` is the first string key: ArrayCreate defines it before
    // anything else can be added (§10.4.2.2).
    if (JsString* atom = length_atom_for(*this))
        keys.push_back(PropertyKey::atom(atom));
    for (PropertyKey const& key : Object::own_keys()) {
        if (!key.is_index())
            keys.push_back(key);
    }
    return keys;
}

void ArrayObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    for (Value const& element : m_elements)
        tracer.visit(element);
}

// -------------------------------------------------------------- Function

std::optional<Value> Function::construct(Interpreter& interpreter, std::span<Value const>, Object*)
{
    return interpreter.throw_type_error("not a constructor");
}

void Function::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_realm);
}

ScriptFunction::ScriptFunction(Object* prototype, FunctionNode const& node, Environment* scope, bool constructable)
    : Function(prototype)
    , m_node(&node)
    , m_scope(scope)
    , m_constructable(constructable)
{
    mark_script();
}

bool ScriptFunction::is_arrow() const
{
    return m_node->is_arrow;
}

bool ScriptFunction::is_strict() const
{
    return m_node->is_strict;
}

void ScriptFunction::defer_own_properties(bool with_prototype)
{
    bool const has_prototype = with_prototype && (m_node->is_constructable || m_node->is_generator);
    set_pending(static_cast<std::uint8_t>(PendingLength | PendingName | (has_prototype ? PendingPrototype : 0)));
    ++heap()->lazy_census().script_functions;
    if (!lazy_natives())
        settle_pending();
}

std::pair<Value, std::uint8_t> ScriptFunction::make_pending(std::uint8_t which)
{
    Heap& owner = *heap();
    if (which != PendingPrototype)
        ++owner.lazy_census().names_or_lengths_asked;
    else
        ++owner.lazy_census().prototypes_asked;
    if (which == PendingLength)
        return { Value::number(static_cast<double>(m_node->expected_argument_count)), Configurable };
    if (which == PendingName)
        return { Value::string(m_node->name ? m_node->name : owner.atoms().empty), Configurable };
    // MakeConstructor: a fresh object pointing back. A generator's is
    // writable, not enumerable, not configurable and has no `constructor`
    // (§15.5.4, §15.6.4), and inherits from its kind's %…Prototype%.
    Intrinsics const& intrinsics = realm()->intrinsics;
    if (m_node->is_constructable) {
        auto* const prototype = owner.allocate<Object>(intrinsics.object_prototype);
        prototype->reserve_properties(1);
        prototype->put(PropertyKey::atom(owner.atoms().constructor), Value::object(this), builtin_attributes);
        return { Value::object(prototype), Writable };
    }
    Object* const instance_prototype = m_node->is_async ? intrinsics.async_generator_prototype : intrinsics.generator_prototype;
    return { Value::object(owner.allocate<Object>(instance_prototype != nullptr ? instance_prototype : intrinsics.object_prototype)), Writable };
}

void ScriptFunction::trace(Tracer& tracer)
{
    // The node is the program's, which the realm keeps for as long as it
    // lives; the closed-over scope, the home object and the fields' keys
    // and initializers are cells.
    Function::trace(tracer);
    tracer.visit(m_scope);
    tracer.visit(m_home_object);
    tracer.visit(m_private_environment);
    for (ClassField const& field : m_fields) {
        tracer.visit(field.key);
        tracer.visit(field.initializer);
    }
    for (PrivateMethod const& method : m_private_methods) {
        tracer.visit(method.name);
        tracer.visit(method.method);
        tracer.visit(method.getter);
        tracer.visit(method.setter);
    }
}

namespace {

// A native's call under the profile: its time is its own (net of the
// script it calls back). Out of the way of the call that is not profiled,
// which pays one test for it.
template<typename Call>
[[gnu::noinline]] std::optional<Value> profiled(Interpreter& interpreter, Object const& function, Call const& call)
{
    Interpreter::ActivityScope const activity(interpreter, interpreter.native_activity(function));
    return call();
}

}

// ------------------------------------------------------- NativeFunction

// What a native made of closures carries, apart from the cell: nearly
// every native is a described one and has none of this.
struct NativeFunction::Closures {
    Callback call;
    ConstructCallback construct;
    JsString* name = nullptr;
    std::int32_t length = 0;
};

NativeFunction::NativeFunction(Object* prototype, Callback call, ConstructCallback construct)
    : Function(prototype)
    , m_closures(new Closures { std::move(call), std::move(construct), nullptr, 0 })
    , m_closures_made(true)
{
}

NativeFunction::NativeFunction(Object* prototype, NativeSpec const& spec, PropertyKey key, Role role, Object* home)
    : Function(prototype)
    , m_spec(&spec)
    , m_key(key)
    , m_home(home)
    , m_role(role)
    , m_in_receivers_realm((spec.flags & NativeSpec::ReceiversRealm) != 0)
{
    set_pending(PendingLength | PendingName);
}

NativeFunction::~NativeFunction()
{
    if (m_closures_made)
        delete m_closures;
}

bool NativeFunction::is_constructor() const
{
    return m_closures_made ? static_cast<bool>(m_closures->construct) : m_spec->construct != nullptr;
}

NativeFunction::Callback const* NativeFunction::closure() const
{
    return m_closures_made ? &m_closures->call : nullptr;
}

void NativeFunction::set_name_and_length(JsString* name, int length)
{
    if (!m_closures_made)
        return;
    m_closures->name = name;
    m_closures->length = length;
    set_pending(PendingLength | PendingName);
    if (!lazy_natives())
        settle_pending();
}

// CreateBuiltinFunction's `length` and `name` (§10.3.3), read-only and
// hidden from enumeration. A described native is named by the key it was
// defined as: the key itself, or the key after "get " or "set " (§10.2.9).
std::pair<Value, std::uint8_t> NativeFunction::make_pending(std::uint8_t which)
{
    Heap& owner = *heap();
    if (which == PendingLength)
        return { Value::number(static_cast<double>(m_closures_made ? m_closures->length : m_spec->length)), Configurable };
    if (m_closures_made)
        return { Value::string(m_closures->name != nullptr ? m_closures->name : owner.atoms().empty), Configurable };
    JsString* name = owner.atoms().empty;
    if (m_key.is_symbol()) {
        JsString const* const description = m_key.as_symbol()->description();
        name = owner.atom(description != nullptr ? u"[" + std::u16string(description->view()) + u"]" : std::u16string());
    } else if (m_key.is_atom() || m_key.is_index()) {
        name = owner.key_to_string(m_key);
    }
    if (m_role != Role::Method)
        name = owner.atom((m_role == Role::Getter ? u"get " : u"set ") + std::u16string(name->view()));
    return { Value::string(name), Configurable };
}

void NativeFunction::trace(Tracer& tracer)
{
    Function::trace(tracer);
    tracer.visit(m_key);
    tracer.visit(m_home);
    if (m_closures_made)
        tracer.visit(m_closures->name);
}

std::size_t NativeFunction::size_in_bytes() const
{
    return sizeof(*this) + (Object::size_in_bytes() - sizeof(Object)) + (m_closures_made ? sizeof(Closures) : 0);
}

std::optional<Value> NativeFunction::perform(Interpreter& interpreter, Value const& this_value, std::span<Value const> arguments)
{
    if (m_closures_made)
        return m_closures->call(interpreter, this_value, arguments);
    // The entry may serve several properties: it reads which one it was
    // called as off the function, which the interpreter holds meanwhile.
    Interpreter::ActiveNative const active(interpreter, this);
    if (m_spec->guard != nullptr)
        return m_spec->guard(interpreter, *this, this_value, arguments);
    return m_spec->call(interpreter, this_value, arguments);
}

std::optional<Value> NativeFunction::call(Interpreter& interpreter, Value const& this_value, std::span<Value const> arguments)
{
    if (m_closures_made ? !m_closures->call : m_spec->call == nullptr)
        return interpreter.throw_type_error("not a function");
    // A built-in's [[Call]] (§10.3.1): its realm is current while it runs —
    // or, for a host's native called on a host's object, that object's.
    RealmRecord* running_in = realm();
    if (m_in_receivers_realm && this_value.is_object()) {
        if (RealmRecord* const home = this_value.as_object()->home_realm())
            running_in = home;
    }
    Interpreter::RealmScope const realm_scope(interpreter, running_in);
    if (interpreter.vm_profiling()) [[unlikely]]
        return profiled(interpreter, *this, [&] { return perform(interpreter, this_value, arguments); });
    return perform(interpreter, this_value, arguments);
}

std::optional<Value> NativeFunction::construct(Interpreter& interpreter, std::span<Value const> arguments, Object* new_target)
{
    if (!is_constructor())
        return interpreter.throw_type_error("not a constructor");
    Interpreter::RealmScope const realm_scope(interpreter, realm());
    auto const run = [&]() -> std::optional<Value> {
        if (m_closures_made)
            return m_closures->construct(interpreter, arguments, new_target);
        Interpreter::ActiveNative const active(interpreter, this);
        return m_spec->construct(interpreter, arguments, new_target);
    };
    if (interpreter.vm_profiling()) [[unlikely]]
        return profiled(interpreter, *this, run);
    return run();
}

namespace {

struct NativeSpecHash {
    std::size_t operator()(NativeSpec const& spec) const
    {
        auto const mix = [](std::size_t seed, std::size_t value) { return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)); };
        std::size_t hash = reinterpret_cast<std::uintptr_t>(spec.call);
        hash = mix(hash, reinterpret_cast<std::uintptr_t>(spec.construct));
        hash = mix(hash, reinterpret_cast<std::uintptr_t>(spec.guard));
        hash = mix(hash, (static_cast<std::size_t>(spec.datum) << 24) | (static_cast<std::size_t>(spec.flags) << 8) | spec.length);
        return hash;
    }
};

std::atomic<int> lazy_mode { -1 };

}

NativeSpec const& NativeSpec::intern(NativeSpec const& spec)
{
    // A thread's own view first: a description it has met costs no lock.
    // The records themselves are one set for the process, never freed (a
    // property of any engine on any thread may hold one), and a set's
    // elements do not move.
    thread_local std::unordered_map<NativeSpec, NativeSpec const*, NativeSpecHash> seen;
    if (auto const found = seen.find(spec); found != seen.end())
        return *found->second;
    static std::mutex lock;
    static auto* const records = new std::unordered_set<NativeSpec, NativeSpecHash>();
    NativeSpec const* record = nullptr;
    {
        std::lock_guard<std::mutex> const held(lock);
        record = &*records->insert(spec).first;
    }
    seen.emplace(spec, record);
    return *record;
}

bool lazy_natives()
{
    int mode = lazy_mode.load(std::memory_order_relaxed);
    if (mode < 0) {
        char const* const asked = std::getenv("SASHFOLD_LAZY");
        mode = asked != nullptr && asked[0] == '0' ? 0 : 1;
        lazy_mode.store(mode, std::memory_order_relaxed);
    }
    return mode == 1;
}

void set_lazy_natives(bool lazy)
{
    lazy_mode.store(lazy ? 1 : 0, std::memory_order_relaxed);
}

// ------------------------------------------------------ ClosureFunction

void ClosureFunction::set_name_and_length(JsString* name, int length)
{
    m_name = name;
    m_length = length;
    set_pending(PendingLength | PendingName);
    if (!lazy_natives())
        settle_pending();
}

std::pair<Value, std::uint8_t> ClosureFunction::make_pending(std::uint8_t which)
{
    if (which == PendingLength)
        return { Value::number(static_cast<double>(m_length)), Configurable };
    return { Value::string(m_name != nullptr ? m_name : heap()->atoms().empty), Configurable };
}

std::optional<Value> ClosureFunction::call(Interpreter& interpreter, Value const& this_value, std::span<Value const> arguments)
{
    Interpreter::RealmScope const realm_scope(interpreter, realm());
    if (interpreter.vm_profiling()) [[unlikely]]
        return profiled(interpreter, *this, [&] { return m_callback(interpreter, *this, this_value, arguments); });
    return m_callback(interpreter, *this, this_value, arguments);
}

void ClosureFunction::trace(Tracer& tracer)
{
    Function::trace(tracer);
    tracer.visit(m_name);
    for (Value const& slot : m_slots)
        tracer.visit(slot);
}

void PromiseObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_result);
    for (std::vector<PromiseReaction> const* reactions : { &m_fulfill_reactions, &m_reject_reactions }) {
        for (PromiseReaction const& reaction : *reactions) {
            tracer.visit(reaction.handler);
            tracer.visit(reaction.capability_promise);
            tracer.visit(reaction.capability_resolve);
            tracer.visit(reaction.capability_reject);
        }
    }
}

std::optional<Value> BoundFunction::call(Interpreter& interpreter, Value const&, std::span<Value const> arguments)
{
    // [[Call]] of a bound function (§10.4.1.1): the bound this replaces
    // the caller's, and the bound arguments go before the caller's. The
    // copies need no rooting: the bound ones live in this function, which
    // is the callee and so alive, and the rest are the caller's.
    std::vector<Value> combined;
    combined.reserve(m_bound_arguments.size() + arguments.size());
    combined.insert(combined.end(), m_bound_arguments.begin(), m_bound_arguments.end());
    combined.insert(combined.end(), arguments.begin(), arguments.end());
    return interpreter.call(Value::object(m_target), m_bound_this, combined);
}

std::optional<Value> BoundFunction::construct(Interpreter& interpreter, std::span<Value const> arguments, Object* new_target)
{
    // [[Construct]] (§10.4.1.2): the target is constructed with the same
    // argument list, and when `new` was applied to this bound function the
    // target takes over as new.target.
    if (!m_target->is_constructor())
        return interpreter.throw_type_error("not a constructor");
    std::vector<Value> combined;
    combined.reserve(m_bound_arguments.size() + arguments.size());
    combined.insert(combined.end(), m_bound_arguments.begin(), m_bound_arguments.end());
    combined.insert(combined.end(), arguments.begin(), arguments.end());
    Object* target_new_target = new_target == this ? m_target : new_target;
    return m_target->construct(interpreter, combined, target_new_target);
}

void BoundFunction::trace(Tracer& tracer)
{
    Function::trace(tracer);
    tracer.visit(m_target);
    tracer.visit(m_bound_this);
    for (Value const& argument : m_bound_arguments)
        tracer.visit(argument);
}

// ------------------------------------------------------- PrimitiveObject

void PrimitiveObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_primitive);
}

// ---------------------------------------------------------- StringObject

std::optional<PropertyDescriptor> StringObject::get_own_property(PropertyKey const& key) const
{
    // StringGetOwnProperty (§10.4.3.5) for the code units, and the
    // `length` StringCreate defines (§10.4.3.4); both read-only and
    // non-configurable, the units enumerable and length not.
    JsString const* text = string();
    if (is_length_key(key))
        return PropertyDescriptor::data(Value::number(static_cast<double>(text->length())), frozen_attributes);
    if (key.is_index()) {
        std::uint32_t const index = key.as_index();
        if (index < text->length())
            return PropertyDescriptor::data(Value::string(code_unit_string(text->data()[index])), Enumerable);
    }
    return Object::get_own_property(key);
}

bool StringObject::define_own_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    // §10.4.3.2: a unit or the length accepts only a descriptor it already
    // satisfies, and accepting changes nothing.
    if (is_length_key(key) || (key.is_index() && key.as_index() < string()->length()))
        return is_compatible(is_extensible(), desc, get_own_property(key));
    return Object::define_own_property(key, desc);
}

bool StringObject::has_property(PropertyKey const& key) const
{
    if (is_length_key(key) || (key.is_index() && key.as_index() < string()->length()))
        return true;
    return Object::has_property(key);
}

bool StringObject::delete_property(PropertyKey const& key)
{
    if (is_length_key(key) || (key.is_index() && key.as_index() < string()->length()))
        return false;
    return Object::delete_property(key);
}

std::vector<PropertyKey> StringObject::own_keys() const
{
    // StringOwnPropertyKeys (§10.4.3.3): the units, then the ordinary
    // indices ascending, then `length` — the first string key, defined by
    // StringCreate before any other — then the rest in creation order.
    std::vector<PropertyKey> const base = Object::own_keys();
    std::vector<PropertyKey> keys;
    keys.reserve(string()->length() + base.size() + 1);
    for (std::uint32_t i = 0; i < string()->length(); ++i)
        keys.push_back(PropertyKey::index(i));
    auto const first_named = std::find_if(base.begin(), base.end(), [](PropertyKey const& key) { return !key.is_index(); });
    keys.insert(keys.end(), base.begin(), first_named);
    if (JsString* atom = length_atom_for(*this))
        keys.push_back(PropertyKey::atom(atom));
    keys.insert(keys.end(), first_named, base.end());
    return keys;
}

// ---------------------------------------------------------- RegExpObject

void RegExpObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_source);
    tracer.visit(m_flags);
    tracer.visit(m_realm);
}

// ----------------------------------------------------------- Environment

std::size_t Environment::place_of(JsString const* name) const
{
    std::size_t const count = m_bindings.size();
    if (count < indexed_from) {
        for (std::size_t i = 0; i < count; ++i) {
            if (m_bindings[i].name == name)
                return i;
        }
        return count;
    }
    if (m_indexed != count) {
        m_index.clear();
        m_index.reserve(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            if (m_bindings[i].name)
                m_index.emplace(m_bindings[i].name, static_cast<std::uint32_t>(i));
        }
        m_indexed = static_cast<std::uint32_t>(count);
    }
    auto const found = m_index.find(name);
    return found == m_index.end() ? count : found->second;
}

void Environment::assign_bindings(std::span<Binding const> bindings)
{
    m_bindings.assign(bindings.begin(), bindings.end());
    m_index.clear();
    m_indexed = 0;
    if (Heap* owner = heap())
        owner->grew(bindings.size() * sizeof(Binding));
}

Environment::Binding* Environment::find(JsString* name)
{
    std::size_t const place = place_of(name);
    return place < m_bindings.size() ? &m_bindings[place] : nullptr;
}

Environment::Binding const* Environment::find(JsString* name) const
{
    std::size_t const place = place_of(name);
    return place < m_bindings.size() ? &m_bindings[place] : nullptr;
}

Environment::Binding& Environment::declare(JsString* name, Value initial, bool mutable_, bool initialized, bool deletable)
{
    if (Binding* existing = find(name))
        return *existing;
    // An index that is up to date stays so; one that is not is made again
    // by the next search.
    bool const indexed = m_indexed == m_bindings.size() && m_indexed != 0;
    if (indexed) {
        m_index.emplace(name, static_cast<std::uint32_t>(m_bindings.size()));
        ++m_indexed;
    }
    Binding& binding = m_bindings.emplace_back();
    if (Heap* owner = heap())
        owner->grew(sizeof(Binding) + (indexed ? sizeof(void*) * 4 : 0));
    binding.name = name;
    binding.value = initial;
    binding.mutable_ = mutable_;
    binding.initialized = initialized;
    binding.deletable = deletable;
    return binding;
}

Environment::Binding& Environment::declare_import(JsString* name, ModuleRecord* module, JsString* import_name)
{
    Binding& binding = declare(name, Value::undefined(), false, true);
    binding.import_module = module;
    binding.import_name = import_name;
    return binding;
}

bool Environment::remove(JsString* name)
{
    for (auto it = m_bindings.begin(); it != m_bindings.end(); ++it) {
        if (it->name != name)
            continue;
        if (!it->deletable)
            return false;
        m_bindings.erase(it);
        // Every place after it has moved: the index is made again when next
        // wanted. Emptied rather than left, since a declaration that follows
        // would bring the counts level again with the places still wrong.
        m_index.clear();
        m_indexed = 0;
        return true;
    }
    return false;
}

void Environment::trace(Tracer& tracer)
{
    for (Binding const& binding : m_bindings) {
        tracer.visit(binding.name);
        tracer.visit(binding.value);
        tracer.visit(binding.import_module);
        tracer.visit(binding.import_name);
    }
    tracer.visit(m_outer);
    tracer.visit(m_object);
    tracer.visit(m_this);
    tracer.visit(m_function);
    tracer.visit(m_new_target);
}

}
