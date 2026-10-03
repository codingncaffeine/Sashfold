// The Streams interfaces' place in the engine: Streams.js run as the
// engine's own code over each realm's global, and what the rest of the
// bindings ask of it — a byte stream over bytes in hand (a body, a Blob),
// and a stream read to its end.

#include "bindings/Internal.h"
#include "bindings/StreamsSource.h"

#include <iostream>
#include <string>

namespace sashfold::bindings {

void install_streams(Realm::Internals& in)
{
    // The program's text, put together from its pieces once.
    static std::string const source = [] {
        std::string whole;
        for (std::string_view const part : streams_source_parts)
            whole += part;
        return whole;
    }();
    js::Interpreter& interpreter = in.interpreter;
    js::Outcome const outcome = interpreter.run_script(source, "streams", true);
    if (!outcome.ok || !js::Interpreter::is_callable(outcome.value)) {
        std::cerr << "streams: the interfaces did not install: " << interpreter.describe(outcome.value) << "\n";
        return;
    }
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(outcome.value);
    js::Value const arguments[] = { js::Value::object(interpreter.global()) };
    std::optional<js::Value> const hooks = interpreter.call(outcome.value, js::Value::undefined(), arguments);
    if (!hooks || !hooks->is_object()) {
        std::cerr << "streams: the interfaces did not install: " << interpreter.describe(interpreter.take_exception()) << "\n";
        return;
    }
    in.streams = hooks->as_object();

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
    if (!in.streams || !value.is_object())
        return false;
    js::Value const arguments[] = { value };
    Native const answer = call_streams_hook(in, "isReadableStream", arguments);
    return answer && answer->is_boolean() && answer->as_boolean();
}

}
