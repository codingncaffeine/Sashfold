#include "bindings/Internal.h"

// The Indexed Database API (https://w3c.github.io/IndexedDB/): indexedDB on
// a window and on a worker's scope, and the IDB* interfaces over the storage
// of storage/IndexedDb.h. Script values meet the storage here: keys are
// converted from values and back (section 7), key paths are evaluated on a
// clone of each value stored (section 7.6), values are kept as structured
// clone serializations. A request's operation runs in a task of the realm's
// event loop when it reaches the front of its transaction's queue, and its
// success or error event is fired in that task, the transaction active while
// the listeners and their microtasks run; a transaction commits once it is
// inactive with nothing left to do, or aborts, undoing what it did.
//
// The storage is the host's for the origin (HostHooks::indexed_db), which
// may write each database to a file under the profile on every commit that
// changed it; without the hook, the agent keeps one in memory per origin.
// No quota is enforced.

#include "storage/IndexedDb.h"
#include "js/Object.h"
#include "js/Runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sashfold::bindings {

// The objects are the realm's too (IdbRealm below keeps them), so they are
// not in an anonymous namespace; their names are the bindings' own.
using idb::Key;
using idb::KeyPath;
using idb::KeyRange;
using Lock = std::lock_guard<std::mutex>;

class IdbTransactionObject;
class IdbDatabaseObject;
class IdbObjectStoreObject;
class IdbIndexObject;

// --- The objects -------------------------------------------------------------------------------

// IDBRequest and IDBOpenDBRequest (section 4.1): what an operation answers
// through. A request's parent on an event's path is its transaction.
class IdbRequestObject final : public EventTargetObject {
public:
    explicit IdbRequestObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    js::Value result;
    js::Value error = js::Value::null(); // a DOMException, or null
    js::Value source = js::Value::null(); // a store, an index or a cursor
    IdbTransactionObject* transaction = nullptr;
    bool done = false;
    bool open_request = false;
    js::Object* event_parent() const override;
    void trace(js::Tracer& tracer) override;
};

// What an open or delete request is waiting on, on the storage's side.
struct QueuedRequest {
    std::uint64_t id = 0;
    std::u16string name;
    idb::Storage::Database* database = nullptr;
    bool finished = false;
};

// A connection's side the storage need not see.
struct ConnectionCore {
    std::shared_ptr<idb::ConnectionEntry> entry;
    idb::Storage::Database* database = nullptr;
};

// IDBDatabase (section 4.4): a connection.
class IdbDatabaseObject final : public EventTargetObject {
public:
    explicit IdbDatabaseObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    std::shared_ptr<ConnectionCore> core;
    std::u16string name;
    std::uint64_t version = 0;
    IdbTransactionObject* upgrade = nullptr; // the live upgrade transaction
    bool close_pending() const { return core && core->entry->close_pending; }
    void trace(js::Tracer& tracer) override;
};

// A transaction's side the storage and a dying realm need: what undoes it.
struct TransactionCore {
    std::shared_ptr<idb::ScheduledTransaction> scheduled;
    idb::Storage::Database* database = nullptr;
    idb::UndoLog undo;
    std::optional<idb::DatabaseState> snapshot; // an upgrade's: the database as it was
    bool existed_before = true;
    bool finished = false;
};

// One request placed against a transaction whose event has not been fired:
// the operation, and the values it holds until it runs (a clone to store).
// An operation runs as soon as it is placed when the transaction has started
// and everything before it has run, as the shipping engines perform them, so
// that what a later call changes (a store deleted in an upgrade) comes after
// it; its event still waits for its turn. A cursor's step is not eager: the
// cursor keeps showing its record until the step's event.
struct Operation {
    IdbRequestObject* request = nullptr;
    std::function<void(Realm::Internals&, IdbTransactionObject&, IdbRequestObject&)> run;
    std::vector<js::Value> held;
    bool eager = true;
    bool executed = false;
    // With no request: a step of the transaction's own, in its turn.
    std::function<void(Realm::Internals&, IdbTransactionObject&)> control;
};

// IDBTransaction (section 4.9). Its parent on an event's path is its
// connection.
class IdbTransactionObject final : public EventTargetObject {
public:
    explicit IdbTransactionObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    enum class State : std::uint8_t { Active, Inactive, Committing, Finished };
    std::shared_ptr<TransactionCore> core;
    IdbDatabaseObject* db = nullptr;
    std::vector<std::u16string> scope; // sorted; an upgrade's is every store, read live
    idb::Mode mode = idb::Mode::ReadOnly;
    std::string durability = "default";
    State state = State::Active;
    js::Value error = js::Value::null();
    IdbRequestObject* open_request = nullptr; // an upgrade's
    std::uint64_t old_version = 0; // an upgrade's: the connection's before
    std::deque<Operation> queue;
    Operation* executing = nullptr; // the operation running now, which is on the queue
    bool started = false;
    bool processing = false; // a task to run the next request is queued
    bool commit_queued = false;
    std::vector<std::pair<std::uint64_t, IdbObjectStoreObject*>> store_handles; // by store id
    // An upgrade's: what the open request does once the transaction is over.
    std::function<void(Realm::Internals&, bool committed)> on_done;
    js::Object* event_parent() const override { return db; }
    void trace(js::Tracer& tracer) override;
};

js::Object* IdbRequestObject::event_parent() const
{
    return transaction;
}

// IDBObjectStore (section 4.5): a handle on a store in one transaction.
class IdbObjectStoreObject final : public js::Object {
public:
    explicit IdbObjectStoreObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    IdbTransactionObject* transaction = nullptr;
    std::uint64_t store_id = 0;
    std::u16string name;
    js::Value key_path_value; // [SameObject] for an array
    std::vector<std::pair<std::uint64_t, IdbIndexObject*>> index_handles;
    void trace(js::Tracer& tracer) override;
};

// IDBIndex (section 4.6).
class IdbIndexObject final : public js::Object {
public:
    explicit IdbIndexObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    IdbObjectStoreObject* store = nullptr;
    std::uint64_t index_id = 0;
    std::u16string name;
    js::Value key_path_value;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(store);
        tracer.visit(key_path_value);
    }
};

void IdbObjectStoreObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(transaction);
    tracer.visit(key_path_value);
    for (auto const& [id, handle] : index_handles)
        tracer.visit(handle);
}

// IDBKeyRange (section 4.7).
class IdbKeyRangeObject final : public js::Object {
public:
    IdbKeyRangeObject(js::Object* prototype, KeyRange the_range)
        : Object(prototype, Class::Host)
        , range(std::move(the_range))
    {
    }
    KeyRange range;
};

enum class Direction : std::uint8_t { Next, NextUnique, Prev, PrevUnique };

// IDBCursor and IDBCursorWithValue (section 4.8).
class IdbCursorObject final : public js::Object {
public:
    explicit IdbCursorObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    IdbTransactionObject* transaction = nullptr;
    IdbObjectStoreObject* store = nullptr; // the effective object store's handle
    IdbIndexObject* index = nullptr; // the source, when an index
    IdbRequestObject* request = nullptr;
    KeyRange range;
    Direction direction = Direction::Next;
    bool key_only = false;
    std::optional<Key> position;
    std::optional<Key> object_store_position;
    std::optional<Key> key;
    std::optional<Key> primary_key;
    bool got_value = false;
    js::Value value;
    js::Value key_value; // the key as last converted, until the cursor moves
    js::Value primary_key_value;
    js::Object* source() const
    {
        return index != nullptr ? static_cast<js::Object*>(index) : static_cast<js::Object*>(store);
    }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(transaction);
        tracer.visit(store);
        tracer.visit(index);
        tracer.visit(request);
        tracer.visit(value);
        tracer.visit(key_value);
        tracer.visit(primary_key_value);
    }
};

// IDBRecord: what getAllRecords answers with.
class IdbRecordObject final : public js::Object {
public:
    explicit IdbRecordObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    js::Value key;
    js::Value primary_key;
    js::Value value;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(key);
        tracer.visit(primary_key);
        tracer.visit(value);
    }
};

// DOMStringList (HTML section 2.7.6): a list of strings that does not change.
class StringListObject final : public js::Object {
public:
    StringListObject(js::Object* prototype, std::vector<std::u16string> the_items)
        : Object(prototype, Class::Host)
        , items(std::move(the_items))
    {
    }
    std::vector<std::u16string> items;
};

void IdbRequestObject::trace(js::Tracer& tracer)
{
    EventTargetObject::trace(tracer);
    tracer.visit(result);
    tracer.visit(error);
    tracer.visit(source);
    tracer.visit(transaction);
}

void IdbDatabaseObject::trace(js::Tracer& tracer)
{
    EventTargetObject::trace(tracer);
    tracer.visit(upgrade);
}

void trace_operation(js::Tracer& tracer, Operation const& operation)
{
    tracer.visit(operation.request);
    for (js::Value const& value : operation.held)
        tracer.visit(value);
}

void IdbTransactionObject::trace(js::Tracer& tracer)
{
    EventTargetObject::trace(tracer);
    tracer.visit(db);
    tracer.visit(error);
    tracer.visit(open_request);
    for (Operation const& operation : queue)
        trace_operation(tracer, operation);
    for (auto const& [id, handle] : store_handles)
        tracer.visit(handle);
}

template<typename T>
std::optional<T*> this_of(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* object = dynamic_cast<T*>(this_value.as_object()))
            return object;
    }
    return interp.throw_type_error("Illegal invocation");
}

// --- The realm's side ----------------------------------------------------------------------------

// How anything (a connection of another realm, a database on another
// thread) reaches this realm: a task on its loop, from this thread at once,
// from another through the agent's remote tasks. Dead once the realm ends.
struct IdbLink : std::enable_shared_from_this<IdbLink> {
    std::thread::id thread;
    Realm::Internals* internals = nullptr;
    std::shared_ptr<Agent::RemoteTasks> remote;
    std::atomic<bool> alive { true };

    void post(std::function<void(Realm::Internals&)> task)
    {
        std::shared_ptr<IdbLink> self = shared_from_this();
        auto run = [self, task = std::move(task)] {
            if (!self->alive.load())
                return;
            task(*self->internals);
        };
        if (std::this_thread::get_id() == thread) {
            if (alive.load())
                internals->post_task(std::move(run));
            return;
        }
        // The agent's wake as it stands now: the realm may have come here
        // before its agent had a thread to be woken from.
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> const lock(remote->mutex);
            remote->tasks.push_back(std::move(run));
            wake = remote->wake;
        }
        if (wake)
            wake();
    }
};

struct IdbRealm {
    std::shared_ptr<idb::Storage> storage;
    std::shared_ptr<IdbLink> link;
    bool opaque = false;
    js::Object* factory = nullptr;
    // What has work outstanding, kept alive for it.
    std::vector<IdbTransactionObject*> transactions;
    std::vector<IdbDatabaseObject*> connections;
    std::vector<IdbRequestObject*> requests; // open and delete requests
    // The same on the storage's side, for the realm's end.
    std::vector<std::shared_ptr<TransactionCore>> transaction_cores;
    std::vector<std::shared_ptr<ConnectionCore>> connection_cores;
    std::vector<std::shared_ptr<QueuedRequest>> queued;

    ~IdbRealm()
    {
        // The realm is ending: nothing here runs again. What it had not
        // committed is undone, its connections close, its requests leave the
        // queue, so that the others waiting on them go on.
        if (link)
            link->alive.store(false);
        if (!storage)
            return;
        Lock const lock(storage->mutex());
        for (auto const& core : transaction_cores) {
            if (core->finished)
                continue;
            core->finished = true;
            if (core->snapshot) {
                core->database->state = std::move(*core->snapshot);
                core->database->exists = core->existed_before;
            } else {
                core->undo.revert();
            }
            storage->finish(*core->database, *core->scheduled);
        }
        for (auto const& core : connection_cores) {
            if (!core->entry->closed)
                storage->request_close(*core->database, *core->entry);
        }
        for (auto const& request : queued) {
            if (!request->finished)
                storage->dequeue(*request->database, request->id);
        }
    }
};

namespace {

// --- Keys and values ----------------------------------------------------------------------------

std::u16string string_of(js::Value const& value)
{
    return value.as_string()->data();
}

js::Value string_value(js::Interpreter& interp, std::u16string_view text)
{
    return js::Value::string(interp.string(text));
}

enum class Outcome : std::uint8_t { Ok, Invalid, Failure, Threw };

struct Converted {
    Outcome outcome = Outcome::Ok;
    Key key;
};

bool is_detached_buffer_source(js::Object const& object)
{
    switch (object.class_id()) {
    case js::Object::Class::ArrayBuffer:
        return static_cast<js::ArrayBufferObject const&>(object).is_detached();
    case js::Object::Class::TypedArray: {
        auto const* buffer = static_cast<js::TypedArrayObject const&>(object).buffer();
        return buffer == nullptr || buffer->is_detached();
    }
    case js::Object::Class::DataView: {
        auto const* buffer = static_cast<js::DataViewObject const&>(object).buffer();
        return buffer == nullptr || buffer->is_detached();
    }
    default:
        return false;
    }
}

// Section 7.2, "convert a value to a key".
Converted to_key(Realm::Internals& in, js::Value const& input, std::vector<js::Object const*>& seen)
{
    js::Interpreter& interp = in.interpreter;
    if (input.is_number()) {
        if (std::isnan(input.as_number()))
            return { Outcome::Invalid, {} };
        return { Outcome::Ok, Key::from_number(input.as_number()) };
    }
    if (input.is_string())
        return { Outcome::Ok, Key::from_string(string_of(input)) };
    if (!input.is_object())
        return { Outcome::Invalid, {} };
    js::Object& object = *input.as_object();
    if (object.class_id() == js::Object::Class::Date) {
        double const time = static_cast<js::DateObject&>(object).time_value();
        if (std::isnan(time))
            return { Outcome::Invalid, {} };
        return { Outcome::Ok, Key::from_date(time) };
    }
    if (std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(input)) {
        if (is_detached_buffer_source(object))
            return { Outcome::Invalid, {} };
        return { Outcome::Ok, Key::from_binary(idb::Bytes(bytes->begin(), bytes->end())) };
    }
    if (!object.is_array())
        return { Outcome::Invalid, {} };
    if (std::find(seen.begin(), seen.end(), &object) != seen.end())
        return { Outcome::Invalid, {} };
    seen.push_back(&object);
    js::Interpreter::Roots const roots(interp);
    interp.root(input);
    std::optional<js::Value> const length_value = interp.get(object, interp.key("length"));
    if (!length_value)
        return { Outcome::Threw, {} };
    std::optional<double> const length = interp.to_length(*length_value);
    if (!length)
        return { Outcome::Threw, {} };
    std::vector<Key> keys;
    for (double index = 0; index < *length; ++index) {
        js::PropertyKey const property = index < 4294967295.0 ? js::PropertyKey::index(static_cast<std::uint32_t>(index))
                                                              : interp.key(std::to_string(static_cast<std::uint64_t>(index)));
        std::optional<std::optional<js::PropertyDescriptor>> const own = interp.get_own_property(object, property);
        if (!own)
            return { Outcome::Threw, {} };
        if (!*own)
            return { Outcome::Invalid, {} };
        std::optional<js::Value> const entry = interp.get(object, property);
        if (!entry)
            return { Outcome::Threw, {} };
        Converted converted = to_key(in, *entry, seen);
        if (converted.outcome != Outcome::Ok)
            return converted;
        keys.push_back(std::move(converted.key));
    }
    return { Outcome::Ok, Key::from_array(std::move(keys)) };
}

Converted to_key(Realm::Internals& in, js::Value const& input)
{
    std::vector<js::Object const*> seen;
    return to_key(in, input, seen);
}

// Section 7.2, "convert a value to a multiEntry key": an array's valid keys,
// each once.
Converted to_multi_entry_key(Realm::Internals& in, js::Value const& input)
{
    js::Interpreter& interp = in.interpreter;
    if (!input.is_object() || !input.as_object()->is_array())
        return to_key(in, input);
    js::Object& object = *input.as_object();
    js::Interpreter::Roots const roots(interp);
    interp.root(input);
    std::vector<js::Object const*> seen { &object };
    std::optional<js::Value> const length_value = interp.get(object, interp.key("length"));
    if (!length_value)
        return { Outcome::Threw, {} };
    std::optional<double> const length = interp.to_length(*length_value);
    if (!length)
        return { Outcome::Threw, {} };
    std::vector<Key> keys;
    for (double index = 0; index < *length && index < 4294967295.0; ++index) {
        js::PropertyKey const property = js::PropertyKey::index(static_cast<std::uint32_t>(index));
        std::optional<std::optional<js::PropertyDescriptor>> const own = interp.get_own_property(object, property);
        if (!own)
            return { Outcome::Threw, {} };
        if (!*own)
            continue;
        std::optional<js::Value> const entry = interp.get(object, property);
        if (!entry)
            return { Outcome::Threw, {} };
        Converted converted = to_key(in, *entry, seen);
        if (converted.outcome == Outcome::Threw)
            return converted;
        if (converted.outcome != Outcome::Ok)
            continue;
        bool const known = std::any_of(keys.begin(), keys.end(), [&](Key const& key) { return idb::keys_equal(key, converted.key); });
        if (!known)
            keys.push_back(std::move(converted.key));
    }
    return { Outcome::Ok, Key::from_array(std::move(keys)) };
}

// Section 7.2, "convert a key to a value". The caller roots the result.
js::Value key_value(Realm::Internals& in, Key const& key)
{
    js::Interpreter& interp = in.interpreter;
    js::Intrinsics const& intrinsics = in.realm_record->intrinsics;
    switch (key.type) {
    case Key::Type::Number:
        return js::Value::number(key.number);
    case Key::Type::Date:
        return js::Value::object(interp.heap().allocate<js::DateObject>(intrinsics.date_prototype, key.number));
    case Key::Type::String:
        return string_value(interp, key.string);
    case Key::Type::Binary: {
        auto* buffer = interp.heap().allocate<js::ArrayBufferObject>(intrinsics.array_buffer_prototype, key.binary.size(), std::nullopt);
        if (!key.binary.empty())
            std::memcpy(buffer->data(), key.binary.data(), key.binary.size());
        return js::Value::object(buffer);
    }
    case Key::Type::Array: {
        js::Interpreter::Roots const roots(interp);
        std::vector<js::Value> items;
        items.reserve(key.array.size());
        for (Key const& item : key.array)
            items.push_back(interp.root(key_value(in, item)));
        return js::Value::object(interp.new_array(items));
    }
    }
    return js::Value::undefined();
}

// --- Key paths ------------------------------------------------------------------------------------

std::vector<std::u16string> identifiers_of(std::u16string const& path)
{
    std::vector<std::u16string> out;
    std::size_t start = 0;
    while (true) {
        std::size_t const dot = path.find(u'.', start);
        out.push_back(path.substr(start, dot == std::u16string::npos ? std::u16string::npos : dot - start));
        if (dot == std::u16string::npos)
            return out;
        start = dot + 1;
    }
}

struct Evaluated {
    Outcome outcome = Outcome::Ok;
    js::Value value;
};

// Section 7.5, "evaluate a key path on a value". The caller roots the result.
Evaluated evaluate_path(Realm::Internals& in, js::Value const& start, std::u16string const& path)
{
    js::Interpreter& interp = in.interpreter;
    if (path.empty())
        return { Outcome::Ok, start };
    js::Interpreter::Roots const roots(interp);
    js::Value& value = interp.root(start);
    for (std::u16string const& identifier : identifiers_of(path)) {
        if (value.is_string() && identifier == u"length") {
            value = js::Value::number(static_cast<double>(value.as_string()->length()));
            continue;
        }
        if (!value.is_object())
            return { Outcome::Failure, {} };
        js::Object& object = *value.as_object();
        if (auto* blob = dynamic_cast<BlobObject*>(&object)) {
            if (identifier == u"size") {
                value = js::Value::number(static_cast<double>(blob->bytes.size()));
                continue;
            }
            if (identifier == u"type") {
                value = in.string(blob->type);
                continue;
            }
            if (blob->is_file && identifier == u"name") {
                value = in.string(blob->name);
                continue;
            }
            if (blob->is_file && identifier == u"lastModified") {
                value = js::Value::number(blob->last_modified);
                continue;
            }
        }
        js::PropertyKey const property = interp.key(identifier);
        if (object.is_array() && identifier == u"length") {
            std::optional<js::Value> const length = interp.get(object, property);
            if (!length)
                return { Outcome::Threw, {} };
            std::optional<double> const count = interp.to_length(*length);
            if (!count)
                return { Outcome::Threw, {} };
            value = js::Value::number(*count);
            continue;
        }
        std::optional<std::optional<js::PropertyDescriptor>> const own = interp.get_own_property(object, property);
        if (!own)
            return { Outcome::Threw, {} };
        if (!*own)
            return { Outcome::Failure, {} };
        std::optional<js::Value> const next = interp.get(object, property);
        if (!next)
            return { Outcome::Threw, {} };
        value = *next;
    }
    return { Outcome::Ok, value };
}

Evaluated evaluate_key_path(Realm::Internals& in, js::Value const& value, KeyPath const& path)
{
    js::Interpreter& interp = in.interpreter;
    if (path.kind != KeyPath::Kind::Array)
        return evaluate_path(in, value, path.paths.empty() ? std::u16string() : path.paths.front());
    js::Interpreter::Roots const roots(interp);
    std::vector<js::Value> items;
    for (std::u16string const& item : path.paths) {
        Evaluated evaluated = evaluate_path(in, value, item);
        if (evaluated.outcome != Outcome::Ok)
            return evaluated;
        items.push_back(interp.root(evaluated.value));
    }
    return { Outcome::Ok, js::Value::object(interp.new_array(items)) };
}

// Section 7.6, "extract a key from a value using a key path".
Converted extract_key(Realm::Internals& in, js::Value const& value, KeyPath const& path, bool multi_entry = false)
{
    js::Interpreter::Roots const roots(in.interpreter);
    Evaluated const evaluated = evaluate_key_path(in, value, path);
    if (evaluated.outcome != Outcome::Ok)
        return { evaluated.outcome, {} };
    in.interpreter.root(evaluated.value);
    return multi_entry ? to_multi_entry_key(in, evaluated.value) : to_key(in, evaluated.value);
}

// Section 7.6, "check that a key could be injected into a value".
bool can_inject(Realm::Internals& in, js::Value const& start, std::u16string const& path)
{
    js::Interpreter& interp = in.interpreter;
    std::vector<std::u16string> identifiers = identifiers_of(path);
    identifiers.pop_back();
    js::Interpreter::Roots const roots(interp);
    js::Value& value = interp.root(start);
    for (std::u16string const& identifier : identifiers) {
        if (!value.is_object())
            return false;
        js::PropertyKey const property = interp.key(identifier);
        std::optional<std::optional<js::PropertyDescriptor>> const own = interp.get_own_property(*value.as_object(), property);
        if (!own || !*own)
            return own.has_value();
        std::optional<js::Value> const next = interp.get(*value.as_object(), property);
        if (!next)
            return false;
        value = *next;
    }
    return value.is_object();
}

// Section 7.6, "inject a key into a value using a key path".
void inject_key(Realm::Internals& in, js::Value const& start, std::u16string const& path, Key const& key)
{
    js::Interpreter& interp = in.interpreter;
    std::vector<std::u16string> identifiers = identifiers_of(path);
    std::u16string const last = identifiers.back();
    identifiers.pop_back();
    js::Interpreter::Roots const roots(interp);
    js::Value& value = interp.root(start);
    for (std::u16string const& identifier : identifiers) {
        js::PropertyKey const property = interp.key(identifier);
        std::optional<std::optional<js::PropertyDescriptor>> const own = interp.get_own_property(*value.as_object(), property);
        if (!own)
            return;
        if (!*own) {
            js::Object* const made = interp.new_object();
            interp.root(js::Value::object(made));
            (void)interp.create_data_property(*value.as_object(), property, js::Value::object(made), false);
            value = js::Value::object(made);
            continue;
        }
        std::optional<js::Value> const next = interp.get(*value.as_object(), property);
        if (!next || !next->is_object())
            return;
        value = *next;
    }
    js::Value const made = interp.root(key_value(in, key));
    (void)interp.create_data_property(*value.as_object(), interp.key(last), made, false);
}

// A (DOMString or sequence<DOMString>) key path from script: nullopt with
// the exception pending.
std::optional<KeyPath> key_path_from(Realm::Internals& in, js::Value const& value)
{
    js::Interpreter& interp = in.interpreter;
    KeyPath path;
    if (value.is_object()) {
        std::optional<js::Value> const method = interp.get_method(value, js::PropertyKey::symbol(interp.atoms().symbol_iterator));
        if (!method)
            return std::nullopt;
        if (!method->is_undefined()) {
            std::optional<std::vector<js::Value>> const items = interp.iterable_to_list(value);
            if (!items)
                return std::nullopt;
            js::Interpreter::Roots const roots(interp);
            for (js::Value const& item : *items)
                interp.root(item);
            path.kind = KeyPath::Kind::Array;
            for (js::Value const& item : *items) {
                std::optional<js::JsString*> const text = interp.to_string(item);
                if (!text)
                    return std::nullopt;
                path.paths.push_back((*text)->data());
            }
            return path;
        }
    }
    std::optional<js::JsString*> const text = interp.to_string(value);
    if (!text)
        return std::nullopt;
    path.kind = KeyPath::Kind::String;
    path.paths.push_back((*text)->data());
    return path;
}

bool key_path_valid(KeyPath const& path)
{
    if (path.kind == KeyPath::Kind::Array && path.paths.empty())
        return false;
    return std::all_of(path.paths.begin(), path.paths.end(), [](std::u16string const& item) { return idb::is_valid_key_path_string(item); });
}

js::Value key_path_value(Realm::Internals& in, KeyPath const& path)
{
    js::Interpreter& interp = in.interpreter;
    switch (path.kind) {
    case KeyPath::Kind::None:
        return js::Value::null();
    case KeyPath::Kind::String:
        return string_value(interp, path.paths.front());
    case KeyPath::Kind::Array: {
        js::Interpreter::Roots const roots(interp);
        std::vector<js::Value> items;
        for (std::u16string const& item : path.paths)
            items.push_back(interp.root(string_value(interp, item)));
        return js::Value::object(interp.new_array(items));
    }
    }
    return js::Value::null();
}

js::Value string_list(Realm::Internals& in, std::vector<std::u16string> items)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    auto* list = interp.heap().allocate<StringListObject>(in.prototype("DOMStringList"), items);
    interp.root(js::Value::object(list));
    for (std::size_t i = 0; i < items.size(); ++i)
        list->put(js::PropertyKey::index(static_cast<std::uint32_t>(i)), string_value(interp, items[i]), js::Enumerable);
    return js::Value::object(list);
}

// --- The realm's state ------------------------------------------------------------------------------

IdbRealm& realm_state(Realm::Internals& in)
{
    if (in.indexed_db)
        return *in.indexed_db;
    auto state = std::make_shared<IdbRealm>();
    auto link = std::make_shared<IdbLink>();
    link->thread = std::this_thread::get_id();
    link->internals = &in;
    link->remote = in.agent.remote_tasks;
    state->link = link;
    // Local files share one storage, as the browsers give them; an opaque
    // origin has none (a SecurityError at every door).
    std::string const origin = in.origin_url.scheme == "file" ? std::string("file://") : in.origin_url.serialize_origin();
    state->opaque = origin == "null" || (in.sandbox_flags & sandboxing::origin) != 0;
    if (!state->opaque) {
        if (in.hooks.indexed_db)
            state->storage = in.hooks.indexed_db(origin);
        if (!state->storage) {
            std::shared_ptr<idb::Storage>& kept = in.agent.indexed_db_storages[origin];
            if (!kept)
                kept = std::make_shared<idb::Storage>();
            state->storage = kept;
        }
    }
    in.indexed_db = state;
    return *state;
}

idb::UndoLog* undo_of(IdbTransactionObject& tx)
{
    return tx.mode == idb::Mode::ReadWrite ? &tx.core->undo : nullptr;
}

idb::StoreState* store_state(IdbObjectStoreObject const& handle)
{
    return handle.transaction->core->database->state.store(handle.store_id);
}

idb::IndexState* index_state(IdbIndexObject const& handle)
{
    idb::StoreState* const store = store_state(*handle.store);
    if (store == nullptr)
        return nullptr;
    auto const found = store->indexes.find(handle.index_id);
    return found == store->indexes.end() ? nullptr : &found->second;
}

js::Value error_value(Realm::Internals& in, std::string_view name, std::string_view message)
{
    return dom_exception_value(in, name, message);
}

// A record's value made again in the realm: rooted for the caller's scope.
js::Value deserialize_value(Realm::Internals& in, idb::Bytes const& bytes)
{
    std::shared_ptr<SerializedMessage const> const message = serialized_from_bytes(bytes);
    if (!message)
        return js::Value::undefined();
    std::optional<Deserialized> const made = structured_deserialize(in, *message);
    if (!made) {
        in.interpreter.clear_exception();
        return js::Value::undefined();
    }
    return in.interpreter.root(made->value);
}

// --- Events ------------------------------------------------------------------------------------------

struct Fired {
    bool allowed = true;
    bool threw = false;
};

// Dispatches a trusted event, the transaction active while the listeners
// (and the microtasks they leave) run, inactive after.
Fired fire(Realm::Internals& in, js::Object& target, EventObject& event, IdbTransactionObject* tx)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&event));
    in.interpreter.root(js::Value::object(&target));
    if (tx != nullptr)
        in.interpreter.root(js::Value::object(tx));
    event.is_trusted = true;
    int const before = in.stats.uncaught_errors;
    Fired fired;
    {
        // No entry of its own around the dispatch: each listener is one, so
        // that the microtasks it queued run before the next listener does.
        js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
        if (tx != nullptr && tx->state == IdbTransactionObject::State::Inactive)
            tx->state = IdbTransactionObject::State::Active;
        fired.allowed = in.dispatch(event, &target);
    }
    if (tx != nullptr && tx->state == IdbTransactionObject::State::Active)
        tx->state = IdbTransactionObject::State::Inactive;
    fired.threw = in.stats.uncaught_errors != before;
    return fired;
}

Fired fire_simple(Realm::Internals& in, js::Object& target, std::string_view type, bool bubbles, bool cancelable,
    IdbTransactionObject* tx = nullptr)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&target));
    EventObject* const event = in.new_event("Event", type, bubbles, cancelable);
    return fire(in, target, *event, tx);
}

Fired fire_version_change(Realm::Internals& in, js::Object& target, std::string_view type, std::uint64_t old_version,
    std::optional<std::uint64_t> new_version, IdbTransactionObject* tx = nullptr)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&target));
    EventObject* const event = in.new_event("IDBVersionChangeEvent", type, false, false);
    event->old_version = static_cast<double>(old_version);
    if (new_version)
        event->new_version = static_cast<double>(*new_version);
    return fire(in, target, *event, tx);
}

// --- Transactions -------------------------------------------------------------------------------------

void pump(Realm::Internals& in, IdbTransactionObject& tx);
void abort_transaction(Realm::Internals& in, IdbTransactionObject& tx, js::Value const& error);

IdbTransactionObject* find_transaction(IdbRealm& state, std::uint64_t serial)
{
    for (IdbTransactionObject* tx : state.transactions) {
        if (tx->core->scheduled->serial == serial)
            return tx;
    }
    return nullptr;
}

// A task of the realm's that finds the transaction again by its serial:
// it may have finished and gone meanwhile.
void post_to_transaction(Realm::Internals& in, IdbTransactionObject& tx, std::function<void(Realm::Internals&, IdbTransactionObject&)> step)
{
    std::uint64_t const serial = tx.core->scheduled->serial;
    realm_state(in).link->post([serial, step = std::move(step)](Realm::Internals& here) {
        IdbTransactionObject* const found = find_transaction(realm_state(here), serial);
        if (found == nullptr)
            return;
        js::Interpreter::RealmScope const inside(here.interpreter, here.realm_record);
        step(here, *found);
    });
}

void forget_transaction(Realm::Internals& in, IdbTransactionObject& tx)
{
    IdbRealm& state = realm_state(in);
    std::erase(state.transactions, &tx);
    std::erase(state.transaction_cores, tx.core);
}

// Runs one operation: its result or its error goes on its request, which
// is not done until its event.
void execute_operation(Realm::Internals& in, IdbTransactionObject& tx, Operation& operation)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(operation.request));
    IdbRequestObject& request = *operation.request;
    request.error = js::Value::null();
    request.result = js::Value::undefined();
    tx.executing = &operation;
    operation.run(in, tx, request);
    tx.executing = nullptr;
    operation.executed = true;
    if (interp.has_exception())
        interp.clear_exception();
}

// Runs every operation of a started transaction that may run now: in order,
// up to the first that waits for its turn.
void execute_ready(Realm::Internals& in, IdbTransactionObject& tx)
{
    if (!tx.started || tx.state == IdbTransactionObject::State::Finished || tx.executing != nullptr)
        return;
    for (Operation& operation : tx.queue) {
        if (operation.executed)
            continue;
        if (!operation.eager)
            return;
        execute_operation(in, tx, operation);
    }
}

void process_next(Realm::Internals& in, IdbTransactionObject& tx)
{
    tx.processing = false;
    if (tx.state == IdbTransactionObject::State::Finished || tx.queue.empty()) {
        pump(in, tx);
        return;
    }
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&tx));
    Operation& operation = tx.queue.front();
    if (operation.request == nullptr) {
        // A step of the transaction's own in the queue: an abort decided
        // after the requests before it.
        auto const step = std::move(operation.control);
        tx.queue.pop_front();
        if (step)
            step(in, tx);
        pump(in, tx);
        return;
    }
    if (!operation.executed)
        execute_operation(in, tx, operation);
    IdbRequestObject& request = *operation.request;
    interp.root(js::Value::object(&request));
    tx.queue.pop_front();
    request.done = true;
    bool const committing = tx.state == IdbTransactionObject::State::Committing;
    if (!request.error.is_null()) {
        // Section 5.7, "fire an error event": the transaction aborts with the
        // request's error unless a listener prevented it.
        request.result = js::Value::undefined();
        Fired const fired = fire_simple(in, request, "error", true, true, &tx);
        if (tx.state != IdbTransactionObject::State::Finished) {
            if (fired.threw && !committing)
                abort_transaction(in, tx, error_value(in, "AbortError", "An error event listener threw an exception."));
            else if (fired.allowed)
                abort_transaction(in, tx, request.error);
        }
    } else {
        // An exception after an explicit commit does not stop the commit.
        Fired const fired = fire_simple(in, request, "success", false, false, &tx);
        if (fired.threw && !committing && tx.state != IdbTransactionObject::State::Finished)
            abort_transaction(in, tx, error_value(in, "AbortError", "A success event listener threw an exception."));
    }
    pump(in, tx);
}

void finish_commit(Realm::Internals& in, IdbTransactionObject& tx)
{
    if (tx.state == IdbTransactionObject::State::Finished)
        return;
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&tx));
    IdbRealm& state = realm_state(in);
    {
        Lock const lock(state.storage->mutex());
        if (tx.mode != idb::Mode::ReadOnly)
            state.storage->persist(*tx.core->database);
        tx.core->undo.clear();
        tx.core->snapshot.reset();
        tx.core->finished = true;
        state.storage->finish(*tx.core->database, *tx.core->scheduled);
        if (tx.mode == idb::Mode::VersionChange)
            tx.scope = tx.core->database->state.store_names(); // what objectStoreNames keeps showing
    }
    tx.state = IdbTransactionObject::State::Finished;
    if (tx.db != nullptr && tx.db->upgrade == &tx)
        tx.db->upgrade = nullptr;
    forget_transaction(in, tx);
    fire_simple(in, tx, "complete", false, false);
    if (tx.on_done)
        tx.on_done(in, true);
}

// Section 5.3, "commit a transaction": once nothing is left to run.
void commit_transaction(Realm::Internals& in, IdbTransactionObject& tx)
{
    if (tx.state == IdbTransactionObject::State::Finished)
        return;
    tx.state = IdbTransactionObject::State::Committing;
    if (!tx.started || !tx.queue.empty() || tx.processing || tx.executing != nullptr || tx.commit_queued)
        return;
    tx.commit_queued = true;
    post_to_transaction(in, tx, [](Realm::Internals& here, IdbTransactionObject& found) { finish_commit(here, found); });
}

void pump(Realm::Internals& in, IdbTransactionObject& tx)
{
    if (tx.state == IdbTransactionObject::State::Finished || !tx.started || tx.processing || tx.executing != nullptr)
        return;
    if (!tx.queue.empty()) {
        tx.processing = true;
        post_to_transaction(in, tx, [](Realm::Internals& here, IdbTransactionObject& found) { process_next(here, found); });
        return;
    }
    // Section 2.7.1: a transaction with nothing left to do commits itself
    // once it is inactive.
    if (tx.state == IdbTransactionObject::State::Inactive || tx.state == IdbTransactionObject::State::Committing)
        commit_transaction(in, tx);
}

// Section 5.4, "abort a transaction".
void abort_transaction(Realm::Internals& in, IdbTransactionObject& tx, js::Value const& error)
{
    if (tx.state == IdbTransactionObject::State::Finished)
        return;
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&tx));
    interp.root(error);
    IdbRealm& state = realm_state(in);
    bool const upgrade = tx.mode == idb::Mode::VersionChange;
    {
        Lock const lock(state.storage->mutex());
        if (tx.core->snapshot) {
            tx.core->database->state = std::move(*tx.core->snapshot);
            tx.core->database->exists = tx.core->existed_before;
            tx.core->snapshot.reset();
        } else {
            tx.core->undo.revert();
        }
        tx.core->finished = true;
        state.storage->finish(*tx.core->database, *tx.core->scheduled);
    }
    if (upgrade && tx.db != nullptr) {
        tx.db->version = tx.core->existed_before ? tx.old_version : 0;
        tx.db->core->entry->version = tx.db->version;
    }
    if (upgrade) {
        // The handles show the names the stores and indexes have again
        // (section 5.4, "abort an upgrade transaction").
        tx.scope = tx.core->database->state.store_names();
        for (auto const& [store_id, handle] : tx.store_handles) {
            idb::StoreState* const store = tx.core->database->state.store(store_id);
            if (store == nullptr)
                continue;
            handle->name = store->name;
            for (auto const& [index_id, index] : handle->index_handles) {
                auto const found = store->indexes.find(index_id);
                if (found != store->indexes.end())
                    index->name = found->second.name;
            }
        }
    }
    tx.state = IdbTransactionObject::State::Finished;
    if (!error.is_null())
        tx.error = error;
    // Every request not yet run fails with an AbortError, each in a task of
    // its own, before the transaction's abort event.
    std::vector<IdbRequestObject*> pending;
    for (Operation const& operation : tx.queue) {
        if (operation.request != nullptr)
            pending.push_back(operation.request);
    }
    tx.queue.clear();
    for (IdbRequestObject* request : pending) {
        request->done = true;
        request->result = js::Value::undefined();
        request->error = error_value(in, "AbortError", "The transaction was aborted.");
    }
    std::uint64_t const serial = tx.core->scheduled->serial;
    auto held = std::make_shared<std::vector<IdbRequestObject*>>(std::move(pending));
    // The requests and the transaction stay on the realm's list, traced,
    // until the abort event has been fired.
    for (IdbRequestObject* request : *held) {
        Operation keep;
        keep.request = request;
        tx.queue.push_back(std::move(keep));
    }
    realm_state(in).link->post([serial, held](Realm::Internals& here) {
        IdbTransactionObject* const found = find_transaction(realm_state(here), serial);
        if (found == nullptr)
            return;
        js::Interpreter::RealmScope const inside(here.interpreter, here.realm_record);
        js::Interpreter::Roots const inner(here.interpreter);
        here.interpreter.root(js::Value::object(found));
        found->queue.clear();
        for (IdbRequestObject* request : *held) {
            here.interpreter.root(js::Value::object(request));
            fire_simple(here, *request, "error", true, true);
        }
        if (found->db != nullptr && found->db->upgrade == found)
            found->db->upgrade = nullptr;
        forget_transaction(here, *found);
        fire_simple(here, *found, "abort", true, false);
        if (found->on_done)
            found->on_done(here, false);
    });
}

// A transaction made in a script becomes inactive when the script and its
// microtasks are over.
void arrange_deactivation(Realm::Internals& in, IdbTransactionObject& tx)
{
    std::uint64_t const serial = tx.core->scheduled->serial;
    std::shared_ptr<IdbLink> const link = realm_state(in).link;
    auto deactivate = [link, serial] {
        if (!link->alive.load())
            return;
        Realm::Internals& here = *link->internals;
        IdbTransactionObject* const found = find_transaction(realm_state(here), serial);
        if (found == nullptr || found->state != IdbTransactionObject::State::Active)
            return;
        found->state = IdbTransactionObject::State::Inactive;
        pump(here, *found);
    };
    if (in.agent.script_depth > 0)
        in.agent.after_script.push_back(std::move(deactivate));
    else
        in.post_task(std::move(deactivate));
}

IdbTransactionObject* new_transaction(Realm::Internals& in, IdbDatabaseObject& db, idb::Mode mode, std::vector<std::u16string> scope)
{
    js::Interpreter& interp = in.interpreter;
    IdbRealm& state = realm_state(in);
    auto* tx = interp.heap().allocate<IdbTransactionObject>(in.prototype("IDBTransaction"));
    tx->db = &db;
    tx->mode = mode;
    tx->scope = std::move(scope);
    tx->core = std::make_shared<TransactionCore>();
    tx->core->database = db.core->database;
    tx->core->scheduled = std::make_shared<idb::ScheduledTransaction>();
    tx->core->scheduled->mode = mode;
    tx->core->scheduled->scope = tx->scope;
    tx->core->scheduled->connection = db.core->entry->id;
    {
        Lock const lock(state.storage->mutex());
        tx->core->scheduled->serial = state.storage->next_serial();
    }
    state.transactions.push_back(tx);
    state.transaction_cores.push_back(tx->core);
    return tx;
}

// Places a request's operation at the end of the transaction's queue.
IdbRequestObject* place_request(Realm::Internals& in, IdbTransactionObject& tx, js::Value const& source,
    std::function<void(Realm::Internals&, IdbTransactionObject&, IdbRequestObject&)> run, std::vector<js::Value> held = {},
    IdbRequestObject* reuse = nullptr, bool eager = true)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(source);
    for (js::Value const& value : held)
        interp.root(value);
    IdbRequestObject* request = reuse;
    if (request == nullptr) {
        request = interp.heap().allocate<IdbRequestObject>(in.prototype("IDBRequest"));
        request->source = source;
        request->transaction = &tx;
    }
    interp.root(js::Value::object(request));
    Operation operation;
    operation.request = request;
    operation.run = std::move(run);
    operation.held = std::move(held);
    operation.eager = eager;
    tx.queue.push_back(std::move(operation));
    execute_ready(in, tx);
    pump(in, tx);
    return request;
}

// --- Connections and the open and delete requests ---------------------------------------------------

void prune_connections(IdbRealm& state)
{
    std::erase_if(state.connections, [](IdbDatabaseObject* db) { return db->core->entry->closed; });
    std::erase_if(state.connection_cores, [](std::shared_ptr<ConnectionCore> const& core) { return core->entry->closed; });
}

IdbDatabaseObject* find_connection(IdbRealm& state, std::uint64_t id)
{
    for (IdbDatabaseObject* db : state.connections) {
        if (db->core->entry->id == id)
            return db;
    }
    return nullptr;
}

IdbDatabaseObject* new_connection(Realm::Internals& in, std::shared_ptr<ConnectionCore> core, std::u16string const& name)
{
    IdbRealm& state = realm_state(in);
    prune_connections(state);
    auto* db = in.interpreter.heap().allocate<IdbDatabaseObject>(in.prototype("IDBDatabase"));
    db->core = core;
    db->name = name;
    db->version = core->entry->version;
    state.connections.push_back(db);
    std::uint64_t const id = core->entry->id;
    std::shared_ptr<IdbLink> const link = state.link;
    core->entry->on_versionchange = [link, id](std::uint64_t old_version, std::optional<std::uint64_t> new_version) {
        link->post([id, old_version, new_version](Realm::Internals& here) {
            IdbDatabaseObject* const found = find_connection(realm_state(here), id);
            if (found == nullptr || found->close_pending())
                return;
            js::Interpreter::RealmScope const inside(here.interpreter, here.realm_record);
            fire_version_change(here, *found, "versionchange", old_version, new_version);
        });
    };
    return db;
}

void close_connection(Realm::Internals& in, IdbDatabaseObject& db)
{
    IdbRealm& state = realm_state(in);
    Lock const lock(state.storage->mutex());
    state.storage->request_close(*db.core->database, *db.core->entry);
}

void finish_queued(Realm::Internals& in, IdbRequestObject& request, std::shared_ptr<QueuedRequest> const& queued)
{
    IdbRealm& state = realm_state(in);
    {
        Lock const lock(state.storage->mutex());
        if (!queued->finished) {
            queued->finished = true;
            state.storage->dequeue(*queued->database, queued->id);
        }
    }
    std::erase(state.queued, queued);
    std::erase(state.requests, &request);
}

// Fires the open or delete request's last event, then lets the next in the
// queue go.
void settle_request(Realm::Internals& in, IdbRequestObject& request, std::shared_ptr<QueuedRequest> const& queued,
    std::optional<std::pair<std::uint64_t, std::optional<std::uint64_t>>> versions = std::nullopt)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&request));
    request.done = true;
    // Out of the queue before the event, as the next request may be made in
    // its listener.
    finish_queued(in, request, queued);
    if (!request.error.is_null()) {
        request.result = js::Value::undefined();
        fire_simple(in, request, "error", true, true);
    } else if (versions) {
        fire_version_change(in, request, "success", versions->first, versions->second);
    } else {
        fire_simple(in, request, "success", false, false);
    }
}

// Runs `step` once every connection to the database but `except` has closed:
// at once when none is open; else after a blocked event, when the last
// closes.
void when_others_closed(Realm::Internals& in, IdbRequestObject& request, std::shared_ptr<QueuedRequest> const& queued,
    std::uint64_t except, std::uint64_t old_version, std::optional<std::uint64_t> new_version,
    std::function<void(Realm::Internals&, IdbRequestObject&)> step)
{
    IdbRealm& state = realm_state(in);
    std::shared_ptr<IdbLink> const link = state.link;
    auto shared_step = std::make_shared<std::function<void(Realm::Internals&, IdbRequestObject&)>>(std::move(step));
    IdbRequestObject* const target = &request;
    // After the versionchange events queued before this task.
    link->post([link, queued, except, old_version, new_version, shared_step, target](Realm::Internals& here) {
        IdbRealm& here_state = realm_state(here);
        if (std::find(here_state.requests.begin(), here_state.requests.end(), target) == here_state.requests.end())
            return;
        js::Interpreter::RealmScope const inside(here.interpreter, here.realm_record);
        bool open = false;
        {
            Lock const lock(here_state.storage->mutex());
            open = here_state.storage->others_open(*queued->database, except);
            if (open) {
                auto ran = std::make_shared<bool>(false);
                queued->database->close_watchers[queued->id] = [link, queued, except, shared_step, target, ran] {
                    link->post([queued, except, shared_step, target, ran](Realm::Internals& there) {
                        IdbRealm& there_state = realm_state(there);
                        if (*ran || std::find(there_state.requests.begin(), there_state.requests.end(), target) == there_state.requests.end())
                            return;
                        {
                            Lock const inner(there_state.storage->mutex());
                            if (there_state.storage->others_open(*queued->database, except))
                                return;
                            queued->database->close_watchers.erase(queued->id);
                        }
                        *ran = true;
                        js::Interpreter::RealmScope const scope(there.interpreter, there.realm_record);
                        (*shared_step)(there, *target);
                    });
                };
            }
        }
        if (!open) {
            (*shared_step)(here, *target);
            return;
        }
        fire_version_change(here, *target, "blocked", old_version, new_version);
    });
}

void run_upgrade(Realm::Internals& in, IdbRequestObject& request, std::shared_ptr<QueuedRequest> const& queued,
    std::shared_ptr<ConnectionCore> const& connection, std::uint64_t version)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&request));
    IdbRealm& state = realm_state(in);
    IdbDatabaseObject* const db = find_connection(state, connection->entry->id);
    if (db == nullptr)
        return;
    interp.root(js::Value::object(db));
    IdbTransactionObject* const tx = new_transaction(in, *db, idb::Mode::VersionChange, {});
    interp.root(js::Value::object(tx));
    std::uint64_t old_version = 0;
    {
        Lock const lock(state.storage->mutex());
        idb::Storage::Database& database = *queued->database;
        old_version = database.exists ? database.state.version : 0;
        tx->core->existed_before = database.exists;
        tx->core->snapshot = database.state;
        database.exists = true;
        database.state.version = version;
        state.storage->schedule(database, tx->core->scheduled);
    }
    tx->started = true;
    tx->old_version = old_version;
    tx->open_request = &request;
    tx->state = IdbTransactionObject::State::Inactive;
    db->version = version;
    db->upgrade = tx;
    connection->entry->version = version;
    request.result = js::Value::object(db);
    request.transaction = tx;
    request.done = true;
    IdbRequestObject* const open_request = &request;
    tx->on_done = [queued, connection, open_request](Realm::Internals& here, bool committed) {
        IdbRealm& here_state = realm_state(here);
        // Kept alive by the realm's list for as long as it is on it.
        if (std::find(here_state.requests.begin(), here_state.requests.end(), open_request) == here_state.requests.end())
            return;
        IdbRequestObject* const open = open_request;
        js::Interpreter::Roots const inner(here.interpreter);
        here.interpreter.root(js::Value::object(open));
        open->transaction = nullptr;
        if (!committed || connection->entry->close_pending) {
            open->error = error_value(here, "AbortError", committed ? "The connection was closed before the upgrade was over." : "The upgrade transaction was aborted.");
            if (!committed) {
                Lock const lock(here_state.storage->mutex());
                here_state.storage->request_close(*connection->database, *connection->entry);
            }
        }
        settle_request(here, *open, queued);
    };
    Fired const fired = fire_version_change(in, request, "upgradeneeded", old_version, version, tx);
    if (fired.threw && tx->state != IdbTransactionObject::State::Finished)
        abort_transaction(in, *tx, error_value(in, "AbortError", "An upgradeneeded event listener threw an exception."));
    pump(in, *tx);
}

// Section 5.1, "open a database connection", once the request is at the
// front of the queue.
void run_open(Realm::Internals& in, IdbRequestObject& request, std::shared_ptr<QueuedRequest> const& queued,
    std::optional<std::uint64_t> requested)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&request));
    IdbRealm& state = realm_state(in);
    std::uint64_t old_version = 0;
    std::uint64_t version = 0;
    std::shared_ptr<ConnectionCore> connection;
    {
        Lock const lock(state.storage->mutex());
        idb::Storage::Database& database = *queued->database;
        old_version = database.exists ? database.state.version : 0;
        version = requested ? *requested : (database.exists ? database.state.version : 1);
        if (old_version <= version) {
            connection = std::make_shared<ConnectionCore>();
            connection->database = &database;
            connection->entry = state.storage->connect(database, old_version);
            state.connection_cores.push_back(connection);
        }
    }
    if (!connection) {
        request.error = error_value(in, "VersionError", "The requested version is less than the existing version.");
        settle_request(in, request, queued);
        return;
    }
    IdbDatabaseObject* const db = new_connection(in, connection, queued->name);
    interp.root(js::Value::object(db));
    if (old_version == version) {
        request.result = js::Value::object(db);
        settle_request(in, request, queued);
        return;
    }
    // Every other connection is told, and the upgrade waits for them all.
    {
        Lock const lock(state.storage->mutex());
        for (auto const& other : queued->database->connections) {
            if (other != connection->entry && !other->close_pending && other->on_versionchange)
                other->on_versionchange(old_version, version);
        }
    }
    when_others_closed(in, request, queued, connection->entry->id, old_version, version,
        [queued, connection, version](Realm::Internals& here, IdbRequestObject& target) {
            if (connection->entry->close_pending) {
                target.error = error_value(here, "AbortError", "The connection was closed.");
                settle_request(here, target, queued);
                return;
            }
            run_upgrade(here, target, queued, connection, version);
        });
}

// Section 5.2, "delete a database".
void run_delete(Realm::Internals& in, IdbRequestObject& request, std::shared_ptr<QueuedRequest> const& queued)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&request));
    IdbRealm& state = realm_state(in);
    std::uint64_t old_version = 0;
    bool exists = false;
    {
        Lock const lock(state.storage->mutex());
        idb::Storage::Database& database = *queued->database;
        exists = database.exists;
        old_version = exists ? database.state.version : 0;
        if (exists) {
            for (auto const& other : database.connections) {
                if (!other->close_pending && other->on_versionchange)
                    other->on_versionchange(old_version, std::nullopt);
            }
        }
    }
    if (!exists) {
        settle_request(in, request, queued, std::pair { std::uint64_t(0), std::optional<std::uint64_t>() });
        return;
    }
    when_others_closed(in, request, queued, 0, old_version, std::nullopt,
        [queued, old_version](Realm::Internals& here, IdbRequestObject& target) {
            IdbRealm& here_state = realm_state(here);
            {
                Lock const lock(here_state.storage->mutex());
                here_state.storage->erase(*queued->database);
            }
            settle_request(here, target, queued, std::pair { old_version, std::optional<std::uint64_t>() });
        });
}

// --- Walking the records ------------------------------------------------------------------------------

struct Step {
    Key key; // the record's key, or its index key
    Key primary; // the record's key
};

bool is_next(Direction direction)
{
    return direction == Direction::Next || direction == Direction::NextUnique;
}

// The record of a store a cursor goes to next (section 7.4.3, "iterate a
// cursor"): past `position`, at or past `target`, inside the range.
std::optional<Step> step_store(idb::StoreState& store, Direction direction, KeyRange const& range, std::optional<Key> const& position,
    std::optional<Key> const& target)
{
    auto& records = store.records;
    Key const* bound = nullptr;
    bool strict = false;
    bool const forward = is_next(direction);
    auto consider = [&](Key const& key, bool is_strict) {
        if (bound == nullptr) {
            bound = &key;
            strict = is_strict;
            return;
        }
        int const order = idb::compare(key, *bound);
        if ((forward ? order > 0 : order < 0) || (order == 0 && is_strict)) {
            bound = &key;
            strict = is_strict;
        }
    };
    if (forward) {
        if (range.lower)
            consider(*range.lower, range.lower_open);
    } else if (range.upper) {
        consider(*range.upper, range.upper_open);
    }
    if (target)
        consider(*target, false);
    if (position)
        consider(*position, true);
    if (forward) {
        auto const at = bound == nullptr ? records.begin() : strict ? records.upper_bound(*bound) : records.lower_bound(*bound);
        if (at == records.end() || range.above_upper(at->first))
            return std::nullopt;
        return Step { at->first, at->first };
    }
    auto at = bound == nullptr ? records.end() : strict ? records.lower_bound(*bound) : records.upper_bound(*bound);
    if (at == records.begin())
        return std::nullopt;
    --at;
    if (range.below_lower(at->first))
        return std::nullopt;
    return Step { at->first, at->first };
}

// The same over an index's records, ordered by index key and then by
// primary key; a unique direction skips the rest of a key's records, and
// lands on the first of them either way.
std::optional<Step> step_index(idb::IndexState& index, Direction direction, KeyRange const& range, std::optional<Key> const& position,
    std::optional<Key> const& object_position, std::optional<Key> const& target, std::optional<Key> const& target_primary)
{
    auto& entries = index.entries;
    using Iterator = decltype(entries.begin());
    idb::IndexEntryLess const less;
    if (is_next(direction)) {
        Iterator best = entries.begin();
        auto later = [&](Iterator candidate) {
            if (best == entries.end())
                return;
            if (candidate == entries.end() || less(*best, *candidate))
                best = candidate;
        };
        if (range.lower)
            later(entries.lower_bound(idb::IndexProbe { &*range.lower, nullptr, range.lower_open ? 1 : -1 }));
        if (target) {
            later(target_primary ? entries.lower_bound(idb::IndexProbe { &*target, &*target_primary, 0 })
                                 : entries.lower_bound(idb::IndexProbe { &*target, nullptr, -1 }));
        }
        if (position) {
            if (direction == Direction::NextUnique || !object_position)
                later(entries.lower_bound(idb::IndexProbe { &*position, nullptr, 1 }));
            else
                later(entries.upper_bound(idb::IndexProbe { &*position, &*object_position, 0 }));
        }
        if (best == entries.end() || range.above_upper(best->first))
            return std::nullopt;
        return Step { best->first, best->second };
    }
    Iterator end = entries.end();
    auto earlier = [&](Iterator candidate) {
        if (candidate == entries.end())
            return;
        if (end == entries.end() || less(*candidate, *end))
            end = candidate;
    };
    if (range.upper)
        earlier(entries.lower_bound(idb::IndexProbe { &*range.upper, nullptr, range.upper_open ? -1 : 1 }));
    if (target) {
        earlier(target_primary ? entries.upper_bound(idb::IndexProbe { &*target, &*target_primary, 0 })
                               : entries.lower_bound(idb::IndexProbe { &*target, nullptr, 1 }));
    }
    if (position) {
        if (direction == Direction::PrevUnique || !object_position)
            earlier(entries.lower_bound(idb::IndexProbe { &*position, nullptr, -1 }));
        else
            earlier(entries.lower_bound(idb::IndexProbe { &*position, &*object_position, 0 }));
    }
    if (end == entries.begin())
        return std::nullopt;
    Iterator found = std::prev(end);
    if (range.below_lower(found->first))
        return std::nullopt;
    if (direction == Direction::PrevUnique) {
        Key const key = found->first;
        found = entries.lower_bound(idb::IndexProbe { &key, nullptr, -1 });
    }
    return Step { found->first, found->second };
}

std::vector<Step> collect(idb::StoreState& store, idb::IndexState* index, Direction direction, KeyRange const& range, std::uint32_t count)
{
    std::vector<Step> out;
    std::optional<Key> position;
    std::optional<Key> object_position;
    while (count == 0 || out.size() < count) {
        std::optional<Step> found = index != nullptr ? step_index(*index, direction, range, position, object_position, std::nullopt, std::nullopt)
                                                     : step_store(store, direction, range, position, std::nullopt);
        if (!found)
            break;
        position = found->key;
        object_position = found->primary;
        out.push_back(std::move(*found));
    }
    return out;
}

// --- The operations -----------------------------------------------------------------------------------

// Section 5.5.1, "store a record into an object store".
void store_record(Realm::Internals& in, IdbTransactionObject& tx, IdbRequestObject& request, std::uint64_t store_id, js::Value const& clone,
    std::optional<Key> key, bool no_overwrite)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(clone);
    IdbRealm& state = realm_state(in);
    Lock const lock(state.storage->mutex());
    idb::StoreState* const store = tx.core->database->state.store(store_id);
    if (store == nullptr) {
        request.error = error_value(in, "InvalidStateError", "The object store has been deleted.");
        return;
    }
    idb::UndoLog* const undo = undo_of(tx);
    // A record refused keeps the generator where it was, as the shipping
    // engines do.
    double const generator_before = store->current_number;
    if (store->auto_increment) {
        if (!key) {
            std::optional<double> const generated = idb::generate_key(*store, undo);
            if (!generated) {
                request.error = error_value(in, "ConstraintError", "The key generator has no more keys to give.");
                return;
            }
            key = Key::from_number(*generated);
            if (store->key_path.kind == KeyPath::Kind::String)
                inject_key(in, clone, store->key_path.paths.front(), *key);
        } else {
            idb::possibly_update_key_generator(*store, *key, undo);
        }
    }
    if (no_overwrite && store->records.contains(*key)) {
        store->current_number = generator_before;
        request.error = error_value(in, "ConstraintError", "A record with this key already exists in the object store.");
        return;
    }
    idb::IndexKeys index_keys;
    for (auto& [index_id, index] : store->indexes) {
        Converted converted = extract_key(in, clone, index.key_path, index.multi_entry);
        if (converted.outcome == Outcome::Threw)
            interp.clear_exception();
        if (converted.outcome != Outcome::Ok)
            continue;
        std::vector<Key> keys;
        if (index.multi_entry && converted.key.type == Key::Type::Array)
            keys = std::move(converted.key.array);
        else
            keys.push_back(std::move(converted.key));
        if (!keys.empty())
            index_keys.emplace_back(index_id, std::move(keys));
    }
    if (idb::violates_unique_index(*store, *key, index_keys)) {
        store->current_number = generator_before;
        request.error = error_value(in, "ConstraintError", "A unique index already has a record with this key.");
        return;
    }
    std::shared_ptr<SerializedMessage const> const serialized = structured_serialize(in, clone, {});
    if (!serialized) {
        interp.clear_exception();
        request.error = error_value(in, "DataCloneError", "The value could not be stored.");
        return;
    }
    idb::put_record(*store, *key, idb::Record { serialized_to_bytes(*serialized), std::move(index_keys) }, undo);
    request.result = key_value(in, *key);
}

// Section 7.4.3, "iterate a cursor", `count` steps.
void iterate_cursor(Realm::Internals& in, IdbCursorObject& cursor, IdbRequestObject& request, std::optional<Key> const& target,
    std::optional<Key> const& target_primary, std::uint32_t count)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&cursor));
    IdbRealm& state = realm_state(in);
    Lock const lock(state.storage->mutex());
    idb::StoreState* const store = store_state(*cursor.store);
    idb::IndexState* const index = cursor.index != nullptr ? index_state(*cursor.index) : nullptr;
    cursor.key_value = js::Value::undefined();
    cursor.primary_key_value = js::Value::undefined();
    std::optional<Step> found;
    if (store != nullptr && (cursor.index == nullptr || index != nullptr)) {
        std::optional<Key> position = cursor.position;
        std::optional<Key> object_position = cursor.object_store_position;
        for (std::uint32_t i = 0; i < count; ++i) {
            found = index != nullptr ? step_index(*index, cursor.direction, cursor.range, position, object_position, target, target_primary)
                                     : step_store(*store, cursor.direction, cursor.range, position, target);
            if (!found)
                break;
            position = found->key;
            object_position = found->primary;
        }
    }
    if (!found) {
        cursor.key.reset();
        if (cursor.index != nullptr)
            cursor.object_store_position.reset();
        if (!cursor.key_only)
            cursor.value = js::Value::undefined();
        request.result = js::Value::null();
        return;
    }
    cursor.position = found->key;
    if (cursor.index != nullptr)
        cursor.object_store_position = found->primary;
    cursor.key = found->key;
    cursor.primary_key = found->primary;
    if (!cursor.key_only) {
        auto const record = store->records.find(found->primary);
        cursor.value = record == store->records.end() ? js::Value::undefined() : deserialize_value(in, record->second.value);
    }
    cursor.got_value = true;
    request.result = js::Value::object(&cursor);
}

// What a read answers with.
enum class Read : std::uint8_t { Value, Key, PrimaryKey, Record };

js::Value read_one(Realm::Internals& in, idb::StoreState& store, Step const& step, Read what, bool from_index)
{
    js::Interpreter& interp = in.interpreter;
    auto const value_of = [&]() -> js::Value {
        auto const record = store.records.find(step.primary);
        return record == store.records.end() ? js::Value::undefined() : deserialize_value(in, record->second.value);
    };
    switch (what) {
    case Read::Value:
        return value_of();
    case Read::Key:
        return key_value(in, step.key);
    case Read::PrimaryKey:
        return key_value(in, step.primary);
    case Read::Record: {
        js::Interpreter::Roots const roots(interp);
        auto* record = interp.heap().allocate<IdbRecordObject>(in.prototype("IDBRecord"));
        interp.root(js::Value::object(record));
        record->key = key_value(in, from_index ? step.key : step.primary);
        record->primary_key = key_value(in, step.primary);
        record->value = value_of();
        return js::Value::object(record);
    }
    }
    return js::Value::undefined();
}

// get, getKey, getAll, getAllKeys, getAllRecords and count, over a store or
// one of its indexes.
std::function<void(Realm::Internals&, IdbTransactionObject&, IdbRequestObject&)> read_operation(std::uint64_t store_id,
    std::optional<std::uint64_t> index_id, KeyRange range, Read what, bool all, std::uint32_t count, Direction direction, bool counting)
{
    return [=](Realm::Internals& in, IdbTransactionObject& tx, IdbRequestObject& request) {
        js::Interpreter& interp = in.interpreter;
        js::Interpreter::Roots const roots(interp);
        IdbRealm& state = realm_state(in);
        Lock const lock(state.storage->mutex());
        idb::StoreState* const store = tx.core->database->state.store(store_id);
        if (store == nullptr)
            return;
        idb::IndexState* index = nullptr;
        if (index_id) {
            auto const found = store->indexes.find(*index_id);
            if (found == store->indexes.end())
                return;
            index = &found->second;
        }
        if (counting) {
            std::size_t total = 0;
            if (index != nullptr) {
                auto at = range.lower ? index->entries.lower_bound(idb::IndexProbe { &*range.lower, nullptr, range.lower_open ? 1 : -1 })
                                      : index->entries.begin();
                for (; at != index->entries.end() && !range.above_upper(at->first); ++at)
                    ++total;
            } else {
                auto at = range.lower ? (range.lower_open ? store->records.upper_bound(*range.lower) : store->records.lower_bound(*range.lower))
                                      : store->records.begin();
                for (; at != store->records.end() && !range.above_upper(at->first); ++at)
                    ++total;
            }
            request.result = js::Value::number(static_cast<double>(total));
            return;
        }
        std::vector<Step> const steps = collect(*store, index, all ? direction : Direction::Next, range, all ? count : 1);
        if (!all) {
            request.result = steps.empty() ? js::Value::undefined() : read_one(in, *store, steps.front(), what, index != nullptr);
            return;
        }
        std::vector<js::Value> items;
        items.reserve(steps.size());
        for (Step const& step : steps)
            items.push_back(interp.root(read_one(in, *store, step, what, index != nullptr)));
        request.result = js::Value::object(interp.new_array(items));
    };
}

// --- Conversions of arguments -------------------------------------------------------------------------

// Section 7.3, "convert a value to a key range".
std::optional<KeyRange> to_range(Realm::Internals& in, js::Value const& value, bool null_disallowed = false)
{
    if (value.is_object()) {
        if (auto* range = dynamic_cast<IdbKeyRangeObject*>(value.as_object()))
            return range->range;
    }
    if (value.is_nullish()) {
        if (null_disallowed) {
            in.throw_dom_exception("DataError", "No key or key range was given.");
            return std::nullopt;
        }
        return KeyRange::unbounded();
    }
    Converted converted = to_key(in, value);
    if (converted.outcome == Outcome::Threw)
        return std::nullopt;
    if (converted.outcome != Outcome::Ok) {
        in.throw_dom_exception("DataError", "The parameter is not a valid key.");
        return std::nullopt;
    }
    return KeyRange::only(std::move(converted.key));
}

// A key argument: nullopt with the exception pending, a DataError for a
// value that is no key.
std::optional<Key> key_argument(Realm::Internals& in, js::Value const& value)
{
    Converted converted = to_key(in, value);
    if (converted.outcome == Outcome::Threw)
        return std::nullopt;
    if (converted.outcome != Outcome::Ok) {
        in.throw_dom_exception("DataError", "The parameter is not a valid key.");
        return std::nullopt;
    }
    return std::move(converted.key);
}

// WebIDL's [EnforceRange] unsigned long, and unsigned long long.
std::optional<double> enforce_range(Realm::Internals& in, js::Value const& value, double maximum)
{
    std::optional<double> const number = in.interpreter.to_number(value);
    if (!number)
        return std::nullopt;
    if (std::isnan(*number) || std::isinf(*number)) {
        in.interpreter.throw_type_error("The value is not a finite number.");
        return std::nullopt;
    }
    double const whole = std::trunc(*number);
    if (whole < 0 || whole > maximum) {
        in.interpreter.throw_type_error("The value is outside the range the parameter takes.");
        return std::nullopt;
    }
    return whole;
}

std::optional<Direction> direction_argument(Realm::Internals& in, js::Value const& value)
{
    if (value.is_undefined())
        return Direction::Next;
    std::optional<js::JsString*> const text = in.interpreter.to_string(value);
    if (!text)
        return std::nullopt;
    std::u16string const& name = (*text)->data();
    if (name == u"next")
        return Direction::Next;
    if (name == u"nextunique")
        return Direction::NextUnique;
    if (name == u"prev")
        return Direction::Prev;
    if (name == u"prevunique")
        return Direction::PrevUnique;
    in.interpreter.throw_type_error("The direction is not one of 'next', 'nextunique', 'prev' and 'prevunique'.");
    return std::nullopt;
}

std::string_view direction_name(Direction direction)
{
    switch (direction) {
    case Direction::Next:
        return "next";
    case Direction::NextUnique:
        return "nextunique";
    case Direction::Prev:
        return "prev";
    case Direction::PrevUnique:
        return "prevunique";
    }
    return "next";
}

// The arguments getAll and its kin take (IDBGetAllOptions, or a query and a
// count).
struct GetAllArguments {
    KeyRange range;
    std::uint32_t count = 0;
    Direction direction = Direction::Next;
};

bool is_potentially_valid_key_range(js::Value const& value)
{
    if (value.is_number() || value.is_string())
        return true;
    if (!value.is_object())
        return false;
    js::Object const& object = *value.as_object();
    return dynamic_cast<IdbKeyRangeObject const*>(&object) != nullptr || object.is_array() || object.class_id() == js::Object::Class::Date
        || buffer_source_bytes(value).has_value();
}

std::optional<GetAllArguments> get_all_arguments(Realm::Internals& in, Args args, bool options_only)
{
    js::Interpreter& interp = in.interpreter;
    GetAllArguments out;
    js::Value const first = js::argument(args, 0);
    bool const dictionary = options_only || (first.is_object() && !is_potentially_valid_key_range(first));
    if (dictionary) {
        if (!first.is_nullish() && !first.is_object()) {
            interp.throw_type_error("The options are not a dictionary.");
            return std::nullopt;
        }
        js::Value count = js::Value::undefined();
        js::Value direction = js::Value::undefined();
        js::Value query = js::Value::undefined();
        js::Interpreter::Roots const roots(interp);
        if (first.is_object()) {
            std::optional<js::Value> const got_count = interp.get(first, "count");
            if (!got_count)
                return std::nullopt;
            count = interp.root(*got_count);
            std::optional<js::Value> const got_direction = interp.get(first, "direction");
            if (!got_direction)
                return std::nullopt;
            direction = interp.root(*got_direction);
            std::optional<js::Value> const got_query = interp.get(first, "query");
            if (!got_query)
                return std::nullopt;
            query = interp.root(*got_query);
        }
        if (!count.is_undefined()) {
            std::optional<double> const number = enforce_range(in, count, 4294967295.0);
            if (!number)
                return std::nullopt;
            out.count = static_cast<std::uint32_t>(*number);
        }
        std::optional<Direction> const parsed = direction_argument(in, direction);
        if (!parsed)
            return std::nullopt;
        out.direction = *parsed;
        std::optional<KeyRange> range = to_range(in, query);
        if (!range)
            return std::nullopt;
        out.range = std::move(*range);
        return out;
    }
    if (args.size() > 1 && !args[1].is_undefined()) {
        std::optional<double> const number = enforce_range(in, args[1], 4294967295.0);
        if (!number)
            return std::nullopt;
        out.count = static_cast<std::uint32_t>(*number);
    }
    std::optional<KeyRange> range = to_range(in, first);
    if (!range)
        return std::nullopt;
    out.range = std::move(*range);
    return out;
}

// A clone of a value in the realm, the transaction inactive while it is
// serialized (section 5.8.1, "clone a value").
std::optional<js::Value> clone_value(Realm::Internals& in, IdbTransactionObject& tx, js::Value const& value)
{
    IdbTransactionObject::State const was = tx.state;
    tx.state = IdbTransactionObject::State::Inactive;
    std::shared_ptr<SerializedMessage const> const serialized = structured_serialize(in, value, {});
    tx.state = was;
    if (!serialized)
        return std::nullopt;
    std::optional<Deserialized> const made = structured_deserialize(in, *serialized);
    if (!made)
        return std::nullopt;
    return in.interpreter.root(made->value);
}

// --- Handles ----------------------------------------------------------------------------------------------

IdbObjectStoreObject* store_handle(Realm::Internals& in, IdbTransactionObject& tx, std::uint64_t id)
{
    for (auto const& [store_id, handle] : tx.store_handles) {
        if (store_id == id)
            return handle;
    }
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&tx));
    auto* handle = in.interpreter.heap().allocate<IdbObjectStoreObject>(in.prototype("IDBObjectStore"));
    handle->transaction = &tx;
    handle->store_id = id;
    if (idb::StoreState* const store = tx.core->database->state.store(id))
        handle->name = store->name;
    tx.store_handles.emplace_back(id, handle);
    return handle;
}

IdbIndexObject* index_handle(Realm::Internals& in, IdbObjectStoreObject& store, std::uint64_t id)
{
    for (auto const& [index_id, handle] : store.index_handles) {
        if (index_id == id)
            return handle;
    }
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&store));
    auto* handle = in.interpreter.heap().allocate<IdbIndexObject>(in.prototype("IDBIndex"));
    handle->store = &store;
    handle->index_id = id;
    if (idb::StoreState* const state = store_state(store)) {
        auto const found = state->indexes.find(id);
        if (found != state->indexes.end())
            handle->name = found->second.name;
    }
    store.index_handles.emplace_back(id, handle);
    return handle;
}

bool active(IdbTransactionObject const& tx)
{
    return tx.state == IdbTransactionObject::State::Active;
}

// The first checks of a store's read methods: the store still there, the
// transaction active.
std::optional<idb::StoreState*> readable_store(Realm::Internals& in, IdbObjectStoreObject& handle)
{
    idb::StoreState* const store = store_state(handle);
    if (store == nullptr) {
        in.throw_dom_exception("InvalidStateError", "The object store has been deleted.");
        return std::nullopt;
    }
    if (!active(*handle.transaction)) {
        in.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        return std::nullopt;
    }
    return store;
}

std::optional<idb::IndexState*> readable_index(Realm::Internals& in, IdbIndexObject& handle)
{
    idb::IndexState* const index = index_state(handle);
    if (index == nullptr) {
        in.throw_dom_exception("InvalidStateError", "The index or its object store has been deleted.");
        return std::nullopt;
    }
    if (!active(*handle.store->transaction)) {
        in.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        return std::nullopt;
    }
    return index;
}

std::optional<IdbCursorObject*> this_cursor(js::Interpreter& interp, js::Value const& this_value)
{
    return this_of<IdbCursorObject>(interp, this_value);
}

// --- The members --------------------------------------------------------------------------------------------

Native open_cursor(js::Interpreter& interp, js::Value const& this_value, Args args, bool on_index, bool key_only)
{
    Realm::Internals& in = internals_of(interp);
    IdbObjectStoreObject* store = nullptr;
    IdbIndexObject* index = nullptr;
    if (on_index) {
        std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        index = *found;
        store = index->store;
    } else {
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        store = *found;
    }
    std::optional<Direction> const direction = direction_argument(in, js::argument(args, 1));
    if (!direction)
        return std::nullopt;
    if (on_index ? !readable_index(in, *index) : !readable_store(in, *store))
        return std::nullopt;
    std::optional<KeyRange> range = to_range(in, js::argument(args, 0));
    if (!range)
        return std::nullopt;
    js::Interpreter::Roots const roots(interp);
    auto* cursor = interp.heap().allocate<IdbCursorObject>(in.prototype(key_only ? "IDBCursor" : "IDBCursorWithValue"));
    interp.root(js::Value::object(cursor));
    cursor->transaction = store->transaction;
    cursor->store = store;
    cursor->index = index;
    cursor->range = std::move(*range);
    cursor->direction = *direction;
    cursor->key_only = key_only;
    IdbRequestObject* const request = place_request(in, *store->transaction, js::Value::object(cursor->source()),
        [cursor](Realm::Internals& here, IdbTransactionObject&, IdbRequestObject& target) {
            iterate_cursor(here, *cursor, target, std::nullopt, std::nullopt, 1);
        },
        { js::Value::object(cursor) });
    cursor->request = request;
    return js::Value::object(request);
}

Native read_request(js::Interpreter& interp, js::Value const& this_value, Args args, bool on_index, Read what, bool all, bool counting,
    bool options_only = false)
{
    Realm::Internals& in = internals_of(interp);
    IdbObjectStoreObject* store = nullptr;
    IdbIndexObject* index = nullptr;
    if (on_index) {
        std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        index = *found;
        store = index->store;
    } else {
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        store = *found;
    }
    if (!all && !counting && args.empty())
        return interp.throw_type_error("1 argument required, but only 0 present.");
    if (on_index ? !readable_index(in, *index) : !readable_store(in, *store))
        return std::nullopt;
    GetAllArguments arguments;
    if (all) {
        std::optional<GetAllArguments> parsed = get_all_arguments(in, args, options_only);
        if (!parsed)
            return std::nullopt;
        arguments = std::move(*parsed);
    } else {
        std::optional<KeyRange> range = to_range(in, js::argument(args, 0), !counting);
        if (!range)
            return std::nullopt;
        arguments.range = std::move(*range);
    }
    js::Value const source = js::Value::object(on_index ? static_cast<js::Object*>(index) : static_cast<js::Object*>(store));
    std::optional<std::uint64_t> const index_id = on_index ? std::optional<std::uint64_t>(index->index_id) : std::nullopt;
    return js::Value::object(place_request(in, *store->transaction, source,
        read_operation(store->store_id, index_id, std::move(arguments.range), what, all, arguments.count, arguments.direction, counting)));
}

Native add_or_put(js::Interpreter& interp, js::Value const& this_value, Args args, bool no_overwrite)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
    if (!found)
        return std::nullopt;
    IdbObjectStoreObject& handle = **found;
    IdbTransactionObject& tx = *handle.transaction;
    idb::StoreState* const store = store_state(handle);
    if (store == nullptr)
        return in.throw_dom_exception("InvalidStateError", "The object store has been deleted.");
    if (!active(tx))
        return in.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
    if (tx.mode == idb::Mode::ReadOnly)
        return in.throw_dom_exception("ReadOnlyError", "The transaction is read-only.");
    bool const in_line = store->key_path.kind != KeyPath::Kind::None;
    bool const key_given = args.size() > 1 && !args[1].is_undefined();
    if (in_line && key_given)
        return in.throw_dom_exception("DataError", "The object store uses in-line keys and the key parameter was provided.");
    if (!in_line && !store->auto_increment && !key_given)
        return in.throw_dom_exception("DataError", "The object store uses out-of-line keys and has no key generator, and the key parameter was not provided.");
    std::optional<Key> key;
    if (key_given) {
        key = key_argument(in, args[1]);
        if (!key)
            return std::nullopt;
    }
    js::Interpreter::Roots const roots(interp);
    std::optional<js::Value> const clone = clone_value(in, tx, js::argument(args, 0));
    if (!clone)
        return std::nullopt;
    // Refetched: a getter run by the serialization may have changed the schema.
    idb::StoreState* const current = store_state(handle);
    if (current == nullptr)
        return in.throw_dom_exception("InvalidStateError", "The object store has been deleted.");
    if (in_line) {
        Converted converted = extract_key(in, *clone, current->key_path);
        if (converted.outcome == Outcome::Threw)
            return std::nullopt;
        if (converted.outcome == Outcome::Invalid)
            return in.throw_dom_exception("DataError", "Evaluating the object store's key path yielded a value that is not a valid key.");
        if (converted.outcome == Outcome::Ok) {
            key = std::move(converted.key);
        } else if (!current->auto_increment) {
            return in.throw_dom_exception("DataError", "Evaluating the object store's key path did not yield a value.");
        } else if (!can_inject(in, *clone, current->key_path.paths.front())) {
            return in.throw_dom_exception("DataError", "A generated key could not be inserted into the value.");
        }
    }
    std::uint64_t const store_id = handle.store_id;
    auto shared_key = std::make_shared<std::optional<Key>>(std::move(key));
    return js::Value::object(place_request(in, tx, this_value,
        [store_id, shared_key, no_overwrite](Realm::Internals& here, IdbTransactionObject& transaction, IdbRequestObject& request) {
            // The clone is the operation's first held value.
            js::Value const value = transaction.executing != nullptr ? transaction.executing->held.front() : js::Value::undefined();
            store_record(here, transaction, request, store_id, value, *shared_key, no_overwrite);
        },
        { *clone }));
}

Native rename(js::Interpreter& interp, js::Value const& this_value, Args args, bool is_index)
{
    Realm::Internals& in = internals_of(interp);
    IdbObjectStoreObject* store = nullptr;
    IdbIndexObject* index = nullptr;
    if (is_index) {
        std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        index = *found;
        store = index->store;
    } else {
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        store = *found;
    }
    std::optional<js::JsString*> const text = interp.to_string(js::argument(args, 0));
    if (!text)
        return std::nullopt;
    std::u16string const name = (*text)->data();
    IdbTransactionObject& tx = *store->transaction;
    idb::StoreState* const state = store_state(*store);
    idb::IndexState* const index_found = is_index ? index_state(*index) : nullptr;
    if (state == nullptr || (is_index && index_found == nullptr))
        return in.throw_dom_exception("InvalidStateError", "The object store or index has been deleted.");
    if (tx.mode != idb::Mode::VersionChange)
        return in.throw_dom_exception("InvalidStateError", "Names change only in an upgrade transaction.");
    if (!active(tx))
        return in.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
    IdbRealm& realm = realm_state(in);
    Lock const lock(realm.storage->mutex());
    if (is_index) {
        if (index_found->name == name)
            return js::Value::undefined();
        if (state->index_named(name) != nullptr)
            return in.throw_dom_exception("ConstraintError", "An index with this name already exists.");
        index_found->name = name;
        index->name = name;
    } else {
        if (state->name == name)
            return js::Value::undefined();
        if (tx.core->database->state.store_named(name) != nullptr)
            return in.throw_dom_exception("ConstraintError", "An object store with this name already exists.");
        state->name = name;
        store->name = name;
    }
    return js::Value::undefined();
}

Native factory_open(js::Interpreter& interp, js::Value const& this_value, Args args, bool deleting)
{
    Realm::Internals& in = internals_of(interp);
    if (!this_value.is_object() || this_value.as_object() != in.window_values["indexedDB"].as_object())
        return interp.throw_type_error("Illegal invocation");
    if (args.empty())
        return interp.throw_type_error(deleting ? "Failed to execute 'deleteDatabase' on 'IDBFactory': 1 argument required."
                                                : "Failed to execute 'open' on 'IDBFactory': 1 argument required.");
    std::optional<js::JsString*> const text = interp.to_string(args[0]);
    if (!text)
        return std::nullopt;
    std::u16string const name = (*text)->data();
    std::optional<std::uint64_t> version;
    if (!deleting && args.size() > 1 && !args[1].is_undefined()) {
        std::optional<double> const number = enforce_range(in, args[1], 9007199254740991.0);
        if (!number)
            return std::nullopt;
        if (*number == 0)
            return interp.throw_type_error("The version provided must not be 0.");
        version = static_cast<std::uint64_t>(*number);
    }
    IdbRealm& state = realm_state(in);
    if (state.opaque || !state.storage)
        return in.throw_dom_exception("SecurityError", "Access to the Indexed Database API is denied in this context.");
    js::Interpreter::Roots const roots(interp);
    auto* request = interp.heap().allocate<IdbRequestObject>(in.prototype("IDBOpenDBRequest"));
    interp.root(js::Value::object(request));
    request->open_request = true;
    state.requests.push_back(request);
    auto queued = std::make_shared<QueuedRequest>();
    queued->name = name;
    state.queued.push_back(queued);
    std::shared_ptr<IdbLink> const link = state.link;
    Lock const lock(state.storage->mutex());
    queued->id = state.storage->next_serial();
    queued->database = &state.storage->database(name);
    state.storage->enqueue(*queued->database, queued->id, [link, queued, request, version, deleting] {
        link->post([queued, request, version, deleting](Realm::Internals& here) {
            IdbRealm& here_state = realm_state(here);
            if (std::find(here_state.requests.begin(), here_state.requests.end(), request) == here_state.requests.end())
                return;
            js::Interpreter::RealmScope const inside(here.interpreter, here.realm_record);
            if (deleting)
                run_delete(here, *request, queued);
            else
                run_open(here, *request, queued, version);
        });
    });
    return js::Value::object(request);
}

// The IDBVersionChangeEvent constructor: the Event's init, and the versions.
Native construct_version_change_event(js::Interpreter& interp, Args args, js::Object*)
{
    Realm::Internals& in = internals_of(interp);
    if (args.empty())
        return interp.throw_type_error("Failed to construct 'IDBVersionChangeEvent': 1 argument required.");
    std::optional<std::string> const type = in.to_utf8(args[0]);
    if (!type)
        return std::nullopt;
    js::Value const init = js::argument(args, 1);
    if (!init.is_nullish() && !init.is_object())
        return interp.throw_type_error("Failed to construct 'IDBVersionChangeEvent': parameter 2 is not a dictionary.");
    js::Interpreter::Roots const roots(interp);
    bool flags[3] = { false, false, false };
    double old_version = 0;
    std::optional<double> new_version;
    if (init.is_object()) {
        std::string_view const names[3] = { "bubbles", "cancelable", "composed" };
        for (int i = 0; i < 3; ++i) {
            std::optional<js::Value> const value = interp.get(init, names[i]);
            if (!value)
                return std::nullopt;
            flags[i] = js::Interpreter::to_boolean(*value);
        }
        std::optional<js::Value> const got_new = interp.get(init, "newVersion");
        if (!got_new)
            return std::nullopt;
        if (!got_new->is_nullish()) {
            std::optional<double> const number = interp.to_number(*got_new);
            if (!number)
                return std::nullopt;
            new_version = std::isfinite(*number) ? std::trunc(*number) : 0;
        }
        std::optional<js::Value> const got_old = interp.get(init, "oldVersion");
        if (!got_old)
            return std::nullopt;
        if (!got_old->is_undefined()) {
            std::optional<double> const number = interp.to_number(*got_old);
            if (!number)
                return std::nullopt;
            old_version = std::isfinite(*number) ? std::trunc(*number) : 0;
        }
    }
    EventObject* const event = in.new_event("IDBVersionChangeEvent", *type, flags[0], flags[1]);
    event->composed = flags[2];
    event->old_version = old_version;
    event->new_version = new_version;
    return js::Value::object(event);
}

}

std::shared_ptr<idb::Storage> indexeddb_storage(Realm::Internals& in)
{
    return realm_state(in).storage;
}

void trace_indexeddb(Realm::Internals const& in, js::Tracer& tracer)
{
    if (!in.indexed_db)
        return;
    IdbRealm const& state = *in.indexed_db;
    tracer.visit(state.factory);
    for (IdbTransactionObject* tx : state.transactions)
        tracer.visit(tx);
    for (IdbDatabaseObject* db : state.connections)
        tracer.visit(db);
    for (IdbRequestObject* request : state.requests)
        tracer.visit(request);
}

void install_indexeddb(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* const global = interpreter.global();
    js::Object* const event_target = in.prototype("EventTarget");

    // DOMStringList (HTML section 2.7.6), when nothing else has made it.
    if (in.prototype("DOMStringList") == nullptr) {
        js::Object* list = define_interface(in, "DOMStringList", nullptr);
        define_getter(in, *list, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<StringListObject*> const found = this_of<StringListObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            return js::Value::number(static_cast<double>((*found)->items.size()));
        });
        define_operation(interpreter, *list, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<StringListObject*> const found = this_of<StringListObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            std::optional<double> const index = interp.to_number(js::argument(args, 0));
            if (!index)
                return std::nullopt;
            std::uint32_t const at = to_unsigned_long(*index);
            if (at >= (*found)->items.size())
                return js::Value::null();
            return string_value(interp, (*found)->items[at]);
        });
        define_operation(interpreter, *list, "contains", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<StringListObject*> const found = this_of<StringListObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            std::optional<js::JsString*> const text = interp.to_string(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            auto const& items = (*found)->items;
            return js::Value::boolean(std::find(items.begin(), items.end(), (*text)->data()) != items.end());
        });
        list->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator),
            *interpreter.get(js::Value::object(interpreter.intrinsics().array_prototype), js::PropertyKey::symbol(interpreter.atoms().symbol_iterator)),
            js::builtin_attributes);
    }

    // IDBVersionChangeEvent.
    js::Object* version_event = define_interface(in, "IDBVersionChangeEvent", in.prototype("Event"), construct_version_change_event, 1);
    define_getter(in, *version_event, "oldVersion", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const found = this_of<EventObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::number((*found)->old_version);
    });
    define_getter(in, *version_event, "newVersion", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<EventObject*> const found = this_of<EventObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->new_version ? js::Value::number(*(*found)->new_version) : js::Value::null();
    });

    // IDBRequest and IDBOpenDBRequest.
    js::Object* request = define_interface(in, "IDBRequest", event_target);
    define_getter(in, *request, "result", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbRequestObject*> const found = this_of<IdbRequestObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (!(*found)->done)
            return internals_of(interp).throw_dom_exception("InvalidStateError", "The request has not finished.");
        return (*found)->result;
    });
    define_getter(in, *request, "error", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbRequestObject*> const found = this_of<IdbRequestObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (!(*found)->done)
            return internals_of(interp).throw_dom_exception("InvalidStateError", "The request has not finished.");
        return (*found)->error;
    });
    define_getter(in, *request, "source", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbRequestObject*> const found = this_of<IdbRequestObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->source;
    });
    define_getter(in, *request, "transaction", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbRequestObject*> const found = this_of<IdbRequestObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->transaction != nullptr ? js::Value::object((*found)->transaction) : js::Value::null();
    });
    define_getter(in, *request, "readyState", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbRequestObject*> const found = this_of<IdbRequestObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return internals_of(interp).string((*found)->done ? "done" : "pending");
    });
    static constexpr std::string_view request_events[] = { "success", "error" };
    define_event_handlers(in, *request, request_events);
    js::Object* open_request = define_interface(in, "IDBOpenDBRequest", request);
    static constexpr std::string_view open_events[] = { "blocked", "upgradeneeded" };
    define_event_handlers(in, *open_request, open_events);

    // IDBFactory, and indexedDB: the one factory of the global.
    js::Object* factory = define_interface(in, "IDBFactory", nullptr);
    define_operation(interpreter, *factory, "open", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return factory_open(interp, this_value, args, false);
    });
    define_operation(interpreter, *factory, "deleteDatabase", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return factory_open(interp, this_value, args, true);
    });
    define_operation(interpreter, *factory, "databases", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        IdbRealm& state = realm_state(internals);
        if (state.opaque || !state.storage)
            return rejected_promise(interp, error_value(internals, "SecurityError", "Access to the Indexed Database API is denied in this context."));
        std::vector<std::pair<std::u16string, std::uint64_t>> listed;
        {
            Lock const lock(state.storage->mutex());
            listed = state.storage->databases();
        }
        js::Interpreter::Roots const roots(interp);
        std::vector<js::Value> items;
        for (auto const& [name, version] : listed) {
            js::Object* const info = interp.new_object();
            items.push_back(interp.root(js::Value::object(info)));
            info->put(interp.key("name"), string_value(interp, name));
            info->put(interp.key("version"), js::Value::number(static_cast<double>(version)));
        }
        return resolved_promise(interp, js::Value::object(interp.new_array(items)));
    });
    define_operation(interpreter, *factory, "cmp", 2, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        if (args.size() < 2)
            return interp.throw_type_error("Failed to execute 'cmp' on 'IDBFactory': 2 arguments required.");
        std::optional<Key> const first = key_argument(internals, args[0]);
        if (!first)
            return std::nullopt;
        std::optional<Key> const second = key_argument(internals, args[1]);
        if (!second)
            return std::nullopt;
        return js::Value::number(idb::compare(*first, *second));
    });
    js::Object* const factory_object = interpreter.heap().allocate<PlainPlatformObject>(factory);
    in.window_values["indexedDB"] = js::Value::object(factory_object);
    define_getter(in, *global, "indexedDB", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).window_values["indexedDB"];
    });

    // IDBDatabase.
    js::Object* database = define_interface(in, "IDBDatabase", event_target);
    define_getter(in, *database, "name", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbDatabaseObject*> const found = this_of<IdbDatabaseObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return string_value(interp, (*found)->name);
    });
    define_getter(in, *database, "version", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbDatabaseObject*> const found = this_of<IdbDatabaseObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*found)->version));
    });
    define_getter(in, *database, "objectStoreNames", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbDatabaseObject*> const found = this_of<IdbDatabaseObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return string_list(internals_of(interp), (*found)->core->database->state.store_names());
    });
    define_operation(interpreter, *database, "createObjectStore", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbDatabaseObject*> const found = this_of<IdbDatabaseObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbDatabaseObject& db = **found;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'createObjectStore' on 'IDBDatabase': 1 argument required.");
        std::optional<js::JsString*> const text = interp.to_string(args[0]);
        if (!text)
            return std::nullopt;
        std::u16string const name = (*text)->data();
        js::Value const options = js::argument(args, 1);
        if (!options.is_nullish() && !options.is_object())
            return interp.throw_type_error("Failed to execute 'createObjectStore' on 'IDBDatabase': The options are not a dictionary.");
        bool auto_increment = false;
        KeyPath key_path;
        if (options.is_object()) {
            std::optional<js::Value> const increment = interp.get(options, "autoIncrement");
            if (!increment)
                return std::nullopt;
            auto_increment = js::Interpreter::to_boolean(*increment);
            std::optional<js::Value> const path = interp.get(options, "keyPath");
            if (!path)
                return std::nullopt;
            if (!path->is_nullish()) {
                std::optional<KeyPath> converted = key_path_from(internals, *path);
                if (!converted)
                    return std::nullopt;
                key_path = std::move(*converted);
            }
        }
        // The upgrade transaction stays the connection's until its complete
        // or abort event, finished or not.
        IdbTransactionObject* const tx = db.upgrade;
        if (tx == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "Object stores are made only in an upgrade transaction.");
        if (!active(*tx))
            return internals.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        if (key_path.kind != KeyPath::Kind::None && !key_path_valid(key_path))
            return internals.throw_dom_exception("SyntaxError", "The keyPath option is not a valid key path.");
        IdbRealm& state = realm_state(internals);
        std::uint64_t id = 0;
        {
            Lock const lock(state.storage->mutex());
            idb::DatabaseState& schema = db.core->database->state;
            if (schema.store_named(name) != nullptr)
                return internals.throw_dom_exception("ConstraintError", "An object store with this name already exists.");
            bool const empty_or_list = key_path.kind == KeyPath::Kind::Array
                || (key_path.kind == KeyPath::Kind::String && key_path.paths.front().empty());
            if (auto_increment && empty_or_list)
                return internals.throw_dom_exception("InvalidAccessError", "An autoIncrement store needs a non-empty string key path, or none.");
            id = schema.next_id++;
            idb::StoreState store;
            store.id = id;
            store.name = name;
            store.key_path = key_path;
            store.auto_increment = auto_increment;
            schema.stores.emplace(id, std::move(store));
        }
        return js::Value::object(store_handle(internals, *tx, id));
    });
    define_operation(interpreter, *database, "deleteObjectStore", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbDatabaseObject*> const found = this_of<IdbDatabaseObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'deleteObjectStore' on 'IDBDatabase': 1 argument required.");
        std::optional<js::JsString*> const text = interp.to_string(args[0]);
        if (!text)
            return std::nullopt;
        IdbTransactionObject* const tx = (*found)->upgrade;
        if (tx == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "Object stores are deleted only in an upgrade transaction.");
        if (!active(*tx))
            return internals.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        IdbRealm& state = realm_state(internals);
        Lock const lock(state.storage->mutex());
        idb::DatabaseState& schema = (*found)->core->database->state;
        idb::StoreState* const store = schema.store_named((*text)->data());
        if (store == nullptr)
            return internals.throw_dom_exception("NotFoundError", "No object store has this name.");
        schema.stores.erase(store->id);
        return js::Value::undefined();
    });
    define_operation(interpreter, *database, "transaction", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbDatabaseObject*> const found = this_of<IdbDatabaseObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbDatabaseObject& db = **found;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'transaction' on 'IDBDatabase': 1 argument required.");
        // The arguments as WebIDL converts them, before any step.
        std::vector<std::u16string> names;
        js::Value const store_names = args[0];
        bool sequence = false;
        if (store_names.is_object()) {
            std::optional<js::Value> const method = interp.get_method(store_names, js::PropertyKey::symbol(interp.atoms().symbol_iterator));
            if (!method)
                return std::nullopt;
            sequence = !method->is_undefined();
        }
        if (sequence) {
            std::optional<std::vector<js::Value>> const items = interp.iterable_to_list(store_names);
            if (!items)
                return std::nullopt;
            js::Interpreter::Roots const roots(interp);
            for (js::Value const& item : *items)
                interp.root(item);
            for (js::Value const& item : *items) {
                std::optional<js::JsString*> const text = interp.to_string(item);
                if (!text)
                    return std::nullopt;
                names.push_back((*text)->data());
            }
        } else {
            std::optional<js::JsString*> const text = interp.to_string(store_names);
            if (!text)
                return std::nullopt;
            names.push_back((*text)->data());
        }
        idb::Mode mode = idb::Mode::ReadOnly;
        bool version_change_mode = false;
        if (args.size() > 1 && !args[1].is_undefined()) {
            std::optional<js::JsString*> const text = interp.to_string(args[1]);
            if (!text)
                return std::nullopt;
            std::u16string const& word = (*text)->data();
            if (word == u"readwrite")
                mode = idb::Mode::ReadWrite;
            else if (word == u"versionchange")
                version_change_mode = true;
            else if (word != u"readonly")
                return interp.throw_type_error("The mode is not one of 'readonly', 'readwrite' and 'versionchange'.");
        }
        std::string durability = "default";
        js::Value const options = js::argument(args, 2);
        if (!options.is_nullish() && !options.is_object())
            return interp.throw_type_error("The options are not a dictionary.");
        if (options.is_object()) {
            std::optional<js::Value> const value = interp.get(options, "durability");
            if (!value)
                return std::nullopt;
            if (!value->is_undefined()) {
                std::optional<std::string> const word = internals.to_utf8(*value);
                if (!word)
                    return std::nullopt;
                if (*word != "default" && *word != "strict" && *word != "relaxed")
                    return interp.throw_type_error("The durability is not one of 'default', 'strict' and 'relaxed'.");
                durability = *word;
            }
        }
        if (db.upgrade != nullptr && db.upgrade->state != IdbTransactionObject::State::Finished)
            return internals.throw_dom_exception("InvalidStateError", "An upgrade transaction is running on this connection.");
        if (db.close_pending())
            return internals.throw_dom_exception("InvalidStateError", "The connection is closing.");
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (std::u16string const& name : names) {
            if (db.core->database->state.store_named(name) == nullptr)
                return internals.throw_dom_exception("NotFoundError", "One of the object stores named was not found.");
        }
        // The empty scope before the mode, in the specification's order.
        if (names.empty())
            return internals.throw_dom_exception("InvalidAccessError", "The scope names no object store.");
        if (version_change_mode)
            return interp.throw_type_error("A transaction cannot be made in the 'versionchange' mode.");
        js::Interpreter::Roots const roots(interp);
        IdbTransactionObject* const tx = new_transaction(internals, db, mode, names);
        interp.root(js::Value::object(tx));
        tx->durability = durability;
        IdbRealm& state = realm_state(internals);
        std::uint64_t const serial = tx->core->scheduled->serial;
        std::shared_ptr<IdbLink> const link = state.link;
        tx->core->scheduled->on_start = [link, serial] {
            link->post([serial](Realm::Internals& here) {
                IdbTransactionObject* const started = find_transaction(realm_state(here), serial);
                if (started == nullptr)
                    return;
                started->started = true;
                js::Interpreter::RealmScope const inside(here.interpreter, here.realm_record);
                pump(here, *started);
            });
        };
        {
            Lock const lock(state.storage->mutex());
            state.storage->schedule(*db.core->database, tx->core->scheduled);
        }
        arrange_deactivation(internals, *tx);
        return js::Value::object(tx);
    });
    define_operation(interpreter, *database, "close", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbDatabaseObject*> const found = this_of<IdbDatabaseObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        close_connection(internals_of(interp), **found);
        return js::Value::undefined();
    });
    static constexpr std::string_view database_events[] = { "abort", "close", "error", "versionchange" };
    define_event_handlers(in, *database, database_events);

    // IDBTransaction.
    js::Object* transaction = define_interface(in, "IDBTransaction", event_target);
    define_getter(in, *transaction, "objectStoreNames", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbTransactionObject& tx = **found;
        if (tx.mode == idb::Mode::VersionChange && tx.state != IdbTransactionObject::State::Finished)
            return string_list(internals_of(interp), tx.core->database->state.store_names());
        return string_list(internals_of(interp), tx.scope);
    });
    define_getter(in, *transaction, "mode", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        idb::Mode const mode = (*found)->mode;
        return internals_of(interp).string(mode == idb::Mode::ReadOnly ? "readonly" : mode == idb::Mode::ReadWrite ? "readwrite" : "versionchange");
    });
    define_getter(in, *transaction, "durability", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return internals_of(interp).string((*found)->durability);
    });
    define_getter(in, *transaction, "db", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::object((*found)->db);
    });
    define_getter(in, *transaction, "error", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->error;
    });
    define_operation(interpreter, *transaction, "objectStore", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'objectStore' on 'IDBTransaction': 1 argument required.");
        std::optional<js::JsString*> const text = interp.to_string(args[0]);
        if (!text)
            return std::nullopt;
        IdbTransactionObject& tx = **found;
        if (tx.state == IdbTransactionObject::State::Finished)
            return internals.throw_dom_exception("InvalidStateError", "The transaction has finished.");
        std::u16string const& name = (*text)->data();
        bool const in_scope = tx.mode == idb::Mode::VersionChange || std::find(tx.scope.begin(), tx.scope.end(), name) != tx.scope.end();
        idb::StoreState* const store = in_scope ? tx.core->database->state.store_named(name) : nullptr;
        if (store == nullptr)
            return internals.throw_dom_exception("NotFoundError", "The object store is not in the transaction's scope.");
        return js::Value::object(store_handle(internals, tx, store->id));
    });
    define_operation(interpreter, *transaction, "commit", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (!active(**found))
            return internals.throw_dom_exception("InvalidStateError", "The transaction is not active.");
        commit_transaction(internals, **found);
        return js::Value::undefined();
    });
    define_operation(interpreter, *transaction, "abort", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbTransactionObject*> const found = this_of<IdbTransactionObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbTransactionObject& tx = **found;
        if (tx.state == IdbTransactionObject::State::Committing || tx.state == IdbTransactionObject::State::Finished)
            return internals.throw_dom_exception("InvalidStateError", "The transaction is committing or has finished.");
        tx.state = IdbTransactionObject::State::Inactive;
        abort_transaction(internals, tx, js::Value::null());
        return js::Value::undefined();
    });
    static constexpr std::string_view transaction_events[] = { "abort", "complete", "error" };
    define_event_handlers(in, *transaction, transaction_events);

    // IDBObjectStore.
    js::Object* store = define_interface(in, "IDBObjectStore", nullptr);
    define_getter(in, *store, "name",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            // A finished transaction's handle keeps the name it had then.
            if ((*found)->transaction->state != IdbTransactionObject::State::Finished) {
                if (idb::StoreState* const state = store_state(**found))
                    (*found)->name = state->name;
            }
            return string_value(interp, (*found)->name);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native { return rename(interp, this_value, args, false); });
    define_getter(in, *store, "keyPath", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbObjectStoreObject& handle = **found;
        if (handle.key_path_value.is_undefined()) {
            idb::StoreState* const state = store_state(handle);
            handle.key_path_value = state != nullptr ? key_path_value(internals_of(interp), state->key_path) : js::Value::null();
        }
        return handle.key_path_value;
    });
    define_getter(in, *store, "indexNames", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        idb::StoreState* const state = store_state(**found);
        return string_list(internals_of(interp), state != nullptr ? state->index_names() : std::vector<std::u16string>());
    });
    define_getter(in, *store, "transaction", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::object((*found)->transaction);
    });
    define_getter(in, *store, "autoIncrement", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        idb::StoreState* const state = store_state(**found);
        return js::Value::boolean(state != nullptr && state->auto_increment);
    });
    define_operation(interpreter, *store, "put", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return add_or_put(interp, this_value, args, false);
    });
    define_operation(interpreter, *store, "add", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return add_or_put(interp, this_value, args, true);
    });
    auto const write_request = [](js::Interpreter& interp, js::Value const& this_value, Args args, bool clearing) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbObjectStoreObject& handle = **found;
        if (!clearing && args.empty())
            return interp.throw_type_error("Failed to execute 'delete' on 'IDBObjectStore': 1 argument required.");
        if (store_state(handle) == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "The object store has been deleted.");
        if (!active(*handle.transaction))
            return internals.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        if (handle.transaction->mode == idb::Mode::ReadOnly)
            return internals.throw_dom_exception("ReadOnlyError", "The transaction is read-only.");
        KeyRange range;
        if (!clearing) {
            std::optional<KeyRange> converted = to_range(internals, args[0], true);
            if (!converted)
                return std::nullopt;
            range = std::move(*converted);
        }
        std::uint64_t const store_id = handle.store_id;
        return js::Value::object(place_request(internals, *handle.transaction, this_value,
            [store_id, range, clearing](Realm::Internals& here, IdbTransactionObject& tx, IdbRequestObject&) {
                IdbRealm& state = realm_state(here);
                Lock const lock(state.storage->mutex());
                idb::StoreState* const target = tx.core->database->state.store(store_id);
                if (target == nullptr)
                    return;
                if (clearing)
                    idb::clear_records(*target, undo_of(tx));
                else
                    idb::delete_records(*target, range, undo_of(tx));
            }));
    };
    define_operation(interpreter, *store, "delete", 1, [write_request](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return write_request(interp, this_value, args, false);
    });
    define_operation(interpreter, *store, "clear", 0, [write_request](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return write_request(interp, this_value, args, true);
    });

    // The read methods a store and an index share.
    auto const define_reads = [&](js::Object& target, bool on_index) {
        define_operation(interpreter, target, "get", 1, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return read_request(interp, this_value, args, on_index, Read::Value, false, false);
        });
        define_operation(interpreter, target, "getKey", 1, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return read_request(interp, this_value, args, on_index, on_index ? Read::PrimaryKey : Read::Key, false, false);
        });
        define_operation(interpreter, target, "getAll", 0, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return read_request(interp, this_value, args, on_index, Read::Value, true, false);
        });
        define_operation(interpreter, target, "getAllKeys", 0, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return read_request(interp, this_value, args, on_index, Read::PrimaryKey, true, false);
        });
        define_operation(interpreter, target, "getAllRecords", 0, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return read_request(interp, this_value, args, on_index, Read::Record, true, false, true);
        });
        define_operation(interpreter, target, "count", 0, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return read_request(interp, this_value, args, on_index, Read::Value, false, true);
        });
        define_operation(interpreter, target, "openCursor", 0, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return open_cursor(interp, this_value, args, on_index, false);
        });
        define_operation(interpreter, target, "openKeyCursor", 0, [on_index](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            return open_cursor(interp, this_value, args, on_index, true);
        });
    };
    define_reads(*store, false);
    define_operation(interpreter, *store, "index", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'index' on 'IDBObjectStore': 1 argument required.");
        std::optional<js::JsString*> const text = interp.to_string(args[0]);
        if (!text)
            return std::nullopt;
        idb::StoreState* const state = store_state(**found);
        if (state == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "The object store has been deleted.");
        if ((*found)->transaction->state == IdbTransactionObject::State::Finished)
            return internals.throw_dom_exception("InvalidStateError", "The transaction has finished.");
        idb::IndexState* const index = state->index_named((*text)->data());
        if (index == nullptr)
            return internals.throw_dom_exception("NotFoundError", "No index has this name.");
        return js::Value::object(index_handle(internals, **found, index->id));
    });
    define_operation(interpreter, *store, "createIndex", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbObjectStoreObject& handle = **found;
        if (args.size() < 2)
            return interp.throw_type_error("Failed to execute 'createIndex' on 'IDBObjectStore': 2 arguments required.");
        std::optional<js::JsString*> const text = interp.to_string(args[0]);
        if (!text)
            return std::nullopt;
        std::u16string const name = (*text)->data();
        std::optional<KeyPath> const key_path = key_path_from(internals, args[1]);
        if (!key_path)
            return std::nullopt;
        js::Value const options = js::argument(args, 2);
        if (!options.is_nullish() && !options.is_object())
            return interp.throw_type_error("The options are not a dictionary.");
        bool multi_entry = false;
        bool unique = false;
        if (options.is_object()) {
            std::optional<js::Value> const multi = interp.get(options, "multiEntry");
            if (!multi)
                return std::nullopt;
            multi_entry = js::Interpreter::to_boolean(*multi);
            std::optional<js::Value> const is_unique = interp.get(options, "unique");
            if (!is_unique)
                return std::nullopt;
            unique = js::Interpreter::to_boolean(*is_unique);
        }
        IdbTransactionObject& tx = *handle.transaction;
        if (tx.mode != idb::Mode::VersionChange)
            return internals.throw_dom_exception("InvalidStateError", "Indexes are made only in an upgrade transaction.");
        idb::StoreState* const state = store_state(handle);
        if (state == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "The object store has been deleted.");
        if (!active(tx))
            return internals.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        if (state->index_named(name) != nullptr)
            return internals.throw_dom_exception("ConstraintError", "An index with this name already exists.");
        if (!key_path_valid(*key_path))
            return internals.throw_dom_exception("SyntaxError", "The keyPath argument is not a valid key path.");
        if (key_path->kind == KeyPath::Kind::Array && multi_entry)
            return internals.throw_dom_exception("InvalidAccessError", "A multiEntry index cannot have an array key path.");
        IdbRealm& realm = realm_state(internals);
        std::uint64_t id = 0;
        bool populated = true;
        {
            Lock const lock(realm.storage->mutex());
            idb::DatabaseState& schema = tx.core->database->state;
            id = schema.next_id++;
            idb::IndexState index;
            index.id = id;
            index.name = name;
            index.key_path = *key_path;
            index.unique = unique;
            index.multi_entry = multi_entry;
            idb::IndexState& made = state->indexes.emplace(id, std::move(index)).first->second;
            // The records the store has already, indexed now.
            std::vector<std::pair<Key, std::vector<Key>>> keys_by_record;
            for (auto const& [primary, record] : state->records) {
                js::Interpreter::Roots const roots(interp);
                js::Value const value = deserialize_value(internals, record.value);
                Converted converted = extract_key(internals, value, *key_path, multi_entry);
                if (converted.outcome == Outcome::Threw)
                    interp.clear_exception();
                if (converted.outcome != Outcome::Ok)
                    continue;
                std::vector<Key> keys;
                if (multi_entry && converted.key.type == Key::Type::Array)
                    keys = std::move(converted.key.array);
                else
                    keys.push_back(std::move(converted.key));
                if (!keys.empty())
                    keys_by_record.emplace_back(primary, std::move(keys));
            }
            populated = idb::populate_index(*state, made, keys_by_record);
        }
        js::Interpreter::Roots const roots(interp);
        IdbIndexObject* const index = index_handle(internals, handle, id);
        interp.root(js::Value::object(index));
        if (!populated) {
            // The index cannot be made as asked: the upgrade fails, once the
            // requests placed before it have had their events.
            Operation abort;
            abort.eager = false;
            abort.control = [](Realm::Internals& here, IdbTransactionObject& found_tx) {
                abort_transaction(here, found_tx, error_value(here, "ConstraintError", "A unique index would hold one key twice."));
            };
            tx.queue.push_back(std::move(abort));
            pump(internals, tx);
        }
        return js::Value::object(index);
    });
    define_operation(interpreter, *store, "deleteIndex", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbObjectStoreObject*> const found = this_of<IdbObjectStoreObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'deleteIndex' on 'IDBObjectStore': 1 argument required.");
        std::optional<js::JsString*> const text = interp.to_string(args[0]);
        if (!text)
            return std::nullopt;
        IdbTransactionObject& tx = *(*found)->transaction;
        if (tx.mode != idb::Mode::VersionChange)
            return internals.throw_dom_exception("InvalidStateError", "Indexes are deleted only in an upgrade transaction.");
        idb::StoreState* const state = store_state(**found);
        if (state == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "The object store has been deleted.");
        if (!active(tx))
            return internals.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        idb::IndexState* const index = state->index_named((*text)->data());
        if (index == nullptr)
            return internals.throw_dom_exception("NotFoundError", "No index has this name.");
        IdbRealm& realm = realm_state(internals);
        Lock const lock(realm.storage->mutex());
        state->indexes.erase(index->id);
        return js::Value::undefined();
    });

    // IDBIndex.
    js::Object* index = define_interface(in, "IDBIndex", nullptr);
    define_getter(in, *index, "name",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            if ((*found)->store->transaction->state != IdbTransactionObject::State::Finished) {
                if (idb::IndexState* const state = index_state(**found))
                    (*found)->name = state->name;
            }
            return string_value(interp, (*found)->name);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native { return rename(interp, this_value, args, true); });
    define_getter(in, *index, "objectStore", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::object((*found)->store);
    });
    define_getter(in, *index, "keyPath", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbIndexObject& handle = **found;
        if (handle.key_path_value.is_undefined()) {
            idb::IndexState* const state = index_state(handle);
            handle.key_path_value = state != nullptr ? key_path_value(internals_of(interp), state->key_path) : js::Value::null();
        }
        return handle.key_path_value;
    });
    define_getter(in, *index, "multiEntry", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        idb::IndexState* const state = index_state(**found);
        return js::Value::boolean(state != nullptr && state->multi_entry);
    });
    define_getter(in, *index, "unique", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbIndexObject*> const found = this_of<IdbIndexObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        idb::IndexState* const state = index_state(**found);
        return js::Value::boolean(state != nullptr && state->unique);
    });
    define_reads(*index, true);

    // IDBKeyRange.
    js::Object* key_range = define_interface(in, "IDBKeyRange", nullptr);
    auto const range_getter = [&](std::string_view name, int which) {
        define_getter(in, *key_range, name, [which](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<IdbKeyRangeObject*> const found = this_of<IdbKeyRangeObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            KeyRange const& range = (*found)->range;
            switch (which) {
            case 0:
                return range.lower ? key_value(internals_of(interp), *range.lower) : js::Value::undefined();
            case 1:
                return range.upper ? key_value(internals_of(interp), *range.upper) : js::Value::undefined();
            case 2:
                return js::Value::boolean(range.lower_open);
            default:
                return js::Value::boolean(range.upper_open);
            }
        });
    };
    range_getter("lower", 0);
    range_getter("upper", 1);
    range_getter("lowerOpen", 2);
    range_getter("upperOpen", 3);
    define_operation(interpreter, *key_range, "includes", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<IdbKeyRangeObject*> const found = this_of<IdbKeyRangeObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'includes' on 'IDBKeyRange': 1 argument required.");
        std::optional<Key> const key = key_argument(internals_of(interp), args[0]);
        if (!key)
            return std::nullopt;
        return js::Value::boolean((*found)->range.includes(*key));
    });
    js::Value const range_constructor = *interpreter.get(js::Value::object(global), interpreter.key("IDBKeyRange"));
    auto const make_range = [](js::Interpreter& interp, KeyRange range) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(interp.heap().allocate<IdbKeyRangeObject>(internals.prototype("IDBKeyRange"), std::move(range)));
    };
    define_operation(interpreter, *range_constructor.as_object(), "only", 1, [make_range](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'only' on 'IDBKeyRange': 1 argument required.");
        std::optional<Key> key = key_argument(internals_of(interp), args[0]);
        if (!key)
            return std::nullopt;
        return make_range(interp, KeyRange::only(std::move(*key)));
    });
    define_operation(interpreter, *range_constructor.as_object(), "lowerBound", 1, [make_range](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'lowerBound' on 'IDBKeyRange': 1 argument required.");
        std::optional<Key> key = key_argument(internals_of(interp), args[0]);
        if (!key)
            return std::nullopt;
        KeyRange range;
        range.lower = std::move(*key);
        range.lower_open = js::Interpreter::to_boolean(js::argument(args, 1));
        range.upper_open = true; // an unbounded side reads as open
        return make_range(interp, std::move(range));
    });
    define_operation(interpreter, *range_constructor.as_object(), "upperBound", 1, [make_range](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'upperBound' on 'IDBKeyRange': 1 argument required.");
        std::optional<Key> key = key_argument(internals_of(interp), args[0]);
        if (!key)
            return std::nullopt;
        KeyRange range;
        range.upper = std::move(*key);
        range.upper_open = js::Interpreter::to_boolean(js::argument(args, 1));
        range.lower_open = true;
        return make_range(interp, std::move(range));
    });
    define_operation(interpreter, *range_constructor.as_object(), "bound", 2, [make_range](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        if (args.size() < 2)
            return interp.throw_type_error("Failed to execute 'bound' on 'IDBKeyRange': 2 arguments required.");
        std::optional<Key> lower = key_argument(internals, args[0]);
        if (!lower)
            return std::nullopt;
        std::optional<Key> upper = key_argument(internals, args[1]);
        if (!upper)
            return std::nullopt;
        bool const lower_open = js::Interpreter::to_boolean(js::argument(args, 2));
        bool const upper_open = js::Interpreter::to_boolean(js::argument(args, 3));
        int const order = idb::compare(*lower, *upper);
        if (order > 0 || (order == 0 && (lower_open || upper_open)))
            return internals.throw_dom_exception("DataError", "The lower key is greater than the upper key, or the range is empty.");
        KeyRange range;
        range.lower = std::move(*lower);
        range.upper = std::move(*upper);
        range.lower_open = lower_open;
        range.upper_open = upper_open;
        return make_range(interp, std::move(range));
    });

    // IDBRecord.
    js::Object* record = define_interface(in, "IDBRecord", nullptr);
    auto const record_getter = [&](std::string_view name, js::Value IdbRecordObject::* member) {
        define_getter(in, *record, name, [member](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<IdbRecordObject*> const found = this_of<IdbRecordObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            return (**found).*member;
        });
    };
    record_getter("key", &IdbRecordObject::key);
    record_getter("primaryKey", &IdbRecordObject::primary_key);
    record_getter("value", &IdbRecordObject::value);

    // IDBCursor and IDBCursorWithValue.
    js::Object* cursor = define_interface(in, "IDBCursor", nullptr);
    define_getter(in, *cursor, "source", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::object((*found)->source());
    });
    define_getter(in, *cursor, "direction", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        return internals_of(interp).string(direction_name((*found)->direction));
    });
    define_getter(in, *cursor, "key", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbCursorObject& target = **found;
        if (!target.key)
            return js::Value::undefined();
        if (target.key_value.is_undefined())
            target.key_value = key_value(internals_of(interp), *target.key);
        return target.key_value;
    });
    define_getter(in, *cursor, "primaryKey", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbCursorObject& target = **found;
        if (!target.primary_key || !target.key)
            return js::Value::undefined();
        if (target.primary_key_value.is_undefined())
            target.primary_key_value = key_value(internals_of(interp), *target.primary_key);
        return target.primary_key_value;
    });
    define_getter(in, *cursor, "request", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->request != nullptr ? js::Value::object((*found)->request) : js::Value::null();
    });
    // The checks continue, advance and continuePrimaryKey begin with.
    auto const movable = [](Realm::Internals& internals, IdbCursorObject& target) -> bool {
        if (!active(*target.transaction)) {
            internals.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
            return false;
        }
        if (store_state(*target.store) == nullptr || (target.index != nullptr && index_state(*target.index) == nullptr)) {
            internals.throw_dom_exception("InvalidStateError", "The cursor's source has been deleted.");
            return false;
        }
        return true;
    };
    auto const move_cursor = [](Realm::Internals& internals, IdbCursorObject& target, std::optional<Key> key, std::optional<Key> primary,
                                 std::uint32_t count) {
        target.got_value = false;
        target.request->done = false;
        IdbCursorObject* const moving = &target;
        auto shared = std::make_shared<std::pair<std::optional<Key>, std::optional<Key>>>(std::move(key), std::move(primary));
        place_request(internals, *target.transaction, target.request->source,
            [moving, shared, count](Realm::Internals& here, IdbTransactionObject&, IdbRequestObject& moved) {
                iterate_cursor(here, *moving, moved, shared->first, shared->second, count);
            },
            { js::Value::object(&target) }, target.request, false);
    };
    define_operation(interpreter, *cursor, "advance", 1, [movable, move_cursor](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        if (args.empty())
            return interp.throw_type_error("Failed to execute 'advance' on 'IDBCursor': 1 argument required.");
        std::optional<double> const count = enforce_range(internals, args[0], 4294967295.0);
        if (!count)
            return std::nullopt;
        if (*count == 0)
            return interp.throw_type_error("A count argument with value 0 (zero) was supplied, must be greater than 0.");
        if (!movable(internals, **found))
            return std::nullopt;
        if (!(*found)->got_value)
            return internals.throw_dom_exception("InvalidStateError", "The cursor is being iterated or has iterated past its end.");
        move_cursor(internals, **found, std::nullopt, std::nullopt, static_cast<std::uint32_t>(*count));
        return js::Value::undefined();
    });
    define_operation(interpreter, *cursor, "continue", 0, [movable, move_cursor](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbCursorObject& target = **found;
        if (!movable(internals, target))
            return std::nullopt;
        if (!target.got_value)
            return internals.throw_dom_exception("InvalidStateError", "The cursor is being iterated or has iterated past its end.");
        std::optional<Key> key;
        if (!args.empty() && !args[0].is_undefined()) {
            key = key_argument(internals, args[0]);
            if (!key)
                return std::nullopt;
            int const order = target.position ? idb::compare(*key, *target.position) : 1;
            if (is_next(target.direction) ? order <= 0 : order >= 0)
                return internals.throw_dom_exception("DataError", "The key is not past the cursor's position in its direction.");
        }
        move_cursor(internals, target, std::move(key), std::nullopt, 1);
        return js::Value::undefined();
    });
    define_operation(interpreter, *cursor, "continuePrimaryKey", 2, [movable, move_cursor](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbCursorObject& target = **found;
        if (args.size() < 2)
            return interp.throw_type_error("Failed to execute 'continuePrimaryKey' on 'IDBCursor': 2 arguments required.");
        if (!movable(internals, target))
            return std::nullopt;
        if (target.index == nullptr)
            return internals.throw_dom_exception("InvalidAccessError", "The cursor's source is not an index.");
        if (target.direction != Direction::Next && target.direction != Direction::Prev)
            return internals.throw_dom_exception("InvalidAccessError", "The cursor's direction is not 'next' or 'prev'.");
        if (!target.got_value)
            return internals.throw_dom_exception("InvalidStateError", "The cursor is being iterated or has iterated past its end.");
        std::optional<Key> key = key_argument(internals, args[0]);
        if (!key)
            return std::nullopt;
        std::optional<Key> primary = key_argument(internals, args[1]);
        if (!primary)
            return std::nullopt;
        int const order = target.position ? idb::compare(*key, *target.position) : 1;
        int const primary_order = target.object_store_position ? idb::compare(*primary, *target.object_store_position) : 1;
        bool const forward = target.direction == Direction::Next;
        if (forward ? order < 0 : order > 0)
            return internals.throw_dom_exception("DataError", "The key is behind the cursor's position.");
        if (order == 0 && (forward ? primary_order <= 0 : primary_order >= 0))
            return internals.throw_dom_exception("DataError", "The primary key is not past the cursor's position.");
        move_cursor(internals, target, std::move(key), std::move(primary), 1);
        return js::Value::undefined();
    });
    // update() and delete(): the record the cursor is on.
    auto const cursor_write = [](js::Interpreter& interp, js::Value const& this_value, Args args, bool deleting) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found)
            return std::nullopt;
        IdbCursorObject& target = **found;
        IdbTransactionObject& tx = *target.transaction;
        if (!deleting && args.empty())
            return interp.throw_type_error("Failed to execute 'update' on 'IDBCursor': 1 argument required.");
        if (!active(tx))
            return internals.throw_dom_exception("TransactionInactiveError", "The transaction is not active.");
        if (tx.mode == idb::Mode::ReadOnly)
            return internals.throw_dom_exception("ReadOnlyError", "The transaction is read-only.");
        if (store_state(*target.store) == nullptr || (target.index != nullptr && index_state(*target.index) == nullptr))
            return internals.throw_dom_exception("InvalidStateError", "The cursor's source has been deleted.");
        if (!target.got_value)
            return internals.throw_dom_exception("InvalidStateError", "The cursor is being iterated or has iterated past its end.");
        if (target.key_only)
            return internals.throw_dom_exception("InvalidStateError", "The cursor is a key cursor.");
        Key const primary = *target.primary_key;
        std::uint64_t const store_id = target.store->store_id;
        if (deleting) {
            return js::Value::object(place_request(internals, tx, this_value,
                [store_id, primary](Realm::Internals& here, IdbTransactionObject& running_tx, IdbRequestObject&) {
                    IdbRealm& state = realm_state(here);
                    Lock const lock(state.storage->mutex());
                    if (idb::StoreState* const store_found = running_tx.core->database->state.store(store_id))
                        idb::delete_records(*store_found, KeyRange::only(primary), undo_of(running_tx));
                }));
        }
        js::Interpreter::Roots const roots(interp);
        std::optional<js::Value> const clone = clone_value(internals, tx, args[0]);
        if (!clone)
            return std::nullopt;
        idb::StoreState* const store_found = store_state(*target.store);
        if (store_found == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "The cursor's source has been deleted.");
        if (store_found->key_path.kind != KeyPath::Kind::None) {
            Converted converted = extract_key(internals, *clone, store_found->key_path);
            if (converted.outcome == Outcome::Threw)
                return std::nullopt;
            if (converted.outcome != Outcome::Ok || !idb::keys_equal(converted.key, primary))
                return internals.throw_dom_exception("DataError", "The value's key does not match the cursor's primary key.");
        }
        return js::Value::object(place_request(internals, tx, this_value,
            [store_id, primary](Realm::Internals& here, IdbTransactionObject& running_tx, IdbRequestObject& target_request) {
                js::Value const value = running_tx.executing != nullptr ? running_tx.executing->held.front() : js::Value::undefined();
                store_record(here, running_tx, target_request, store_id, value, primary, false);
            },
            { *clone }));
    };
    define_operation(interpreter, *cursor, "update", 1, [cursor_write](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return cursor_write(interp, this_value, args, false);
    });
    define_operation(interpreter, *cursor, "delete", 0, [cursor_write](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return cursor_write(interp, this_value, args, true);
    });
    js::Object* cursor_with_value = define_interface(in, "IDBCursorWithValue", cursor);
    define_getter(in, *cursor_with_value, "value", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IdbCursorObject*> const found = this_cursor(interp, this_value);
        if (!found || (*found)->key_only)
            return interp.throw_type_error("Illegal invocation");
        return (*found)->value;
    });
}

}
