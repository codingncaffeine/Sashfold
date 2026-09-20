#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"

#include "bindings/Fetching.h"

#include "media/StreamBuffer.h"
#include "media/Wav.h"
#include "platform/Audio.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Media: the media element's own state (HTML §4.8.11) — loading, the ready
// state, the playback position kept against the clock, seeking, the play
// promises and the events each change fires — and Media Source Extensions
// over it: MediaSource, SourceBuffer, SourceBufferList, TimeRanges. A page
// hands a SourceBuffer bytes; media::StreamBuffer files them as coded
// frames; the element reads what is buffered from there. The element can
// only be fed through a MediaSource: any other source fails to load, as it
// does in a browser without a decoder for it.

namespace sashfold::bindings {

namespace {

using media::StreamBuffer;
using media::TimeRange;
using media::TimeRanges;

constexpr double tick_ms = 50;
constexpr double time_update_ms = 250;
constexpr double enough_ahead_seconds = 3.0;
constexpr double future_ahead_seconds = 0.05;

enum ReadyState : int { HaveNothing = 0,
    HaveMetadata = 1,
    HaveCurrentData = 2,
    HaveFutureData = 3,
    HaveEnoughData = 4 };
enum NetworkState : int { NetworkEmpty = 0,
    NetworkIdle = 1,
    NetworkLoading = 2,
    NetworkNoSource = 3 };

// Whether this build answers that it plays anything. Until the decoders
// and the presentation are in, a page is told the truth — that it cannot —
// unless the environment asks for the pipeline as far as it goes.
bool playback_enabled()
{
    static bool const enabled = [] {
        char const* const value = std::getenv("SASHFOLD_MEDIA");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// The pipeline's own account on stderr, under SASHFOLD_MEDIA_TRACE=1: what a
// page asked for, what it appended and what became of it.
bool tracing()
{
    static bool const enabled = [] {
        char const* const value = std::getenv("SASHFOLD_MEDIA_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void trace(std::string const& line)
{
    if (tracing())
        std::cerr << "media: " << line << "\n";
}

class MediaSourceObject;

class TimeRangesObject final : public js::Object {
public:
    TimeRangesObject(js::Object* prototype, TimeRanges the_ranges)
        : Object(prototype, Class::Host)
        , ranges(std::move(the_ranges))
    {
    }
    TimeRanges ranges;
};

// A media element's state, kept by its wrapper for the element's life.
class MediaStateObject final : public js::Object {
public:
    explicit MediaStateObject(NodeWrapper& the_wrapper)
        : Object(nullptr, Class::Host)
        , wrapper(&the_wrapper)
    {
    }
    NodeWrapper* wrapper;
    MediaSourceObject* source = nullptr;
    int ready_state = HaveNothing;
    int network_state = NetworkEmpty;
    bool paused = true;
    bool seeking = false;
    bool ended_fired = false;
    bool loaded_data = false;
    double duration = std::nan("");
    // The playback position as of `position_at` on the realm's clock; it
    // moves with the clock only while `advancing`.
    double position = 0;
    double position_at = 0;
    bool advancing = false;
    double playback_rate = 1;
    double default_playback_rate = 1;
    double volume = 1;
    bool muted = false;
    double last_time_update = 0;
    std::uint32_t video_width = 0;
    std::uint32_t video_height = 0;
    TimeRanges played;
    js::Value error = js::Value::null();
    std::vector<std::pair<js::Value, js::Value>> play_promises; // resolve, reject
    bool timer_armed = false;
    bool load_considered = false; // the src the parser gave it has been looked at
    std::uint64_t generation = 0;

    // A file the element fetched whole, and the way out to the speakers it
    // is played through. The device is opened when playing begins and let
    // go with the element; where the machine has no sound server there is
    // none, and the element plays silently against the clock, which is what
    // a browser does on a machine with no sound card.
    std::shared_ptr<media::Sound const> sound;
    std::unique_ptr<platform::AudioDevice> device;
    bool device_tried = false;
    double sound_base = 0; // where in the sound the open stream began
    std::size_t fed_frames = 0; // of the sound, handed to the device

    void trace(js::Tracer& tracer) override;
};

class SourceBufferObject final : public EventTargetObject {
public:
    explicit SourceBufferObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    MediaSourceObject* parent = nullptr; // null once removed
    StreamBuffer buffer;
    bool updating = false;
    bool first_init_seen = false;
    std::uint64_t generation = 0; // bumped by abort(): an append under way is dropped

    void trace(js::Tracer& tracer) override;
    std::size_t size_in_bytes() const override { return EventTargetObject::size_in_bytes() + sizeof(StreamBuffer); }
};

class SourceBufferListObject final : public EventTargetObject {
public:
    explicit SourceBufferListObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    std::vector<SourceBufferObject*> items;

    std::optional<js::Value> get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver) override
    {
        if (key.is_index())
            return key.as_index() < items.size() ? js::Value::object(items[key.as_index()]) : js::Value::undefined();
        return Object::get(interpreter, key, receiver);
    }
    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        for (SourceBufferObject* item : items)
            tracer.visit(static_cast<js::Object*>(item));
    }
};

class MediaSourceObject final : public EventTargetObject {
public:
    explicit MediaSourceObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    enum Ready { Closed,
        Open,
        Ended };
    Ready ready = Closed;
    double duration = std::nan("");
    MediaStateObject* attached = nullptr;
    SourceBufferListObject* buffers = nullptr;
    SourceBufferListObject* active = nullptr;

    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(static_cast<js::Object*>(attached));
        tracer.visit(static_cast<js::Object*>(buffers));
        tracer.visit(static_cast<js::Object*>(active));
    }
};

void MediaStateObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(static_cast<js::Object*>(wrapper));
    tracer.visit(static_cast<js::Object*>(source));
    tracer.visit(error);
    for (auto const& [resolve, reject] : play_promises) {
        tracer.visit(resolve);
        tracer.visit(reject);
    }
}

void SourceBufferObject::trace(js::Tracer& tracer)
{
    EventTargetObject::trace(tracer);
    tracer.visit(static_cast<js::Object*>(parent));
}

template<typename T>
std::optional<T*> this_as(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<T*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

// --- Events ---------------------------------------------------------------------------------

void fire(Realm::Internals& in, js::Object* target, std::string_view type)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(target));
    EventObject* event = in.new_event("Event", type, false, false);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    in.dispatch(*event, target);
}

void queue_fire(Realm::Internals& in, js::Object* target, std::string_view type)
{
    auto held = std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(target));
    in.post_task([&in, held, name = std::string(type)] {
        Realm::Internals::Entry const entry(in);
        fire(in, held->value().as_object(), name);
    });
}

void queue_element_event(Realm::Internals& in, MediaStateObject& state, std::string_view type)
{
    queue_fire(in, state.wrapper, type);
}

// --- Types ----------------------------------------------------------------------------------

std::string ascii_lowered(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

// A "vp09.PP.LL.DD..." codec string (the VP9 codec string of the WebM
// project): profile 0 at 8 bits is what the pipeline is built for; the
// 10 and 12 bit profiles are HDR streams it has no path for yet.
bool vp9_string_ours(std::string_view codec)
{
    if (!codec.starts_with("vp09."))
        return false;
    std::vector<std::string_view> fields;
    for (std::string_view rest = codec.substr(5);;) {
        std::size_t const dot = rest.find('.');
        fields.push_back(rest.substr(0, dot));
        if (dot == std::string_view::npos)
            break;
        rest = rest.substr(dot + 1);
    }
    for (std::string_view const field : fields) {
        if (field.size() != 2 || field[0] < '0' || field[0] > '9' || field[1] < '0' || field[1] > '9')
            return false;
    }
    return fields.size() >= 3 && fields[0] == "00" && fields[2] == "08";
}

// The parameters a player adds to a type to ask about one stream — its
// size, rate, channels, transfer function — and sends impossible values of
// to learn whether the answer means anything. A name not known is passed
// over, as any MIME parameter is.
bool stream_parameter_ours(std::string_view name, std::string_view value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    auto const at_most = [value](double limit) {
        std::string const text(value);
        char* end = nullptr;
        double const number = std::strtod(text.c_str(), &end);
        return end != text.c_str() && *end == '\0' && number > 0 && number <= limit;
    };
    if (name == "width")
        return at_most(3840);
    if (name == "height")
        return at_most(2160);
    if (name == "framerate")
        return at_most(60);
    if (name == "bitrate")
        return at_most(100e6);
    if (name == "channels")
        return at_most(2);
    if (name == "eotf")
        return value == "bt709";
    if (name == "decode-to-texture")
        return value == "true" || value == "false";
    if (name == "experimental")
        return value == "allowed";
    if (name == "cryptoblockformat")
        return false; // encrypted media: none here
    return true;
}

// What this engine has, or is getting, a decoder for: VP9 and Opus in WebM.
// 0: not at all; 1: the container alone was named; 2: with codecs, every
// one of them ours.
int type_support(std::string_view type)
{
    std::string const lowered = ascii_lowered(type);
    std::string_view rest = lowered;
    std::size_t const semicolon = rest.find(';');
    std::string_view const essence = trimmed(rest.substr(0, semicolon));
    // WAVE needs no decoder between the file and the speakers, so it plays
    // whatever else this build can do. Its only codec parameter anyone
    // writes is "1", the whole numbers a plain file carries.
    for (std::string_view const wave : { "audio/wav", "audio/wave", "audio/x-wav", "audio/vnd.wave" }) {
        if (essence != wave)
            continue;
        if (semicolon == std::string_view::npos)
            return 1;
        std::string_view const parameter = trimmed(rest.substr(semicolon + 1));
        return parameter == "codecs=1" || parameter == "codecs=\"1\"" ? 2 : 0;
    }
    if (!playback_enabled())
        return 0;
    bool const video = essence == "video/webm";
    if (!video && essence != "audio/webm")
        return 0;
    int support = 1;
    rest = semicolon == std::string_view::npos ? std::string_view {} : rest.substr(semicolon + 1);
    while (!rest.empty()) {
        std::size_t const next = rest.find(';');
        std::string_view parameter = trimmed(rest.substr(0, next));
        rest = next == std::string_view::npos ? std::string_view {} : rest.substr(next + 1);
        std::size_t const equals = parameter.find('=');
        if (equals == std::string_view::npos)
            continue;
        std::string_view const name = trimmed(parameter.substr(0, equals));
        if (name != "codecs") {
            if (!stream_parameter_ours(name, trimmed(parameter.substr(equals + 1))))
                return 0;
            continue;
        }
        std::string_view list = trimmed(parameter.substr(equals + 1));
        if (list.size() >= 2 && list.front() == '"' && list.back() == '"')
            list = list.substr(1, list.size() - 2);
        if (trimmed(list).empty())
            return 0;
        while (!list.empty()) {
            std::size_t const comma = list.find(',');
            std::string_view const codec = trimmed(list.substr(0, comma));
            list = comma == std::string_view::npos ? std::string_view {} : list.substr(comma + 1);
            bool const ours = codec == "opus" || (video && (codec == "vp9" || vp9_string_ours(codec)));
            if (!ours)
                return 0;
        }
        support = 2;
    }
    return support;
}

// --- TimeRanges and MediaError --------------------------------------------------------------

js::Value make_time_ranges(Realm::Internals& in, TimeRanges ranges)
{
    return js::Value::object(in.interpreter.heap().allocate<TimeRangesObject>(in.prototype("TimeRanges"), std::move(ranges)));
}

js::Value make_media_error(Realm::Internals& in, int code, std::string_view message)
{
    js::Heap::NoCollect const no_collect(in.interpreter.heap());
    js::Object* error = in.interpreter.heap().allocate<PlainPlatformObject>(in.prototype("MediaError"));
    error->put(in.interpreter.key("code"), js::Value::number(code), js::Enumerable);
    error->put(in.interpreter.key("message"), in.string(message), js::Enumerable);
    return js::Value::object(error);
}

// --- The element's state --------------------------------------------------------------------

void run_load(Realm::Internals& in, MediaStateObject& state);
void update_media(Realm::Internals& in, MediaStateObject& state);
void update_sound(Realm::Internals& in, MediaStateObject& state);
void restart_device(MediaStateObject& state);
void fail_load(Realm::Internals& in, MediaStateObject& state, std::string_view message);
void fetch_media(Realm::Internals& in, MediaStateObject& state, net::Url const& url);
TimeRanges sound_buffered(MediaStateObject const& state);

bool is_media_element(dom::Element const& element)
{
    return element.is_html("video") || element.is_html("audio");
}

MediaStateObject& state_of(Realm::Internals& in, dom::Element& element)
{
    NodeWrapper& wrapper = wrapper_for(in, element);
    if (js::Object* const kept = wrapper.same_object("media state"))
        return *static_cast<MediaStateObject*>(kept);
    MediaStateObject* made = nullptr;
    {
        js::Heap::NoCollect const no_collect(in.interpreter.heap());
        made = in.interpreter.heap().allocate<MediaStateObject>(wrapper);
        wrapper.keep_same_object("media state", made);
    }
    return *made;
}

// The state, the element's markup looked at once: a src the parser gave it
// is loaded from the first time a script asks anything of the element.
MediaStateObject& live_state_of(Realm::Internals& in, dom::Element& element)
{
    MediaStateObject& state = state_of(in, element);
    if (!state.load_considered) {
        state.load_considered = true;
        if (element.find_attribute("src") != nullptr)
            run_load(in, state);
    }
    return state;
}

double current_position(Realm::Internals& in, MediaStateObject const& state)
{
    if (!state.advancing)
        return state.position;
    return state.position + (in.now() - state.position_at) / 1000.0 * state.playback_rate;
}

TimeRanges element_buffered(MediaStateObject const& state)
{
    // A file the element holds whole is buffered from end to end.
    if (state.sound)
        return sound_buffered(state);
    if (state.source == nullptr || state.source->buffers == nullptr)
        return {};
    bool const ended = state.source->ready == MediaSourceObject::Ended;
    double highest = 0;
    for (SourceBufferObject const* buffer : state.source->buffers->items)
        highest = std::max(highest, buffer->buffer.highest_end());
    std::optional<TimeRanges> all;
    for (SourceBufferObject const* buffer : state.source->buffers->items) {
        TimeRanges ranges = buffer->buffer.buffered(ended);
        if (ended && !ranges.empty())
            ranges.back().end = std::max(ranges.back().end, highest);
        all = all ? media::intersect(*all, ranges) : std::move(ranges);
    }
    return all.value_or(TimeRanges {});
}

void note_played(MediaStateObject& state, double from, double to)
{
    if (!(to > from))
        return;
    for (TimeRange& range : state.played) {
        if (from <= range.end + 0.001 && to >= range.start - 0.001) {
            range.start = std::min(range.start, from);
            range.end = std::max(range.end, to);
            return;
        }
    }
    state.played.push_back({ from, to });
    std::sort(state.played.begin(), state.played.end(), [](TimeRange const& a, TimeRange const& b) { return a.start < b.start; });
}

void settle_play_promises(Realm::Internals& in, MediaStateObject& state, bool resolve, std::string_view error_name = {}, std::string_view message = {})
{
    if (state.play_promises.empty())
        return;
    js::Interpreter::Roots const roots(in.interpreter);
    std::vector<std::pair<js::Value, js::Value>> const taken = std::exchange(state.play_promises, {});
    for (auto const& [resolver, rejecter] : taken) {
        in.interpreter.root(resolver);
        in.interpreter.root(rejecter);
    }
    for (auto const& [resolver, rejecter] : taken) {
        if (resolve) {
            in.call_reporting(resolver, js::Value::undefined(), {}, "play()");
        } else {
            js::Value const reason = dom_exception_value(in, error_name, message);
            in.interpreter.root(reason);
            js::Value const arguments[] = { reason };
            in.call_reporting(rejecter, js::Value::undefined(), arguments, "play()");
        }
    }
}

void arm_timer(Realm::Internals& in, MediaStateObject& state);

void set_ready_state(Realm::Internals& in, MediaStateObject& state, int next)
{
    int const previous = state.ready_state;
    if (next == previous)
        return;
    trace("readyState " + std::to_string(previous) + " -> " + std::to_string(next) + " at " + std::to_string(state.position));
    state.ready_state = next;
    if (previous == HaveNothing)
        return; // loadedmetadata is fired by whoever read the metadata
    if (next >= HaveCurrentData && !state.loaded_data) {
        state.loaded_data = true;
        queue_element_event(in, state, "loadeddata");
    }
    if (next >= HaveFutureData && previous <= HaveCurrentData) {
        queue_element_event(in, state, "canplay");
        if (!state.paused) {
            queue_element_event(in, state, "playing");
            settle_play_promises(in, state, true);
        }
    }
    if (next == HaveEnoughData) {
        queue_element_event(in, state, "canplaythrough");
        dom::Node& node = state.wrapper->node();
        if (state.paused && node.is_element() && static_cast<dom::Element&>(node).find_attribute("autoplay") != nullptr) {
            state.paused = false;
            queue_element_event(in, state, "play");
            queue_element_event(in, state, "playing");
        }
    }
    if (next <= HaveCurrentData && previous >= HaveFutureData && !state.paused && !state.ended_fired && !state.seeking) {
        queue_element_event(in, state, "timeupdate");
        queue_element_event(in, state, "waiting");
    }
}

void set_duration(Realm::Internals& in, MediaStateObject& state, double duration)
{
    if (duration == state.duration || (std::isnan(duration) && std::isnan(state.duration)))
        return;
    state.duration = duration;
    queue_element_event(in, state, "durationchange");
    if (!std::isnan(duration) && state.position > duration) {
        state.position = duration;
        state.seeking = true;
        queue_element_event(in, state, "seeking");
    }
}

// Brings everything that follows from the clock and from what is buffered
// up to date: the position, the ready state, a seek that can now finish,
// the end of playback.
void update_media(Realm::Internals& in, MediaStateObject& state)
{
    if (state.wrapper->detached() || state.ready_state == HaveNothing)
        return;
    if (state.sound) {
        update_sound(in, state);
        return;
    }
    double const now = in.now();
    TimeRanges const buffered = element_buffered(state);
    bool const source_ended = state.source != nullptr && state.source->ready == MediaSourceObject::Ended;

    if (state.advancing) {
        double const from = state.position;
        double target = from + (now - state.position_at) / 1000.0 * state.playback_rate;
        TimeRange const* const range = media::range_at(buffered, from, true);
        double limit = range != nullptr ? range->end : from;
        if (!std::isnan(state.duration))
            limit = std::min(limit, state.duration);
        target = std::clamp(target, 0.0, std::max(limit, 0.0));
        state.position = target;
        note_played(state, from, target);
    }
    state.position_at = now;

    double const position = state.position;
    bool const at_end = !std::isnan(state.duration) && std::isfinite(state.duration) && position >= state.duration
        && (source_ended || state.source == nullptr);
    TimeRange const* const range = media::range_at(buffered, position, at_end);
    int ready = HaveMetadata;
    if (range != nullptr) {
        double const ahead = range->end - position;
        bool const to_the_end = !std::isnan(state.duration) && range->end >= state.duration && source_ended;
        if (to_the_end || ahead >= enough_ahead_seconds)
            ready = HaveEnoughData;
        else if (ahead > future_ahead_seconds)
            ready = HaveFutureData;
        else
            ready = HaveCurrentData;
    } else if (media::range_at(buffered, position, true) != nullptr) {
        ready = HaveCurrentData; // at the very end of what is buffered: the last picture, nothing after it
    }

    if (state.seeking && (range != nullptr || at_end)) {
        state.seeking = false;
        queue_element_event(in, state, "timeupdate");
        queue_element_event(in, state, "seeked");
    }
    set_ready_state(in, state, ready);

    if (at_end && !state.seeking) {
        if (!state.ended_fired) {
            state.ended_fired = true;
            queue_element_event(in, state, "timeupdate");
            dom::Node& node = state.wrapper->node();
            bool const loops = node.is_element() && static_cast<dom::Element&>(node).find_attribute("loop") != nullptr;
            if (loops) {
                state.ended_fired = false;
                state.position = 0;
                state.seeking = true;
                queue_element_event(in, state, "seeking");
            } else {
                if (!state.paused) {
                    state.paused = true;
                    queue_element_event(in, state, "pause");
                    settle_play_promises(in, state, false, "AbortError", "The play() request was interrupted because playback ended.");
                }
                queue_element_event(in, state, "ended");
            }
        }
    } else {
        state.ended_fired = false;
    }

    state.advancing = !state.paused && !state.seeking && !state.ended_fired && state.ready_state >= HaveFutureData
        && state.playback_rate > 0;
    if (state.advancing && now - state.last_time_update >= time_update_ms) {
        state.last_time_update = now;
        queue_element_event(in, state, "timeupdate");
    }
    arm_timer(in, state);
}

void arm_timer(Realm::Internals& in, MediaStateObject& state)
{
    // The clock is only watched while it can change something: playing, or
    // a seek waiting for its data.
    if (state.timer_armed || (state.source == nullptr && !state.sound) || (state.paused && !state.seeking))
        return;
    state.timer_armed = true;
    js::Interpreter::Roots const roots(in.interpreter);
    js::Value const held = js::Value::object(&state);
    in.interpreter.root(held);
    js::ClosureFunction* tick = in.interpreter.new_closure("media tick", 0, { held },
        [](js::Interpreter& inner, js::ClosureFunction& self, js::Value const&, Args) -> Native {
            auto& ticking = *static_cast<MediaStateObject*>(self.slot(0).as_object());
            ticking.timer_armed = false;
            update_media(internals_of(inner), ticking);
            return js::Value::undefined();
        });
    in.interpreter.root(js::Value::object(tick));
    schedule_native(in, tick_ms, js::Value::object(tick));
}

void seek(Realm::Internals& in, MediaStateObject& state, double time)
{
    if (state.ready_state == HaveNothing) {
        state.position = std::max(time, 0.0);
        return;
    }
    update_media(in, state);
    if (!std::isnan(state.duration))
        time = std::min(time, state.duration);
    trace("seek to " + std::to_string(time));
    time = std::max(time, 0.0);
    state.seeking = true;
    state.advancing = false;
    state.ended_fired = false;
    state.position = time;
    state.position_at = in.now();
    // What the speakers still hold is of the old position: it goes, and the
    // sound is fed again from where the seek landed.
    restart_device(state);
    queue_element_event(in, state, "seeking");
    auto held = std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(&state));
    in.post_task([&in, held] {
        Realm::Internals::Entry const entry(in);
        update_media(in, *static_cast<MediaStateObject*>(held->value().as_object()));
    });
}

// --- A file, played through the machine's speakers ---------------------------------------------

// Opens the way out, once, at the sound's own rate: a server resamples for
// its sink, which is its business and not the engine's. A machine with no
// sound server leaves the element playing silently against the clock, as a
// browser does on a machine with no sound card.
void open_device(MediaStateObject& state)
{
    if (state.device || state.device_tried || !state.sound)
        return;
    state.device_tried = true;
    platform::AudioFormat format;
    format.rate = state.sound->rate;
    format.channels = state.sound->channels;
    std::string error;
    state.device = platform::AudioDevice::open(format, "Sashfold", error);
    trace(state.device ? "opened the speakers" : "no speakers: " + error);
    if (state.device) {
        state.device->set_volume(state.muted ? 0.0 : state.volume);
        state.sound_base = state.position;
        state.fed_frames = static_cast<std::size_t>(state.position * static_cast<double>(state.sound->rate));
    }
}

// Tops the device up with what it will take. The samples are decoded
// already, so this is a copy of a few milliseconds' worth; the socket and
// the waiting are the device's own thread.
void feed_device(MediaStateObject& state)
{
    if (!state.device || !state.device->ok() || !state.sound)
        return;
    media::Sound const& sound = *state.sound;
    std::size_t const total = sound.frames();
    while (state.fed_frames < total) {
        std::size_t const room = state.device->writable_frames();
        if (room == 0)
            return;
        std::size_t const frames = std::min(room, total - state.fed_frames);
        std::span<float const> const samples(sound.samples.data() + state.fed_frames * sound.channels, frames * sound.channels);
        std::size_t const taken = state.device->write(samples);
        if (taken == 0)
            return;
        state.fed_frames += taken;
    }
}

// Where the sound begins again after a seek, or when it is first played.
void restart_device(MediaStateObject& state)
{
    if (!state.device || !state.sound)
        return;
    state.device->flush();
    state.sound_base = state.position;
    state.fed_frames = std::min(static_cast<std::size_t>(state.position * static_cast<double>(state.sound->rate)), state.sound->frames());
}

// What a file the element holds whole does as time passes: everything is
// buffered, so what moves is the position — by what has been HEARD where
// there are speakers, and by the clock where there are none.
void update_sound(Realm::Internals& in, MediaStateObject& state)
{
    media::Sound const& sound = *state.sound;
    double const duration = sound.seconds();
    double const now = in.now();
    bool const playing = !state.paused && !state.seeking && !state.ended_fired;
    // The device plays at the rate the samples were made for; a page that
    // asks for another rate is answered by the clock until there is a
    // resampler to ask for it properly.
    bool const through_speakers = state.playback_rate == 1;
    if (playing && through_speakers)
        open_device(state);
    if (state.device)
        state.device->set_paused(!playing || !through_speakers);
    if (playing && through_speakers)
        feed_device(state);
    bool const heard = playing && through_speakers && state.device && state.device->ok() && state.device->clock().valid;

    double const before = state.position;
    if (playing) {
        if (heard)
            state.position = state.sound_base + state.device->clock().played_seconds;
        else
            state.position += (now - state.position_at) / 1000.0 * state.playback_rate;
        state.position = std::clamp(state.position, 0.0, duration);
        note_played(state, before, state.position);
    }
    state.position_at = now;
    state.advancing = playing;

    if (state.seeking) {
        state.seeking = false;
        queue_element_event(in, state, "timeupdate");
        queue_element_event(in, state, "seeked");
    }
    set_ready_state(in, state, HaveEnoughData);

    // The end: the clock has run out, and nothing is left in flight.
    bool const drained = !state.device || !state.device->ok() || state.fed_frames >= sound.frames();
    if (state.position >= duration - 0.0005 && drained) {
        dom::Node& node = state.wrapper->node();
        bool const loops = node.is_element() && static_cast<dom::Element&>(node).find_attribute("loop") != nullptr;
        state.position = duration;
        if (loops) {
            seek(in, state, 0);
            return;
        }
        if (!state.ended_fired) {
            state.ended_fired = true;
            state.advancing = false;
            queue_element_event(in, state, "timeupdate");
            if (!state.paused) {
                state.paused = true;
                queue_element_event(in, state, "pause");
                settle_play_promises(in, state, false, "AbortError", "The play() request was interrupted because playback ended.");
            }
            queue_element_event(in, state, "ended");
        }
    }
    if (state.advancing && now - state.last_time_update >= time_update_ms) {
        state.last_time_update = now;
        queue_element_event(in, state, "timeupdate");
    }
    arm_timer(in, state);
}

// What the element holds of a file it fetched: all of it, from the moment
// it is read.
TimeRanges sound_buffered(MediaStateObject const& state)
{
    if (!state.sound || state.sound->frames() == 0)
        return {};
    return TimeRanges { { 0, state.sound->seconds() } };
}

// Fetches a src that names no MediaSource and reads what came back. Only
// what this build decodes plays; anything else fails the way a browser
// without that decoder does.
void fetch_media(Realm::Internals& in, MediaStateObject& state, net::Url const& url)
{
    PageRequest request;
    request.url = url;
    request.mode = FetchMode::NoCors;
    request.credentials = FetchCredentials::SameOrigin;
    request.destination = "audio";
    auto held = std::make_shared<js::Persistent>(in.interpreter.heap(), js::Value::object(&state));
    auto shared = std::make_shared<PageRequest>(std::move(request));
    std::uint64_t const generation = state.generation;
    in.post_task([&in, held, shared, generation] {
        Realm::Internals::Entry const entry(in);
        auto& target = *static_cast<MediaStateObject*>(held->value().as_object());
        if (target.generation != generation)
            return;
        FetchOutcome const outcome = perform_fetch(in, *shared);
        if (target.generation != generation)
            return;
        trace("fetched " + shared->url.serialize() + ": " + (outcome.ok ? std::to_string(outcome.body.size()) + " bytes" : outcome.error));
        if (!outcome.ok || outcome.status < 200 || outcome.status >= 300) {
            fail_load(in, target, "The media could not be fetched.");
            return;
        }
        std::optional<media::Sound> decoded = media::decode_wav(outcome.body);
        if (!decoded || decoded->frames() == 0) {
            fail_load(in, target, "No decoder is available for this source.");
            return;
        }
        target.sound = std::make_shared<media::Sound const>(std::move(*decoded));
        target.network_state = NetworkIdle;
        target.ready_state = HaveMetadata;
        target.position_at = in.now();
        queue_element_event(in, target, "loadedmetadata");
        set_duration(in, target, target.sound->seconds());
        update_sound(in, target);
    });
}

// --- MediaSource on the element -------------------------------------------------------------

void remove_all_buffers(Realm::Internals& in, MediaSourceObject& source)
{
    if (source.active != nullptr && !source.active->items.empty()) {
        source.active->items.clear();
        queue_fire(in, source.active, "removesourcebuffer");
    }
    if (source.buffers != nullptr && !source.buffers->items.empty()) {
        for (SourceBufferObject* buffer : source.buffers->items) {
            buffer->parent = nullptr;
            ++buffer->generation;
            buffer->updating = false;
        }
        source.buffers->items.clear();
        queue_fire(in, source.buffers, "removesourcebuffer");
    }
}

void detach_source(Realm::Internals& in, MediaStateObject& state)
{
    MediaSourceObject* const source = state.source;
    if (source == nullptr)
        return;
    state.source = nullptr;
    source->attached = nullptr;
    source->ready = MediaSourceObject::Closed;
    source->duration = std::nan("");
    remove_all_buffers(in, *source);
    queue_fire(in, source, "sourceclose");
}

void fail_load(Realm::Internals& in, MediaStateObject& state, std::string_view message)
{
    trace("load failed: " + std::string(message));
    state.error = make_media_error(in, 4, message);
    state.network_state = NetworkNoSource;
    queue_element_event(in, state, "error");
    settle_play_promises(in, state, false, "NotSupportedError", "The element has no supported sources.");
}

// The media element load algorithm (HTML §4.8.11.5) as far as this engine
// has resources to select: a MediaSource by its object URL, or a failure.
void run_load(Realm::Internals& in, MediaStateObject& state)
{
    state.load_considered = true;
    ++state.generation;
    settle_play_promises(in, state, false, "AbortError", "The play() request was interrupted by a new load request.");
    detach_source(in, state);
    if (state.network_state != NetworkEmpty) {
        queue_element_event(in, state, "emptied");
        state.ready_state = HaveNothing;
        state.paused = true;
        state.seeking = false;
        state.advancing = false;
        state.ended_fired = false;
        state.loaded_data = false;
        state.position = 0;
        state.played.clear();
        state.video_width = 0;
        state.video_height = 0;
        state.duration = std::nan("");
    }
    state.sound.reset();
    state.device.reset();
    state.device_tried = false;
    state.fed_frames = 0;
    state.sound_base = 0;
    state.error = js::Value::null();
    state.playback_rate = state.default_playback_rate;

    dom::Node& node = state.wrapper->node();
    if (!node.is_element())
        return;
    auto& element = static_cast<dom::Element&>(node);
    dom::Attr const* const src = element.find_attribute("src");
    if (src == nullptr) {
        // With <source> children each is tried and each fails, at the
        // <source>; with none there is nothing to load.
        bool any = false;
        for (dom::Node* child : element.children()) {
            if (child->is_element() && static_cast<dom::Element*>(child)->is_html("source")) {
                any = true;
                queue_fire(in, in.wrap(*child), "error");
            }
        }
        state.network_state = any ? NetworkNoSource : NetworkEmpty;
        if (any)
            queue_element_event(in, state, "loadstart");
        return;
    }
    state.network_state = NetworkLoading;
    queue_element_event(in, state, "loadstart");
    std::optional<net::Url> const url = net::parse_url(src->value, &in.base_url());
    auto const named = url ? in.media_source_urls.find(url->serialize(true)) : in.media_source_urls.end();
    auto* const source = named != in.media_source_urls.end() ? dynamic_cast<MediaSourceObject*>(named->second) : nullptr;
    if (source != nullptr && (source->ready != MediaSourceObject::Closed || source->attached != nullptr)) {
        fail_load(in, state, "The MediaSource is already in use.");
        return;
    }
    if (source == nullptr) {
        // Anything else is a file to fetch and read.
        if (!url) {
            fail_load(in, state, "That is not an address a media element can load.");
            return;
        }
        fetch_media(in, state, *url);
        return;
    }
    state.source = source;
    source->attached = &state;
    source->ready = MediaSourceObject::Open;
    queue_fire(in, source, "sourceopen");
    trace("MediaSource attached");
}

// A fault in the stream, or endOfStream() with an error (MSE §3.5.6): a
// load that never got its metadata fails; one that did stops with a fatal
// error.
void source_failed(Realm::Internals& in, MediaStateObject& state, int code, std::string_view message)
{
    if (state.ready_state == HaveNothing) {
        fail_load(in, state, message);
        return;
    }
    state.error = make_media_error(in, code, message);
    state.network_state = NetworkIdle;
    state.advancing = false;
    queue_element_event(in, state, "error");
}

void end_of_stream(Realm::Internals& in, MediaSourceObject& source, std::string_view error)
{
    trace("endOfStream " + std::string(error));
    source.ready = MediaSourceObject::Ended;
    queue_fire(in, &source, "sourceended");
    MediaStateObject* const state = source.attached;
    if (error.empty()) {
        double highest = 0;
        for (SourceBufferObject const* buffer : source.buffers->items)
            highest = std::max(highest, buffer->buffer.highest_end());
        source.duration = highest;
        if (state != nullptr) {
            set_duration(in, *state, highest);
            update_media(in, *state);
        }
    } else if (state != nullptr) {
        source_failed(in, *state, error == "network" ? 2 : 3, error == "network" ? "A network error ended the stream." : "The stream could not be decoded.");
    }
}

// What an append or a removal leaves behind for the element to notice.
void buffer_changed(Realm::Internals& in, SourceBufferObject& buffer, bool init_segment)
{
    MediaSourceObject* const source = buffer.parent;
    if (source == nullptr)
        return;
    MediaStateObject* const state = source->attached;
    if (init_segment && !buffer.first_init_seen) {
        buffer.first_init_seen = true;
        source->active->items.push_back(&buffer);
        queue_fire(in, source->active, "addsourcebuffer");
    }
    if (init_segment && std::isnan(source->duration))
        source->duration = buffer.buffer.init().duration_seconds.value_or(std::numeric_limits<double>::infinity());
    double const highest = buffer.buffer.highest_end();
    if (!std::isnan(source->duration) && highest > source->duration)
        source->duration = highest;
    if (state == nullptr)
        return;
    set_duration(in, *state, source->duration);
    if (init_segment) {
        if (StreamBuffer::Track const* const video = buffer.buffer.track_of(media::WebmTrack::Kind::Video)) {
            if (video->description.width != state->video_width || video->description.height != state->video_height) {
                state->video_width = video->description.width;
                state->video_height = video->description.height;
                queue_element_event(in, *state, "resize");
            }
        }
        bool all = true;
        for (SourceBufferObject const* each : source->buffers->items)
            all = all && each->first_init_seen;
        if (all && state->ready_state == HaveNothing) {
            state->ready_state = HaveMetadata;
            state->network_state = NetworkIdle;
            state->position_at = in.now();
            queue_element_event(in, *state, "loadedmetadata");
        }
    }
    update_media(in, *state);
}

// --- SourceBuffer ---------------------------------------------------------------------------

// The checks every changing call begins with; the "prepare append" half
// reopens an ended source (MSE §3.5.4).
Native refuse_if_busy(Realm::Internals& in, SourceBufferObject const& buffer)
{
    if (buffer.parent == nullptr)
        return in.throw_dom_exception("InvalidStateError", "This SourceBuffer has been removed from its MediaSource.");
    if (buffer.updating)
        return in.throw_dom_exception("InvalidStateError", "This SourceBuffer is still processing an append or a removal.");
    return js::Value::undefined();
}

void reopen_if_ended(Realm::Internals& in, MediaSourceObject& source)
{
    if (source.ready == MediaSourceObject::Ended) {
        source.ready = MediaSourceObject::Open;
        queue_fire(in, &source, "sourceopen");
    }
}

void finish_update(Realm::Internals& in, SourceBufferObject& buffer, bool failed)
{
    buffer.updating = false;
    fire(in, &buffer, failed ? "error" : "update");
    fire(in, &buffer, "updateend");
}

Native append_buffer(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
    if (!found)
        return std::nullopt;
    SourceBufferObject& buffer = **found;
    std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(js::argument(args, 0));
    if (!bytes)
        return interp.throw_type_error("Failed to execute 'appendBuffer' on 'SourceBuffer': parameter 1 is not an ArrayBuffer or a view of one.");
    if (Native refused = refuse_if_busy(in, buffer); !refused)
        return refused;
    MediaSourceObject& source = *buffer.parent;
    if (source.attached != nullptr && !source.attached->error.is_null())
        return in.throw_dom_exception("InvalidStateError", "The media element has an error.");
    reopen_if_ended(in, source);
    double const position = source.attached != nullptr ? current_position(in, *source.attached) : 0;
    if (!buffer.buffer.evict(position, bytes->size()))
        return in.throw_dom_exception("QuotaExceededError", "The SourceBuffer is full, and nothing before the playback position is left to give up.");

    auto data = std::make_shared<std::vector<std::uint8_t>>(bytes->begin(), bytes->end());
    buffer.updating = true;
    std::uint64_t const generation = buffer.generation;
    auto held = std::make_shared<js::Persistent>(interp.heap(), this_value);
    in.post_task([&in, held, data, generation] {
        Realm::Internals::Entry const entry(in);
        auto& target = *static_cast<SourceBufferObject*>(held->value().as_object());
        if (target.generation != generation || target.parent == nullptr)
            return;
        fire(in, &target, "updatestart");
        if (target.generation != generation || target.parent == nullptr)
            return;
        StreamBuffer::Appended const appended = target.buffer.append(*data);
        if (tracing()) {
            std::string line = "append " + std::to_string(data->size()) + " bytes" + (appended.ok ? "" : " FAILED") + (appended.init_segment ? " init" : "")
                + " held=" + std::to_string(target.buffer.bytes()) + " buffered=";
            for (TimeRange const& range : target.buffer.buffered())
                line += "[" + std::to_string(range.start) + "," + std::to_string(range.end) + ")";
            for (StreamBuffer::Track const& track : target.buffer.tracks())
                line += " " + track.description.codec_id + ":" + std::to_string(track.frames.size());
            trace(line);
        }
        buffer_changed(in, target, appended.init_segment);
        finish_update(in, target, !appended.ok);
        if (!appended.ok && target.parent != nullptr && target.parent->ready == MediaSourceObject::Open)
            end_of_stream(in, *target.parent, "decode");
    });
    return js::Value::undefined();
}

Native remove_range(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
    if (!found)
        return std::nullopt;
    SourceBufferObject& buffer = **found;
    std::optional<double> const start = interp.to_number(js::argument(args, 0));
    if (!start)
        return std::nullopt;
    std::optional<double> const end = interp.to_number(js::argument(args, 1));
    if (!end)
        return std::nullopt;
    if (Native refused = refuse_if_busy(in, buffer); !refused)
        return refused;
    MediaSourceObject& source = *buffer.parent;
    if (std::isnan(source.duration))
        return interp.throw_type_error("Failed to execute 'remove' on 'SourceBuffer': the duration is not known.");
    if (std::isnan(*start) || *start < 0 || *start > source.duration)
        return interp.throw_type_error("Failed to execute 'remove' on 'SourceBuffer': the start is outside the media.");
    if (std::isnan(*end) || *end <= *start)
        return interp.throw_type_error("Failed to execute 'remove' on 'SourceBuffer': the end must be after the start.");
    reopen_if_ended(in, source);

    buffer.updating = true;
    std::uint64_t const generation = buffer.generation;
    auto held = std::make_shared<js::Persistent>(interp.heap(), this_value);
    in.post_task([&in, held, generation, from = *start, to = *end] {
        Realm::Internals::Entry const entry(in);
        auto& target = *static_cast<SourceBufferObject*>(held->value().as_object());
        if (target.generation != generation || target.parent == nullptr)
            return;
        fire(in, &target, "updatestart");
        if (target.generation != generation || target.parent == nullptr)
            return;
        target.buffer.remove(from, to);
        buffer_changed(in, target, false);
        finish_update(in, target, false);
    });
    return js::Value::undefined();
}

void install_source_buffer(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* proto = define_interface(in, "SourceBuffer", in.prototype("EventTarget"));
    js::define_method(interpreter, *proto, "appendBuffer", 1, append_buffer);
    js::define_method(interpreter, *proto, "remove", 2, remove_range);
    js::define_method(interpreter, *proto, "abort", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        SourceBufferObject& buffer = **found;
        if (buffer.parent == nullptr || buffer.parent->ready != MediaSourceObject::Open)
            return internals.throw_dom_exception("InvalidStateError", "The MediaSource is not open.");
        if (buffer.updating) {
            ++buffer.generation;
            buffer.updating = false;
            queue_fire(internals, &buffer, "abort");
            queue_fire(internals, &buffer, "updateend");
        }
        buffer.buffer.reset_parser();
        buffer.buffer.append_window_start = 0;
        buffer.buffer.append_window_end = std::numeric_limits<double>::infinity();
        return js::Value::undefined();
    });
    js::define_method(interpreter, *proto, "changeType", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const type = internals.to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        if (type->empty())
            return interp.throw_type_error("Failed to execute 'changeType' on 'SourceBuffer': the type is empty.");
        if (Native refused = refuse_if_busy(internals, **found); !refused)
            return refused;
        if (type_support(*type) == 0)
            return internals.throw_dom_exception("NotSupportedError", "The type is not supported.");
        reopen_if_ended(internals, *(*found)->parent);
        (*found)->buffer.reset_parser();
        return js::Value::undefined();
    });

    define_getter(in, *proto, "updating", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::boolean((*found)->updating);
    });
    define_getter(in, *proto, "buffered", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        if ((*found)->parent == nullptr)
            return internals.throw_dom_exception("InvalidStateError", "This SourceBuffer has been removed from its MediaSource.");
        return make_time_ranges(internals, (*found)->buffer.buffered((*found)->parent->ready == MediaSourceObject::Ended));
    });
    define_getter(
        in, *proto, "mode",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            return internals_of(interp).string((*found)->buffer.mode == StreamBuffer::Mode::Sequence ? "sequence" : "segments");
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            std::optional<std::string> const mode = internals.to_utf8(js::argument(args, 0));
            if (!mode)
                return std::nullopt;
            if (*mode != "segments" && *mode != "sequence")
                return js::Value::undefined(); // not a value of the enumeration: ignored
            if (Native refused = refuse_if_busy(internals, **found); !refused)
                return refused;
            StreamBuffer& buffer = (*found)->buffer;
            if (buffer.parsing_media_segment())
                return internals.throw_dom_exception("InvalidStateError", "A media segment is being parsed.");
            reopen_if_ended(internals, *(*found)->parent);
            buffer.mode = *mode == "sequence" ? StreamBuffer::Mode::Sequence : StreamBuffer::Mode::Segments;
            if (buffer.mode == StreamBuffer::Mode::Sequence)
                buffer.group_start = buffer.group_end();
            return js::Value::undefined();
        });

    struct Number {
        char const* name;
        double StreamBuffer::* member;
    };
    static constexpr Number numbers[] = { { "timestampOffset", &StreamBuffer::timestamp_offset },
        { "appendWindowStart", &StreamBuffer::append_window_start }, { "appendWindowEnd", &StreamBuffer::append_window_end } };
    for (Number const& number : numbers) {
        auto const member = number.member;
        std::string const name = number.name;
        define_getter(
            in, *proto, name,
            [member](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
                if (!found)
                    return std::nullopt;
                return js::Value::number((*found)->buffer.*member);
            },
            [member, name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                Realm::Internals& internals = internals_of(interp);
                std::optional<SourceBufferObject*> const found = this_as<SourceBufferObject>(interp, this_value);
                if (!found)
                    return std::nullopt;
                std::optional<double> const value = interp.to_number(js::argument(args, 0));
                if (!value)
                    return std::nullopt;
                StreamBuffer& buffer = (*found)->buffer;
                if (name == "timestampOffset" && !std::isfinite(*value))
                    return interp.throw_type_error("The provided double value is non-finite.");
                if (Native refused = refuse_if_busy(internals, **found); !refused)
                    return refused;
                if (name == "timestampOffset") {
                    if (buffer.parsing_media_segment())
                        return internals.throw_dom_exception("InvalidStateError", "A media segment is being parsed.");
                    reopen_if_ended(internals, *(*found)->parent);
                    if (buffer.mode == StreamBuffer::Mode::Sequence)
                        buffer.group_start = *value;
                } else if (name == "appendWindowStart") {
                    if (!std::isfinite(*value) || *value < 0 || *value >= buffer.append_window_end)
                        return interp.throw_type_error("The append window's start must be at least 0 and before its end.");
                } else if (std::isnan(*value) || *value <= buffer.append_window_start) {
                    return interp.throw_type_error("The append window's end must be after its start.");
                }
                buffer.*member = *value;
                return js::Value::undefined();
            });
    }
    static constexpr std::string_view buffer_events[] = { "updatestart", "update", "updateend", "error", "abort" };
    define_event_handlers(in, *proto, buffer_events);

    js::Object* list = define_interface(in, "SourceBufferList", in.prototype("EventTarget"));
    define_getter(in, *list, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<SourceBufferListObject*> const found = this_as<SourceBufferListObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*found)->items.size()));
    });
    static constexpr std::string_view list_events[] = { "addsourcebuffer", "removesourcebuffer" };
    define_event_handlers(in, *list, list_events);
}

// --- MediaSource ----------------------------------------------------------------------------

void install_media_source(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* proto = define_interface(in, "MediaSource", in.prototype("EventTarget"),
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Interpreter::Roots const roots(interp);
            auto* made = interp.heap().allocate<MediaSourceObject>(internals.prototype("MediaSource"));
            interp.root(js::Value::object(made));
            made->buffers = interp.heap().allocate<SourceBufferListObject>(internals.prototype("SourceBufferList"));
            made->active = interp.heap().allocate<SourceBufferListObject>(internals.prototype("SourceBufferList"));
            return js::Value::object(made);
        },
        0);
    js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key("MediaSource"));
    js::define_method(interpreter, *constructor.as_object(), "isTypeSupported", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        std::optional<std::string> const type = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        trace("isTypeSupported " + *type + " -> " + (type_support(*type) > 0 ? "yes" : "no"));
        return js::Value::boolean(type_support(*type) > 0);
    });
    constructor.as_object()->put(interpreter.key("canConstructInDedicatedWorker"), js::Value::boolean(false), js::Enumerable);

    define_getter(in, *proto, "readyState", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        static constexpr std::string_view names[] = { "closed", "open", "ended" };
        return internals_of(interp).string(names[(*found)->ready]);
    });
    define_getter(in, *proto, "sourceBuffers", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::object((*found)->buffers);
    });
    define_getter(in, *proto, "activeSourceBuffers", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::object((*found)->active);
    });
    define_getter(
        in, *proto, "duration",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            return js::Value::number((*found)->ready == MediaSourceObject::Closed ? std::nan("") : (*found)->duration);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            Realm::Internals& internals = internals_of(interp);
            std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            std::optional<double> const value = interp.to_number(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            if (std::isnan(*value) || *value < 0)
                return interp.throw_type_error("Failed to set the 'duration' property on 'MediaSource': the value is negative or not a number.");
            MediaSourceObject& source = **found;
            if (source.ready != MediaSourceObject::Open)
                return internals.throw_dom_exception("InvalidStateError", "The MediaSource is not open.");
            double highest = 0;
            for (SourceBufferObject const* buffer : source.buffers->items) {
                if (buffer->updating)
                    return internals.throw_dom_exception("InvalidStateError", "A SourceBuffer is still updating.");
                highest = std::max(highest, buffer->buffer.highest_end());
            }
            if (*value < highest)
                return internals.throw_dom_exception("InvalidStateError", "The duration cannot be set before the end of what is buffered; remove that first.");
            source.duration = *value;
            if (source.attached != nullptr) {
                set_duration(internals, *source.attached, *value);
                update_media(internals, *source.attached);
            }
            return js::Value::undefined();
        });

    js::define_method(interpreter, *proto, "addSourceBuffer", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        std::optional<std::string> const type = internals.to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        if (type->empty())
            return interp.throw_type_error("Failed to execute 'addSourceBuffer' on 'MediaSource': the type is empty.");
        if (type_support(*type) == 0)
            return internals.throw_dom_exception("NotSupportedError", "The type is not supported.");
        MediaSourceObject& source = **found;
        if (source.ready != MediaSourceObject::Open)
            return internals.throw_dom_exception("InvalidStateError", "The MediaSource is not open.");
        if (source.buffers->items.size() >= 16)
            return internals.throw_dom_exception("QuotaExceededError", "This MediaSource cannot hold another SourceBuffer.");
        js::Interpreter::Roots const roots(interp);
        auto* made = interp.heap().allocate<SourceBufferObject>(internals.prototype("SourceBuffer"));
        interp.root(js::Value::object(made));
        trace("addSourceBuffer " + *type);
        made->parent = &source;
        source.buffers->items.push_back(made);
        queue_fire(internals, source.buffers, "addsourcebuffer");
        return js::Value::object(made);
    });
    js::define_method(interpreter, *proto, "removeSourceBuffer", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        js::Value const given = js::argument(args, 0);
        auto* const buffer = given.is_object() ? dynamic_cast<SourceBufferObject*>(given.as_object()) : nullptr;
        if (buffer == nullptr)
            return interp.throw_type_error("Failed to execute 'removeSourceBuffer' on 'MediaSource': parameter 1 is not of type 'SourceBuffer'.");
        MediaSourceObject& source = **found;
        auto& items = source.buffers->items;
        auto const where = std::find(items.begin(), items.end(), buffer);
        if (where == items.end())
            return internals.throw_dom_exception("NotFoundError", "The SourceBuffer is not in this MediaSource.");
        if (buffer->updating) {
            ++buffer->generation;
            buffer->updating = false;
            queue_fire(internals, buffer, "abort");
            queue_fire(internals, buffer, "updateend");
        }
        auto& active = source.active->items;
        auto const active_where = std::find(active.begin(), active.end(), buffer);
        if (active_where != active.end()) {
            active.erase(active_where);
            queue_fire(internals, source.active, "removesourcebuffer");
        }
        items.erase(where);
        buffer->parent = nullptr;
        queue_fire(internals, source.buffers, "removesourcebuffer");
        if (source.attached != nullptr)
            update_media(internals, *source.attached);
        return js::Value::undefined();
    });
    js::define_method(interpreter, *proto, "endOfStream", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        std::string error;
        if (!js::argument(args, 0).is_undefined()) {
            std::optional<std::string> const given = internals.to_utf8(args[0]);
            if (!given)
                return std::nullopt;
            if (*given != "network" && *given != "decode")
                return interp.throw_type_error("Failed to execute 'endOfStream' on 'MediaSource': the error is not 'network' or 'decode'.");
            error = *given;
        }
        MediaSourceObject& source = **found;
        if (source.ready != MediaSourceObject::Open)
            return internals.throw_dom_exception("InvalidStateError", "The MediaSource is not open.");
        for (SourceBufferObject const* buffer : source.buffers->items) {
            if (buffer->updating)
                return internals.throw_dom_exception("InvalidStateError", "A SourceBuffer is still updating.");
        }
        end_of_stream(internals, source, error);
        return js::Value::undefined();
    });
    for (std::string_view const name : { "setLiveSeekableRange", "clearLiveSeekableRange" }) {
        js::define_method(interpreter, *proto, name, 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<MediaSourceObject*> const found = this_as<MediaSourceObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            if ((*found)->ready != MediaSourceObject::Open)
                return internals_of(interp).throw_dom_exception("InvalidStateError", "The MediaSource is not open.");
            return js::Value::undefined();
        });
    }
    static constexpr std::string_view source_events[] = { "sourceopen", "sourceended", "sourceclose" };
    define_event_handlers(in, *proto, source_events);
}

// --- HTMLMediaElement -----------------------------------------------------------------------

void install_media_element(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* ranges = define_interface(in, "TimeRanges", nullptr);
    define_getter(in, *ranges, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TimeRangesObject*> const found = this_as<TimeRangesObject>(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*found)->ranges.size()));
    });
    for (bool const start : { true, false }) {
        js::define_method(interpreter, *ranges, start ? "start" : "end", 1, [start](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<TimeRangesObject*> const found = this_as<TimeRangesObject>(interp, this_value);
            if (!found)
                return std::nullopt;
            std::optional<double> const index = interp.to_number(js::argument(args, 0));
            if (!index)
                return std::nullopt;
            TimeRanges const& held = (*found)->ranges;
            if (std::isnan(*index) || *index < 0 || *index >= static_cast<double>(held.size()))
                return internals_of(interp).throw_dom_exception("IndexSizeError", "The index is not in the ranges.");
            TimeRange const& range = held[static_cast<std::size_t>(*index)];
            return js::Value::number(start ? range.start : range.end);
        });
    }
    js::Object* media_error = define_interface(in, "MediaError", nullptr);
    js::Value const error_constructor = *interpreter.get(*interpreter.global(), interpreter.key("MediaError"));
    for (auto const& [name, value] : { std::pair { "MEDIA_ERR_ABORTED", 1 }, std::pair { "MEDIA_ERR_NETWORK", 2 }, std::pair { "MEDIA_ERR_DECODE", 3 },
             std::pair { "MEDIA_ERR_SRC_NOT_SUPPORTED", 4 } }) {
        media_error->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        error_constructor.as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }

    js::Object& element = *in.prototype("HTMLMediaElement");
    element_getter(in, element, "currentSrc", [](Realm::Internals& internals, dom::Element& e) -> Native {
        dom::Attr const* src = e.find_attribute("src");
        std::optional<net::Url> const url = src ? net::parse_url(src->value, &internals.base_url()) : std::nullopt;
        return internals.string(url ? url->serialize() : "");
    });
    element_getter(in, element, "paused", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::boolean(live_state_of(internals, e).paused); });
    element_getter(in, element, "seeking", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::boolean(live_state_of(internals, e).seeking); });
    element_getter(in, element, "ended", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::boolean(live_state_of(internals, e).ended_fired); });
    element_getter(in, element, "duration", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::number(live_state_of(internals, e).duration); });
    element_getter(in, element, "readyState", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::number(live_state_of(internals, e).ready_state); });
    element_getter(in, element, "networkState", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::number(live_state_of(internals, e).network_state); });
    element_getter(in, element, "error", [](Realm::Internals& internals, dom::Element& e) -> Native { return live_state_of(internals, e).error; });
    element_getter(in, element, "buffered", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_time_ranges(internals, element_buffered(live_state_of(internals, e))); });
    element_getter(in, element, "played", [](Realm::Internals& internals, dom::Element& e) -> Native {
        MediaStateObject& state = live_state_of(internals, e);
        update_media(internals, state);
        return make_time_ranges(internals, state.played);
    });
    element_getter(in, element, "seekable", [](Realm::Internals& internals, dom::Element& e) -> Native {
        MediaStateObject const& state = live_state_of(internals, e);
        TimeRanges seekable;
        // A file held whole can be sought anywhere within it.
        if (state.sound)
            return make_time_ranges(internals, sound_buffered(state));
        if (state.source != nullptr && !std::isnan(state.duration)) {
            if (std::isfinite(state.duration)) {
                seekable.push_back({ 0, state.duration });
            } else {
                TimeRanges const buffered = element_buffered(state);
                if (!buffered.empty())
                    seekable.push_back({ 0, buffered.back().end });
            }
        }
        return make_time_ranges(internals, std::move(seekable));
    });
    element_accessor(
        in, element, "currentTime",
        [](Realm::Internals& internals, dom::Element& e) -> Native {
            MediaStateObject& state = live_state_of(internals, e);
            update_media(internals, state);
            return js::Value::number(state.position);
        },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<double> const time = internals.interpreter.to_number(value);
            if (!time)
                return std::nullopt;
            if (!std::isfinite(*time))
                return internals.interpreter.throw_type_error("Failed to set the 'currentTime' property on 'HTMLMediaElement': the value is non-finite.");
            seek(internals, live_state_of(internals, e), *time);
            return js::Value::undefined();
        });

    struct Rate {
        char const* name;
        double MediaStateObject::* member;
        char const* event;
    };
    static constexpr Rate rates[] = { { "playbackRate", &MediaStateObject::playback_rate, "ratechange" },
        { "defaultPlaybackRate", &MediaStateObject::default_playback_rate, "ratechange" }, { "volume", &MediaStateObject::volume, "volumechange" } };
    for (Rate const& rate : rates) {
        auto const member = rate.member;
        std::string const event = rate.event;
        bool const is_volume = event == "volumechange";
        element_accessor(
            in, element, rate.name,
            [member](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::number(live_state_of(internals, e).*member); },
            [member, event, is_volume](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<double> const number = internals.interpreter.to_number(value);
                if (!number)
                    return std::nullopt;
                if (!std::isfinite(*number))
                    return internals.interpreter.throw_type_error("The provided double value is non-finite.");
                if (is_volume && (*number < 0 || *number > 1))
                    return internals.throw_dom_exception("IndexSizeError", "The volume must be between 0 and 1.");
                MediaStateObject& state = live_state_of(internals, e);
                update_media(internals, state); // the position so far moved at the old rate
                if (state.*member != *number) {
                    state.*member = *number;
                    if (is_volume && state.device)
                        state.device->set_volume(state.muted ? 0.0 : state.volume);
                    queue_element_event(internals, state, event);
                }
                update_media(internals, state);
                return js::Value::undefined();
            });
    }
    element_accessor(
        in, element, "muted", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::boolean(live_state_of(internals, e).muted); },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            MediaStateObject& state = live_state_of(internals, e);
            bool const muted = js::Interpreter::to_boolean(value);
            if (muted != state.muted) {
                state.muted = muted;
                if (state.device)
                    state.device->set_volume(muted ? 0.0 : state.volume);
                queue_element_event(internals, state, "volumechange");
            }
            return js::Value::undefined();
        });

    element_method(in, element, "load", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        run_load(internals, state_of(internals, e));
        return js::Value::undefined();
    });
    element_method(in, element, "pause", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        MediaStateObject& state = live_state_of(internals, e);
        update_media(internals, state);
        if (!state.paused) {
            state.paused = true;
            state.advancing = false;
            queue_element_event(internals, state, "timeupdate");
            queue_element_event(internals, state, "pause");
            settle_play_promises(internals, state, false, "AbortError", "The play() request was interrupted by a call to pause().");
            update_media(internals, state); // the speakers are held where they stand
        }
        return js::Value::undefined();
    });
    element_method(in, element, "play", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        js::Interpreter& interp = internals.interpreter;
        trace("play()");
        MediaStateObject& state = live_state_of(internals, e);
        if (!state.error.is_null() || state.network_state == NetworkNoSource || (state.source == nullptr && !state.sound))
            return rejected_promise(interp, dom_exception_value(internals, "NotSupportedError", "The element has no supported sources."));
        std::optional<js::PromiseCapability> const capability
            = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
        if (!capability)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        interp.root(capability->promise);
        state.play_promises.emplace_back(capability->resolve, capability->reject);
        update_media(internals, state);
        if (state.ended_fired)
            seek(internals, state, 0);
        if (state.paused) {
            state.paused = false;
            queue_element_event(internals, state, "play");
            if (state.ready_state <= HaveCurrentData) {
                queue_element_event(internals, state, "waiting");
            } else {
                queue_element_event(internals, state, "playing");
                settle_play_promises(internals, state, true);
            }
        } else if (state.ready_state >= HaveFutureData) {
            settle_play_promises(internals, state, true);
        }
        update_media(internals, state);
        return capability->promise;
    });
    element_method(in, element, "canPlayType", 1, [](Realm::Internals& internals, dom::Element&, Args args) -> Native {
        std::optional<std::string> const type = internals.to_utf8(js::argument(args, 0));
        if (!type)
            return std::nullopt;
        int const support = type_support(*type);
        trace("canPlayType " + *type + " -> " + std::to_string(support));
        return internals.string(support == 2 ? "probably" : support == 1 ? "maybe" : "");
    });
    for (auto const& [name, value] : { std::pair { "NETWORK_EMPTY", 0 }, std::pair { "NETWORK_IDLE", 1 }, std::pair { "NETWORK_LOADING", 2 },
             std::pair { "NETWORK_NO_SOURCE", 3 }, std::pair { "HAVE_NOTHING", 0 }, std::pair { "HAVE_METADATA", 1 },
             std::pair { "HAVE_CURRENT_DATA", 2 }, std::pair { "HAVE_FUTURE_DATA", 3 }, std::pair { "HAVE_ENOUGH_DATA", 4 } })
        element.put(interpreter.key(name), js::Value::number(value), js::Enumerable);

    js::Object& video = *in.prototype("HTMLVideoElement");
    element_getter(in, video, "videoWidth", [](Realm::Internals& internals, dom::Element& e) -> Native {
        MediaStateObject const& state = live_state_of(internals, e);
        return js::Value::number(state.ready_state >= HaveMetadata ? state.video_width : 0);
    });
    element_getter(in, video, "videoHeight", [](Realm::Internals& internals, dom::Element& e) -> Native {
        MediaStateObject const& state = live_state_of(internals, e);
        return js::Value::number(state.ready_state >= HaveMetadata ? state.video_height : 0);
    });
}

}

void install_media(Realm::Internals& in)
{
    install_media_element(in);
    if (playback_enabled()) {
        install_source_buffer(in);
        install_media_source(in);
    }
}

void media_src_changed(Realm::Internals& in, dom::Element& element)
{
    // Set or changed, the element loads again; removed, it does not (HTML
    // §4.8.11.2).
    if (is_media_element(element) && element.find_attribute("src") != nullptr)
        run_load(in, state_of(in, element));
}

bool register_media_source_url(Realm::Internals& in, js::Value const& value, std::string const& url)
{
    if (!value.is_object() || dynamic_cast<MediaSourceObject*>(value.as_object()) == nullptr)
        return false;
    in.media_source_urls[url] = value.as_object();
    return true;
}

}
