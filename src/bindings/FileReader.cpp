#include "bindings/Internal.h"

// FileReader (File API §6): a Blob read as an ArrayBuffer, a binary string,
// text or a data: URL, with the progress events a read fires. The bytes are
// in memory already (BlobObject), so a read is three tasks: one that says it
// has begun (loadstart), one for its one chunk (progress), one that hands the
// result over (load, loadend) — what the specification's steps come to for a
// blob read in one chunk. abort() stops a read in any gap.

#include "html/Encoding.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

namespace {

enum class ReadAs : std::uint8_t { ArrayBuffer, BinaryString, Text, DataUrl };

class FileReaderObject final : public EventTargetObject {
public:
    explicit FileReaderObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    enum State : int { Empty = 0, Loading = 1, Done = 2 };

    int state = Empty;
    js::Value result = js::Value::null();
    js::Value error = js::Value::null();
    // Bumped by every read and by abort(): a task of an older read is dropped.
    std::uint64_t generation = 0;

    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(result);
        tracer.visit(error);
    }
};

std::optional<FileReaderObject*> this_reader(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* reader = dynamic_cast<FileReaderObject*>(this_value.as_object()))
            return reader;
    }
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

void fire(Realm::Internals& in, FileReaderObject& reader, std::string_view type, double loaded, double total)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&reader));
    EventObject* event = in.new_event("ProgressEvent", type, false, false);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    event->length_computable = true;
    event->loaded = loaded;
    event->total = total;
    in.dispatch(*event, &reader);
}

// The charset parameter of a MIME type, if it has one.
std::optional<std::string> charset_of(std::string_view type)
{
    std::size_t at = 0;
    while ((at = type.find(';', at)) != std::string_view::npos) {
        ++at;
        while (at < type.size() && (type[at] == ' ' || type[at] == '\t'))
            ++at;
        std::size_t const equals = type.find('=', at);
        if (equals == std::string_view::npos)
            return std::nullopt;
        std::string name(type.substr(at, equals - at));
        for (char& c : name)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        std::size_t const end = type.find(';', equals);
        std::string_view value = type.substr(equals + 1, end == std::string_view::npos ? std::string_view::npos : end - equals - 1);
        if (!value.empty() && value.front() == '"' && value.size() >= 2 && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (name == "charset")
            return std::string(value);
    }
    return std::nullopt;
}

std::string base64(std::span<std::uint8_t const> bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        std::uint32_t const v = (std::uint32_t { bytes[i] } << 16) | (std::uint32_t { bytes[i + 1] } << 8) | bytes[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (bytes.size() - i == 1) {
        std::uint32_t const v = std::uint32_t { bytes[i] } << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (bytes.size() - i == 2) {
        std::uint32_t const v = (std::uint32_t { bytes[i] } << 16) | (std::uint32_t { bytes[i + 1] } << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// The package data steps (§6.4.4): the result the read asked for.
Native package(Realm::Internals& in, ReadAs kind, std::vector<std::uint8_t> const& bytes, std::string const& type, std::optional<std::string> const& label)
{
    js::Interpreter& interp = in.interpreter;
    switch (kind) {
    case ReadAs::ArrayBuffer: {
        std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(bytes.size()), std::nullopt);
        if (!buffer)
            return std::nullopt;
        std::copy(bytes.begin(), bytes.end(), (*buffer)->data());
        return js::Value::object(*buffer);
    }
    case ReadAs::BinaryString: {
        std::u16string text;
        text.reserve(bytes.size());
        for (std::uint8_t const byte : bytes)
            text.push_back(static_cast<char16_t>(byte));
        return js::Value::string(interp.string(text));
    }
    case ReadAs::Text: {
        // The encoding named by the call, else the blob's charset, else
        // UTF-8; a byte order mark overrides all three.
        html::Encoding encoding = html::Encoding::Utf8;
        std::optional<html::Encoding> named;
        if (label)
            named = html::encoding_from_label(*label);
        if (!named) {
            if (std::optional<std::string> const charset = charset_of(type))
                named = html::encoding_from_label(*charset);
        }
        if (named)
            encoding = *named;
        std::string_view view(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        html::SniffedEncoding const sniffed = html::sniff_encoding(view);
        if (sniffed.bom_length > 0) {
            encoding = sniffed.encoding;
            view.remove_prefix(sniffed.bom_length);
        }
        std::u16string text;
        for (char32_t const code_point : html::decode(view, encoding))
            js::append_code_point(text, code_point);
        return js::Value::string(interp.string(text));
    }
    case ReadAs::DataUrl:
        return in.string("data:" + (type.empty() ? std::string("application/octet-stream") : type) + ";base64," + base64(bytes));
    }
    return std::nullopt;
}

// The read operation (§6.4.4) over a Blob already in memory.
Native read(js::Interpreter& interp, js::Value const& this_value, Args args, ReadAs kind)
{
    std::optional<FileReaderObject*> const self = this_reader(interp, this_value);
    if (!self)
        return std::nullopt;
    js::Value const blob_value = js::argument(args, 0);
    auto const* blob = blob_value.is_object() ? dynamic_cast<BlobObject const*>(blob_value.as_object()) : nullptr;
    if (blob == nullptr)
        return interp.throw_type_error("Failed to execute 'read' on 'FileReader': parameter 1 is not of type 'Blob'.");
    std::optional<std::string> label;
    if (kind == ReadAs::Text && args.size() > 1 && !args[1].is_undefined()) {
        label = internals_of(interp).to_utf8(args[1]);
        if (!label)
            return std::nullopt;
    }
    Realm::Internals& internals = internals_of(interp);
    FileReaderObject& reader = **self;
    if (reader.state == FileReaderObject::Loading)
        return internals.throw_dom_exception("InvalidStateError", "The object is already busy reading Blobs.");
    reader.state = FileReaderObject::Loading;
    reader.result = js::Value::null();
    reader.error = js::Value::null();
    std::uint64_t const generation = ++reader.generation;
    auto bytes = std::make_shared<std::vector<std::uint8_t> const>(blob->bytes);
    auto held = std::make_shared<js::Persistent>(interp.heap(), this_value);
    std::string const type = blob->type;
    double const total = static_cast<double>(bytes->size());
    internals.post_task([&internals, held, generation, total] {
        // No script entry around the task: each listener is one, so the
        // microtasks a listener queued run before the next event (HTML
        // §8.1.4.4 "clean up after running script").
        Realm::Internals::HostEntry const host(internals.agent);
        auto& target = *static_cast<FileReaderObject*>(held->value().as_object());
        if (target.generation != generation)
            return;
        fire(internals, target, "loadstart", 0, total);
    });
    // The chunk's progress in a task of its own, and none for an empty blob,
    // which loads no data (§6.4.4 step 10.5).
    if (total > 0) {
        internals.post_task([&internals, held, generation, total] {
            // No script entry around the task: each listener is one, so the
        // microtasks a listener queued run before the next event (HTML
        // §8.1.4.4 "clean up after running script").
        Realm::Internals::HostEntry const host(internals.agent);
            auto& target = *static_cast<FileReaderObject*>(held->value().as_object());
            if (target.generation != generation)
                return;
            fire(internals, target, "progress", total, total);
        });
    }
    internals.post_task([&internals, held, generation, bytes, type, label, kind, total] {
        // No script entry around the task: each listener is one, so the
        // microtasks a listener queued run before the next event (HTML
        // §8.1.4.4 "clean up after running script").
        Realm::Internals::HostEntry const host(internals.agent);
        auto& target = *static_cast<FileReaderObject*>(held->value().as_object());
        if (target.generation != generation)
            return;
        Native const result = package(internals, kind, *bytes, type, label);
        if (!result) {
            internals.interpreter.clear_exception();
            target.state = FileReaderObject::Done;
            target.error = internals.make_dom_exception("NotReadableError", "The blob could not be read.");
            fire(internals, target, "error", 0, total);
        } else {
            target.state = FileReaderObject::Done;
            target.result = *result;
            fire(internals, target, "load", total, total);
        }
        // loadend only when no read began in the load handler.
        if (target.state != FileReaderObject::Loading)
            fire(internals, target, "loadend", total, total);
    });
    return js::Value::undefined();
}

}

void install_file_reader(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* reader = define_interface(in, "FileReader", in.prototype("EventTarget"),
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            return js::Value::object(interp.heap().allocate<FileReaderObject>(internals_of(interp).prototype("FileReader")));
        },
        0);
    define_operation(interpreter, *reader, "readAsArrayBuffer", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return read(interp, this_value, args, ReadAs::ArrayBuffer);
    });
    define_operation(interpreter, *reader, "readAsBinaryString", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return read(interp, this_value, args, ReadAs::BinaryString);
    });
    define_operation(interpreter, *reader, "readAsText", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return read(interp, this_value, args, ReadAs::Text);
    });
    define_operation(interpreter, *reader, "readAsDataURL", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return read(interp, this_value, args, ReadAs::DataUrl);
    });
    // abort() (§6.4.3): nothing for a reader that is not reading but the
    // result cleared; a read in progress ends with abort and loadend.
    define_operation(interpreter, *reader, "abort", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<FileReaderObject*> const self = this_reader(interp, this_value);
        if (!self)
            return std::nullopt;
        FileReaderObject& target = **self;
        if (target.state != FileReaderObject::Loading) {
            target.result = js::Value::null();
            return js::Value::undefined();
        }
        target.state = FileReaderObject::Done;
        target.result = js::Value::null();
        ++target.generation;
        Realm::Internals& internals = internals_of(interp);
        fire(internals, target, "abort", 0, 0);
        if (target.state != FileReaderObject::Loading)
            fire(internals, target, "loadend", 0, 0);
        return js::Value::undefined();
    });
    define_getter(in, *reader, "readyState", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<FileReaderObject*> const self = this_reader(interp, this_value);
        if (!self)
            return std::nullopt;
        return js::Value::number((*self)->state);
    });
    define_getter(in, *reader, "result", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<FileReaderObject*> const self = this_reader(interp, this_value);
        if (!self)
            return std::nullopt;
        return (*self)->result;
    });
    define_getter(in, *reader, "error", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<FileReaderObject*> const self = this_reader(interp, this_value);
        if (!self)
            return std::nullopt;
        return (*self)->error;
    });
    static constexpr std::string_view events[] = { "loadstart", "progress", "load", "abort", "error", "loadend" };
    define_event_handlers(in, *reader, events);
    js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key("FileReader"));
    for (js::Object* target : { constructor.as_object(), reader }) {
        for (auto const& [name, value] : { std::pair { "EMPTY", 0 }, std::pair { "LOADING", 1 }, std::pair { "DONE", 2 } })
            target->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
}

}
