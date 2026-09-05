#include "js/Object.h"

#include "js/Ast.h"
#include "js/Interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {

// A linear scan wins on small objects; past this many properties the
// hash index is built and kept in step with every insert and erase.
constexpr std::size_t index_threshold = 8;

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

PropertyDescriptor descriptor_of(Property const& property)
{
    if (property.accessor)
        return PropertyDescriptor::accessor(property.getter, property.setter, property.attributes);
    return PropertyDescriptor::data(property.value, property.attributes);
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
Property make_property(PropertyKey const& key, PropertyDescriptor const& desc)
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
void apply_descriptor(Property& property, PropertyDescriptor const& desc)
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
    for (Property const& property : object.properties()) {
        if (property.key.is_index())
            return true;
    }
    return false;
}

// Does this object, on its own, answer any array index? What the dense
// fast paths must not find anywhere on an array's prototype chain. The
// class ids stand for the exotic classes the way is_array() already does.
bool has_indexed_properties(Object const& object)
{
    if (object.is_host())
        return true; // the bindings may answer indices the storage does not show
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

// ---------------------------------------------------------------- Object

Property const* Object::find_own(PropertyKey const& key) const
{
    if (m_properties.size() > index_threshold) {
        auto const found = m_index.find(key);
        return found == m_index.end() ? nullptr : &m_properties[found->second];
    }
    for (Property const& property : m_properties) {
        if (property.key == key)
            return &property;
    }
    return nullptr;
}

Property* Object::find_own(PropertyKey const& key)
{
    return const_cast<Property*>(static_cast<Object const*>(this)->find_own(key));
}

Property& Object::insert(PropertyKey const& key)
{
    Property& property = m_properties.emplace_back();
    property.key = key;
    std::size_t const count = m_properties.size();
    if (count > index_threshold) {
        if (count == index_threshold + 1)
            rebuild_index();
        else
            m_index.emplace(key, count - 1);
    }
    return property;
}

void Object::erase_at(std::size_t index)
{
    PropertyKey const key = m_properties[index].key;
    m_properties.erase(m_properties.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_properties.size() <= index_threshold) {
        m_index.clear();
        return;
    }
    // Everything after the gap moved down one slot.
    m_index.erase(key);
    for (auto& entry : m_index) {
        if (entry.second > index)
            --entry.second;
    }
}

void Object::rebuild_index()
{
    m_index.clear();
    m_index.reserve(m_properties.size());
    for (std::size_t i = 0; i < m_properties.size(); ++i)
        m_index.emplace(m_properties[i].key, i);
}

void Object::put(PropertyKey const& key, Value const& value, std::uint8_t attributes)
{
    if (m_class == Class::Array && key.is_index()) {
        // put() is not virtual, and an array answers an index from its
        // dense vector before its ordinary storage (every index goes
        // through the array's own [[DefineOwnProperty]], §10.4.2.1): an
        // ordinary property at an index the vector covers would shadow
        // the element, or be shadowed by it. So the value goes where
        // set_element puts it, and attributes the vector has no room for
        // move the element — and everything above it, so that every
        // ordinary index stays at or past the dense size — out of it.
        auto& array = static_cast<ArrayObject&>(*this);
        std::uint32_t const index = key.as_index();
        array.set_element(index, value);
        std::vector<Value>& dense = array.dense();
        if (attributes != default_attributes && index < dense.size()) {
            for (std::uint32_t i = index; i < dense.size(); ++i) {
                if (!dense[i].is_empty())
                    insert(PropertyKey::index(i)).value = dense[i];
            }
            dense.resize(index);
        }
        if (Property* property = find_own(key)) {
            property->getter = nullptr;
            property->setter = nullptr;
            property->attributes = attributes;
            property->accessor = false;
        }
        return;
    }
    Property* property = find_own(key);
    if (property == nullptr)
        property = &insert(key);
    property->value = value;
    property->getter = nullptr;
    property->setter = nullptr;
    property->attributes = attributes;
    property->accessor = false;
}

void Object::put_accessor(PropertyKey const& key, Object* getter, Object* setter, std::uint8_t attributes)
{
    Property* property = find_own(key);
    if (property == nullptr)
        property = &insert(key);
    property->value = Value::undefined();
    property->getter = getter;
    property->setter = setter;
    property->attributes = static_cast<std::uint8_t>(attributes & ~Writable);
    property->accessor = true;
}

bool Object::remove_own(PropertyKey const& key)
{
    if (m_class == Class::Array && key.is_index()) {
        // The dense side of the same routing as put(): an element the
        // vector holds becomes a hole.
        std::vector<Value>& dense = static_cast<ArrayObject&>(*this).dense();
        std::uint32_t const index = key.as_index();
        if (index < dense.size()) {
            bool const present = !dense[index].is_empty();
            dense[index] = Value::empty();
            return present;
        }
    }
    Property const* property = find_own(key);
    if (property == nullptr)
        return false;
    erase_at(static_cast<std::size_t>(property - m_properties.data()));
    return true;
}

bool Object::set_prototype(Object* proto)
{
    // OrdinarySetPrototypeOf (§10.1.2.1): the same prototype is always
    // fine, even on a non-extensible object; otherwise the object must be
    // extensible and the new chain must not run back into this object.
    if (proto == m_prototype)
        return true;
    if (!m_extensible)
        return false;
    for (Object const* link = proto; link != nullptr; link = link->prototype()) {
        if (link == this)
            return false;
    }
    m_prototype = proto;
    return true;
}

std::optional<PropertyDescriptor> Object::get_own_property(PropertyKey const& key) const
{
    Property const* property = find_own(key);
    if (property == nullptr)
        return std::nullopt;
    return descriptor_of(*property);
}

bool Object::define_own_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    // OrdinaryDefineOwnProperty (§10.1.6.1) over the ordinary storage.
    // The exotic subclasses handle their own keys before coming here, so
    // the current descriptor is read from the storage, not virtually.
    Property* existing = find_own(key);
    std::optional<PropertyDescriptor> current;
    if (existing != nullptr)
        current = descriptor_of(*existing);
    if (!is_compatible(m_extensible, desc, current))
        return false;
    if (existing != nullptr)
        apply_descriptor(*existing, desc);
    else
        insert(key) = make_property(key, desc);
    return true;
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
    return false;
}

std::optional<Value> Object::get(Interpreter& interpreter, PropertyKey const& key, Value const& receiver)
{
    // OrdinaryGet (§10.1.8.1), the recursion over the chain unrolled.
    for (Object* link = this; link != nullptr; link = link->prototype()) {
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
    return Value::undefined();
}

std::optional<bool> Object::set(Interpreter& interpreter, PropertyKey const& key, Value const& value, Value const& receiver)
{
    // OrdinarySet (§10.1.9.1) with OrdinarySetWithOwnDescriptor
    // (§10.1.9.2) unrolled: the first own descriptor up the chain decides,
    // and a chain with none behaves as a writable data property would.
    std::optional<PropertyDescriptor> own;
    for (Object* link = this; link != nullptr; link = link->prototype()) {
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
    // or a read-only property on the receiver refuses.
    Object* target = receiver.as_object();
    std::optional<PropertyDescriptor> const existing = target->get_own_property(key);
    if (existing) {
        if (existing->is_accessor())
            return false;
        if (!existing->writable.value_or(false))
            return false;
        PropertyDescriptor value_only;
        value_only.value = value;
        return target->define_own_property(key, value_only);
    }
    return target->define_own_property(key, PropertyDescriptor::data(value, default_attributes));
}

bool Object::delete_property(PropertyKey const& key)
{
    // OrdinaryDelete (§10.1.10.1): a missing property deletes fine.
    Property const* property = find_own(key);
    if (property == nullptr)
        return true;
    if (!property->configurable())
        return false;
    erase_at(static_cast<std::size_t>(property - m_properties.data()));
    return true;
}

std::vector<PropertyKey> Object::own_keys() const
{
    // OrdinaryOwnPropertyKeys (§10.1.11.1).
    std::vector<std::uint32_t> indices;
    std::vector<PropertyKey> atoms;
    std::vector<PropertyKey> symbols;
    for (Property const& property : m_properties) {
        switch (property.key.kind()) {
        case PropertyKey::Kind::Index:
            indices.push_back(property.key.as_index());
            break;
        case PropertyKey::Kind::Atom:
            atoms.push_back(property.key);
            break;
        case PropertyKey::Kind::Symbol:
            // A Private Name keys a class's `#x`: it is not a property key
            // the language can see (§6.2.10), so no key list has it.
            if (!property.key.as_symbol()->is_private())
                symbols.push_back(property.key);
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

void Object::trace(Tracer& tracer)
{
    tracer.visit(m_prototype);
    for (Property const& property : m_properties) {
        tracer.visit(property.key);
        tracer.visit(property.value);
        tracer.visit(property.getter);
        tracer.visit(property.setter);
    }
}

// ----------------------------------------------------------- ArrayObject

ArrayObject::ArrayObject(Object* prototype, std::span<Value const> elements)
    : Object(prototype, Class::Array)
    , m_elements(elements.begin(), elements.end())
    , m_length(static_cast<std::uint32_t>(elements.size()))
{
}

// The storage invariant every method below keeps: an index below
// dense_size() is answered by the vector alone (a hole is absent), and
// every index held as an ordinary property is at or past dense_size().
// So element() is exact, and the dense storage never grows over an
// ordinary index property.

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
    for (Property const& property : m_properties) {
        if (!property.key.is_index() || property.configurable())
            continue;
        std::uint32_t const index = property.key.as_index();
        if (index >= new_length && index + 1 > stop) {
            stop = index + 1;
            blocked = true;
        }
    }
    std::erase_if(m_properties, [stop](Property const& property) {
        return property.key.is_index() && property.key.as_index() >= stop;
    });
    if (m_properties.size() > index_threshold)
        rebuild_index();
    else
        m_index.clear();
    if (m_elements.size() > stop)
        m_elements.resize(stop);
    m_length = stop;
    return !blocked;
}

Value ArrayObject::element(std::uint32_t index) const
{
    if (index < m_elements.size())
        return m_elements[index];
    Property const* property = find_own(PropertyKey::index(index));
    // An accessor has no value without running its getter, which this
    // path never does; has_element() still reports it present.
    if (property == nullptr || property->accessor)
        return Value::empty();
    return property->value;
}

bool ArrayObject::has_element(std::uint32_t index) const
{
    if (index < m_elements.size())
        return !m_elements[index].is_empty();
    return find_own(PropertyKey::index(index)) != nullptr;
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
    if (index < size) {
        m_elements[index] = value;
    } else if (Property* existing = find_own(PropertyKey::index(index))) {
        if (existing->accessor) {
            existing->accessor = false;
            existing->getter = nullptr;
            existing->setter = nullptr;
            existing->attributes = default_attributes;
        }
        existing->value = value;
    } else {
        bool clear_run = true;
        for (Property const& property : m_properties) {
            if (property.key.is_index() && property.key.as_index() >= size && property.key.as_index() < index)
                clear_run = false;
        }
        if (index - size < dense_growth_limit && clear_run) {
            m_elements.resize(static_cast<std::size_t>(index) + 1, Value::empty());
            m_elements[index] = value;
        } else {
            insert(PropertyKey::index(index)).value = value;
        }
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
            if (!is_compatible(m_extensible, desc, current))
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
            if (!is_compatible(m_extensible, new_desc, current))
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
        if (!is_compatible(m_extensible, new_desc, current))
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
    if (!is_compatible(m_extensible, desc, current))
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
        apply_descriptor(scratch, desc);
        if (is_dense_eligible(scratch)) {
            m_elements[index] = scratch.value;
        } else {
            for (std::uint32_t i = index; i < size; ++i) {
                if (!m_elements[i].is_empty())
                    insert(PropertyKey::index(i)).value = m_elements[i];
            }
            m_elements.resize(index);
            apply_descriptor(*find_own(key), desc);
        }
    } else if (current) {
        apply_descriptor(*find_own(key), desc);
    } else {
        Property const fresh = make_property(key, desc);
        bool const near = index < size || index - size < dense_growth_limit;
        bool clear_run = true;
        for (Property const& property : m_properties) {
            if (property.key.is_index() && property.key.as_index() >= size && property.key.as_index() < index)
                clear_run = false;
        }
        if (is_dense_eligible(fresh) && near && clear_run) {
            if (index >= size)
                m_elements.resize(static_cast<std::size_t>(index) + 1, Value::empty());
            m_elements[index] = fresh.value;
        } else {
            // An ordinary index inside the dense range would be shadowed
            // by the hole there, so the dense storage ends below it.
            if (index < size) {
                for (std::uint32_t i = index; i < size; ++i) {
                    if (!m_elements[i].is_empty())
                        insert(PropertyKey::index(i)).value = m_elements[i];
                }
                m_elements.resize(index);
            }
            insert(key) = fresh;
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
    for (Property const& property : m_properties) {
        if (property.key.is_index())
            indices.push_back(property.key.as_index());
    }
    std::sort(indices.begin(), indices.end());
    std::vector<PropertyKey> keys;
    keys.reserve(indices.size() + m_properties.size() + 1);
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

ScriptFunction::ScriptFunction(Object* prototype, FunctionNode const& node, Environment* scope, bool constructable)
    : Function(prototype)
    , m_node(&node)
    , m_scope(scope)
    , m_constructable(constructable)
{
}

bool ScriptFunction::is_arrow() const
{
    return m_node->is_arrow;
}

bool ScriptFunction::is_strict() const
{
    return m_node->is_strict;
}

void ScriptFunction::trace(Tracer& tracer)
{
    // The node is the program's, which the realm keeps for as long as it
    // lives; the closed-over scope, the home object and the fields' keys
    // and initializers are cells.
    Object::trace(tracer);
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

std::optional<Value> NativeFunction::call(Interpreter& interpreter, Value const& this_value, std::span<Value const> arguments)
{
    if (!m_call)
        return interpreter.throw_type_error("not a function");
    return m_call(interpreter, this_value, arguments);
}

std::optional<Value> NativeFunction::construct(Interpreter& interpreter, std::span<Value const> arguments, Object* new_target)
{
    if (!m_construct)
        return interpreter.throw_type_error("not a constructor");
    return m_construct(interpreter, arguments, new_target);
}

std::optional<Value> ClosureFunction::call(Interpreter& interpreter, Value const& this_value, std::span<Value const> arguments)
{
    return m_callback(interpreter, *this, this_value, arguments);
}

void ClosureFunction::trace(Tracer& tracer)
{
    Object::trace(tracer);
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
    Object::trace(tracer);
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
        return is_compatible(m_extensible, desc, get_own_property(key));
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
}

// ----------------------------------------------------------- Environment

Environment::Binding* Environment::find(JsString* name)
{
    for (Binding& binding : m_bindings) {
        if (binding.name == name)
            return &binding;
    }
    return nullptr;
}

Environment::Binding const* Environment::find(JsString* name) const
{
    for (Binding const& binding : m_bindings) {
        if (binding.name == name)
            return &binding;
    }
    return nullptr;
}

Environment::Binding& Environment::declare(JsString* name, Value initial, bool mutable_, bool initialized, bool deletable)
{
    if (Binding* existing = find(name))
        return *existing;
    Binding& binding = m_bindings.emplace_back();
    binding.name = name;
    binding.value = initial;
    binding.mutable_ = mutable_;
    binding.initialized = initialized;
    binding.deletable = deletable;
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
        return true;
    }
    return false;
}

void Environment::trace(Tracer& tracer)
{
    for (Binding const& binding : m_bindings) {
        tracer.visit(binding.name);
        tracer.visit(binding.value);
    }
    tracer.visit(m_outer);
    tracer.visit(m_object);
    tracer.visit(m_this);
    tracer.visit(m_function);
    tracer.visit(m_new_target);
}

}
