// Web Animations (web-animations-1, with the members of level 2 the browsers
// ship): Animation, AnimationEffect, KeyframeEffect, AnimationTimeline,
// DocumentTimeline, AnimationPlaybackEvent, element.animate() and the
// getAnimations() family, over the model in css/Animation.h — and the
// rendering update every frame of the agent runs (HTML §8.1.7.3): the
// documents' animations updated and their events sent, then the
// requestAnimationFrame callbacks with one timestamp.

#include "bindings/Internal.h"
#include "core/Ascii.h"
#include "css/Animation.h"
#include "css/Easing.h"
#include "css/Parser.h"
#include "css/StyleResolver.h"
#include "js/Runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

namespace sashfold::bindings {

namespace {

constexpr double frame_interval = 1000.0 / 60.0;

// The first time anything here wants frames, the grid starts from now: the
// first update falls on the slot after it, as a browser's first frame
// after a page asks for one falls on the next vertical blank.
void wish_for_frames(Agent& agent, double now)
{
    if (!std::isfinite(agent.last_rendering_update))
        agent.last_rendering_update = now;
}

// --- Script objects ---------------------------------------------------------------------------

// The promise of one of an animation's two, as a script has asked for it:
// made when first asked, settled by what the model says after that.
struct HeldPromise {
    js::Value promise;
    js::Value resolve;
    js::Value reject;
    bool settled = false;
    bool made() const { return !promise.is_undefined(); }
};

class AnimationObject final : public EventTargetObject, public css::AnimationClient {
public:
    AnimationObject(js::Object* prototype, Realm::Internals& owner, std::shared_ptr<css::Animation> held)
        : EventTargetObject(prototype)
        , animation(std::move(held))
        , realm(&owner)
    {
        animation->wrapper = this;
        animation->client = this;
    }
    ~AnimationObject() override
    {
        if (animation->wrapper == this)
            animation->wrapper = nullptr;
        if (animation->client == this)
            animation->client = nullptr;
    }

    std::shared_ptr<css::Animation> animation;
    Realm::Internals* realm;
    HeldPromise ready;
    HeldPromise finished;

    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        for (HeldPromise const* held : { &ready, &finished }) {
            tracer.visit(held->promise);
            tracer.visit(held->resolve);
            tracer.visit(held->reject);
        }
        if (animation->effect && animation->effect->wrapper)
            tracer.visit(animation->effect->wrapper);
        if (animation->timeline && animation->timeline->wrapper)
            tracer.visit(animation->timeline->wrapper);
    }

    HeldPromise& held(Promise which) { return which == Promise::Ready ? ready : finished; }

    void resolved(Promise which) override
    {
        HeldPromise& promise = held(which);
        if (!promise.made() || promise.settled)
            return;
        promise.settled = true;
        js::Value const argument[1] = { js::Value::object(this) };
        static_cast<void>(realm->interpreter.call(promise.resolve, js::Value::undefined(), argument));
    }
    void rejected(Promise which) override
    {
        HeldPromise& promise = held(which);
        if (promise.made() && !promise.settled) {
            js::Interpreter::Roots const roots(realm->interpreter);
            js::Value const error = realm->interpreter.root(dom_exception_value(*realm, "AbortError", "The user aborted a request."));
            if (promise.promise.is_object())
                static_cast<js::PromiseObject*>(promise.promise.as_object())->set_handled();
            js::Value const argument[1] = { error };
            static_cast<void>(realm->interpreter.call(promise.reject, js::Value::undefined(), argument));
        }
        promise = {};
    }
    void replaced(Promise which) override { held(which) = {}; }
};

class KeyframeEffectObject final : public js::Object {
public:
    KeyframeEffectObject(js::Object* prototype, std::shared_ptr<css::KeyframeEffect> held)
        : Object(prototype, Class::Host)
        , effect(std::move(held))
    {
        effect->wrapper = this;
    }
    ~KeyframeEffectObject() override
    {
        if (effect->wrapper == this)
            effect->wrapper = nullptr;
    }
    std::shared_ptr<css::KeyframeEffect> effect;
};

class TimelineObject final : public js::Object {
public:
    TimelineObject(js::Object* prototype, std::shared_ptr<css::AnimationTimeline> held)
        : Object(prototype, Class::Host)
        , timeline(std::move(held))
    {
        timeline->wrapper = this;
    }
    ~TimelineObject() override
    {
        if (timeline->wrapper == this)
            timeline->wrapper = nullptr;
    }
    std::shared_ptr<css::AnimationTimeline> timeline;
};

// A document's animations; for the realm's own document, set up the first
// time the realm asks (the model may have made them already, for an effect
// targeting one of its elements): the clock its timeline reads, and the
// hooks the model calls. Any other document's timeline stays inactive.
css::DocumentAnimations& animations_of(Realm::Internals& in, dom::Document& document)
{
    css::DocumentAnimations& animations = css::DocumentAnimations::of(document);
    if (&document == in.document && !animations.on_change) {
        // The time of the last rendering update, or the time now when
        // there has been none: what a timeline reads between frames.
        double const frame = std::isfinite(in.agent.last_rendering_update) ? in.agent.last_rendering_update : in.now();
        animations.now = frame - in.time_origin;
        Realm::Internals* const owner = &in;
        animations.queue_microtask = [owner](std::function<void()> run) {
            js::NativeFunction* const job = owner->interpreter.new_native("", 0,
                [run = std::move(run)](js::Interpreter&, js::Value const&, std::span<js::Value const>) -> std::optional<js::Value> {
                    run();
                    return js::Value::undefined();
                });
            owner->interpreter.enqueue_microtask(js::Value::object(job), {});
        };
        animations.on_change = [owner] { ++owner->mutations; };
        wish_for_frames(in.agent, in.now());
        in.agent.animated_realms.emplace_back(in.alive, &in);
    }
    return animations;
}

js::Value wrap_animation(Realm::Internals& in, std::shared_ptr<css::Animation> const& animation)
{
    if (animation->wrapper)
        return js::Value::object(animation->wrapper);
    char const* const interface = animation->kind == css::Animation::Kind::CssAnimation ? "CSSAnimation"
        : animation->kind == css::Animation::Kind::CssTransition                      ? "CSSTransition"
                                                                                       : "Animation";
    js::Object* proto = in.prototype(interface);
    if (!proto)
        proto = in.prototype("Animation");
    auto* const made = in.interpreter.heap().allocate<AnimationObject>(proto, in, animation);
    return js::Value::object(made);
}

js::Value wrap_effect(Realm::Internals& in, std::shared_ptr<css::KeyframeEffect> const& effect)
{
    if (effect->wrapper)
        return js::Value::object(effect->wrapper);
    return js::Value::object(in.interpreter.heap().allocate<KeyframeEffectObject>(in.prototype("KeyframeEffect"), effect));
}

js::Value wrap_timeline(Realm::Internals& in, std::shared_ptr<css::AnimationTimeline> const& timeline)
{
    if (!timeline)
        return js::Value::null();
    if (timeline->wrapper)
        return js::Value::object(timeline->wrapper);
    return js::Value::object(in.interpreter.heap().allocate<TimelineObject>(in.prototype("DocumentTimeline"), timeline));
}

std::optional<AnimationObject*> this_animation(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<AnimationObject*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

std::optional<KeyframeEffectObject*> this_effect(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<KeyframeEffectObject*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

js::Value time_value(std::optional<double> time) { return time ? js::Value::number(*time) : js::Value::null(); }

Native refuse(Realm::Internals& in, css::Animation::Refusal const& refusal)
{
    if (refusal.name == "TypeError")
        return in.interpreter.throw_type_error(refusal.message);
    return in.throw_dom_exception(refusal.name, refusal.message);
}

// --- Property names ---------------------------------------------------------------------------

// A property's CSS name as an IDL attribute names it (CSSOM §6.7.1): the
// dashes taken out and the letter after each raised; float and offset are
// cssFloat and cssOffset, since a keyframe's own `offset` takes the name.
std::string idl_name(std::string_view css)
{
    if (css.starts_with("--"))
        return std::string(css);
    if (css == "float")
        return "cssFloat";
    if (css == "offset")
        return "cssOffset";
    std::string_view rest = css;
    std::string out;
    if (rest.starts_with("-webkit-")) {
        out = "webkit";
        rest.remove_prefix(7);
    }
    bool raise = false;
    for (char const c : rest) {
        if (c == '-') {
            raise = true;
            continue;
        }
        out += raise ? (c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c) : c;
        raise = false;
    }
    return out;
}

// The other way: the CSS name an IDL name stands for, if any. Names with
// a dash or a capital first letter are no IDL attribute's.
std::optional<std::string> css_name(std::string_view idl)
{
    if (idl.starts_with("--"))
        return std::string(idl);
    if (idl == "cssFloat")
        return std::string("float");
    if (idl == "cssOffset")
        return std::string("offset");
    if (idl.empty() || idl.find('-') != std::string_view::npos || (idl[0] >= 'A' && idl[0] <= 'Z'))
        return std::nullopt;
    if (idl == "float" || idl == "offset")
        return std::nullopt;
    std::string out;
    std::string_view rest = idl;
    if (rest.starts_with("webkit") && rest.size() > 6 && rest[6] >= 'A' && rest[6] <= 'Z') {
        out = "-webkit";
        rest.remove_prefix(6);
    }
    for (char const c : rest) {
        if (c >= 'A' && c <= 'Z') {
            out += '-';
            out += static_cast<char>(c - 'A' + 'a');
        } else {
            out += c;
        }
    }
    return out;
}

// Whether a CSS name is a property keyframes may animate: one the cascade
// knows (it takes the CSS-wide keywords, as every real one does) and not
// one of those the specifications call not animatable.
bool animatable_name(std::string const& name)
{
    if (name.starts_with("--"))
        return name.size() > 2;
    if (css::is_not_animatable(name))
        return false;
    thread_local std::unordered_map<std::string, bool> known;
    auto const found = known.find(name);
    if (found != known.end())
        return found->second;
    static std::vector<css::ComponentValue> const inherit = css::parse_component_value_list("inherit");
    bool const yes = css::declaration_is_supported(name, inherit);
    known.emplace(name, yes);
    return yes;
}

// --- Dictionaries -----------------------------------------------------------------------------

// What a timing dictionary (EffectTiming, OptionalEffectTiming) held,
// converted member by member in the order WebIDL reads them.
struct TimingInput {
    std::optional<double> delay;
    std::optional<css::PlaybackDirection> direction;
    std::optional<std::variant<double, std::string>> duration;
    std::optional<std::string> easing;
    std::optional<double> end_delay;
    std::optional<css::FillMode> fill;
    std::optional<double> iteration_start;
    std::optional<double> iterations;
};

std::optional<double> restricted_number(js::Interpreter& interp, js::Value const& value, std::string_view what)
{
    std::optional<double> const number = interp.to_number(value);
    if (!number)
        return std::nullopt;
    if (!std::isfinite(*number)) {
        interp.throw_type_error("Failed to read the '" + std::string(what) + "' property from 'EffectTiming': The provided double value is non-finite.");
        return std::nullopt;
    }
    return number;
}

// One enumeration member: its value among `names`, or a TypeError.
template<typename E>
std::optional<E> enum_member(Realm::Internals& in, js::Value const& value, std::initializer_list<std::pair<std::string_view, E>> names,
    std::string_view type)
{
    std::optional<std::string> const text = in.to_utf8(value);
    if (!text)
        return std::nullopt;
    for (auto const& [name, which] : names) {
        if (*text == name)
            return which;
    }
    in.interpreter.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type " + std::string(type) + ".");
    return std::nullopt;
}

std::optional<css::FillMode> fill_mode(Realm::Internals& in, js::Value const& value)
{
    return enum_member<css::FillMode>(in, value,
        { { "none", css::FillMode::None }, { "forwards", css::FillMode::Forwards }, { "backwards", css::FillMode::Backwards },
            { "both", css::FillMode::Both }, { "auto", css::FillMode::Auto } },
        "FillMode");
}

std::optional<css::PlaybackDirection> playback_direction(Realm::Internals& in, js::Value const& value)
{
    return enum_member<css::PlaybackDirection>(in, value,
        { { "normal", css::PlaybackDirection::Normal }, { "reverse", css::PlaybackDirection::Reverse },
            { "alternate", css::PlaybackDirection::Alternate }, { "alternate-reverse", css::PlaybackDirection::AlternateReverse } },
        "PlaybackDirection");
}

std::optional<css::CompositeOperation> composite_operation(Realm::Internals& in, js::Value const& value)
{
    return enum_member<css::CompositeOperation>(in, value,
        { { "replace", css::CompositeOperation::Replace }, { "add", css::CompositeOperation::Add },
            { "accumulate", css::CompositeOperation::Accumulate } },
        "CompositeOperation");
}

// CompositeOperationOrAuto: auto is no operation of the keyframe's own.
std::optional<std::optional<css::CompositeOperation>> composite_or_auto(Realm::Internals& in, js::Value const& value)
{
    std::optional<std::string> const text = in.to_utf8(value);
    if (!text)
        return std::nullopt;
    if (*text == "auto")
        return std::optional<css::CompositeOperation> {};
    if (*text == "replace")
        return std::optional<css::CompositeOperation> { css::CompositeOperation::Replace };
    if (*text == "add")
        return std::optional<css::CompositeOperation> { css::CompositeOperation::Add };
    if (*text == "accumulate")
        return std::optional<css::CompositeOperation> { css::CompositeOperation::Accumulate };
    in.interpreter.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type CompositeOperationOrAuto.");
    return std::nullopt;
}

// Reads the timing members of a dictionary, in WebIDL's order. False
// when a conversion threw.
bool read_timing(Realm::Internals& in, js::Value const& dictionary, TimingInput& out)
{
    js::Interpreter& interp = in.interpreter;
    if (!dictionary.is_object())
        return true;
    auto const member = [&](std::string_view name) -> std::optional<js::Value> { return interp.get(dictionary, name); };
    std::optional<js::Value> value = member("delay");
    if (!value)
        return false;
    if (!value->is_undefined()) {
        if (!(out.delay = restricted_number(interp, *value, "delay")))
            return false;
    }
    if (!(value = member("direction")))
        return false;
    if (!value->is_undefined()) {
        if (!(out.direction = playback_direction(in, *value)))
            return false;
    }
    if (!(value = member("duration")))
        return false;
    if (!value->is_undefined()) {
        if (value->is_number()) {
            out.duration = value->as_number();
        } else {
            std::optional<std::string> const text = in.to_utf8(*value);
            if (!text)
                return false;
            out.duration = *text;
        }
    }
    if (!(value = member("easing")))
        return false;
    if (!value->is_undefined()) {
        if (!(out.easing = in.to_utf8(*value)))
            return false;
    }
    if (!(value = member("endDelay")))
        return false;
    if (!value->is_undefined()) {
        if (!(out.end_delay = restricted_number(interp, *value, "endDelay")))
            return false;
    }
    if (!(value = member("fill")))
        return false;
    if (!value->is_undefined()) {
        if (!(out.fill = fill_mode(in, *value)))
            return false;
    }
    if (!(value = member("iterationStart")))
        return false;
    if (!value->is_undefined()) {
        if (!(out.iteration_start = restricted_number(interp, *value, "iterationStart")))
            return false;
    }
    if (!(value = member("iterations")))
        return false;
    if (!value->is_undefined()) {
        if (!(out.iterations = interp.to_number(*value)))
            return false;
    }
    return true;
}

// web-animations-1 §6.5.2 "update the timing properties": checked first,
// then written. False when it threw.
bool apply_timing(Realm::Internals& in, TimingInput const& input, css::EffectTiming& timing)
{
    js::Interpreter& interp = in.interpreter;
    if (input.iteration_start && *input.iteration_start < 0) {
        interp.throw_type_error("Failed to execute 'updateTiming' on 'AnimationEffect': iterationStart must be non-negative.");
        return false;
    }
    if (input.iterations && (std::isnan(*input.iterations) || *input.iterations < 0)) {
        interp.throw_type_error("Failed to execute 'updateTiming' on 'AnimationEffect': iterationCount must be non-negative.");
        return false;
    }
    std::optional<double> duration_number;
    bool duration_auto = false;
    if (input.duration) {
        if (std::holds_alternative<double>(*input.duration)) {
            double const duration = std::get<double>(*input.duration);
            if (std::isnan(duration) || duration < 0) {
                interp.throw_type_error("Failed to execute 'updateTiming' on 'AnimationEffect': duration must be non-negative or auto.");
                return false;
            }
            duration_number = duration;
        } else if (std::get<std::string>(*input.duration) == "auto") {
            duration_auto = true;
        } else {
            interp.throw_type_error("Failed to execute 'updateTiming' on 'AnimationEffect': duration must be non-negative or auto.");
            return false;
        }
    }
    std::optional<css::Easing> easing;
    if (input.easing) {
        easing = css::parse_easing_text(*input.easing);
        if (!easing) {
            interp.throw_type_error("Failed to execute 'updateTiming' on 'AnimationEffect': '" + *input.easing + "' is not a valid value for easing");
            return false;
        }
    }
    if (input.delay)
        timing.delay = *input.delay;
    if (input.direction)
        timing.direction = *input.direction;
    if (duration_number)
        timing.duration = duration_number;
    else if (duration_auto)
        timing.duration.reset();
    if (easing)
        timing.easing = *easing;
    if (input.end_delay)
        timing.end_delay = *input.end_delay;
    if (input.fill)
        timing.fill = *input.fill;
    if (input.iteration_start)
        timing.iteration_start = *input.iteration_start;
    if (input.iterations)
        timing.iterations = *input.iterations;
    return true;
}

// A pseudo-element selector as an effect's target takes one (css-pseudo-4):
// its kind and its name as the API gives it back, the legacy one-colon
// spellings and the case of the name normalized. Any pseudo-element the
// selectors name is taken; only ::before and ::after are boxes here.
struct PseudoTarget {
    css::PseudoElement kind = css::PseudoElement::None;
    std::string name;
};

std::optional<PseudoTarget> parse_pseudo(std::string_view text)
{
    std::string_view rest;
    bool legacy = false;
    if (text.starts_with("::")) {
        rest = text.substr(2);
    } else if (text.starts_with(":")) {
        rest = text.substr(1);
        legacy = true;
    } else {
        return std::nullopt;
    }
    std::size_t const open = rest.find('(');
    std::string name;
    for (char const c : rest.substr(0, open))
        name += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    if (legacy && name != "before" && name != "after" && name != "first-line" && name != "first-letter")
        return std::nullopt;
    static constexpr std::string_view plain[] = { "before", "after", "marker", "first-line", "first-letter",
        "placeholder", "selection", "backdrop", "file-selector-button", "spelling-error", "grammar-error",
        "target-text", "cue", "cue-region", "details-content", "search-text", "checkmark", "picker-icon", "column",
        "scroll-marker", "scroll-marker-group", "scroll-button", "view-transition", "view-transition-group",
        "view-transition-image-pair", "view-transition-old", "view-transition-new" };
    static constexpr std::string_view functional[] = { "part", "slotted", "highlight", "picker", "scroll-button",
        "view-transition-group", "view-transition-image-pair", "view-transition-old", "view-transition-new" };
    PseudoTarget target;
    if (open == std::string_view::npos) {
        if (std::find(std::begin(plain), std::end(plain), name) == std::end(plain))
            return std::nullopt;
        target.name = "::" + name;
    } else {
        if (!rest.ends_with(")") || std::find(std::begin(functional), std::end(functional), name) == std::end(functional))
            return std::nullopt;
        std::string_view const argument = rest.substr(open + 1, rest.size() - open - 2);
        if (argument.empty())
            return std::nullopt;
        target.name = "::" + name + "(" + std::string(argument) + ")";
    }
    target.kind = name == "before" ? css::PseudoElement::Before
        : name == "after"         ? css::PseudoElement::After
        : name == "marker"        ? css::PseudoElement::Marker
                                  : css::PseudoElement::Other;
    return target;
}

std::string_view pseudo_text(css::PseudoElement pseudo)
{
    switch (pseudo) {
    case css::PseudoElement::Before: return "::before";
    case css::PseudoElement::After: return "::after";
    case css::PseudoElement::Marker: return "::marker";
    case css::PseudoElement::Other:
    case css::PseudoElement::None: break;
    }
    return {};
}

// --- Keyframes ---------------------------------------------------------------------------------

// A keyframe as the processing reads it, before the offsets and the
// easings are checked: the offset, the easing's text, the composite, and
// the property values as text.
struct ReadKeyframe {
    std::optional<double> offset;
    double computed = 0; // the property-indexed form's spacing
    std::string easing = "linear";
    std::optional<css::CompositeOperation> composite;
    std::vector<std::pair<std::string, std::string>> values; // CSS name, value text
};

// An argument that may be one value or a sequence of them (the
// property-indexed form): a sequence when it is an object with an
// iterator.
std::optional<std::vector<js::Value>> one_or_sequence(js::Interpreter& interp, js::Value const& value, bool& was_sequence)
{
    was_sequence = false;
    if (value.is_object()) {
        std::optional<js::Value> const method = interp.get_method(value, js::PropertyKey::symbol(interp.atoms().symbol_iterator));
        if (!method)
            return std::nullopt;
        if (!method->is_undefined()) {
            was_sequence = true;
            js::Interpreter::Roots const roots(interp);
            std::optional<js::IteratorRecord> record = interp.get_iterator_from_method(value, *method);
            if (!record)
                return std::nullopt;
            interp.root(record->iterator);
            interp.root(record->next_method);
            std::vector<js::Value> items;
            for (;;) {
                js::Value item;
                std::optional<bool> const step = interp.iterator_step(*record, item);
                if (!step)
                    return std::nullopt;
                if (!*step)
                    break;
                interp.root(item);
                items.push_back(item);
            }
            return items;
        }
    }
    return std::vector<js::Value> { value };
}

// §5.10.3 "process a keyframe-like object": the base members first, in
// their order, then the property values, read in their names' order.
// `lists` is the property-indexed form, where each may be a sequence.
struct KeyframeLike {
    std::vector<std::optional<double>> offsets;
    bool offsets_list = false;
    std::vector<std::string> easings;
    std::vector<std::optional<css::CompositeOperation>> composites;
    std::vector<std::pair<std::string, std::vector<std::string>>> properties; // CSS name, values
};

std::optional<KeyframeLike> process_keyframe_like(Realm::Internals& in, js::Value const& input, bool lists)
{
    js::Interpreter& interp = in.interpreter;
    KeyframeLike out;
    if (input.is_undefined() || input.is_null())
        return out;
    if (!input.is_object()) {
        interp.throw_type_error("Failed to execute 'animate' on 'Element': The provided value is not of type 'Keyframe'.");
        return std::nullopt;
    }
    js::Interpreter::Roots const roots(interp);
    // composite
    std::optional<js::Value> value = interp.get(input, "composite");
    if (!value)
        return std::nullopt;
    if (!value->is_undefined()) {
        bool sequence = false;
        std::optional<std::vector<js::Value>> const items = lists ? one_or_sequence(interp, *value, sequence) : std::vector<js::Value> { *value };
        if (!items)
            return std::nullopt;
        for (js::Value const& item : *items) {
            std::optional<std::optional<css::CompositeOperation>> const composite = composite_or_auto(in, item);
            if (!composite)
                return std::nullopt;
            out.composites.push_back(*composite);
        }
    } else if (!lists) {
        out.composites.push_back(std::nullopt);
    }
    // easing
    if (!(value = interp.get(input, "easing")))
        return std::nullopt;
    if (!value->is_undefined()) {
        bool sequence = false;
        std::optional<std::vector<js::Value>> const items = lists ? one_or_sequence(interp, *value, sequence) : std::vector<js::Value> { *value };
        if (!items)
            return std::nullopt;
        for (js::Value const& item : *items) {
            std::optional<std::string> text = in.to_utf8(item);
            if (!text)
                return std::nullopt;
            out.easings.push_back(std::move(*text));
        }
    } else {
        out.easings.push_back("linear");
    }
    // offset
    if (!(value = interp.get(input, "offset")))
        return std::nullopt;
    if (!value->is_undefined()) {
        bool sequence = false;
        std::optional<std::vector<js::Value>> const items = lists ? one_or_sequence(interp, *value, sequence) : std::vector<js::Value> { *value };
        if (!items)
            return std::nullopt;
        out.offsets_list = sequence;
        for (js::Value const& item : *items) {
            if (item.is_null() || item.is_undefined()) {
                out.offsets.push_back(std::nullopt);
                continue;
            }
            std::optional<double> const number = interp.to_number(item);
            if (!number)
                return std::nullopt;
            if (!std::isfinite(*number)) {
                interp.throw_type_error("Failed to execute 'animate' on 'Element': Non numeric offset provided");
                return std::nullopt;
            }
            out.offsets.push_back(*number);
        }
    } else if (!lists) {
        out.offsets.push_back(std::nullopt);
    }
    // The property values, by name.
    js::Object& object = *input.as_object();
    std::optional<std::vector<js::PropertyKey>> const keys = interp.own_keys(object);
    if (!keys)
        return std::nullopt;
    std::vector<std::pair<std::string, std::string>> names; // IDL name, CSS name
    for (js::PropertyKey const& key : *keys) {
        if (!key.is_string())
            continue;
        std::optional<std::optional<js::PropertyDescriptor>> const descriptor = interp.get_own_property(object, key);
        if (!descriptor)
            return std::nullopt;
        if (!*descriptor || !(*descriptor)->enumerable.value_or(false))
            continue;
        if (!key.is_atom())
            continue; // an index names no property
        std::string const idl = key.as_atom()->to_utf8();
        std::optional<std::string> const css = css_name(idl);
        if (!css || !animatable_name(*css))
            continue;
        names.emplace_back(idl, *css);
    }
    std::sort(names.begin(), names.end());
    for (auto const& [idl, css] : names) {
        std::optional<js::Value> const raw = interp.get(input, idl);
        if (!raw)
            return std::nullopt;
        std::vector<std::string> texts;
        bool sequence = false;
        std::optional<std::vector<js::Value>> const items = lists ? one_or_sequence(interp, *raw, sequence) : std::vector<js::Value> { *raw };
        if (!items)
            return std::nullopt;
        for (js::Value const& item : *items) {
            std::optional<std::string> text = in.to_utf8(item);
            if (!text)
                return std::nullopt;
            texts.push_back(std::move(*text));
        }
        out.properties.emplace_back(css, std::move(texts));
    }
    return out;
}

std::string trimmed_text(std::string_view text)
{
    while (!text.empty() && is_tokenizer_whitespace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && is_tokenizer_whitespace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return std::string(text);
}

// §5.10.3 "process a keyframes argument" to the model's keyframes; null
// (with the exception thrown) when the argument is refused.
std::optional<std::vector<css::Keyframe>> process_keyframes(Realm::Internals& in, js::Value const& argument)
{
    js::Interpreter& interp = in.interpreter;
    std::vector<ReadKeyframe> processed;
    std::vector<std::string> unused_easings;
    if (argument.is_null() || argument.is_undefined())
        return std::vector<css::Keyframe> {};
    if (!argument.is_object()) {
        interp.throw_type_error("Failed to execute 'animate' on 'Element': The provided value is not of type '(sequence<object> or object)'.");
        return std::nullopt;
    }
    js::Interpreter::Roots const roots(interp);
    std::optional<js::Value> const method = interp.get_method(argument, js::PropertyKey::symbol(interp.atoms().symbol_iterator));
    if (!method)
        return std::nullopt;
    if (!method->is_undefined()) {
        // A sequence of keyframes.
        std::optional<js::IteratorRecord> record = interp.get_iterator_from_method(argument, *method);
        if (!record)
            return std::nullopt;
        interp.root(record->iterator);
        interp.root(record->next_method);
        for (;;) {
            js::Value item;
            std::optional<bool> const step = interp.iterator_step(*record, item);
            if (!step)
                return std::nullopt;
            if (!*step)
                break;
            interp.root(item);
            if (!item.is_object() && !item.is_null() && !item.is_undefined()) {
                interp.throw_type_error("Failed to execute 'animate' on 'Element': Keyframes must be objects.");
                static_cast<void>(interp.iterator_close(*record, true));
                return std::nullopt;
            }
            std::optional<KeyframeLike> const like = process_keyframe_like(in, item, false);
            if (!like)
                return std::nullopt;
            ReadKeyframe keyframe;
            keyframe.offset = like->offsets.empty() ? std::nullopt : like->offsets.front();
            keyframe.easing = like->easings.empty() ? "linear" : like->easings.front();
            keyframe.composite = like->composites.empty() ? std::nullopt : like->composites.front();
            for (auto const& [name, values] : like->properties)
                keyframe.values.emplace_back(name, values.empty() ? std::string() : values.front());
            processed.push_back(std::move(keyframe));
        }
    } else {
        // Property-indexed: each property's values spaced along the
        // iteration, the keyframes at one offset merged.
        std::optional<KeyframeLike> const like = process_keyframe_like(in, argument, true);
        if (!like)
            return std::nullopt;
        std::vector<ReadKeyframe> spread;
        for (auto const& [name, values] : like->properties) {
            std::size_t const n = values.size();
            for (std::size_t i = 0; i < n; ++i) {
                ReadKeyframe keyframe;
                keyframe.computed = n == 1 ? 1.0 : static_cast<double>(i) / static_cast<double>(n - 1);
                keyframe.values.emplace_back(name, values[i]);
                spread.push_back(std::move(keyframe));
            }
        }
        std::stable_sort(spread.begin(), spread.end(), [](ReadKeyframe const& a, ReadKeyframe const& b) { return a.computed < b.computed; });
        for (ReadKeyframe& keyframe : spread) {
            if (!processed.empty() && processed.back().computed == keyframe.computed) {
                for (auto& value : keyframe.values)
                    processed.back().values.push_back(std::move(value));
                continue;
            }
            processed.push_back(std::move(keyframe));
        }
        for (std::size_t i = 0; i < like->offsets.size() && i < processed.size(); ++i)
            processed[i].offset = like->offsets[i];
        std::vector<std::string> easings = like->easings;
        if (easings.empty())
            easings.push_back("linear");
        for (std::size_t i = 0; i < processed.size(); ++i)
            processed[i].easing = easings[i % easings.size()];
        for (std::size_t i = processed.size(); i < easings.size(); ++i)
            unused_easings.push_back(easings[i]);
        if (!like->composites.empty()) {
            for (std::size_t i = 0; i < processed.size(); ++i)
                processed[i].composite = like->composites[i % like->composites.size()];
        }
    }
    // The offsets in order, and within [0, 1].
    std::optional<double> previous;
    for (ReadKeyframe const& keyframe : processed) {
        if (!keyframe.offset)
            continue;
        if (previous && *keyframe.offset < *previous) {
            interp.throw_type_error("Failed to execute 'animate' on 'Element': Offsets must be monotonically non-decreasing.");
            return std::nullopt;
        }
        previous = keyframe.offset;
    }
    for (ReadKeyframe const& keyframe : processed) {
        if (keyframe.offset && (*keyframe.offset < 0 || *keyframe.offset > 1)) {
            interp.throw_type_error("Failed to execute 'animate' on 'Element': Offsets must be null or in the range [0,1].");
            return std::nullopt;
        }
    }
    // The easings, each a timing function; the property values parsed, a
    // value the property does not take dropped.
    std::vector<css::Keyframe> keyframes;
    for (ReadKeyframe const& read : processed) {
        css::Keyframe keyframe;
        keyframe.offset = read.offset;
        keyframe.composite = read.composite;
        std::optional<css::Easing> const easing = css::parse_easing_text(read.easing);
        if (!easing) {
            interp.throw_type_error("Failed to execute 'animate' on 'Element': '" + read.easing + "' is not a valid value for easing");
            return std::nullopt;
        }
        keyframe.easing = *easing;
        for (auto const& [name, text] : read.values) {
            std::vector<css::ComponentValue> value = css::parse_component_value_list(text);
            if (!name.starts_with("--") && !css::declaration_is_supported(name, value))
                continue;
            keyframe.values.push_back({ name, std::move(value), trimmed_text(text), std::nullopt });
        }
        keyframes.push_back(std::move(keyframe));
    }
    for (std::string const& easing : unused_easings) {
        if (!css::parse_easing_text(easing)) {
            interp.throw_type_error("Failed to execute 'animate' on 'Element': '" + easing + "' is not a valid value for easing");
            return std::nullopt;
        }
    }
    return keyframes;
}

// A plain object with these properties, in this order.
js::Object* plain_object(Realm::Internals& in, std::vector<std::pair<std::string_view, js::Value>> const& properties)
{
    js::Heap::NoCollect const guard(in.interpreter.heap());
    js::Object* object = in.interpreter.new_object();
    for (auto const& [name, value] : properties)
        object->put(in.interpreter.key(name), value);
    return object;
}

std::string_view fill_text(css::FillMode fill)
{
    switch (fill) {
    case css::FillMode::None: return "none";
    case css::FillMode::Forwards: return "forwards";
    case css::FillMode::Backwards: return "backwards";
    case css::FillMode::Both: return "both";
    case css::FillMode::Auto: return "auto";
    }
    return "auto";
}

std::string_view direction_text(css::PlaybackDirection direction)
{
    switch (direction) {
    case css::PlaybackDirection::Normal: return "normal";
    case css::PlaybackDirection::Reverse: return "reverse";
    case css::PlaybackDirection::Alternate: return "alternate";
    case css::PlaybackDirection::AlternateReverse: return "alternate-reverse";
    }
    return "normal";
}

std::string_view composite_text(css::CompositeOperation operation)
{
    switch (operation) {
    case css::CompositeOperation::Replace: return "replace";
    case css::CompositeOperation::Add: return "add";
    case css::CompositeOperation::Accumulate: return "accumulate";
    }
    return "replace";
}

// EffectTiming as getTiming() gives it, members in the dictionary's order;
// with `computed`, ComputedEffectTiming's after them.
js::Value timing_object(Realm::Internals& in, css::KeyframeEffect const& effect, bool computed)
{
    css::EffectTiming const& timing = effect.timing;
    js::Interpreter::Roots const roots(in.interpreter);
    std::vector<std::pair<std::string_view, js::Value>> members;
    members.emplace_back("delay", js::Value::number(timing.delay));
    members.emplace_back("direction", in.interpreter.root(in.string(direction_text(timing.direction))));
    if (computed || timing.duration)
        members.emplace_back("duration", js::Value::number(timing.duration.value_or(0)));
    else
        members.emplace_back("duration", in.interpreter.root(in.string("auto")));
    members.emplace_back("easing", in.interpreter.root(in.string(timing.easing.serialize())));
    members.emplace_back("endDelay", js::Value::number(timing.end_delay));
    css::ComputedTiming const result = effect.computed_timing();
    members.emplace_back("fill", in.interpreter.root(in.string(fill_text(computed ? result.fill : timing.fill))));
    members.emplace_back("iterationStart", js::Value::number(timing.iteration_start));
    members.emplace_back("iterations", js::Value::number(timing.iterations));
    if (computed) {
        members.emplace_back("activeDuration", js::Value::number(result.active_duration));
        members.emplace_back("currentIteration", time_value(result.current_iteration));
        members.emplace_back("endTime", js::Value::number(result.end_time));
        members.emplace_back("localTime", time_value(result.local_time));
        members.emplace_back("progress", time_value(result.progress));
        // web-animations-2: where the effect begins on its animation's time,
        // which outside a group effect is zero.
        members.emplace_back("startTime", js::Value::number(0));
    }
    return js::Value::object(plain_object(in, members));
}

// The effect's keyframes as getKeyframes() gives them (§6.6.2).
js::Value keyframes_array(Realm::Internals& in, css::KeyframeEffect const& effect)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    js::ArrayObject* const list = interp.new_array();
    interp.root(js::Value::object(list));
    for (css::Keyframe const& keyframe : effect.keyframes()) {
        std::vector<std::pair<std::string_view, js::Value>> members;
        members.emplace_back("offset", time_value(keyframe.offset));
        members.emplace_back("computedOffset", js::Value::number(keyframe.computed_offset));
        members.emplace_back("easing", interp.root(in.string(keyframe.easing.serialize())));
        members.emplace_back("composite",
            interp.root(in.string(keyframe.composite ? composite_text(*keyframe.composite) : std::string_view("auto"))));
        std::vector<std::string> names;
        for (css::Keyframe::Value const& value : keyframe.values)
            names.push_back(idl_name(value.property));
        for (std::size_t i = 0; i < keyframe.values.size(); ++i)
            members.emplace_back(names[i], interp.root(in.string(keyframe.values[i].text)));
        list->push(js::Value::object(plain_object(in, members)));
    }
    return js::Value::object(list);
}

// What a KeyframeEffectOptions (or a bare duration) gives an effect, read
// in WebIDL's order; null when a conversion threw.
struct EffectOptions {
    TimingInput timing;
    std::optional<css::CompositeOperation> composite;
    std::optional<css::IterationComposite> iteration_composite;
    std::optional<std::optional<std::string>> pseudo;
    // KeyframeAnimationOptions, for animate().
    std::string id;
    bool timeline_given = false;
    js::Value timeline;
};

// scroll-animations-1 §4.2: a timeline range name, and the units a range
// offset may not be in (it has no font to measure them against).
bool is_range_name(std::string_view name)
{
    return name == "cover" || name == "contain" || name == "entry" || name == "exit" || name == "entry-crossing"
        || name == "exit-crossing";
}

bool font_relative_unit(std::string_view unit)
{
    for (std::string_view const relative : { "em", "rem", "ex", "rex", "cap", "rcap", "ch", "rch", "ic", "ric", "lh", "rlh" }) {
        if (ascii_ci_equals(unit, relative))
            return true;
    }
    return false;
}

bool mentions_font_relative(std::vector<css::ComponentValue> const& values)
{
    for (css::ComponentValue const& value : values) {
        if (value.is_token(css::Token::Type::Dimension) && font_relative_unit(value.token().unit))
            return true;
        if (value.is_function() && mentions_font_relative(value.function().values))
            return true;
        if (value.is_block() && mentions_font_relative(value.block().values))
            return true;
    }
    return false;
}

// A rangeStart or rangeEnd: unset; `normal`, or a range name and/or a
// length-percentage, as text; a TimelineRangeOffset dictionary; or a
// numeric value. False when it is none of those; an exception a getter
// threw is left pending.
bool range_is_valid(Realm::Internals& in, js::Value const& range)
{
    js::Interpreter& interp = in.interpreter;
    if (range.is_undefined())
        return true;
    auto const numeric_ok = [&](js::Value const& value) -> std::optional<bool> {
        std::optional<js::Value> const unit = interp.get(value, "unit");
        if (!unit)
            return std::nullopt;
        if (unit->is_undefined())
            return true;
        std::optional<std::string> const text = in.to_utf8(*unit);
        if (!text)
            return std::nullopt;
        return !font_relative_unit(*text);
    };
    if (range.is_object()) {
        std::optional<js::Value> const name = interp.get(range, "rangeName");
        if (!name)
            return false;
        std::optional<js::Value> const offset = interp.get(range, "offset");
        if (!offset)
            return false;
        if (name->is_undefined() && offset->is_undefined()) {
            std::optional<bool> const ok = numeric_ok(range);
            return ok.value_or(false);
        }
        if (!name->is_undefined() && !name->is_null()) {
            std::optional<std::string> const text = in.to_utf8(*name);
            if (!text || !is_range_name(*text))
                return false;
        }
        if (offset->is_object()) {
            std::optional<bool> const ok = numeric_ok(*offset);
            return ok.value_or(false);
        }
        return true;
    }
    std::optional<std::string> const text = in.to_utf8(range);
    if (!text)
        return false;
    std::vector<css::ComponentValue> const parsed = css::parse_component_value_list(*text);
    std::vector<css::ComponentValue const*> parts;
    for (css::ComponentValue const& value : parsed) {
        if (!value.is_token(css::Token::Type::Whitespace))
            parts.push_back(&value);
    }
    if (parts.size() == 1 && parts[0]->is_token(css::Token::Type::Ident) && ascii_ci_equals(parts[0]->token().value, "normal"))
        return true;
    std::size_t at = 0;
    if (at < parts.size() && parts[at]->is_token(css::Token::Type::Ident)) {
        if (!is_range_name(parts[at]->token().value))
            return false;
        ++at;
    }
    if (at < parts.size()) {
        css::ComponentValue const& length = *parts[at];
        bool const is_length = length.is_token(css::Token::Type::Dimension) || length.is_token(css::Token::Type::Percentage)
            || (length.is_token(css::Token::Type::Number) && length.token().numeric_value == 0) || length.is_function();
        if (!is_length || mentions_font_relative({ length }))
            return false;
        ++at;
    }
    return at == parts.size() && at > 0;
}

std::optional<EffectOptions> read_effect_options(Realm::Internals& in, js::Value const& options, bool animation_options)
{
    js::Interpreter& interp = in.interpreter;
    EffectOptions out;
    if (!options.is_object() && !options.is_undefined() && !options.is_null()) {
        std::optional<double> const duration = interp.to_number(options);
        if (!duration)
            return std::nullopt;
        out.timing.duration = *duration;
        return out;
    }
    if (!options.is_object())
        return out;
    if (!read_timing(in, options, out.timing))
        return std::nullopt;
    std::optional<js::Value> value = interp.get(options, "composite");
    if (!value)
        return std::nullopt;
    if (!value->is_undefined()) {
        if (!(out.composite = composite_operation(in, *value)))
            return std::nullopt;
    }
    if (!(value = interp.get(options, "iterationComposite")))
        return std::nullopt;
    if (!value->is_undefined()) {
        std::optional<css::IterationComposite> const which = enum_member<css::IterationComposite>(in, *value,
            { { "replace", css::IterationComposite::Replace }, { "accumulate", css::IterationComposite::Accumulate } },
            "IterationCompositeOperation");
        if (!which)
            return std::nullopt;
        out.iteration_composite = which;
    }
    if (!(value = interp.get(options, "pseudoElement")))
        return std::nullopt;
    if (!value->is_undefined()) {
        if (value->is_null()) {
            out.pseudo = std::optional<std::string> {};
        } else {
            std::optional<std::string> text = in.to_utf8(*value);
            if (!text)
                return std::nullopt;
            out.pseudo = std::optional<std::string> { std::move(*text) };
        }
    }
    if (!animation_options)
        return out;
    if (!(value = interp.get(options, "id")))
        return std::nullopt;
    if (!value->is_undefined()) {
        std::optional<std::string> text = in.to_utf8(*value);
        if (!text)
            return std::nullopt;
        out.id = std::move(*text);
    }
    // The scroll-driven ranges are read and checked, as the browsers that
    // have them check them, and not used: there are no view timelines here.
    for (std::string_view const name : { "rangeEnd", "rangeStart" }) {
        std::optional<js::Value> const range = interp.get(options, name);
        if (!range)
            return std::nullopt;
        if (!range_is_valid(in, *range)) {
            if (!interp.has_exception())
                interp.throw_type_error("Failed to execute 'animate' on 'Element': Invalid " + std::string(name) + ".");
            return std::nullopt;
        }
    }
    if (!(value = interp.get(options, "timeline")))
        return std::nullopt;
    if (!value->is_undefined()) {
        if (!value->is_null() && !(value->is_object() && dynamic_cast<TimelineObject*>(value->as_object()))) {
            interp.throw_type_error("Failed to execute 'animate' on 'Element': Failed to read the 'timeline' property from 'KeyframeAnimationOptions': Failed to convert value to 'AnimationTimeline'.");
            return std::nullopt;
        }
        out.timeline_given = true;
        out.timeline = *value;
    }
    return out;
}

// §6.6.1, the KeyframeEffect constructor's steps after its arguments are
// converted: the pseudo-element, the timing, the composite operations, then
// the keyframes.
std::optional<std::shared_ptr<css::KeyframeEffect>> make_effect(Realm::Internals& in, dom::Element* target, js::Value const& keyframes,
    EffectOptions const& options)
{
    auto effect = std::make_shared<css::KeyframeEffect>();
    effect->set_target(target);
    if (options.pseudo && *options.pseudo) {
        std::optional<PseudoTarget> const pseudo = parse_pseudo(**options.pseudo);
        if (!pseudo) {
            in.throw_dom_exception("SyntaxError", "Failed to construct 'KeyframeEffect': A valid PseudoElement must be provided.");
            return std::nullopt;
        }
        effect->pseudo = pseudo->kind;
        effect->pseudo_name = pseudo->name;
    }
    if (!apply_timing(in, options.timing, effect->timing))
        return std::nullopt;
    if (options.composite)
        effect->composite = *options.composite;
    if (options.iteration_composite)
        effect->iteration_composite = *options.iteration_composite;
    std::optional<std::vector<css::Keyframe>> processed = process_keyframes(in, keyframes);
    if (!processed)
        return std::nullopt;
    effect->set_keyframes(std::move(*processed));
    return effect;
}

dom::Element* element_argument(Realm::Internals& in, js::Value const& value)
{
    dom::Node* const node = in.realm.node_of(value);
    return node && node->is_element() ? static_cast<dom::Element*>(node) : nullptr;
}

// The document an animation made by this realm runs against by default:
// the realm's own document's timeline.
std::shared_ptr<css::AnimationTimeline> default_timeline(Realm::Internals& in)
{
    return animations_of(in, *in.document).timeline;
}

std::shared_ptr<css::Animation> new_animation(Realm::Internals& in, std::shared_ptr<css::KeyframeEffect> effect,
    std::shared_ptr<css::AnimationTimeline> timeline)
{
    css::DocumentAnimations& owner = timeline && timeline->document ? *timeline->document : animations_of(in, *in.document);
    auto animation = std::make_shared<css::Animation>(owner, css::Animation::Kind::Script);
    animation->set_timeline(std::move(timeline));
    animation->set_effect(std::move(effect));
    return animation;
}

// getAnimations(): the relevant animations of `root` (an element, its
// subtree with `subtree`, or a whole document or shadow tree).
js::Value animations_array(Realm::Internals& in, dom::Document& document, dom::Node const* root, bool subtree,
    std::string const* pseudo = nullptr)
{
    js::Interpreter& interp = in.interpreter;
    // The styles first: what they start is among what is asked for (a CSS
    // animation, a transition), as getAnimations() flushes them in every
    // engine.
    if (Realm::Internals* const owner = in.realm_of(document); owner && owner->hooks.computed_style) {
        dom::Element const* element = root && root->is_element() ? static_cast<dom::Element const*>(root) : nullptr;
        for (dom::Node const* child : document.children()) {
            if (!element && child->is_element())
                element = static_cast<dom::Element const*>(child);
        }
        if (element)
            static_cast<void>(owner->hooks.computed_style(*element));
    }
    js::Interpreter::Roots const roots(interp);
    js::ArrayObject* const list = interp.new_array();
    interp.root(js::Value::object(list));
    css::DocumentAnimations* const animations = css::DocumentAnimations::find(document);
    if (!animations)
        return js::Value::object(list);
    std::vector<std::shared_ptr<css::Animation>> found;
    if (root && root->is_element()) {
        found = animations->relevant(static_cast<dom::Element const*>(root), subtree, pseudo);
    } else {
        found = animations->relevant(nullptr, true);
        if (root) {
            std::erase_if(found, [root](std::shared_ptr<css::Animation> const& animation) {
                return &animation->effect->target->root() != root;
            });
        }
    }
    for (std::shared_ptr<css::Animation> const& animation : found)
        list->push(interp.root(wrap_animation(in, animation)));
    return js::Value::object(list);
}

// --- The rendering update ---------------------------------------------------------------------

bool frame_wanted(Agent const& agent)
{
    if (!agent.animation_frame_callbacks.empty())
        return true;
    for (auto const& [alive, owner] : agent.animated_realms) {
        if (alive.expired() || owner->ended)
            continue;
        css::DocumentAnimations const* const animations = css::DocumentAnimations::find(*owner->document);
        if (animations && animations->wants_frame())
            return true;
    }
    return false;
}

void dispatch_animation_events(Realm::Internals& owner, css::DocumentAnimations& animations)
{
    std::vector<css::AnimationEvent> events = std::move(animations.pending_events());
    animations.pending_events().clear();
    if (events.empty())
        return;
    // §4.4.18.2 step 5: by scheduled time, the unresolved first, the rest
    // in the order they were queued.
    std::stable_sort(events.begin(), events.end(), [](css::AnimationEvent const& a, css::AnimationEvent const& b) {
        if (!a.scheduled_time || !b.scheduled_time)
            return !a.scheduled_time && b.scheduled_time;
        return *a.scheduled_time < *b.scheduled_time;
    });
    Realm::Internals::Entry const entry(owner);
    js::Interpreter& interp = owner.interpreter;
    for (css::AnimationEvent const& queued : events) {
        if (owner.ended)
            return;
        js::Interpreter::Roots const roots(interp);
        if (queued.interface == css::AnimationEvent::Interface::Playback) {
            // Only a script object can have listeners.
            js::Object* const target = queued.animation->wrapper;
            if (!target)
                continue;
            interp.root(js::Value::object(target));
            EventObject* const event = owner.new_event("AnimationPlaybackEvent", queued.type, false, false);
            interp.root(js::Value::object(event));
            event->is_trusted = true;
            event->animation_current_time = queued.current_time;
            event->animation_timeline_time = queued.timeline_time;
            owner.dispatch(*event, target);
            continue;
        }
        if (!queued.target)
            continue;
        js::Object* const target = owner.wrap(*queued.target);
        interp.root(js::Value::object(target));
        bool const transition = queued.interface == css::AnimationEvent::Interface::Transition;
        EventObject* const event = owner.new_event(transition ? "TransitionEvent" : "AnimationEvent", queued.type, true, false);
        interp.root(js::Value::object(event));
        event->is_trusted = true;
        event->animation_name = queued.name;
        event->elapsed_time = queued.elapsed;
        event->detail_value = wrap_animation(owner, queued.animation);
        event->pseudo_element = std::string(pseudo_text(queued.pseudo));
        owner.dispatch(*event, target);
    }
}

void run_animation_frame_callbacks(Agent& agent, double now)
{
    // Those asked before this update; one asked during it waits for the next.
    std::vector<int> ids;
    for (Agent::AnimationFrameCallback const& callback : agent.animation_frame_callbacks)
        ids.push_back(callback.id);
    for (int const id : ids) {
        auto const found = std::find_if(agent.animation_frame_callbacks.begin(), agent.animation_frame_callbacks.end(),
            [id](Agent::AnimationFrameCallback const& callback) { return callback.id == id; });
        if (found == agent.animation_frame_callbacks.end())
            continue; // cancelled by one that ran before it
        Agent::AnimationFrameCallback callback = std::move(*found);
        agent.animation_frame_callbacks.erase(found);
        Realm::Internals& owner = *callback.owner;
        if (owner.ended || owner.discarded)
            continue;
        js::Interpreter::Roots const roots(agent.interpreter);
        js::Value const function = agent.interpreter.root(callback.callback->value());
        js::Value const timestamp[1] = { js::Value::number(now - owner.time_origin) };
        ++owner.stats.timers_fired;
        owner.call_reporting(function, js::Value::object(owner.window_proxy()), timestamp, "animation frame");
        if (agent.interpreter.terminated())
            return;
    }
}

}

std::optional<double> next_rendering_update(Agent const& agent)
{
    if (!frame_wanted(agent))
        return std::nullopt;
    if (!std::isfinite(agent.last_rendering_update))
        return 0.0; // not reached: the first wish for a frame sets it
    // The slot of the grid after the one the last update fell in: an update
    // that ran late does not push the next one a whole slot on.
    double const slot = std::floor(agent.last_rendering_update / frame_interval + 1e-6);
    return (slot + 1) * frame_interval;
}

bool run_rendering_update(Realm::Internals& page, double now)
{
    Agent& agent = page.agent;
    std::optional<double> const due = next_rendering_update(agent);
    if (!due || now < *due)
        return false;
    agent.last_rendering_update = now;
    std::erase_if(agent.animated_realms, [](auto const& entry) { return entry.first.expired() || entry.second->ended; });
    std::erase_if(agent.animation_frame_callbacks, [](Agent::AnimationFrameCallback const& callback) { return callback.owner->ended; });
    std::vector<Realm::Internals*> realms;
    for (auto const& [alive, owner] : agent.animated_realms)
        realms.push_back(owner);
    // §4.4.18.2 "update animations and send events", each document: its
    // timelines moved on (the pending tasks, the finished states, what was
    // replaced) with the promise reactions run, then its events.
    for (Realm::Internals* const owner : realms) {
        css::DocumentAnimations* const animations = css::DocumentAnimations::find(*owner->document);
        if (!animations)
            continue;
        {
            Realm::Internals::Entry const entry(*owner);
            if (animations->update(now - owner->time_origin))
                ++owner->mutations;
        }
        if (agent.interpreter.terminated())
            return true;
        dispatch_animation_events(*owner, *animations);
        if (agent.interpreter.terminated())
            return true;
    }
    run_animation_frame_callbacks(agent, now);
    // Then the styles, as a frame restyles before it paints: what changed
    // starts its CSS animations and transitions now, and their first events
    // go out with the next update. A host with nothing to paint (a test
    // harness) has its styles brought up to date the same way.
    if (std::find(realms.begin(), realms.end(), &page) == realms.end())
        realms.insert(realms.begin(), &page);
    css::set_frame_restyle(true);
    for (Realm::Internals* const owner : realms) {
        if (owner->ended || !owner->hooks.computed_style || agent.interpreter.terminated())
            continue;
        for (dom::Node const* child : owner->document->children()) {
            if (child->is_element()) {
                static_cast<void>(owner->hooks.computed_style(static_cast<dom::Element const&>(*child)));
                break;
            }
        }
    }
    css::set_frame_restyle(false);
    return true;
}

void animations_made(Realm::Internals& in)
{
    animations_of(in, *in.document);
}

int request_animation_frame(Realm::Internals& in, js::Value const& callback)
{
    wish_for_frames(in.agent, in.now());
    Agent::AnimationFrameCallback entry;
    entry.id = in.agent.next_timer_id++;
    entry.owner = &in;
    entry.callback = std::make_unique<js::Persistent>(in.interpreter.heap(), callback);
    int const id = entry.id;
    in.trace("animation frame " + std::to_string(id) + " asked for");
    in.agent.animation_frame_callbacks.push_back(std::move(entry));
    return id;
}

void cancel_animation_frame(Realm::Internals& in, int id)
{
    std::erase_if(in.agent.animation_frame_callbacks,
        [&in, id](Agent::AnimationFrameCallback const& callback) { return callback.id == id && callback.owner == &in; });
}

void trace_animations(Realm::Internals const& in, js::Tracer& tracer)
{
    css::DocumentAnimations const* const animations = css::DocumentAnimations::find(*in.document);
    if (!animations)
        return;
    if (animations->timeline->wrapper)
        tracer.visit(animations->timeline->wrapper);
    // What plays keeps its script object, whose listeners its events reach.
    for (std::shared_ptr<css::Animation> const& animation : animations->animations()) {
        if (animation->wrapper)
            tracer.visit(animation->wrapper);
    }
    for (css::AnimationEvent const& event : const_cast<css::DocumentAnimations*>(animations)->pending_events()) {
        if (event.animation->wrapper)
            tracer.visit(event.animation->wrapper);
    }
}

void install_animations(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    // --- AnimationTimeline, DocumentTimeline ---
    js::Object* timeline_proto = define_interface(in, "AnimationTimeline", nullptr);
    define_getter(in, *timeline_proto, "currentTime", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const object = this_value.is_object() ? dynamic_cast<TimelineObject*>(this_value.as_object()) : nullptr;
        if (!object)
            return interp.throw_type_error("Illegal invocation");
        return time_value(object->timeline->current_time());
    });
    // web-animations-2: a document timeline has no duration.
    define_getter(in, *timeline_proto, "duration", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_value.is_object() || !dynamic_cast<TimelineObject*>(this_value.as_object()))
            return interp.throw_type_error("Illegal invocation");
        return js::Value::null();
    });
    define_interface(in, "DocumentTimeline", timeline_proto,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            double origin = 0;
            js::Value const options = js::argument(args, 0);
            if (options.is_object()) {
                std::optional<js::Value> const value = interp.get(options, "originTime");
                if (!value)
                    return std::nullopt;
                if (!value->is_undefined()) {
                    std::optional<double> const number = interp.to_number(*value);
                    if (!number)
                        return std::nullopt;
                    if (!std::isfinite(*number))
                        return interp.throw_type_error("Failed to construct 'DocumentTimeline': The provided double value is non-finite.");
                    origin = *number;
                }
            }
            css::DocumentAnimations& animations = animations_of(internals, *internals.document);
            auto made = std::make_shared<css::AnimationTimeline>(&animations, origin);
            return js::Value::object(interp.heap().allocate<TimelineObject>(internals.prototype("DocumentTimeline"), std::move(made)));
        });

    // --- AnimationEffect, KeyframeEffect ---
    js::Object* effect = define_interface(in, "AnimationEffect", nullptr);
    define_operation(interpreter, *effect, "getTiming", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
        if (!object)
            return std::nullopt;
        return timing_object(internals_of(interp), *(*object)->effect, false);
    });
    define_operation(interpreter, *effect, "getComputedTiming", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
        if (!object)
            return std::nullopt;
        return timing_object(internals_of(interp), *(*object)->effect, true);
    });
    define_operation(interpreter, *effect, "updateTiming", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
        if (!object)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Value const given = js::argument(args, 0);
        if (!given.is_undefined() && !given.is_null() && !given.is_object())
            return interp.throw_type_error("Failed to execute 'updateTiming' on 'AnimationEffect': The provided value is not of type 'OptionalEffectTiming'.");
        TimingInput input;
        if (!read_timing(internals, given, input))
            return std::nullopt;
        css::KeyframeEffect& model = *(*object)->effect;
        if (!apply_timing(internals, input, model.timing))
            return std::nullopt;
        // What a script sets of a CSS animation's timing, the animation-*
        // properties no longer set (css-animations-2 §4.1).
        if (model.animation) {
            unsigned & own = model.animation->css_timing_overridden;
            own |= (input.duration ? 1u : 0u) | (input.delay ? 2u : 0u) | (input.iterations ? 4u : 0u)
                | (input.direction ? 8u : 0u) | (input.fill ? 16u : 0u) | (input.end_delay ? 32u : 0u)
                | (input.iteration_start ? 64u : 0u) | (input.easing ? 128u : 0u);
        }
        if (model.animation)
            model.animation->effect_changed();
        return js::Value::undefined();
    });

    js::Object* keyframe_effect = define_interface(in, "KeyframeEffect", effect,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.empty())
                return interp.throw_type_error("Failed to construct 'KeyframeEffect': 1 argument required, but only 0 present.");
            js::Value const first = args[0];
            // new KeyframeEffect(source): a copy of another's.
            if (args.size() == 1 && first.is_object()) {
                if (auto* const source = dynamic_cast<KeyframeEffectObject*>(first.as_object())) {
                    auto copy = std::make_shared<css::KeyframeEffect>();
                    copy->set_target(source->effect->target);
                    copy->pseudo = source->effect->pseudo;
                    copy->pseudo_name = source->effect->pseudo_name;
                    copy->timing = source->effect->timing;
                    copy->composite = source->effect->composite;
                    copy->iteration_composite = source->effect->iteration_composite;
                    copy->set_keyframes(source->effect->keyframes());
                    return wrap_effect(internals, copy);
                }
            }
            dom::Element* target = nullptr;
            if (!first.is_null()) {
                target = element_argument(internals, first);
                if (!target)
                    return interp.throw_type_error("Failed to construct 'KeyframeEffect': parameter 1 is not of type 'Element'.");
            }
            js::Value const keyframes = js::argument(args, 1);
            if (!keyframes.is_object() && !keyframes.is_null() && !keyframes.is_undefined())
                return interp.throw_type_error("Failed to construct 'KeyframeEffect': The provided value is not of type 'object'.");
            std::optional<EffectOptions> const options = read_effect_options(internals, js::argument(args, 2), false);
            if (!options)
                return std::nullopt;
            std::optional<std::shared_ptr<css::KeyframeEffect>> made = make_effect(internals, target, keyframes, *options);
            if (!made)
                return std::nullopt;
            return wrap_effect(internals, *made);
        },
        1);
    define_getter(in, *keyframe_effect, "target",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            dom::Element* const target = (*object)->effect->target;
            return target ? js::Value::object(internals_of(interp).wrap(*target)) : js::Value::null();
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            dom::Element* target = nullptr;
            if (!value.is_null()) {
                target = element_argument(internals_of(interp), value);
                if (!target)
                    return interp.throw_type_error("Failed to set the 'target' property on 'KeyframeEffect': The provided value is not of type 'Element'.");
            }
            css::KeyframeEffect& model = *(*object)->effect;
            if (model.animation && model.animation->document)
                model.animation->document->mark_target(*model.animation);
            model.set_target(target);
            if (model.animation)
                model.animation->effect_changed();
            return js::Value::undefined();
        });
    define_getter(in, *keyframe_effect, "pseudoElement",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            std::string const& text = (*object)->effect->pseudo_name;
            return text.empty() ? js::Value::null() : internals_of(interp).string(text);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            js::Value const value = js::argument(args, 0);
            PseudoTarget pseudo;
            if (!value.is_null()) {
                std::optional<std::string> const text = internals.to_utf8(value);
                if (!text)
                    return std::nullopt;
                std::optional<PseudoTarget> const parsed = parse_pseudo(*text);
                if (!parsed)
                    return internals.throw_dom_exception("SyntaxError",
                        "Failed to set the 'pseudoElement' property on 'KeyframeEffect': A valid PseudoElement must be provided.");
                pseudo = *parsed;
            }
            css::KeyframeEffect& model = *(*object)->effect;
            if (model.animation && model.animation->document)
                model.animation->document->mark_target(*model.animation);
            model.pseudo = pseudo.kind;
            model.pseudo_name = pseudo.name;
            if (model.animation)
                model.animation->effect_changed();
            return js::Value::undefined();
        });
    define_getter(in, *keyframe_effect, "composite",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            return internals_of(interp).string(composite_text((*object)->effect->composite));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            // An enumeration attribute ignores a value outside it (WebIDL §3.7.6).
            std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            css::KeyframeEffect& model = *(*object)->effect;
            if (*text == "replace")
                model.composite = css::CompositeOperation::Replace;
            else if (*text == "add")
                model.composite = css::CompositeOperation::Add;
            else if (*text == "accumulate")
                model.composite = css::CompositeOperation::Accumulate;
            else
                return js::Value::undefined();
            if (model.animation)
                model.animation->effect_changed();
            return js::Value::undefined();
        });
    define_getter(in, *keyframe_effect, "iterationComposite",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            return internals_of(interp).string(
                (*object)->effect->iteration_composite == css::IterationComposite::Accumulate ? "accumulate" : "replace");
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
            if (!object)
                return std::nullopt;
            std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            css::KeyframeEffect& model = *(*object)->effect;
            if (*text == "replace")
                model.iteration_composite = css::IterationComposite::Replace;
            else if (*text == "accumulate")
                model.iteration_composite = css::IterationComposite::Accumulate;
            else
                return js::Value::undefined();
            if (model.animation)
                model.animation->effect_changed();
            return js::Value::undefined();
        });
    define_operation(interpreter, *keyframe_effect, "getKeyframes", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
        if (!object)
            return std::nullopt;
        return keyframes_array(internals_of(interp), *(*object)->effect);
    });
    define_operation(interpreter, *keyframe_effect, "setKeyframes", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<KeyframeEffectObject*> const object = this_effect(interp, this_value);
        if (!object)
            return std::nullopt;
        js::Value const keyframes = js::argument(args, 0);
        if (!keyframes.is_object() && !keyframes.is_null() && !keyframes.is_undefined())
            return interp.throw_type_error("Failed to execute 'setKeyframes' on 'KeyframeEffect': The provided value is not of type 'object'.");
        std::optional<std::vector<css::Keyframe>> processed = process_keyframes(internals_of(interp), keyframes);
        if (!processed)
            return std::nullopt;
        css::KeyframeEffect& model = *(*object)->effect;
        model.set_keyframes(std::move(*processed));
        if (model.animation)
            model.animation->css_keyframes_overridden = true;
        if (model.animation)
            model.animation->effect_changed();
        return js::Value::undefined();
    });

    // --- Animation ---
    js::Object* animation_proto = define_interface(in, "Animation", in.prototype("EventTarget"),
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Value const effect_value = js::argument(args, 0);
            std::shared_ptr<css::KeyframeEffect> model_effect;
            if (!effect_value.is_null() && !effect_value.is_undefined()) {
                auto* const object = effect_value.is_object() ? dynamic_cast<KeyframeEffectObject*>(effect_value.as_object()) : nullptr;
                if (!object)
                    return interp.throw_type_error("Failed to construct 'Animation': parameter 1 is not of type 'AnimationEffect'.");
                model_effect = object->effect;
            }
            std::shared_ptr<css::AnimationTimeline> model_timeline;
            js::Value const timeline_value = js::argument(args, 1);
            if (args.size() < 2 || timeline_value.is_undefined()) {
                model_timeline = default_timeline(internals);
            } else if (!timeline_value.is_null()) {
                auto* const object = timeline_value.is_object() ? dynamic_cast<TimelineObject*>(timeline_value.as_object()) : nullptr;
                if (!object)
                    return interp.throw_type_error("Failed to construct 'Animation': parameter 2 is not of type 'AnimationTimeline'.");
                model_timeline = object->timeline;
            }
            std::shared_ptr<css::Animation> const made = new_animation(internals, std::move(model_effect), std::move(model_timeline));
            return wrap_animation(internals, made);
        });
    static constexpr std::string_view handlers[] = { "finish", "cancel", "remove" };
    define_event_handlers(in, *animation_proto, handlers);
    define_getter(in, *animation_proto, "id",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            return internals_of(interp).string((*object)->animation->id);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            std::optional<std::string> text = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            (*object)->animation->id = std::move(*text);
            return js::Value::undefined();
        });
    define_getter(in, *animation_proto, "effect",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            std::shared_ptr<css::KeyframeEffect> const& model = (*object)->animation->effect;
            return model ? wrap_effect(internals_of(interp), model) : js::Value::null();
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            std::shared_ptr<css::KeyframeEffect> model;
            if (!value.is_null()) {
                auto* const given = value.is_object() ? dynamic_cast<KeyframeEffectObject*>(value.as_object()) : nullptr;
                if (!given)
                    return interp.throw_type_error("Failed to set the 'effect' property on 'Animation': The provided value is not of type 'AnimationEffect'.");
                model = given->effect;
            }
            css::Animation& target_animation = *(*object)->animation;
            // A CSS animation given another effect by a script keeps it:
            // its keyframes and timing are no longer the style's.
            target_animation.css_keyframes_overridden = true;
            target_animation.css_timing_overridden = ~0u;
            target_animation.set_effect(std::move(model));
            return js::Value::undefined();
        });
    define_getter(in, *animation_proto, "timeline",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            return wrap_timeline(internals_of(interp), (*object)->animation->timeline);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            std::shared_ptr<css::AnimationTimeline> model;
            if (!value.is_null()) {
                auto* const given = value.is_object() ? dynamic_cast<TimelineObject*>(value.as_object()) : nullptr;
                if (!given)
                    return interp.throw_type_error("Failed to set the 'timeline' property on 'Animation': The provided value is not of type 'AnimationTimeline'.");
                model = given->timeline;
            }
            (*object)->animation->set_timeline(std::move(model));
            return js::Value::undefined();
        });
    define_getter(in, *animation_proto, "startTime",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            return time_value((*object)->animation->start_time);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            std::optional<double> time;
            if (!value.is_null() && !value.is_undefined()) {
                std::optional<double> const number = interp.to_number(value);
                if (!number)
                    return std::nullopt;
                if (!std::isfinite(*number))
                    return interp.throw_type_error("Failed to set the 'startTime' property on 'Animation': The provided double value is non-finite.");
                time = number;
            }
            (*object)->animation->css_play_state_overridden = true;
            (*object)->animation->set_start_time(time);
            return js::Value::undefined();
        });
    define_getter(in, *animation_proto, "currentTime",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            return time_value((*object)->animation->current_time());
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            std::optional<double> time;
            if (!value.is_null() && !value.is_undefined()) {
                std::optional<double> const number = interp.to_number(value);
                if (!number)
                    return std::nullopt;
                if (!std::isfinite(*number))
                    return interp.throw_type_error("Failed to set the 'currentTime' property on 'Animation': The provided double value is non-finite.");
                time = number;
            }
            if (std::optional<css::Animation::Refusal> const refusal = (*object)->animation->set_current_time(time))
                return refuse(internals_of(interp), *refusal);
            return js::Value::undefined();
        });
    define_getter(in, *animation_proto, "playbackRate",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            return js::Value::number((*object)->animation->playback_rate);
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            std::optional<double> const number = interp.to_number(js::argument(args, 0));
            if (!number)
                return std::nullopt;
            if (!std::isfinite(*number))
                return interp.throw_type_error("Failed to set the 'playbackRate' property on 'Animation': The provided double value is non-finite.");
            (*object)->animation->set_playback_rate(*number);
            return js::Value::undefined();
        });
    define_getter(in, *animation_proto, "playState", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        switch ((*object)->animation->play_state()) {
        case css::PlayState::Idle: return internals_of(interp).string("idle");
        case css::PlayState::Running: return internals_of(interp).string("running");
        case css::PlayState::Paused: return internals_of(interp).string("paused");
        case css::PlayState::Finished: return internals_of(interp).string("finished");
        }
        return internals_of(interp).string("idle");
    });
    define_getter(in, *animation_proto, "replaceState", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        switch ((*object)->animation->replace_state) {
        case css::ReplaceState::Active: return internals_of(interp).string("active");
        case css::ReplaceState::Removed: return internals_of(interp).string("removed");
        case css::ReplaceState::Persisted: return internals_of(interp).string("persisted");
        }
        return internals_of(interp).string("active");
    });
    define_getter(in, *animation_proto, "pending", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        return js::Value::boolean((*object)->animation->pending());
    });
    // web-animations-2 §4.4.?: how far through its whole effect the
    // animation is, from 0 to 1; null without an effect or a time.
    define_getter(in, *animation_proto, "overallProgress", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        css::Animation const& model = *(*object)->animation;
        std::optional<double> const current = model.current_time();
        if (!model.effect || !current)
            return js::Value::null();
        double const end = model.effect_end();
        if (end == 0)
            return js::Value::number(*current < 0 ? 0 : 1);
        if (std::isinf(end))
            return js::Value::number(0);
        return js::Value::number(std::clamp(*current / end, 0.0, 1.0));
    });
    // The scroll-driven ranges (scroll-animations-1 §4.2): with no view
    // timelines here they are always `normal`; a value set is checked, as
    // the browsers that have them check it.
    for (std::string_view const name : { "rangeStart", "rangeEnd" }) {
        define_getter(in, *animation_proto, name,
            [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                if (!this_animation(interp, this_value))
                    return std::nullopt;
                return internals_of(interp).string("normal");
            },
            [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
                if (!this_animation(interp, this_value))
                    return std::nullopt;
                if (!range_is_valid(internals_of(interp), js::argument(args, 0))) {
                    if (!interp.has_exception())
                        interp.throw_type_error("Failed to set a range on 'Animation': Invalid range.");
                    return std::nullopt;
                }
                return js::Value::undefined();
            });
    }
    // The two promises, made when first asked for.
    auto const promise_getter = [](bool ready) {
        return [ready](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            AnimationObject& self = **object;
            HeldPromise& held = ready ? self.ready : self.finished;
            if (held.made())
                return held.promise;
            std::optional<js::PromiseCapability> const capability
                = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
            if (!capability)
                return std::nullopt;
            held.promise = capability->promise;
            held.resolve = capability->resolve;
            held.reject = capability->reject;
            bool const settled = ready ? !self.animation->ready_pending : self.animation->finished_resolved;
            if (settled) {
                held.settled = true;
                js::Value const argument[1] = { js::Value::object(&self) };
                static_cast<void>(interp.call(held.resolve, js::Value::undefined(), argument));
            }
            return held.promise;
        };
    };
    define_getter(in, *animation_proto, "ready", promise_getter(true), {}, MemberKind::ReturnsPromise);
    define_getter(in, *animation_proto, "finished", promise_getter(false), {}, MemberKind::ReturnsPromise);
    auto const procedure = [&](std::string_view name, std::optional<css::Animation::Refusal> (*run)(css::Animation&)) {
        define_operation(interpreter, *animation_proto, name, 0, [run](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<AnimationObject*> const object = this_animation(interp, this_value);
            if (!object)
                return std::nullopt;
            if (std::optional<css::Animation::Refusal> const refusal = run(*(*object)->animation))
                return refuse(internals_of(interp), *refusal);
            return js::Value::undefined();
        });
    };
    procedure("cancel", [](css::Animation& a) -> std::optional<css::Animation::Refusal> {
        a.cancel();
        return std::nullopt;
    });
    procedure("finish", [](css::Animation& a) { return a.finish(); });
    // A script that plays or pauses a CSS animation takes its play state
    // over from animation-play-state (css-animations-2 §4.1).
    procedure("play", [](css::Animation& a) {
        std::optional<css::Animation::Refusal> refusal = a.play(true);
        a.css_play_state_overridden = a.css_play_state_overridden || !refusal;
        return refusal;
    });
    procedure("pause", [](css::Animation& a) {
        std::optional<css::Animation::Refusal> refusal = a.pause();
        a.css_play_state_overridden = a.css_play_state_overridden || !refusal;
        return refusal;
    });
    procedure("reverse", [](css::Animation& a) {
        std::optional<css::Animation::Refusal> refusal = a.reverse();
        a.css_play_state_overridden = a.css_play_state_overridden || !refusal;
        return refusal;
    });
    procedure("persist", [](css::Animation& a) -> std::optional<css::Animation::Refusal> {
        a.persist();
        return std::nullopt;
    });
    // web-animations-1 "commit computed styles": what the effect stack up
    // to this animation gives each property it animates, written into its
    // target's style attribute — how a page keeps where an animation ended
    // before cancelling it.
    define_operation(interpreter, *animation_proto, "commitStyles", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        css::Animation& model = *(*object)->animation;
        if (!model.effect || !model.effect->target)
            return js::Value::undefined();
        if (!model.effect->pseudo_name.empty())
            return internals.throw_dom_exception("NoModificationAllowedError",
                "Failed to execute 'commitStyles' on 'Animation': Animation target is a pseudo-element.");
        dom::Element& target = *model.effect->target;
        // Only an HTML, SVG or MathML element has a style attribute.
        if (target.namespace_uri() != dom::ns::html && target.namespace_uri() != dom::ns::svg
            && target.namespace_uri() != dom::ns::mathml)
            return internals.throw_dom_exception("NoModificationAllowedError",
                "Failed to execute 'commitStyles' on 'Animation': Target element does not have a style attribute.");
        Realm::Internals* const owner = internals.realm_of(target.document());
        if (!owner || !target.is_connected() || !owner->hooks.computed_style)
            return internals.throw_dom_exception("InvalidStateError",
                "Failed to execute 'commitStyles' on 'Animation': Target element is not rendered.");
        // The style with the stack cut at this animation, then as it was.
        css::set_sample_limit(&model);
        target.mark_style_self();
        ++owner->mutations;
        css::ComputedStyle const* const limited = owner->hooks.computed_style(target);
        std::optional<css::ComputedStyle> const style = limited ? std::optional<css::ComputedStyle>(*limited) : std::nullopt;
        css::set_sample_limit(nullptr);
        target.mark_style_self();
        ++owner->mutations;
        // Rendered: neither it nor anything it is in is display: none.
        bool rendered = style && style->display != css::Display::None;
        for (dom::Node* node = target.parent_or_host(); rendered && node && node->is_element(); node = node->parent_or_host()) {
            css::ComputedStyle const* const above = owner->hooks.computed_style(static_cast<dom::Element&>(*node));
            rendered = above && above->display != css::Display::None;
        }
        if (!rendered)
            return internals.throw_dom_exception("InvalidStateError",
                "Failed to execute 'commitStyles' on 'Animation': Target element is not rendered.");
        std::vector<std::string> targeted;
        for (std::string const& property : model.effect->target_properties()) {
            for (std::string const& physical : property.starts_with("--") ? std::vector<std::string> { property }
                                                                           : css::physical_property_names(property, *style)) {
                if (std::find(targeted.begin(), targeted.end(), physical) == targeted.end())
                    targeted.push_back(physical);
            }
        }
        for (std::string const& property : targeted) {
            std::string text;
            if (property.starts_with("--")) {
                if (std::vector<css::ComponentValue> const* const value = style->custom ? style->custom->find(property) : nullptr)
                    text = css_values_text(*value);
            } else {
                text = computed_value_text(*owner, target, *style, property);
            }
            if (!text.empty())
                write_inline_declaration(*owner, target, property, text);
        }
        return js::Value::undefined();
    });
    define_operation(interpreter, *animation_proto, "updatePlaybackRate", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        std::optional<double> const number = interp.to_number(js::argument(args, 0));
        if (!number)
            return std::nullopt;
        if (!std::isfinite(*number))
            return interp.throw_type_error("Failed to execute 'updatePlaybackRate' on 'Animation': The provided double value is non-finite.");
        (*object)->animation->update_playback_rate(*number);
        return js::Value::undefined();
    });

    // --- CSSAnimation, CSSTransition (css-animations-2 §6, css-transitions-2 §6) ---
    js::Object* css_animation = define_interface(in, "CSSAnimation", animation_proto);
    define_getter(in, *css_animation, "animationName", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        return internals_of(interp).string((*object)->animation->css_name);
    });
    js::Object* css_transition = define_interface(in, "CSSTransition", animation_proto);
    define_getter(in, *css_transition, "transitionProperty", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AnimationObject*> const object = this_animation(interp, this_value);
        if (!object)
            return std::nullopt;
        return internals_of(interp).string((*object)->animation->css_name);
    });

    // --- AnimationPlaybackEvent ---
    js::Object* playback_event = define_interface(in, "AnimationPlaybackEvent", in.prototype("Event"),
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.empty())
                return interp.throw_type_error("Failed to construct 'AnimationPlaybackEvent': 1 argument required, but only 0 present.");
            std::optional<std::string> const type = internals.to_utf8(args[0]);
            if (!type)
                return std::nullopt;
            js::Value const init = js::argument(args, 1);
            bool flags[3] = { false, false, false };
            std::optional<double> times[2];
            if (init.is_object()) {
                std::string_view const names[3] = { "bubbles", "cancelable", "composed" };
                for (int i = 0; i < 3; ++i) {
                    std::optional<js::Value> const value = interp.get(init, names[i]);
                    if (!value)
                        return std::nullopt;
                    flags[i] = js::Interpreter::to_boolean(*value);
                }
                std::string_view const time_names[2] = { "currentTime", "timelineTime" };
                for (int i = 0; i < 2; ++i) {
                    std::optional<js::Value> const value = interp.get(init, time_names[i]);
                    if (!value)
                        return std::nullopt;
                    if (value->is_undefined() || value->is_null())
                        continue;
                    std::optional<double> const number = interp.to_number(*value);
                    if (!number)
                        return std::nullopt;
                    times[i] = number;
                }
            }
            EventObject* const event = internals.new_event("AnimationPlaybackEvent", *type, flags[0], flags[1]);
            event->composed = flags[2];
            event->animation_current_time = times[0];
            event->animation_timeline_time = times[1];
            return js::Value::object(event);
        },
        1);
    auto const event_time = [](bool timeline_time) {
        return [timeline_time](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            auto* const event = this_value.is_object() ? dynamic_cast<EventObject*>(this_value.as_object()) : nullptr;
            if (!event)
                return interp.throw_type_error("Illegal invocation");
            return time_value(timeline_time ? event->animation_timeline_time : event->animation_current_time);
        };
    };
    define_getter(in, *playback_event, "currentTime", event_time(false));
    define_getter(in, *playback_event, "timelineTime", event_time(true));

    // --- What elements and documents add ---
    js::Object* element = in.prototype("Element");
    define_operation(interpreter, *element, "animate", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        dom::Element* const target = element_argument(internals, this_value);
        if (!target)
            return interp.throw_type_error("Illegal invocation");
        js::Value const keyframes = js::argument(args, 0);
        if (!keyframes.is_object() && !keyframes.is_null() && !keyframes.is_undefined())
            return interp.throw_type_error("Failed to execute 'animate' on 'Element': The provided value is not of type 'object'.");
        std::optional<EffectOptions> const options = read_effect_options(internals, js::argument(args, 1), true);
        if (!options)
            return std::nullopt;
        std::optional<std::shared_ptr<css::KeyframeEffect>> made = make_effect(internals, target, keyframes, *options);
        if (!made)
            return std::nullopt;
        std::shared_ptr<css::AnimationTimeline> timeline;
        if (options->timeline_given) {
            if (options->timeline.is_object())
                timeline = static_cast<TimelineObject*>(options->timeline.as_object())->timeline;
        } else {
            // The target's document's timeline.
            Realm::Internals* owner = &internals;
            if (Realm::Internals* const found = internals.realm_of(target->document()))
                owner = found;
            timeline = animations_of(*owner, target->document()).timeline;
        }
        std::shared_ptr<css::Animation> const animation = new_animation(internals, std::move(*made), std::move(timeline));
        animation->id = options->id;
        js::Interpreter::Roots const roots(interp);
        js::Value const wrapper = interp.root(wrap_animation(internals, animation));
        if (std::optional<css::Animation::Refusal> const refusal = animation->play(true))
            return refuse(internals, *refusal);
        return wrapper;
    });
    define_operation(interpreter, *element, "getAnimations", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        dom::Element* const target = element_argument(internals, this_value);
        if (!target)
            return interp.throw_type_error("Illegal invocation");
        bool subtree = false;
        std::optional<std::string> pseudo;
        js::Value const options = js::argument(args, 0);
        if (options.is_object()) {
            std::optional<js::Value> value = interp.get(options, "pseudoElement");
            if (!value)
                return std::nullopt;
            if (!value->is_undefined() && !value->is_null()) {
                std::optional<std::string> const text = internals.to_utf8(*value);
                if (!text)
                    return std::nullopt;
                std::optional<PseudoTarget> const parsed = parse_pseudo(*text);
                if (!parsed)
                    return internals.throw_dom_exception("SyntaxError",
                        "Failed to execute 'getAnimations' on 'Element': A valid pseudo-element must be provided.");
                pseudo = parsed->name;
            }
            if (!(value = interp.get(options, "subtree")))
                return std::nullopt;
            subtree = js::Interpreter::to_boolean(*value);
        }
        return animations_array(internals, target->document(), target, subtree, pseudo ? &*pseudo : nullptr);
    });
    js::Object* document = in.prototype("Document");
    define_getter(in, *document, "timeline", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        dom::Node* const node = internals.realm.node_of(this_value);
        if (!node || node->type() != dom::NodeType::Document)
            return interp.throw_type_error("Illegal invocation");
        auto& owner_document = static_cast<dom::Document&>(*node);
        return wrap_timeline(internals, animations_of(internals, owner_document).timeline);
    });
    auto const tree_animations = [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        dom::Node* const node = internals.realm.node_of(this_value);
        if (!node || !(node->type() == dom::NodeType::Document || node->is_shadow_root()))
            return interp.throw_type_error("Illegal invocation");
        return animations_array(internals, node->document(), node, true);
    };
    define_operation(interpreter, *document, "getAnimations", 0, tree_animations);
    if (js::Object* const shadow_root = in.prototype("ShadowRoot"))
        define_operation(interpreter, *shadow_root, "getAnimations", 0, tree_animations);
}

}
