#include "bindings/Eme.h"

#include "media/ClearKey.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Encrypted Media Extensions (W3C Recommendation, with the Encryption Scheme
// Query and HDCP Policy Check additions) and its one key system here, Clear
// Key (§9.1). A page asks navigator.requestMediaKeySystemAccess for a key
// system and the configurations it would play; the answer's createMediaKeys
// makes the content decryption module (for Clear Key, a table of keys:
// media::KeyStore); its sessions turn initialization data into license
// requests ("message" events) and licenses (update()) into keys; a media
// element given the MediaKeys (setMediaKeys, Media.cpp) decrypts with them.
//
// No other key system is known: every other name is "not supported", and
// nothing here looks for, loads or talks to a decryption module of anyone
// else's. Clear Key offers temporary sessions only, as the shipping
// browsers do: nothing is stored, nothing is identified.

namespace sashfold::bindings {

namespace {

using media::Bytes;

constexpr std::string_view clear_key = "org.w3.clearkey";

// --- The objects ----------------------------------------------------------------------------

struct Capability {
    std::string content_type;
    std::optional<std::string> encryption_scheme;
    std::string robustness;
};

struct Configuration {
    std::string label;
    std::vector<std::string> init_data_types;
    std::vector<Capability> audio;
    std::vector<Capability> video;
    std::string distinctive_identifier = "optional";
    std::string persistent_state = "optional";
    std::optional<std::vector<std::string>> session_types;
};

class MediaKeySystemAccessObject final : public js::Object {
public:
    explicit MediaKeySystemAccessObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::string key_system;
    Configuration configuration;
};

class MediaKeysObject final : public js::Object {
public:
    explicit MediaKeysObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::shared_ptr<media::KeyStore> store = std::make_shared<media::KeyStore>();
    std::vector<std::string> session_types; // what the configuration allowed
    js::Object* element_state = nullptr; // the media element using it (setMediaKeys)
    // Its sessions that are not closed: a session lives, and keeps its
    // keys, while it is open and its MediaKeys can be reached (§6).
    std::vector<js::Object*> open_sessions;

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(element_state);
        for (js::Object* session : open_sessions)
            tracer.visit(session);
    }
};

// A read-only map of key IDs to their status (EME §5), kept in key ID order.
class MediaKeyStatusMapObject final : public js::Object {
public:
    explicit MediaKeyStatusMapObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::vector<std::pair<Bytes, std::string>> entries;
};

class MediaKeySessionObject final : public EventTargetObject {
public:
    explicit MediaKeySessionObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    ~MediaKeySessionObject() override
    {
        // A session nothing can reach any more holds no keys.
        if (store)
            store->remove_session(number);
    }
    MediaKeysObject* keys = nullptr;
    std::shared_ptr<media::KeyStore> store;
    std::uint64_t number = 0; // the session's own, for the key store
    std::string session_type;
    std::string session_id;
    double expiration = std::nan("");
    // §6's flags: generateRequest or load not called yet; a request made
    // (update, close, remove may be called); closing or closed.
    bool uninitialized = true;
    bool callable = false;
    bool closing_or_closed = false;
    MediaKeyStatusMapObject* statuses = nullptr;
    js::Value closed_promise = js::Value::undefined();
    js::Value closed_resolve = js::Value::undefined();
    std::vector<Bytes> requested; // the key IDs the license request named

    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(static_cast<js::Object*>(keys));
        tracer.visit(static_cast<js::Object*>(statuses));
        tracer.visit(closed_promise);
        tracer.visit(closed_resolve);
    }
};

template<typename T>
T* object_as(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<T*>(value.as_object()) : nullptr;
}

template<typename T>
T* this_or_throw(js::Interpreter& interp, js::Value const& this_value)
{
    T* const found = object_as<T>(this_value);
    if (found == nullptr)
        interp.throw_type_error("Illegal invocation");
    return found;
}

// --- Values across the boundary ---------------------------------------------------------------

js::Value array_buffer_of(js::Interpreter& interp, std::span<std::uint8_t const> bytes)
{
    std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(bytes.size()), std::nullopt);
    if (!buffer)
        return js::Value::null();
    if (!bytes.empty())
        std::memcpy((*buffer)->data(), bytes.data(), bytes.size());
    return js::Value::object(*buffer);
}

js::Value type_error_value(js::Interpreter& interp, std::string_view message)
{
    interp.throw_type_error(message);
    return interp.take_exception();
}

// A promise made now and settled by a task later: its functions held across.
struct Settler {
    std::shared_ptr<js::Persistent> resolve;
    std::shared_ptr<js::Persistent> reject;

    void settle(Realm::Internals& in, bool fulfil, js::Value const& value) const
    {
        js::Interpreter::Roots const roots(in.interpreter);
        in.interpreter.root(value);
        js::Value const arguments[] = { value };
        in.call_reporting((fulfil ? resolve : reject)->value(), js::Value::undefined(), arguments, "Encrypted Media");
    }
    void fulfil(Realm::Internals& in, js::Value const& value) const { settle(in, true, value); }
    void reject_dom(Realm::Internals& in, std::string_view name, std::string_view message) const
    {
        settle(in, false, dom_exception_value(in, name, message));
    }
    void reject_type(Realm::Internals& in, std::string_view message) const
    {
        settle(in, false, type_error_value(in.interpreter, message));
    }
};

// A pending promise of the realm's own and what settles it.
std::optional<std::pair<js::Value, Settler>> new_pending(js::Interpreter& interp)
{
    std::optional<js::PromiseCapability> const capability = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    interp.root(capability->promise);
    Settler settler { std::make_shared<js::Persistent>(interp.heap(), capability->resolve),
        std::make_shared<js::Persistent>(interp.heap(), capability->reject) };
    return std::pair { capability->promise, std::move(settler) };
}

// An operation of Promise type rejects where it would throw (WebIDL
// §3.7.7): its body's exception becomes the rejection.
template<typename F>
auto promising(F body)
{
    return [body](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return as_promise(interp, body(interp, this_value, args));
    };
}

std::shared_ptr<js::Persistent> hold(Realm::Internals& in, js::Object* object)
{
    return std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(object));
}

// --- Reading the configurations (WebIDL dictionaries) -------------------------------------------

// A dictionary member: undefined when the dictionary has none.
std::optional<js::Value> member(js::Interpreter& interp, js::Value const& dictionary, std::string_view name)
{
    if (!dictionary.is_object())
        return js::Value::undefined();
    return interp.get(dictionary, name);
}

bool read_string(Realm::Internals& in, js::Value const& dictionary, std::string_view name, std::string& into)
{
    std::optional<js::Value> const value = member(in.interpreter, dictionary, name);
    if (!value)
        return false;
    if (value->is_undefined())
        return true;
    std::optional<std::string> text = in.to_utf8(*value);
    if (!text)
        return false;
    into = std::move(*text);
    return true;
}

// A sequence member, each element read by `each`; absent stays as it is.
template<typename F>
bool read_sequence(Realm::Internals& in, js::Value const& dictionary, std::string_view name, bool& present, F&& each)
{
    js::Interpreter& interp = in.interpreter;
    std::optional<js::Value> const value = member(interp, dictionary, name);
    if (!value)
        return false;
    present = !value->is_undefined();
    if (!present)
        return true;
    if (!value->is_object()) {
        interp.throw_type_error("Failed to execute 'requestMediaKeySystemAccess' on 'Navigator': " + std::string(name) + " is not a sequence.");
        return false;
    }
    js::Interpreter::Roots const roots(interp);
    interp.root(*value);
    std::optional<std::vector<js::Value>> const items = interp.iterable_to_list(*value);
    if (!items)
        return false;
    for (js::Value const& item : *items)
        interp.root(item);
    for (js::Value const& item : *items) {
        if (!each(item))
            return false;
    }
    return true;
}

bool read_requirement(Realm::Internals& in, js::Value const& dictionary, std::string_view name, std::string& into)
{
    if (!read_string(in, dictionary, name, into))
        return false;
    if (into != "required" && into != "optional" && into != "not-allowed") {
        in.interpreter.throw_type_error("The provided value '" + into + "' is not a valid enum value of type MediaKeysRequirement.");
        return false;
    }
    return true;
}

bool read_capability(Realm::Internals& in, js::Value const& value, Capability& out)
{
    if (!value.is_undefined() && !value.is_null() && !value.is_object()) {
        in.interpreter.throw_type_error("A MediaKeySystemMediaCapability is not a dictionary.");
        return false;
    }
    // Members in WebIDL's order: contentType, encryptionScheme, robustness.
    if (!read_string(in, value, "contentType", out.content_type))
        return false;
    std::optional<js::Value> const scheme = member(in.interpreter, value, "encryptionScheme");
    if (!scheme)
        return false;
    if (!scheme->is_undefined() && !scheme->is_null()) {
        std::optional<std::string> text = in.to_utf8(*scheme);
        if (!text)
            return false;
        out.encryption_scheme = std::move(*text);
    }
    return read_string(in, value, "robustness", out.robustness);
}

bool read_configuration(Realm::Internals& in, js::Value const& value, Configuration& out)
{
    if (!value.is_undefined() && !value.is_null() && !value.is_object()) {
        in.interpreter.throw_type_error("A MediaKeySystemConfiguration is not a dictionary.");
        return false;
    }
    bool present = false;
    // Members in WebIDL's order: audioCapabilities, distinctiveIdentifier,
    // initDataTypes, label, persistentState, sessionTypes, videoCapabilities.
    if (!read_sequence(in, value, "audioCapabilities", present, [&](js::Value const& item) {
            Capability capability;
            if (!read_capability(in, item, capability))
                return false;
            out.audio.push_back(std::move(capability));
            return true;
        }))
        return false;
    if (!read_requirement(in, value, "distinctiveIdentifier", out.distinctive_identifier))
        return false;
    if (!read_sequence(in, value, "initDataTypes", present, [&](js::Value const& item) {
            std::optional<std::string> text = in.to_utf8(item);
            if (!text)
                return false;
            out.init_data_types.push_back(std::move(*text));
            return true;
        }))
        return false;
    if (!read_string(in, value, "label", out.label))
        return false;
    if (!read_requirement(in, value, "persistentState", out.persistent_state))
        return false;
    std::vector<std::string> session_types;
    if (!read_sequence(in, value, "sessionTypes", present, [&](js::Value const& item) {
            std::optional<std::string> text = in.to_utf8(item);
            if (!text)
                return false;
            session_types.push_back(std::move(*text));
            return true;
        }))
        return false;
    if (present)
        out.session_types = std::move(session_types);
    return read_sequence(in, value, "videoCapabilities", present, [&](js::Value const& item) {
        Capability capability;
        if (!read_capability(in, item, capability))
            return false;
        out.video.push_back(std::move(capability));
        return true;
    });
}

// --- Get Supported Configuration (EME §3.1.1.2) ---------------------------------------------------

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

std::string lowered(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// Whether a codec string names a codec of the kind asked for: its family
// as RFC 6381 and the WebM project write it, in their case (codec strings
// are case-sensitive, MIME types are not).
bool codec_of_kind(std::string_view codec, bool audio)
{
    std::string_view const family = codec.substr(0, codec.find('.'));
    static constexpr std::string_view video_families[] = { "avc1", "avc3", "vp09", "vp9", "vp8", "hev1", "hvc1", "av01" };
    static constexpr std::string_view audio_families[] = { "mp4a", "opus", "vorbis", "flac", "mp3" };
    std::span<std::string_view const> const families = audio ? std::span<std::string_view const>(audio_families) : std::span<std::string_view const>(video_families);
    for (std::string_view const known : families) {
        if (family == known)
            return true;
    }
    return false;
}

// Steps 3.1 to 3.11 of "Get Supported Capabilities for Audio/Video Type"
// for one capability: a MIME type of a container this engine plays, no
// parameter but codecs, every codec of the field's kind and one the
// engine decodes (asked of the same answer canPlayType and isTypeSupported
// give), an encryption scheme Clear Key decrypts, no robustness.
bool capability_supported(Capability const& capability, bool audio)
{
    std::string_view const type = capability.content_type;
    std::size_t const semicolon = type.find(';');
    std::string const essence = lowered(trimmed(type.substr(0, semicolon)));
    if (essence != "video/mp4" && essence != "audio/mp4" && essence != "video/webm" && essence != "audio/webm")
        return false;
    std::string_view rest = semicolon == std::string_view::npos ? std::string_view {} : type.substr(semicolon + 1);
    while (!rest.empty()) {
        std::size_t const next = rest.find(';');
        std::string_view const parameter = trimmed(rest.substr(0, next));
        rest = next == std::string_view::npos ? std::string_view {} : rest.substr(next + 1);
        std::size_t const equals = parameter.find('=');
        if (equals == std::string_view::npos || lowered(trimmed(parameter.substr(0, equals))) != "codecs")
            return false; // a parameter not recognized
        std::string_view list = trimmed(parameter.substr(equals + 1));
        if (list.size() >= 2 && list.front() == '"' && list.back() == '"')
            list = list.substr(1, list.size() - 2);
        if (trimmed(list).empty())
            return false;
        while (true) {
            std::size_t const comma = list.find(',');
            std::string_view const codec = trimmed(list.substr(0, comma));
            if (codec.empty() || !codec_of_kind(codec, audio))
                return false;
            if (comma == std::string_view::npos)
                break;
            list = list.substr(comma + 1);
        }
    }
    if (media_type_support(type) == 0)
        return false;
    if (capability.encryption_scheme) {
        std::string const& scheme = *capability.encryption_scheme;
        if (scheme != "cenc" && scheme != "cbcs" && scheme != "cbcs-1-9")
            return false;
    }
    return capability.robustness.empty();
}

std::optional<std::vector<Capability>> supported_capabilities(std::vector<Capability> const& requested, bool audio)
{
    std::vector<Capability> supported;
    for (Capability const& capability : requested) {
        // An empty type makes the whole configuration unsupported (step 3.3).
        if (capability.content_type.empty())
            return std::nullopt;
        if (capability_supported(capability, audio))
            supported.push_back(capability);
    }
    if (supported.empty())
        return std::nullopt;
    return supported;
}

std::optional<Configuration> supported_configuration(Configuration const& candidate)
{
    Configuration accumulated;
    accumulated.label = candidate.label;
    if (!candidate.init_data_types.empty()) {
        for (std::string const& type : candidate.init_data_types) {
            if (!type.empty() && media::init_data_type_supported(type))
                accumulated.init_data_types.push_back(type);
        }
        if (accumulated.init_data_types.empty())
            return std::nullopt;
    }
    // Clear Key identifies nothing (§9.1.1): a distinctive identifier
    // cannot be had, and none is used.
    if (candidate.distinctive_identifier == "required")
        return std::nullopt;
    accumulated.distinctive_identifier = "not-allowed";
    // Nor does it keep anything: temporary sessions only.
    if (candidate.persistent_state == "required")
        return std::nullopt;
    std::vector<std::string> const session_types = candidate.session_types.value_or(std::vector<std::string> { "temporary" });
    for (std::string const& type : session_types) {
        if (type != "temporary")
            return std::nullopt;
    }
    accumulated.session_types = session_types;
    accumulated.persistent_state = "not-allowed";
    if (candidate.video.empty() && candidate.audio.empty())
        return std::nullopt;
    if (!candidate.video.empty()) {
        std::optional<std::vector<Capability>> capabilities = supported_capabilities(candidate.video, false);
        if (!capabilities)
            return std::nullopt;
        accumulated.video = std::move(*capabilities);
    }
    if (!candidate.audio.empty()) {
        std::optional<std::vector<Capability>> capabilities = supported_capabilities(candidate.audio, true);
        if (!capabilities)
            return std::nullopt;
        accumulated.audio = std::move(*capabilities);
    }
    return accumulated;
}

bool secure_context(Realm::Internals& in)
{
    // As isSecureContext says it (Window.cpp).
    net::Url const& url = in.url;
    return url.scheme == "https" || url.scheme == "file" || url.scheme == "about" || url.host == "localhost";
}

// --- Events ----------------------------------------------------------------------------------

// MediaKeyMessageEvent and MediaEncryptedEvent keep their two members in
// EventObject::members: the type string, then the bytes.
EventObject* new_member_event(Realm::Internals& in, std::string_view interface, std::string_view type, js::Value const& first,
    js::Value const& second)
{
    EventObject* event = in.new_event(interface, type, false, false);
    event->members = { first, second };
    return event;
}

void queue_event(Realm::Internals& in, js::Object* target, std::string_view interface, std::string_view type, std::string first,
    std::optional<Bytes> second)
{
    auto held = hold(in, target);
    in.post_task([&in, held, interface = std::string(interface), type = std::string(type), first = std::move(first), second = std::move(second)] {
        Realm::Internals::Entry const entry(in);
        js::Interpreter& interp = in.interpreter;
        js::Interpreter::Roots const roots(interp);
        js::Value const bytes = second ? interp.root(array_buffer_of(interp, *second)) : js::Value::null();
        js::Value const text = interp.root(in.string(first));
        EventObject* event = interface.empty() ? in.new_event("Event", type, false, false) : new_member_event(in, interface, type, text, bytes);
        interp.root(js::Value::object(event));
        event->is_trusted = true;
        in.dispatch(*event, held->value().as_object());
    });
}

// --- Sessions ------------------------------------------------------------------------------

// "Update Key Statuses" (§6.6.3): the map becomes the new one, a
// keystatuseschange event is queued, and every media element using the
// session's MediaKeys tries to go on (§7.3.5).
void update_key_statuses(Realm::Internals& in, MediaKeySessionObject& session, std::vector<std::pair<Bytes, std::string>> statuses)
{
    std::sort(statuses.begin(), statuses.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    session.statuses->entries = std::move(statuses);
    queue_event(in, &session, {}, "keystatuseschange", {}, std::nullopt);
    if (session.keys != nullptr && session.keys->element_state != nullptr) {
        auto held = hold(in, session.keys->element_state);
        in.post_task([&in, held] {
            Realm::Internals::Entry const entry(in);
            media_keys_changed(in, held->value().as_object());
        });
    }
}

// "Session Closed" (§6.6.6): the keys go, the statuses empty, the closed
// promise resolves with the reason.
void session_closed(Realm::Internals& in, MediaKeySessionObject& session, std::string_view reason)
{
    if (session.closed_resolve.is_undefined())
        return; // closed already
    session.closing_or_closed = true;
    if (session.store)
        session.store->remove_session(session.number);
    if (session.keys != nullptr)
        std::erase(session.keys->open_sessions, static_cast<js::Object*>(&session));
    update_key_statuses(in, session, {});
    session.expiration = std::nan("");
    js::Interpreter::Roots const roots(in.interpreter);
    js::Value const resolve = in.interpreter.root(std::exchange(session.closed_resolve, js::Value::undefined()));
    js::Value const arguments[] = { in.interpreter.root(in.string(reason)) };
    in.call_reporting(resolve, js::Value::undefined(), arguments, "MediaKeySession.closed");
}

MediaKeySessionObject* new_session(Realm::Internals& in, MediaKeysObject& keys, std::string type)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    auto* session = interp.heap().allocate<MediaKeySessionObject>(in.prototype("MediaKeySession"));
    interp.root(js::Value::object(session));
    static std::atomic<std::uint64_t> next_number { 1 };
    session->number = next_number++;
    session->keys = &keys;
    session->store = keys.store;
    session->session_type = std::move(type);
    session->statuses = interp.heap().allocate<MediaKeyStatusMapObject>(in.prototype("MediaKeyStatusMap"));
    std::optional<js::PromiseCapability> const closed = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
    if (closed) {
        session->closed_promise = closed->promise;
        session->closed_resolve = closed->resolve;
    }
    return session;
}

Native generate_request(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interp);
    MediaKeySessionObject* const self = this_or_throw<MediaKeySessionObject>(interp, this_value);
    if (self == nullptr)
        return std::nullopt;
    std::optional<std::string> const type = in.to_utf8(js::argument(args, 0));
    if (!type)
        return std::nullopt;
    std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(js::argument(args, 1));
    if (!bytes)
        return interp.throw_type_error("Failed to execute 'generateRequest' on 'MediaKeySession': parameter 2 is not of type 'BufferSource'.");
    if (self->closing_or_closed)
        return in.throw_dom_exception("InvalidStateError", "The session is already closed.");
    if (!self->uninitialized)
        return in.throw_dom_exception("InvalidStateError", "The session is already initialized.");
    self->uninitialized = false;
    if (type->empty())
        return interp.throw_type_error("Failed to execute 'generateRequest' on 'MediaKeySession': The initDataType parameter is empty.");
    if (bytes->empty())
        return interp.throw_type_error("Failed to execute 'generateRequest' on 'MediaKeySession': The initData parameter is empty.");
    if (!media::init_data_type_supported(*type))
        return in.throw_dom_exception("NotSupportedError", "The initialization data type '" + *type + "' is not supported.");
    Bytes init_data(bytes->begin(), bytes->end());
    js::Interpreter::Roots const roots(interp);
    std::optional<std::pair<js::Value, Settler>> pending = new_pending(interp);
    if (!pending)
        return std::nullopt;
    // What the module does in parallel (§6.6.2 step 10), and the task that
    // follows it.
    auto held = hold(in, self);
    in.post_task([&in, held, settler = pending->second, type = *type, init_data = std::move(init_data)] {
        Realm::Internals::Entry const entry(in);
        auto& session = *static_cast<MediaKeySessionObject*>(held->value().as_object());
        std::optional<std::vector<Bytes>> const ids = media::key_ids_from_init_data(type, init_data);
        if (!ids) {
            settler.reject_type(in, "Failed to execute 'generateRequest' on 'MediaKeySession': The initialization data is not valid for its type.");
            return;
        }
        if (ids->empty()) {
            settler.reject_dom(in, "NotSupportedError", "The initialization data names no key ID Clear Key can use.");
            return;
        }
        if (session.closing_or_closed) {
            settler.reject_dom(in, "InvalidStateError", "The session is closed.");
            return;
        }
        static std::atomic<std::uint64_t> next_id { 1 };
        session.session_id = std::to_string(next_id++);
        session.callable = true;
        // From here on the session may hold keys: it lives while it is
        // open and its MediaKeys can be reached, whoever else holds it.
        if (session.keys != nullptr)
            session.keys->open_sessions.push_back(&session);
        session.requested = *ids;
        std::string const request = media::license_request(*ids, session.session_type);
        queue_event(in, &session, "MediaKeyMessageEvent", "message", "license-request", Bytes(request.begin(), request.end()));
        settler.fulfil(in, js::Value::undefined());
    });
    return pending->first;
}

Native update_session(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interp);
    MediaKeySessionObject* const self = this_or_throw<MediaKeySessionObject>(interp, this_value);
    if (self == nullptr)
        return std::nullopt;
    std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(js::argument(args, 0));
    if (!bytes)
        return interp.throw_type_error("Failed to execute 'update' on 'MediaKeySession': parameter 1 is not of type 'BufferSource'.");
    if (self->closing_or_closed)
        return in.throw_dom_exception("InvalidStateError", "The session is already closed.");
    if (!self->callable)
        return in.throw_dom_exception("InvalidStateError", "The session is not yet initialized.");
    if (bytes->empty())
        return interp.throw_type_error("Failed to execute 'update' on 'MediaKeySession': The response parameter is empty.");
    Bytes response(bytes->begin(), bytes->end());
    js::Interpreter::Roots const roots(interp);
    std::optional<std::pair<js::Value, Settler>> pending = new_pending(interp);
    if (!pending)
        return std::nullopt;
    auto held = hold(in, self);
    in.post_task([&in, held, settler = pending->second, response = std::move(response)] {
        Realm::Internals::Entry const entry(in);
        auto& session = *static_cast<MediaKeySessionObject*>(held->value().as_object());
        std::optional<media::License> const license = media::parse_license(response);
        if (!license) {
            settler.reject_type(in, "Failed to execute 'update' on 'MediaKeySession': The response is not a valid Clear Key license.");
            return;
        }
        if (session.closing_or_closed) {
            settler.reject_dom(in, "InvalidStateError", "The session is closed.");
            return;
        }
        // Every key the license carries is usable from now on, with those
        // the session had before.
        std::vector<std::pair<Bytes, std::string>> statuses = session.statuses->entries;
        for (auto const& [id, key] : license->keys) {
            session.store->add(session.number, id, key);
            auto const known = std::find_if(statuses.begin(), statuses.end(), [&id = id](auto const& status) { return status.first == id; });
            if (known != statuses.end())
                known->second = "usable";
            else
                statuses.emplace_back(id, "usable");
        }
        update_key_statuses(in, session, std::move(statuses));
        settler.fulfil(in, js::Value::undefined());
    });
    return pending->first;
}

Native close_session(js::Interpreter& interp, js::Value const& this_value, Args)
{
    Realm::Internals& in = internals_of(interp);
    MediaKeySessionObject* const session = this_or_throw<MediaKeySessionObject>(interp, this_value);
    if (session == nullptr)
        return std::nullopt;
    if (session->closing_or_closed)
        return resolved_promise(interp, js::Value::undefined());
    if (!session->callable)
        return in.throw_dom_exception("InvalidStateError", "The session is not yet initialized.");
    session->closing_or_closed = true;
    js::Interpreter::Roots const roots(interp);
    std::optional<std::pair<js::Value, Settler>> pending = new_pending(interp);
    if (!pending)
        return std::nullopt;
    auto held = hold(in, session);
    in.post_task([&in, held, settler = pending->second] {
        Realm::Internals::Entry const entry(in);
        session_closed(in, *static_cast<MediaKeySessionObject*>(held->value().as_object()), "closed-by-application");
        settler.fulfil(in, js::Value::undefined());
    });
    return pending->first;
}

Native remove_session(js::Interpreter& interp, js::Value const& this_value, Args)
{
    Realm::Internals& in = internals_of(interp);
    MediaKeySessionObject* const self = this_or_throw<MediaKeySessionObject>(interp, this_value);
    if (self == nullptr)
        return std::nullopt;
    if (self->closing_or_closed)
        return in.throw_dom_exception("InvalidStateError", "The session is already closed.");
    if (!self->callable)
        return in.throw_dom_exception("InvalidStateError", "The session is not yet initialized.");
    js::Interpreter::Roots const roots(interp);
    std::optional<std::pair<js::Value, Settler>> pending = new_pending(interp);
    if (!pending)
        return std::nullopt;
    auto held = hold(in, self);
    in.post_task([&in, held, settler = pending->second] {
        Realm::Internals::Entry const entry(in);
        auto& session = *static_cast<MediaKeySessionObject*>(held->value().as_object());
        // §6.6.5: the keys are let go and each says so ("released"); a
        // temporary session sends no release message.
        session.store->remove_session(session.number);
        std::vector<std::pair<Bytes, std::string>> statuses = session.statuses->entries;
        for (auto& entry_status : statuses)
            entry_status.second = "released";
        update_key_statuses(in, session, std::move(statuses));
        session.expiration = std::nan("");
        settler.fulfil(in, js::Value::undefined());
    });
    return pending->first;
}

// --- Installing --------------------------------------------------------------------------------

js::Value configuration_value(Realm::Internals& in, Configuration const& configuration)
{
    js::Interpreter& interp = in.interpreter;
    js::Object* out = interp.new_object();
    interp.root(js::Value::object(out));
    auto const strings = [&](std::vector<std::string> const& list) {
        std::vector<js::Value> items;
        for (std::string const& item : list)
            items.push_back(interp.root(in.string(item)));
        return interp.root(js::Value::object(interp.new_array(items)));
    };
    auto const capabilities = [&](std::vector<Capability> const& list) {
        std::vector<js::Value> items;
        for (Capability const& capability : list) {
            js::Object* entry = interp.new_object();
            interp.root(js::Value::object(entry));
            entry->put(interp.key("contentType"), interp.root(in.string(capability.content_type)), js::Enumerable);
            entry->put(interp.key("encryptionScheme"),
                capability.encryption_scheme ? interp.root(in.string(*capability.encryption_scheme)) : js::Value::null(), js::Enumerable);
            entry->put(interp.key("robustness"), interp.root(in.string(capability.robustness)), js::Enumerable);
            items.push_back(js::Value::object(entry));
        }
        return interp.root(js::Value::object(interp.new_array(items)));
    };
    out->put(interp.key("audioCapabilities"), capabilities(configuration.audio), js::Enumerable);
    out->put(interp.key("distinctiveIdentifier"), interp.root(in.string(configuration.distinctive_identifier)), js::Enumerable);
    out->put(interp.key("initDataTypes"), strings(configuration.init_data_types), js::Enumerable);
    out->put(interp.key("label"), interp.root(in.string(configuration.label)), js::Enumerable);
    out->put(interp.key("persistentState"), interp.root(in.string(configuration.persistent_state)), js::Enumerable);
    out->put(interp.key("sessionTypes"), strings(configuration.session_types.value_or(std::vector<std::string> {})), js::Enumerable);
    out->put(interp.key("videoCapabilities"), capabilities(configuration.video), js::Enumerable);
    return js::Value::object(out);
}

void install_access(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* proto = define_interface(in, "MediaKeySystemAccess", nullptr);
    define_getter(in, *proto, "keySystem", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const access = this_or_throw<MediaKeySystemAccessObject>(interp, this_value);
        if (access == nullptr)
            return std::nullopt;
        return internals_of(interp).string(access->key_system);
    });
    define_operation(interpreter, *proto, "getConfiguration", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const access = this_or_throw<MediaKeySystemAccessObject>(interp, this_value);
        if (access == nullptr)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        return configuration_value(internals_of(interp), access->configuration);
    });
    define_promise_operation(interpreter, *proto, "createMediaKeys", 0, promising([](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const access = this_or_throw<MediaKeySystemAccessObject>(interp, this_value);
        if (access == nullptr)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        auto* keys = interp.heap().allocate<MediaKeysObject>(internals_of(interp).prototype("MediaKeys"));
        interp.root(js::Value::object(keys));
        keys->session_types = access->configuration.session_types.value_or(std::vector<std::string> { "temporary" });
        return resolved_promise(interp, js::Value::object(keys));
    }));
}

void install_media_keys(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* proto = define_interface(in, "MediaKeys", nullptr);
    define_operation(interpreter, *proto, "createSession", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        auto* const keys = this_or_throw<MediaKeysObject>(interp, this_value);
        if (keys == nullptr)
            return std::nullopt;
        std::string type = "temporary";
        if (!js::argument(args, 0).is_undefined()) {
            std::optional<std::string> given = internals.to_utf8(args[0]);
            if (!given)
                return std::nullopt;
            if (*given != "temporary" && *given != "persistent-license")
                return interp.throw_type_error("Failed to execute 'createSession' on 'MediaKeys': The provided value '" + *given
                    + "' is not a valid enum value of type MediaKeySessionType.");
            type = std::move(*given);
        }
        if (std::find(keys->session_types.begin(), keys->session_types.end(), type) == keys->session_types.end())
            return internals.throw_dom_exception("NotSupportedError", "The session type '" + type + "' is not supported.");
        return js::Value::object(new_session(internals, *keys, std::move(type)));
    });
    define_promise_operation(interpreter, *proto, "setServerCertificate", 1, promising([](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* const keys = this_or_throw<MediaKeysObject>(interp, this_value);
        if (keys == nullptr)
            return std::nullopt;
        std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(js::argument(args, 0));
        if (!bytes)
            return interp.throw_type_error("Failed to execute 'setServerCertificate' on 'MediaKeys': parameter 1 is not of type 'BufferSource'.");
        if (bytes->empty())
            return interp.throw_type_error("Failed to execute 'setServerCertificate' on 'MediaKeys': The serverCertificate parameter is empty.");
        // Clear Key has no server certificates (§9.1.1): false, as the
        // algorithm answers for a key system without them.
        return resolved_promise(interp, js::Value::boolean(false));
    }));
    // The HDCP Policy Check: nothing Clear Key decrypts is held back from
    // an output, so every policy is "usable".
    define_promise_operation(interpreter, *proto, "getStatusForPolicy", 0, promising([](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        auto* const keys = this_or_throw<MediaKeysObject>(interp, this_value);
        if (keys == nullptr)
            return std::nullopt;
        js::Value const policy = js::argument(args, 0);
        if (!policy.is_undefined() && !policy.is_null() && !policy.is_object())
            return interp.throw_type_error("Failed to execute 'getStatusForPolicy' on 'MediaKeys': parameter 1 is not a dictionary.");
        std::string version;
        if (!read_string(internals, policy, "minHdcpVersion", version))
            return std::nullopt;
        return resolved_promise(interp, internals.string("usable"));
    }));
}

void install_session(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* proto = define_interface(in, "MediaKeySession", in.prototype("EventTarget"));
    define_getter(in, *proto, "sessionId", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const session = this_or_throw<MediaKeySessionObject>(interp, this_value);
        if (session == nullptr)
            return std::nullopt;
        return internals_of(interp).string(session->session_id);
    });
    define_getter(in, *proto, "expiration", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const session = this_or_throw<MediaKeySessionObject>(interp, this_value);
        if (session == nullptr)
            return std::nullopt;
        return js::Value::number(session->expiration);
    });
    define_promise_getter(in, *proto, "closed", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const session = this_or_throw<MediaKeySessionObject>(interp, this_value);
        if (session == nullptr)
            return std::nullopt;
        return session->closed_promise;
    });
    define_getter(in, *proto, "keyStatuses", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const session = this_or_throw<MediaKeySessionObject>(interp, this_value);
        if (session == nullptr)
            return std::nullopt;
        return js::Value::object(session->statuses);
    });
    define_promise_operation(interpreter, *proto, "generateRequest", 2, promising(generate_request));
    define_promise_operation(interpreter, *proto, "load", 1, promising([](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        auto* const session = this_or_throw<MediaKeySessionObject>(interp, this_value);
        if (session == nullptr)
            return std::nullopt;
        std::optional<std::string> const id = internals.to_utf8(js::argument(args, 0));
        if (!id)
            return std::nullopt;
        if (session->closing_or_closed)
            return internals.throw_dom_exception("InvalidStateError", "The session is already closed.");
        if (!session->uninitialized)
            return internals.throw_dom_exception("InvalidStateError", "The session is already initialized.");
        session->uninitialized = false;
        if (id->empty())
            return interp.throw_type_error("Failed to execute 'load' on 'MediaKeySession': The sessionId parameter is empty.");
        // §6.6.2 step 5: only a session that is kept can be loaded, and
        // Clear Key keeps none.
        return interp.throw_type_error("Failed to execute 'load' on 'MediaKeySession': The session type is not persistent.");
    }));
    define_promise_operation(interpreter, *proto, "update", 1, promising(update_session));
    define_promise_operation(interpreter, *proto, "close", 0, promising(close_session));
    define_promise_operation(interpreter, *proto, "remove", 0, promising(remove_session));
    static constexpr std::string_view session_events[] = { "keystatuseschange", "message" };
    define_event_handlers(in, *proto, session_events);
}

void install_status_map(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* proto = define_interface(in, "MediaKeyStatusMap", nullptr);
    define_getter(in, *proto, "size", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const map = this_or_throw<MediaKeyStatusMapObject>(interp, this_value);
        if (map == nullptr)
            return std::nullopt;
        return js::Value::number(static_cast<double>(map->entries.size()));
    });
    auto const find = [](MediaKeyStatusMapObject const& map, js::Value const& key) -> std::pair<bool, std::string> {
        std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(key);
        if (!bytes)
            return { false, {} };
        for (auto const& [id, status] : map.entries) {
            if (std::equal(id.begin(), id.end(), bytes->begin(), bytes->end()))
                return { true, status };
        }
        return { true, {} };
    };
    define_operation(interpreter, *proto, "has", 1, [find](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* const map = this_or_throw<MediaKeyStatusMapObject>(interp, this_value);
        if (map == nullptr)
            return std::nullopt;
        auto const [valid, status] = find(*map, js::argument(args, 0));
        if (!valid)
            return interp.throw_type_error("Failed to execute 'has' on 'MediaKeyStatusMap': parameter 1 is not of type 'BufferSource'.");
        return js::Value::boolean(!status.empty());
    });
    define_operation(interpreter, *proto, "get", 1, [find](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* const map = this_or_throw<MediaKeyStatusMapObject>(interp, this_value);
        if (map == nullptr)
            return std::nullopt;
        auto const [valid, status] = find(*map, js::argument(args, 0));
        if (!valid)
            return interp.throw_type_error("Failed to execute 'get' on 'MediaKeyStatusMap': parameter 1 is not of type 'BufferSource'.");
        return status.empty() ? js::Value::undefined() : internals_of(interp).string(status);
    });
    define_operation(interpreter, *proto, "forEach", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* const map = this_or_throw<MediaKeyStatusMapObject>(interp, this_value);
        if (map == nullptr)
            return std::nullopt;
        js::Value const callback = js::argument(args, 0);
        if (!interp.is_callable(callback))
            return interp.throw_type_error("Failed to execute 'forEach' on 'MediaKeyStatusMap': The callback provided as parameter 1 is not a function.");
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        interp.root(callback);
        js::Value const this_argument = interp.root(js::argument(args, 1));
        std::vector<std::pair<Bytes, std::string>> const entries = map->entries;
        for (auto const& [id, status] : entries) {
            js::Interpreter::Roots const each(interp);
            js::Value const arguments[3] = { interp.root(internals_of(interp).string(status)), interp.root(array_buffer_of(interp, id)), this_value };
            if (!interp.call(callback, this_argument, arguments))
                return std::nullopt;
        }
        return js::Value::undefined();
    });
    for (auto const& [name, kind] : { std::pair { "entries", 0 }, std::pair { "keys", 1 }, std::pair { "values", 2 } }) {
        int const which = kind;
        js::NativeFunction* method = define_operation(interpreter, *proto, name, 0, [which](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            auto* const map = this_or_throw<MediaKeyStatusMapObject>(interp, this_value);
            if (map == nullptr)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            std::vector<js::Value> items;
            for (auto const& [id, status] : map->entries) {
                js::Value const key = interp.root(array_buffer_of(interp, id));
                js::Value const value = interp.root(internals.string(status));
                if (which == 1) {
                    items.push_back(key);
                } else if (which == 2) {
                    items.push_back(value);
                } else {
                    js::Value const pair[2] = { key, value };
                    items.push_back(interp.root(js::Value::object(interp.new_array(pair))));
                }
            }
            return iterator_of_values(interp, items);
        });
        if (which == 0)
            proto->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), js::Value::object(method), js::builtin_attributes);
    }
}

// The two events' constructors (§6.4, §7.1): their init dictionaries'
// members in WebIDL's order.
void install_eme_events(Realm::Internals& realm)
{
    js::Object* message = define_interface(realm, "MediaKeyMessageEvent", realm.prototype("Event"),
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& in = internals_of(interp);
            if (args.size() < 2)
                return interp.throw_type_error("Failed to construct 'MediaKeyMessageEvent': 2 arguments required.");
            std::optional<std::string> const type = in.to_utf8(args[0]);
            if (!type)
                return std::nullopt;
            js::Value const init = args[1];
            if (!init.is_object())
                return interp.throw_type_error("Failed to construct 'MediaKeyMessageEvent': parameter 2 is not a dictionary.");
            js::Interpreter::Roots const roots(interp);
            interp.root(init);
            bool flags[3] = {};
            static constexpr std::string_view flag_names[] = { "bubbles", "cancelable", "composed" };
            for (std::size_t i = 0; i < 3; ++i) {
                std::optional<js::Value> const value = interp.get(init, flag_names[i]);
                if (!value)
                    return std::nullopt;
                flags[i] = js::Interpreter::to_boolean(*value);
            }
            std::optional<js::Value> const bytes = interp.get(init, "message");
            if (!bytes)
                return std::nullopt;
            if (bytes->is_undefined())
                return interp.throw_type_error("Failed to construct 'MediaKeyMessageEvent': required member message is undefined.");
            if (!object_as<js::ArrayBufferObject>(*bytes))
                return interp.throw_type_error("Failed to construct 'MediaKeyMessageEvent': member message is not of type ArrayBuffer.");
            interp.root(*bytes);
            std::optional<js::Value> const kind = interp.get(init, "messageType");
            if (!kind)
                return std::nullopt;
            if (kind->is_undefined())
                return interp.throw_type_error("Failed to construct 'MediaKeyMessageEvent': required member messageType is undefined.");
            std::optional<std::string> const kind_text = in.to_utf8(*kind);
            if (!kind_text)
                return std::nullopt;
            if (*kind_text != "license-request" && *kind_text != "license-renewal" && *kind_text != "license-release"
                && *kind_text != "individualization-request")
                return interp.throw_type_error("Failed to construct 'MediaKeyMessageEvent': The provided value '" + *kind_text
                    + "' is not a valid enum value of type MediaKeyMessageType.");
            EventObject* event = new_member_event(in, "MediaKeyMessageEvent", *type, interp.root(in.string(*kind_text)), *bytes);
            event->bubbles = flags[0];
            event->cancelable = flags[1];
            event->composed = flags[2];
            return js::Value::object(event);
        },
        2);
    js::Object* encrypted = define_interface(realm, "MediaEncryptedEvent", realm.prototype("Event"),
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& in = internals_of(interp);
            if (args.empty())
                return interp.throw_type_error("Failed to construct 'MediaEncryptedEvent': 1 argument required.");
            std::optional<std::string> const type = in.to_utf8(args[0]);
            if (!type)
                return std::nullopt;
            js::Value const init = js::argument(args, 1);
            if (!init.is_undefined() && !init.is_null() && !init.is_object())
                return interp.throw_type_error("Failed to construct 'MediaEncryptedEvent': parameter 2 is not a dictionary.");
            js::Interpreter::Roots const roots(interp);
            interp.root(init);
            bool flags[3] = {};
            static constexpr std::string_view flag_names[] = { "bubbles", "cancelable", "composed" };
            for (std::size_t i = 0; i < 3; ++i) {
                std::optional<js::Value> const value = member(interp, init, flag_names[i]);
                if (!value)
                    return std::nullopt;
                flags[i] = js::Interpreter::to_boolean(*value);
            }
            std::optional<js::Value> bytes = member(interp, init, "initData");
            if (!bytes)
                return std::nullopt;
            if (bytes->is_undefined())
                bytes = js::Value::null();
            if (!bytes->is_null() && !object_as<js::ArrayBufferObject>(*bytes))
                return interp.throw_type_error("Failed to construct 'MediaEncryptedEvent': member initData is not of type ArrayBuffer.");
            interp.root(*bytes);
            std::string kind;
            if (!read_string(in, init, "initDataType", kind))
                return std::nullopt;
            EventObject* event = new_member_event(in, "MediaEncryptedEvent", *type, interp.root(in.string(kind)), *bytes);
            event->bubbles = flags[0];
            event->cancelable = flags[1];
            event->composed = flags[2];
            return js::Value::object(event);
        },
        1);
    auto const member_getter = [&](js::Object& proto, std::string_view name, std::size_t slot) {
        define_getter(realm, proto, name, [slot](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            auto* const event = object_as<EventObject>(this_value);
            if (event == nullptr || event->members.size() != 2)
                return interp.throw_type_error("Illegal invocation");
            return event->members[slot];
        });
    };
    member_getter(*message, "messageType", 0);
    member_getter(*message, "message", 1);
    member_getter(*encrypted, "initDataType", 0);
    member_getter(*encrypted, "initData", 1);
}

}

void install_eme(Realm::Internals& in)
{
    install_access(in);
    install_media_keys(in);
    install_session(in);
    install_status_map(in);
    install_eme_events(in);
}

Native request_media_key_system_access(js::Interpreter& interp, Args args)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<std::string> const key_system = in.to_utf8(js::argument(args, 0));
    if (!key_system)
        return std::nullopt;
    js::Interpreter::Roots const roots(interp);
    js::Value const list_value = interp.root(js::argument(args, 1));
    if (!list_value.is_object())
        return interp.throw_type_error("Failed to execute 'requestMediaKeySystemAccess' on 'Navigator': The provided value cannot be converted to a sequence.");
    std::optional<std::vector<js::Value>> const list = interp.iterable_to_list(list_value);
    if (!list)
        return std::nullopt;
    for (js::Value const& item : *list)
        interp.root(item);
    std::vector<Configuration> configurations;
    for (js::Value const& item : *list) {
        Configuration configuration;
        if (!read_configuration(in, item, configuration))
            return std::nullopt;
        configurations.push_back(std::move(configuration));
    }
    // §3.1.1.1 steps 1 and 2.
    if (key_system->empty())
        return interp.throw_type_error("Failed to execute 'requestMediaKeySystemAccess' on 'Navigator': The keySystem parameter is empty.");
    if (configurations.empty())
        return interp.throw_type_error("Failed to execute 'requestMediaKeySystemAccess' on 'Navigator': The supportedConfigurations parameter is empty.");
    // The interface is a secure context's alone ([SecureContext]).
    if (!secure_context(in))
        return in.throw_dom_exception("NotSupportedError", "Encrypted Media is available only in secure contexts.");
    std::optional<std::pair<js::Value, Settler>> pending = new_pending(interp);
    if (!pending)
        return std::nullopt;
    // A document of an opaque origin (a sandbox without allow-same-origin)
    // has no origin a key system's state could belong to: as the shipping
    // engines answer it, nothing is supported there.
    bool const opaque = document_origin(in).opaque();
    // Steps 6 and 7, in parallel and then in a task: the first
    // configuration the key system supports.
    in.post_task([&in, opaque, settler = pending->second, key_system = *key_system, configurations = std::move(configurations)] {
        Realm::Internals::Entry const entry(in);
        if (key_system != clear_key || opaque) {
            settler.reject_dom(in, "NotSupportedError", "Unsupported keySystem or supportedConfigurations.");
            return;
        }
        for (Configuration const& candidate : configurations) {
            std::optional<Configuration> supported = supported_configuration(candidate);
            if (!supported)
                continue;
            js::Interpreter& inner = in.interpreter;
            js::Interpreter::Roots const task_roots(inner);
            auto* access = inner.heap().allocate<MediaKeySystemAccessObject>(in.prototype("MediaKeySystemAccess"));
            inner.root(js::Value::object(access));
            access->key_system = key_system;
            access->configuration = std::move(*supported);
            settler.fulfil(in, js::Value::object(access));
            return;
        }
        settler.reject_dom(in, "NotSupportedError", "Unsupported keySystem or supportedConfigurations.");
    });
    return pending->first;
}

Native as_promise(js::Interpreter& interp, Native result)
{
    if (result || !interp.has_exception())
        return result;
    return rejected_promise(interp, interp.take_exception());
}

std::shared_ptr<media::KeyStore const> media_keys_store(js::Value const& value)
{
    auto* const keys = object_as<MediaKeysObject>(value);
    return keys ? keys->store : nullptr;
}

bool claim_media_keys(js::Value const& media_keys, js::Object* element_state)
{
    auto* const keys = object_as<MediaKeysObject>(media_keys);
    if (keys == nullptr)
        return false;
    if (keys->element_state != nullptr && keys->element_state != element_state)
        return false;
    keys->element_state = element_state;
    return true;
}

void release_media_keys(js::Value const& media_keys, js::Object* element_state)
{
    auto* const keys = object_as<MediaKeysObject>(media_keys);
    if (keys != nullptr && keys->element_state == element_state)
        keys->element_state = nullptr;
}

}
