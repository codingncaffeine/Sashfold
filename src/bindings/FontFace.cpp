#include "bindings/Internal.h"

#include "core/Ascii.h"
#include "core/Base64.h"
#include "dom/Dom.h"

#include <algorithm>
#include <string>
#include <vector>

// FontFace and the document's FontFaceSet (CSS Font Loading §2, §4): a font
// a script makes — from a URL, or from bytes it has — and adds to
// document.fonts, after which the page's text may use it by its family as
// if a stylesheet had declared it.
//
// Font loaders and icon kits work this way: a FontFace made, load() waited
// for, the face added, the text drawn. A page that finds no FontFace stops
// there with a ReferenceError.
//
// A face added is written into the document as the @font-face rule it
// amounts to (dom::Document::script_font_faces), which every host collects
// with the document's sheets: its bytes come the way a stylesheet's fonts
// come. load() fetches the face's first url() and keeps the bytes in the
// face, so the rule carries them; a face made from bytes is loaded at once.

namespace sashfold::bindings {

namespace {

class FontFaceObject final : public js::Object {
public:
    explicit FontFaceObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::string family;
    std::string source; // a CSS src value, or url() of a data: URL for bytes
    // The descriptors, as given (CSS Font Loading §2.1).
    std::string style = "normal";
    std::string weight = "normal";
    std::string stretch = "normal";
    std::string unicode_range = "U+0-10FFFF";
    std::string variant = "normal";
    std::string feature_settings = "normal";
    std::string variation_settings = "normal";
    std::string display = "auto";
    std::string ascent_override = "normal";
    std::string descent_override = "normal";
    std::string line_gap_override = "normal";
    std::string status = "unloaded";
    js::PromiseCapability loaded {};

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(loaded.promise);
        tracer.visit(loaded.resolve);
        tracer.visit(loaded.reject);
    }
};

FontFaceObject* face_of(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<FontFaceObject*>(value.as_object()) : nullptr;
}

std::optional<FontFaceObject*> this_face(js::Interpreter& interp, js::Value const& this_value)
{
    if (FontFaceObject* const face = face_of(this_value))
        return face;
    return interp.throw_type_error("Illegal invocation");
}

// A descriptor's value as it may stand in a rule: what would end the rule
// is left out (a ';' stays: a data: URL has one).
std::string declaration_safe(std::string_view value)
{
    std::string out;
    for (char const c : value) {
        if (c != '{' && c != '}')
            out += c;
    }
    return out;
}

std::string quoted_family(std::string_view family)
{
    std::string out = "\"";
    for (char const c : family) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n' || c == '\r' || c == '\f')
            continue;
        out += c;
    }
    return out + "\"";
}

// The @font-face rule a face amounts to.
std::string rule_of(FontFaceObject const& face)
{
    std::string rule = "@font-face{font-family:" + quoted_family(face.family) + ";src:" + declaration_safe(face.source);
    auto const descriptor = [&rule](std::string_view name, std::string const& value, std::string_view fallback) {
        if (value != fallback)
            rule += ";" + std::string(name) + ":" + declaration_safe(value);
    };
    descriptor("font-style", face.style, "normal");
    descriptor("font-weight", face.weight, "normal");
    descriptor("font-stretch", face.stretch, "normal");
    descriptor("unicode-range", face.unicode_range, "U+0-10FFFF");
    descriptor("font-display", face.display, "auto");
    return rule + "}\n";
}

// The document's script-made faces written out again, for its hosts to
// collect with its sheets; a layout is due.
void faces_changed(Realm::Internals& in)
{
    std::string text;
    for (js::Object* const object : in.font_faces)
        text += rule_of(*static_cast<FontFaceObject*>(object));
    in.document->script_font_faces = std::move(text);
    ++in.document->script_font_faces_version;
    in.realm.note_mutation();
}

void settle_loaded(Realm::Internals& in, FontFaceObject& face)
{
    face.status = "loaded";
    js::Value const argument[1] = { js::Value::object(&face) };
    static_cast<void>(in.interpreter.call(face.loaded.resolve, js::Value::undefined(), argument));
}

void settle_failed(Realm::Internals& in, FontFaceObject& face, std::string_view name, std::string_view message)
{
    face.status = "error";
    js::Interpreter::Roots const roots(in.interpreter);
    js::Value const argument[1] = { in.interpreter.root(dom_exception_value(in, name, message)) };
    static_cast<void>(in.interpreter.call(face.loaded.reject, js::Value::undefined(), argument));
}

// Whether bytes are a font this engine reads: TrueType, OpenType, a
// collection of either, WOFF or WOFF2, by their first four bytes.
bool looks_like_font(std::span<std::uint8_t const> bytes)
{
    if (bytes.size() < 4)
        return false;
    std::string_view const tag(reinterpret_cast<char const*>(bytes.data()), 4);
    return tag == std::string_view("\0\1\0\0", 4) || tag == "OTTO" || tag == "true" || tag == "typ1" || tag == "ttcf" || tag == "wOFF"
        || tag == "wOF2";
}

std::string as_data_url(std::span<std::uint8_t const> bytes)
{
    return "url(\"data:font/opentype;base64," + base64_encode(bytes) + "\")";
}

// The address of a src value's first url(), unquoted; nothing when it has
// none (local() alone).
std::optional<std::string> first_url(std::string_view source)
{
    for (std::size_t at = 0; at + 4 <= source.size(); ++at) {
        if (!ascii_ci_equals(source.substr(at, 4), "url("))
            continue;
        std::size_t const end = source.find(')', at + 4);
        if (end == std::string_view::npos)
            return std::nullopt;
        std::string_view inner = source.substr(at + 4, end - at - 4);
        while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t'))
            inner.remove_prefix(1);
        while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t'))
            inner.remove_suffix(1);
        if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'') && inner.back() == inner.front())
            inner = inner.substr(1, inner.size() - 2);
        return std::string(inner);
    }
    return std::nullopt;
}

// load()'s fetch (§2.2 "load a font"), in a task: the face's first url()
// fetched as a font is, its bytes checked to be one and kept in the face —
// the rule then carries them, and nothing is fetched twice; a face of
// local() fonts alone is loaded as it is.
void fetch_face(Realm::Internals& in, FontFaceObject& face)
{
    std::optional<std::string> const address = first_url(face.source);
    if (!address) {
        settle_loaded(in, face);
        return;
    }
    std::optional<net::Url> const url = net::parse_url(*address, &in.base_url());
    if (!url || !in.hooks.fetch_resource) {
        settle_failed(in, face, "NetworkError", "A network error occurred.");
        return;
    }
    net::ResourceRequest request;
    request.method = "GET";
    request.headers.push_back({ "Accept", "application/font-woff2;q=1.0,application/font-woff;q=0.9,*/*;q=0.8" });
    request.credentials = url->serialize_origin() == in.origin_url.serialize_origin();
    request.destination = "font";
    net::FetchResult const result = in.hooks.fetch_resource(*url, request, in.request_guard(net::ResourceKind::Font, {}, false));
    if (!result.response || result.response->status < 200 || result.response->status > 299 || !looks_like_font(result.response->body)) {
        settle_failed(in, face, "NetworkError", "A network error occurred.");
        return;
    }
    face.source = as_data_url(result.response->body);
    if (std::find(in.font_faces.begin(), in.font_faces.end(), &face) != in.font_faces.end())
        faces_changed(in);
    settle_loaded(in, face);
}

// The descriptors of a FontFaceDescriptors dictionary, in its members'
// order; nothing with an exception pending.
bool read_descriptors(Realm::Internals& in, FontFaceObject& face, js::Value const& given)
{
    if (given.is_undefined() || given.is_null())
        return true;
    if (!given.is_object()) {
        in.interpreter.throw_type_error("Failed to construct 'FontFace': The provided value is not of type 'FontFaceDescriptors'.");
        return false;
    }
    struct Member {
        std::string_view name;
        std::string FontFaceObject::* field;
    };
    static constexpr Member members[] = { { "ascentOverride", &FontFaceObject::ascent_override },
        { "descentOverride", &FontFaceObject::descent_override }, { "display", &FontFaceObject::display },
        { "featureSettings", &FontFaceObject::feature_settings }, { "lineGapOverride", &FontFaceObject::line_gap_override },
        { "stretch", &FontFaceObject::stretch }, { "style", &FontFaceObject::style }, { "unicodeRange", &FontFaceObject::unicode_range },
        { "variationSettings", &FontFaceObject::variation_settings }, { "weight", &FontFaceObject::weight } };
    for (Member const& member : members) {
        std::optional<js::Value> const value = in.interpreter.get(given, member.name);
        if (!value)
            return false;
        if (value->is_undefined())
            continue;
        std::optional<std::string> text = in.to_utf8(*value);
        if (!text)
            return false;
        face.*member.field = std::move(*text);
    }
    return true;
}

// The faces of the document's set, as an array: what forEach walks and the
// iterators go over (the set's iteration order is insertion order).
js::Value faces_array(Realm::Internals& in, bool pairs)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    js::ArrayObject* const list = interpreter.new_array();
    interpreter.root(js::Value::object(list));
    for (js::Object* const face : in.font_faces) {
        if (pairs) {
            js::Value const pair[2] = { js::Value::object(face), js::Value::object(face) };
            list->push(interpreter.root(js::Value::object(interpreter.new_array(pair))));
        } else {
            list->push(js::Value::object(face));
        }
    }
    return js::Value::object(list);
}

Native array_iterator(js::Interpreter& interp, js::Value const& array, std::string_view method)
{
    std::optional<js::Value> const iterate = interp.get(array, method);
    if (!iterate)
        return std::nullopt;
    return interp.call(*iterate, array, std::span<js::Value const>());
}

} // namespace

void trace_font_faces(Realm::Internals const& in, js::Tracer& tracer)
{
    for (js::Object* const face : in.font_faces)
        tracer.visit(face);
}

void install_font_face_set_members(Realm::Internals& in, js::Object& set)
{
    js::Interpreter& interpreter = in.interpreter;
    define_getter(in, set, "size", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return js::Value::number(static_cast<double>(internals_of(interp).font_faces.size()));
    });
    define_operation(interpreter, set, "add", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        FontFaceObject* const face = face_of(js::argument(args, 0));
        if (face == nullptr)
            return interp.throw_type_error("Failed to execute 'add' on 'FontFaceSet': parameter 1 is not of type 'FontFace'.");
        Realm::Internals& internals = internals_of(interp);
        if (std::find(internals.font_faces.begin(), internals.font_faces.end(), face) == internals.font_faces.end()) {
            internals.font_faces.push_back(face);
            faces_changed(internals);
        }
        return this_value;
    });
    define_operation(interpreter, set, "delete", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        FontFaceObject* const face = face_of(js::argument(args, 0));
        Realm::Internals& internals = internals_of(interp);
        auto const found = std::find(internals.font_faces.begin(), internals.font_faces.end(), face);
        if (face == nullptr || found == internals.font_faces.end())
            return js::Value::boolean(false);
        internals.font_faces.erase(found);
        faces_changed(internals);
        return js::Value::boolean(true);
    });
    define_operation(interpreter, set, "has", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        FontFaceObject* const face = face_of(js::argument(args, 0));
        Realm::Internals& internals = internals_of(interp);
        return js::Value::boolean(face != nullptr && std::find(internals.font_faces.begin(), internals.font_faces.end(), face) != internals.font_faces.end());
    });
    define_operation(interpreter, set, "clear", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        if (!internals.font_faces.empty()) {
            internals.font_faces.clear();
            faces_changed(internals);
        }
        return js::Value::undefined();
    });
    define_operation(interpreter, set, "forEach", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("Failed to execute 'forEach' on 'FontFaceSet': The callback provided as parameter 1 is not a function.");
        js::Interpreter::Roots const roots(interp);
        std::vector<js::Object*> const faces = internals_of(interp).font_faces;
        for (js::Object* const face : faces) {
            interp.root(js::Value::object(face));
            js::Value const arguments[3] = { js::Value::object(face), js::Value::object(face), this_value };
            if (!interp.call(callback, js::argument(args, 1), arguments))
                return std::nullopt;
        }
        return js::Value::undefined();
    });
    define_operation(interpreter, set, "values", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        js::Interpreter::Roots const roots(interp);
        return array_iterator(interp, interp.root(faces_array(internals_of(interp), false)), "values");
    });
    define_operation(interpreter, set, "keys", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        js::Interpreter::Roots const roots(interp);
        return array_iterator(interp, interp.root(faces_array(internals_of(interp), false)), "values");
    });
    define_operation(interpreter, set, "entries", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        js::Interpreter::Roots const roots(interp);
        return array_iterator(interp, interp.root(faces_array(internals_of(interp), true)), "values");
    });
    if (std::optional<js::Value> const values = set.get(interpreter, interpreter.key("values"), js::Value::object(&set)))
        set.put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), *values, js::Writable | js::Configurable);
}

void install_font_face(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* const proto = define_interface(in, "FontFace", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.size() < 2)
                return interp.throw_type_error("Failed to construct 'FontFace': 2 arguments required, but only " + std::to_string(args.size()) + " present.");
            std::optional<std::string> family = internals.to_utf8(args[0]);
            if (!family)
                return std::nullopt;
            js::Interpreter::Roots const roots(interp);
            auto* const face = interp.heap().allocate<FontFaceObject>(internals.prototype("FontFace"));
            interp.root(js::Value::object(face));
            face->family = std::move(*family);
            std::optional<js::PromiseCapability> const loaded
                = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
            if (!loaded)
                return std::nullopt;
            face->loaded = *loaded;
            // The source: bytes the page has (an ArrayBuffer or a view of
            // one), loaded as they are; else a CSS src value.
            bool binary = false;
            bool valid = true;
            if (std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(args[1])) {
                face->source = as_data_url(*bytes);
                binary = true;
                valid = looks_like_font(*bytes);
            } else {
                std::optional<std::string> source = internals.to_utf8(args[1]);
                if (!source)
                    return std::nullopt;
                face->source = std::move(*source);
            }
            if (!read_descriptors(internals, *face, js::argument(args, 2)))
                return std::nullopt;
            // Bytes are loaded at once, or found to be no font (§2.1 step 6).
            if (binary && valid)
                settle_loaded(internals, *face);
            else if (binary)
                settle_failed(internals, *face, "SyntaxError", "The font data is not a font this engine reads.");
            return js::Value::object(face);
        },
        2);
    struct Attribute {
        std::string_view name;
        std::string FontFaceObject::* field;
    };
    static constexpr Attribute attributes[] = { { "family", &FontFaceObject::family }, { "style", &FontFaceObject::style },
        { "weight", &FontFaceObject::weight }, { "stretch", &FontFaceObject::stretch }, { "unicodeRange", &FontFaceObject::unicode_range },
        { "variant", &FontFaceObject::variant }, { "featureSettings", &FontFaceObject::feature_settings },
        { "variationSettings", &FontFaceObject::variation_settings }, { "display", &FontFaceObject::display },
        { "ascentOverride", &FontFaceObject::ascent_override }, { "descentOverride", &FontFaceObject::descent_override },
        { "lineGapOverride", &FontFaceObject::line_gap_override } };
    for (std::size_t i = 0; i < std::size(attributes); ++i) {
        define_getter(
            in, *proto, attributes[i].name,
            js::NativeFunction::Callback([i](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<FontFaceObject*> const face = this_face(interp, this_value);
                if (!face)
                    return std::nullopt;
                return internals_of(interp).string((*face)->*attributes[i].field);
            }),
            js::NativeFunction::Callback([i](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<FontFaceObject*> const face = this_face(interp, this_value);
                if (!face)
                    return std::nullopt;
                Realm::Internals& internals = internals_of(interp);
                std::optional<std::string> text = internals.to_utf8(js::argument(args, 0));
                if (!text)
                    return std::nullopt;
                (*face)->*attributes[i].field = std::move(*text);
                if (std::find(internals.font_faces.begin(), internals.font_faces.end(), *face) != internals.font_faces.end())
                    faces_changed(internals);
                return js::Value::undefined();
            }));
    }
    define_getter(in, *proto, "status", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<FontFaceObject*> const face = this_face(interp, this_value);
        return face ? Native(internals_of(interp).string((*face)->status)) : Native(std::nullopt);
    });
    define_getter(in, *proto, "loaded", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<FontFaceObject*> const face = this_face(interp, this_value);
        return face ? Native((*face)->loaded.promise) : Native(std::nullopt);
    });
    // load() (§2.2): an unloaded face starts loading, and is fetched in a
    // task.
    define_operation(interpreter, *proto, "load", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<FontFaceObject*> const face = this_face(interp, this_value);
        if (!face)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if ((*face)->status == "unloaded") {
            (*face)->status = "loading";
            auto held = std::make_shared<js::Persistent>(interp.heap(), js::Value::object(*face));
            internals.post_task([&internals, held] {
                Realm::Internals::Entry const entry(internals);
                auto* const loading = static_cast<FontFaceObject*>(held->value().as_object());
                if (loading->status == "loading")
                    fetch_face(internals, *loading);
            });
        }
        return (*face)->loaded.promise;
    });
}

}
