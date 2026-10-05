#pragma once

// Animations (web-animations-1): the timing model, keyframe effects, the
// animation's play/pause/finish state machine, and the document's set of
// animations that the cascade samples. One model for the three sources —
// the Web Animations API, CSS animations and CSS transitions — as every
// shipping engine has; no script here: the bindings wrap these objects,
// and settle their promises and dispatch their events through the hooks
// below. `_plans/ANIMATIONS-DESIGN.md` has the decisions.

#include "css/ComputedStyle.h"
#include "css/Easing.h"
#include "css/Interpolation.h"
#include "css/Parser.h"
#include "dom/Dom.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sashfold::js {
class Object;
}

namespace sashfold::css {

enum class FillMode : std::uint8_t { None, Forwards, Backwards, Both, Auto };
enum class PlaybackDirection : std::uint8_t { Normal, Reverse, Alternate, AlternateReverse };
enum class CompositeOperation : std::uint8_t { Replace, Add, Accumulate };
enum class IterationComposite : std::uint8_t { Replace, Accumulate };
enum class AnimationPhase : std::uint8_t { Before, Active, After, Idle };
enum class PlayState : std::uint8_t { Idle, Running, Paused, Finished };
enum class ReplaceState : std::uint8_t { Active, Removed, Persisted };
// Which box of an element an effect animates: the element's own, or one
// of its generated boxes (the resolver's targets 1 and 2).
enum class PseudoElement : std::uint8_t { None, Before, After, Marker, Other };

// The animation-* longhands (css-animations-1 §3, -2 §3), each a list as
// written: a list shorter than the names repeats to meet them. A name unset
// is `none`. A style holds none of this until one is written.
struct AnimationLists {
    std::vector<std::optional<std::string>> names = { std::nullopt };
    std::vector<std::optional<double>> durations = { std::nullopt }; // ms; unset is auto
    std::vector<Easing> timing_functions = { Easing::ease() };
    std::vector<double> delays = { 0 }; // ms
    std::vector<double> iteration_counts = { 1 }; // infinity for infinite
    std::vector<PlaybackDirection> directions = { PlaybackDirection::Normal };
    std::vector<FillMode> fill_modes = { FillMode::None };
    std::vector<bool> paused = { false };
    std::vector<CompositeOperation> compositions = { CompositeOperation::Replace };
    bool operator==(AnimationLists const&) const = default;
};

// The transition-* longhands (css-transitions-1 §2, -2 §3): which
// properties (`all`, names; an empty list is `none`), how long, how, after
// how long, and whether discrete properties transition.
struct TransitionLists {
    std::vector<std::string> properties = { "all" };
    std::vector<double> durations = { 0 }; // ms
    std::vector<Easing> timing_functions = { Easing::ease() };
    std::vector<double> delays = { 0 }; // ms
    std::vector<bool> allow_discrete = { false };
    bool operator==(TransitionLists const&) const = default;
};

// The timing an effect is given (§4.5 to §4.9); `duration` unset is auto,
// which for a keyframe effect is zero.
struct EffectTiming {
    double delay = 0;
    double end_delay = 0;
    FillMode fill = FillMode::Auto;
    double iteration_start = 0;
    double iterations = 1;
    std::optional<double> duration;
    PlaybackDirection direction = PlaybackDirection::Normal;
    Easing easing;
};

// What the timing comes to at one local time (getComputedTiming()).
struct ComputedTiming {
    double duration = 0; // the iteration duration, auto resolved
    double active_duration = 0;
    double end_time = 0;
    FillMode fill = FillMode::None; // auto resolved
    std::optional<double> local_time;
    AnimationPhase phase = AnimationPhase::Idle;
    std::optional<double> active_time;
    std::optional<double> progress; // the iteration progress, eased
    std::optional<double> current_iteration;
    bool before_flag = false;
};

// §4.6 to §4.10 for one effect: `backwards` when the animation plays in
// reverse (its playback rate is negative).
// `inclusive_end` counts a time exactly at the end of the active interval as
// in it, as commitStyles() samples (Gecko's inclusive endpoint).
ComputedTiming compute_timing(EffectTiming const& timing, std::optional<double> local_time, bool backwards,
    bool inclusive_end = false);

class Animation;
class DocumentAnimations;

// One keyframe as given (§5.3.2): its offset (unset: spaced out), its
// easing, its composite operation (unset: the effect's), and its property
// values as written — the property's CSS name and its value.
struct Keyframe {
    std::optional<double> offset;
    double computed_offset = 0;
    Easing easing;
    std::optional<CompositeOperation> composite;
    struct Value {
        std::string property; // dashed, lowercase; or a custom property's --name
        std::vector<ComponentValue> value;
        std::string text; // as written, for getKeyframes()
        // A computed value given as it is (a transition's ends), which is
        // then not computed from `value`.
        std::optional<AnimatedValue> typed;
    };
    std::vector<Value> values;
};

// An @keyframes rule as the cascade read it (css-animations-1 §4): its
// keyframes in the order written, each with the offsets its selector names
// (0 to 1), the easing and composite operation it sets for itself, and its
// values; and a signature that changes exactly when what it says does.
struct KeyframesRule {
    std::string name;
    struct Frame {
        std::vector<double> offsets;
        std::optional<Easing> easing;
        std::optional<CompositeOperation> composite;
        std::vector<Keyframe::Value> values;
    };
    std::vector<Frame> frames;
    std::uint64_t signature = 0;
};

// A timeline (§4.2): a document's, whose time is the document's frame time
// less its origin. Unattached (no document) or the document not shown, it
// is inactive and its time is null.
class AnimationTimeline {
public:
    AnimationTimeline(DocumentAnimations* owner, double origin)
        : document(owner)
        , origin_time(origin)
    {
    }
    std::optional<double> current_time() const;
    bool active() const { return current_time().has_value(); }
    // Timeline time to the document's time origin, and back (§4.2.1.1).
    double to_origin_relative(double timeline_time) const { return timeline_time + origin_time; }

    DocumentAnimations* document;
    double origin_time;
    js::Object* wrapper = nullptr; // the script object, while one lives
};

// A keyframe effect (§5.3): its target, timing and keyframes; the
// animation it is associated with, when it is. The target's document keeps
// the effect where its resolvers sample it — which need not be the document
// of the animation's timeline — and lets go of the target when it ends.
class KeyframeEffect : public std::enable_shared_from_this<KeyframeEffect> {
public:
    dom::Element* target = nullptr; // set through set_target
    void set_target(dom::Element* element);
    PseudoElement pseudo = PseudoElement::None;
    // The pseudo-element as the API spells it (::before, ::part(x)); empty
    // for the element itself. Only ::before and ::after are drawn here.
    std::string pseudo_name;
    EffectTiming timing;
    CompositeOperation composite = CompositeOperation::Replace;
    IterationComposite iteration_composite = IterationComposite::Replace;
    Animation* animation = nullptr;
    js::Object* wrapper = nullptr;
    // The easing of the keyframes the effect stands in at 0 and 1 where its
    // own name a property at neither: linear, but a CSS animation's timing
    // function (css-animations-1 §4, the constructed keyframes).
    Easing neutral_easing;

    std::vector<Keyframe> const& keyframes() const { return m_keyframes; }
    // Replaces the keyframes; their computed offsets and the effect's
    // property set are worked out again.
    void set_keyframes(std::vector<Keyframe> keyframes);

    std::optional<double> local_time() const;
    ComputedTiming computed_timing() const;
    // §4.6.4: in play, current, in effect.
    bool in_play() const;
    bool is_current() const;
    bool in_effect() const;
    // The longhands (and custom properties) the keyframes name, sorted.
    std::vector<std::string> const& target_properties() const { return m_properties; }
    // Per keyframe, which of its values sets each longhand: a longhand
    // written out wins over a shorthand that holds it, and a shorthand
    // holding fewer longhands over one holding more (§5.3.4).
    std::vector<std::unordered_map<std::string, std::size_t>> const& expanded() const { return m_expanded; }

private:
    std::vector<Keyframe> m_keyframes;
    std::vector<std::string> m_properties;
    std::vector<std::unordered_map<std::string, std::size_t>> m_expanded;
};

// The script side of an animation: its two promises, which the model
// settles at the steps the specification names. A promise that is
// rejected (with an AbortError) is replaced by a new pending one at once;
// one resolved stays resolved until `replaced` says a new pending one
// takes its place.
class AnimationClient {
public:
    enum class Promise : std::uint8_t { Ready, Finished };
    virtual ~AnimationClient() = default;
    virtual void resolved(Promise) = 0;
    virtual void rejected(Promise) = 0;
    virtual void replaced(Promise) = 0;
};

// An event the model has for the document's pending animation event queue
// (§4.4.18.1): an AnimationPlaybackEvent, or a CSS animation's or
// transition's event.
struct AnimationEvent {
    enum class Interface : std::uint8_t { Playback, Animation, Transition };
    Interface interface = Interface::Playback;
    std::shared_ptr<Animation> animation;
    std::string type;
    std::optional<double> current_time; // AnimationPlaybackEvent
    std::optional<double> timeline_time;
    std::optional<double> scheduled_time; // origin-relative; unset sorts by composite order
    // AnimationEvent and TransitionEvent: the element (or its generated
    // box), the name or the property, the elapsed time in seconds.
    dom::Element* target = nullptr;
    PseudoElement pseudo = PseudoElement::None;
    std::string name;
    double elapsed = 0;
};

// An animation (§4.4).
class Animation : public std::enable_shared_from_this<Animation> {
public:
    enum class Kind : std::uint8_t { Script, CssAnimation, CssTransition };

    Animation(DocumentAnimations& owner, Kind what);
    ~Animation();

    Kind const kind;
    DocumentAnimations* document; // the document for timing; null once it is gone
    std::string id;
    std::shared_ptr<AnimationTimeline> timeline;
    std::shared_ptr<KeyframeEffect> effect;
    std::optional<double> start_time;
    std::optional<double> hold_time;
    double playback_rate = 1;
    std::optional<double> pending_playback_rate;
    bool pending_play = false;
    bool pending_pause = false;
    std::optional<double> previous_current_time;
    ReplaceState replace_state = ReplaceState::Active;
    std::uint64_t sequence; // place in the global animation list (§5.4.1)
    AnimationClient* client = nullptr;
    js::Object* wrapper = nullptr;
    // The promises' states, which a script object made later reads.
    bool ready_pending = false;
    bool finished_resolved = false;
    bool finish_notification_queued = false;
    // Made pending in a frame's own restyle: ready at that frame's time,
    // the paint that first shows it, as the engines start what a frame
    // painted; one a script made pending between frames is ready at the next.
    bool pending_in_flush = false;
    // A CSS animation's or transition's owning element, and its name (the
    // animation-name, the transitioned property) and place in its list.
    dom::Element* owning_element = nullptr;
    PseudoElement owning_pseudo = PseudoElement::None;
    std::string css_name;
    std::size_t css_index = 0;
    // A CSS animation's bookkeeping: the signature of the @keyframes rule
    // its keyframes came from; what a script took over from the style
    // (play state, keyframes, each timing member by bit: duration 1, delay
    // 2, iterations 4, direction 8, fill 16, end delay 32, iteration start
    // 64, easing 128), which animation-* no longer changes; and the phase
    // and iteration its events last said (css-animations-2 §4.2).
    std::uint64_t css_signature = 0;
    bool css_play_state_overridden = false;
    bool css_keyframes_overridden = false;
    unsigned css_timing_overridden = 0;
    AnimationPhase css_phase = AnimationPhase::Idle;
    double css_iteration = 0;
    double css_active_time = 0;
    // A transition's ends as css-transitions-1 §3 keeps them: where it
    // runs to, where a reversal measures from, and how much shorter than
    // its transition-duration a reversal made it.
    std::optional<AnimatedValue> transition_end;
    std::optional<AnimatedValue> transition_reversing_start;
    double transition_shortening = 1;
    // What the last rendering step saw, so that a box is restyled while its
    // animation plays and once more when it stops or leaves its effect.
    bool was_running = false;
    bool was_in_effect = false;

    std::optional<double> current_time() const;
    PlayState play_state() const;
    bool pending() const { return pending_play || pending_pause; }
    double effective_playback_rate() const { return pending_playback_rate.value_or(playback_rate); }
    // The associated effect's end (§4.4.3), zero with none.
    double effect_end() const;
    bool timeline_active() const { return timeline && timeline->active(); }

    // The procedures of §4.4; a refusal names the DOMException.
    struct Refusal {
        std::string name;
        std::string message;
    };
    std::optional<Refusal> play(bool auto_rewind = true);
    std::optional<Refusal> pause();
    std::optional<Refusal> finish();
    std::optional<Refusal> reverse();
    void cancel();
    void persist();
    void update_playback_rate(double rate);
    void set_playback_rate(double rate);
    // A TypeError when an unresolved time is asked of a resolved animation.
    std::optional<Refusal> set_current_time(std::optional<double> time);
    void set_start_time(std::optional<double> time);
    void set_timeline(std::shared_ptr<AnimationTimeline> new_timeline);
    void set_effect(std::shared_ptr<KeyframeEffect> new_effect);
    // The effect's timing or keyframes changed through it.
    void effect_changed();

    // The rendering step's part (§4.4.4, "as soon as the animation is
    // ready"): the pending play or pause task, at this ready time.
    void run_pending_task(double ready_time);
    void update_finished_state(bool did_seek, bool synchronously_notify);
    void finish_notification();

private:
    std::optional<double> current_time_with(std::optional<double> hold) const;
    void silently_set_current_time(std::optional<double> time);
    void apply_pending_playback_rate();
    void reset_pending_tasks();
    void resolve_ready();
    void new_ready();
    void tell(AnimationClient::Promise promise, int what);
    void changed(); // the effect's output may differ: the target is restyled
};

// What an element's style takes from its animations for one property: a
// typed value to write, a computed value to copy whole from another style
// (a discrete step), or a custom property's declaration.
struct AnimatedProperty {
    std::string name;
    AnimatableProperty const* property = nullptr;
    std::optional<AnimatedValue> value;
    std::shared_ptr<ComputedStyle const> discrete;
    std::optional<Declaration> custom;
};

// How a keyframe's value is computed in its element's context: the style
// the element would have with `declaration` over its base style. The
// resolver answers it.
class KeyframeComputer {
public:
    virtual ~KeyframeComputer() = default;
    virtual ComputedStyle computed_with(Declaration const& declaration) = 0;
    // The physical properties a flow-relative one stands for on this box
    // (margin-inline-start is margin-left in a left-to-right line); any
    // other name as itself.
    virtual std::vector<std::string> physical_names(std::string const& name) = 0;
};

// A document's animations, in two roles. As the document for timing: its
// timeline, every animation on it that can still matter (playing, pending,
// in effect), and the pending animation event queue. As the document the
// targets are in: the effects whose target is one of its elements, which
// its resolvers sample and its getAnimations() reads, whichever document's
// timeline drives them.
class DocumentAnimations final : public dom::DocumentAnimationsBase {
public:
    explicit DocumentAnimations(dom::Document& document);
    ~DocumentAnimations() override;

    // The document's own, made on first use.
    static DocumentAnimations& of(dom::Document& document);
    static DocumentAnimations* find(dom::Document const& document);

    dom::Document& document;
    std::shared_ptr<AnimationTimeline> timeline;
    // The document's frame time, in ms since its time origin: what its
    // timelines read, moved only by the rendering step. Unset while the
    // document is not shown (a document made by script): its timelines
    // are inactive.
    std::optional<double> now;
    std::uint64_t next_sequence = 1;

    // The host's hooks: run a microtask; an effect's output may have
    // changed outside a frame (a script set its time), so styles are owed.
    std::function<void(std::function<void()>)> queue_microtask;
    std::function<void()> on_change;

    // Keeps an animation where the rendering step and the sampling see it.
    void track(Animation& animation);
    std::vector<std::shared_ptr<Animation>> const& animations() const { return m_animations; }
    std::vector<AnimationEvent>& pending_events() { return m_events; }
    void queue_event(AnimationEvent event);

    // The rendering step's "update animations" (§4.4.18.2 steps 1-2):
    // the pending tasks, every animation's finished state, the replaced
    // ones removed, and every target whose value may have moved marked
    // for a restyle. Then the host checkpoints and dispatches the events.
    // Whether it marked any box.
    bool update(double frame_time);
    // Whether another frame is owed: something plays, waits to start, or
    // has events to send.
    bool wants_frame() const;
    // Marks the target of an animation's effect for a restyle.
    void mark_target(Animation const& animation);

    // The cascade's side: whether anything animates this box, and what
    // its animations come to over `base` — the animations' origin, or the
    // transitions'.
    bool animates(dom::Element const& element, PseudoElement pseudo) const;
    std::vector<AnimatedProperty> sample(dom::Element const& element, PseudoElement pseudo, ComputedStyle const& base,
        KeyframeComputer& computer, bool transitions) const;

    // getAnimations(): the relevant animations (§4.4.?), in composite
    // order, whose effect's target is the element (or, with `subtree`, an
    // element in its subtree, its generated boxes counting as its own).
    // With `pseudo`, the animations of that one pseudo-element of the
    // element. A subtree is the element's descendants, not a shadow tree it
    // hosts.
    std::vector<std::shared_ptr<Animation>> relevant(dom::Element const* element, bool subtree,
        std::string const* pseudo = nullptr) const;

    // An effect whose target is one of this document's elements.
    void hold_effect(KeyframeEffect& effect);

    // The style change event's part for CSS animations (css-animations-1
    // §3): a box's animation-* lists against its CSS animations. Each name
    // with an @keyframes rule is an animation: a new one plays from the next
    // frame, one kept takes any new timing, keyframes and play state, the
    // rest are cancelled; a box not displayed has none. The same lists
    // again change nothing, so each resolver over the document may call it.
    using KeyframesLookup = std::function<std::shared_ptr<KeyframesRule const>(std::string const& name)>;
    void update_css_animations(dom::Element& element, PseudoElement pseudo, ComputedStyle const& style,
        KeyframesLookup const& lookup);
    bool has_css_animations(dom::Element const& element, PseudoElement pseudo) const;
    bool has_transitions(dom::Element const& element, PseudoElement pseudo) const;
    // The style change event's part for transitions (css-transitions-1 §3):
    // for each property transition-property names, the box's value before
    // the change (the previous computed style, a running transition's value
    // now) against after it (`after`, the cascade's without animations), and
    // a transition started, reversed or cancelled by the specification's
    // rules. Called again with the same styles, it changes nothing.
    void update_transitions(dom::Element& element, PseudoElement pseudo, ComputedStyle const* before,
        ComputedStyle const& after, KeyframeComputer& names);
    // The events a CSS animation owes for where it is now against where
    // it was (css-animations-2 §4.2).
    void queue_css_animation_events(Animation& animation);

private:
    friend class KeyframeEffect;
    void remove_replaced();
    void prune();
    std::vector<std::shared_ptr<Animation>> m_animations;
    std::vector<AnimationEvent> m_events;
    // The effects targeting this document's elements, and those targets,
    // for the cascade's quick no; worked out again when an effect comes or
    // a target moves.
    std::vector<std::weak_ptr<KeyframeEffect>> m_effects;
    struct CssAnimations {
        dom::Element* element;
        PseudoElement pseudo;
        std::vector<std::shared_ptr<Animation>> animations;
    };
    std::vector<CssAnimations> m_css;
    struct Transitions {
        dom::Element* element;
        PseudoElement pseudo;
        std::vector<std::shared_ptr<Animation>> running; // by css_name, the property
    };
    std::vector<Transitions> m_transitions;
    // How far the document's record of removed elements has been read: an
    // element taken out ends its CSS animations even if it is put back.
    std::uint32_t m_removals_read = 0;
    bool removal_ends_css() const;
    void end_removed_css();
    mutable std::unordered_map<dom::Element const*, unsigned> m_targets;
    mutable bool m_targets_stale = true;
    void index_targets() const;
    template<typename Visit>
    void for_each_effect(Visit&& visit) const;
};

// Whether the host is restyling at the end of a frame (per thread): what
// starts then is ready at that frame's time.
void set_frame_restyle(bool restyling);

// commitStyles() ("commit computed styles"): while set, sampling stops at this animation in
// composite order and takes it even if it was removed. Per thread.
void set_sample_limit(Animation const* animation);

// Composite order between two animations (§5.4.2): CSS transitions, then
// CSS animations, then the rest by their place in the global list.
bool composite_order_before(Animation const& a, Animation const& b);

}
