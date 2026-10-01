#include "bindings/Internal.h"

// The objects hanging off navigator that answer for themselves, each the
// standard's shape, each the same object every time it is read
// ([SameObject]), and each answering what an engine with no device, no
// permission granted and no system integration answers in a browser:
// mediaCapabilities (Media Capabilities), mediaSession and MediaMetadata
// (Media Session — kept, shown nowhere yet), storage (Storage Standard),
// permissions (Permissions), geolocation (Geolocation, refused), mediaDevices
// (Media Capture and Streams, no devices), wakeLock (Screen Wake Lock,
// refused), credentials (Credential Management, nothing stored); and the
// operations getGamepads (none), share and canShare (no share target),
// setAppBadge and clearAppBadge (no badge to show), and
// requestMediaKeySystemAccess (Encrypted Media, no key system).

#include "js/Runtime.h"
#include "net/Url.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// The same object each read: a plain platform object of `interface`.
void define_same_object(Realm::Internals& in, js::Object& navigator, std::string_view name, std::string_view interface)
{
    define_getter(in, navigator, name, [name, interface](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(same_object(internals, "navigator." + std::string(name), [&] {
            return internals.interpreter.heap().allocate<PlainPlatformObject>(internals.prototype(interface), internals.realm_record);
        }));
    });
}

Native reject_with(Realm::Internals& in, std::string_view name, std::string_view message)
{
    return rejected_promise(in.interpreter, dom_exception_value(in, name, message));
}

// A TypeError a promise-returning operation makes is a rejection (WebIDL
// §3.7.7: exceptions thrown in the steps of a promise-returning
// operation become rejections).
Native reject_type_error(Realm::Internals& in, std::string_view message)
{
    in.interpreter.throw_type_error(message);
    return rejected_promise(in.interpreter, in.interpreter.take_exception());
}

// A dictionary member: undefined when the dictionary has none.
std::optional<js::Value> member(js::Interpreter& interp, js::Value const& dictionary, std::string_view name)
{
    if (!dictionary.is_object())
        return js::Value::undefined();
    return interp.get(dictionary, name);
}

// --- Media Capabilities ---------------------------------------------------------------------

// A MIME type as the standard reads one for a configuration (Media
// Capabilities §2.1.3): type/subtype, with no parameter but codecs.
bool is_media_configuration_type(std::string_view text)
{
    std::size_t const slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0)
        return false;
    std::size_t const semicolon = text.find(';');
    std::string_view const subtype = text.substr(slash + 1, semicolon == std::string_view::npos ? std::string_view::npos : semicolon - slash - 1);
    if (subtype.empty())
        return false;
    auto const token = [](std::string_view part) {
        for (char const c : part) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '+' || c == '_'))
                return false;
        }
        return !part.empty();
    };
    if (!token(text.substr(0, slash)) || !token(subtype))
        return false;
    std::string_view rest = semicolon == std::string_view::npos ? std::string_view {} : text.substr(semicolon + 1);
    while (!rest.empty()) {
        std::size_t const next = rest.find(';');
        std::string_view parameter = rest.substr(0, next);
        rest = next == std::string_view::npos ? std::string_view {} : rest.substr(next + 1);
        while (!parameter.empty() && (parameter.front() == ' ' || parameter.front() == '\t'))
            parameter.remove_prefix(1);
        std::size_t const equals = parameter.find('=');
        if (equals == std::string_view::npos)
            return false;
        std::string const name = ascii_lower(parameter.substr(0, equals));
        if (name != "codecs")
            return false;
    }
    return true;
}

// Reads one of the two configurations: the contentType, and for a video
// one the width, height, bitrate and framerate the dictionary must have.
// False with a TypeError thrown when it is not valid; nullopt with the
// type's support otherwise.
std::optional<bool> read_media_configuration(Realm::Internals& in, js::Value const& configuration, bool video, std::string_view operation)
{
    js::Interpreter& interp = in.interpreter;
    std::string const where = "Failed to execute '" + std::string(operation) + "' on 'MediaCapabilities': ";
    if (!configuration.is_object()) {
        interp.throw_type_error(where + (video ? "The video configuration" : "The audio configuration") + " is not a dictionary.");
        return std::nullopt;
    }
    std::optional<js::Value> const type_value = interp.get(configuration, "contentType");
    if (!type_value)
        return std::nullopt;
    if (type_value->is_undefined()) {
        interp.throw_type_error(where + "Failed to read the 'contentType' property: Required member is undefined.");
        return std::nullopt;
    }
    std::optional<std::string> const content_type = in.to_utf8(*type_value);
    if (!content_type)
        return std::nullopt;
    if (!is_media_configuration_type(*content_type)) {
        interp.throw_type_error(where + "The contentType '" + *content_type + "' is not a valid MIME type with at most a codecs parameter.");
        return std::nullopt;
    }
    if (video) {
        for (std::string_view const name : { "width", "height", "bitrate", "framerate" }) {
            std::optional<js::Value> const value = interp.get(configuration, name);
            if (!value)
                return std::nullopt;
            if (value->is_undefined()) {
                interp.throw_type_error(where + "Failed to read the '" + std::string(name) + "' property from 'VideoConfiguration': Required member is undefined.");
                return std::nullopt;
            }
            std::optional<double> const number = interp.to_number(*value);
            if (!number)
                return std::nullopt;
            if (!(*number > 0) || !std::isfinite(*number)) {
                interp.throw_type_error(where + "The " + std::string(name) + " must be greater than zero.");
                return std::nullopt;
            }
        }
    }
    return media_type_support(*content_type) > 0;
}

Native media_capabilities_info(js::Interpreter& interp, Args args, bool decoding)
{
    Realm::Internals& in = internals_of(interp);
    std::string_view const operation = decoding ? "decodingInfo" : "encodingInfo";
    js::Interpreter::Roots const roots(interp);
    js::Value const configuration = js::argument(args, 0);
    interp.root(configuration);
    std::string const where = "Failed to execute '" + std::string(operation) + "' on 'MediaCapabilities': ";
    if (!configuration.is_object())
        return reject_type_error(in, where + "parameter 1 is not of type '" + (decoding ? "MediaDecodingConfiguration" : "MediaEncodingConfiguration") + "'.");
    std::optional<js::Value> const type_value = interp.get(configuration, "type");
    if (!type_value)
        return std::nullopt;
    if (type_value->is_undefined())
        return reject_type_error(in, where + "Failed to read the 'type' property: Required member is undefined.");
    std::optional<std::string> const type = in.to_utf8(*type_value);
    if (!type)
        return std::nullopt;
    bool const known = decoding ? (*type == "file" || *type == "media-source" || *type == "webrtc") : (*type == "record" || *type == "webrtc");
    if (!known)
        return reject_type_error(in, where + "The provided value '" + *type + "' is not a valid enum value of type " + (decoding ? "MediaDecodingType" : "MediaEncodingType") + ".");
    std::optional<js::Value> const audio = interp.get(configuration, "audio");
    std::optional<js::Value> const video = interp.get(configuration, "video");
    if (!audio || !video)
        return std::nullopt;
    if (audio->is_undefined() && video->is_undefined())
        return reject_type_error(in, where + "The configuration dictionary has neither |video| nor |audio| specified and needs at least one of them.");
    bool supported = *type != "webrtc" && decoding; // nothing here records or speaks WebRTC
    for (bool const is_video : { false, true }) {
        js::Value const part = is_video ? *video : *audio;
        if (part.is_undefined())
            continue;
        std::optional<bool> const answer = read_media_configuration(in, part, is_video, operation);
        if (!answer)
            return rejected_promise(interp, interp.take_exception());
        supported = supported && *answer;
    }
    js::Object* info = interp.new_object();
    interp.root(js::Value::object(info));
    info->put(interp.key("supported"), js::Value::boolean(supported), js::default_attributes);
    info->put(interp.key("smooth"), js::Value::boolean(supported), js::default_attributes);
    info->put(interp.key("powerEfficient"), js::Value::boolean(supported), js::default_attributes);
    info->put(interp.key("configuration"), configuration, js::default_attributes);
    return resolved_promise(interp, js::Value::object(info));
}

void install_media_capabilities(Realm::Internals& in, js::Object& navigator)
{
    js::Object* capabilities = define_interface(in, "MediaCapabilities", nullptr);
    define_promise_operation(in.interpreter, *capabilities, "decodingInfo", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return media_capabilities_info(interp, args, true);
    });
    define_promise_operation(in.interpreter, *capabilities, "encodingInfo", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return media_capabilities_info(interp, args, false);
    });
    define_same_object(in, navigator, "mediaCapabilities", "MediaCapabilities");
}

// --- Media Session --------------------------------------------------------------------------

// MediaMetadata (Media Session §4.3): the title, artist and album, and the
// artwork as a frozen list of frozen images, each src a URL resolved
// against the document when it was set.
class MediaMetadataObject final : public js::Object {
public:
    MediaMetadataObject(js::Object* prototype, js::RealmRecord* realm)
        : Object(prototype, Class::Host)
        , m_realm(realm)
    {
    }
    std::string title;
    std::string artist;
    std::string album;
    js::Value artwork; // a frozen array
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(artwork);
    }

private:
    js::RealmRecord* m_realm;
};

// The session's state, kept for a host that would show it; none does yet.
class MediaSessionObject final : public js::Object {
public:
    MediaSessionObject(js::Object* prototype, js::RealmRecord* realm)
        : Object(prototype, Class::Host)
        , m_realm(realm)
    {
    }
    js::Value metadata; // null or a MediaMetadata
    std::string playback_state = "none";
    std::unordered_map<std::string, js::Value> handlers;
    std::optional<double> duration;
    double playback_rate = 1;
    double position = 0;
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(metadata);
        for (auto const& [action, handler] : handlers)
            tracer.visit(handler);
    }

private:
    js::RealmRecord* m_realm;
};

std::optional<MediaMetadataObject*> this_metadata(js::Interpreter& interp, js::Value const& this_value)
{
    auto* found = this_value.is_object() ? dynamic_cast<MediaMetadataObject*>(this_value.as_object()) : nullptr;
    if (found == nullptr) {
        interp.throw_type_error("Illegal invocation");
        return std::nullopt;
    }
    return found;
}

std::optional<MediaSessionObject*> this_session(js::Interpreter& interp, js::Value const& this_value)
{
    auto* found = this_value.is_object() ? dynamic_cast<MediaSessionObject*>(this_value.as_object()) : nullptr;
    if (found == nullptr) {
        interp.throw_type_error("Illegal invocation");
        return std::nullopt;
    }
    return found;
}

// A sequence of MediaImage dictionaries as a frozen array of frozen
// objects (§4.3, "convert artwork algorithm"): each src resolved, a bad
// one a TypeError.
Native convert_artwork(Realm::Internals& in, js::Value const& given)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(given);
    std::optional<std::vector<js::Value>> const images = interp.iterable_to_list(given);
    if (!images)
        return std::nullopt;
    std::vector<js::Value> converted;
    for (js::Value const& image : *images) {
        interp.root(image);
        if (!image.is_object())
            return interp.throw_type_error("Failed to read the 'artwork' property from 'MediaMetadataInit': The provided value is not of type 'MediaImage'.");
        std::optional<js::Value> const src_value = interp.get(image, "src");
        if (!src_value)
            return std::nullopt;
        if (src_value->is_undefined())
            return interp.throw_type_error("Failed to read the 'src' property from 'MediaImage': Required member is undefined.");
        std::optional<std::string> const src = in.to_utf8(*src_value);
        if (!src)
            return std::nullopt;
        std::optional<net::Url> const resolved = net::parse_url(*src, &in.base_url());
        if (!resolved)
            return interp.throw_type_error("Failed to read the 'src' property from 'MediaImage': '" + *src + "' is not a valid URL.");
        std::string sizes;
        std::string type;
        for (std::pair<std::string_view, std::string*> const& part : { std::pair<std::string_view, std::string*> { "sizes", &sizes }, std::pair<std::string_view, std::string*> { "type", &type } }) {
            std::optional<js::Value> const value = interp.get(image, part.first);
            if (!value)
                return std::nullopt;
            if (value->is_undefined())
                continue;
            std::optional<std::string> const text = in.to_utf8(*value);
            if (!text)
                return std::nullopt;
            *part.second = *text;
        }
        js::Heap::NoCollect const no_collect(interp.heap());
        js::Object* copy = interp.new_object();
        interp.root(js::Value::object(copy));
        copy->put(interp.key("src"), in.string(resolved->serialize()), js::default_attributes);
        copy->put(interp.key("sizes"), in.string(sizes), js::default_attributes);
        copy->put(interp.key("type"), in.string(type), js::default_attributes);
        static_cast<void>(js::set_integrity_level(interp, *copy, true));
        converted.push_back(js::Value::object(copy));
    }
    js::ArrayObject* const list = interp.new_array(converted);
    interp.root(js::Value::object(list));
    static_cast<void>(js::set_integrity_level(interp, *list, true));
    return js::Value::object(list);
}

void install_media_session(Realm::Internals& in, js::Object& navigator)
{
    js::Interpreter& interpreter = in.interpreter;

    js::Object* metadata = define_interface(
        in, "MediaMetadata", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            js::Value const init = js::argument(args, 0);
            interp.root(init);
            if (!init.is_undefined() && !init.is_object())
                return interp.throw_type_error("Failed to construct 'MediaMetadata': parameter 1 is not of type 'MediaMetadataInit'.");
            auto* made = interp.heap().allocate<MediaMetadataObject>(internals.prototype("MediaMetadata"), internals.realm_record);
            interp.root(js::Value::object(made));
            for (std::pair<std::string_view, std::string*> const& part : { std::pair<std::string_view, std::string*> { "title", &made->title },
                     std::pair<std::string_view, std::string*> { "artist", &made->artist }, std::pair<std::string_view, std::string*> { "album", &made->album } }) {
                std::optional<js::Value> const value = member(interp, init, part.first);
                if (!value)
                    return std::nullopt;
                if (value->is_undefined())
                    continue;
                std::optional<std::string> const text = internals.to_utf8(*value);
                if (!text)
                    return std::nullopt;
                *part.second = *text;
            }
            std::optional<js::Value> const artwork = member(interp, init, "artwork");
            if (!artwork)
                return std::nullopt;
            Native const converted = convert_artwork(internals, artwork->is_undefined() ? js::Value::object(interp.new_array()) : *artwork);
            if (!converted)
                return std::nullopt;
            made->artwork = *converted;
            return js::Value::object(made);
        },
        0);
    auto text_accessor = [&](std::string_view name, std::string MediaMetadataObject::*field) {
        define_getter(
            in, *metadata, name,
            [field](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<MediaMetadataObject*> const found = this_metadata(interp, this_value);
                if (!found)
                    return std::nullopt;
                return internals_of(interp).string((*found)->*field);
            },
            [field](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                std::optional<MediaMetadataObject*> const found = this_metadata(interp, this_value);
                if (!found)
                    return std::nullopt;
                std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
                if (!text)
                    return std::nullopt;
                (*found)->*field = *text;
                return js::Value::undefined();
            });
    };
    text_accessor("title", &MediaMetadataObject::title);
    text_accessor("artist", &MediaMetadataObject::artist);
    text_accessor("album", &MediaMetadataObject::album);
    define_getter(
        in, *metadata, "artwork",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<MediaMetadataObject*> const found = this_metadata(interp, this_value);
            if (!found)
                return std::nullopt;
            return (*found)->artwork;
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<MediaMetadataObject*> const found = this_metadata(interp, this_value);
            if (!found)
                return std::nullopt;
            Native const converted = convert_artwork(internals_of(interp), js::argument(args, 0));
            if (!converted)
                return std::nullopt;
            (*found)->artwork = *converted;
            return js::Value::undefined();
        });

    js::Object* session = define_interface(in, "MediaSession", nullptr);
    define_getter(
        in, *session, "metadata",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<MediaSessionObject*> const found = this_session(interp, this_value);
            if (!found)
                return std::nullopt;
            return (*found)->metadata.is_undefined() ? js::Value::null() : (*found)->metadata;
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<MediaSessionObject*> const found = this_session(interp, this_value);
            if (!found)
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            if (value.is_nullish()) {
                (*found)->metadata = js::Value::null();
                return js::Value::undefined();
            }
            if (!value.is_object() || dynamic_cast<MediaMetadataObject*>(value.as_object()) == nullptr)
                return interp.throw_type_error("Failed to set the 'metadata' property on 'MediaSession': The provided value is not of type 'MediaMetadata'.");
            (*found)->metadata = value;
            return js::Value::undefined();
        });
    define_getter(
        in, *session, "playbackState",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<MediaSessionObject*> const found = this_session(interp, this_value);
            if (!found)
                return std::nullopt;
            return internals_of(interp).string((*found)->playback_state);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<MediaSessionObject*> const found = this_session(interp, this_value);
            if (!found)
                return std::nullopt;
            std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            // An enumeration attribute: a value not in it is ignored (WebIDL §3.7.6).
            if (*text == "none" || *text == "paused" || *text == "playing")
                (*found)->playback_state = *text;
            return js::Value::undefined();
        });
    define_operation(interpreter, *session, "setActionHandler", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<MediaSessionObject*> const found = this_session(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const action = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!action)
            return std::nullopt;
        static constexpr std::string_view actions[] = { "play", "pause", "seekbackward", "seekforward", "previoustrack", "nexttrack", "skipad",
            "stop", "seekto", "togglemicrophone", "togglecamera", "togglescreenshare", "hangup", "previousslide", "nextslide",
            "enterpictureinpicture", "voiceactivity" };
        bool known = false;
        for (std::string_view const one : actions)
            known = known || one == *action;
        if (!known)
            return interp.throw_type_error("Failed to execute 'setActionHandler' on 'MediaSession': The provided value '" + *action + "' is not a valid enum value of type MediaSessionAction.");
        js::Value const handler = js::argument(args, 1);
        if (handler.is_nullish()) {
            (*found)->handlers.erase(*action);
            return js::Value::undefined();
        }
        if (!js::Interpreter::is_callable(handler))
            return interp.throw_type_error("Failed to execute 'setActionHandler' on 'MediaSession': parameter 2 is not of type 'Function'.");
        (*found)->handlers[*action] = handler;
        return js::Value::undefined();
    });
    define_operation(interpreter, *session, "setPositionState", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<MediaSessionObject*> const found = this_session(interp, this_value);
        if (!found)
            return std::nullopt;
        js::Value const state = js::argument(args, 0);
        if (!state.is_undefined() && !state.is_object())
            return interp.throw_type_error("Failed to execute 'setPositionState' on 'MediaSession': parameter 1 is not of type 'MediaPositionState'.");
        auto const number = [&](std::string_view name) -> std::optional<std::optional<double>> {
            std::optional<js::Value> const value = member(interp, state, name);
            if (!value)
                return std::nullopt;
            if (value->is_undefined())
                return std::optional<double> {};
            std::optional<double> const got = interp.to_number(*value);
            if (!got)
                return std::nullopt;
            if (!std::isfinite(*got))
                return std::nullopt;
            return std::optional<double> { *got };
        };
        std::optional<std::optional<double>> const duration = number("duration");
        std::optional<std::optional<double>> const rate = number("playbackRate");
        std::optional<std::optional<double>> const position = number("position");
        if (!duration || !rate || !position) {
            if (interp.has_exception())
                return std::nullopt;
            return interp.throw_type_error("Failed to execute 'setPositionState' on 'MediaSession': The provided double value is non-finite.");
        }
        // An empty dictionary clears the state (§4.4 step 1).
        if (!*duration && !*rate && !*position) {
            (*found)->duration.reset();
            (*found)->playback_rate = 1;
            (*found)->position = 0;
            return js::Value::undefined();
        }
        if (!*duration)
            return interp.throw_type_error("Failed to execute 'setPositionState' on 'MediaSession': The duration must be provided.");
        if (**duration < 0)
            return interp.throw_type_error("Failed to execute 'setPositionState' on 'MediaSession': The duration must be positive.");
        double const position_value = *position ? **position : 0;
        if (position_value < 0 || position_value > **duration)
            return interp.throw_type_error("Failed to execute 'setPositionState' on 'MediaSession': The position must be between 0 and the duration.");
        double const rate_value = *rate ? **rate : 1;
        if (rate_value == 0)
            return interp.throw_type_error("Failed to execute 'setPositionState' on 'MediaSession': The playback rate must not be zero.");
        (*found)->duration = **duration;
        (*found)->playback_rate = rate_value;
        (*found)->position = position_value;
        return js::Value::undefined();
    });
    define_getter(in, navigator, "mediaSession", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(same_object(internals, "navigator.mediaSession", [&] {
            auto* made = internals.interpreter.heap().allocate<MediaSessionObject>(internals.prototype("MediaSession"), internals.realm_record);
            made->metadata = js::Value::null();
            return made;
        }));
    });
}

// --- Storage Standard: navigator.storage ----------------------------------------------------

void install_storage_manager(Realm::Internals& in, js::Object& navigator)
{
    js::Object* manager = define_interface(in, "StorageManager", nullptr);
    // Nothing here is persisted beyond what the profile keeps, and nothing
    // asks the reader for more: persistence is never granted.
    define_operation(in.interpreter, *manager, "persisted", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return resolved_promise(interp, js::Value::boolean(false));
    });
    define_operation(in.interpreter, *manager, "persist", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return resolved_promise(interp, js::Value::boolean(false));
    });
    // The estimate: a quota of one gibibyte for an origin, the usage what
    // the engine can count — nothing yet, which is a lower bound, not a lie
    // the other way.
    define_operation(in.interpreter, *manager, "estimate", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        js::Interpreter::Roots const roots(interp);
        js::Object* estimate = interp.new_object();
        interp.root(js::Value::object(estimate));
        estimate->put(interp.key("quota"), js::Value::number(1073741824), js::default_attributes);
        estimate->put(interp.key("usage"), js::Value::number(0), js::default_attributes);
        return resolved_promise(interp, js::Value::object(estimate));
    });
    define_same_object(in, navigator, "storage", "StorageManager");
}

// --- Permissions ------------------------------------------------------------------------------

void install_permissions(Realm::Internals& in, js::Object& navigator)
{
    js::Object* status = define_interface(in, "PermissionStatus", in.prototype("EventTarget"));
    static constexpr std::string_view status_events[] = { "change" };
    define_event_handlers(in, *status, status_events);
    for (std::string_view const name : { "state", "name" }) {
        define_getter(in, *status, name, [name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            if (!this_value.is_object())
                return interp.throw_type_error("Illegal invocation");
            return interp.get(this_value, name == "state" ? "__state" : "__name");
        });
    }
    js::Object* permissions = define_interface(in, "Permissions", nullptr);
    // query(descriptor): the names the registry and the shipping engines
    // know; every one is denied here, since nothing here asks the reader
    // and nothing here has the device.
    define_promise_operation(in.interpreter, *permissions, "query", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::Value const descriptor = js::argument(args, 0);
        interp.root(descriptor);
        if (!descriptor.is_object())
            return reject_type_error(internals, "Failed to execute 'query' on 'Permissions': parameter 1 is not of type 'PermissionDescriptor'.");
        std::optional<js::Value> const name_value = interp.get(descriptor, "name");
        if (!name_value)
            return std::nullopt;
        if (name_value->is_undefined())
            return reject_type_error(internals, "Failed to execute 'query' on 'Permissions': Failed to read the 'name' property from 'PermissionDescriptor': Required member is undefined.");
        std::optional<std::string> const name = internals.to_utf8(*name_value);
        if (!name)
            return std::nullopt;
        static constexpr std::string_view names[] = { "accelerometer", "ambient-light-sensor", "background-fetch", "background-sync", "bluetooth",
            "camera", "clipboard-read", "clipboard-write", "display-capture", "geolocation", "gyroscope", "idle-detection", "local-fonts",
            "magnetometer", "microphone", "midi", "nfc", "notifications", "payment-handler", "persistent-storage", "push", "screen-wake-lock",
            "storage-access", "top-level-storage-access", "window-management", "xr-spatial-tracking" };
        bool known = false;
        for (std::string_view const one : names)
            known = known || one == *name;
        if (!known)
            return reject_type_error(internals, "Failed to execute 'query' on 'Permissions': Failed to read the 'name' property from 'PermissionDescriptor': The provided value '" + *name + "' is not a valid enum value of type PermissionName.");
        // The strings go in under a guard: a value made in an argument list
        // before the key is allocated is held by nothing yet.
        js::Heap::NoCollect const no_collect(interp.heap());
        auto* made = interp.heap().allocate<EventTargetObject>(internals.prototype("PermissionStatus"));
        interp.root(js::Value::object(made));
        made->put(interp.key("__state"), internals.string("denied"), 0);
        made->put(interp.key("__name"), internals.string(*name), 0);
        return resolved_promise(interp, js::Value::object(made));
    });
    define_same_object(in, navigator, "permissions", "Permissions");
}

// --- Geolocation ------------------------------------------------------------------------------

// Every request is refused, in a task, through the error callback: the
// engine has no position source and asks the reader nothing.
Native refuse_position(js::Interpreter& interp, Args args, std::string_view operation)
{
    Realm::Internals& in = internals_of(interp);
    js::Value const success = js::argument(args, 0);
    js::Value const error = js::argument(args, 1);
    std::string const where = "Failed to execute '" + std::string(operation) + "' on 'Geolocation': ";
    if (!js::Interpreter::is_callable(success))
        return interp.throw_type_error(where + "parameter 1 is not of type 'Function'.");
    if (!error.is_nullish() && !js::Interpreter::is_callable(error))
        return interp.throw_type_error(where + "parameter 2 is not of type 'Function'.");
    js::Value const options = js::argument(args, 2);
    if (!options.is_undefined() && !options.is_object())
        return interp.throw_type_error(where + "parameter 3 is not of type 'PositionOptions'.");
    if (error.is_nullish())
        return js::Value::undefined();
    auto held = std::make_shared<js::Persistent>(interp.heap(), error);
    in.post_task([&in, held] {
        Realm::Internals::Entry const entry(in);
        js::Interpreter& inner = in.interpreter;
        js::Interpreter::Roots const roots(inner);
        js::Heap::NoCollect const no_collect(inner.heap());
        js::Object* made = inner.heap().allocate<PlainPlatformObject>(in.prototype("GeolocationPositionError"), in.realm_record);
        inner.root(js::Value::object(made));
        made->put(inner.key("__code"), js::Value::number(1), 0);
        made->put(inner.key("__message"), in.string("User denied Geolocation"), 0);
        js::Value const arguments[] = { js::Value::object(made) };
        in.call_reporting(held->value(), js::Value::undefined(), arguments, "geolocation");
    });
    return js::Value::undefined();
}

void install_geolocation(Realm::Internals& in, js::Object& navigator)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* error = define_interface(in, "GeolocationPositionError", nullptr);
    define_getter(in, *error, "code", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_value.is_object())
            return interp.throw_type_error("Illegal invocation");
        return interp.get(this_value, "__code");
    });
    define_getter(in, *error, "message", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_value.is_object())
            return interp.throw_type_error("Illegal invocation");
        return interp.get(this_value, "__message");
    });
    js::Value const error_constructor = *interpreter.get(*interpreter.global(), interpreter.key("GeolocationPositionError"));
    struct Constant {
        std::string_view name;
        double value;
    };
    for (Constant const constant : { Constant { "PERMISSION_DENIED", 1 }, Constant { "POSITION_UNAVAILABLE", 2 }, Constant { "TIMEOUT", 3 } }) {
        error->put(interpreter.key(constant.name), js::Value::number(constant.value), js::Enumerable);
        error_constructor.as_object()->put(interpreter.key(constant.name), js::Value::number(constant.value), js::Enumerable);
    }
    define_interface(in, "GeolocationPosition", nullptr);
    define_interface(in, "GeolocationCoordinates", nullptr);
    js::Object* geolocation = define_interface(in, "Geolocation", nullptr);
    define_operation(interpreter, *geolocation, "getCurrentPosition", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return refuse_position(interp, args, "getCurrentPosition");
    });
    define_operation(interpreter, *geolocation, "watchPosition", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        if (!refuse_position(interp, args, "watchPosition"))
            return std::nullopt;
        return js::Value::number(static_cast<double>(internals_of(interp).next_listener_id++));
    });
    define_operation(interpreter, *geolocation, "clearWatch", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
    define_same_object(in, navigator, "geolocation", "Geolocation");
}

// --- Media Capture and Streams: navigator.mediaDevices --------------------------------------

void install_media_devices(Realm::Internals& in, js::Object& navigator)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* devices = define_interface(in, "MediaDevices", in.prototype("EventTarget"));
    static constexpr std::string_view device_events[] = { "devicechange" };
    define_event_handlers(in, *devices, device_events);
    define_operation(interpreter, *devices, "enumerateDevices", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return resolved_promise(interp, js::Value::object(interp.new_array()));
    });
    define_operation(interpreter, *devices, "getSupportedConstraints", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return js::Value::object(interp.new_object());
    });
    // getUserMedia: a request with neither audio nor video is a TypeError;
    // any other is refused — no camera or microphone is opened here, and
    // the reader is not asked (NotAllowedError, §10.2).
    define_promise_operation(interpreter, *devices, "getUserMedia", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Value const constraints = js::argument(args, 0);
        if (!constraints.is_undefined() && !constraints.is_object())
            return reject_type_error(internals, "Failed to execute 'getUserMedia' on 'MediaDevices': parameter 1 is not of type 'MediaStreamConstraints'.");
        std::optional<js::Value> const audio = member(interp, constraints, "audio");
        std::optional<js::Value> const video = member(interp, constraints, "video");
        if (!audio || !video)
            return std::nullopt;
        if (!js::Interpreter::to_boolean(*audio) && !js::Interpreter::to_boolean(*video))
            return reject_type_error(internals, "Failed to execute 'getUserMedia' on 'MediaDevices': At least one of audio and video must be requested");
        return reject_with(internals, "NotAllowedError", "Permission denied");
    });
    define_operation(interpreter, *devices, "getDisplayMedia", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return reject_with(internals_of(interp), "NotAllowedError", "Permission denied");
    });
    define_getter(in, navigator, "mediaDevices", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(same_object(internals, "navigator.mediaDevices", [&] {
            return internals.interpreter.heap().allocate<EventTargetObject>(internals.prototype("MediaDevices"));
        }));
    });
}

// --- Screen Wake Lock, Credential Management, and the operations ----------------------------

void install_wake_lock(Realm::Internals& in, js::Object& navigator)
{
    js::Object* wake_lock = define_interface(in, "WakeLock", nullptr);
    define_promise_operation(in.interpreter, *wake_lock, "request", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Value const type = js::argument(args, 0);
        if (!type.is_undefined()) {
            std::optional<std::string> const text = internals.to_utf8(type);
            if (!text)
                return std::nullopt;
            if (*text != "screen")
                return reject_type_error(internals, "Failed to execute 'request' on 'WakeLock': The provided value '" + *text + "' is not a valid enum value of type WakeLockType.");
        }
        // No screen here is kept awake, and the reader is not asked (§4.1 step 7).
        return reject_with(internals, "NotAllowedError", "Wake Lock permission request denied");
    });
    define_same_object(in, navigator, "wakeLock", "WakeLock");
}

void install_credentials(Realm::Internals& in, js::Object& navigator)
{
    js::Object* container = define_interface(in, "CredentialsContainer", nullptr);
    // No credential type is written: a request for one is refused as a
    // browser refuses a type it has not got (NotSupportedError).
    for (std::string_view const name : { "get", "create" }) {
        define_promise_operation(in.interpreter, *container, name, 0, [name](js::Interpreter& interp, js::Value const&, Args args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Value const options = js::argument(args, 0);
            if (!options.is_undefined() && !options.is_object())
                return reject_type_error(internals, "Failed to execute '" + std::string(name) + "' on 'CredentialsContainer': parameter 1 is not of type 'Credential" + (name == "get" ? "Request" : "Creation") + "Options'.");
            return reject_with(internals, "NotSupportedError", "No credential type is supported.");
        });
    }
    define_promise_operation(in.interpreter, *container, "store", 1, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return reject_type_error(internals_of(interp), "Failed to execute 'store' on 'CredentialsContainer': parameter 1 is not of type 'Credential'.");
    });
    define_promise_operation(in.interpreter, *container, "preventSilentAccess", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return resolved_promise(interp, js::Value::undefined());
    });
    define_same_object(in, navigator, "credentials", "CredentialsContainer");
}

void install_navigator_operations(Realm::Internals& in, js::Object& navigator)
{
    js::Interpreter& interpreter = in.interpreter;
    // Gamepad: none is read here.
    define_operation(interpreter, navigator, "getGamepads", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return js::Value::object(interp.new_array());
    });
    // Web Share: no share target on any platform here.
    define_operation(interpreter, navigator, "canShare", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        js::Value const data = js::argument(args, 0);
        if (!data.is_undefined() && !data.is_object())
            return interp.throw_type_error("Failed to execute 'canShare' on 'Navigator': parameter 1 is not of type 'ShareData'.");
        return js::Value::boolean(false);
    });
    define_promise_operation(interpreter, navigator, "share", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Value const data = js::argument(args, 0);
        if (!data.is_undefined() && !data.is_object())
            return reject_type_error(internals, "Failed to execute 'share' on 'Navigator': parameter 1 is not of type 'ShareData'.");
        if (internals.hooks.user_activation && !internals.hooks.user_activation())
            return reject_with(internals, "NotAllowedError", "Failed to execute 'share' on 'Navigator': Must be handling a user gesture to perform a share request.");
        return reject_with(internals, "AbortError", "Share canceled: no share target is available.");
    });
    // Badging: nothing shows a badge, and the standard lets a user agent
    // take the ask and show none.
    for (std::string_view const name : { "setAppBadge", "clearAppBadge" }) {
        define_promise_operation(interpreter, navigator, name, 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
            return resolved_promise(interp, js::Value::undefined());
        });
    }
    // Encrypted Media: no key system is known (§3.1.1 step 6).
    define_promise_operation(interpreter, navigator, "requestMediaKeySystemAccess", 2, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const key_system = internals.to_utf8(js::argument(args, 0));
        if (!key_system)
            return std::nullopt;
        if (key_system->empty())
            return reject_type_error(internals, "Failed to execute 'requestMediaKeySystemAccess' on 'Navigator': The keySystem parameter is empty.");
        js::Interpreter::Roots const roots(interp);
        js::Value const configurations = js::argument(args, 1);
        interp.root(configurations);
        std::optional<std::vector<js::Value>> const list = interp.iterable_to_list(configurations);
        if (!list)
            return rejected_promise(interp, interp.take_exception());
        if (list->empty())
            return reject_type_error(internals, "Failed to execute 'requestMediaKeySystemAccess' on 'Navigator': The supportedConfigurations parameter is empty.");
        return reject_with(internals, "NotSupportedError", "Unsupported keySystem or supportedConfigurations.");
    });
}

} // namespace

void install_navigator_objects(Realm::Internals& in, js::Object& navigator)
{
    js::Heap::NoCollect const guard(in.interpreter.heap());
    install_media_capabilities(in, navigator);
    install_media_session(in, navigator);
    install_storage_manager(in, navigator);
    install_permissions(in, navigator);
    install_geolocation(in, navigator);
    install_media_devices(in, navigator);
    install_wake_lock(in, navigator);
    install_credentials(in, navigator);
    install_navigator_operations(in, navigator);
}

}
