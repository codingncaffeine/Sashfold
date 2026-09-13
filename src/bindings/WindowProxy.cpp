#include "bindings/Internal.h"

// The WindowProxy and Location exotic objects (HTML §7.2.3, §7.10) and the
// cross-origin rules they share. No script holds a window itself: it holds
// the window's WindowProxy, which forwards to the window of its frame's
// current document and is the same object before and after the frame goes on
// to another. To a script of another origin a WindowProxy or a Location
// shows only the few members HTML lets through — each through a function
// made for the realm that asks — and throws a SecurityError for the rest.
// Every member the window's interfaces put on its global object checks its
// `this` the way WebIDL has it for a [Global] interface, and the same-origin
// rule unless another origin may reach it.

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

namespace {

// CrossOriginProperties (HTML §7.2.3.2), a window's and a location's.
constexpr CrossOriginProperty window_cross_origin[] = {
    { "window", false, true, false, 0 },
    { "self", false, true, false, 0 },
    { "location", false, true, true, 0 },
    { "close", true, false, false, 0 },
    { "closed", false, true, false, 0 },
    { "focus", true, false, false, 0 },
    { "blur", true, false, false, 0 },
    { "frames", false, true, false, 0 },
    { "length", false, true, false, 0 },
    { "top", false, true, false, 0 },
    { "opener", false, true, false, 0 },
    { "parent", false, true, false, 0 },
    { "postMessage", true, false, false, 1 },
};

constexpr CrossOriginProperty location_cross_origin[] = {
    { "href", false, false, true, 0 },
    { "replace", true, false, false, 1 },
};

CrossOriginProperty const* find_cross_origin(std::span<CrossOriginProperty const> properties, std::string_view name)
{
    for (CrossOriginProperty const& property : properties) {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

OriginSnapshot snapshot_of(Realm::Internals const& in)
{
    return OriginSnapshot { in.origin_url.serialize_origin(), in.origin_url.scheme, in.domain.get() };
}

// The origin of a realm's document, or the one it had when its realm ended;
// none for a realm no host stands behind.
std::optional<OriginSnapshot> origin_of(js::RealmRecord const& record)
{
    if (record.host_defined == nullptr)
        return std::nullopt;
    Realm::Internals const& in = static_cast<Realm*>(record.host_defined)->internals();
    if (in.ended) {
        auto const found = in.agent.ended_origins.find(&record);
        if (found != in.agent.ended_origins.end())
            return found->second;
    }
    return snapshot_of(in);
}

// Same origin-domain (HTML §7.1.1): two tuple origins whose documents both
// set the same domain under one scheme, or the same origin where neither did.
// An opaque origin is only its own, which the caller has already asked.
bool same_origin_domain(OriginSnapshot const& a, OriginSnapshot const& b)
{
    if (a.serialized == "null" || b.serialized == "null")
        return false;
    if (a.domain || b.domain)
        return a.domain && b.domain && a.scheme == b.scheme && *a.domain == *b.domain;
    return a.serialized == b.serialized;
}

void collect_iframes(dom::Node const& node, std::vector<dom::Element const*>& out)
{
    for (dom::Node const* const child : node.children()) {
        if (child->is_element() && static_cast<dom::Element const*>(child)->is_html("iframe"))
            out.push_back(static_cast<dom::Element const*>(child));
        collect_iframes(*child, out);
    }
}

// A function of the realm that asks, which performs a member's steps: those
// of the window's or location's own member, with the `this` it is called
// with, so that anything but that window or location is refused as the
// member itself refuses it (WebIDL §3.7.6). The steps run in this function's
// realm rather than the member's, so what they throw before they reach the
// window — a `this` that is no window, an argument that is no string — is
// the asking realm's error.
js::Object* forwarding_function(js::Interpreter& interpreter, std::string_view name, int length, js::Value const& original)
{
    return interpreter.new_closure(name, length, { original },
        [](js::Interpreter& interp, js::ClosureFunction& function, js::Value const& this_value, std::span<js::Value const> arguments) -> std::optional<js::Value> {
            js::Value const member = function.slot(0);
            if (auto* const native = member.is_object() ? dynamic_cast<js::NativeFunction*>(member.as_object()) : nullptr)
                return native->callback()(interp, this_value, arguments);
            return interp.call(member, this_value, arguments);
        });
}

// The arguments with the first, a URL, converted to a string before a member
// enters its window's realm (WebIDL §3.2.10, USVString): a value that cannot
// be converted throws in the realm of the script that passed it.
std::optional<std::vector<js::Value>> with_url_converted(js::Interpreter& interpreter, Args arguments)
{
    std::vector<js::Value> converted(arguments.begin(), arguments.end());
    if (converted.empty())
        return converted;
    std::optional<js::JsString*> const text = interpreter.to_string(converted[0]);
    if (!text)
        return std::nullopt;
    converted[0] = js::Value::string(*text);
    return converted;
}

std::uint8_t attributes_of(js::PropertyDescriptor const& descriptor)
{
    std::uint8_t attributes = 0;
    if (descriptor.writable.value_or(false))
        attributes |= js::Writable;
    if (descriptor.enumerable.value_or(false))
        attributes |= js::Enumerable;
    if (descriptor.configurable.value_or(false))
        attributes |= js::Configurable;
    return attributes;
}

// A member of a [Global] interface called with `this` (WebIDL §3.7.6,
// §3.7.7): undefined or null is the function's own window, and so is its
// global object; a WindowProxy is the window it stands for; anything else is
// no window. The member then runs as that window's, after the security check
// (HTML §7.2.3.2) unless another origin may reach it. [LegacyLenientThis]
// members answer undefined for no window instead of throwing. A member that
// takes a URL has it converted first, in the realm of the script that called.
js::NativeFunction::Callback window_member(js::NativeFunction::Callback original, bool shown_to_other_origins, bool lenient_this, bool takes_url)
{
    return [original = std::move(original), shown_to_other_origins, lenient_this, takes_url](
               js::Interpreter& interpreter, js::Value const& this_value, Args arguments) -> Native {
        js::RealmRecord* target = nullptr;
        if (this_value.is_nullish()) {
            target = interpreter.current_realm();
        } else if (WindowProxyObject* const proxy = as_window_proxy(this_value)) {
            target = &proxy->record();
        } else if (this_value.is_object() && this_value.as_object() == interpreter.global()) {
            target = interpreter.current_realm();
        }
        if (target == nullptr) {
            if (lenient_this)
                return js::Value::undefined();
            return interpreter.throw_type_error("Illegal invocation");
        }
        if (!shown_to_other_origins && !is_platform_object_same_origin(interpreter, *target))
            return throw_security_error(interpreter);
        js::Interpreter::Roots const roots(interpreter);
        std::optional<std::vector<js::Value>> converted;
        if (takes_url) {
            converted = with_url_converted(interpreter, arguments);
            if (!converted)
                return std::nullopt;
            for (js::Value const& argument : *converted)
                interpreter.root(argument);
        }
        js::Interpreter::RealmScope const inside(interpreter, target);
        return original(interpreter, this_value, converted ? Args(*converted) : arguments);
    };
}

} // namespace

// --- The rules ------------------------------------------------------------------------------

bool is_platform_object_same_origin(js::Interpreter& interpreter, js::RealmRecord const& window_realm)
{
    js::RealmRecord const* const current = interpreter.current_realm();
    if (current == &window_realm)
        return true;
    std::optional<OriginSnapshot> const mine = origin_of(*current);
    std::optional<OriginSnapshot> const theirs = origin_of(window_realm);
    if (!mine || !theirs)
        return true; // no host behind one of them: no origins to keep apart
    return same_origin_domain(*mine, *theirs);
}

std::optional<js::Value> throw_security_error(js::Interpreter& interpreter)
{
    if (interpreter.current_realm()->host_defined == nullptr)
        return interpreter.throw_type_error("Blocked a frame from accessing a cross-origin frame.");
    Realm::Internals& internals = internals_of(interpreter);
    return internals.throw_dom_exception("SecurityError",
        "Blocked a frame with origin \"" + internals.origin_url.serialize_origin() + "\" from accessing a cross-origin frame.");
}

std::vector<ChildFrame const*> child_navigables(Realm::Internals const& in)
{
    std::vector<ChildFrame const*> children;
    if (in.ended || in.discarded || in.child_frames.empty())
        return children;
    std::vector<dom::Element const*> iframes;
    collect_iframes(in.document, iframes);
    for (dom::Element const* const iframe : iframes) {
        for (ChildFrame const& frame : in.child_frames) {
            if (frame.container == iframe) {
                children.push_back(&frame);
                break;
            }
        }
    }
    return children;
}

WindowProxyObject* as_window_proxy(js::Value const& value)
{
    if (!value.is_object() || !value.as_object()->is_proxy())
        return nullptr;
    return dynamic_cast<WindowProxyObject*>(value.as_object());
}

LocationObject* as_location(js::Value const& value)
{
    if (!value.is_object() || !value.as_object()->is_proxy())
        return nullptr;
    return dynamic_cast<LocationObject*>(value.as_object());
}

js::Object* Realm::Internals::window_proxy() const
{
    return realm_record->global_this ? realm_record->global_this : realm_record->intrinsics.global;
}

bool Realm::Internals::is_window(js::Object const* object) const
{
    return object != nullptr && (object == realm_record->intrinsics.global || object == realm_record->global_this);
}

// --- CrossOriginObject ----------------------------------------------------------------------

CrossOriginObject::CrossOriginObject(js::Object& target, js::RealmRecord& window_realm)
    : js::ProxyObject(&target, &target)
    , m_record(&window_realm)
{
}

std::optional<bool> CrossOriginObject::has_property(js::Interpreter& interpreter, js::PropertyKey const& key)
{
    std::optional<std::optional<js::PropertyDescriptor>> const own = get_own_property(interpreter, key);
    if (!own)
        return std::nullopt;
    if (*own)
        return true;
    std::optional<js::Object*> const parent = get_prototype_of(interpreter);
    if (!parent)
        return std::nullopt;
    if (*parent == nullptr)
        return false;
    return interpreter.has_property(**parent, key);
}

void CrossOriginObject::trace(js::Tracer& tracer)
{
    js::ProxyObject::trace(tracer);
    tracer.visit(m_record);
    for (CrossOriginEntry const& entry : m_cross_origin) {
        tracer.visit(entry.current);
        tracer.visit(entry.object_realm);
        if (entry.descriptor.value)
            tracer.visit(*entry.descriptor.value);
        if (entry.descriptor.get)
            tracer.visit(*entry.descriptor.get);
        if (entry.descriptor.set)
            tracer.visit(*entry.descriptor.set);
    }
}

std::optional<js::PropertyDescriptor> CrossOriginObject::cross_origin_property(js::Interpreter& interpreter, js::PropertyKey const& key)
{
    if (!key.is_atom())
        return std::nullopt;
    CrossOriginProperty const* const property = find_cross_origin(cross_origin_properties(), key.as_atom()->to_utf8());
    if (property == nullptr)
        return std::nullopt;
    js::RealmRecord* const current = interpreter.current_realm();
    for (CrossOriginEntry const& entry : m_cross_origin) {
        if (entry.current == current && entry.object_realm == m_record && entry.name == property->name)
            return entry.descriptor;
    }
    std::optional<js::PropertyDescriptor> const original = original_member(interpreter, property->name);
    if (!original)
        return std::nullopt;
    js::Interpreter::Roots const roots(interpreter);
    js::PropertyDescriptor shown;
    std::string const name(property->name);
    if (property->method) {
        js::Value value = original->value.value_or(js::Value::undefined());
        if (js::Interpreter::is_callable(value))
            value = js::Value::object(forwarding_function(interpreter, name, property->length, value));
        shown = js::PropertyDescriptor::data(value, js::Configurable);
    } else {
        js::Object* getter = nullptr;
        js::Object* setter = nullptr;
        if (property->needs_get && original->get && *original->get) {
            getter = forwarding_function(interpreter, "get " + name, 0, js::Value::object(*original->get));
            interpreter.root(js::Value::object(getter));
        }
        if (property->needs_set && original->set && *original->set)
            setter = forwarding_function(interpreter, "set " + name, 1, js::Value::object(*original->set));
        shown = js::PropertyDescriptor::accessor(getter, setter, js::Configurable);
    }
    m_cross_origin.push_back(CrossOriginEntry { current, m_record, property->name, shown });
    return shown;
}

std::optional<js::Value> CrossOriginObject::cross_origin_get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    std::optional<std::optional<js::PropertyDescriptor>> const own = get_own_property(interpreter, key);
    if (!own)
        return std::nullopt;
    if (!*own)
        return js::Value::undefined();
    js::PropertyDescriptor const& descriptor = **own;
    if (!descriptor.is_accessor())
        return descriptor.value.value_or(js::Value::undefined());
    if (!descriptor.get || *descriptor.get == nullptr)
        return throw_security_error(interpreter);
    return interpreter.call(js::Value::object(*descriptor.get), receiver, {});
}

std::optional<bool> CrossOriginObject::cross_origin_set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value,
    js::Value const& receiver)
{
    std::optional<std::optional<js::PropertyDescriptor>> const own = get_own_property(interpreter, key);
    if (!own)
        return std::nullopt;
    if (*own && (*own)->set && *(*own)->set != nullptr) {
        js::Value const arguments[1] = { value };
        if (!interpreter.call(js::Value::object(*(*own)->set), receiver, arguments))
            return std::nullopt;
        return true;
    }
    throw_security_error(interpreter);
    return std::nullopt;
}

std::vector<js::PropertyKey> CrossOriginObject::cross_origin_keys(js::Interpreter& interpreter) const
{
    std::vector<js::PropertyKey> keys;
    for (CrossOriginProperty const& property : cross_origin_properties())
        keys.push_back(interpreter.key(property.name));
    keys.push_back(interpreter.key("then"));
    keys.push_back(js::PropertyKey::symbol(interpreter.atoms().symbol_to_string_tag));
    keys.push_back(js::PropertyKey::symbol(interpreter.atoms().symbol_has_instance));
    keys.push_back(js::PropertyKey::symbol(interpreter.atoms().symbol_is_concat_spreadable));
    return keys;
}

std::optional<std::optional<js::PropertyDescriptor>> CrossOriginObject::cross_origin_fallback(js::Interpreter& interpreter, js::PropertyKey const& key)
{
    bool const shown = key == interpreter.key("then") || key == js::PropertyKey::symbol(interpreter.atoms().symbol_to_string_tag)
        || key == js::PropertyKey::symbol(interpreter.atoms().symbol_has_instance)
        || key == js::PropertyKey::symbol(interpreter.atoms().symbol_is_concat_spreadable);
    if (shown)
        return std::optional<js::PropertyDescriptor>(js::PropertyDescriptor::data(js::Value::undefined(), js::Configurable));
    throw_security_error(interpreter);
    return std::nullopt;
}

// --- WindowProxyObject ----------------------------------------------------------------------

WindowProxyObject::WindowProxyObject(js::RealmRecord& window_realm)
    : CrossOriginObject(*window_realm.intrinsics.global, window_realm)
{
}

Realm::Internals& WindowProxyObject::internals() const
{
    return static_cast<Realm*>(m_record->host_defined)->internals();
}

void WindowProxyObject::stand_for(js::RealmRecord& window_realm)
{
    m_record = &window_realm;
    retarget(window_realm.intrinsics.global);
}

std::span<CrossOriginProperty const> WindowProxyObject::cross_origin_properties() const
{
    return window_cross_origin;
}

std::optional<js::PropertyDescriptor> WindowProxyObject::original_member(js::Interpreter&, std::string_view name) const
{
    if (m_record->host_defined == nullptr)
        return std::nullopt;
    auto const found = internals().cross_origin_members.find(name);
    if (found == internals().cross_origin_members.end())
        return std::nullopt;
    return found->second;
}

std::optional<js::PropertyDescriptor> WindowProxyObject::child_window(js::PropertyKey const& key) const
{
    if (!key.is_index() || m_record->host_defined == nullptr)
        return std::nullopt;
    std::vector<ChildFrame const*> const children = child_navigables(internals());
    if (key.as_index() >= children.size())
        return std::nullopt;
    return js::PropertyDescriptor::data(js::Value::object(children[key.as_index()]->realm->internals().window_proxy()), js::Enumerable | js::Configurable);
}

// [[GetPrototypeOf]] (HTML §7.2.3.3.1): the window's, or null to another origin.
std::optional<js::Object*> WindowProxyObject::get_prototype_of(js::Interpreter& interpreter)
{
    if (!is_platform_object_same_origin(interpreter, *m_record))
        return static_cast<js::Object*>(nullptr);
    return window().prototype();
}

// [[SetPrototypeOf]]: SetImmutablePrototype (§10.4.7.2).
std::optional<bool> WindowProxyObject::set_prototype_of(js::Interpreter& interpreter, js::Object* prototype)
{
    std::optional<js::Object*> const current = get_prototype_of(interpreter);
    if (!current)
        return std::nullopt;
    return *current == prototype;
}

std::optional<bool> WindowProxyObject::is_extensible(js::Interpreter&)
{
    return true;
}

std::optional<bool> WindowProxyObject::prevent_extensions(js::Interpreter&)
{
    return false;
}

// [[GetOwnProperty]] (HTML §7.2.3.3.5): a frame by index first, whatever the
// origin; the window's own property to its origin; to another, a
// CrossOriginProperty, then a frame by its name, then the fallback.
std::optional<std::optional<js::PropertyDescriptor>> WindowProxyObject::get_own_property(js::Interpreter& interpreter, js::PropertyKey const& key)
{
    if (std::optional<js::PropertyDescriptor> const child = child_window(key))
        return child;
    if (is_platform_object_same_origin(interpreter, *m_record))
        return window().get_own_property(key);
    if (std::optional<js::PropertyDescriptor> const shown = cross_origin_property(interpreter, key))
        return shown;
    if (key.is_atom() && m_record->host_defined != nullptr) {
        std::string const name = key.as_atom()->to_utf8();
        for (ChildFrame const* const child : child_navigables(internals())) {
            if (child->realm->internals().window_name == name)
                return std::optional<js::PropertyDescriptor>(js::PropertyDescriptor::data(js::Value::object(child->realm->internals().window_proxy()), js::Configurable));
        }
    }
    return cross_origin_fallback(interpreter, key);
}

std::optional<bool> WindowProxyObject::define_own_property(js::Interpreter& interpreter, js::PropertyKey const& key, js::PropertyDescriptor const& descriptor)
{
    if (!is_platform_object_same_origin(interpreter, *m_record)) {
        throw_security_error(interpreter);
        return std::nullopt;
    }
    if (key.is_index())
        return false;
    return interpreter.define_own_property(window(), key, descriptor);
}

std::optional<bool> WindowProxyObject::delete_property(js::Interpreter& interpreter, js::PropertyKey const& key)
{
    if (!is_platform_object_same_origin(interpreter, *m_record)) {
        throw_security_error(interpreter);
        return std::nullopt;
    }
    if (key.is_index())
        return !child_window(key).has_value();
    return interpreter.delete_property(window(), key);
}

// [[OwnPropertyKeys]]: the frames' indices, then the window's own keys to its
// origin, or CrossOriginOwnPropertyKeys to another.
std::optional<std::vector<js::PropertyKey>> WindowProxyObject::own_keys(js::Interpreter& interpreter)
{
    std::vector<js::PropertyKey> keys;
    if (m_record->host_defined != nullptr) {
        std::size_t const count = child_navigables(internals()).size();
        for (std::size_t index = 0; index < count; ++index)
            keys.push_back(js::PropertyKey::index(static_cast<std::uint32_t>(index)));
    }
    std::vector<js::PropertyKey> const rest
        = is_platform_object_same_origin(interpreter, *m_record) ? window().own_keys() : cross_origin_keys(interpreter);
    keys.insert(keys.end(), rest.begin(), rest.end());
    return keys;
}

std::optional<js::Value> WindowProxyObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (std::optional<js::PropertyDescriptor> const child = child_window(key))
        return child->value;
    if (is_platform_object_same_origin(interpreter, *m_record))
        return window().get(interpreter, key, receiver);
    return cross_origin_get(interpreter, key, receiver);
}

std::optional<bool> WindowProxyObject::set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver)
{
    if (is_platform_object_same_origin(interpreter, *m_record)) {
        if (key.is_index())
            return false;
        return window().set(interpreter, key, value, receiver);
    }
    return cross_origin_set(interpreter, key, value, receiver);
}

// --- LocationObject -------------------------------------------------------------------------

LocationObject::LocationObject(js::Object& members, js::RealmRecord& window_realm)
    : CrossOriginObject(members, window_realm)
    , m_default_properties(members.own_keys())
{
}

void LocationObject::trace(js::Tracer& tracer)
{
    CrossOriginObject::trace(tracer);
    for (js::PropertyKey const& key : m_default_properties)
        tracer.visit(key);
}

bool LocationObject::is_default_property(js::PropertyKey const& key) const
{
    return std::find(m_default_properties.begin(), m_default_properties.end(), key) != m_default_properties.end();
}

std::span<CrossOriginProperty const> LocationObject::cross_origin_properties() const
{
    return location_cross_origin;
}

std::optional<js::PropertyDescriptor> LocationObject::original_member(js::Interpreter& interpreter, std::string_view name) const
{
    return target()->get_own_property(interpreter.key(name));
}

std::optional<js::Object*> LocationObject::get_prototype_of(js::Interpreter& interpreter)
{
    if (!is_platform_object_same_origin(interpreter, *m_record))
        return static_cast<js::Object*>(nullptr);
    return target()->prototype();
}

std::optional<bool> LocationObject::set_prototype_of(js::Interpreter& interpreter, js::Object* prototype)
{
    std::optional<js::Object*> const current = get_prototype_of(interpreter);
    if (!current)
        return std::nullopt;
    return *current == prototype;
}

std::optional<bool> LocationObject::is_extensible(js::Interpreter&)
{
    return true;
}

std::optional<bool> LocationObject::prevent_extensions(js::Interpreter&)
{
    return false;
}

// [[GetOwnProperty]] (HTML §7.10.3.5): to its origin the object's own
// property, reported configurable when it is one it was made with; to
// another, a CrossOriginProperty or the fallback.
std::optional<std::optional<js::PropertyDescriptor>> LocationObject::get_own_property(js::Interpreter& interpreter, js::PropertyKey const& key)
{
    if (is_platform_object_same_origin(interpreter, *m_record)) {
        std::optional<js::PropertyDescriptor> descriptor = target()->get_own_property(key);
        if (descriptor && is_default_property(key))
            descriptor->configurable = true;
        return descriptor;
    }
    if (std::optional<js::PropertyDescriptor> const shown = cross_origin_property(interpreter, key))
        return shown;
    return cross_origin_fallback(interpreter, key);
}

std::optional<bool> LocationObject::define_own_property(js::Interpreter& interpreter, js::PropertyKey const& key, js::PropertyDescriptor const& descriptor)
{
    if (!is_platform_object_same_origin(interpreter, *m_record)) {
        throw_security_error(interpreter);
        return std::nullopt;
    }
    if (is_default_property(key))
        return false;
    return interpreter.define_own_property(*target(), key, descriptor);
}

std::optional<bool> LocationObject::delete_property(js::Interpreter& interpreter, js::PropertyKey const& key)
{
    if (!is_platform_object_same_origin(interpreter, *m_record)) {
        throw_security_error(interpreter);
        return std::nullopt;
    }
    return interpreter.delete_property(*target(), key);
}

std::optional<std::vector<js::PropertyKey>> LocationObject::own_keys(js::Interpreter& interpreter)
{
    if (is_platform_object_same_origin(interpreter, *m_record))
        return target()->own_keys();
    return cross_origin_keys(interpreter);
}

std::optional<js::Value> LocationObject::get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver)
{
    if (is_platform_object_same_origin(interpreter, *m_record))
        return target()->get(interpreter, key, receiver);
    return cross_origin_get(interpreter, key, receiver);
}

std::optional<bool> LocationObject::set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver)
{
    if (is_platform_object_same_origin(interpreter, *m_record))
        return target()->set(interpreter, key, value, receiver);
    return cross_origin_set(interpreter, key, value, receiver);
}

// --- The members ----------------------------------------------------------------------------

js::NativeFunction::Callback location_member(js::NativeFunction::Callback original, bool shown_to_other_origins)
{
    return [original = std::move(original), shown_to_other_origins](js::Interpreter& interpreter, js::Value const& this_value, Args arguments) -> Native {
        LocationObject* const location = as_location(this_value);
        if (location == nullptr)
            return interpreter.throw_type_error("Illegal invocation");
        if (!shown_to_other_origins && !is_platform_object_same_origin(interpreter, location->record()))
            return throw_security_error(interpreter);
        // Every member of a Location that takes an argument takes a URL.
        js::Interpreter::Roots const roots(interpreter);
        std::optional<std::vector<js::Value>> const converted = with_url_converted(interpreter, arguments);
        if (!converted)
            return std::nullopt;
        for (js::Value const& argument : *converted)
            interpreter.root(argument);
        js::Interpreter::RealmScope const inside(interpreter, &location->record());
        return original(interpreter, this_value, Args(*converted));
    };
}

namespace {

// The window's [LegacyUnforgeable] attributes (HTML §7.2.2), whose properties
// cannot be configured, and its [Replaceable] ones (HTML §7.2.2, CSSOM View,
// High Resolution Time), which a script's assignment replaces with a data
// property of the window's own.
constexpr std::string_view unforgeable_members[] = { "window", "document", "location", "top" };
constexpr std::string_view replaceable_members[] = { "self", "locationbar", "menubar", "personalbar", "scrollbars", "statusbar", "toolbar",
    "frames", "parent", "length", "origin", "external", "screen", "scrollX",
    "scrollY", "pageXOffset", "pageYOffset", "innerWidth", "innerHeight", "screenLeft", "screenTop", "screenX", "screenY", "outerWidth",
    "outerHeight", "devicePixelRatio", "event", "performance", "visualViewport", "clientInformation" };
// Namespace objects, which are plain data properties like Math.
constexpr std::string_view namespace_members[] = { "CSS" };

bool listed(std::span<std::string_view const> names, std::string_view name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

js::NativeFunction::Callback replaceable_setter(std::string name)
{
    return [name = std::move(name)](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        interp.global()->put(interp.key(name), js::argument(args, 0), js::default_attributes);
        return js::Value::undefined();
    };
}

// The window's members as WebIDL makes a [Global] interface's (§3.7.5 to
// §3.7.7): operations writable, enumerable and configurable; attributes
// accessors, enumerable, configurable unless unforgeable, with a setter
// when replaceable; an attribute the installers kept as a plain value
// becomes a getter over it.
void shape_window_members(Realm::Internals& in, std::vector<js::PropertyKey> const& language_globals)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object& global = *interpreter.global();
    for (js::PropertyKey const& key : global.own_keys()) {
        if (!key.is_atom() || std::find(language_globals.begin(), language_globals.end(), key) != language_globals.end())
            continue;
        std::optional<js::PropertyDescriptor> const own = global.get_own_property(key);
        if (!own)
            continue;
        std::string const name = key.as_atom()->to_utf8();
        bool const replaceable = listed(replaceable_members, name);
        std::uint8_t const accessor_attributes = listed(unforgeable_members, name) ? js::Enumerable : js::Enumerable | js::Configurable;
        if (own->is_accessor()) {
            js::Object* setter = own->set ? *own->set : nullptr;
            if (setter == nullptr && replaceable)
                setter = interpreter.new_native("set " + name, 1, replaceable_setter(name));
            global.put_accessor(key, own->get ? *own->get : nullptr, setter, accessor_attributes);
            continue;
        }
        js::Value const value = own->value.value_or(js::Value::undefined());
        if (js::Interpreter::is_callable(value)) {
            auto* const native = dynamic_cast<js::NativeFunction*>(value.as_object());
            if (native != nullptr && !native->is_constructor() && !native->get_own_property(interpreter.key("prototype")))
                global.put(key, value, js::Writable | js::Enumerable | js::Configurable);
            continue;
        }
        if (listed(namespace_members, name))
            continue;
        in.window_values[name] = value;
        js::NativeFunction* const getter = interpreter.new_native("get " + name, 0, [name](js::Interpreter& interp, js::Value const&, Args) -> Native {
            auto const& values = internals_of(interp).window_values;
            auto const found = values.find(name);
            return found != values.end() ? found->second : js::Value::undefined();
        });
        js::NativeFunction* setter = nullptr;
        if (replaceable) {
            setter = interpreter.new_native("set " + name, 1, replaceable_setter(name));
        } else if (name == "status") {
            setter = interpreter.new_native("set status", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
                Realm::Internals& internals = internals_of(interp);
                std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
                if (!text)
                    return std::nullopt;
                internals.window_values["status"] = internals.string(*text);
                return js::Value::undefined();
            });
        }
        global.put_accessor(key, getter, setter, accessor_attributes);
    }
}

} // namespace

void install_window_proxy(Realm::Internals& in, std::vector<js::PropertyKey> const& language_globals)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object& global = *interpreter.global();
    shape_window_members(in, language_globals);
    // A method or an accessor function made by the interfaces, put behind the
    // checks; an interface object, which has a prototype, is left as it is.
    auto const guarded = [&interpreter](js::Object* function, bool shown, bool lenient, bool takes_url) -> js::Object* {
        auto* const native = dynamic_cast<js::NativeFunction*>(function);
        if (native == nullptr || native->is_constructor() || native->get_own_property(interpreter.key("prototype")))
            return function;
        std::optional<js::PropertyDescriptor> const length = native->get_own_property(interpreter.key("length"));
        std::optional<js::PropertyDescriptor> const name = native->get_own_property(interpreter.key("name"));
        int const arity = length && length->value && length->value->is_number() ? static_cast<int>(length->value->as_number()) : 0;
        std::string const function_name = name && name->value && name->value->is_string() ? name->value->as_string()->to_utf8() : std::string();
        return interpreter.new_native(function_name, arity, window_member(native->callback(), shown, lenient, takes_url));
    };
    for (js::PropertyKey const& key : global.own_keys()) {
        if (!key.is_atom() || std::find(language_globals.begin(), language_globals.end(), key) != language_globals.end())
            continue;
        std::optional<js::PropertyDescriptor> const own = global.get_own_property(key);
        if (!own)
            continue;
        std::string const name = key.as_atom()->to_utf8();
        CrossOriginProperty const* const shown = find_cross_origin(window_cross_origin, name);
        bool const lenient = name == "onmouseenter" || name == "onmouseleave";
        // window.location's setter forwards a URL to the Location's href.
        bool const takes_url = name == "location";
        if (own->is_accessor()) {
            js::Object* const getter = own->get && *own->get ? guarded(*own->get, shown != nullptr, lenient, takes_url) : nullptr;
            js::Object* const setter = own->set && *own->set ? guarded(*own->set, shown != nullptr, lenient, takes_url) : nullptr;
            global.put_accessor(key, getter, setter, attributes_of(*own));
            if (shown)
                in.cross_origin_members[shown->name] = js::PropertyDescriptor::accessor(getter, setter, attributes_of(*own));
        } else if (own->value && js::Interpreter::is_callable(*own->value)) {
            js::Object* const function = guarded(own->value->as_object(), shown != nullptr, lenient, takes_url);
            if (function != own->value->as_object())
                global.put(key, js::Value::object(function), attributes_of(*own));
            if (shown)
                in.cross_origin_members[shown->name] = js::PropertyDescriptor::data(js::Value::object(function), attributes_of(*own));
        }
    }
    interpreter.set_global_this(*in.realm_record, interpreter.heap().allocate<WindowProxyObject>(*in.realm_record));
}

}
