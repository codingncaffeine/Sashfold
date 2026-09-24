#include "bindings/Internal.h"

// Structured clone (HTML §2.7): a value serialized in the realm of the call
// into records of C++ data alone — nothing a collection could free while the
// records wait in a task — and made again later in the realm that receives
// it, with that realm's intrinsics. structuredClone does both at once;
// window.postMessage and MessagePort.postMessage serialize at the call and
// deserialize at delivery; history.pushState and replaceState keep a clone of
// their state. What cannot be cloned is a DataCloneError DOMException.

#include "js/Object.h"
#include "js/Runtime.h"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::bindings {

// The serialized form, StructuredSerializeInternal's records: one record for
// each value, an object's children named by their records' indices, so that
// an object reached twice is one record and a cycle is an index back to an
// earlier one; and the transfer list's data holders, in the list's order.
struct SerializedMessage {
    enum class Kind : std::uint8_t {
        Undefined,
        Null,
        Boolean,
        Number,
        BigInt,
        String,
        BooleanObject,
        NumberObject,
        BigIntObject,
        StringObject,
        Date,
        RegExp,
        ArrayBuffer,
        ArrayBufferView,
        Map,
        Set,
        Error,
        DomException,
        Array,
        Object,
        Blob,
        ImageData, // its size in `byte_offset` and `buffer`, its pixels in `bytes`
        Transferred, // an entry of the transfer list: made from its data holder
    };
    struct Record {
        Kind kind = Kind::Undefined;
        bool boolean = false;
        double number = 0; // a Number, a Date's time value, a File's lastModified
        js::BigInteger bigint;
        std::u16string text; // a string, a RegExp's source, an error's message
        std::u16string name; // a RegExp's flags, an error's name
        std::u16string stack; // an error's
        bool has_message = false;
        std::vector<std::uint8_t> bytes; // an ArrayBuffer's or a Blob's
        std::optional<std::size_t> max_byte_length; // a resizable ArrayBuffer's
        // An ArrayBufferView: its buffer's record, its element type or a
        // DataView, where it starts, and its length — elements for a typed
        // array, bytes for a DataView — or none when it tracks its buffer.
        std::size_t buffer = 0;
        js::ElementType element_type = js::ElementType::Uint8;
        bool data_view = false;
        std::size_t byte_offset = 0;
        std::optional<std::size_t> length;
        // A Blob, or a File with its name.
        std::string type;
        std::string file_name;
        bool is_file = false;
        std::uint32_t array_length = 0;
        // An array's or an object's own enumerable properties, in order.
        std::vector<std::pair<std::u16string, std::size_t>> properties;
        // A Map's keys and values in turn, a Set's values.
        std::vector<std::size_t> entries;
        std::optional<std::size_t> cause; // an error's own cause
        std::size_t transfer = 0; // Transferred: the data holder's index
    };
    // A transferred ArrayBuffer's bytes; or the MessagePort detached for the
    // transfer, which whoever holds the message keeps alive until it arrives;
    // or, in a message for another agent, the end of the channel that port
    // became (Tasks.cpp), which holds nothing of any heap.
    struct TransferHolder {
        std::vector<std::uint8_t> bytes;
        std::optional<std::size_t> max_byte_length;
        MessagePortObject* port = nullptr;
        std::shared_ptr<PortChannel> channel;
        int channel_end = 0;
    };
    std::vector<Record> records;
    std::size_t root = 0; // the value's record
    std::vector<TransferHolder> transfers;
};

namespace {

using Kind = SerializedMessage::Kind;
using Record = SerializedMessage::Record;

// The names an error keeps (StructuredSerializeInternal's [[ErrorData]]
// step), in js::ErrorType's order; any other name is "Error".
constexpr std::array<std::u16string_view, 7> error_names = {
    u"Error", u"EvalError", u"RangeError", u"ReferenceError", u"SyntaxError", u"TypeError", u"URIError"
};

std::nullopt_t data_clone_error(Realm::Internals& in, std::string const& message)
{
    in.throw_dom_exception("DataCloneError", message);
    return std::nullopt;
}

// Every realm of the agent `in` runs in, from its page down.
std::vector<Realm::Internals*> agent_realms(Realm::Internals& in)
{
    Realm::Internals* page = &in;
    while (page->parent_realm != nullptr)
        page = page->parent_realm;
    std::vector<Realm::Internals*> found;
    std::vector<Realm::Internals*> pending { page };
    while (!pending.empty()) {
        Realm::Internals* const at = pending.back();
        pending.pop_back();
        found.push_back(at);
        for (ChildFrame const& listed : at->child_frames)
            pending.push_back(&listed.realm->internals());
    }
    return found;
}

// A DOMException — an error whose chain reaches the DOMException prototype of
// a realm of the agent — is a platform object serialized by its name and
// message, a subclass's instance as the DOMException it is.
bool is_dom_exception(Realm::Internals& in, js::Object const& object)
{
    std::vector<Realm::Internals*> const realms = agent_realms(in);
    for (js::Object const* prototype = object.prototype(); prototype != nullptr && !prototype->is_proxy(); prototype = prototype->prototype()) {
        for (Realm::Internals const* realm : realms) {
            if (realm->prototype("DOMException") == prototype)
                return true;
        }
    }
    return false;
}

std::u16string key_text(js::PropertyKey const& key)
{
    if (key.is_index()) {
        std::string const digits = std::to_string(key.as_index());
        return std::u16string(digits.begin(), digits.end());
    }
    return key.as_atom()->data();
}

// StructuredSerializeInternal, with its memory. Every object the memory
// names is rooted until the whole value is serialized, since the memory
// knows it by its address and a getter may drop the last reference to it.
class Serializer {
public:
    Serializer(Realm::Internals& in, SerializedMessage& message)
        : m_in(in)
        , m_interp(in.interpreter)
        , m_message(message)
    {
    }

    std::size_t add(Kind kind)
    {
        m_message.records.emplace_back();
        m_message.records.back().kind = kind;
        return m_message.records.size() - 1;
    }
    Record& at(std::size_t index) { return m_message.records[index]; }
    std::optional<std::size_t> serialize(js::Value const& value);

    std::unordered_map<js::Object const*, std::size_t> memory;

private:
    std::optional<std::size_t> refuse(js::Value const& value)
    {
        return data_clone_error(m_in, m_interp.describe(value) + " could not be cloned.");
    }
    std::optional<std::size_t> serialize_object(js::Object&, js::Value const&);
    bool serialize_error(js::Object&, std::size_t index);
    bool serialize_entries(js::CollectionObject&, std::size_t index, bool is_map);
    bool serialize_properties(js::Object&, std::size_t index);

    Realm::Internals& m_in;
    js::Interpreter& m_interp;
    SerializedMessage& m_message;
};

std::optional<std::size_t> Serializer::serialize(js::Value const& value)
{
    switch (value.type()) {
    case js::Value::Type::Undefined:
    case js::Value::Type::Empty:
        return add(Kind::Undefined);
    case js::Value::Type::Null:
        return add(Kind::Null);
    case js::Value::Type::Boolean: {
        std::size_t const index = add(Kind::Boolean);
        at(index).boolean = value.as_boolean();
        return index;
    }
    case js::Value::Type::Number: {
        std::size_t const index = add(Kind::Number);
        at(index).number = value.as_number();
        return index;
    }
    case js::Value::Type::BigInt: {
        std::size_t const index = add(Kind::BigInt);
        at(index).bigint = value.as_bigint()->value();
        return index;
    }
    case js::Value::Type::String: {
        std::size_t const index = add(Kind::String);
        at(index).text = value.as_string()->data();
        return index;
    }
    case js::Value::Type::Symbol:
        return refuse(value);
    case js::Value::Type::Object:
        break;
    }
    js::Object& object = *value.as_object();
    auto const known = memory.find(&object);
    if (known != memory.end())
        return known->second;
    // A structure nested past the stack's budget is the RangeError a
    // recursion in script would be.
    if (!m_interp.stack_ok())
        return std::nullopt;
    m_interp.root(value);
    return serialize_object(object, value);
}

std::optional<std::size_t> Serializer::serialize_object(js::Object& object, js::Value const& value)
{
    using Class = js::Object::Class;
    // Functions, and callable proxies with them.
    if (object.is_callable())
        return refuse(value);
    switch (object.class_id()) {
    case Class::Boolean:
    case Class::Number:
    case Class::BigInt:
    case Class::String: {
        auto const* wrapper = dynamic_cast<js::PrimitiveObject const*>(&object);
        if (wrapper == nullptr)
            return refuse(value);
        js::Value const primitive = wrapper->primitive();
        Class const class_id = object.class_id();
        std::size_t const index = add(class_id == Class::Boolean ? Kind::BooleanObject
                : class_id == Class::Number                      ? Kind::NumberObject
                : class_id == Class::BigInt                      ? Kind::BigIntObject
                                                                 : Kind::StringObject);
        Record& record = at(index);
        if (class_id == Class::Boolean)
            record.boolean = primitive.as_boolean();
        else if (class_id == Class::Number)
            record.number = primitive.as_number();
        else if (class_id == Class::BigInt)
            record.bigint = primitive.as_bigint()->value();
        else
            record.text = primitive.as_string()->data();
        memory[&object] = index;
        return index;
    }
    case Class::Date: {
        auto const* date = dynamic_cast<js::DateObject const*>(&object);
        if (date == nullptr)
            return refuse(value);
        std::size_t const index = add(Kind::Date);
        at(index).number = date->time_value();
        memory[&object] = index;
        return index;
    }
    case Class::RegExp: {
        // The original source and flags; lastIndex is not kept.
        auto const* regexp = dynamic_cast<js::RegExpObject const*>(&object);
        if (regexp == nullptr)
            return refuse(value);
        std::size_t const index = add(Kind::RegExp);
        at(index).text = regexp->source()->data();
        at(index).name = regexp->flags()->data();
        memory[&object] = index;
        return index;
    }
    case Class::ArrayBuffer: {
        // The bytes copied, and a resizable buffer's maximum with them.
        auto const& buffer = static_cast<js::ArrayBufferObject const&>(object);
        if (buffer.is_detached())
            return data_clone_error(m_in, "An ArrayBuffer is detached and could not be cloned.");
        std::size_t const index = add(Kind::ArrayBuffer);
        Record& record = at(index);
        if (buffer.byte_length() > 0)
            record.bytes.assign(buffer.data(), buffer.data() + buffer.byte_length());
        record.max_byte_length = buffer.max_byte_length();
        memory[&object] = index;
        return index;
    }
    case Class::TypedArray:
    case Class::DataView: {
        // A view out of bounds cannot be cloned; one in bounds is its buffer's
        // record — shared with every other view of it in the value — and
        // where it looks.
        bool const data_view = object.class_id() == Class::DataView;
        auto const* typed = data_view ? nullptr : &static_cast<js::TypedArrayObject const&>(object);
        auto const* window = data_view ? &static_cast<js::DataViewObject const&>(object) : nullptr;
        if (data_view ? window->is_out_of_bounds() : typed->is_out_of_bounds())
            return data_clone_error(m_in, std::string(data_view ? "A DataView" : "A TypedArray") + " out of its buffer's bounds could not be cloned.");
        js::ArrayBufferObject* const viewed = data_view ? window->buffer() : typed->buffer();
        std::optional<std::size_t> const buffer = serialize(js::Value::object(viewed));
        if (!buffer)
            return std::nullopt;
        std::size_t const index = add(Kind::ArrayBufferView);
        Record& record = at(index);
        record.buffer = *buffer;
        record.data_view = data_view;
        if (data_view) {
            record.byte_offset = window->byte_offset();
            if (!window->is_length_tracking())
                record.length = window->view_byte_length();
        } else {
            record.element_type = typed->element_type();
            record.byte_offset = typed->byte_offset();
            if (!typed->is_length_tracking())
                record.length = typed->length();
        }
        memory[&object] = index;
        return index;
    }
    case Class::Map:
    case Class::Set: {
        bool const is_map = object.class_id() == Class::Map;
        std::size_t const index = add(is_map ? Kind::Map : Kind::Set);
        memory[&object] = index;
        if (!serialize_entries(static_cast<js::CollectionObject&>(object), index, is_map))
            return std::nullopt;
        return index;
    }
    case Class::Error: {
        std::size_t const index = add(Kind::Error);
        if (!serialize_error(object, index))
            return std::nullopt;
        memory[&object] = index;
        if (at(index).kind == Kind::DomException)
            return index;
        // The cause, an own data property, is the accompanying data the
        // standard asks a user agent to keep; it is cloned as any value,
        // after the error is in the memory, so a cause may be the error.
        std::optional<std::optional<js::PropertyDescriptor>> const cause
            = m_interp.get_own_property(object, js::PropertyKey::atom(m_interp.atoms().cause));
        if (!cause)
            return std::nullopt;
        if (*cause && (*cause)->is_data()) {
            js::Value const cause_value = m_interp.root((*cause)->value.value_or(js::Value::undefined()));
            std::optional<std::size_t> const child = serialize(cause_value);
            if (!child)
                return std::nullopt;
            at(index).cause = *child;
        }
        return index;
    }
    case Class::Array: {
        std::size_t const index = add(Kind::Array);
        at(index).array_length = static_cast<js::ArrayObject const&>(object).length();
        memory[&object] = index;
        if (!serialize_properties(object, index))
            return std::nullopt;
        return index;
    }
    case Class::Host: {
        // The serializable platform objects: Blob and File, their bytes
        // copied, and ImageData, its pixels copied. Every other one — a
        // Response, a node, a port outside the transfer list — cannot be
        // cloned.
        if (std::optional<ImageDataCopy> image = image_data_copy(object)) {
            std::size_t const index = add(Kind::ImageData);
            Record& record = at(index);
            record.byte_offset = static_cast<std::size_t>(image->width);
            record.buffer = static_cast<std::size_t>(image->height);
            record.bytes = std::move(image->bytes);
            memory[&object] = index;
            return index;
        }
        auto const* blob = dynamic_cast<BlobObject const*>(&object);
        if (blob == nullptr)
            return refuse(value);
        std::size_t const index = add(Kind::Blob);
        Record& record = at(index);
        record.bytes = blob->bytes;
        record.type = blob->type;
        record.is_file = blob->is_file;
        record.file_name = blob->name;
        record.number = blob->last_modified;
        memory[&object] = index;
        return index;
    }
    case Class::Object:
    case Class::Math:
    case Class::Json: {
        // A platform object the bindings build as an ordinary object is not
        // serializable either; an ordinary object is, whatever its prototype.
        if (dynamic_cast<PlainPlatformObject const*>(&object) != nullptr)
            return refuse(value);
        std::size_t const index = add(Kind::Object);
        memory[&object] = index;
        if (!serialize_properties(object, index))
            return std::nullopt;
        return index;
    }
    default:
        // An object with internal slots of its own (a WeakMap, a promise, an
        // iterator, a symbol's wrapper, arguments), an exotic object (a
        // proxy, a module namespace) or the window itself.
        return refuse(value);
    }
}

bool Serializer::serialize_error(js::Object& object, std::size_t index)
{
    if (is_dom_exception(m_in, object)) {
        at(index).kind = Kind::DomException;
        std::optional<js::Value> const name = m_interp.get(object, m_interp.key("name"));
        if (!name)
            return false;
        std::optional<js::JsString*> const name_text = m_interp.to_string(m_interp.root(*name));
        if (!name_text)
            return false;
        m_interp.root(js::Value::string(*name_text));
        std::optional<js::Value> const message = m_interp.get(object, js::PropertyKey::atom(m_interp.atoms().message));
        if (!message)
            return false;
        std::optional<js::JsString*> const message_text = m_interp.to_string(m_interp.root(*message));
        if (!message_text)
            return false;
        at(index).name = (*name_text)->data();
        at(index).text = (*message_text)->data();
        at(index).has_message = true;
        // Its stack goes with it, as an error's does: the clone says where
        // the exception was made, not where the clone was.
        if (object.is_error()) {
            if (js::JsString const* const stack = static_cast<js::ErrorObject&>(object).stack())
                at(index).stack = stack->data();
        }
        return true;
    }
    // The name, when it is one of the seven; the own message, when it is a
    // data property, as a string; the stack.
    std::optional<js::Value> const name = m_interp.get(object, m_interp.key("name"));
    if (!name)
        return false;
    std::u16string name_text(error_names[0]);
    if (name->is_string()) {
        for (std::u16string_view const candidate : error_names) {
            if (name->as_string()->equals(candidate))
                name_text = candidate;
        }
    }
    std::optional<std::optional<js::PropertyDescriptor>> const message
        = m_interp.get_own_property(object, js::PropertyKey::atom(m_interp.atoms().message));
    if (!message)
        return false;
    std::optional<std::u16string> message_text;
    if (*message && (*message)->is_data()) {
        js::Value const message_value = m_interp.root((*message)->value.value_or(js::Value::undefined()));
        std::optional<js::JsString*> const text = m_interp.to_string(message_value);
        if (!text)
            return false;
        message_text = (*text)->data();
    }
    Record& record = at(index);
    record.name = std::move(name_text);
    record.has_message = message_text.has_value();
    record.text = message_text.value_or(std::u16string());
    if (auto const* error = dynamic_cast<js::ErrorObject const*>(&object); error != nullptr && error->stack() != nullptr)
        record.stack = error->stack()->data();
    return true;
}

bool Serializer::serialize_entries(js::CollectionObject& collection, std::size_t index, bool is_map)
{
    // The entries are copied first, so that a getter run while one of them
    // serializes cannot change which are cloned.
    std::vector<js::Value> copied;
    for (js::CollectionTable::Entry const& entry : collection.table().entries()) {
        if (!entry.live)
            continue;
        copied.push_back(m_interp.root(entry.key));
        if (is_map)
            copied.push_back(m_interp.root(entry.value));
    }
    for (js::Value const& item : copied) {
        std::optional<std::size_t> const child = serialize(item);
        if (!child)
            return false;
        at(index).entries.push_back(*child);
    }
    return true;
}

bool Serializer::serialize_properties(js::Object& object, std::size_t index)
{
    // EnumerableOwnProperties(value, key) taken once; each key still an own
    // property when its turn comes is read with [[Get]] and serialized.
    std::optional<std::vector<js::PropertyKey>> const keys = m_interp.own_keys(object);
    if (!keys)
        return false;
    std::vector<js::PropertyKey> enumerable;
    for (js::PropertyKey const& key : *keys) {
        if (key.is_symbol())
            continue;
        std::optional<std::optional<js::PropertyDescriptor>> const descriptor = m_interp.get_own_property(object, key);
        if (!descriptor)
            return false;
        if (*descriptor && (*descriptor)->enumerable.value_or(false))
            enumerable.push_back(key);
    }
    for (js::PropertyKey const& key : enumerable) {
        std::optional<std::optional<js::PropertyDescriptor>> const still = m_interp.get_own_property(object, key);
        if (!still)
            return false;
        if (!*still)
            continue;
        std::optional<js::Value> const property = m_interp.get(object, key);
        if (!property)
            return false;
        std::optional<std::size_t> const child = serialize(m_interp.root(*property));
        if (!child)
            return false;
        at(index).properties.emplace_back(key_text(key), *child);
    }
    return true;
}

// A value that cannot be made in the receiving realm — a buffer past what the
// engine allocates — is a DataCloneError there.
std::nullopt_t deserialization_failed(Realm::Internals& target)
{
    target.interpreter.clear_exception();
    return data_clone_error(target, "The serialized value could not be deserialized.");
}

} // namespace

std::shared_ptr<SerializedMessage const> structured_serialize(Realm::Internals& in, js::Value const& value, std::span<js::Value const> transfer)
{
    // StructuredSerializeWithTransfer: the transfer list checked and put in the
    // memory first, so that the value refers to its entries rather than
    // copying them; the value serialized; then each entry detached.
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(value);
    auto message = std::make_shared<SerializedMessage>();
    Serializer serializer(in, *message);
    for (std::size_t i = 0; i < transfer.size(); ++i) {
        js::Object* const entry = transfer[i].is_object() ? transfer[i].as_object() : nullptr;
        bool const transferable = entry != nullptr
            && (entry->class_id() == js::Object::Class::ArrayBuffer || dynamic_cast<MessagePortObject*>(entry) != nullptr);
        if (!transferable) {
            data_clone_error(in, "Value at index " + std::to_string(i) + " does not have a transferable type.");
            return nullptr;
        }
        if (serializer.memory.contains(entry)) {
            data_clone_error(in, "Value at index " + std::to_string(i) + " is a duplicate of an earlier value.");
            return nullptr;
        }
        interp.root(transfer[i]);
        std::size_t const record = serializer.add(Kind::Transferred);
        message->records[record].transfer = i;
        serializer.memory[entry] = record;
    }
    std::optional<std::size_t> const root = serializer.serialize(value);
    if (!root)
        return nullptr;
    message->root = *root;
    for (std::size_t i = 0; i < transfer.size(); ++i) {
        SerializedMessage::TransferHolder holder;
        if (auto* port = dynamic_cast<MessagePortObject*>(transfer[i].as_object())) {
            if (port->detached) {
                data_clone_error(in, "MessagePort at index " + std::to_string(i) + " is already detached.");
                return nullptr;
            }
            port->detached = true;
            holder.port = port;
        } else {
            auto& buffer = static_cast<js::ArrayBufferObject&>(*transfer[i].as_object());
            if (buffer.is_detached()) {
                data_clone_error(in, "ArrayBuffer at index " + std::to_string(i) + " is already detached.");
                return nullptr;
            }
            if (buffer.byte_length() > 0)
                holder.bytes.assign(buffer.data(), buffer.data() + buffer.byte_length());
            holder.max_byte_length = buffer.max_byte_length();
            buffer.detach();
        }
        message->transfers.push_back(std::move(holder));
    }
    return message;
}

std::shared_ptr<SerializedMessage const> message_for_another_agent(std::shared_ptr<SerializedMessage const> const& message)
{
    bool holds_a_port = false;
    for (SerializedMessage::TransferHolder const& holder : message->transfers)
        holds_a_port = holds_a_port || holder.port != nullptr;
    if (!holds_a_port)
        return message;
    auto copy = std::make_shared<SerializedMessage>(*message);
    for (SerializedMessage::TransferHolder& holder : copy->transfers) {
        if (holder.port == nullptr)
            continue;
        ChannelEnd const end = channel_end_of(*holder.port);
        holder.port = nullptr;
        holder.channel = end.channel;
        holder.channel_end = end.end;
    }
    return copy;
}

std::shared_ptr<SerializedMessage const> structured_serialize_for_another_agent(Realm::Internals& in, js::Value const& value,
    std::span<js::Value const> transfer)
{
    std::shared_ptr<SerializedMessage const> const message = structured_serialize(in, value, transfer);
    return message ? message_for_another_agent(message) : nullptr;
}

std::optional<Deserialized> structured_deserialize(Realm::Internals& target, SerializedMessage const& message)
{
    // StructuredDeserializeWithTransfer into the target's realm: the
    // transferred values first, then every record made — views once their
    // buffers are — then the children filled in. Nothing in it runs script.
    js::Interpreter& interp = target.interpreter;
    js::Heap& heap = interp.heap();
    js::Interpreter::RealmScope const scope(interp, target.realm_record);
    js::Intrinsics const& intrinsics = target.realm_record->intrinsics;
    Deserialized result;
    {
        js::Interpreter::Roots const roots(interp);
        for (SerializedMessage::TransferHolder const& holder : message.transfers) {
            if (holder.port != nullptr) {
                // The port's transfer-receiving steps (HTML §9.5.3): a new port
                // here, entangled with the one the detached port was, and
                // taking every message in its queue, in order — one already in
                // a task for the detached port among them.
                MessagePortObject* const detached = holder.port;
                auto* port = heap.allocate<MessagePortObject>(target.prototype("MessagePort"), *target.realm_record);
                result.transferred.push_back(interp.root(js::Value::object(port)));
                MessagePortObject* const remote = detached->entangled;
                detached->entangled = nullptr;
                if (remote != nullptr) {
                    remote->entangled = port;
                    port->entangled = remote;
                }
                port->pending = std::move(detached->pending);
                detached->pending.clear();
                // Entangled with another agent's port: the new port is the
                // channel's end from now on.
                if (detached->channel) {
                    std::shared_ptr<PortChannel> const channel = detached->channel;
                    int const end = detached->channel_end;
                    release_channel_end(*detached);
                    entangle_with_channel(*port, channel, end);
                }
                continue;
            }
            if (holder.channel) {
                // A port from another agent: a new port here, entangled with
                // the channel's end that travelled in its place.
                auto* port = heap.allocate<MessagePortObject>(target.prototype("MessagePort"), *target.realm_record);
                result.transferred.push_back(interp.root(js::Value::object(port)));
                entangle_with_channel(*port, holder.channel, holder.channel_end);
                continue;
            }
            std::optional<double> maximum;
            if (holder.max_byte_length)
                maximum = static_cast<double>(*holder.max_byte_length);
            std::optional<js::ArrayBufferObject*> const buffer
                = js::allocate_array_buffer(interp, nullptr, static_cast<double>(holder.bytes.size()), maximum);
            if (!buffer)
                return deserialization_failed(target);
            if (!holder.bytes.empty())
                std::memcpy((*buffer)->data(), holder.bytes.data(), holder.bytes.size());
            result.transferred.push_back(interp.root(js::Value::object(*buffer)));
        }

        std::vector<js::Value> values(message.records.size());
        for (std::size_t index = 0; index < message.records.size(); ++index) {
            Record const& record = message.records[index];
            js::Value made;
            switch (record.kind) {
            case Kind::Undefined:
            case Kind::ArrayBufferView:
                break;
            case Kind::Null:
                made = js::Value::null();
                break;
            case Kind::Boolean:
                made = js::Value::boolean(record.boolean);
                break;
            case Kind::Number:
                made = js::Value::number(record.number);
                break;
            case Kind::BigInt:
                made = js::Value::bigint(heap.bigint(record.bigint));
                break;
            case Kind::String:
                made = js::Value::string(interp.string(std::u16string_view(record.text)));
                break;
            case Kind::BooleanObject:
                made = js::Value::object(heap.allocate<js::PrimitiveObject>(intrinsics.boolean_prototype, js::Object::Class::Boolean, js::Value::boolean(record.boolean)));
                break;
            case Kind::NumberObject:
                made = js::Value::object(heap.allocate<js::PrimitiveObject>(intrinsics.number_prototype, js::Object::Class::Number, js::Value::number(record.number)));
                break;
            case Kind::BigIntObject: {
                js::Heap::NoCollect const no_collect(heap);
                js::Value const primitive = js::Value::bigint(heap.bigint(record.bigint));
                made = js::Value::object(heap.allocate<js::PrimitiveObject>(intrinsics.bigint_prototype, js::Object::Class::BigInt, primitive));
                break;
            }
            case Kind::StringObject: {
                js::Heap::NoCollect const no_collect(heap);
                js::JsString* const text = interp.string(std::u16string_view(record.text));
                made = js::Value::object(heap.allocate<js::StringObject>(intrinsics.string_prototype, text));
                break;
            }
            case Kind::Date:
                made = js::Value::object(heap.allocate<js::DateObject>(intrinsics.date_prototype, record.number));
                break;
            case Kind::RegExp: {
                js::Value const source = interp.root(js::Value::string(interp.string(std::u16string_view(record.text))));
                js::Value const flags = interp.root(js::Value::string(interp.string(std::u16string_view(record.name))));
                std::optional<js::Value> const regexp = js::create_regexp(interp, source, flags);
                if (!regexp)
                    return deserialization_failed(target);
                made = *regexp;
                break;
            }
            case Kind::ArrayBuffer: {
                std::optional<double> maximum;
                if (record.max_byte_length)
                    maximum = static_cast<double>(*record.max_byte_length);
                std::optional<js::ArrayBufferObject*> const buffer
                    = js::allocate_array_buffer(interp, nullptr, static_cast<double>(record.bytes.size()), maximum);
                if (!buffer)
                    return deserialization_failed(target);
                if (!record.bytes.empty())
                    std::memcpy((*buffer)->data(), record.bytes.data(), record.bytes.size());
                made = js::Value::object(*buffer);
                break;
            }
            case Kind::Map:
                made = js::Value::object(heap.allocate<js::CollectionObject>(intrinsics.map_prototype, js::Object::Class::Map));
                break;
            case Kind::Set:
                made = js::Value::object(heap.allocate<js::CollectionObject>(intrinsics.set_prototype, js::Object::Class::Set));
                break;
            case Kind::Error: {
                // The prototype of the error's name, the message when it had
                // one, the stack.
                std::size_t type = 0;
                for (std::size_t k = 0; k < error_names.size(); ++k) {
                    if (record.name == error_names[k])
                        type = k;
                }
                js::Heap::NoCollect const no_collect(heap);
                auto* error = heap.allocate<js::ErrorObject>(intrinsics.error_prototypes[type]);
                if (record.has_message)
                    error->put(js::PropertyKey::atom(interp.atoms().message), js::Value::string(interp.string(std::u16string_view(record.text))), js::builtin_attributes);
                error->set_stack(interp.string(std::u16string_view(record.stack)));
                made = js::Value::object(error);
                break;
            }
            case Kind::DomException:
                target.throw_dom_exception(encode_utf8(record.name), encode_utf8(record.text));
                // Taken, the exception is held by nothing: rooted here,
                // since the stack's string is an allocation and may collect.
                made = interp.root(interp.take_exception());
                if (!record.stack.empty() && made.is_object() && made.as_object()->is_error())
                    static_cast<js::ErrorObject*>(made.as_object())->set_stack(interp.string(std::u16string_view(record.stack)));
                break;
            case Kind::Array: {
                auto* array = heap.allocate<js::ArrayObject>(intrinsics.array_prototype);
                array->set_length(record.array_length);
                made = js::Value::object(array);
                break;
            }
            case Kind::Object:
                made = js::Value::object(interp.new_object(intrinsics.object_prototype));
                break;
            case Kind::Blob: {
                // Made from the realm's own interfaces, which a script deleting
                // the global Blob or File does not reach.
                auto* blob = heap.allocate<BlobObject>(target.prototype(record.is_file ? "File" : "Blob"));
                blob->bytes = record.bytes;
                blob->type = record.type;
                blob->is_file = record.is_file;
                blob->name = record.file_name;
                blob->last_modified = record.number;
                made = js::Value::object(blob);
                break;
            }
            case Kind::ImageData: {
                ImageDataCopy copy;
                copy.width = static_cast<int>(record.byte_offset);
                copy.height = static_cast<int>(record.buffer);
                copy.bytes = record.bytes;
                std::optional<js::Value> const image = image_data_from_copy(target, copy);
                if (!image)
                    return std::nullopt;
                made = *image;
                break;
            }
            case Kind::Transferred:
                made = result.transferred[record.transfer];
                break;
            }
            values[index] = interp.root(made);
        }

        for (std::size_t index = 0; index < message.records.size(); ++index) {
            Record const& record = message.records[index];
            if (record.kind != Kind::ArrayBufferView)
                continue;
            auto* buffer = static_cast<js::ArrayBufferObject*>(values[record.buffer].as_object());
            js::Object* view = nullptr;
            if (record.data_view)
                view = heap.allocate<js::DataViewObject>(intrinsics.data_view_prototype, buffer, record.byte_offset, record.length);
            else
                view = heap.allocate<js::TypedArrayObject>(intrinsics.typed_array_prototypes[static_cast<std::size_t>(record.element_type)],
                    record.element_type, buffer, record.byte_offset, record.length);
            values[index] = interp.root(js::Value::object(view));
        }

        for (std::size_t index = 0; index < message.records.size(); ++index) {
            Record const& record = message.records[index];
            switch (record.kind) {
            case Kind::Map:
            case Kind::Set: {
                js::CollectionTable& table = static_cast<js::CollectionObject*>(values[index].as_object())->table();
                bool const is_map = record.kind == Kind::Map;
                for (std::size_t k = 0; k < record.entries.size(); k += is_map ? 2 : 1) {
                    js::Value const& key = values[record.entries[k]];
                    table.set(key, is_map ? values[record.entries[k + 1]] : key);
                }
                break;
            }
            case Kind::Error:
                if (record.cause)
                    values[index].as_object()->put(js::PropertyKey::atom(interp.atoms().cause), values[*record.cause], js::builtin_attributes);
                break;
            case Kind::Array:
            case Kind::Object:
                for (auto const& [key, child] : record.properties) {
                    if (!interp.create_data_property(*values[index].as_object(), interp.key(std::u16string_view(key)), values[child]))
                        return std::nullopt;
                }
                break;
            default:
                break;
            }
        }
        result.value = values[message.root];
    }
    // Rooted again for the caller's scope, nothing allocated since the
    // records' roots were let go.
    interp.root(result.value);
    for (js::Value const& transferred : result.transferred)
        interp.root(transferred);
    return result;
}

void trace_transferred_ports(js::Tracer& tracer, SerializedMessage const& message)
{
    for (SerializedMessage::TransferHolder const& holder : message.transfers)
        tracer.visit(holder.port);
}

std::vector<MessagePortObject*> transferred_ports(SerializedMessage const& message)
{
    std::vector<MessagePortObject*> ports;
    for (SerializedMessage::TransferHolder const& holder : message.transfers) {
        if (holder.port != nullptr)
            ports.push_back(holder.port);
    }
    return ports;
}

std::optional<std::vector<js::Value>> transfer_sequence(Realm::Internals& in, js::Value const& value)
{
    // A WebIDL sequence<object>: any iterable, each of its values an object.
    js::Interpreter& interp = in.interpreter;
    if (!value.is_object())
        return interp.throw_type_error("The transfer list is not a sequence.");
    std::optional<std::vector<js::Value>> list = interp.iterable_to_list(value);
    if (!list)
        return std::nullopt;
    // IteratorToList roots the values only while it runs. They are rooted
    // again, before anything allocates, for the caller's scope: a caller goes
    // on converting its arguments — window.postMessage reads targetOrigin
    // after transfer, and a getter there may drop the last reference to an
    // entry and collect — before the list is serialized.
    for (js::Value const& entry : *list)
        interp.root(entry);
    for (js::Value const& entry : *list) {
        if (!entry.is_object())
            return interp.throw_type_error("A value in the transfer list is not an object.");
    }
    return list;
}

std::optional<std::vector<js::Value>> options_transfer(Realm::Internals& in, js::Value const& options)
{
    // A StructuredSerializeOptions dictionary: absent, or an object whose
    // transfer member is an empty list unless given.
    js::Interpreter& interp = in.interpreter;
    if (options.is_nullish())
        return std::vector<js::Value> {};
    if (!options.is_object())
        return interp.throw_type_error("The provided value is not of type 'StructuredSerializeOptions'.");
    std::optional<js::Value> const transfer = interp.get(*options.as_object(), interp.key("transfer"));
    if (!transfer)
        return std::nullopt;
    if (transfer->is_undefined())
        return std::vector<js::Value> {};
    interp.root(*transfer);
    return transfer_sequence(in, *transfer);
}

std::optional<std::vector<js::Value>> transfer_or_options(Realm::Internals& in, js::Value const& argument)
{
    // postMessage(message, transfer) against postMessage(message, options),
    // told apart as WebIDL's overload resolution does: an object with an
    // @@iterator method is the sequence, anything else the dictionary.
    js::Interpreter& interp = in.interpreter;
    if (argument.is_object()) {
        std::optional<js::Value> const method = interp.get_method(argument, js::PropertyKey::symbol(interp.atoms().symbol_iterator));
        if (!method)
            return std::nullopt;
        if (!method->is_undefined())
            return transfer_sequence(in, argument);
    }
    return options_transfer(in, argument);
}

Native structured_clone(js::Interpreter& interp, js::Value const&, Args args)
{
    // structuredClone(value, options): deserialized into this's relevant
    // realm, which is the current one — the window's member wrapper enters
    // the realm of the window this is before the method runs.
    Realm::Internals& in = internals_of(interp);
    Realm::Internals& target = in;
    js::Interpreter::Roots const roots(interp);
    std::optional<std::vector<js::Value>> const transfer = options_transfer(in, js::argument(args, 1));
    if (!transfer)
        return std::nullopt;
    std::shared_ptr<SerializedMessage const> const message = structured_serialize(in, js::argument(args, 0), *transfer);
    if (!message)
        return std::nullopt;
    std::optional<Deserialized> const clone = structured_deserialize(target, *message);
    if (!clone)
        return std::nullopt;
    return clone->value;
}

// --- The serialization as bytes ------------------------------------------------------------------

namespace {

class ByteWriter {
public:
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u64(std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void f64(double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        u64(bits);
    }
    void text(std::u16string const& value)
    {
        u64(value.size());
        for (char16_t const unit : value) {
            bytes.push_back(static_cast<std::uint8_t>(unit & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>(unit >> 8));
        }
    }
    void utf8(std::string const& value)
    {
        u64(value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void blob(std::vector<std::uint8_t> const& value)
    {
        u64(value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void optional_size(std::optional<std::size_t> const& value)
    {
        u8(value ? 1 : 0);
        u64(value.value_or(0));
    }
    std::vector<std::uint8_t> bytes;
};

class ByteReader {
public:
    explicit ByteReader(std::span<std::uint8_t const> bytes)
        : m_bytes(bytes)
    {
    }
    bool ok() const { return m_ok; }
    bool at_end() const { return m_at == m_bytes.size(); }
    bool has(std::uint64_t count)
    {
        if (m_bytes.size() - m_at < count)
            m_ok = false;
        return m_ok;
    }
    std::uint8_t u8() { return has(1) ? m_bytes[m_at++] : 0; }
    std::uint64_t u64()
    {
        if (!has(8))
            return 0;
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(m_bytes[m_at++]) << shift;
        return value;
    }
    std::size_t size() { return static_cast<std::size_t>(u64()); }
    double f64()
    {
        std::uint64_t const bits = u64();
        double value = 0;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    std::u16string text()
    {
        std::uint64_t const length = u64();
        if (length > m_bytes.size() || !has(length * 2))
            return {};
        std::u16string value;
        value.reserve(static_cast<std::size_t>(length));
        for (std::uint64_t i = 0; i < length; ++i) {
            value.push_back(static_cast<char16_t>(m_bytes[m_at] | (m_bytes[m_at + 1] << 8)));
            m_at += 2;
        }
        return value;
    }
    std::vector<std::uint8_t> blob()
    {
        std::uint64_t const length = u64();
        if (!has(length))
            return {};
        std::vector<std::uint8_t> value(m_bytes.begin() + static_cast<std::ptrdiff_t>(m_at),
            m_bytes.begin() + static_cast<std::ptrdiff_t>(m_at + length));
        m_at += static_cast<std::size_t>(length);
        return value;
    }
    std::string utf8()
    {
        std::vector<std::uint8_t> const value = blob();
        return std::string(value.begin(), value.end());
    }
    std::optional<std::size_t> optional_size()
    {
        bool const present = u8() != 0;
        std::size_t const value = size();
        return present ? std::optional<std::size_t>(value) : std::nullopt;
    }

private:
    std::span<std::uint8_t const> m_bytes;
    std::size_t m_at = 0;
    bool m_ok = true;
};

constexpr std::uint8_t serialized_format = 1;

}

std::vector<std::uint8_t> serialized_to_bytes(SerializedMessage const& message)
{
    ByteWriter out;
    out.u8(serialized_format);
    out.u64(message.root);
    out.u64(message.records.size());
    for (Record const& record : message.records) {
        out.u8(static_cast<std::uint8_t>(record.kind));
        out.u8(record.boolean ? 1 : 0);
        out.f64(record.number);
        out.utf8(record.bigint.to_string(16));
        out.text(record.text);
        out.text(record.name);
        out.text(record.stack);
        out.u8(record.has_message ? 1 : 0);
        out.blob(record.bytes);
        out.optional_size(record.max_byte_length);
        out.u64(record.buffer);
        out.u8(static_cast<std::uint8_t>(record.element_type));
        out.u8(record.data_view ? 1 : 0);
        out.u64(record.byte_offset);
        out.optional_size(record.length);
        out.utf8(record.type);
        out.utf8(record.file_name);
        out.u8(record.is_file ? 1 : 0);
        out.u64(record.array_length);
        out.u64(record.properties.size());
        for (auto const& [name, index] : record.properties) {
            out.text(name);
            out.u64(index);
        }
        out.u64(record.entries.size());
        for (std::size_t const entry : record.entries)
            out.u64(entry);
        out.optional_size(record.cause);
    }
    return std::move(out.bytes);
}

std::shared_ptr<SerializedMessage const> serialized_from_bytes(std::span<std::uint8_t const> bytes)
{
    ByteReader in(bytes);
    if (in.u8() != serialized_format)
        return nullptr;
    auto message = std::make_shared<SerializedMessage>();
    message->root = in.size();
    std::size_t const count = in.size();
    if (count > bytes.size())
        return nullptr;
    message->records.resize(count);
    for (Record& record : message->records) {
        std::uint8_t const kind = in.u8();
        if (kind >= static_cast<std::uint8_t>(Kind::Transferred))
            return nullptr;
        record.kind = static_cast<Kind>(kind);
        record.boolean = in.u8() != 0;
        record.number = in.f64();
        std::string const digits = in.utf8();
        std::string_view magnitude = digits;
        bool const negative = magnitude.starts_with('-');
        if (negative)
            magnitude.remove_prefix(1);
        if (magnitude != "0") {
            std::optional<js::BigInteger> const value = js::BigInteger::parse_digits(magnitude, 16);
            if (!value)
                return nullptr;
            record.bigint = negative ? value->negated() : *value;
        }
        record.text = in.text();
        record.name = in.text();
        record.stack = in.text();
        record.has_message = in.u8() != 0;
        record.bytes = in.blob();
        record.max_byte_length = in.optional_size();
        record.buffer = in.size();
        std::uint8_t const element_type = in.u8();
        if (element_type >= js::element_type_count)
            return nullptr;
        record.element_type = static_cast<js::ElementType>(element_type);
        record.data_view = in.u8() != 0;
        record.byte_offset = in.size();
        record.length = in.optional_size();
        record.type = in.utf8();
        record.file_name = in.utf8();
        record.is_file = in.u8() != 0;
        record.array_length = static_cast<std::uint32_t>(in.u64());
        std::size_t const properties = in.size();
        for (std::size_t i = 0; i < properties && in.ok(); ++i) {
            std::u16string name = in.text();
            std::size_t const index = in.size();
            if (index >= count)
                return nullptr;
            record.properties.emplace_back(std::move(name), index);
        }
        std::size_t const entries = in.size();
        for (std::size_t i = 0; i < entries && in.ok(); ++i) {
            std::size_t const entry = in.size();
            if (entry >= count)
                return nullptr;
            record.entries.push_back(entry);
        }
        record.cause = in.optional_size();
        if (record.kind == Kind::ArrayBufferView && record.buffer >= count)
            return nullptr;
        if (record.cause && *record.cause >= count)
            return nullptr;
        if (!in.ok())
            return nullptr;
    }
    if (!in.ok() || !in.at_end() || message->root >= count)
        return nullptr;
    // A view's buffer must be a buffer, as the deserializer takes it to be.
    for (Record const& record : message->records) {
        if (record.kind == Kind::ArrayBufferView && message->records[record.buffer].kind != Kind::ArrayBuffer)
            return nullptr;
    }
    return message;
}

}
