#include "bindings/NodeSupport.h"

// The window: the global object's Web API — timers and animation frames,
// location, history, navigator, screen, the storages, matchMedia, URL and
// URLSearchParams, DOMException, the observers a page constructs and the
// small things (atob, alert, performance) scripts reach for on every page.

#include "core/Base64.h"
#include "core/Unicode.h"
#include "css/Stylesheets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

// --- StorageObject ------------------------------------------------------------------

std::string const* StorageObject::find(std::string_view key) const
{
    for (auto const& [name, value] : items) {
        if (name == key)
            return &value;
    }
    return nullptr;
}

void StorageObject::put_item(std::string key, std::string value)
{
    for (auto& [name, existing] : items) {
        if (name == key) {
            existing = std::move(value);
            return;
        }
    }
    items.emplace_back(std::move(key), std::move(value));
}

bool StorageObject::remove_item(std::string_view key)
{
    auto const it = std::find_if(items.begin(), items.end(), [key](auto const& item) { return item.first == key; });
    if (it == items.end())
        return false;
    items.erase(it);
    return true;
}

std::optional<js::PropertyDescriptor> StorageObject::get_own_property(js::PropertyKey const& key) const
{
    if (key.is_string()) {
        std::string const name = key.is_index() ? std::to_string(key.as_index()) : key.as_atom()->to_utf8();
        if (std::string const* value = find(name))
            return js::PropertyDescriptor::data(js::Value::string(heap()->string(*value)), js::default_attributes);
    }
    return Object::get_own_property(key);
}

std::optional<js::Value> StorageObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (key.is_string() && !has_property(key)) {
        std::string const name = key.is_index() ? std::to_string(key.as_index()) : key.as_atom()->to_utf8();
        if (std::string const* value = find(name))
            return js::Value::string(interpreter.string(*value));
    }
    return Object::get(interpreter, key, receiver);
}

std::optional<bool> StorageObject::set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver)
{
    if (!key.is_string() || has_property(key))
        return Object::set(interpreter, key, value, receiver);
    std::optional<js::JsString*> const text = interpreter.to_string(value);
    if (!text)
        return std::nullopt;
    put_item(key.is_index() ? std::to_string(key.as_index()) : key.as_atom()->to_utf8(), (*text)->to_utf8());
    return true;
}

bool StorageObject::delete_property(js::PropertyKey const& key)
{
    if (key.is_string()) {
        std::string const name = key.is_index() ? std::to_string(key.as_index()) : key.as_atom()->to_utf8();
        if (remove_item(name))
            return true;
    }
    return Object::delete_property(key);
}

std::vector<js::PropertyKey> StorageObject::own_keys() const
{
    std::vector<js::PropertyKey> keys;
    for (auto const& [name, value] : items)
        keys.push_back(heap()->key(name));
    for (js::PropertyKey const& key : Object::own_keys())
        keys.push_back(key);
    return keys;
}

namespace {

// --- Timers ------------------------------------------------------------------------------

int schedule_timer(Realm::Internals& in, js::Value const& callback, double delay, std::vector<js::Value> const& arguments,
    double interval, bool animation_frame)
{
    Timer timer;
    timer.id = in.next_timer_id++;
    timer.due = in.now() + std::max(0.0, std::isfinite(delay) ? delay : 0.0);
    timer.sequence = in.next_sequence++;
    timer.interval = interval;
    timer.animation_frame = animation_frame;
    timer.callback = std::make_unique<js::Persistent>(in.interpreter.heap(), callback);
    for (js::Value const& argument : arguments)
        timer.arguments.push_back(std::make_unique<js::Persistent>(in.interpreter.heap(), argument));
    int const id = timer.id;
    in.timers.push_back(std::move(timer));
    return id;
}

Native set_timer(js::Interpreter& interpreter, Args args, bool repeat)
{
    Realm::Internals& in = internals_of(interpreter);
    js::Value handler = js::argument(args, 0);
    if (!js::Interpreter::is_callable(handler)) {
        // A string of source, or anything else made one (§8.6 step 8).
        std::optional<js::JsString*> const text = interpreter.to_string(handler);
        if (!text)
            return std::nullopt;
        handler = js::Value::string(*text);
    }
    double delay = 0;
    if (args.size() > 1) {
        std::optional<double> const number = interpreter.to_number(args[1]);
        if (!number)
            return std::nullopt;
        delay = *number;
    }
    if (!std::isfinite(delay) || delay < 0)
        delay = 0;
    std::vector<js::Value> arguments;
    for (std::size_t i = 2; i < args.size(); ++i)
        arguments.push_back(args[i]);
    return js::Value::number(schedule_timer(in, handler, delay, arguments, repeat ? std::max(delay, 0.0) : -1, false));
}

Native clear_timer(js::Interpreter& interpreter, Args args)
{
    Realm::Internals& in = internals_of(interpreter);
    js::Value const id_value = js::argument(args, 0);
    if (id_value.is_nullish())
        return js::Value::undefined();
    std::optional<double> const id = interpreter.to_number(id_value);
    if (!id)
        return std::nullopt;
    auto const it = std::find_if(in.timers.begin(), in.timers.end(), [&](Timer const& timer) { return static_cast<double>(timer.id) == *id; });
    if (it != in.timers.end())
        in.timers.erase(it);
    return js::Value::undefined();
}

// --- Location ----------------------------------------------------------------------------

void navigate_to(Realm::Internals& in, std::string const& target)
{
    std::optional<net::Url> const url = net::parse_url(target, &in.url);
    if (!url) {
        in.console("error", "location: '" + target + "' is not a URL");
        return;
    }
    if (in.hooks.navigate)
        in.hooks.navigate(*url);
    else
        in.url = *url;
}

// Sets one part of the URL and navigates to the result.
Native set_url_part(js::Interpreter& interpreter, std::string_view part, js::Value const& value)
{
    Realm::Internals& in = internals_of(interpreter);
    std::optional<std::string> const text = in.to_utf8(value);
    if (!text)
        return std::nullopt;
    net::Url url = in.url;
    if (part == "href") {
        navigate_to(in, *text);
        return js::Value::undefined();
    }
    if (part == "hash") {
        std::string fragment = *text;
        if (fragment.starts_with('#'))
            fragment.erase(0, 1);
        url.fragment = fragment;
    } else if (part == "search") {
        std::string query = *text;
        if (query.starts_with('?'))
            query.erase(0, 1);
        url.query = query.empty() ? std::nullopt : std::optional<std::string>(query);
    } else if (part == "pathname") {
        std::optional<net::Url> const parsed = net::parse_url(text->starts_with('/') ? *text : "/" + *text, &in.url);
        if (!parsed)
            return js::Value::undefined();
        url.path = parsed->path;
        url.has_opaque_path = parsed->has_opaque_path;
    } else if (part == "protocol" || part == "host" || part == "hostname" || part == "port") {
        std::string serialized = url.serialize();
        std::string rebuilt;
        if (part == "protocol") {
            std::string scheme = *text;
            if (scheme.ends_with(':'))
                scheme.pop_back();
            rebuilt = scheme + serialized.substr(url.scheme.size());
        } else {
            std::string const authority = url.host_with_port();
            std::size_t const at = serialized.find(authority);
            if (at == std::string::npos)
                return js::Value::undefined();
            std::string replacement = *text;
            if (part == "hostname")
                replacement = *text + (url.port ? ":" + url.port_string() : "");
            else if (part == "port")
                replacement = url.serialize_host() + (text->empty() ? "" : ":" + *text);
            rebuilt = serialized.substr(0, at) + replacement + serialized.substr(at + authority.size());
        }
        std::optional<net::Url> const parsed = net::parse_url(rebuilt);
        if (!parsed)
            return js::Value::undefined();
        url = *parsed;
    }
    if (in.hooks.navigate)
        in.hooks.navigate(url);
    else
        in.url = url;
    return js::Value::undefined();
}

std::string url_part(net::Url const& url, std::string_view part)
{
    if (part == "href")
        return url.serialize();
    if (part == "protocol")
        return url.protocol();
    if (part == "host")
        return url.host_with_port();
    if (part == "hostname")
        return url.serialize_host();
    if (part == "port")
        return url.port_string();
    if (part == "pathname")
        return url.serialize_path();
    if (part == "search")
        return url.search();
    if (part == "hash")
        return url.hash();
    if (part == "origin")
        return url.serialize_origin();
    if (part == "username")
        return url.username;
    if (part == "password")
        return url.password;
    return "";
}

// --- URLSearchParams -----------------------------------------------------------------------

std::optional<SearchParamsObject*> this_params(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* params = dynamic_cast<SearchParamsObject*>(this_value.as_object()))
            return params;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

std::optional<UrlObject*> this_url(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* url = dynamic_cast<UrlObject*>(this_value.as_object()))
            return url;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

std::string percent_decode(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const c = text[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < text.size() + 0 && i + 2 <= text.size() - 1 + 0) {
            auto const hex = [](char h) -> int {
                if (h >= '0' && h <= '9')
                    return h - '0';
                if (h >= 'a' && h <= 'f')
                    return h - 'a' + 10;
                if (h >= 'A' && h <= 'F')
                    return h - 'A' + 10;
                return -1;
            };
            int const high = hex(text[i + 1]);
            int const low = hex(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>(high * 16 + low);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

std::string form_urlencode(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        unsigned char const byte = static_cast<unsigned char>(c);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '*' || byte == '-'
            || byte == '.' || byte == '_') {
            out += c;
        } else if (byte == ' ') {
            out += '+';
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof buffer, "%%%02X", byte);
            out += buffer;
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view query)
{
    std::vector<std::pair<std::string, std::string>> pairs;
    if (query.starts_with('?'))
        query.remove_prefix(1);
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t end = query.find('&', start);
        if (end == std::string_view::npos)
            end = query.size();
        std::string_view const piece = query.substr(start, end - start);
        if (!piece.empty()) {
            std::size_t const equals = piece.find('=');
            std::string const name = percent_decode(equals == std::string_view::npos ? piece : piece.substr(0, equals));
            std::string const value = percent_decode(equals == std::string_view::npos ? "" : piece.substr(equals + 1));
            pairs.emplace_back(name, value);
        }
        if (end == query.size())
            break;
        start = end + 1;
    }
    return pairs;
}

std::string serialize_query(std::vector<std::pair<std::string, std::string>> const& pairs)
{
    std::string out;
    for (auto const& [name, value] : pairs) {
        if (!out.empty())
            out += '&';
        out += form_urlencode(name) + "=" + form_urlencode(value);
    }
    return out;
}

// Writes a params list back to the URL it belongs to (§6.2 "update").
void update_owner(SearchParamsObject& params)
{
    if (!params.owner)
        return;
    std::string const query = serialize_query(params.pairs);
    params.owner->url.query = query.empty() ? std::nullopt : std::optional<std::string>(query);
}

// --- Media queries -------------------------------------------------------------------------

css::MediaContext media_context(Realm::Internals& in)
{
    return css::MediaContext { in.hooks.viewport_width, in.hooks.viewport_height };
}

// --- Observers ---------------------------------------------------------------------------------

// An observer object: the callback it was made with, delivered once per
// observed target on the next turn with a record saying the target is
// wholly in view (IntersectionObserver) or has its size (ResizeObserver).
// MutationObserver keeps its callback and never fires: records are not
// written yet.
class ObserverObject final : public EventTargetObject {
public:
    ObserverObject(js::Object* prototype, js::Value the_callback)
        : EventTargetObject(prototype)
        , callback(the_callback)
    {
    }
    js::Value callback;
    std::vector<js::Value> targets; // wrappers of the observed nodes
    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(callback);
        for (js::Value const& target : targets)
            tracer.visit(target);
    }
};

std::optional<ObserverObject*> this_observer(js::Interpreter& interpreter, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* observer = dynamic_cast<ObserverObject*>(this_value.as_object()))
            return observer;
    }
    return interpreter.throw_type_error("Illegal invocation");
}

// Builds the entries for the observer's pending targets and calls back.
Native deliver_observations(js::Interpreter& interpreter, js::Value const&, Args args)
{
    Realm::Internals& in = internals_of(interpreter);
    js::Value const observer_value = js::argument(args, 0);
    bool const intersection = js::Interpreter::to_boolean(js::argument(args, 1));
    if (!observer_value.is_object())
        return js::Value::undefined();
    auto* observer = dynamic_cast<ObserverObject*>(observer_value.as_object());
    if (!observer || observer->targets.empty())
        return js::Value::undefined();
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(observer_value);
    js::ArrayObject* entries = interpreter.new_array();
    interpreter.root(js::Value::object(entries));
    std::vector<js::Value> const targets = std::move(observer->targets);
    observer->targets.clear();
    for (js::Value const& target : targets) {
        js::Object* entry = interpreter.new_object();
        entries->push(js::Value::object(entry));
        entry->put(interpreter.key("target"), target);
        entry->put(interpreter.key("time"), js::Value::number(in.now() - in.time_origin));
        dom::Node* node = in.realm.node_of(target);
        std::optional<LayoutBox> box;
        if (node && node->is_element())
            box = client_box(in, static_cast<dom::Element&>(*node));
        js::Value const rect = box ? make_rect(in, *box) : make_rect(in, 0, 0, 0, 0);
        if (intersection) {
            entry->put(interpreter.key("isIntersecting"), js::Value::boolean(true));
            entry->put(interpreter.key("intersectionRatio"), js::Value::number(1));
            entry->put(interpreter.key("boundingClientRect"), rect);
            entry->put(interpreter.key("intersectionRect"), rect);
            entry->put(interpreter.key("rootBounds"), make_rect(in, 0, 0, static_cast<double>(in.hooks.viewport_width), static_cast<double>(in.hooks.viewport_height)));
            entry->put(interpreter.key("isVisible"), js::Value::boolean(true));
        } else {
            entry->put(interpreter.key("contentRect"), rect);
            js::Object* size = interpreter.new_object();
            size->put(interpreter.key("inlineSize"), js::Value::number(static_cast<double>(box ? box->width : 0)));
            size->put(interpreter.key("blockSize"), js::Value::number(static_cast<double>(box ? box->height : 0)));
            js::Value const sizes[1] = { js::Value::object(size) };
            js::ArrayObject* size_list = interpreter.new_array(sizes);
            entry->put(interpreter.key("contentBoxSize"), js::Value::object(size_list));
            entry->put(interpreter.key("borderBoxSize"), js::Value::object(size_list));
            entry->put(interpreter.key("devicePixelContentBoxSize"), js::Value::object(size_list));
        }
    }
    js::Value const callback_arguments[2] = { js::Value::object(entries), observer_value };
    in.call_reporting(observer->callback, observer_value, callback_arguments, intersection ? "IntersectionObserver callback" : "ResizeObserver callback");
    return js::Value::undefined();
}

void install_observer(Realm::Internals& in, std::string_view name, bool intersection, bool fires)
{
    js::Interpreter& interpreter = in.interpreter;
    std::string const interface(name);
    js::Object* proto = define_interface(in, name, nullptr,
        [interface](js::Interpreter& interp, Args args, js::Object*) -> Native {
            js::Value const callback = js::argument(args, 0);
            if (!js::Interpreter::is_callable(callback))
                return interp.throw_type_error("Failed to construct '" + interface + "': parameter 1 is not of type 'Function'.");
            Realm::Internals& internals = internals_of(interp);
            return js::Value::object(interp.heap().allocate<ObserverObject>(internals.prototype(interface), callback));
        },
        1);
    js::define_method(interpreter, *proto, "observe", 1, [intersection, fires](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<ObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Value const target = js::argument(args, 0);
        if (!internals.realm.node_of(target))
            return interp.throw_type_error("parameter 1 is not of type 'Element'.");
        if (!fires)
            return js::Value::undefined();
        bool const pending = !(*observer)->targets.empty();
        (*observer)->targets.push_back(target);
        if (!pending) {
            js::Interpreter::Roots const roots(interp);
            js::Value const deliver = interp.root(js::Value::object(interp.new_native("deliver", 2, deliver_observations)));
            std::vector<js::Value> const arguments = { this_value, js::Value::boolean(intersection) };
            schedule_timer(internals, deliver, 0, arguments, -1, false);
        }
        return js::Value::undefined();
    });
    js::define_method(interpreter, *proto, "unobserve", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<ObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        js::Value const target = js::argument(args, 0);
        auto& targets = (*observer)->targets;
        targets.erase(std::remove(targets.begin(), targets.end(), target), targets.end());
        return js::Value::undefined();
    });
    js::define_method(interpreter, *proto, "disconnect", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        (*observer)->targets.clear();
        return js::Value::undefined();
    });
    js::define_method(interpreter, *proto, "takeRecords", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return js::Value::object(interp.new_array());
    });
    if (intersection) {
        define_getter(in, *proto, "root", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::null(); });
        define_getter(in, *proto, "rootMargin", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return internals_of(interp).string("0px 0px 0px 0px"); });
        define_getter(in, *proto, "thresholds", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
            js::Value const zero[1] = { js::Value::number(0) };
            return js::Value::object(interp.new_array(zero));
        });
    }
}

// A plain object with these string properties.
js::Object* object_with(Realm::Internals& in, std::vector<std::pair<std::string_view, js::Value>> const& properties)
{
    js::Heap::NoCollect const guard(in.interpreter.heap());
    js::Object* object = in.interpreter.new_object();
    for (auto const& [name, value] : properties)
        object->put(in.interpreter.key(name), value);
    return object;
}

std::string platform_name()
{
#if defined(_WIN32)
    return "Win32";
#elif defined(__APPLE__)
    return "MacIntel";
#else
    return "Linux x86_64";
#endif
}

} // namespace

// --- install_window ---------------------------------------------------------------------------------

void install_window(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* global = interpreter.global();
    in.prototypes["Window"] = global;

    // The window is its own frame tree.
    for (std::string_view const name : { "window", "self", "frames", "top", "parent" })
        define_getter(in, *global, name, [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::object(interp.global()); });
    define_getter(in, *global, "document", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(internals.wrap(internals.document));
    });
    global->put(interpreter.key("name"), in.string(""), js::default_attributes);
    global->put(interpreter.key("status"), in.string(""), js::default_attributes);
    global->put(interpreter.key("closed"), js::Value::boolean(false), js::builtin_attributes);
    global->put(interpreter.key("length"), js::Value::number(0), js::builtin_attributes);
    global->put(interpreter.key("opener"), js::Value::null(), js::default_attributes);
    global->put(interpreter.key("frameElement"), js::Value::null(), js::builtin_attributes);
    define_getter(in, *global, "origin", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).string(internals_of(interp).url.serialize_origin());
    });
    define_getter(in, *global, "isSecureContext", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        net::Url const& url = internals_of(interp).url;
        return js::Value::boolean(url.scheme == "https" || url.scheme == "file" || url.scheme == "about" || url.host == "localhost");
    });
    define_getter(in, *global, "innerWidth", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::number(static_cast<double>(internals_of(interp).hooks.viewport_width)); });
    define_getter(in, *global, "innerHeight", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::number(static_cast<double>(internals_of(interp).hooks.viewport_height)); });
    define_getter(in, *global, "outerWidth", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::number(static_cast<double>(internals_of(interp).hooks.viewport_width)); });
    define_getter(in, *global, "outerHeight", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::number(static_cast<double>(internals_of(interp).hooks.viewport_height + 80)); });
    define_getter(in, *global, "devicePixelRatio", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(1); });
    for (std::string_view const name : { "screenX", "screenY", "screenLeft", "screenTop" })
        define_getter(in, *global, name, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(0); });
    for (std::string_view const name : { "scrollX", "pageXOffset" }) {
        define_getter(in, *global, name, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            return js::Value::number(internals.hooks.scroll_position ? internals.hooks.scroll_position().first : 0);
        });
    }
    for (std::string_view const name : { "scrollY", "pageYOffset" }) {
        define_getter(in, *global, name, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            return js::Value::number(internals.hooks.scroll_position ? internals.hooks.scroll_position().second : 0);
        });
    }
    define_getter(in, *global, "event", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        js::Value const current = internals_of(interp).current_event;
        return current.is_object() ? current : js::Value::undefined();
    });
    define_getter(in, *global, "visualViewport", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::pair<int, int> const scroll = internals.hooks.scroll_position ? internals.hooks.scroll_position() : std::pair<int, int> { 0, 0 };
        return js::Value::object(object_with(internals, { { "width", js::Value::number(static_cast<double>(internals.hooks.viewport_width)) },
            { "height", js::Value::number(static_cast<double>(internals.hooks.viewport_height)) }, { "scale", js::Value::number(1) },
            { "offsetLeft", js::Value::number(0) }, { "offsetTop", js::Value::number(0) }, { "pageLeft", js::Value::number(scroll.first) },
            { "pageTop", js::Value::number(scroll.second) } }));
    });

    // Timers.
    js::define_method(interpreter, *global, "setTimeout", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return set_timer(interp, args, false); });
    js::define_method(interpreter, *global, "setInterval", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return set_timer(interp, args, true); });
    js::define_method(interpreter, *global, "clearTimeout", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return clear_timer(interp, args); });
    js::define_method(interpreter, *global, "clearInterval", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return clear_timer(interp, args); });
    js::define_method(interpreter, *global, "requestAnimationFrame", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("Failed to execute 'requestAnimationFrame' on 'Window': parameter 1 is not of type 'Function'.");
        return js::Value::number(schedule_timer(internals_of(interp), callback, 16, {}, -1, true));
    });
    js::define_method(interpreter, *global, "cancelAnimationFrame", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return clear_timer(interp, args); });
    js::define_method(interpreter, *global, "requestIdleCallback", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("Failed to execute 'requestIdleCallback' on 'Window': parameter 1 is not of type 'Function'.");
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::Object* deadline = object_with(internals, { { "didTimeout", js::Value::boolean(false) } });
        interp.root(js::Value::object(deadline));
        js::define_method(interp, *deadline, "timeRemaining", 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(50); });
        std::vector<js::Value> const arguments = { js::Value::object(deadline) };
        return js::Value::number(schedule_timer(internals, callback, 1, arguments, -1, false));
    });
    js::define_method(interpreter, *global, "cancelIdleCallback", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return clear_timer(interp, args); });
    js::define_method(interpreter, *global, "queueMicrotask", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("Failed to execute 'queueMicrotask' on 'Window': parameter 1 is not of type 'Function'.");
        Realm::Internals& internals = internals_of(interp);
        internals.microtasks.push_back(std::make_unique<js::Persistent>(interp.heap(), callback));
        return js::Value::undefined();
    });

    // Dialogs and the rest of the window's methods.
    js::define_method(interpreter, *global, "alert", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const message = args.empty() ? std::optional<std::string>("") : internals.to_utf8(args[0]);
        if (!message)
            return std::nullopt;
        internals.console("info", "alert: " + *message);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *global, "confirm", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const message = args.empty() ? std::optional<std::string>("") : internals.to_utf8(args[0]);
        if (!message)
            return std::nullopt;
        internals.console("info", "confirm: " + *message + " (answered no)");
        return js::Value::boolean(false);
    });
    js::define_method(interpreter, *global, "prompt", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const message = args.empty() ? std::optional<std::string>("") : internals.to_utf8(args[0]);
        if (!message)
            return std::nullopt;
        internals.console("info", "prompt: " + *message + " (cancelled)");
        return js::Value::null();
    });
    for (std::string_view const name : { "print", "close", "stop", "focus", "blur", "postMessage", "captureEvents", "releaseEvents", "moveTo",
             "moveBy", "resizeTo", "resizeBy" })
        js::define_method(interpreter, *global, name, 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    js::define_method(interpreter, *global, "open", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const target = args.empty() ? std::optional<std::string>("") : internals.to_utf8(args[0]);
        if (!target)
            return std::nullopt;
        internals.console("info", "window.open blocked: " + *target);
        return js::Value::null();
    });
    js::define_method(interpreter, *global, "reportError", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        internals_of(interp).report_uncaught(js::argument(args, 0), "reportError");
        return js::Value::undefined();
    });
    auto const scroll = [](bool relative) {
        return [relative](js::Interpreter& interp, js::Value const&, Args args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            double x = 0;
            double y = 0;
            js::Value const first = js::argument(args, 0);
            if (first.is_object()) {
                for (auto const& [name, out] : { std::pair { "left", &x }, std::pair { "top", &y } }) {
                    std::optional<js::Value> const value = interp.get(first, name);
                    if (!value)
                        return std::nullopt;
                    if (!value->is_undefined()) {
                        std::optional<double> const number = interp.to_number(*value);
                        if (!number)
                            return std::nullopt;
                        *out = *number;
                    }
                }
            } else {
                std::optional<double> const nx = interp.to_number(first);
                std::optional<double> const ny = interp.to_number(js::argument(args, 1));
                if (!nx || !ny)
                    return std::nullopt;
                x = std::isnan(*nx) ? 0 : *nx;
                y = std::isnan(*ny) ? 0 : *ny;
            }
            if (relative && internals.hooks.scroll_position) {
                std::pair<int, int> const current = internals.hooks.scroll_position();
                x += current.first;
                y += current.second;
            }
            if (internals.hooks.scroll_to)
                internals.hooks.scroll_to(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
            return js::Value::undefined();
        };
    };
    js::define_method(interpreter, *global, "scrollTo", 2, scroll(false));
    js::define_method(interpreter, *global, "scroll", 2, scroll(false));
    js::define_method(interpreter, *global, "scrollBy", 2, scroll(true));
    js::define_method(interpreter, *global, "atob", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> text = internals.to_utf8(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        std::string stripped;
        for (char const c : *text) {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r')
                stripped += c;
        }
        if (stripped.size() % 4 == 0 && stripped.ends_with("=="))
            stripped.resize(stripped.size() - 2);
        else if (stripped.size() % 4 == 0 && stripped.ends_with('='))
            stripped.pop_back();
        std::optional<std::vector<std::uint8_t>> const bytes = base64_decode(stripped);
        if (!bytes)
            return internals.throw_dom_exception("InvalidCharacterError", "The string to be decoded is not correctly encoded.");
        // Each byte is one code unit (a Latin-1 string).
        std::u16string units;
        for (std::uint8_t const byte : *bytes)
            units += static_cast<char16_t>(byte);
        return js::Value::string(interp.string(std::u16string_view(units)));
    });
    js::define_method(interpreter, *global, "btoa", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<js::JsString*> const text = interp.to_string(js::argument(args, 0));
        if (!text)
            return std::nullopt;
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::u16string_view const units = (*text)->view();
        std::vector<std::uint8_t> bytes;
        for (char16_t const unit : units) {
            if (unit > 0xFF)
                return internals.throw_dom_exception("InvalidCharacterError", "The string to be encoded contains characters outside of the Latin1 range.");
            bytes.push_back(static_cast<std::uint8_t>(unit));
        }
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16;
            if (i + 1 < bytes.size())
                chunk |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
            if (i + 2 < bytes.size())
                chunk |= bytes[i + 2];
            out += alphabet[(chunk >> 18) & 63];
            out += alphabet[(chunk >> 12) & 63];
            out += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
        }
        return internals.string(out);
    });
    js::define_method(interpreter, *global, "structuredClone", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        // A JSON round trip: what the algorithm does for the values pages
        // clone in practice, minus Dates and Maps.
        js::Value const json = js::Value::object(interp.intrinsics().json);
        std::optional<js::Value> const stringify = interp.get(json, "stringify");
        std::optional<js::Value> const parse = interp.get(json, "parse");
        if (!stringify || !parse)
            return std::nullopt;
        js::Value const to_text[1] = { js::argument(args, 0) };
        std::optional<js::Value> const text = interp.call(*stringify, json, to_text);
        if (!text)
            return std::nullopt;
        if (text->is_undefined())
            return js::Value::undefined();
        js::Interpreter::Roots const roots(interp);
        interp.root(*text);
        js::Value const from_text[1] = { *text };
        return interp.call(*parse, json, from_text);
    });
    js::define_method(interpreter, *global, "matchMedia", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const query = internals.to_utf8(js::argument(args, 0));
        if (!query)
            return std::nullopt;
        js::Heap::NoCollect const no_collect(interp.heap());
        auto* list = interp.heap().allocate<EventTargetObject>(internals.prototype("MediaQueryList"));
        list->put(interp.key("media"), internals.string(*query), js::Enumerable);
        list->put(interp.key("matches"), js::Value::boolean(css::media_query_matches(*query, media_context(internals))), js::Enumerable);
        list->put(interp.key("onchange"), js::Value::null(), js::default_attributes);
        return js::Value::object(list);
    });
    js::Object* media_query_list = define_interface(in, "MediaQueryList", in.prototype("EventTarget"));
    for (std::string_view const name : { "addListener", "removeListener" })
        js::define_method(interpreter, *media_query_list, name, 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    js::define_method(interpreter, *global, "getSelection", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Heap::NoCollect const no_collect(interp.heap());
        js::Object* selection = object_with(internals, { { "anchorNode", js::Value::null() }, { "anchorOffset", js::Value::number(0) },
            { "focusNode", js::Value::null() }, { "focusOffset", js::Value::number(0) }, { "isCollapsed", js::Value::boolean(true) },
            { "rangeCount", js::Value::number(0) }, { "type", internals.string("None") } });
        js::define_method(interp, *selection, "toString", 0, [](js::Interpreter& i, js::Value const&, Args) -> Native { return internals_of(i).string(""); });
        for (std::string_view const name : { "removeAllRanges", "empty", "addRange", "collapse", "collapseToStart", "collapseToEnd", "selectAllChildren",
                 "deleteFromDocument", "extend", "setBaseAndExtent", "modify" })
            js::define_method(interp, *selection, name, 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
        js::define_method(interp, *selection, "containsNode", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(false); });
        js::define_method(interp, *selection, "getRangeAt", 1, [](js::Interpreter& i, js::Value const&, Args) -> Native {
            return internals_of(i).throw_dom_exception("IndexSizeError", "0 is not a valid index.");
        });
        return js::Value::object(selection);
    });

    // Location.
    js::Object* location_proto = define_interface(in, "Location", nullptr);
    for (std::string_view const part : { "href", "protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin", "username", "password" }) {
        std::string const part_name(part);
        bool const settable = part != "origin";
        define_getter(
            in, *location_proto, part,
            [part_name](js::Interpreter& interp, js::Value const&, Args) -> Native {
                return internals_of(interp).string(url_part(internals_of(interp).url, part_name));
            },
            settable ? js::NativeFunction::Callback([part_name](js::Interpreter& interp, js::Value const&, Args args) -> Native {
                return set_url_part(interp, part_name, js::argument(args, 0));
            })
                     : js::NativeFunction::Callback());
    }
    js::define_method(interpreter, *location_proto, "toString", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).string(internals_of(interp).url.serialize());
    });
    js::define_method(interpreter, *location_proto, "assign", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return set_url_part(interp, "href", js::argument(args, 0));
    });
    js::define_method(interpreter, *location_proto, "replace", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return set_url_part(interp, "href", js::argument(args, 0));
    });
    js::define_method(interpreter, *location_proto, "reload", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        if (internals.hooks.navigate)
            internals.hooks.navigate(internals.url);
        return js::Value::undefined();
    });
    define_getter(in, *location_proto, "ancestorOrigins", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return js::Value::object(interp.new_array());
    });
    in.location = interpreter.new_object(location_proto);
    define_getter(
        in, *global, "location", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::object(internals_of(interp).location); },
        [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return set_url_part(interp, "href", js::argument(args, 0)); });

    // History.
    js::Object* history_proto = define_interface(in, "History", nullptr);
    define_getter(in, *history_proto, "length", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::number(internals_of(interp).history_length); });
    define_getter(in, *history_proto, "state", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        js::Value const state = internals_of(interp).history_state;
        return state.is_undefined() ? js::Value::null() : state;
    });
    define_getter(
        in, *history_proto, "scrollRestoration", [](js::Interpreter& interp, js::Value const&, Args) -> Native { return internals_of(interp).string("auto"); },
        [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    for (std::string_view const name : { "pushState", "replaceState" }) {
        bool const push = name == "pushState";
        js::define_method(interpreter, *history_proto, name, 2, [push](js::Interpreter& interp, js::Value const&, Args args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            internals.history_state = js::argument(args, 0);
            js::Value const url_value = js::argument(args, 2);
            if (!url_value.is_nullish()) {
                std::optional<std::string> const text = internals.to_utf8(url_value);
                if (!text)
                    return std::nullopt;
                std::optional<net::Url> const url = net::parse_url(*text, &internals.url);
                if (!url || url->serialize_origin() != internals.url.serialize_origin())
                    return internals.throw_dom_exception("SecurityError", "A history state object with URL '" + *text + "' cannot be created in a document with origin '" + internals.url.serialize_origin() + "'.");
                internals.url = *url;
            }
            if (push)
                ++internals.history_length;
            return js::Value::undefined();
        });
    }
    for (std::string_view const name : { "back", "forward", "go" })
        js::define_method(interpreter, *history_proto, name, 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    global->put(interpreter.key("history"), js::Value::object(interpreter.new_object(history_proto)), js::builtin_attributes);

    // Navigator.
    js::Object* navigator_proto = define_interface(in, "Navigator", nullptr);
    std::string const user_agent = in.hooks.user_agent.empty() ? std::string("Mozilla/5.0 Sashfold/" SASHFOLD_VERSION) : in.hooks.user_agent;
    auto const constant_string = [&](js::Object& target, std::string_view name, std::string value) {
        define_getter(in, target, name, [value](js::Interpreter& interp, js::Value const&, Args) -> Native { return internals_of(interp).string(value); });
    };
    constant_string(*navigator_proto, "userAgent", user_agent);
    constant_string(*navigator_proto, "appVersion", user_agent.starts_with("Mozilla/") ? user_agent.substr(8) : user_agent);
    constant_string(*navigator_proto, "appName", "Netscape");
    constant_string(*navigator_proto, "appCodeName", "Mozilla");
    constant_string(*navigator_proto, "product", "Gecko");
    constant_string(*navigator_proto, "productSub", "20030107");
    constant_string(*navigator_proto, "vendor", "");
    constant_string(*navigator_proto, "vendorSub", "");
    constant_string(*navigator_proto, "platform", platform_name());
    constant_string(*navigator_proto, "language", "en-US");
    constant_string(*navigator_proto, "oscpu", platform_name());
    define_getter(in, *navigator_proto, "languages", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::ArrayObject* languages = interp.new_array();
        interp.root(js::Value::object(languages));
        languages->push(internals.string("en-US"));
        languages->push(internals.string("en"));
        return js::Value::object(languages);
    });
    define_getter(in, *navigator_proto, "onLine", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(true); });
    define_getter(in, *navigator_proto, "cookieEnabled", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(true); });
    define_getter(in, *navigator_proto, "webdriver", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(false); });
    define_getter(in, *navigator_proto, "pdfViewerEnabled", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(false); });
    define_getter(in, *navigator_proto, "doNotTrack", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::null(); });
    define_getter(in, *navigator_proto, "hardwareConcurrency", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(4); });
    define_getter(in, *navigator_proto, "maxTouchPoints", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(0); });
    define_getter(in, *navigator_proto, "deviceMemory", [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(8); });
    for (std::string_view const name : { "plugins", "mimeTypes" }) {
        define_getter(in, *navigator_proto, name, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
            js::ArrayObject* empty = interp.new_array();
            js::define_method(interp, *empty, "refresh", 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
            return js::Value::object(empty);
        });
    }
    js::define_method(interpreter, *navigator_proto, "javaEnabled", 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(false); });
    js::define_method(interpreter, *navigator_proto, "sendBeacon", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(false); });
    js::define_method(interpreter, *navigator_proto, "vibrate", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(false); });
    js::define_method(interpreter, *navigator_proto, "registerProtocolHandler", 2, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    global->put(interpreter.key("navigator"), js::Value::object(interpreter.new_object(navigator_proto)), js::builtin_attributes);
    global->put(interpreter.key("clientInformation"), *global->get(interpreter, interpreter.key("navigator"), js::Value::object(global)), js::builtin_attributes);

    // Screen.
    js::Object* screen_proto = define_interface(in, "Screen", nullptr);
    for (std::string_view const name : { "width", "availWidth" })
        define_getter(in, *screen_proto, name, [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::number(static_cast<double>(internals_of(interp).hooks.viewport_width)); });
    for (std::string_view const name : { "height", "availHeight" })
        define_getter(in, *screen_proto, name, [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::number(static_cast<double>(internals_of(interp).hooks.viewport_height)); });
    for (std::string_view const name : { "colorDepth", "pixelDepth" })
        define_getter(in, *screen_proto, name, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(24); });
    for (std::string_view const name : { "availLeft", "availTop" })
        define_getter(in, *screen_proto, name, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::number(0); });
    define_getter(in, *screen_proto, "orientation", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        bool const landscape = internals.hooks.viewport_width >= internals.hooks.viewport_height;
        return js::Value::object(object_with(internals, { { "type", internals.string(landscape ? "landscape-primary" : "portrait-primary") }, { "angle", js::Value::number(0) } }));
    });
    global->put(interpreter.key("screen"), js::Value::object(interpreter.new_object(screen_proto)), js::builtin_attributes);

    // Storage.
    js::Object* storage_proto = define_interface(in, "Storage", nullptr);
    auto const this_storage = [](js::Interpreter& interp, js::Value const& this_value) -> std::optional<StorageObject*> {
        if (this_value.is_object()) {
            if (auto* storage = dynamic_cast<StorageObject*>(this_value.as_object()))
                return storage;
        }
        return interp.throw_type_error("Illegal invocation");
    };
    define_getter(in, *storage_proto, "length", [this_storage](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<StorageObject*> const storage = this_storage(interp, this_value);
        if (!storage)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*storage)->items.size()));
    });
    js::define_method(interpreter, *storage_proto, "key", 1, [this_storage](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StorageObject*> const storage = this_storage(interp, this_value);
        if (!storage)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        if (*index < 0 || *index >= static_cast<double>((*storage)->items.size()))
            return js::Value::null();
        return internals_of(interp).string((*storage)->items[static_cast<std::size_t>(*index)].first);
    });
    js::define_method(interpreter, *storage_proto, "getItem", 1, [this_storage](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StorageObject*> const storage = this_storage(interp, this_value);
        if (!storage)
            return std::nullopt;
        std::optional<std::string> const key = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!key)
            return std::nullopt;
        std::string const* value = (*storage)->find(*key);
        return value ? internals_of(interp).string(*value) : js::Value::null();
    });
    js::define_method(interpreter, *storage_proto, "setItem", 2, [this_storage](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StorageObject*> const storage = this_storage(interp, this_value);
        if (!storage)
            return std::nullopt;
        std::optional<std::string> key = internals_of(interp).to_utf8(js::argument(args, 0));
        std::optional<std::string> value = internals_of(interp).to_utf8(js::argument(args, 1));
        if (!key || !value)
            return std::nullopt;
        (*storage)->put_item(std::move(*key), std::move(*value));
        return js::Value::undefined();
    });
    js::define_method(interpreter, *storage_proto, "removeItem", 1, [this_storage](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<StorageObject*> const storage = this_storage(interp, this_value);
        if (!storage)
            return std::nullopt;
        std::optional<std::string> const key = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!key)
            return std::nullopt;
        (*storage)->remove_item(*key);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *storage_proto, "clear", 0, [this_storage](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<StorageObject*> const storage = this_storage(interp, this_value);
        if (!storage)
            return std::nullopt;
        (*storage)->items.clear();
        return js::Value::undefined();
    });
    for (std::string_view const name : { "localStorage", "sessionStorage" })
        global->put(interpreter.key(name), js::Value::object(interpreter.heap().allocate<StorageObject>(storage_proto)), js::builtin_attributes);

    // Performance.
    js::Object* performance = interpreter.new_object();
    global->put(interpreter.key("performance"), js::Value::object(performance), js::builtin_attributes);
    js::define_method(interpreter, *performance, "now", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::number(internals.now() - internals.time_origin);
    });
    performance->put(interpreter.key("timeOrigin"), js::Value::number(in.time_origin), js::builtin_attributes);
    for (std::string_view const name : { "mark", "measure", "clearMarks", "clearMeasures", "clearResourceTimings", "setResourceTimingBufferSize" })
        js::define_method(interpreter, *performance, name, 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    for (std::string_view const name : { "getEntries", "getEntriesByType", "getEntriesByName" })
        js::define_method(interpreter, *performance, name, 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::object(interp.new_array()); });
    js::define_method(interpreter, *performance, "toJSON", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::object(interp.new_object()); });
    performance->put(interpreter.key("timing"), js::Value::object(object_with(in, { { "navigationStart", js::Value::number(in.time_origin) },
        { "fetchStart", js::Value::number(in.time_origin) }, { "domLoading", js::Value::number(in.time_origin) },
        { "responseEnd", js::Value::number(in.time_origin) }, { "domInteractive", js::Value::number(0) }, { "domContentLoadedEventStart", js::Value::number(0) },
        { "domContentLoadedEventEnd", js::Value::number(0) }, { "domComplete", js::Value::number(0) }, { "loadEventStart", js::Value::number(0) },
        { "loadEventEnd", js::Value::number(0) } })), js::builtin_attributes);
    performance->put(interpreter.key("navigation"), js::Value::object(object_with(in, { { "type", js::Value::number(0) }, { "redirectCount", js::Value::number(0) } })), js::builtin_attributes);
    performance->put(interpreter.key("eventCounts"), js::Value::object(object_with(in, { { "size", js::Value::number(0) } })), js::builtin_attributes);

    // Observers.
    install_observer(in, "IntersectionObserver", true, true);
    install_observer(in, "ResizeObserver", false, true);
    install_observer(in, "MutationObserver", false, false);
    js::Object* performance_observer = define_interface(in, "PerformanceObserver", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            js::Value const callback = js::argument(args, 0);
            if (!js::Interpreter::is_callable(callback))
                return interp.throw_type_error("Failed to construct 'PerformanceObserver': parameter 1 is not of type 'Function'.");
            return js::Value::object(interp.heap().allocate<ObserverObject>(internals_of(interp).prototype("PerformanceObserver"), callback));
        },
        1);
    for (std::string_view const name : { "observe", "disconnect" })
        js::define_method(interpreter, *performance_observer, name, 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    js::define_method(interpreter, *performance_observer, "takeRecords", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native { return js::Value::object(interp.new_array()); });
    {
        std::optional<js::Value> const constructor = performance_observer->get(interpreter, interpreter.key("constructor"), js::Value::object(performance_observer));
        if (constructor && constructor->is_object())
            constructor->as_object()->put(interpreter.key("supportedEntryTypes"), js::Value::object(interpreter.new_array()), js::builtin_attributes);
    }

    // customElements: definitions are taken, never upgraded (classes are v1).
    js::Object* custom_elements = interpreter.new_object();
    global->put(interpreter.key("customElements"), js::Value::object(custom_elements), js::builtin_attributes);
    js::define_method(interpreter, *custom_elements, "define", 2, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        internals.console("warn", "customElements.define('" + *name + "'): custom elements are not upgraded yet");
        return js::Value::undefined();
    });
    js::define_method(interpreter, *custom_elements, "get", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    js::define_method(interpreter, *custom_elements, "getName", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::null(); });
    js::define_method(interpreter, *custom_elements, "whenDefined", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    js::define_method(interpreter, *custom_elements, "upgrade", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });

    // crypto: randomUUID and getRandomValues over an array-like.
    js::Object* crypto = interpreter.new_object();
    global->put(interpreter.key("crypto"), js::Value::object(crypto), js::builtin_attributes);
    js::define_method(interpreter, *crypto, "randomUUID", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        std::random_device device;
        std::uniform_int_distribution<int> digit(0, 15);
        std::string out;
        static constexpr std::string_view pattern = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
        for (char const c : pattern) {
            if (c == 'x')
                out += "0123456789abcdef"[digit(device)];
            else if (c == 'y')
                out += "89ab"[digit(device) % 4];
            else
                out += c;
        }
        return internals_of(interp).string(out);
    });
    js::define_method(interpreter, *crypto, "getRandomValues", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        js::Value const array = js::argument(args, 0);
        if (!array.is_object())
            return interp.throw_type_error("Failed to execute 'getRandomValues' on 'Crypto': parameter 1 is not of type 'ArrayBufferView'.");
        std::optional<double> const length = interp.length_of_array_like(*array.as_object());
        if (!length)
            return std::nullopt;
        std::random_device device;
        std::uniform_int_distribution<int> byte(0, 255);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(*length); ++i) {
            if (!interp.set(array, interp.heap().key(i), js::Value::number(byte(device)), false))
                return std::nullopt;
        }
        return array;
    });

    // DOMException.
    js::Object* dom_exception = define_interface(in, "DOMException", interpreter.intrinsics().error_prototype,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> const message = js::argument(args, 0).is_undefined() ? std::optional<std::string>("") : internals.to_utf8(args[0]);
            std::optional<std::string> const name = js::argument(args, 1).is_undefined() ? std::optional<std::string>("Error") : internals.to_utf8(args[1]);
            if (!message || !name)
                return std::nullopt;
            // The realm's own thrower builds the same shape; it throws, so
            // the exception is taken back as the return value.
            internals.throw_dom_exception(*name, *message);
            return interp.take_exception();
        },
        0);
    for (auto const& [name, code] : { std::pair { "INDEX_SIZE_ERR", 1 }, std::pair { "HIERARCHY_REQUEST_ERR", 3 }, std::pair { "WRONG_DOCUMENT_ERR", 4 },
             std::pair { "INVALID_CHARACTER_ERR", 5 }, std::pair { "NO_MODIFICATION_ALLOWED_ERR", 7 }, std::pair { "NOT_FOUND_ERR", 8 },
             std::pair { "NOT_SUPPORTED_ERR", 9 }, std::pair { "INVALID_STATE_ERR", 11 }, std::pair { "SYNTAX_ERR", 12 },
             std::pair { "INVALID_MODIFICATION_ERR", 13 }, std::pair { "NAMESPACE_ERR", 14 }, std::pair { "INVALID_ACCESS_ERR", 15 },
             std::pair { "TYPE_MISMATCH_ERR", 17 }, std::pair { "SECURITY_ERR", 18 }, std::pair { "NETWORK_ERR", 19 }, std::pair { "ABORT_ERR", 20 },
             std::pair { "URL_MISMATCH_ERR", 21 }, std::pair { "QUOTA_EXCEEDED_ERR", 22 }, std::pair { "TIMEOUT_ERR", 23 },
             std::pair { "INVALID_NODE_TYPE_ERR", 24 }, std::pair { "DATA_CLONE_ERR", 25 } }) {
        dom_exception->put(interpreter.key(name), js::Value::number(code), js::Enumerable);
        std::optional<js::Value> const constructor = dom_exception->get(interpreter, interpreter.key("constructor"), js::Value::object(dom_exception));
        if (constructor && constructor->is_object())
            constructor->as_object()->put(interpreter.key(name), js::Value::number(code), js::Enumerable);
    }

    // URL and URLSearchParams.
    js::Object* url_proto = define_interface(in, "URL", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            std::optional<net::Url> base;
            js::Value const base_value = js::argument(args, 1);
            if (!base_value.is_undefined()) {
                std::optional<std::string> const base_text = internals.to_utf8(base_value);
                if (!base_text)
                    return std::nullopt;
                base = net::parse_url(*base_text);
                if (!base)
                    return interp.throw_type_error("Failed to construct 'URL': Invalid base URL");
            }
            std::optional<net::Url> url = net::parse_url(*text, base ? &*base : nullptr);
            if (!url)
                return interp.throw_type_error("Failed to construct 'URL': Invalid URL");
            return js::Value::object(interp.heap().allocate<UrlObject>(internals.prototype("URL"), std::move(*url)));
        },
        1);
    for (std::string_view const part : { "href", "protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin", "username", "password" }) {
        std::string const part_name(part);
        define_getter(
            in, *url_proto, part,
            [part_name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<UrlObject*> const url = this_url(interp, this_value);
                if (!url)
                    return std::nullopt;
                return internals_of(interp).string(url_part((*url)->url, part_name));
            },
            [part_name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<UrlObject*> const url = this_url(interp, this_value);
                if (!url)
                    return std::nullopt;
                Realm::Internals& internals = internals_of(interp);
                std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
                if (!text)
                    return std::nullopt;
                net::Url& target = (*url)->url;
                if (part_name == "href") {
                    std::optional<net::Url> parsed = net::parse_url(*text);
                    if (!parsed)
                        return interp.throw_type_error("Failed to set the 'href' property on 'URL': Invalid URL");
                    target = std::move(*parsed);
                } else if (part_name == "hash") {
                    std::string fragment = *text;
                    if (fragment.starts_with('#'))
                        fragment.erase(0, 1);
                    target.fragment = fragment.empty() ? std::nullopt : std::optional<std::string>(fragment);
                } else if (part_name == "search") {
                    std::string query = *text;
                    if (query.starts_with('?'))
                        query.erase(0, 1);
                    target.query = query.empty() ? std::nullopt : std::optional<std::string>(query);
                    if ((*url)->search_params)
                        static_cast<SearchParamsObject*>((*url)->search_params)->pairs = parse_query(query);
                } else if (part_name == "pathname") {
                    std::optional<net::Url> const parsed = net::parse_url(text->starts_with('/') ? *text : "/" + *text, &target);
                    if (parsed) {
                        target.path = parsed->path;
                        target.has_opaque_path = parsed->has_opaque_path;
                    }
                } else if (part_name == "protocol") {
                    std::string scheme = *text;
                    if (scheme.ends_with(':'))
                        scheme.pop_back();
                    std::optional<net::Url> const parsed = net::parse_url(scheme + target.serialize().substr(target.scheme.size()));
                    if (parsed)
                        target = *parsed;
                } else if (part_name == "host" || part_name == "hostname" || part_name == "port") {
                    std::string const serialized = target.serialize();
                    std::string const authority = target.host_with_port();
                    std::size_t const at = serialized.find(authority);
                    if (at != std::string::npos) {
                        std::string replacement = *text;
                        if (part_name == "hostname")
                            replacement = *text + (target.port ? ":" + target.port_string() : "");
                        else if (part_name == "port")
                            replacement = target.serialize_host() + (text->empty() ? "" : ":" + *text);
                        std::optional<net::Url> const parsed = net::parse_url(serialized.substr(0, at) + replacement + serialized.substr(at + authority.size()));
                        if (parsed)
                            target = *parsed;
                    }
                } else if (part_name == "username") {
                    target.username = *text;
                } else if (part_name == "password") {
                    target.password = *text;
                }
                return js::Value::undefined();
            });
    }
    js::define_method(interpreter, *url_proto, "toString", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<UrlObject*> const url = this_url(interp, this_value);
        if (!url)
            return std::nullopt;
        return internals_of(interp).string((*url)->url.serialize());
    });
    js::define_method(interpreter, *url_proto, "toJSON", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<UrlObject*> const url = this_url(interp, this_value);
        if (!url)
            return std::nullopt;
        return internals_of(interp).string((*url)->url.serialize());
    });
    define_getter(in, *url_proto, "searchParams", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<UrlObject*> const url = this_url(interp, this_value);
        if (!url)
            return std::nullopt;
        if (!(*url)->search_params) {
            auto* params = interp.heap().allocate<SearchParamsObject>(internals_of(interp).prototype("URLSearchParams"));
            params->owner = *url;
            params->pairs = parse_query((*url)->url.query.value_or(""));
            (*url)->search_params = params;
        }
        return js::Value::object((*url)->search_params);
    });
    {
        std::optional<js::Value> const constructor = url_proto->get(interpreter, interpreter.key("constructor"), js::Value::object(url_proto));
        if (constructor && constructor->is_object()) {
            js::Object& url_constructor = *constructor->as_object();
            js::define_method(interpreter, url_constructor, "canParse", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
                Realm::Internals& internals = internals_of(interp);
                std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
                if (!text)
                    return std::nullopt;
                std::optional<net::Url> base;
                if (!js::argument(args, 1).is_undefined()) {
                    std::optional<std::string> const base_text = internals.to_utf8(args[1]);
                    if (!base_text)
                        return std::nullopt;
                    base = net::parse_url(*base_text);
                    if (!base)
                        return js::Value::boolean(false);
                }
                return js::Value::boolean(net::parse_url(*text, base ? &*base : nullptr).has_value());
            });
            js::define_method(interpreter, url_constructor, "createObjectURL", 1, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
                return internals_of(interp).string("blob:" + internals_of(interp).url.serialize_origin() + "/00000000-0000-4000-8000-000000000000");
            });
            js::define_method(interpreter, url_constructor, "revokeObjectURL", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
        }
    }
    js::Object* params_proto = define_interface(in, "URLSearchParams", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            auto* params = interp.heap().allocate<SearchParamsObject>(internals.prototype("URLSearchParams"));
            js::Interpreter::Roots const roots(interp);
            interp.root(js::Value::object(params));
            js::Value const init = js::argument(args, 0);
            if (init.is_undefined())
                return js::Value::object(params);
            if (init.is_object()) {
                if (auto* other = dynamic_cast<SearchParamsObject*>(init.as_object())) {
                    params->pairs = other->pairs;
                    return js::Value::object(params);
                }
                if (js::Interpreter::is_array(init)) {
                    std::optional<std::vector<js::Value>> const entries = interp.create_list_from_array_like(init);
                    if (!entries)
                        return std::nullopt;
                    for (js::Value const& entry : *entries) {
                        std::optional<std::vector<js::Value>> const pair = interp.create_list_from_array_like(entry);
                        if (!pair)
                            return std::nullopt;
                        if (pair->size() != 2)
                            return interp.throw_type_error("Failed to construct 'URLSearchParams': Sequence initializer must only contain pair elements");
                        std::optional<std::string> const name = internals.to_utf8((*pair)[0]);
                        std::optional<std::string> const value = internals.to_utf8((*pair)[1]);
                        if (!name || !value)
                            return std::nullopt;
                        params->pairs.emplace_back(*name, *value);
                    }
                    return js::Value::object(params);
                }
                for (js::PropertyKey const& key : init.as_object()->own_keys()) {
                    if (!key.is_string())
                        continue;
                    std::optional<js::PropertyDescriptor> const descriptor = init.as_object()->get_own_property(key);
                    if (!descriptor || !descriptor->enumerable.value_or(false))
                        continue;
                    std::optional<js::Value> const value = interp.get(init, key);
                    if (!value)
                        return std::nullopt;
                    std::optional<std::string> const text = internals.to_utf8(*value);
                    if (!text)
                        return std::nullopt;
                    params->pairs.emplace_back(key.is_index() ? std::to_string(key.as_index()) : key.as_atom()->to_utf8(), *text);
                }
                return js::Value::object(params);
            }
            std::optional<std::string> const text = internals.to_utf8(init);
            if (!text)
                return std::nullopt;
            params->pairs = parse_query(*text);
            return js::Value::object(params);
        },
        0);
    js::define_method(interpreter, *params_proto, "append", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        std::optional<std::string> const value = internals_of(interp).to_utf8(js::argument(args, 1));
        if (!name || !value)
            return std::nullopt;
        (*params)->pairs.emplace_back(*name, *value);
        update_owner(**params);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *params_proto, "delete", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        auto& pairs = (*params)->pairs;
        pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](auto const& pair) { return pair.first == *name; }), pairs.end());
        update_owner(**params);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *params_proto, "get", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        for (auto const& [key, value] : (*params)->pairs) {
            if (key == *name)
                return internals_of(interp).string(value);
        }
        return js::Value::null();
    });
    js::define_method(interpreter, *params_proto, "getAll", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        js::ArrayObject* values = interp.new_array();
        interp.root(js::Value::object(values));
        for (auto const& [key, value] : (*params)->pairs) {
            if (key == *name)
                values->push(internals_of(interp).string(value));
        }
        return js::Value::object(values);
    });
    js::define_method(interpreter, *params_proto, "has", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        for (auto const& [key, value] : (*params)->pairs) {
            if (key == *name)
                return js::Value::boolean(true);
        }
        return js::Value::boolean(false);
    });
    js::define_method(interpreter, *params_proto, "set", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        std::optional<std::string> const value = internals_of(interp).to_utf8(js::argument(args, 1));
        if (!name || !value)
            return std::nullopt;
        auto& pairs = (*params)->pairs;
        bool replaced = false;
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (it->first != *name) {
                ++it;
            } else if (!replaced) {
                it->second = *value;
                replaced = true;
                ++it;
            } else {
                it = pairs.erase(it);
            }
        }
        if (!replaced)
            pairs.emplace_back(*name, *value);
        update_owner(**params);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *params_proto, "sort", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        std::stable_sort((*params)->pairs.begin(), (*params)->pairs.end(), [](auto const& a, auto const& b) {
            return js::utf16_from_utf8(a.first) < js::utf16_from_utf8(b.first);
        });
        update_owner(**params);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *params_proto, "toString", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        return internals_of(interp).string(serialize_query((*params)->pairs));
    });
    define_getter(in, *params_proto, "size", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*params)->pairs.size()));
    });
    js::define_method(interpreter, *params_proto, "forEach", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
        if (!params)
            return std::nullopt;
        js::Value const callback = js::argument(args, 0);
        if (!js::Interpreter::is_callable(callback))
            return interp.throw_type_error("parameter 1 is not of type 'Function'");
        std::vector<std::pair<std::string, std::string>> const pairs = (*params)->pairs;
        js::Interpreter::Roots const roots(interp);
        interp.root(callback);
        for (auto const& [name, value] : pairs) {
            js::Value const arguments[3] = { internals_of(interp).string(value), internals_of(interp).string(name), this_value };
            interp.root(arguments[0]);
            if (!interp.call(callback, js::argument(args, 1), arguments))
                return std::nullopt;
        }
        return js::Value::undefined();
    });
    for (std::string_view const name : { "keys", "values", "entries" }) {
        bool const pairs_out = name == "entries";
        bool const keys = name == "keys";
        js::define_method(interpreter, *params_proto, name, 0, [pairs_out, keys](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SearchParamsObject*> const params = this_params(interp, this_value);
            if (!params)
                return std::nullopt;
            js::Interpreter::Roots const roots(interp);
            js::ArrayObject* out = interp.new_array();
            interp.root(js::Value::object(out));
            for (auto const& [key, value] : (*params)->pairs) {
                if (keys) {
                    out->push(internals_of(interp).string(key));
                } else if (pairs_out) {
                    js::Value const pair[2] = { internals_of(interp).string(key), js::Value::undefined() };
                    interp.root(pair[0]);
                    js::ArrayObject* entry = interp.new_array(pair);
                    entry->set_element(1, internals_of(interp).string(value));
                    out->push(js::Value::object(entry));
                } else {
                    out->push(internals_of(interp).string(value));
                }
            }
            return js::Value::object(out);
        });
    }

    // The window's event handlers: the element set plus the window's own.
    static constexpr std::string_view window_event_types[] = { "load", "error", "resize", "scroll", "unload", "beforeunload", "hashchange",
        "popstate", "pageshow", "pagehide", "storage", "message", "messageerror", "online", "offline", "languagechange", "rejectionhandled",
        "unhandledrejection", "focus", "blur", "click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "mouseout", "mouseenter",
        "mouseleave", "keydown", "keyup", "keypress", "input", "change", "submit", "reset", "wheel", "contextmenu", "touchstart", "touchend",
        "touchmove", "touchcancel", "pointerdown", "pointerup", "pointermove", "pointerover", "pointerout", "pointerenter", "pointerleave",
        "pointercancel", "scrollend", "animationend", "animationstart", "animationiteration", "transitionend", "afterprint", "beforeprint",
        "select", "abort", "auxclick", "copy", "cut", "paste", "drag", "dragstart", "dragend", "dragover", "dragenter", "dragleave", "drop",
        "orientationchange", "devicemotion", "deviceorientation", "gamepadconnected", "gamepaddisconnected" };
    define_event_handlers(in, *global, window_event_types);
}

}
