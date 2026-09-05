#include "js/Runtime.h"

// The keyed collections (§24): Map, Set, WeakMap and WeakSet over one
// insertion-ordered table keyed by SameValueZero, their iterators
// (%MapIteratorPrototype%, %SetIteratorPrototype%), the Set methods of
// ES2025 (union, intersection, difference, symmetricDifference,
// isSubsetOf, isSupersetOf, isDisjointFrom over any set-like), and
// Map.groupBy / Object.groupBy.
//
// The weak kinds hold their keys strongly: the collector has no
// ephemerons yet, and nothing a script can observe tells the difference.

#include "js/Object.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

// ------------------------------------------------------- CollectionTable

std::size_t CollectionTable::Hash::operator()(Value const& value) const
{
    // SameValueZero's equivalence classes: −0 with +0, every NaN with
    // every other, strings by their contents, everything else by identity.
    if (value.is_number()) {
        double number = value.as_number();
        if (number == 0)
            number = 0;
        if (number != number)
            return 0x7ff8;
        return std::hash<double> {}(number);
    }
    if (value.is_string())
        return value.as_string()->hash();
    if (value.is_boolean())
        return value.as_boolean() ? 3 : 5;
    if (value.is_undefined())
        return 7;
    if (value.is_null())
        return 11;
    return std::hash<void const*> {}(value.as_cell());
}

bool CollectionTable::Equal::operator()(Value const& a, Value const& b) const
{
    return Interpreter::same_value_zero(a, b);
}

CollectionTable::~CollectionTable()
{
    for (CollectionIteratorObject* iterator : m_iterators)
        iterator->table_gone();
}

CollectionTable::Entry const* CollectionTable::find(Value const& key) const
{
    auto const it = m_index.find(key);
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

void CollectionTable::set(Value const& key, Value const& value)
{
    Value stored = key;
    if (stored.is_number() && stored.as_number() == 0)
        stored = Value::number(0); // §24.1.3.9 step 6: −0 becomes +0
    auto const it = m_index.find(stored);
    if (it != m_index.end()) {
        m_entries[it->second].value = value;
        return;
    }
    m_index.emplace(stored, m_entries.size());
    m_entries.push_back(Entry { stored, value, true });
    ++m_live;
}

bool CollectionTable::remove(Value const& key)
{
    auto const it = m_index.find(key);
    if (it == m_index.end())
        return false;
    Entry& entry = m_entries[it->second];
    entry.live = false;
    entry.key = Value::undefined();
    entry.value = Value::undefined();
    m_index.erase(it);
    --m_live;
    if (m_entries.size() > 32 && m_entries.size() > 2 * m_live)
        compact();
    return true;
}

void CollectionTable::clear()
{
    // §24.1.3.1: every entry emptied where it stands, so an iterator keeps
    // its place and sees only what is added afterwards.
    for (Entry& entry : m_entries) {
        entry.live = false;
        entry.key = Value::undefined();
        entry.value = Value::undefined();
    }
    m_index.clear();
    m_live = 0;
    compact();
}

void CollectionTable::compact()
{
    if (m_walkers > 0)
        return; // a forEach is counting through the holes; later
    std::size_t const old_size = m_entries.size();
    std::vector<std::size_t> moved(old_size + 1);
    std::vector<Entry> kept;
    kept.reserve(m_live);
    for (std::size_t i = 0; i < old_size; ++i) {
        moved[i] = kept.size();
        if (m_entries[i].live)
            kept.push_back(m_entries[i]);
    }
    moved[old_size] = kept.size();
    m_entries = std::move(kept);
    m_index.clear();
    for (std::size_t i = 0; i < m_entries.size(); ++i)
        m_index.emplace(m_entries[i].key, i);
    for (CollectionIteratorObject* iterator : m_iterators)
        iterator->set_index(moved[std::min(iterator->index(), old_size)]);
}

void CollectionTable::attach(CollectionIteratorObject* iterator)
{
    m_iterators.push_back(iterator);
}

void CollectionTable::detach(CollectionIteratorObject* iterator)
{
    std::erase(m_iterators, iterator);
}

void CollectionTable::trace(Tracer& tracer) const
{
    for (Entry const& entry : m_entries) {
        if (!entry.live)
            continue;
        tracer.visit(entry.key);
        tracer.visit(entry.value);
    }
}

// ------------------------------------------------------ the objects

void CollectionObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    m_table.trace(tracer);
}

CollectionIteratorObject::CollectionIteratorObject(Object* prototype, CollectionObject* collection, Kind kind, bool is_map)
    : Object(prototype, Class::CollectionIterator)
    , m_collection(collection)
    , m_kind(kind)
    , m_is_map(is_map)
{
    m_collection->table().attach(this);
}

CollectionIteratorObject::~CollectionIteratorObject()
{
    if (m_attached && m_collection)
        m_collection->table().detach(this);
}

void CollectionIteratorObject::finish()
{
    if (m_attached && m_collection)
        m_collection->table().detach(this);
    m_attached = false;
    m_collection = nullptr;
}

void CollectionIteratorObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_collection);
}

// ------------------------------------------------------- the library

namespace {

char const* class_name(Object::Class class_id)
{
    switch (class_id) {
    case Object::Class::Map: return "Map";
    case Object::Class::Set: return "Set";
    case Object::Class::WeakMap: return "WeakMap";
    case Object::Class::WeakSet: return "WeakSet";
    default: return "?";
    }
}

// A key as the table stores it: −0 as +0 (§24.1.3.9 step 6 and kin).
Value normalized(Value const& value)
{
    if (value.is_number() && value.as_number() == 0)
        return Value::number(0);
    return value;
}

// The receiver as the collection a method wants, or a TypeError naming it.
std::optional<CollectionObject*> this_collection(Interpreter& in, Value const& this_value, Object::Class class_id, std::string_view method)
{
    if (!this_value.is_object() || this_value.as_object()->class_id() != class_id)
        return in.throw_type_error("Method " + std::string(class_name(class_id)) + ".prototype." + std::string(method)
            + " called on incompatible receiver " + in.describe(this_value));
    return static_cast<CollectionObject*>(this_value.as_object());
}

// CanBeHeldWeakly (§9.13): an object, or a symbol that is not in the
// global registry.
bool can_be_held_weakly(Interpreter& in, Value const& value)
{
    if (value.is_object())
        return true;
    if (!value.is_symbol())
        return false;
    Object* registry = in.intrinsics().symbol_registry;
    if (registry == nullptr)
        return true;
    for (PropertyKey const& key : registry->own_keys()) {
        std::optional<PropertyDescriptor> const desc = registry->get_own_property(key);
        if (desc && desc->value && desc->value->is_symbol() && desc->value->as_symbol() == value.as_symbol())
            return false;
    }
    return true;
}

// AddEntriesFromIterable (§24.1.1.2) and the Set constructor's loop
// (§24.2.1.1): every value the iterable yields handed to the adder — for
// a map, an entry object's 0 and 1 — the iterator closed on any throw.
std::optional<Value> add_entries_from_iterable(Interpreter& in, CollectionObject& target, Value const& iterable, Value const& adder, bool pairs)
{
    Interpreter::Roots const roots(in);
    in.root(Value::object(&target));
    in.root(iterable);
    in.root(adder);
    std::optional<IteratorRecord> record = in.get_iterator(iterable);
    if (!record)
        return std::nullopt;
    in.root(record->iterator);
    in.root(record->next_method);
    while (true) {
        Value item;
        std::optional<bool> const stepped = in.iterator_step(*record, item);
        if (!stepped)
            return std::nullopt;
        if (!*stepped)
            return Value::object(&target);
        Interpreter::Roots const item_roots(in);
        in.root(item);
        std::optional<Value> result;
        if (pairs) {
            if (!item.is_object()) {
                in.throw_type_error("Iterator value " + in.describe(item) + " is not an entry object");
                in.iterator_close(*record, true);
                return std::nullopt;
            }
            std::optional<Value> const key = in.get(item, PropertyKey::index(0));
            if (!key) {
                in.iterator_close(*record, true);
                return std::nullopt;
            }
            in.root(*key);
            std::optional<Value> const value = in.get(item, PropertyKey::index(1));
            if (!value) {
                in.iterator_close(*record, true);
                return std::nullopt;
            }
            in.root(*value);
            Value const arguments[2] = { *key, *value };
            result = in.call(adder, Value::object(&target), arguments);
        } else {
            Value const arguments[1] = { item };
            result = in.call(adder, Value::object(&target), arguments);
        }
        if (!result) {
            in.iterator_close(*record, true);
            return std::nullopt;
        }
    }
}

struct CollectionKind {
    Object::Class class_id;
    bool pairs; // the constructor's iterable yields [key, value] entries
    std::string adder; // the method the constructor feeds ("set" or "add")
    Object* Intrinsics::*prototype;
};

// A constructor that requires `new` (§24.1.1.1, §24.2.1.1, §24.3.1.1,
// §24.4.1.1): the object from new.target's prototype, then the iterable.
NativeFunction* install_constructor(Interpreter& in, std::string_view name, Object& prototype, CollectionKind kind)
{
    Heap::NoCollect const guard(in.heap());
    std::string const constructor_name(name);
    NativeFunction* constructor = in.new_native(
        name, 0,
        [constructor_name](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
            return interp.throw_type_error("Constructor " + constructor_name + " requires 'new'");
        },
        [kind](Interpreter& interp, Args args, Object* new_target) -> std::optional<Value> {
            Interpreter::Roots const roots(interp);
            if (new_target)
                interp.root(Value::object(new_target));
            std::optional<Object*> const proto = interp.get_prototype_from_constructor(new_target, interp.intrinsics().*(kind.prototype));
            if (!proto)
                return std::nullopt;
            CollectionObject* collection = interp.heap().allocate<CollectionObject>(*proto, kind.class_id);
            interp.root(Value::object(collection));
            Value const iterable = argument(args, 0);
            if (iterable.is_nullish())
                return Value::object(collection);
            std::optional<Value> const adder = interp.get(*collection, interp.key(kind.adder));
            if (!adder)
                return std::nullopt;
            if (!Interpreter::is_callable(*adder))
                return interp.throw_type_error("'" + interp.describe(*adder) + "' returned for property '" + kind.adder + "' of object is not a function");
            return add_entries_from_iterable(interp, *collection, iterable, *adder, kind.pairs);
        });
    constructor->put(PropertyKey::atom(in.atoms().prototype), Value::object(&prototype), frozen_attributes);
    prototype.put(PropertyKey::atom(in.atoms().constructor), Value::object(constructor), builtin_attributes);
    in.global()->put(in.key(name), Value::object(constructor), builtin_attributes);
    // get [Symbol.species] (§24.1.2.2, §24.2.2.2): the receiver.
    NativeFunction* species = in.new_native("get [Symbol.species]", 0, [](Interpreter&, Value const& this_value, Args) -> std::optional<Value> {
        return this_value;
    });
    constructor->put_accessor(PropertyKey::symbol(in.atoms().symbol_species), species, nullptr, Configurable);
    return constructor;
}

// Map.prototype.getOrInsert / getOrInsertComputed and the WeakMap pair
// (the upsert proposal): the value under the key, or the one given —
// or computed by the callback, which may itself have added the key —
// stored first. A weak map refuses a key it cannot hold.
std::optional<Value> get_or_insert(Interpreter& in, Value const& this_value, Args args, Object::Class class_id, bool computed)
{
    char const* const method = computed ? "getOrInsertComputed" : "getOrInsert";
    std::optional<CollectionObject*> const map = this_collection(in, this_value, class_id, method);
    if (!map)
        return std::nullopt;
    Value const key = normalized(argument(args, 0));
    if (class_id == Object::Class::WeakMap && !can_be_held_weakly(in, key))
        return in.throw_type_error("Invalid value used as weak map key");
    Value const second = argument(args, 1);
    if (computed && !Interpreter::is_callable(second))
        return in.throw_type_error(in.describe(second) + " is not a function");
    if (CollectionTable::Entry const* entry = (*map)->table().find(key))
        return entry->value;
    Value value = second;
    if (computed) {
        Interpreter::Roots const roots(in);
        in.root(this_value);
        in.root(key);
        in.root(second);
        Value const arguments[1] = { key };
        std::optional<Value> const result = in.call(second, Value::undefined(), arguments);
        if (!result)
            return std::nullopt;
        value = *result;
    }
    (*map)->table().set(key, value);
    return value;
}

// forEach (§24.1.3.5, §24.2.3.6): the callback over every live entry in
// order, entries added meanwhile included; the table does not compact
// while the walk counts through it.
std::optional<Value> for_each(Interpreter& in, CollectionObject& collection, Args args, bool pairs)
{
    Value const callback = argument(args, 0);
    if (!Interpreter::is_callable(callback))
        return in.throw_type_error(in.describe(callback) + " is not a function");
    Value const this_arg = argument(args, 1);
    Interpreter::Roots const roots(in);
    in.root(Value::object(&collection));
    in.root(callback);
    in.root(this_arg);
    CollectionTable::WalkGuard const walking(collection.table());
    for (std::size_t i = 0; i < collection.table().entries().size(); ++i) {
        CollectionTable::Entry const entry = collection.table().entries()[i];
        if (!entry.live)
            continue;
        Interpreter::Roots const entry_roots(in);
        in.root(entry.key);
        in.root(entry.value);
        Value const arguments[3] = { pairs ? entry.value : entry.key, entry.key, Value::object(&collection) };
        if (!in.call(callback, this_arg, arguments))
            return std::nullopt;
    }
    return Value::undefined();
}

std::optional<Value> make_iterator(Interpreter& in, CollectionObject& collection, CollectionIteratorObject::Kind kind, bool is_map)
{
    Heap::NoCollect const guard(in.heap());
    Object* prototype = is_map ? in.intrinsics().map_iterator_prototype : in.intrinsics().set_iterator_prototype;
    return Value::object(in.heap().allocate<CollectionIteratorObject>(prototype, &collection, kind, is_map));
}

// %MapIteratorPrototype%.next / %SetIteratorPrototype%.next (§24.1.5.2.1,
// §24.2.5.2.1): the next live entry past the position; at the end the
// collection is let go.
std::optional<Value> iterator_next(Interpreter& in, Value const& this_value, bool is_map)
{
    char const* const which = is_map ? "Map Iterator" : "Set Iterator";
    if (!this_value.is_object() || this_value.as_object()->class_id() != Object::Class::CollectionIterator
        || static_cast<CollectionIteratorObject*>(this_value.as_object())->is_map() != is_map)
        return in.throw_type_error("next method called on incompatible receiver " + in.describe(this_value) + " (wanted a " + which + ")");
    auto& iterator = *static_cast<CollectionIteratorObject*>(this_value.as_object());
    Interpreter::Roots const roots(in);
    in.root(this_value);
    CollectionObject* collection = iterator.collection();
    if (collection == nullptr)
        return Value::object(in.create_iter_result(Value::undefined(), true));
    std::vector<CollectionTable::Entry> const& entries = collection->table().entries();
    while (iterator.index() < entries.size()) {
        CollectionTable::Entry const entry = entries[iterator.index()];
        iterator.set_index(iterator.index() + 1);
        if (!entry.live)
            continue;
        in.root(entry.key);
        in.root(entry.value);
        switch (iterator.kind()) {
        case CollectionIteratorObject::Kind::Keys:
            return Value::object(in.create_iter_result(entry.key, false));
        case CollectionIteratorObject::Kind::Values:
            return Value::object(in.create_iter_result(is_map ? entry.value : entry.key, false));
        case CollectionIteratorObject::Kind::Entries: {
            Value const pair[2] = { entry.key, is_map ? entry.value : entry.key };
            ArrayObject* array = in.new_array(pair);
            in.root(Value::object(array));
            return Value::object(in.create_iter_result(Value::object(array), false));
        }
        }
    }
    iterator.finish();
    return Value::object(in.create_iter_result(Value::undefined(), true));
}

// ---- the Set methods of §24.2.4 over set-likes

// GetSetRecord (§24.2.1.2): a size, a has and a keys.
struct SetRecord {
    Object* set = nullptr;
    double size = 0;
    Value has;
    Value keys;
};

std::optional<SetRecord> get_set_record(Interpreter& in, Value const& other)
{
    if (!other.is_object())
        return in.throw_type_error("The 'other' argument must be an object");
    SetRecord record;
    record.set = other.as_object();
    Interpreter::Roots const roots(in);
    in.root(other);
    std::optional<Value> const raw_size = in.get(other, in.key("size"));
    if (!raw_size)
        return std::nullopt;
    std::optional<double> const number_size = in.to_number(*raw_size);
    if (!number_size)
        return std::nullopt;
    if (*number_size != *number_size)
        return in.throw_type_error("The 'size' property must be a number");
    double const integer_size = Interpreter::to_integer_or_infinity(*number_size);
    if (integer_size < 0)
        return in.throw_range_error("The 'size' property must not be negative");
    record.size = integer_size;
    std::optional<Value> const has = in.get(other, in.key("has"));
    if (!has)
        return std::nullopt;
    if (!Interpreter::is_callable(*has))
        return in.throw_type_error("The 'has' property must be a function");
    record.has = *has;
    std::optional<Value> const keys = in.get(other, in.key("keys"));
    if (!keys)
        return std::nullopt;
    if (!Interpreter::is_callable(*keys))
        return in.throw_type_error("The 'keys' property must be a function");
    record.keys = *keys;
    return record;
}

std::optional<bool> set_record_has(Interpreter& in, SetRecord const& record, Value const& value)
{
    Value const arguments[1] = { value };
    std::optional<Value> const result = in.call(record.has, Value::object(record.set), arguments);
    if (!result)
        return std::nullopt;
    return Interpreter::to_boolean(*result);
}

std::optional<IteratorRecord> set_record_keys(Interpreter& in, SetRecord const& record)
{
    // GetIteratorFromMethod over keys (§24.2.4 steps); the result must
    // be an object, and its `next` is read once.
    return in.get_iterator_from_method(Value::object(record.set), record.keys);
}

// A fresh Set holding this set's live entries (the methods build their
// results from a copy, §24.2.4.x "Let resultSetData be a copy").
CollectionObject* copy_set(Interpreter& in, CollectionObject const& source)
{
    Heap::NoCollect const guard(in.heap());
    auto* result = in.heap().allocate<CollectionObject>(in.intrinsics().set_prototype, Object::Class::Set);
    for (CollectionTable::Entry const& entry : source.table().entries()) {
        if (entry.live)
            result->table().set(entry.key, entry.key);
    }
    return result;
}

CollectionObject* new_set(Interpreter& in)
{
    return in.heap().allocate<CollectionObject>(in.intrinsics().set_prototype, Object::Class::Set);
}

// Walks this set's entries by index, live ones only, the table kept from
// compacting; `visit` answers false to stop.
std::optional<bool> walk_set(Interpreter& in, CollectionObject& set, std::function<std::optional<bool>(Value const&)> const& visit)
{
    CollectionTable::WalkGuard const walking(set.table());
    for (std::size_t i = 0; i < set.table().entries().size(); ++i) {
        CollectionTable::Entry const entry = set.table().entries()[i];
        if (!entry.live)
            continue;
        Interpreter::Roots const roots(in);
        in.root(entry.key);
        std::optional<bool> const go_on = visit(entry.key);
        if (!go_on)
            return std::nullopt;
        if (!*go_on)
            return false;
    }
    return true;
}

// Walks the other set-like's keys iterator; `visit` answers false to stop,
// which closes the iterator.
std::optional<bool> walk_keys(Interpreter& in, SetRecord const& record, std::function<std::optional<bool>(Value const&)> const& visit)
{
    std::optional<IteratorRecord> iterator = set_record_keys(in, record);
    if (!iterator)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(iterator->iterator);
    in.root(iterator->next_method);
    while (true) {
        Value next;
        std::optional<bool> const stepped = in.iterator_step(*iterator, next);
        if (!stepped)
            return std::nullopt;
        if (!*stepped)
            return true;
        Interpreter::Roots const step_roots(in);
        in.root(next);
        std::optional<bool> const go_on = visit(next);
        if (!go_on) {
            in.iterator_close(*iterator, true);
            return std::nullopt;
        }
        if (!*go_on) {
            if (!in.iterator_close(*iterator, false))
                return std::nullopt;
            return false;
        }
    }
}

std::optional<Value> set_union(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<CollectionObject*> const set = this_collection(in, this_value, Object::Class::Set, "union");
    if (!set)
        return std::nullopt;
    std::optional<SetRecord> const other = get_set_record(in, argument(args, 0));
    if (!other)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(argument(args, 0));
    in.root(other->has);
    in.root(other->keys);
    CollectionObject* result = copy_set(in, **set);
    in.root(Value::object(result));
    std::optional<bool> const walked = walk_keys(in, *other, [&](Value const& key) -> std::optional<bool> {
        result->table().set(normalized(key), normalized(key));
        return true;
    });
    if (!walked)
        return std::nullopt;
    return Value::object(result);
}

std::optional<Value> set_intersection(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<CollectionObject*> const set = this_collection(in, this_value, Object::Class::Set, "intersection");
    if (!set)
        return std::nullopt;
    std::optional<SetRecord> const other = get_set_record(in, argument(args, 0));
    if (!other)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(argument(args, 0));
    in.root(other->has);
    in.root(other->keys);
    CollectionObject* result = new_set(in);
    in.root(Value::object(result));
    std::optional<bool> walked;
    if (static_cast<double>((*set)->table().size()) <= other->size) {
        walked = walk_set(in, **set, [&](Value const& key) -> std::optional<bool> {
            std::optional<bool> const has = set_record_has(in, *other, key);
            if (!has)
                return std::nullopt;
            if (*has)
                result->table().set(key, key);
            return true;
        });
    } else {
        walked = walk_keys(in, *other, [&](Value const& key) -> std::optional<bool> {
            if ((*set)->table().has(key))
                result->table().set(normalized(key), normalized(key));
            return true;
        });
    }
    if (!walked)
        return std::nullopt;
    return Value::object(result);
}

std::optional<Value> set_difference(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<CollectionObject*> const set = this_collection(in, this_value, Object::Class::Set, "difference");
    if (!set)
        return std::nullopt;
    std::optional<SetRecord> const other = get_set_record(in, argument(args, 0));
    if (!other)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(argument(args, 0));
    in.root(other->has);
    in.root(other->keys);
    CollectionObject* result = copy_set(in, **set);
    in.root(Value::object(result));
    std::optional<bool> walked;
    if (static_cast<double>((*set)->table().size()) <= other->size) {
        walked = walk_set(in, **set, [&](Value const& key) -> std::optional<bool> {
            std::optional<bool> const has = set_record_has(in, *other, key);
            if (!has)
                return std::nullopt;
            if (*has)
                result->table().remove(key);
            return true;
        });
    } else {
        walked = walk_keys(in, *other, [&](Value const& key) -> std::optional<bool> {
            result->table().remove(key);
            return true;
        });
    }
    if (!walked)
        return std::nullopt;
    return Value::object(result);
}

std::optional<Value> set_symmetric_difference(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<CollectionObject*> const set = this_collection(in, this_value, Object::Class::Set, "symmetricDifference");
    if (!set)
        return std::nullopt;
    std::optional<SetRecord> const other = get_set_record(in, argument(args, 0));
    if (!other)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(argument(args, 0));
    in.root(other->has);
    in.root(other->keys);
    CollectionObject* result = copy_set(in, **set);
    in.root(Value::object(result));
    std::optional<bool> const walked = walk_keys(in, *other, [&](Value const& key) -> std::optional<bool> {
        Value const value = normalized(key);
        if ((*set)->table().has(value))
            result->table().remove(value);
        else
            result->table().set(value, value);
        return true;
    });
    if (!walked)
        return std::nullopt;
    return Value::object(result);
}

std::optional<Value> set_is_subset_of(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<CollectionObject*> const set = this_collection(in, this_value, Object::Class::Set, "isSubsetOf");
    if (!set)
        return std::nullopt;
    std::optional<SetRecord> const other = get_set_record(in, argument(args, 0));
    if (!other)
        return std::nullopt;
    if (static_cast<double>((*set)->table().size()) > other->size)
        return Value::boolean(false);
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(argument(args, 0));
    in.root(other->has);
    in.root(other->keys);
    std::optional<bool> const walked = walk_set(in, **set, [&](Value const& key) -> std::optional<bool> {
        std::optional<bool> const has = set_record_has(in, *other, key);
        if (!has)
            return std::nullopt;
        return *has;
    });
    if (!walked)
        return std::nullopt;
    return Value::boolean(*walked);
}

std::optional<Value> set_is_superset_of(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<CollectionObject*> const set = this_collection(in, this_value, Object::Class::Set, "isSupersetOf");
    if (!set)
        return std::nullopt;
    std::optional<SetRecord> const other = get_set_record(in, argument(args, 0));
    if (!other)
        return std::nullopt;
    if (static_cast<double>((*set)->table().size()) < other->size)
        return Value::boolean(false);
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(argument(args, 0));
    in.root(other->has);
    in.root(other->keys);
    std::optional<bool> const walked = walk_keys(in, *other, [&](Value const& key) -> std::optional<bool> {
        return (*set)->table().has(key);
    });
    if (!walked)
        return std::nullopt;
    return Value::boolean(*walked);
}

std::optional<Value> set_is_disjoint_from(Interpreter& in, Value const& this_value, Args args)
{
    std::optional<CollectionObject*> const set = this_collection(in, this_value, Object::Class::Set, "isDisjointFrom");
    if (!set)
        return std::nullopt;
    std::optional<SetRecord> const other = get_set_record(in, argument(args, 0));
    if (!other)
        return std::nullopt;
    Interpreter::Roots const roots(in);
    in.root(this_value);
    in.root(argument(args, 0));
    in.root(other->has);
    in.root(other->keys);
    std::optional<bool> walked;
    if (static_cast<double>((*set)->table().size()) <= other->size) {
        walked = walk_set(in, **set, [&](Value const& key) -> std::optional<bool> {
            std::optional<bool> const has = set_record_has(in, *other, key);
            if (!has)
                return std::nullopt;
            return !*has;
        });
    } else {
        walked = walk_keys(in, *other, [&](Value const& key) -> std::optional<bool> {
            return !(*set)->table().has(key);
        });
    }
    if (!walked)
        return std::nullopt;
    return Value::boolean(*walked);
}

// GroupBy (§7.3.35): the items grouped by what the callback answers for
// each, as a list of (key, elements) in first-seen order.
struct Group {
    Value key;
    std::vector<Value> elements;
};

std::optional<std::vector<Group>> group_by(Interpreter& in, Args args, bool property_keys)
{
    Value const items = argument(args, 0);
    Value const callback = argument(args, 1);
    if (items.is_nullish())
        return in.throw_type_error("Cannot convert undefined or null to object");
    if (!Interpreter::is_callable(callback))
        return in.throw_type_error(in.describe(callback) + " is not a function");
    Interpreter::Roots const roots(in);
    in.root(items);
    in.root(callback);
    std::optional<IteratorRecord> record = in.get_iterator(items);
    if (!record)
        return std::nullopt;
    in.root(record->iterator);
    in.root(record->next_method);
    std::vector<Group> groups;
    double index = 0;
    while (true) {
        Value item;
        std::optional<bool> const stepped = in.iterator_step(*record, item);
        if (!stepped)
            return std::nullopt;
        if (!*stepped)
            return groups;
        in.root(item);
        Value const arguments[2] = { item, Value::number(index) };
        std::optional<Value> key = in.call(callback, Value::undefined(), arguments);
        if (!key) {
            in.iterator_close(*record, true);
            return std::nullopt;
        }
        in.root(*key);
        if (property_keys) {
            std::optional<PropertyKey> const converted = in.to_property_key(*key);
            if (!converted) {
                in.iterator_close(*record, true);
                return std::nullopt;
            }
            key = converted->is_symbol() ? Value::symbol(converted->as_symbol()) : Value::string(in.heap().key_to_string(*converted));
            in.root(*key);
        } else {
            key = normalized(*key);
        }
        auto found = std::find_if(groups.begin(), groups.end(), [&](Group const& group) {
            return property_keys ? Interpreter::same_value(group.key, *key) : Interpreter::same_value_zero(group.key, *key);
        });
        if (found == groups.end()) {
            groups.push_back(Group { *key, {} });
            found = groups.end() - 1;
        }
        found->elements.push_back(item);
        index += 1;
    }
}

} // namespace

void install_collections(Interpreter& in)
{
    Intrinsics& i = in.intrinsics();
    WellKnownAtoms const& atoms = in.atoms();
    Heap::NoCollect const guard(in.heap());

    // ---- Map (§24.1)
    i.map_prototype = in.new_object();
    Object& map_prototype = *i.map_prototype;
    NativeFunction* map_constructor = install_constructor(in, "Map", map_prototype,
        CollectionKind { Object::Class::Map, true, "set", &Intrinsics::map_prototype });
    define_method(in, map_prototype, "get", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "get");
        if (!map)
            return std::nullopt;
        CollectionTable::Entry const* entry = (*map)->table().find(argument(args, 0));
        return entry ? entry->value : Value::undefined();
    });
    define_method(in, map_prototype, "set", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "set");
        if (!map)
            return std::nullopt;
        (*map)->table().set(argument(args, 0), argument(args, 1));
        return this_value;
    });
    define_method(in, map_prototype, "has", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "has");
        if (!map)
            return std::nullopt;
        return Value::boolean((*map)->table().has(argument(args, 0)));
    });
    define_method(in, map_prototype, "delete", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "delete");
        if (!map)
            return std::nullopt;
        return Value::boolean((*map)->table().remove(argument(args, 0)));
    });
    define_method(in, map_prototype, "clear", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "clear");
        if (!map)
            return std::nullopt;
        (*map)->table().clear();
        return Value::undefined();
    });
    define_method(in, map_prototype, "forEach", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "forEach");
        if (!map)
            return std::nullopt;
        return for_each(interp, **map, args, true);
    });
    define_method(in, map_prototype, "getOrInsert", 2, [](Interpreter& interp, Value const& this_value, Args args) {
        return get_or_insert(interp, this_value, args, Object::Class::Map, false);
    });
    define_method(in, map_prototype, "getOrInsertComputed", 2, [](Interpreter& interp, Value const& this_value, Args args) {
        return get_or_insert(interp, this_value, args, Object::Class::Map, true);
    });
    define_accessor(in, map_prototype, "size", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "size");
        if (!map)
            return std::nullopt;
        return Value::number(static_cast<double>((*map)->table().size()));
    });
    define_method(in, map_prototype, "keys", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "keys");
        if (!map)
            return std::nullopt;
        return make_iterator(interp, **map, CollectionIteratorObject::Kind::Keys, true);
    });
    define_method(in, map_prototype, "values", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "values");
        if (!map)
            return std::nullopt;
        return make_iterator(interp, **map, CollectionIteratorObject::Kind::Values, true);
    });
    NativeFunction* map_entries = define_method(in, map_prototype, "entries", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::Map, "entries");
        if (!map)
            return std::nullopt;
        return make_iterator(interp, **map, CollectionIteratorObject::Kind::Entries, true);
    });
    map_prototype.put(PropertyKey::symbol(atoms.symbol_iterator), Value::object(map_entries), builtin_attributes);
    map_prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Map")), Configurable);
    // Map.groupBy (§24.1.2.1).
    define_method(in, *map_constructor, "groupBy", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<std::vector<Group>> const groups = group_by(interp, args, false);
        if (!groups)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        for (Group const& group : *groups) {
            interp.root(group.key);
            for (Value const& element : group.elements)
                interp.root(element);
        }
        auto* map = interp.heap().allocate<CollectionObject>(interp.intrinsics().map_prototype, Object::Class::Map);
        interp.root(Value::object(map));
        for (Group const& group : *groups) {
            ArrayObject* elements = interp.new_array(group.elements);
            map->table().set(group.key, Value::object(elements));
        }
        return Value::object(map);
    });
    // Object.groupBy (§20.1.2.11), the same over property keys.
    define_method(in, *i.object_constructor, "groupBy", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        std::optional<std::vector<Group>> const groups = group_by(interp, args, true);
        if (!groups)
            return std::nullopt;
        Interpreter::Roots const roots(interp);
        for (Group const& group : *groups) {
            interp.root(group.key);
            for (Value const& element : group.elements)
                interp.root(element);
        }
        Object* object = interp.new_object();
        object->set_prototype(nullptr);
        interp.root(Value::object(object));
        for (Group const& group : *groups) {
            ArrayObject* elements = interp.new_array(group.elements);
            interp.root(Value::object(elements));
            std::optional<PropertyKey> const key = interp.to_property_key(group.key);
            if (!key)
                return std::nullopt;
            if (!interp.create_data_property(*object, *key, Value::object(elements)))
                return std::nullopt;
        }
        return Value::object(object);
    });

    // ---- Set (§24.2)
    i.set_prototype = in.new_object();
    Object& set_prototype = *i.set_prototype;
    install_constructor(in, "Set", set_prototype, CollectionKind { Object::Class::Set, false, "add", &Intrinsics::set_prototype });
    define_method(in, set_prototype, "add", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "add");
        if (!set)
            return std::nullopt;
        Value const value = normalized(argument(args, 0));
        (*set)->table().set(value, value);
        return this_value;
    });
    define_method(in, set_prototype, "has", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "has");
        if (!set)
            return std::nullopt;
        return Value::boolean((*set)->table().has(argument(args, 0)));
    });
    define_method(in, set_prototype, "delete", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "delete");
        if (!set)
            return std::nullopt;
        return Value::boolean((*set)->table().remove(argument(args, 0)));
    });
    define_method(in, set_prototype, "clear", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "clear");
        if (!set)
            return std::nullopt;
        (*set)->table().clear();
        return Value::undefined();
    });
    define_method(in, set_prototype, "forEach", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "forEach");
        if (!set)
            return std::nullopt;
        return for_each(interp, **set, args, false);
    });
    define_accessor(in, set_prototype, "size", [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "size");
        if (!set)
            return std::nullopt;
        return Value::number(static_cast<double>((*set)->table().size()));
    });
    NativeFunction* set_values = define_method(in, set_prototype, "values", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "values");
        if (!set)
            return std::nullopt;
        return make_iterator(interp, **set, CollectionIteratorObject::Kind::Values, false);
    });
    set_prototype.put(in.key("keys"), Value::object(set_values), builtin_attributes); // §24.2.3.10: keys is values
    set_prototype.put(PropertyKey::symbol(atoms.symbol_iterator), Value::object(set_values), builtin_attributes);
    define_method(in, set_prototype, "entries", 0, [](Interpreter& interp, Value const& this_value, Args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::Set, "entries");
        if (!set)
            return std::nullopt;
        return make_iterator(interp, **set, CollectionIteratorObject::Kind::Entries, false);
    });
    set_prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Set")), Configurable);
    define_method(in, set_prototype, "union", 1, set_union);
    define_method(in, set_prototype, "intersection", 1, set_intersection);
    define_method(in, set_prototype, "difference", 1, set_difference);
    define_method(in, set_prototype, "symmetricDifference", 1, set_symmetric_difference);
    define_method(in, set_prototype, "isSubsetOf", 1, set_is_subset_of);
    define_method(in, set_prototype, "isSupersetOf", 1, set_is_superset_of);
    define_method(in, set_prototype, "isDisjointFrom", 1, set_is_disjoint_from);

    // ---- WeakMap (§24.3) and WeakSet (§24.4): keys that can be held weakly only.
    i.weak_map_prototype = in.new_object();
    Object& weak_map_prototype = *i.weak_map_prototype;
    install_constructor(in, "WeakMap", weak_map_prototype, CollectionKind { Object::Class::WeakMap, true, "set", &Intrinsics::weak_map_prototype });
    define_method(in, weak_map_prototype, "get", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::WeakMap, "get");
        if (!map)
            return std::nullopt;
        CollectionTable::Entry const* entry = (*map)->table().find(argument(args, 0));
        return entry ? entry->value : Value::undefined();
    });
    define_method(in, weak_map_prototype, "set", 2, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::WeakMap, "set");
        if (!map)
            return std::nullopt;
        if (!can_be_held_weakly(interp, argument(args, 0)))
            return interp.throw_type_error("Invalid value used as weak map key");
        (*map)->table().set(argument(args, 0), argument(args, 1));
        return this_value;
    });
    define_method(in, weak_map_prototype, "has", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::WeakMap, "has");
        if (!map)
            return std::nullopt;
        return Value::boolean((*map)->table().has(argument(args, 0)));
    });
    define_method(in, weak_map_prototype, "delete", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const map = this_collection(interp, this_value, Object::Class::WeakMap, "delete");
        if (!map)
            return std::nullopt;
        return Value::boolean((*map)->table().remove(argument(args, 0)));
    });
    define_method(in, weak_map_prototype, "getOrInsert", 2, [](Interpreter& interp, Value const& this_value, Args args) {
        return get_or_insert(interp, this_value, args, Object::Class::WeakMap, false);
    });
    define_method(in, weak_map_prototype, "getOrInsertComputed", 2, [](Interpreter& interp, Value const& this_value, Args args) {
        return get_or_insert(interp, this_value, args, Object::Class::WeakMap, true);
    });
    weak_map_prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("WeakMap")), Configurable);

    i.weak_set_prototype = in.new_object();
    Object& weak_set_prototype = *i.weak_set_prototype;
    install_constructor(in, "WeakSet", weak_set_prototype, CollectionKind { Object::Class::WeakSet, false, "add", &Intrinsics::weak_set_prototype });
    define_method(in, weak_set_prototype, "add", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::WeakSet, "add");
        if (!set)
            return std::nullopt;
        if (!can_be_held_weakly(interp, argument(args, 0)))
            return interp.throw_type_error("Invalid value used in weak set");
        (*set)->table().set(argument(args, 0), argument(args, 0));
        return this_value;
    });
    define_method(in, weak_set_prototype, "has", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::WeakSet, "has");
        if (!set)
            return std::nullopt;
        return Value::boolean((*set)->table().has(argument(args, 0)));
    });
    define_method(in, weak_set_prototype, "delete", 1, [](Interpreter& interp, Value const& this_value, Args args) -> std::optional<Value> {
        std::optional<CollectionObject*> const set = this_collection(interp, this_value, Object::Class::WeakSet, "delete");
        if (!set)
            return std::nullopt;
        return Value::boolean((*set)->table().remove(argument(args, 0)));
    });
    weak_set_prototype.put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("WeakSet")), Configurable);

    // ---- the iterators (§24.1.5, §24.2.5)
    i.map_iterator_prototype = in.new_object(i.iterator_prototype);
    define_method(in, *i.map_iterator_prototype, "next", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return iterator_next(interp, this_value, true);
    });
    i.map_iterator_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Map Iterator")), Configurable);
    i.set_iterator_prototype = in.new_object(i.iterator_prototype);
    define_method(in, *i.set_iterator_prototype, "next", 0, [](Interpreter& interp, Value const& this_value, Args) {
        return iterator_next(interp, this_value, false);
    });
    i.set_iterator_prototype->put(PropertyKey::symbol(atoms.symbol_to_string_tag), Value::string(in.atom("Set Iterator")), Configurable);
}

}
