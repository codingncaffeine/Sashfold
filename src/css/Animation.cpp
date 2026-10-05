#include "css/Animation.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <limits>
#include <map>

namespace sashfold::css {

namespace {

constexpr double infinity = std::numeric_limits<double>::infinity();

thread_local bool t_frame_restyle = false;

double iteration_duration(EffectTiming const& timing) { return timing.duration.value_or(0); }

double active_duration(EffectTiming const& timing)
{
    double const duration = iteration_duration(timing);
    if (duration == 0 || timing.iterations == 0)
        return 0;
    return duration * timing.iterations;
}

double end_time(EffectTiming const& timing)
{
    return std::max(timing.delay + active_duration(timing) + timing.end_delay, 0.0);
}

// The animations a document keeps, where an animation's procedures find it.
DocumentAnimations* animations_of(dom::Document const& document)
{
    return static_cast<DocumentAnimations*>(document.animations.get());
}

// Whether `a` comes before `b` in tree order (shadow-including): the owning
// elements of two CSS animations or transitions.
bool tree_order_before(dom::Node const* a, dom::Node const* b)
{
    if (a == b || !a || !b)
        return false;
    std::vector<dom::Node const*> chain_a;
    for (dom::Node const* node = a; node; node = node->parent_or_host())
        chain_a.push_back(node);
    std::vector<dom::Node const*> chain_b;
    for (dom::Node const* node = b; node; node = node->parent_or_host())
        chain_b.push_back(node);
    std::size_t i = chain_a.size();
    std::size_t j = chain_b.size();
    while (i > 0 && j > 0 && chain_a[i - 1] == chain_b[j - 1]) {
        --i;
        --j;
    }
    if (i == 0)
        return true; // a is an ancestor of b
    if (j == 0)
        return false;
    dom::Node const* const under_a = chain_a[i - 1];
    dom::Node const* const under_b = chain_b[j - 1];
    dom::Node const* const common = i < chain_a.size() ? chain_a[i] : nullptr;
    if (!common)
        return a < b;
    // A shadow root comes before the host's children.
    if (under_a->is_shadow_root())
        return true;
    if (under_b->is_shadow_root())
        return false;
    return under_a->index() < under_b->index();
}

int animation_class(Animation const& animation)
{
    if (animation.kind == Animation::Kind::CssTransition && animation.owning_element)
        return 0;
    if (animation.kind == Animation::Kind::CssAnimation && animation.owning_element)
        return 1;
    return 2;
}

}

// --- Timing -------------------------------------------------------------------------------

ComputedTiming compute_timing(EffectTiming const& timing, std::optional<double> local_time, bool backwards, bool inclusive_end)
{
    ComputedTiming out;
    out.duration = iteration_duration(timing);
    out.active_duration = active_duration(timing);
    out.end_time = end_time(timing);
    out.fill = timing.fill == FillMode::Auto ? FillMode::None : timing.fill;
    out.local_time = local_time;
    if (!local_time)
        return out;
    double const local = *local_time;
    double const active = out.active_duration;
    // Â§4.6.3: the phases, with the boundaries clamped into [0, end time].
    double const before_active = std::max(std::min(timing.delay, out.end_time), 0.0);
    double const active_after = std::max(std::min(timing.delay + active, out.end_time), 0.0);
    if (local < before_active || (backwards && local == before_active && !inclusive_end))
        out.phase = AnimationPhase::Before;
    else if (local > active_after || (!backwards && local == active_after && !inclusive_end))
        out.phase = AnimationPhase::After;
    else
        out.phase = AnimationPhase::Active;
    // Â§4.8.3.1: the active time.
    bool const fills_backwards = out.fill == FillMode::Backwards || out.fill == FillMode::Both;
    bool const fills_forwards = out.fill == FillMode::Forwards || out.fill == FillMode::Both;
    switch (out.phase) {
    case AnimationPhase::Before:
        if (fills_backwards)
            out.active_time = std::max(local - timing.delay, 0.0);
        break;
    case AnimationPhase::Active: out.active_time = local - timing.delay; break;
    case AnimationPhase::After:
        if (fills_forwards)
            out.active_time = std::max(std::min(local - timing.delay, active), 0.0);
        break;
    case AnimationPhase::Idle: break;
    }
    if (!out.active_time)
        return out;
    double const active_time = *out.active_time;
    // Â§4.8.3.2: the overall progress.
    double overall;
    if (out.duration == 0)
        overall = out.phase == AnimationPhase::Before ? 0 : timing.iterations;
    else
        overall = active_time / out.duration;
    overall += timing.iteration_start;
    // Â§4.8.3.3: the simple iteration progress.
    double simple = std::isinf(overall) ? std::fmod(timing.iteration_start, 1.0) : std::fmod(overall, 1.0);
    if (simple == 0 && (out.phase == AnimationPhase::Active || out.phase == AnimationPhase::After)
        && active_time == active && timing.iterations != 0)
        simple = 1.0;
    // Â§4.8.4: the current iteration.
    double current_iteration;
    if (out.phase == AnimationPhase::After && std::isinf(timing.iterations))
        current_iteration = infinity;
    else if (simple == 1.0)
        current_iteration = std::floor(overall) - 1;
    else
        current_iteration = std::floor(overall);
    out.current_iteration = current_iteration;
    // Â§4.9.1: the direction of this iteration, and the directed progress.
    bool forwards = true;
    switch (timing.direction) {
    case PlaybackDirection::Normal: forwards = true; break;
    case PlaybackDirection::Reverse: forwards = false; break;
    case PlaybackDirection::Alternate:
    case PlaybackDirection::AlternateReverse: {
        double d = current_iteration;
        if (timing.direction == PlaybackDirection::AlternateReverse)
            d += 1;
        forwards = std::isinf(d) || std::fmod(d, 2.0) == 0;
        break;
    }
    }
    double const directed = forwards ? simple : 1.0 - simple;
    // Â§4.10.1: the transformed progress.
    out.before_flag = (out.phase == AnimationPhase::Before && forwards) || (out.phase == AnimationPhase::After && !forwards);
    out.progress = timing.easing.apply(directed, out.before_flag);
    return out;
}

// --- Timelines -------------------------------------------------------------------------------

std::optional<double> AnimationTimeline::current_time() const
{
    if (!document || !document->now)
        return std::nullopt;
    return *document->now - origin_time;
}

// --- Keyframe effects ---------------------------------------------------------------------------

void KeyframeEffect::set_keyframes(std::vector<Keyframe> keyframes)
{
    m_keyframes = std::move(keyframes);
    // Â§5.3.3, "compute missing keyframe offsets".
    std::size_t const n = m_keyframes.size();
    for (Keyframe& keyframe : m_keyframes)
        keyframe.computed_offset = keyframe.offset.value_or(std::numeric_limits<double>::quiet_NaN());
    if (n > 1 && !m_keyframes.front().offset)
        m_keyframes.front().computed_offset = 0;
    if (n > 0 && !m_keyframes.back().offset)
        m_keyframes.back().computed_offset = 1;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        if (!std::isnan(m_keyframes[i].computed_offset))
            continue;
        std::size_t end = i;
        while (end < n && std::isnan(m_keyframes[end].computed_offset))
            ++end;
        double const from = m_keyframes[i - 1].computed_offset;
        double const to = m_keyframes[end].computed_offset;
        double const gaps = static_cast<double>(end - i + 1);
        for (std::size_t k = i; k < end; ++k)
            m_keyframes[k].computed_offset = from + (to - from) * static_cast<double>(k - i + 1) / gaps;
        i = end;
    }
    // Â§5.3.4: the longhands each keyframe sets, and from which of its values.
    m_expanded.assign(n, {});
    std::vector<std::string> properties;
    for (std::size_t k = 0; k < n; ++k) {
        // The rank a longhand was set at: 0 written out, else the number of
        // longhands in the shorthand that set it; the lower wins, and among
        // shorthands of one size the first by name.
        std::unordered_map<std::string, std::pair<std::size_t, std::string_view>> rank;
        auto& expanded = m_expanded[k];
        for (std::size_t i = 0; i < m_keyframes[k].values.size(); ++i) {
            std::string const& name = m_keyframes[k].values[i].property;
            auto const set = [&](std::string const& longhand, std::size_t weight) {
                auto const found = rank.find(longhand);
                if (found != rank.end()) {
                    auto const& [held, by] = found->second;
                    if (held < weight || (held == weight && by <= std::string_view(name)))
                        return;
                }
                rank[longhand] = { weight, name };
                expanded[longhand] = i;
            };
            if (std::vector<std::string_view> const* longhands = shorthand_longhands(name)) {
                for (std::string_view const longhand : *longhands)
                    set(std::string(longhand), longhands->size());
            } else {
                set(name, 0);
            }
        }
        for (auto const& [longhand, index] : expanded)
            properties.push_back(longhand);
    }
    std::sort(properties.begin(), properties.end());
    properties.erase(std::unique(properties.begin(), properties.end()), properties.end());
    m_properties = std::move(properties);
}

std::optional<double> KeyframeEffect::local_time() const
{
    return animation ? animation->current_time() : std::nullopt;
}

ComputedTiming KeyframeEffect::computed_timing() const
{
    return compute_timing(timing, local_time(), animation && animation->playback_rate < 0);
}

bool KeyframeEffect::in_play() const
{
    return computed_timing().phase == AnimationPhase::Active && animation
        && animation->play_state() != PlayState::Finished;
}

bool KeyframeEffect::is_current() const
{
    if (!animation)
        return false;
    ComputedTiming const computed = computed_timing();
    if (computed.phase == AnimationPhase::Active && animation->play_state() != PlayState::Finished)
        return true;
    if (animation->playback_rate > 0 && computed.phase == AnimationPhase::Before)
        return true;
    if (animation->playback_rate < 0 && computed.phase == AnimationPhase::After)
        return true;
    return false;
}

bool KeyframeEffect::in_effect() const { return computed_timing().active_time.has_value(); }

// --- Animations ---------------------------------------------------------------------------------

Animation::Animation(DocumentAnimations& owner, Kind what)
    : kind(what)
    , document(&owner)
    , sequence(owner.next_sequence++)
{
}

Animation::~Animation()
{
    if (effect && effect->animation == this)
        effect->animation = nullptr;
}

std::optional<double> Animation::current_time_with(std::optional<double> hold) const
{
    // Â§4.4.4.
    if (hold)
        return hold;
    if (!timeline_active() || !start_time)
        return std::nullopt;
    return (*timeline->current_time() - *start_time) * playback_rate;
}

std::optional<double> Animation::current_time() const { return current_time_with(hold_time); }

double Animation::effect_end() const { return effect ? end_time(effect->timing) : 0; }

PlayState Animation::play_state() const
{
    // Â§4.4.17.
    std::optional<double> const current = current_time();
    if (!current && !start_time && !pending())
        return PlayState::Idle;
    if (pending_pause || (!start_time && !pending_play))
        return PlayState::Paused;
    double const rate = effective_playback_rate();
    if (current && ((rate > 0 && *current >= effect_end()) || (rate < 0 && *current <= 0)))
        return PlayState::Finished;
    return PlayState::Running;
}

void Animation::tell(AnimationClient::Promise promise, int what)
{
    if (!client)
        return;
    switch (what) {
    case 0: client->resolved(promise); break;
    case 1: client->rejected(promise); break;
    default: client->replaced(promise); break;
    }
}

void Animation::resolve_ready()
{
    if (!ready_pending)
        return;
    ready_pending = false;
    tell(AnimationClient::Promise::Ready, 0);
}

void Animation::new_ready()
{
    if (ready_pending)
        return;
    ready_pending = true;
    tell(AnimationClient::Promise::Ready, 2);
}

void Animation::changed()
{
    // SASHFOLD_TRACE_ANIMATIONS: each change of an animation's output, which
    // costs its box a restyle and its realm a mutation.
    static bool const traced = [] { char const* const asked = std::getenv("SASHFOLD_TRACE_ANIMATIONS"); return asked && *asked && *asked != '0'; }();
    if (traced) {
        char const* const kinds[] = { "script", "css-animation", "css-transition" };
        std::cerr << "animation: changed " << kinds[static_cast<int>(kind)] << " " << css_name << " on "
                  << (effect && effect->target ? effect->target->local_name() : std::string("-")) << " state "
                  << static_cast<int>(play_state()) << "\n";
    }
    if (document) {
        document->track(*this);
        document->mark_target(*this);
        if (document->on_change)
            document->on_change();
    }
}

void Animation::apply_pending_playback_rate()
{
    if (pending_playback_rate) {
        playback_rate = *pending_playback_rate;
        pending_playback_rate.reset();
    }
}

void Animation::reset_pending_tasks()
{
    // Â§4.4.? "reset an animation's pending tasks".
    if (!pending())
        return;
    pending_play = false;
    pending_pause = false;
    apply_pending_playback_rate();
    // The pending ready promise is rejected, and a resolved one takes its
    // place.
    if (ready_pending) {
        ready_pending = false;
        tell(AnimationClient::Promise::Ready, 1);
    }
}

void Animation::silently_set_current_time(std::optional<double> seek_time)
{
    // Â§4.4.5, "silently set the current time" (the unresolved case is the
    // caller's).
    if (!seek_time)
        return;
    if (hold_time || !start_time || !timeline_active() || playback_rate == 0)
        hold_time = seek_time;
    else
        start_time = *timeline->current_time() - *seek_time / playback_rate;
    if (!timeline_active())
        start_time.reset();
    previous_current_time.reset();
}

std::optional<Animation::Refusal> Animation::set_current_time(std::optional<double> seek_time)
{
    if (!seek_time) {
        if (current_time())
            return Refusal { "TypeError", "Failed to set the 'currentTime' property on 'Animation': currentTime may not be changed from resolved to unresolved" };
        return std::nullopt;
    }
    silently_set_current_time(seek_time);
    if (pending_pause) {
        hold_time = seek_time;
        apply_pending_playback_rate();
        start_time.reset();
        pending_pause = false;
        resolve_ready();
    }
    update_finished_state(true, false);
    changed();
    return std::nullopt;
}

void Animation::set_start_time(std::optional<double> new_start_time)
{
    // Â§4.4.6.
    std::optional<double> const timeline_time = timeline ? timeline->current_time() : std::nullopt;
    if (!timeline_time && new_start_time)
        hold_time.reset();
    std::optional<double> const previous_current = current_time();
    apply_pending_playback_rate();
    start_time = new_start_time;
    if (new_start_time) {
        if (playback_rate != 0)
            hold_time.reset();
    } else {
        hold_time = previous_current;
    }
    if (pending()) {
        pending_play = false;
        pending_pause = false;
        resolve_ready();
    }
    update_finished_state(true, false);
    changed();
}

std::optional<Animation::Refusal> Animation::play(bool auto_rewind)
{
    // Â§4.4.8.
    bool const aborted_pause = pending_pause;
    bool has_pending_ready = false;
    std::optional<double> seek_time;
    std::optional<double> const current = current_time();
    double const rate = effective_playback_rate();
    double const end = effect_end();
    if (auto_rewind) {
        if (rate >= 0 && (!current || *current < 0 || *current >= end)) {
            seek_time = 0;
        } else if (rate < 0 && (!current || *current <= 0 || *current > end)) {
            if (std::isinf(end))
                return Refusal { "InvalidStateError", "Failed to execute 'play' on 'Animation': Cannot play reversed Animation with infinite target effect end." };
            seek_time = end;
        }
    }
    if (!seek_time && !start_time && !current)
        seek_time = 0;
    if (seek_time)
        hold_time = seek_time;
    if (hold_time)
        start_time.reset();
    if (pending()) {
        pending_play = false;
        pending_pause = false;
        has_pending_ready = true;
    }
    if (!hold_time && !seek_time && !aborted_pause && !pending_playback_rate)
        return std::nullopt;
    if (!has_pending_ready)
        new_ready();
    pending_play = true;
    pending_in_flush = t_frame_restyle;
    update_finished_state(false, false);
    changed();
    return std::nullopt;
}

std::optional<Animation::Refusal> Animation::pause()
{
    // Â§4.4.10.
    if (pending_pause)
        return std::nullopt;
    if (play_state() == PlayState::Paused)
        return std::nullopt;
    std::optional<double> seek_time;
    if (!current_time()) {
        if (playback_rate >= 0) {
            seek_time = 0;
        } else {
            if (std::isinf(effect_end()))
                return Refusal { "InvalidStateError", "Failed to execute 'pause' on 'Animation': Cannot pause, Animation has infinite target effect end." };
            seek_time = effect_end();
        }
    }
    if (seek_time)
        hold_time = seek_time;
    bool has_pending_ready = false;
    if (pending_play) {
        pending_play = false;
        has_pending_ready = true;
    }
    if (!has_pending_ready)
        new_ready();
    pending_pause = true;
    pending_in_flush = t_frame_restyle;
    update_finished_state(false, false);
    changed();
    return std::nullopt;
}

void Animation::run_pending_task(double ready_time)
{
    if (pending_play) {
        // Â§4.4.8, the pending play task.
        if (hold_time) {
            apply_pending_playback_rate();
            double const new_start = playback_rate == 0 ? ready_time : ready_time - *hold_time / playback_rate;
            start_time = new_start;
            if (playback_rate != 0)
                hold_time.reset();
        } else if (start_time && pending_playback_rate) {
            double const to_match = (ready_time - *start_time) * playback_rate;
            apply_pending_playback_rate();
            if (playback_rate == 0) {
                hold_time = to_match;
                start_time = ready_time;
            } else {
                start_time = ready_time - to_match / playback_rate;
            }
        }
        pending_play = false;
        resolve_ready();
        update_finished_state(false, false);
        return;
    }
    if (pending_pause) {
        // Â§4.4.10, the pending pause task.
        if (start_time && !hold_time)
            hold_time = (ready_time - *start_time) * playback_rate;
        apply_pending_playback_rate();
        start_time.reset();
        pending_pause = false;
        resolve_ready();
        update_finished_state(false, false);
    }
}

void Animation::update_finished_state(bool did_seek, bool synchronously_notify)
{
    // Â§4.4.13.
    std::optional<double> const unconstrained = did_seek ? current_time() : current_time_with(std::nullopt);
    if (unconstrained && start_time && !pending()) {
        double const end = effect_end();
        if (playback_rate > 0 && *unconstrained >= end) {
            if (did_seek)
                hold_time = unconstrained;
            else
                hold_time = previous_current_time ? std::max(*previous_current_time, end) : end;
        } else if (playback_rate < 0 && *unconstrained <= 0) {
            if (did_seek)
                hold_time = unconstrained;
            else
                hold_time = previous_current_time ? std::min(*previous_current_time, 0.0) : 0.0;
        } else if (playback_rate != 0 && timeline_active()) {
            if (did_seek && hold_time)
                start_time = *timeline->current_time() - *hold_time / playback_rate;
            hold_time.reset();
        }
    }
    previous_current_time = current_time();
    bool const finished = play_state() == PlayState::Finished;
    if (finished && !finished_resolved) {
        if (synchronously_notify) {
            finish_notification_queued = false;
            finish_notification();
        } else if (!finish_notification_queued) {
            finish_notification_queued = true;
            if (document && document->queue_microtask) {
                std::weak_ptr<Animation> const weak = weak_from_this();
                document->queue_microtask([weak] {
                    std::shared_ptr<Animation> const self = weak.lock();
                    if (self && self->finish_notification_queued) {
                        self->finish_notification_queued = false;
                        self->finish_notification();
                    }
                });
            } else {
                finish_notification_queued = false;
                finish_notification();
            }
        }
    }
    if (!finished && finished_resolved) {
        finished_resolved = false;
        tell(AnimationClient::Promise::Finished, 2);
    }
}

void Animation::finish_notification()
{
    if (play_state() != PlayState::Finished)
        return;
    finished_resolved = true;
    tell(AnimationClient::Promise::Finished, 0);
    if (!document)
        return;
    AnimationEvent event;
    event.animation = shared_from_this();
    event.type = "finish";
    event.current_time = current_time();
    event.timeline_time = timeline ? timeline->current_time() : std::nullopt;
    if (timeline && start_time && playback_rate != 0)
        event.scheduled_time = timeline->to_origin_relative(*start_time + effect_end() / playback_rate);
    document->queue_event(std::move(event));
}

std::optional<Animation::Refusal> Animation::finish()
{
    // Â§4.4.14.
    double const rate = effective_playback_rate();
    if (rate == 0)
        return Refusal { "InvalidStateError", "Failed to execute 'finish' on 'Animation': Cannot finish Animation with a playbackRate of 0." };
    if (rate > 0 && std::isinf(effect_end()))
        return Refusal { "InvalidStateError", "Failed to execute 'finish' on 'Animation': Cannot finish Animation with an infinite target effect end." };
    apply_pending_playback_rate();
    double const limit = playback_rate > 0 ? effect_end() : 0;
    silently_set_current_time(limit);
    if (!start_time && timeline_active())
        start_time = *timeline->current_time() - limit / playback_rate;
    if (pending_pause && start_time) {
        hold_time.reset();
        pending_pause = false;
        resolve_ready();
    }
    if (pending_play && start_time) {
        pending_play = false;
        resolve_ready();
    }
    update_finished_state(true, true);
    changed();
    return std::nullopt;
}

void Animation::cancel()
{
    // Â§4.4.15.
    // A CSS animation that had begun says so (css-animations-2 Â§4.2), with
    // how far into its active interval it was.
    if (kind != Kind::Script && owning_element && document && effect && css_phase != AnimationPhase::Idle) {
        AnimationEvent event;
        bool const transition = kind == Kind::CssTransition;
        event.interface = transition ? AnimationEvent::Interface::Transition : AnimationEvent::Interface::Animation;
        event.animation = shared_from_this();
        event.type = transition ? "transitioncancel" : "animationcancel";
        event.target = owning_element;
        event.pseudo = owning_pseudo;
        event.name = css_name;
        event.elapsed = effect->computed_timing().active_time.value_or(0) / 1000;
        if (std::optional<double> const now = timeline ? timeline->current_time() : std::nullopt)
            event.scheduled_time = timeline->to_origin_relative(*now);
        document->queue_event(std::move(event));
        css_phase = AnimationPhase::Idle;
    }
    if (play_state() != PlayState::Idle) {
        reset_pending_tasks();
        finished_resolved = false;
        finish_notification_queued = false;
        tell(AnimationClient::Promise::Finished, 1);
        if (document) {
            AnimationEvent event;
            event.animation = shared_from_this();
            event.type = "cancel";
            event.timeline_time = timeline ? timeline->current_time() : std::nullopt;
            if (event.timeline_time)
                event.scheduled_time = timeline->to_origin_relative(*event.timeline_time);
            document->queue_event(std::move(event));
        }
    }
    hold_time.reset();
    start_time.reset();
    changed();
}

std::optional<Animation::Refusal> Animation::reverse()
{
    // Â§4.4.18.
    if (!timeline_active())
        return Refusal { "InvalidStateError", "Failed to execute 'reverse' on 'Animation': Cannot reverse an animation with no active timeline" };
    std::optional<double> const original = pending_playback_rate;
    pending_playback_rate = -effective_playback_rate();
    if (std::optional<Refusal> refusal = play(true)) {
        pending_playback_rate = original;
        return refusal;
    }
    return std::nullopt;
}

void Animation::persist()
{
    replace_state = ReplaceState::Persisted;
    changed();
}

void Animation::set_playback_rate(double rate)
{
    // Â§4.4.16.1.
    pending_playback_rate.reset();
    std::optional<double> const previous = current_time();
    playback_rate = rate;
    if (previous)
        static_cast<void>(set_current_time(previous));
    changed();
}

void Animation::update_playback_rate(double rate)
{
    // Â§4.4.16.2.
    PlayState const previous = play_state();
    pending_playback_rate = rate;
    if (pending()) {
        changed();
        return;
    }
    if (previous == PlayState::Idle || previous == PlayState::Paused || !current_time()) {
        apply_pending_playback_rate();
    } else if (previous == PlayState::Finished) {
        std::optional<double> const unconstrained = current_time_with(std::nullopt);
        std::optional<double> const timeline_time = timeline ? timeline->current_time() : std::nullopt;
        if (rate == 0)
            start_time = timeline_time;
        else if (timeline_time && unconstrained)
            start_time = *timeline_time - *unconstrained / rate;
        else
            start_time.reset();
        apply_pending_playback_rate();
        update_finished_state(false, false);
    } else {
        static_cast<void>(play(false));
    }
    changed();
}

void Animation::set_timeline(std::shared_ptr<AnimationTimeline> new_timeline)
{
    if (new_timeline == timeline)
        return;
    timeline = std::move(new_timeline);
    if (start_time)
        hold_time.reset();
    update_finished_state(false, false);
    changed();
}

void Animation::set_effect(std::shared_ptr<KeyframeEffect> new_effect)
{
    // Â§4.4.2.
    if (new_effect == effect)
        return;
    if (document && effect)
        document->mark_target(*this);
    if (new_effect && new_effect->animation && new_effect->animation != this)
        new_effect->animation->set_effect(nullptr);
    if (effect && effect->animation == this)
        effect->animation = nullptr;
    effect = std::move(new_effect);
    if (effect)
        effect->animation = this;
    update_finished_state(false, false);
    changed();
}

void Animation::effect_changed()
{
    update_finished_state(false, false);
    changed();
}

// --- The document's animations ------------------------------------------------------------------

DocumentAnimations::DocumentAnimations(dom::Document& owner)
    : document(owner)
    , timeline(std::make_shared<AnimationTimeline>(this, 0))
{
}

DocumentAnimations::~DocumentAnimations()
{
    // What scripts still hold outlives the document: the animations timed
    // here stop being anyone's, its timeline goes inactive, and an effect
    // targeting one of its elements targets nothing. The elements are still
    // whole here (the document ends its animations first).
    for (std::shared_ptr<Animation> const& animation : m_animations)
        animation->document = nullptr;
    for (std::weak_ptr<KeyframeEffect> const& held : m_effects) {
        std::shared_ptr<KeyframeEffect> const effect = held.lock();
        if (effect && effect->target && &effect->target->document() == &document)
            effect->target = nullptr;
    }
    timeline->document = nullptr;
}

void KeyframeEffect::set_target(dom::Element* element)
{
    if (target == element)
        return;
    if (target) {
        if (DocumentAnimations const* const old = DocumentAnimations::find(target->document()))
            old->m_targets_stale = true;
    }
    target = element;
    if (element)
        DocumentAnimations::of(element->document()).hold_effect(*this);
}

void DocumentAnimations::hold_effect(KeyframeEffect& effect)
{
    std::erase_if(m_effects, [](std::weak_ptr<KeyframeEffect> const& held) { return held.expired(); });
    for (std::weak_ptr<KeyframeEffect> const& held : m_effects) {
        if (held.lock().get() == &effect) {
            m_targets_stale = true;
            return;
        }
    }
    m_effects.push_back(effect.weak_from_this());
    m_targets_stale = true;
}

template<typename Visit>
void DocumentAnimations::for_each_effect(Visit&& visit) const
{
    for (std::weak_ptr<KeyframeEffect> const& held : m_effects) {
        std::shared_ptr<KeyframeEffect> const effect = held.lock();
        // One whose target has gone elsewhere is that document's now.
        if (!effect || !effect->target || &effect->target->document() != &document)
            continue;
        visit(*effect);
    }
}

DocumentAnimations& DocumentAnimations::of(dom::Document& document)
{
    if (!document.animations) {
        document.animations = std::make_unique<DocumentAnimations>(document);
        if (document.on_animations_made)
            document.on_animations_made();
    }
    return *animations_of(document);
}

DocumentAnimations* DocumentAnimations::find(dom::Document const& document) { return animations_of(document); }

void DocumentAnimations::track(Animation& animation)
{
    if (animation.document != this)
        return;
    for (std::shared_ptr<Animation> const& kept : m_animations) {
        if (kept.get() == &animation)
            return;
    }
    // Kept in the global animation list's order.
    std::shared_ptr<Animation> held = animation.shared_from_this();
    auto const at = std::lower_bound(m_animations.begin(), m_animations.end(), held,
        [](std::shared_ptr<Animation> const& a, std::shared_ptr<Animation> const& b) { return a->sequence < b->sequence; });
    m_animations.insert(at, std::move(held));
}

void DocumentAnimations::queue_event(AnimationEvent event) { m_events.push_back(std::move(event)); }

void DocumentAnimations::index_targets() const
{
    m_targets.clear();
    for_each_effect([&](KeyframeEffect const& effect) { ++m_targets[effect.target]; });
    m_targets_stale = false;
}

void DocumentAnimations::mark_target(Animation const& animation)
{
    if (!animation.effect || !animation.effect->target)
        return;
    animation.effect->target->mark_style_self();
}

bool DocumentAnimations::animates(dom::Element const& element, PseudoElement pseudo) const
{
    if (m_effects.empty())
        return false;
    if (m_targets_stale)
        index_targets();
    if (m_targets.find(&element) == m_targets.end())
        return false;
    bool found = false;
    for_each_effect([&](KeyframeEffect const& effect) {
        found = found || (effect.target == &element && effect.pseudo == pseudo && effect.animation);
    });
    return found;
}

bool DocumentAnimations::update(double frame_time)
{
    double const prior = now.value_or(frame_time);
    now = frame_time;
    // A copy: the procedures below may track or drop animations.
    std::vector<std::shared_ptr<Animation>> const animations = m_animations;
    for (std::shared_ptr<Animation> const& animation : animations) {
        if (!animation->pending() || !animation->timeline_active())
            continue;
        double ready = *animation->timeline->current_time();
        if (animation->pending_in_flush)
            ready = std::min(ready, prior - animation->timeline->origin_time);
        animation->pending_in_flush = false;
        animation->run_pending_task(ready);
    }
    for (std::shared_ptr<Animation> const& animation : animations) {
        if (animation->timeline_active())
            animation->update_finished_state(false, false);
    }
    // A CSS animation whose element left the document ends; the others say
    // where they have got to.
    end_removed_css();
    for (std::shared_ptr<Animation> const& animation : animations)
        queue_css_animation_events(*animation);
    remove_replaced();
    // Every box whose value may have moved is restyled: what plays, and
    // what has stopped or left its effect since the last frame.
    bool marked = false;
    for (std::shared_ptr<Animation> const& animation : animations) {
        bool const running = animation->play_state() == PlayState::Running;
        bool const in_effect = animation->effect && animation->effect->in_effect();
        if (running || animation->was_running || in_effect != animation->was_in_effect) {
            mark_target(*animation);
            marked = marked || (animation->effect && animation->effect->target);
        }
        animation->was_running = running;
        animation->was_in_effect = in_effect;
    }
    prune();
    return marked;
}

bool DocumentAnimations::wants_frame() const
{
    if (!m_events.empty())
        return true;
    if (removal_ends_css())
        return true;
    for (std::shared_ptr<Animation> const& animation : m_animations) {
        if (animation->pending() || animation->finish_notification_queued)
            return true;
        if (animation->timeline_active() && animation->play_state() == PlayState::Running)
            return true;
    }
    return false;
}

void DocumentAnimations::remove_replaced()
{
    // Â§5.5.2: a finished script animation whose every property is set, on
    // the same box, by a later replaceable animation is removed.
    auto const replaceable = [](Animation const& animation) {
        return animation.kind == Animation::Kind::Script || !animation.owning_element
            ? animation.play_state() == PlayState::Finished && animation.replace_state != ReplaceState::Removed
                && animation.timeline && animation.effect && animation.effect->target
            : false;
    };
    std::vector<std::shared_ptr<Animation>> ordered = m_animations;
    std::stable_sort(ordered.begin(), ordered.end(),
        [](std::shared_ptr<Animation> const& a, std::shared_ptr<Animation> const& b) { return composite_order_before(*a, *b); });
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        Animation& animation = *ordered[i];
        if (!replaceable(animation) || animation.replace_state != ReplaceState::Active)
            continue;
        KeyframeEffect const& effect = *animation.effect;
        bool covered = true;
        for (std::string const& property : effect.target_properties()) {
            bool found = false;
            for (std::size_t j = i + 1; j < ordered.size() && !found; ++j) {
                Animation const& later = *ordered[j];
                if (!replaceable(later) || later.effect->target != effect.target || later.effect->pseudo != effect.pseudo)
                    continue;
                auto const& theirs = later.effect->target_properties();
                found = std::binary_search(theirs.begin(), theirs.end(), property);
            }
            if (!found) {
                covered = false;
                break;
            }
        }
        if (!covered)
            continue;
        animation.replace_state = ReplaceState::Removed;
        AnimationEvent event;
        event.animation = ordered[i];
        event.type = "remove";
        event.current_time = animation.current_time();
        event.timeline_time = animation.timeline->current_time();
        if (event.timeline_time)
            event.scheduled_time = animation.timeline->to_origin_relative(*event.timeline_time);
        queue_event(std::move(event));
        mark_target(animation);
    }
}

void DocumentAnimations::prune()
{
    std::erase_if(m_animations, [](std::shared_ptr<Animation> const& animation) {
        if (animation->pending() || animation->finish_notification_queued)
            return false;
        if (animation->play_state() == PlayState::Running && animation->timeline_active())
            return false;
        if (!animation->effect || animation->replace_state == ReplaceState::Removed)
            return true;
        return !animation->effect->is_current() && !animation->effect->in_effect();
    });
}

std::vector<std::shared_ptr<Animation>> DocumentAnimations::relevant(dom::Element const* element, bool subtree,
    std::string const* pseudo) const
{
    std::vector<std::shared_ptr<Animation>> found;
    for_each_effect([&](KeyframeEffect const& effect) {
        Animation* const animation = effect.animation;
        if (!animation || animation->replace_state == ReplaceState::Removed)
            return;
        if (!effect.is_current() && !effect.in_effect())
            return;
        if (element && pseudo) {
            if (effect.target != element || effect.pseudo_name != *pseudo)
                return;
        } else if (element) {
            bool inside = effect.target == element && (subtree || effect.pseudo == PseudoElement::None);
            if (!inside && subtree) {
                for (dom::Node const* node = effect.target->parent(); node && !inside; node = node->parent())
                    inside = node == element;
            }
            if (!inside)
                return;
        } else if (!effect.target->is_connected()) {
            return;
        }
        found.push_back(animation->shared_from_this());
    });
    std::stable_sort(found.begin(), found.end(),
        [](std::shared_ptr<Animation> const& a, std::shared_ptr<Animation> const& b) { return composite_order_before(*a, *b); });
    return found;
}

// --- CSS animations -----------------------------------------------------------------------------

namespace {

template<typename T>
T item_at(std::vector<T> const& list, std::size_t i)
{
    return list.empty() ? T {} : list[i % list.size()];
}

std::string pseudo_name_of(PseudoElement pseudo)
{
    switch (pseudo) {
    case PseudoElement::Before: return "::before";
    case PseudoElement::After: return "::after";
    case PseudoElement::Marker: return "::marker";
    default: return {};
    }
}

// The keyframes an @keyframes rule gives an animation whose timing function
// is `easing`: one for each offset each selector names, with its own easing
// or the animation's; those at one offset with one easing and one composite
// operation are one keyframe, a later value of a property winning
// (css-animations-1 Â§5, css-animations-2 Â§5.3).
std::vector<Keyframe> keyframes_from(KeyframesRule const& rule, Easing const& easing)
{
    std::vector<Keyframe> placed;
    for (KeyframesRule::Frame const& frame : rule.frames) {
        for (double const offset : frame.offsets) {
            Keyframe keyframe;
            keyframe.offset = offset;
            keyframe.easing = frame.easing.value_or(easing);
            keyframe.composite = frame.composite;
            keyframe.values = frame.values;
            placed.push_back(std::move(keyframe));
        }
    }
    std::stable_sort(placed.begin(), placed.end(), [](Keyframe const& a, Keyframe const& b) { return *a.offset < *b.offset; });
    std::vector<Keyframe> out;
    for (Keyframe& keyframe : placed) {
        Keyframe* same = nullptr;
        for (auto it = out.rbegin(); it != out.rend() && *it->offset == *keyframe.offset; ++it) {
            if (it->easing == keyframe.easing && it->composite == keyframe.composite) {
                same = &*it;
                break;
            }
        }
        if (!same) {
            out.push_back(std::move(keyframe));
            continue;
        }
        for (Keyframe::Value& value : keyframe.values) {
            auto const existing = std::find_if(same->values.begin(), same->values.end(),
                [&value](Keyframe::Value const& held) { return held.property == value.property; });
            if (existing != same->values.end())
                *existing = std::move(value);
            else
                same->values.push_back(std::move(value));
        }
    }
    return out;
}

}

bool DocumentAnimations::removal_ends_css() const
{
    if (m_css.empty() && m_transitions.empty())
        return false;
    auto const removed = [&](dom::Element const* element) {
        if (!element->is_connected())
            return true;
        for (dom::Document::StyleRemoval const& removal : document.style_removals()) {
            if (removal.at >= m_removals_read && removal.element == element)
                return true;
        }
        return false;
    };
    for (CssAnimations const& entry : m_css) {
        if (removed(entry.element))
            return true;
    }
    for (Transitions const& entry : m_transitions) {
        if (removed(entry.element))
            return true;
    }
    return false;
}

void DocumentAnimations::end_removed_css()
{
    std::vector<dom::Element const*> removed;
    for (dom::Document::StyleRemoval const& removal : document.style_removals()) {
        if (removal.at >= m_removals_read)
            removed.push_back(removal.element);
    }
    m_removals_read = document.style_clock();
    std::vector<std::shared_ptr<Animation>> ending;
    for (std::size_t i = 0; i < m_css.size();) {
        CssAnimations& entry = m_css[i];
        bool const gone = !entry.element->is_connected()
            || std::find(removed.begin(), removed.end(), entry.element) != removed.end();
        if (!gone) {
            ++i;
            continue;
        }
        for (std::shared_ptr<Animation>& animation : entry.animations)
            ending.push_back(std::move(animation));
        m_css.erase(m_css.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (std::size_t i = 0; i < m_transitions.size();) {
        Transitions& entry = m_transitions[i];
        bool const gone = !entry.element->is_connected()
            || std::find(removed.begin(), removed.end(), entry.element) != removed.end();
        if (!gone) {
            ++i;
            continue;
        }
        for (std::shared_ptr<Animation>& transition : entry.running) {
            if (transition->play_state() != PlayState::Finished)
                ending.push_back(std::move(transition));
        }
        m_transitions.erase(m_transitions.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (std::shared_ptr<Animation> const& animation : ending) {
        animation->cancel();
        animation->owning_element = nullptr;
    }
}

bool DocumentAnimations::has_transitions(dom::Element const& element, PseudoElement pseudo) const
{
    for (Transitions const& entry : m_transitions) {
        if (entry.element == &element && entry.pseudo == pseudo)
            return true;
    }
    return false;
}

bool DocumentAnimations::has_css_animations(dom::Element const& element, PseudoElement pseudo) const
{
    for (CssAnimations const& entry : m_css) {
        if (entry.element == &element && entry.pseudo == pseudo)
            return true;
    }
    return false;
}

void DocumentAnimations::update_css_animations(dom::Element& element, PseudoElement pseudo, ComputedStyle const& style,
    KeyframesLookup const& lookup)
{
    auto entry = std::find_if(m_css.begin(), m_css.end(),
        [&](CssAnimations const& held) { return held.element == &element && held.pseudo == pseudo; });
    std::vector<std::shared_ptr<Animation>> old;
    if (entry != m_css.end())
        old = entry->animations;
    std::vector<std::shared_ptr<Animation>> kept;
    if (style.animation && !style.undisplayed && element.is_connected()) {
        AnimationLists const& lists = *style.animation;
        for (std::size_t i = 0; i < lists.names.size(); ++i) {
            if (!lists.names[i])
                continue;
            std::string const& name = *lists.names[i];
            std::shared_ptr<KeyframesRule const> const rule = lookup(name);
            if (!rule)
                continue;
            std::shared_ptr<Animation> animation;
            for (auto it = old.begin(); it != old.end(); ++it) {
                if ((*it)->css_name == name) {
                    animation = *it;
                    old.erase(it);
                    break;
                }
            }
            bool const created = !animation;
            if (created) {
                animation = std::make_shared<Animation>(*this, Animation::Kind::CssAnimation);
                animation->owning_element = &element;
                animation->owning_pseudo = pseudo;
                animation->css_name = name;
                animation->timeline = timeline;
                auto effect = std::make_shared<KeyframeEffect>();
                effect->set_target(&element);
                effect->pseudo = pseudo;
                effect->pseudo_name = pseudo_name_of(pseudo);
                effect->animation = animation.get();
                animation->effect = std::move(effect);
            }
            animation->css_index = i;
            if (!animation->effect) {
                // A script took its effect away: only its place in the list
                // is the style's now.
                kept.push_back(animation);
                continue;
            }
            KeyframeEffect& effect = *animation->effect;
            // The timing the lists give, but for what a script took over.
            EffectTiming timing = effect.timing;
            unsigned const own = animation->css_timing_overridden;
            if (!(own & 1)) {
                std::optional<double> const duration = item_at(lists.durations, i);
                timing.duration = duration.value_or(0);
            }
            if (!(own & 2))
                timing.delay = item_at(lists.delays, i);
            if (!(own & 4))
                timing.iterations = item_at(lists.iteration_counts, i);
            if (!(own & 8))
                timing.direction = item_at(lists.directions, i);
            if (!(own & 16))
                timing.fill = item_at(lists.fill_modes, i);
            if (!(own & 128))
                timing.easing = Easing::linear();
            bool changed = !(timing.duration == effect.timing.duration && timing.delay == effect.timing.delay
                && timing.iterations == effect.timing.iterations && timing.direction == effect.timing.direction
                && timing.fill == effect.timing.fill && timing.easing == effect.timing.easing);
            effect.timing = timing;
            CompositeOperation const composite = item_at(lists.compositions, i);
            if (effect.composite != composite) {
                effect.composite = composite;
                changed = true;
            }
            // The keyframes, again only when the rule or the timing function
            // they default to says something else.
            Easing const easing = item_at(lists.timing_functions, i);
            std::uint64_t const signature = rule->signature ^ std::hash<std::string> {}(easing.serialize());
            if (!animation->css_keyframes_overridden && (created || animation->css_signature != signature)) {
                effect.set_keyframes(keyframes_from(*rule, easing));
                effect.neutral_easing = easing;
                animation->css_signature = signature;
                changed = true;
            }
            if (created) {
                track(*animation);
                if (item_at(lists.paused, i))
                    static_cast<void>(animation->pause());
                else
                    static_cast<void>(animation->play());
            } else {
                if (!animation->css_play_state_overridden) {
                    bool const paused = item_at(lists.paused, i);
                    PlayState const state = animation->play_state();
                    if (paused && state != PlayState::Paused && state != PlayState::Idle)
                        static_cast<void>(animation->pause());
                    else if (!paused && state == PlayState::Paused)
                        static_cast<void>(animation->play());
                }
                if (changed)
                    animation->effect_changed();
            }
            kept.push_back(animation);
        }
    }
    // What the lists no longer name ends (css-animations-1 Â§3).
    for (std::shared_ptr<Animation> const& gone : old) {
        gone->cancel();
        gone->owning_element = nullptr;
    }
    entry = std::find_if(m_css.begin(), m_css.end(),
        [&](CssAnimations const& held) { return held.element == &element && held.pseudo == pseudo; });
    if (kept.empty()) {
        if (entry != m_css.end())
            m_css.erase(entry);
    } else if (entry != m_css.end()) {
        entry->animations = std::move(kept);
    } else {
        m_css.push_back({ &element, pseudo, std::move(kept) });
    }
}

void DocumentAnimations::queue_css_animation_events(Animation& animation)
{
    if (animation.kind == Animation::Kind::Script || !animation.owning_element)
        return;
    bool const transition = animation.kind == Animation::Kind::CssTransition;
    // With no effect an animation that is not idle is past its end.
    static KeyframeEffect const nothing;
    KeyframeEffect const& effect = animation.effect ? *animation.effect : nothing;
    ComputedTiming const timing = animation.effect ? effect.computed_timing() : ComputedTiming {};
    AnimationPhase const phase = animation.play_state() == PlayState::Idle ? AnimationPhase::Idle
        : animation.effect                                                  ? timing.phase
                                                                            : AnimationPhase::After;
    double const iteration = timing.current_iteration.value_or(0);
    AnimationPhase const was = animation.css_phase;
    double const was_iteration = animation.css_iteration;
    // The active time it was last seen at: what a cancel says it had got to.
    double const last_active = animation.css_active_time;
    if (timing.active_time)
        animation.css_active_time = *timing.active_time;
    if (phase == was && (phase != AnimationPhase::Active || iteration == was_iteration))
        return;
    animation.css_phase = phase;
    animation.css_iteration = iteration;
    double const delay = effect.timing.delay;
    double const active = timing.active_duration;
    // (+ 0.0: a -0 elapsed time is said as 0.)
    double const interval_start = std::max(std::min(-delay, active), 0.0) + 0.0;
    double const interval_end = std::max(std::min(animation.effect_end() - delay, active), 0.0) + 0.0;
    std::vector<AnimationEvent> events;
    auto const queue = [&](char const* type, double elapsed) {
        AnimationEvent event;
        event.interface = transition ? AnimationEvent::Interface::Transition : AnimationEvent::Interface::Animation;
        event.animation = animation.shared_from_this();
        event.type = type;
        event.target = animation.owning_element;
        event.pseudo = animation.owning_pseudo;
        event.name = animation.css_name;
        event.elapsed = elapsed / 1000;
        if (animation.start_time && animation.playback_rate != 0 && animation.timeline)
            event.scheduled_time = animation.timeline->to_origin_relative(*animation.start_time + (delay + elapsed) / animation.playback_rate);
        events.push_back(std::move(event));
    };
    if (transition) {
        // css-transitions-2 Â§6.1's table.
        bool const from_idle = was == AnimationPhase::Idle;
        if (from_idle && phase != AnimationPhase::Idle)
            queue("transitionrun", interval_start);
        if (phase == AnimationPhase::Active && (from_idle || was == AnimationPhase::Before)) {
            queue("transitionstart", interval_start);
        } else if (phase == AnimationPhase::After && (from_idle || was == AnimationPhase::Before)) {
            queue("transitionstart", interval_start);
            queue("transitionend", interval_end);
        } else if (phase == AnimationPhase::After && was == AnimationPhase::Active) {
            queue("transitionend", interval_end);
        } else if (phase == AnimationPhase::Before && was == AnimationPhase::Active) {
            queue("transitionend", interval_start);
        } else if (phase == AnimationPhase::Active && was == AnimationPhase::After) {
            queue("transitionstart", interval_end);
        } else if (phase == AnimationPhase::Before && was == AnimationPhase::After) {
            queue("transitionstart", interval_end);
            queue("transitionend", interval_start);
        } else if (phase == AnimationPhase::Idle && !from_idle && animation.effect) {
            queue("transitioncancel", last_active);
        }
        for (AnimationEvent& event : events)
            queue_event(std::move(event));
        return;
    }
    // css-animations-2 Â§4.2's table, from where it was to where it is.
    bool const was_early = was == AnimationPhase::Idle || was == AnimationPhase::Before;
    if (phase == AnimationPhase::Active && was_early) {
        queue("animationstart", interval_start);
    } else if (phase == AnimationPhase::After && was_early) {
        queue("animationstart", interval_start);
        queue("animationend", interval_end);
    } else if (phase == AnimationPhase::Before && was == AnimationPhase::Active) {
        queue("animationend", interval_start);
    } else if (phase == AnimationPhase::Active && was == AnimationPhase::Active) {
        // The boundary crossed: the later of the two iterations, whichever
        // way the animation runs.
        double const boundary = std::max(iteration, was_iteration);
        queue("animationiteration", std::max(0.0, (boundary - effect.timing.iteration_start) * timing.duration));
    } else if (phase == AnimationPhase::After && was == AnimationPhase::Active) {
        queue("animationend", interval_end);
    } else if (phase == AnimationPhase::Active && was == AnimationPhase::After) {
        queue("animationstart", interval_end);
    } else if (phase == AnimationPhase::Before && was == AnimationPhase::After) {
        queue("animationstart", interval_end);
        queue("animationend", interval_start);
    } else if (phase == AnimationPhase::Idle && was != AnimationPhase::Idle && animation.effect) {
        queue("animationcancel", last_active);
    }
    // A pair said at once keeps its order: both at the earlier time.
    if (events.size() == 2 && events[0].scheduled_time && events[1].scheduled_time) {
        double const first = std::min(*events[0].scheduled_time, *events[1].scheduled_time);
        events[0].scheduled_time = first;
        events[1].scheduled_time = first;
    }
    for (AnimationEvent& event : events)
        queue_event(std::move(event));
}

// --- CSS transitions ----------------------------------------------------------------------------

namespace {

// A running transition's value now: its two ends interpolated at its
// progress; nothing when it is not in effect.
std::optional<AnimatedValue> transition_now(Animation const& transition, AnimatableProperty const& property)
{
    KeyframeEffect const* const effect = transition.effect.get();
    if (!effect || effect->keyframes().size() != 2 || effect->keyframes()[0].values.empty()
        || effect->keyframes()[1].values.empty())
        return std::nullopt;
    std::optional<AnimatedValue> const& from = effect->keyframes()[0].values[0].typed;
    std::optional<AnimatedValue> const& to = effect->keyframes()[1].values[0].typed;
    ComputedTiming const timing = effect->computed_timing();
    if (!from || !to || !timing.progress)
        return std::nullopt;
    return interpolate_animated(property, *from, *to, *timing.progress);
}

}

void DocumentAnimations::update_transitions(dom::Element& element, PseudoElement pseudo, ComputedStyle const* before,
    ComputedStyle const& after, KeyframeComputer& names)
{
    auto entry = std::find_if(m_transitions.begin(), m_transitions.end(),
        [&](Transitions const& held) { return held.element == &element && held.pseudo == pseudo; });
    std::vector<std::shared_ptr<Animation>> running;
    if (entry != m_transitions.end())
        running = entry->running;
    // One a script cancelled is gone; a completed one is kept apart: while
    // its end is the value after the change, no new transition starts
    // (css-transitions-1 §3, the set of completed transitions).
    std::erase_if(running, [](std::shared_ptr<Animation> const& transition) {
        return transition->play_state() == PlayState::Idle || !transition->owning_element;
    });
    std::vector<std::shared_ptr<Animation>> completed;
    std::erase_if(running, [&completed](std::shared_ptr<Animation> const& transition) {
        if (transition->play_state() != PlayState::Finished)
            return false;
        completed.push_back(transition);
        return true;
    });
    bool const displayed = !after.undisplayed && element.is_connected();
    TransitionLists const lists = after.transition ? *after.transition : TransitionLists {};
    // Each longhand transition-property names, with the item that names it
    // last (a later item wins).
    std::map<std::string, std::size_t> matched;
    if (displayed && after.transition) {
        for (std::size_t i = 0; i < lists.properties.size(); ++i) {
            std::string const& name = lists.properties[i];
            if (name == "all") {
                for (AnimatableProperty const* property : animatable_properties())
                    matched[std::string(property_name(*property))] = i;
                continue;
            }
            for (std::string const& physical : names.physical_names(name)) {
                std::vector<std::string_view> const* const longhands = shorthand_longhands(physical);
                std::vector<std::string_view> const single { physical };
                for (std::string_view const longhand : longhands ? *longhands : single) {
                    if (animatable_property(longhand))
                        matched[std::string(longhand)] = i;
                }
            }
        }
    }
    // What another animation of this box animates does not transition: its
    // value is that animation's.
    std::vector<std::string> animated;
    for_each_effect([&](KeyframeEffect const& effect) {
        if (effect.target != &element || effect.pseudo != pseudo || !effect.animation
            || effect.animation->kind == Animation::Kind::CssTransition || !effect.in_effect())
            return;
        for (std::string const& name : effect.target_properties()) {
            for (std::string const& physical : names.physical_names(name))
                animated.push_back(physical);
        }
    });
    auto const timing_of = [&](std::size_t i) {
        double const duration = std::max(item_at(lists.durations, i), 0.0);
        double const delay = item_at(lists.delays, i);
        return std::pair { duration, delay };
    };
    std::vector<std::shared_ptr<Animation>> kept;
    std::vector<std::string> handled;
    auto const start = [&](std::string const& name, AnimatedValue const& from, AnimatedValue const& to, std::size_t i,
                           AnimatedValue const& reversing_start, double shortening) {
        auto [duration, delay] = timing_of(i);
        duration *= shortening;
        if (delay < 0)
            delay *= shortening;
        auto transition = std::make_shared<Animation>(*this, Animation::Kind::CssTransition);
        transition->owning_element = &element;
        transition->owning_pseudo = pseudo;
        transition->css_name = name;
        transition->timeline = timeline;
        transition->transition_end = to;
        transition->transition_reversing_start = reversing_start;
        transition->transition_shortening = shortening;
        auto effect = std::make_shared<KeyframeEffect>();
        effect->set_target(&element);
        effect->pseudo = pseudo;
        effect->pseudo_name = pseudo_name_of(pseudo);
        effect->timing.duration = duration;
        effect->timing.delay = delay;
        effect->timing.fill = FillMode::Backwards;
        effect->timing.easing = item_at(lists.timing_functions, i);
        AnimatableProperty const* const property = animatable_property(name);
        std::vector<Keyframe> keyframes(2);
        keyframes[0].offset = 0;
        keyframes[0].values.push_back({ name, {}, property ? serialize_animated(*property, from) : std::string(), from });
        keyframes[1].offset = 1;
        keyframes[1].values.push_back({ name, {}, property ? serialize_animated(*property, to) : std::string(), to });
        effect->set_keyframes(std::move(keyframes));
        effect->animation = transition.get();
        transition->effect = std::move(effect);
        track(*transition);
        static_cast<void>(transition->play());
        kept.push_back(std::move(transition));
    };
    // css-transitions-1 Â§3, the running transitions first: one the list no
    // longer names ends; one whose end changed ends, and a new one runs from
    // where it had got to â shortened when it runs back to where it began.
    for (std::shared_ptr<Animation> const& transition : running) {
        std::string const& name = transition->css_name;
        AnimatableProperty const* const property = animatable_property(name);
        auto const found = matched.find(name);
        if (!displayed || !property || found == matched.end()) {
            transition->cancel();
            continue;
        }
        std::optional<AnimatedValue> const end = read_animated(*property, after);
        if (end && transition->transition_end && same_animated(*end, *transition->transition_end)) {
            kept.push_back(transition);
            handled.push_back(name);
            continue;
        }
        handled.push_back(name);
        std::optional<AnimatedValue> const current = transition_now(*transition, *property);
        auto const [duration, delay] = timing_of(found->second);
        bool const keep_going = end && current && !same_animated(*current, *end) && duration + delay > 0
            && interpolate_animated(*property, *current, *end, 0.5);
        double const progress = transition->effect ? transition->effect->computed_timing().progress.value_or(0) : 0;
        double const old_shortening = transition->transition_shortening;
        std::optional<AnimatedValue> const reversing_start = transition->transition_reversing_start;
        std::optional<AnimatedValue> const old_end = transition->transition_end;
        transition->cancel();
        if (!keep_going)
            continue;
        if (reversing_start && same_animated(*reversing_start, *end) && old_end) {
            double const shortening = std::clamp(std::fabs(progress * old_shortening + (1 - old_shortening)), 0.0, 1.0);
            start(name, *current, *end, found->second, *old_end, shortening);
        } else {
            start(name, *current, *end, found->second, *current, 1);
        }
    }
    // A completed transition stays while the value is its end.
    for (std::shared_ptr<Animation> const& transition : completed) {
        AnimatableProperty const* const property = animatable_property(transition->css_name);
        std::optional<AnimatedValue> const end = property ? read_animated(*property, after) : std::nullopt;
        if (!displayed || !end || !transition->transition_end || !same_animated(*end, *transition->transition_end))
            continue;
        if (std::find(handled.begin(), handled.end(), transition->css_name) != handled.end())
            continue;
        handled.push_back(transition->css_name);
        kept.push_back(transition);
    }
    // Then a new transition for each named property whose value changed —
    // never for a box that was not rendered before the change.
    if (displayed && before && !before->undisplayed) {
        for (auto const& [name, i] : matched) {
            if (std::find(handled.begin(), handled.end(), name) != handled.end()
                || std::find(animated.begin(), animated.end(), name) != animated.end())
                continue;
            auto const [duration, delay] = timing_of(i);
            if (duration + delay <= 0)
                continue;
            AnimatableProperty const* const property = animatable_property(name);
            if (is_current_color(*property, *before) && is_current_color(*property, after))
                continue;
            std::optional<AnimatedValue> const from = read_animated(*property, *before);
            std::optional<AnimatedValue> const to = read_animated(*property, after);
            if (!from || !to || same_animated(*from, *to) || !interpolate_animated(*property, *from, *to, 0.5))
                continue;
            start(name, *from, *to, i, *from, 1);
        }
    }
    entry = std::find_if(m_transitions.begin(), m_transitions.end(),
        [&](Transitions const& held) { return held.element == &element && held.pseudo == pseudo; });
    if (kept.empty()) {
        if (entry != m_transitions.end())
            m_transitions.erase(entry);
    } else if (entry != m_transitions.end()) {
        entry->running = std::move(kept);
    } else {
        m_transitions.push_back({ &element, pseudo, std::move(kept) });
    }
}

bool composite_order_before(Animation const& a, Animation const& b)
{
    int const class_a = animation_class(a);
    int const class_b = animation_class(b);
    if (class_a != class_b)
        return class_a < class_b;
    if (class_a < 2 && a.owning_element != b.owning_element)
        return tree_order_before(a.owning_element, b.owning_element);
    if (class_a == 1 && a.css_index != b.css_index)
        return a.css_index < b.css_index;
    return a.sequence < b.sequence;
}

// --- Sampling -------------------------------------------------------------------------------------

namespace {
thread_local Animation const* t_sample_limit = nullptr;
}

void set_sample_limit(Animation const* animation) { t_sample_limit = animation; }

void set_frame_restyle(bool restyling) { t_frame_restyle = restyling; }

namespace {

// One property's value at a point of the effect stack: a typed value where
// it has one, the style it comes from (for a discrete step), or a custom
// property's declared value.
struct StackValue {
    std::optional<AnimatedValue> typed;
    std::shared_ptr<ComputedStyle const> style;
    std::optional<Declaration> custom;
};

// The keyframes' values, computed once per sampling of a box: a keyframe
// value names a declaration, and a shorthand's style serves each longhand
// it holds.
class KeyframeValues {
public:
    KeyframeValues(KeyframeComputer& computer)
        : m_computer(computer)
    {
    }

    std::shared_ptr<ComputedStyle const> style(Keyframe::Value const& value)
    {
        auto const found = m_styles.find(&value);
        if (found != m_styles.end())
            return found->second;
        Declaration declaration;
        declaration.name = value.property;
        declaration.value = value.value;
        auto made = std::make_shared<ComputedStyle const>(m_computer.computed_with(declaration));
        m_styles.emplace(&value, made);
        return made;
    }

private:
    KeyframeComputer& m_computer;
    std::unordered_map<Keyframe::Value const*, std::shared_ptr<ComputedStyle const>> m_styles;
};

StackValue keyframe_value(KeyframeValues& values, Keyframe::Value const& value, std::string const& longhand,
    AnimatableProperty const* property)
{
    StackValue out;
    if (value.typed) {
        out.typed = value.typed;
        return out;
    }
    if (longhand.starts_with("--")) {
        Declaration declaration;
        declaration.name = longhand;
        declaration.value = value.value;
        out.custom = std::move(declaration);
        return out;
    }
    out.style = values.style(value);
    if (property) {
        out.typed = read_declared(*property, value.value);
        if (!out.typed)
            out.typed = read_animated(*property, *out.style);
    }
    return out;
}

StackValue combine(AnimatableProperty const* property, StackValue const& underlying, StackValue const& value,
    CompositeOperation operation)
{
    if (operation == CompositeOperation::Replace || !property || !underlying.typed || !value.typed)
        return value;
    std::optional<AnimatedValue> const combined = operation == CompositeOperation::Add
        ? add_animated(*property, *underlying.typed, *value.typed)
        : accumulate_animated(*property, *underlying.typed, *value.typed, 1);
    if (!combined)
        return value;
    StackValue out = value;
    out.typed = combined;
    return out;
}

// Each name the keyframes give, with the physical longhands it sets on this
// box: a flow-relative name mapped by the box's writing mode and direction,
// a shorthand that maps to opened into its longhands.
std::vector<std::pair<std::string, std::string>> physical_longhands(KeyframeComputer& computer,
    std::vector<std::string> const& names)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (std::string const& name : names) {
        if (name.starts_with("--")) {
            out.emplace_back(name, name);
            continue;
        }
        for (std::string const& physical : computer.physical_names(name)) {
            if (std::vector<std::string_view> const* const longhands = shorthand_longhands(physical)) {
                for (std::string_view const longhand : *longhands)
                    out.emplace_back(name, std::string(longhand));
            } else {
                out.emplace_back(name, physical);
            }
        }
    }
    return out;
}

// Â§5.3.5 "the effect value of a keyframe effect", for one property.
// `named` is the property as the keyframes name it, `longhand` the physical
// longhand of this box it sets.
StackValue effect_value(KeyframeEffect const& effect, ComputedTiming const& timing, std::string const& named,
    std::string const& longhand, AnimatableProperty const* property, StackValue const& underlying, KeyframeValues& values)
{
    struct Entry {
        double offset;
        Easing const* easing;
        std::optional<CompositeOperation> composite;
        StackValue value;
        bool neutral;
    };
    std::vector<Entry> keyframes;
    auto const& expanded = effect.expanded();
    for (std::size_t k = 0; k < effect.keyframes().size(); ++k) {
        auto const found = expanded[k].find(named);
        if (found == expanded[k].end())
            continue;
        Keyframe const& keyframe = effect.keyframes()[k];
        keyframes.push_back({ keyframe.computed_offset, &keyframe.easing, keyframe.composite,
            keyframe_value(values, keyframe.values[found->second], longhand, property), false });
    }
    if (keyframes.empty())
        return underlying;
    // The missing ends are the underlying value itself (a neutral keyframe
    // composited onto it adds nothing).
    if (std::none_of(keyframes.begin(), keyframes.end(), [](Entry const& e) { return e.offset == 0; }))
        keyframes.insert(keyframes.begin(), { 0, &effect.neutral_easing, CompositeOperation::Replace, underlying, true });
    if (std::none_of(keyframes.begin(), keyframes.end(), [](Entry const& e) { return e.offset == 1; }))
        keyframes.push_back({ 1, &effect.neutral_easing, CompositeOperation::Replace, underlying, true });
    double const progress = *timing.progress;
    std::vector<Entry*> ends;
    auto const count_at = [&](double offset) {
        return std::count_if(keyframes.begin(), keyframes.end(), [offset](Entry const& e) { return e.offset == offset; });
    };
    if (progress < 0 && count_at(0) > 1) {
        ends.push_back(&keyframes.front());
    } else if (progress >= 1 && count_at(1) > 1) {
        ends.push_back(&keyframes.back());
    } else {
        std::ptrdiff_t start = -1;
        for (std::size_t i = 0; i < keyframes.size(); ++i) {
            if (keyframes[i].offset <= progress && keyframes[i].offset < 1)
                start = static_cast<std::ptrdiff_t>(i);
        }
        if (start < 0) {
            for (std::size_t i = 0; i < keyframes.size(); ++i) {
                if (keyframes[i].offset == 0)
                    start = static_cast<std::ptrdiff_t>(i);
            }
        }
        if (start < 0)
            start = 0;
        if (static_cast<std::size_t>(start) + 1 >= keyframes.size())
            start = static_cast<std::ptrdiff_t>(keyframes.size()) - 2;
        ends.push_back(&keyframes[static_cast<std::size_t>(start)]);
        ends.push_back(&keyframes[static_cast<std::size_t>(start) + 1]);
    }
    // An iteration's accumulation, then the composite operation, on each end.
    double const iteration = timing.current_iteration.value_or(0);
    StackValue const final_value = keyframes.back().value;
    for (Entry* end : ends) {
        if (end->neutral)
            continue;
        if (effect.iteration_composite == IterationComposite::Accumulate && property && iteration > 0
            && std::isfinite(iteration) && end->value.typed && final_value.typed) {
            if (std::optional<AnimatedValue> const accumulated
                = accumulate_animated(*property, *end->value.typed, *final_value.typed, iteration))
                end->value.typed = accumulated;
        }
        end->value = combine(property, underlying, end->value, end->composite.value_or(effect.composite));
    }
    if (ends.size() == 1)
        return ends[0]->value;
    Entry const& from = *ends[0];
    Entry const& to = *ends[1];
    double const distance = to.offset == from.offset ? 0 : (progress - from.offset) / (to.offset - from.offset);
    double const eased = from.easing->apply(distance);
    if (property && from.value.typed && to.value.typed) {
        if (std::optional<AnimatedValue> const between = interpolate_animated(*property, *from.value.typed, *to.value.typed, eased)) {
            StackValue out = eased < 0.5 ? from.value : to.value;
            out.typed = between;
            return out;
        }
    }
    // display: between none and a box, the box throughout the interval
    // (css-display-4 Â§2.9), so that a fade to display: none is seen.
    if (longhand == "display" && from.value.style && to.value.style && eased > 0 && eased < 1) {
        bool const from_none = from.value.style->display == Display::None;
        bool const to_none = to.value.style->display == Display::None;
        if (from_none != to_none)
            return from_none ? to.value : from.value;
    }
    return eased < 0.5 ? from.value : to.value;
}

}

std::vector<AnimatedProperty> DocumentAnimations::sample(dom::Element const& element, PseudoElement pseudo,
    ComputedStyle const& base, KeyframeComputer& computer, bool transitions) const
{
    std::vector<Animation const*> stack;
    for_each_effect([&](KeyframeEffect const& effect) {
        Animation const* const animation = effect.animation;
        if (!animation || effect.target != &element || effect.pseudo != pseudo)
            return;
        if (t_sample_limit && animation != t_sample_limit && composite_order_before(*t_sample_limit, *animation))
            return;
        if (animation->replace_state == ReplaceState::Removed && animation != t_sample_limit)
            return;
        bool const transition = animation->kind == Animation::Kind::CssTransition && animation->owning_element;
        if (transition != transitions)
            return;
        stack.push_back(animation);
    });
    if (stack.empty())
        return {};
    std::stable_sort(stack.begin(), stack.end(), [](Animation const* a, Animation const* b) { return composite_order_before(*a, *b); });
    auto const base_style = std::make_shared<ComputedStyle const>(base);
    KeyframeValues values(computer);
    // property â its value up the stack so far
    std::map<std::string, StackValue> results;
    for (Animation const* animation : stack) {
        KeyframeEffect const& effect = *animation->effect;
        ComputedTiming timing = effect.computed_timing();
        if (animation == t_sample_limit) {
            // What commitStyles() keeps: the value at this time, with a time
            // exactly at the end of the interval counting as in it.
            timing = compute_timing(effect.timing, effect.local_time(), animation->playback_rate < 0, true);
        }
        if (!timing.progress)
            continue;
        for (auto const& [named, longhand] : physical_longhands(computer, effect.target_properties())) {
            AnimatableProperty const* const property = animatable_property(longhand);
            auto found = results.find(longhand);
            if (found == results.end()) {
                StackValue underlying;
                if (longhand.starts_with("--")) {
                    if (base.custom) {
                        if (std::vector<ComponentValue> const* const value = base.custom->find(longhand)) {
                            Declaration declaration;
                            declaration.name = longhand;
                            declaration.value = *value;
                            underlying.custom = std::move(declaration);
                        }
                    }
                } else {
                    underlying.style = base_style;
                    if (property)
                        underlying.typed = read_animated(*property, base);
                }
                found = results.emplace(longhand, std::move(underlying)).first;
            }
            found->second = effect_value(effect, timing, named, longhand, property, found->second, values);
        }
    }
    std::vector<AnimatedProperty> out;
    for (auto& [name, value] : results) {
        AnimatedProperty result;
        result.name = name;
        result.property = animatable_property(name);
        if (name.starts_with("--")) {
            if (!value.custom)
                continue;
            result.custom = std::move(value.custom);
        } else if (value.typed && result.property) {
            result.value = std::move(value.typed);
        } else {
            if (!value.style || value.style == base_style)
                continue;
            result.discrete = std::move(value.style);
        }
        out.push_back(std::move(result));
    }
    return out;
}

}
