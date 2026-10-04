// The Streams interfaces' place in the engine: Streams.js run as the
// engine's own code over a realm's global, and what the rest of the
// bindings ask of it — a byte stream over bytes in hand (a body, a Blob),
// and a stream read to its end.
//
// The script is not run when a realm is made. Its interfaces are on the
// global from the start as names whose values are made at first look
// (js::Object::put_lazy_value), and the first look at any of them, or the
// engine's own first need of a stream, runs the script once for the realm:
// most pages never touch a stream, and the script is the largest single
// thing a realm would otherwise set up.

#include "bindings/Internal.h"
#include "bindings/StreamsSource.h"

#include <iostream>
#include <string>

namespace sashfold::bindings {

namespace {

// The interfaces the script defines, in the order it defines them.
constexpr std::string_view stream_interfaces[] = {
    "ReadableStream", "ReadableStreamDefaultReader", "ReadableStreamBYOBReader", "ReadableStreamDefaultController",
    "ReadableByteStreamController", "ReadableStreamBYOBRequest", "WritableStream", "WritableStreamDefaultWriter",
    "WritableStreamDefaultController", "TransformStream", "TransformStreamDefaultController", "ByteLengthQueuingStrategy",
    "CountQueuingStrategy",
};
// The two it defines where the realm has the Encoding Standard's classes.
constexpr std::string_view text_encoder_stream = "TextEncoderStream";
constexpr std::string_view text_decoder_stream = "TextDecoderStream";

// What the script works with, copied as the realm was born with it. A
// built-in still a description is copied as the description, so keeping it
// makes no function; whatever a page later puts over the original, the
// copy is what the script gets.
class Primordials {
public:
    Primordials(Realm::Internals& in, js::Object& kept)
        : m_in(in)
        , m_interpreter(in.interpreter)
        , m_kept(kept)
    {
    }

    void value(std::string_view name, js::Value const& value) { m_kept.put(m_interpreter.key(name), value, js::builtin_attributes); }
    void object(std::string_view name, js::Object* object)
    {
        if (object != nullptr)
            value(name, js::Value::object(object));
    }
    // A data property of `holder` — a method, a constructor.
    void property(std::string_view name, js::Object* holder, std::string_view key)
    {
        js::Property const* const found = holder != nullptr ? holder->peek_own(m_interpreter.key(key)) : nullptr;
        if (found == nullptr || found->accessor)
            return;
        if (found->lazy == js::Property::LazyNative)
            m_kept.put_lazy(m_interpreter.key(name), *found->lazy_get, *found->lazy_realm, js::builtin_attributes);
        else if (found->lazy == js::Property::NotLazy)
            value(name, found->value);
    }
    // The getter of an accessor of `holder`, kept as a function to call.
    void getter(std::string_view name, js::Object* holder, js::PropertyKey const& key)
    {
        js::Property const* const found = holder != nullptr ? holder->peek_own(key) : nullptr;
        if (found == nullptr || !found->accessor)
            return;
        if (found->lazy == js::Property::LazyNative) {
            if (found->lazy_get != nullptr)
                m_kept.put_lazy(m_interpreter.key(name), *found->lazy_get, *found->lazy_realm, js::builtin_attributes);
        } else if (found->getter != nullptr) {
            value(name, js::Value::object(found->getter));
        }
    }
    void getter(std::string_view name, js::Object* holder, std::string_view key) { getter(name, holder, m_interpreter.key(key)); }
    // An interface attribute's getter, from wherever up the interface's
    // prototype chain it is an accessor; nothing where it is not one.
    void attribute(std::string_view name, std::string_view interface, std::string_view key)
    {
        for (js::Object* prototype = m_in.prototype(interface); prototype != nullptr; prototype = prototype->prototype()) {
            js::Property const* const found = prototype->peek_own(m_interpreter.key(key));
            if (found != nullptr && found->accessor) {
                getter(name, prototype, key);
                return;
            }
        }
    }
    js::Object* global_object(std::string_view name)
    {
        js::Property const* const found = m_interpreter.global()->peek_own(m_interpreter.key(name));
        bool const whole = found != nullptr && found->lazy == js::Property::NotLazy && !found->accessor && found->value.is_object();
        return whole ? found->value.as_object() : nullptr;
    }

private:
    Realm::Internals& m_in;
    js::Interpreter& m_interpreter;
    js::Object& m_kept;
};

void keep_primordials(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Intrinsics const& intrinsics = interpreter.intrinsics();
    js::WellKnownAtoms const& atoms = interpreter.atoms();
    js::Object* const kept = interpreter.heap().allocate<js::Object>(nullptr);
    in.stream_primordials = kept;
    Primordials keep(in, *kept);

    keep.object("Promise", intrinsics.promise_constructor);
    keep.property("promiseThen", intrinsics.promise_prototype, "then");
    keep.property("promiseResolve", intrinsics.promise_constructor, "resolve");
    keep.property("functionBind", intrinsics.function_prototype, "bind");
    keep.property("ReflectApply", keep.global_object("Reflect"), "apply");
    keep.property("ObjectDefineProperty", intrinsics.object_constructor, "defineProperty");
    keep.property("ObjectGetOwnPropertyNames", intrinsics.object_constructor, "getOwnPropertyNames");
    keep.property("ObjectGetOwnPropertyDescriptor", intrinsics.object_constructor, "getOwnPropertyDescriptor");
    keep.property("ObjectSetPrototypeOf", intrinsics.object_constructor, "setPrototypeOf");
    keep.property("ObjectCreate", intrinsics.object_constructor, "create");
    keep.object("TypeError", intrinsics.error_constructors[static_cast<std::size_t>(js::ErrorType::TypeError)]);
    keep.object("RangeError", intrinsics.error_constructors[static_cast<std::size_t>(js::ErrorType::RangeError)]);
    keep.object("ArrayBuffer", intrinsics.array_buffer_constructor);
    keep.property("ArrayBufferIsView", intrinsics.array_buffer_constructor, "isView");
    keep.property("arrayBufferSlice", intrinsics.array_buffer_prototype, "slice");
    keep.property("arrayBufferTransfer", intrinsics.array_buffer_prototype, "transfer");
    keep.getter("arrayBufferByteLength", intrinsics.array_buffer_prototype, "byteLength");
    keep.getter("arrayBufferDetached", intrinsics.array_buffer_prototype, "detached");
    keep.object("Uint8Array", keep.global_object("Uint8Array"));
    keep.object("DataView", intrinsics.data_view_constructor);
    keep.getter("typedArrayName", intrinsics.typed_array_prototype, js::PropertyKey::symbol(atoms.symbol_to_string_tag));
    keep.getter("typedArrayBuffer", intrinsics.typed_array_prototype, "buffer");
    keep.getter("typedArrayByteOffset", intrinsics.typed_array_prototype, "byteOffset");
    keep.getter("typedArrayByteLength", intrinsics.typed_array_prototype, "byteLength");
    keep.getter("typedArrayLength", intrinsics.typed_array_prototype, "length");
    keep.property("typedArraySet", intrinsics.typed_array_prototype, "set");
    keep.getter("dataViewBuffer", intrinsics.data_view_prototype, "buffer");
    keep.getter("dataViewByteOffset", intrinsics.data_view_prototype, "byteOffset");
    keep.getter("dataViewByteLength", intrinsics.data_view_prototype, "byteLength");
    keep.value("SymbolAsyncIterator", js::Value::symbol(atoms.symbol_async_iterator));
    keep.value("SymbolIterator", js::Value::symbol(atoms.symbol_iterator));
    keep.value("SymbolToStringTag", js::Value::symbol(atoms.symbol_to_string_tag));
    keep.property("NumberIsNaN", intrinsics.number_constructor, "isNaN");
    keep.property("MathMin", intrinsics.math, "min");
    keep.property("StringFromCharCode", intrinsics.string_constructor, "fromCharCode");
    keep.property("queueMicrotask", interpreter.global(), "queueMicrotask");
    keep.object("AbortController", keep.global_object("AbortController"));
    keep.object("AbortSignal", keep.global_object("AbortSignal"));
    keep.attribute("abortSignalAborted", "AbortSignal", "aborted");
    keep.attribute("abortSignalReason", "AbortSignal", "reason");
    keep.property("abortControllerAbort", in.prototype("AbortController"), "abort");
    keep.attribute("abortControllerSignal", "AbortController", "signal");
    keep.property("eventTargetAdd", in.prototype("EventTarget"), "addEventListener");
    keep.property("eventTargetRemove", in.prototype("EventTarget"), "removeEventListener");
    keep.object("TextEncoder", keep.global_object("TextEncoder"));
    keep.object("TextDecoder", keep.global_object("TextDecoder"));
    keep.property("textEncoderEncode", in.prototype("TextEncoder"), "encode");
    keep.property("textDecoderDecode", in.prototype("TextDecoder"), "decode");
    keep.object("AsyncIteratorPrototype", intrinsics.async_iterator_prototype);
    // The typed array constructors by name.
    js::Object* const typed_arrays = interpreter.heap().allocate<js::Object>(nullptr);
    for (std::string_view const name : { "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
             "Float16Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array" }) {
        if (js::Object* const constructor = keep.global_object(name))
            typed_arrays->put(interpreter.key(name), js::Value::object(constructor));
    }
    keep.object("typedArrayConstructors", typed_arrays);
}

// Runs the script for the realm, once. A failure is said once and the
// realm goes without streams.
void load_streams(Realm::Internals& in)
{
    if (in.streams != nullptr || in.streams_loading || in.streams_failed)
        return;
    // The program's text, put together from its pieces once.
    static std::string const source = [] {
        std::string whole;
        for (std::string_view const part : streams_source_parts)
            whole += part;
        return whole;
    }();
    js::Interpreter& interpreter = in.interpreter;
    in.streams_loading = true;
    struct Done {
        Realm::Internals& in;
        ~Done()
        {
            in.streams_loading = false;
            in.streams_failed = in.streams == nullptr;
        }
    } const done { in };
    // Its global declarations and its closures are this realm's, whichever
    // realm's code asked.
    js::Interpreter::RealmScope const inside(interpreter, in.realm_record);
    js::Interpreter::Roots const roots(interpreter);
    js::Outcome const outcome = interpreter.run_script(source, "streams", true);
    if (!outcome.ok || !js::Interpreter::is_callable(outcome.value)) {
        std::cerr << "streams: the interfaces did not install: " << interpreter.describe(outcome.value) << "\n";
        if (!outcome.ok)
            interpreter.clear_exception();
        return;
    }
    interpreter.root(outcome.value);
    js::Value const arguments[] = { js::Value::object(interpreter.global()),
        in.stream_primordials != nullptr ? js::Value::object(in.stream_primordials) : js::Value::undefined() };
    std::optional<js::Value> const hooks = interpreter.call(outcome.value, js::Value::undefined(), arguments);
    if (!hooks || !hooks->is_object()) {
        std::cerr << "streams: the interfaces did not install: " << interpreter.describe(interpreter.take_exception()) << "\n";
        return;
    }
    in.streams = hooks->as_object();
}

// One of the script's interfaces looked at for the first time: the script
// runs, and puts the interface where this stood.
js::Value stream_interface(js::Object& global, js::PropertyKey const& key, js::RealmRecord& realm)
{
    auto* const host = static_cast<Realm*>(realm.host_defined);
    if (host == nullptr)
        return js::Value::undefined();
    load_streams(host->internals());
    js::Property const* const now = global.peek_own(key);
    return now != nullptr && now->lazy == js::Property::NotLazy && !now->accessor ? now->value : js::Value::undefined();
}

}

void install_streams(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    keep_primordials(in);
    if (!js::lazy_natives()) {
        load_streams(in);
    } else {
        js::Object& global = *interpreter.global();
        js::RealmRecord& realm = *interpreter.current_realm();
        for (std::string_view const name : stream_interfaces)
            global.put_lazy_value(interpreter.key(name), stream_interface, realm, js::builtin_attributes);
        if (global.peek_own(interpreter.key("TextEncoder")) != nullptr)
            global.put_lazy_value(interpreter.key(text_encoder_stream), stream_interface, realm, js::builtin_attributes);
        if (global.peek_own(interpreter.key("TextDecoder")) != nullptr)
            global.put_lazy_value(interpreter.key(text_decoder_stream), stream_interface, realm, js::builtin_attributes);
    }

    // Blob.stream() (File API §3.3.4): the blob's bytes as a byte stream.
    if (js::Object* blob = in.prototype("Blob")) {
        define_operation(interpreter, *blob, "stream", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            auto* found = this_value.is_object() ? dynamic_cast<BlobObject*>(this_value.as_object()) : nullptr;
            if (!found)
                return interp.throw_type_error("Illegal invocation");
            return bytes_stream(internals_of(interp), found->bytes);
        });
    }
}

Native call_streams_hook(Realm::Internals& in, std::string_view name, std::span<js::Value const> arguments)
{
    js::Interpreter& interpreter = in.interpreter;
    // The engine's own first need of a stream.
    load_streams(in);
    if (!in.streams)
        return interpreter.throw_type_error("Streams are not available");
    std::optional<js::Value> const hook = in.streams->get(interpreter, interpreter.key(name), js::Value::object(in.streams));
    if (!hook || !js::Interpreter::is_callable(*hook))
        return interpreter.throw_type_error("Streams are not available");
    return interpreter.call(*hook, js::Value::undefined(), arguments);
}

Native bytes_stream(Realm::Internals& in, std::span<std::uint8_t const> bytes)
{
    js::Interpreter::Roots const roots(in.interpreter);
    Native const array = uint8_array_of(in.interpreter, bytes);
    if (!array)
        return std::nullopt;
    in.interpreter.root(*array);
    js::Value const arguments[] = { *array };
    return call_streams_hook(in, "bytesStream", arguments);
}

bool is_readable_stream(Realm::Internals& in, js::Value const& value)
{
    // No stream can have been made in a realm whose script has not run.
    if (!in.streams || !value.is_object())
        return false;
    js::Value const arguments[] = { value };
    Native const answer = call_streams_hook(in, "isReadableStream", arguments);
    return answer && answer->is_boolean() && answer->as_boolean();
}

}
