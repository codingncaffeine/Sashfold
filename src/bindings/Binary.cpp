#include "bindings/Internal.h"

// The page's binary-data interfaces over the engine's typed arrays:
// TextEncoder and TextDecoder (the Encoding Standard's UTF-8 decoder with
// its streaming and fatal modes, and the UTF-16 and windows-1252 decoders
// the HTML parser already has), and Blob with File — bytes with a type,
// sliced, and read back through promises. crypto.getRandomValues, which
// fills a typed array, is beside the rest of crypto in Window.cpp.

#include "core/Unicode.h"
#include "html/Encoding.h"
#include "js/Object.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

namespace {

// A BufferSource (WebIDL): the bytes an ArrayBuffer, a typed array or a
// DataView holds right now — none once detached or out of bounds — or
// nothing for any other value.
std::optional<std::span<std::uint8_t const>> buffer_source_bytes(js::Value const& value)
{
    if (!value.is_object())
        return std::nullopt;
    js::Object& object = *value.as_object();
    switch (object.class_id()) {
    case js::Object::Class::ArrayBuffer: {
        auto& buffer = static_cast<js::ArrayBufferObject&>(object);
        return std::span<std::uint8_t const>(buffer.data(), buffer.byte_length());
    }
    case js::Object::Class::TypedArray: {
        auto& array = static_cast<js::TypedArrayObject&>(object);
        if (array.is_out_of_bounds())
            return std::span<std::uint8_t const>();
        return std::span<std::uint8_t const>(array.buffer()->data() + array.byte_offset(), array.byte_length());
    }
    case js::Object::Class::DataView: {
        auto& view = static_cast<js::DataViewObject&>(object);
        if (view.is_out_of_bounds())
            return std::span<std::uint8_t const>();
        return std::span<std::uint8_t const>(view.buffer()->data() + view.byte_offset(), view.view_byte_length());
    }
    default:
        return std::nullopt;
    }
}

// The Encoding Standard's UTF-8 encoder: a lone surrogate becomes U+FFFD.
std::string encode_utf8(std::u16string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        std::size_t units = 1;
        char32_t code_point = js::code_point_at(text, i, &units);
        if (is_surrogate(code_point))
            code_point = 0xFFFD;
        append_utf8(out, code_point);
        i += units;
    }
    return out;
}

// A fresh Uint8Array holding a copy of the bytes.
Native uint8_array_of(js::Interpreter& interp, std::span<std::uint8_t const> bytes)
{
    std::optional<js::TypedArrayObject*> const array = js::new_typed_array(interp, js::ElementType::Uint8, static_cast<double>(bytes.size()));
    if (!array)
        return std::nullopt;
    if (!bytes.empty())
        std::memcpy((*array)->buffer()->data(), bytes.data(), bytes.size());
    return js::Value::object(*array);
}

// A promise already resolved with the value (PromiseResolve on the
// realm's Promise): what Blob's reading methods answer with.
Native resolved(js::Interpreter& interp, js::Value const& value)
{
    js::Interpreter::Roots const roots(interp);
    interp.root(value);
    return js::promise_resolve(interp, js::Value::object(interp.intrinsics().promise_constructor), value);
}

// --- TextEncoder / TextDecoder --------------------------------------------------------------

// A decoder's settings and, between streamed chunks, its state: the
// UTF-8 decoder's partial code point (the Encoding Standard's variables
// by name), the UTF-16 decoders' half unit and half pair, and whether the
// BOM has had its one chance to be skipped.
class TextDecoderObject final : public js::Object {
public:
    explicit TextDecoderObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }

    html::Encoding encoding = html::Encoding::Utf8;
    std::string name = "utf-8";
    bool fatal = false;
    bool ignore_bom = false;
    bool do_not_flush = false;
    bool bom_seen = false;
    char32_t code_point = 0;
    int bytes_seen = 0;
    int bytes_needed = 0;
    std::uint8_t lower = 0x80;
    std::uint8_t upper = 0xBF;
    std::optional<std::uint8_t> lead_byte;
    std::optional<char16_t> lead_surrogate;

    void reset_utf8()
    {
        code_point = 0;
        bytes_seen = 0;
        bytes_needed = 0;
        lower = 0x80;
        upper = 0xBF;
    }
    void reset()
    {
        bom_seen = false;
        reset_utf8();
        lead_byte.reset();
        lead_surrogate.reset();
    }
};

std::string_view encoding_name(html::Encoding encoding)
{
    switch (encoding) {
    case html::Encoding::Utf8:
        return "utf-8";
    case html::Encoding::Utf16Le:
        return "utf-16le";
    case html::Encoding::Utf16Be:
        return "utf-16be";
    case html::Encoding::Windows1252:
        return "windows-1252";
    case html::Encoding::XUserDefined:
        return "x-user-defined";
    }
    return "utf-8";
}

std::optional<TextDecoderObject*> this_decoder(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* decoder = dynamic_cast<TextDecoderObject*>(this_value.as_object()))
            return decoder;
    }
    return interp.throw_type_error("Illegal invocation");
}

// One byte through the Encoding Standard's UTF-8 decoder: the code point
// this byte finishes, or none; `error` marks an invalid sequence, and
// `reprocess` asks for the same byte again as the start of a new one.
std::optional<char32_t> utf8_step(TextDecoderObject& decoder, std::uint8_t byte, bool& error, bool& reprocess)
{
    error = false;
    reprocess = false;
    if (decoder.bytes_needed == 0) {
        if (byte <= 0x7F)
            return byte;
        if (byte >= 0xC2 && byte <= 0xDF) {
            decoder.bytes_needed = 1;
            decoder.code_point = static_cast<char32_t>(byte & 0x1F);
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            if (byte == 0xE0)
                decoder.lower = 0xA0;
            if (byte == 0xED)
                decoder.upper = 0x9F;
            decoder.bytes_needed = 2;
            decoder.code_point = static_cast<char32_t>(byte & 0x0F);
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            if (byte == 0xF0)
                decoder.lower = 0x90;
            if (byte == 0xF4)
                decoder.upper = 0x8F;
            decoder.bytes_needed = 3;
            decoder.code_point = static_cast<char32_t>(byte & 0x07);
        } else {
            error = true;
        }
        return std::nullopt;
    }
    if (byte < decoder.lower || byte > decoder.upper) {
        decoder.reset_utf8();
        error = true;
        reprocess = true;
        return std::nullopt;
    }
    decoder.lower = 0x80;
    decoder.upper = 0xBF;
    decoder.code_point = (decoder.code_point << 6) | static_cast<char32_t>(byte & 0x3F);
    if (++decoder.bytes_seen != decoder.bytes_needed)
        return std::nullopt;
    char32_t const finished = decoder.code_point;
    decoder.reset_utf8();
    return finished;
}

// TextDecoder.decode's loop over one chunk (the Encoding Standard's
// "decode" with the decoder kept in the object): U+FFFD or a TypeError
// per error, the BOM skipped once, an unfinished sequence kept for the
// next chunk while streaming and an error when flushing.
Native decode_bytes(js::Interpreter& interp, TextDecoderObject& decoder, std::span<std::uint8_t const> bytes, bool flush)
{
    std::u16string out;
    bool failed = false;
    bool const unicode = decoder.encoding == html::Encoding::Utf8 || decoder.encoding == html::Encoding::Utf16Le
        || decoder.encoding == html::Encoding::Utf16Be;
    auto const emit = [&](char32_t code_point) {
        if (unicode && !decoder.ignore_bom && !decoder.bom_seen) {
            decoder.bom_seen = true;
            if (code_point == 0xFEFF)
                return;
        }
        js::append_code_point(out, code_point);
    };
    auto const fail = [&]() -> bool {
        if (decoder.fatal) {
            failed = true;
            return false;
        }
        emit(0xFFFD);
        return true;
    };
    switch (decoder.encoding) {
    case html::Encoding::Utf8: {
        for (std::size_t i = 0; i < bytes.size() && !failed;) {
            bool error = false;
            bool reprocess = false;
            std::optional<char32_t> const code_point = utf8_step(decoder, bytes[i], error, reprocess);
            if (code_point)
                emit(*code_point);
            if (error && !fail())
                break;
            if (!reprocess)
                ++i;
        }
        if (!failed && flush && decoder.bytes_needed != 0) {
            decoder.reset_utf8();
            fail();
        }
        break;
    }
    case html::Encoding::Utf16Le:
    case html::Encoding::Utf16Be: {
        bool const little_endian = decoder.encoding == html::Encoding::Utf16Le;
        for (std::size_t i = 0; i < bytes.size() && !failed; ++i) {
            if (!decoder.lead_byte) {
                decoder.lead_byte = bytes[i];
                continue;
            }
            std::uint8_t const lead = *decoder.lead_byte;
            decoder.lead_byte.reset();
            char16_t const unit = little_endian ? static_cast<char16_t>((bytes[i] << 8) | lead) : static_cast<char16_t>((lead << 8) | bytes[i]);
            // A lead surrogate waiting for its trail: a pair when the unit
            // is one, else an error and the unit again on its own.
            for (int pass = 0; pass < 2 && !failed; ++pass) {
                if (decoder.lead_surrogate) {
                    char16_t const high = *decoder.lead_surrogate;
                    decoder.lead_surrogate.reset();
                    if (unit >= 0xDC00 && unit <= 0xDFFF) {
                        emit(0x10000 + ((static_cast<char32_t>(high) - 0xD800) << 10) + (static_cast<char32_t>(unit) - 0xDC00));
                        break;
                    }
                    if (!fail())
                        break;
                    continue;
                }
                if (unit >= 0xD800 && unit <= 0xDBFF)
                    decoder.lead_surrogate = unit;
                else if (unit >= 0xDC00 && unit <= 0xDFFF)
                    fail();
                else
                    emit(unit);
                break;
            }
        }
        if (!failed && flush && (decoder.lead_byte || decoder.lead_surrogate)) {
            decoder.lead_byte.reset();
            decoder.lead_surrogate.reset();
            fail();
        }
        break;
    }
    case html::Encoding::Windows1252: {
        std::string_view const text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        for (char32_t const code_point : html::decode(text, html::Encoding::Windows1252))
            emit(code_point);
        break;
    }
    case html::Encoding::XUserDefined:
        for (std::uint8_t const byte : bytes)
            emit(byte < 0x80 ? static_cast<char32_t>(byte) : static_cast<char32_t>(0xF780 + byte - 0x80));
        break;
    }
    if (failed) {
        decoder.reset();
        decoder.do_not_flush = false;
        return interp.throw_type_error("The encoded data was not valid for encoding " + decoder.name);
    }
    return js::Value::string(interp.string(out));
}

void install_text_coding(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    js::Object* encoder = define_interface(in, "TextEncoder", nullptr,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            return js::Value::object(interp.new_object(internals_of(interp).prototype("TextEncoder")));
        },
        0);
    define_getter(in, *encoder, "encoding", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).string("utf-8");
    });
    js::define_method(interpreter, *encoder, "encode", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        // encode(input = ""): the UTF-8 bytes of the string.
        js::Value const input = js::argument(args, 0);
        if (input.is_undefined())
            return uint8_array_of(interp, {});
        std::optional<js::JsString*> const text = interp.to_string(input);
        if (!text)
            return std::nullopt;
        std::string const bytes = encode_utf8((*text)->view());
        return uint8_array_of(interp, std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(bytes.data()), bytes.size()));
    });
    js::define_method(interpreter, *encoder, "encodeInto", 2, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        // encodeInto(source, destination): as many whole code points as
        // fit, and how many code units and bytes that was.
        std::optional<js::JsString*> const text = interp.to_string(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        js::Value const destination = js::argument(args, 1);
        if (!destination.is_object() || destination.as_object()->class_id() != js::Object::Class::TypedArray
            || static_cast<js::TypedArrayObject*>(destination.as_object())->element_type() != js::ElementType::Uint8)
            return interp.throw_type_error("Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.");
        auto& array = *static_cast<js::TypedArrayObject*>(destination.as_object());
        std::size_t const capacity = array.length();
        std::u16string_view const source = (*text)->view();
        std::size_t read = 0;
        std::size_t written = 0;
        while (read < source.size()) {
            std::size_t units = 1;
            char32_t code_point = js::code_point_at(source, read, &units);
            if (is_surrogate(code_point))
                code_point = 0xFFFD;
            std::string encoded;
            append_utf8(encoded, code_point);
            if (written + encoded.size() > capacity)
                break;
            std::memcpy(array.buffer()->data() + array.byte_offset() + written, encoded.data(), encoded.size());
            written += encoded.size();
            read += units;
        }
        js::Heap::NoCollect const no_collect(interp.heap());
        js::Object* result = interp.new_object();
        result->put(interp.key("read"), js::Value::number(static_cast<double>(read)));
        result->put(interp.key("written"), js::Value::number(static_cast<double>(written)));
        return js::Value::object(result);
    });

    js::Object* decoder = define_interface(in, "TextDecoder", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            // new TextDecoder(label = "utf-8", options): the label through
            // the Encoding Standard's table, the two flags from the options.
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            js::Value const options = js::argument(args, 1);
            interp.root(options);
            std::string label = "utf-8";
            if (!js::argument(args, 0).is_undefined()) {
                std::optional<std::string> const text = internals.to_utf8(args[0]);
                if (!text)
                    return std::nullopt;
                label = *text;
            }
            bool fatal = false;
            bool ignore_bom = false;
            if (options.is_object()) {
                std::optional<js::Value> const fatal_option = interp.get(*options.as_object(), interp.key("fatal"));
                if (!fatal_option)
                    return std::nullopt;
                fatal = js::Interpreter::to_boolean(*fatal_option);
                std::optional<js::Value> const bom_option = interp.get(*options.as_object(), interp.key("ignoreBOM"));
                if (!bom_option)
                    return std::nullopt;
                ignore_bom = js::Interpreter::to_boolean(*bom_option);
            }
            std::optional<html::Encoding> const encoding = html::encoding_from_label(label);
            if (!encoding)
                return interp.throw_range_error("Failed to construct 'TextDecoder': The encoding label provided ('" + label + "') is invalid.");
            js::Heap::NoCollect const no_collect(interp.heap());
            auto* object = interp.heap().allocate<TextDecoderObject>(internals.prototype("TextDecoder"));
            object->encoding = *encoding;
            object->name = std::string(encoding_name(*encoding));
            object->fatal = fatal;
            object->ignore_bom = ignore_bom;
            return js::Value::object(object);
        },
        0);
    define_getter(in, *decoder, "encoding", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TextDecoderObject*> const object = this_decoder(interp, this_value);
        if (!object)
            return std::nullopt;
        return internals_of(interp).string((*object)->name);
    });
    define_getter(in, *decoder, "fatal", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TextDecoderObject*> const object = this_decoder(interp, this_value);
        if (!object)
            return std::nullopt;
        return js::Value::boolean((*object)->fatal);
    });
    define_getter(in, *decoder, "ignoreBOM", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TextDecoderObject*> const object = this_decoder(interp, this_value);
        if (!object)
            return std::nullopt;
        return js::Value::boolean((*object)->ignore_bom);
    });
    js::define_method(interpreter, *decoder, "decode", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        // decode(input, options): a fresh decoder unless the last call
        // streamed; the bytes are read after the options, whose getters
        // may run script.
        std::optional<TextDecoderObject*> const found = this_decoder(interp, this_value);
        if (!found)
            return std::nullopt;
        TextDecoderObject& object = **found;
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        js::Value const input = js::argument(args, 0);
        interp.root(input);
        js::Value const options = js::argument(args, 1);
        bool stream = false;
        if (options.is_object()) {
            std::optional<js::Value> const stream_option = interp.get(*options.as_object(), interp.key("stream"));
            if (!stream_option)
                return std::nullopt;
            stream = js::Interpreter::to_boolean(*stream_option);
        }
        std::span<std::uint8_t const> bytes;
        if (!input.is_undefined()) {
            std::optional<std::span<std::uint8_t const>> const source = buffer_source_bytes(input);
            if (!source)
                return interp.throw_type_error("Failed to execute 'decode' on 'TextDecoder': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
            bytes = *source;
        }
        if (!object.do_not_flush)
            object.reset();
        object.do_not_flush = stream;
        return decode_bytes(interp, object, bytes, !stream);
    });
}

// --- Blob / File -------------------------------------------------------------------------

class BlobObject final : public js::Object {
public:
    explicit BlobObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }

    std::vector<std::uint8_t> bytes;
    std::string type;
    bool is_file = false;
    std::string name;
    double last_modified = 0;

    std::size_t size_in_bytes() const override { return sizeof(*this) + bytes.capacity(); }
};

std::optional<BlobObject*> this_blob(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* blob = dynamic_cast<BlobObject*>(this_value.as_object()))
            return blob;
    }
    return interp.throw_type_error("Illegal invocation");
}

// A blob's type: printable ASCII lowercased, or nothing (File API §3).
std::string normalize_type(std::string_view type)
{
    for (char const c : type) {
        if (c < 0x20 || c > 0x7E)
            return "";
    }
    return ascii_lower(type);
}

// The `type` member of a BlobPropertyBag, when the bag is an object.
std::optional<std::string> type_option(js::Interpreter& interp, js::Value const& options)
{
    if (!options.is_object())
        return std::string();
    std::optional<js::Value> const type = interp.get(*options.as_object(), interp.key("type"));
    if (!type)
        return std::nullopt;
    if (type->is_undefined())
        return std::string();
    std::optional<std::string> const text = internals_of(interp).to_utf8(*type);
    if (!text)
        return std::nullopt;
    return normalize_type(*text);
}

// "Process blob parts" (File API §3.1): each part a BufferSource, a Blob
// or a string, the last in UTF-8.
std::optional<bool> append_parts(js::Interpreter& interp, BlobObject& blob, js::Value const& parts)
{
    if (!parts.is_object())
        return interp.throw_type_error("Failed to construct 'Blob': The provided value cannot be converted to a sequence.");
    std::optional<std::vector<js::Value>> const list = interp.iterable_to_list(parts);
    if (!list)
        return std::nullopt;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&blob));
    for (js::Value const& part : *list)
        interp.root(part);
    for (js::Value const& part : *list) {
        if (std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(part)) {
            blob.bytes.insert(blob.bytes.end(), bytes->begin(), bytes->end());
            continue;
        }
        if (part.is_object()) {
            if (auto const* other = dynamic_cast<BlobObject const*>(part.as_object())) {
                blob.bytes.insert(blob.bytes.end(), other->bytes.begin(), other->bytes.end());
                continue;
            }
        }
        std::optional<js::JsString*> const text = interp.to_string(part);
        if (!text)
            return std::nullopt;
        std::string const encoded = encode_utf8((*text)->view());
        blob.bytes.insert(blob.bytes.end(), encoded.begin(), encoded.end());
    }
    return true;
}

void install_blob(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    js::Object* blob = define_interface(in, "Blob", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            // new Blob(parts = [], options): the parts gathered, the type
            // normalized; `endings` is left as it is.
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            for (js::Value const& argument_value : args)
                interp.root(argument_value);
            auto* object = interp.heap().allocate<BlobObject>(internals.prototype("Blob"));
            interp.root(js::Value::object(object));
            if (!js::argument(args, 0).is_undefined() && !append_parts(interp, *object, args[0]))
                return std::nullopt;
            std::optional<std::string> const type = type_option(interp, js::argument(args, 1));
            if (!type)
                return std::nullopt;
            object->type = *type;
            return js::Value::object(object);
        },
        0);
    define_getter(in, *blob, "size", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BlobObject*> const object = this_blob(interp, this_value);
        if (!object)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*object)->bytes.size()));
    });
    define_getter(in, *blob, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BlobObject*> const object = this_blob(interp, this_value);
        if (!object)
            return std::nullopt;
        return internals_of(interp).string((*object)->type);
    });
    js::define_method(interpreter, *blob, "slice", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        // slice(start, end, contentType): relative positions, both ends
        // clamped, into a fresh blob.
        std::optional<BlobObject*> const found = this_blob(interp, this_value);
        if (!found)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        double const size = static_cast<double>((*found)->bytes.size());
        auto const position = [&](js::Value const& argument_value, double fallback) -> std::optional<double> {
            if (argument_value.is_undefined())
                return fallback;
            std::optional<double> const relative = interp.to_integer_or_infinity(argument_value);
            if (!relative)
                return std::nullopt;
            if (*relative < 0)
                return std::max(size + *relative, 0.0);
            return std::min(*relative, size);
        };
        std::optional<double> const start = position(js::argument(args, 0), 0);
        if (!start)
            return std::nullopt;
        std::optional<double> const end = position(js::argument(args, 1), size);
        if (!end)
            return std::nullopt;
        std::string type;
        if (!js::argument(args, 2).is_undefined()) {
            std::optional<std::string> const text = internals_of(interp).to_utf8(args[2]);
            if (!text)
                return std::nullopt;
            type = normalize_type(*text);
        }
        BlobObject const& source = **found;
        auto* piece = interp.heap().allocate<BlobObject>(internals_of(interp).prototype("Blob"));
        if (*end > *start) {
            auto const from = static_cast<std::size_t>(*start);
            auto const to = static_cast<std::size_t>(*end);
            piece->bytes.assign(source.bytes.begin() + static_cast<std::ptrdiff_t>(from), source.bytes.begin() + static_cast<std::ptrdiff_t>(to));
        }
        piece->type = type;
        return js::Value::object(piece);
    });
    js::define_method(interpreter, *blob, "text", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        // text(): the bytes as UTF-8, errors replaced, through a promise.
        std::optional<BlobObject*> const found = this_blob(interp, this_value);
        if (!found)
            return std::nullopt;
        std::string_view const bytes(reinterpret_cast<char const*>((*found)->bytes.data()), (*found)->bytes.size());
        std::u16string text;
        for (char32_t const code_point : html::decode(bytes, html::Encoding::Utf8))
            js::append_code_point(text, code_point);
        return resolved(interp, js::Value::string(interp.string(text)));
    });
    js::define_method(interpreter, *blob, "arrayBuffer", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BlobObject*> const found = this_blob(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>((*found)->bytes.size()), std::nullopt);
        if (!buffer)
            return std::nullopt;
        if (!(*found)->bytes.empty())
            std::memcpy((*buffer)->data(), (*found)->bytes.data(), (*found)->bytes.size());
        return resolved(interp, js::Value::object(*buffer));
    });
    js::define_method(interpreter, *blob, "bytes", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BlobObject*> const found = this_blob(interp, this_value);
        if (!found)
            return std::nullopt;
        Native const array = uint8_array_of(interp, (*found)->bytes);
        if (!array)
            return std::nullopt;
        return resolved(interp, *array);
    });

    // File: a Blob with a name and a modification time.
    js::Object* file = define_interface(in, "File", blob,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.size() < 2)
                return interp.throw_type_error("Failed to construct 'File': 2 arguments required, but only " + std::to_string(args.size()) + " present.");
            js::Interpreter::Roots const roots(interp);
            for (js::Value const& argument_value : args)
                interp.root(argument_value);
            auto* object = interp.heap().allocate<BlobObject>(internals.prototype("File"));
            interp.root(js::Value::object(object));
            object->is_file = true;
            if (!append_parts(interp, *object, args[0]))
                return std::nullopt;
            std::optional<std::string> const name = internals.to_utf8(args[1]);
            if (!name)
                return std::nullopt;
            object->name = *name;
            js::Value const options = js::argument(args, 2);
            std::optional<std::string> const type = type_option(interp, options);
            if (!type)
                return std::nullopt;
            object->type = *type;
            object->last_modified = js::current_time_ms();
            if (options.is_object()) {
                std::optional<js::Value> const modified = interp.get(*options.as_object(), interp.key("lastModified"));
                if (!modified)
                    return std::nullopt;
                if (!modified->is_undefined()) {
                    std::optional<double> const number = interp.to_integer_or_infinity(*modified);
                    if (!number)
                        return std::nullopt;
                    object->last_modified = *number;
                }
            }
            return js::Value::object(object);
        },
        2);
    define_getter(in, *file, "name", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BlobObject*> const object = this_blob(interp, this_value);
        if (!object)
            return std::nullopt;
        return internals_of(interp).string((*object)->name);
    });
    define_getter(in, *file, "lastModified", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BlobObject*> const object = this_blob(interp, this_value);
        if (!object)
            return std::nullopt;
        return js::Value::number((*object)->last_modified);
    });
    define_getter(in, *file, "webkitRelativePath", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).string("");
    });
}

} // namespace

void install_binary(Realm::Internals& in)
{
    install_text_coding(in);
    install_blob(in);
}

}
