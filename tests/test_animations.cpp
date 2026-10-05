#include "Test.h"

#include "css/Animation.h"
#include "css/Easing.h"
#include "css/Interpolation.h"
#include "css/Parser.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The animation model without script: easing functions, the timing model,
// typed interpolation, the play state machine and what animations do to the
// cascade (css/Animation.h). The Web Platform Tests drive the same code
// through the API; these pin the parts a script cannot see and the cascade's
// order, each with a value worked out by hand.

using namespace sashfold;

namespace {

bool near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }
bool near(float a, double b, double tolerance = 1e-6) { return near(static_cast<double>(a), b, tolerance); }

css::Easing easing(std::string_view text)
{
    std::optional<css::Easing> const parsed = css::parse_easing_text(text);
    CHECK(parsed.has_value());
    return parsed.value_or(css::Easing {});
}

void test_easing()
{
    // The keywords as their Béziers; ease at its middle is the textbook 0.8024.
    CHECK(near(easing("ease").apply(0.5), 0.8024033877399112, 1e-6));
    CHECK(near(easing("linear").apply(0.3), 0.3));
    CHECK(near(easing("ease-in").apply(0), 0));
    CHECK(near(easing("ease-in").apply(1), 1));
    // Outside [0, 1] a Bézier carries on along its end tangents.
    CHECK(near(easing("cubic-bezier(0.5, 1, 0.5, 1)").apply(-1), -2));
    CHECK(!css::parse_easing_text("cubic-bezier(1.5, 0, 0, 1)")); // x out of range
    CHECK(css::parse_easing_text("cubic-bezier(0, -2, 1, 3)").has_value());
    // Steps: where each position puts its jumps, and the before flag.
    CHECK(near(easing("steps(4)").apply(0.3), 0.25));
    CHECK(near(easing("steps(4, start)").apply(0.3), 0.5));
    CHECK(near(easing("steps(3, jump-both)").apply(0), 0.25));
    CHECK(near(easing("steps(3, jump-none)").apply(0.5), 0.5));
    CHECK(near(easing("step-start").apply(0, true), 0)); // before flag: not yet jumped
    CHECK(near(easing("step-start").apply(0, false), 1));
    CHECK(!css::parse_easing_text("steps(1, jump-none)"));
    CHECK(!css::parse_easing_text("steps(0)"));
    CHECK_EQ(easing("steps(2, end)").serialize(), std::string("steps(2)"));
    CHECK_EQ(easing("step-start").serialize(), std::string("steps(1, start)"));
    // linear(): missing inputs spread evenly, extrapolated past the ends.
    css::Easing const points = easing("linear(0, 0.25 75%, 1)");
    CHECK_EQ(points.serialize(), std::string("linear(0 0%, 0.25 75%, 1 100%)"));
    CHECK(near(points.apply(0.375), 0.125));
    CHECK(near(points.apply(2), 4)); // the last segment's slope, 3 per unit
    CHECK_EQ(easing("linear(-10, -5, 0, 5, 10)").serialize(), std::string("linear(-10 0%, -5 25%, 0 50%, 5 75%, 10 100%)"));
    CHECK(!css::parse_easing_text("linear(0)"));
}

void test_timing()
{
    css::EffectTiming timing;
    timing.duration = 1000;
    timing.delay = 500;
    // Before the delay without a backwards fill: no progress at all.
    css::ComputedTiming at = css::compute_timing(timing, 200, false);
    CHECK(at.phase == css::AnimationPhase::Before);
    CHECK(!at.progress);
    timing.fill = css::FillMode::Both;
    at = css::compute_timing(timing, 200, false);
    CHECK(at.progress && near(*at.progress, 0));
    at = css::compute_timing(timing, 1000, false);
    CHECK(at.phase == css::AnimationPhase::Active);
    CHECK(at.progress && near(*at.progress, 0.5));
    // The end: the after phase with the last iteration's end held.
    at = css::compute_timing(timing, 1500, false);
    CHECK(at.phase == css::AnimationPhase::After);
    CHECK(at.progress && near(*at.progress, 1));
    // Iterations and directions: the third iteration of an alternate runs
    // forwards again, and the iteration start shifts where each begins.
    css::EffectTiming loop;
    loop.duration = 100;
    loop.iterations = 3;
    loop.direction = css::PlaybackDirection::Alternate;
    at = css::compute_timing(loop, 125, false);
    CHECK(at.current_iteration && near(*at.current_iteration, 1));
    CHECK(at.progress && near(*at.progress, 0.75));
    at = css::compute_timing(loop, 225, false);
    CHECK(at.progress && near(*at.progress, 0.25));
    loop.iteration_start = 0.5;
    loop.direction = css::PlaybackDirection::Normal;
    at = css::compute_timing(loop, 0, false);
    CHECK(at.progress && near(*at.progress, 0.5));
    // A zero duration: progress jumps at the start, iterations counted.
    css::EffectTiming instant;
    instant.iterations = 2;
    instant.fill = css::FillMode::Forwards;
    at = css::compute_timing(instant, 0, false);
    CHECK(at.phase == css::AnimationPhase::After);
    CHECK(at.current_iteration && near(*at.current_iteration, 1));
    // An inclusive end (what commitStyles samples) keeps the end in play.
    css::EffectTiming plain;
    plain.duration = 1000;
    CHECK(!css::compute_timing(plain, 1000, false).progress);
    at = css::compute_timing(plain, 1000, false, true);
    CHECK(at.progress && near(*at.progress, 1));
}

void test_interpolation()
{
    css::AnimatableProperty const* width = css::animatable_property("width");
    css::AnimatableProperty const* color = css::animatable_property("color");
    css::AnimatableProperty const* visibility = css::animatable_property("visibility");
    css::AnimatableProperty const* z_index = css::animatable_property("z-index");
    css::AnimatableProperty const* dashes = css::animatable_property("stroke-dasharray");
    CHECK(width && color && visibility && z_index && dashes);
    CHECK(!css::animatable_property("margin")); // a shorthand is no longhand
    if (!width || !color || !visibility || !z_index || !dashes)
        return;
    // A length meets a percentage as calc(): each part on its own.
    css::ComputedStyle from;
    from.width = css::LengthPercent::px(100);
    css::ComputedStyle to;
    to.width = css::LengthPercent::percent_of(50);
    std::optional<css::AnimatedValue> const half
        = css::interpolate_animated(*width, *css::read_animated(*width, from), *css::read_animated(*width, to), 0.5);
    CHECK(half && near(half->px, 50) && near(half->percent, 25));
    // auto does not interpolate: the property is discrete there.
    css::ComputedStyle automatic;
    CHECK(!css::read_animated(*width, automatic));
    // An overshoot past zero is held at zero for a size, not dropped.
    css::ComputedStyle written;
    unsigned sides = 0;
    css::AnimatedValue negative;
    negative.kind = css::AnimatedValue::Kind::Length;
    negative.px = -20;
    css::write_animated(*width, negative, written, sides);
    CHECK(written.width.kind == css::LengthPercent::Kind::Px && near(written.width.value, 0));
    // Colors premultiplied: transparent lends no color of its own.
    css::ComputedStyle red;
    red.color = Color::rgb(255, 0, 0);
    css::ComputedStyle clear;
    clear.color = Color::rgba(0, 0, 255, 0);
    std::optional<css::AnimatedValue> const mixed
        = css::interpolate_animated(*color, *css::read_animated(*color, red), *css::read_animated(*color, clear), 0.5);
    CHECK(mixed && near(mixed->r, 1) && near(mixed->b, 0) && near(mixed->a, 0.5));
    // visibility is visible anywhere strictly between visible and hidden.
    css::ComputedStyle hidden;
    hidden.visibility = css::Visibility::Hidden;
    std::optional<css::AnimatedValue> const between = css::interpolate_animated(*visibility,
        *css::read_animated(*visibility, hidden), *css::read_animated(*visibility, css::ComputedStyle {}), 0.1);
    CHECK(between && between->visible);
    // Integers round half up.
    css::AnimatedValue one;
    one.number = 1;
    css::AnimatedValue two;
    two.number = 2;
    std::optional<css::AnimatedValue> const rounded = css::interpolate_animated(*z_index, one, two, 0.5);
    CHECK(rounded && near(rounded->number, 2));
    // Lists of different lengths repeat to their least common multiple.
    css::AnimatedValue a;
    a.kind = css::AnimatedValue::Kind::Numbers;
    a.numbers = { 1, 2 };
    css::AnimatedValue b;
    b.kind = css::AnimatedValue::Kind::Numbers;
    b.numbers = { 3, 4, 5 };
    std::optional<css::AnimatedValue> const lists = css::interpolate_animated(*dashes, a, b, 0.5);
    CHECK(lists && lists->numbers.size() == 6 && near(lists->numbers[2], 3) && near(lists->numbers[5], 3.5));
}

dom::Element* find_by_id(dom::Node& node, std::string_view id)
{
    if (node.is_element()) {
        auto& element = static_cast<dom::Element&>(node);
        if (dom::Attr const* attribute = element.find_attribute("id"); attribute && attribute->value == id)
            return &element;
    }
    for (dom::Node* child : node.children()) {
        if (dom::Element* found = find_by_id(*child, id))
            return found;
    }
    return nullptr;
}

std::vector<css::ComponentValue> value(std::string_view text) { return css::parse_component_value_list(text); }

// An animation on `target` from `from` to `to` of one property, a second
// long, paused at `time` on a timeline at 0 — what the cascade then sees.
std::shared_ptr<css::Animation> paused_at(css::DocumentAnimations& animations, dom::Element& target, std::string const& property,
    std::string_view from, std::string_view to, double time, css::CompositeOperation composite = css::CompositeOperation::Replace)
{
    auto effect = std::make_shared<css::KeyframeEffect>();
    effect->set_target(&target);
    effect->timing.duration = 1000;
    effect->composite = composite;
    std::vector<css::Keyframe> keyframes(2);
    keyframes[0].values.push_back({ property, value(from), std::string(from), std::nullopt });
    keyframes[1].values.push_back({ property, value(to), std::string(to), std::nullopt });
    effect->set_keyframes(std::move(keyframes));
    auto animation = std::make_shared<css::Animation>(animations, css::Animation::Kind::Script);
    animation->set_timeline(animations.timeline);
    animation->set_effect(effect);
    static_cast<void>(animation->pause());
    static_cast<void>(animation->set_current_time(time));
    return animation;
}

void test_cascade()
{
    auto document = html::parse_document(std::string_view(R"(<!doctype html><html><head><style>
  #box { opacity: 0.9; width: 10em; font-size: 10px; color: rgb(0, 0, 0) }
  #strong { opacity: 0.9 !important }
</style></head><body><div id=box><span id=child>x</span></div><div id=strong></div></body></html>)"));
    dom::Element* const box = find_by_id(*document, "box");
    dom::Element* const child = find_by_id(*document, "child");
    dom::Element* const strong = find_by_id(*document, "strong");
    CHECK(box && child && strong);
    if (!box || !child || !strong)
        return;
    css::DocumentAnimations& animations = css::DocumentAnimations::of(*document);
    animations.now = 0;
    auto const fade = paused_at(animations, *box, "opacity", "0", "1", 250);
    auto const grow = paused_at(animations, *box, "font-size", "10px", "20px", 500);
    auto const tint = paused_at(animations, *box, "color", "rgb(0, 0, 0)", "rgb(200, 100, 0)", 500);
    auto const held = paused_at(animations, *strong, "opacity", "0", "1", 500);
    css::StyleMap styles = css::resolve_styles(*document);
    // The animation beats the author's normal declaration...
    CHECK(near(styles[box].opacity, 0.25, 1e-6));
    // ...but not an important one (css-cascade-5 §6.1).
    CHECK(near(styles[strong].opacity, 0.9, 1e-6));
    // An animated font-size is the element's em: 10em of 15px.
    CHECK(near(styles[box].font_size, 15, 1e-4));
    CHECK(styles[box].width.kind == css::LengthPercent::Kind::Px && near(styles[box].width.value, 150, 1e-3));
    // And the child inherits the animated color.
    CHECK_EQ(styles[child].color, Color::rgb(100, 50, 0));
    // An additive animation composites onto the value under it.
    auto const more = paused_at(animations, *box, "opacity", "0.1", "0.1", 0, css::CompositeOperation::Add);
    styles = css::resolve_styles(*document);
    CHECK(near(styles[box].opacity, 0.35, 1e-6));
    // Cancelled, the box is its own again.
    more->cancel();
    fade->cancel();
    styles = css::resolve_styles(*document);
    CHECK(near(styles[box].opacity, 0.9, 1e-6));
}

// An attribute set as the parser or a script would: taking the attributes
// to change them marks the element for a restyle.
void set_attribute(dom::Element& element, std::string const& name, std::string const& text)
{
    std::vector<dom::Attr>& attributes = element.attributes();
    for (dom::Attr& attribute : attributes) {
        if (attribute.local_name == name) {
            attribute.value = text;
            return;
        }
    }
    dom::Attr attribute;
    attribute.local_name = name;
    attribute.value = text;
    attributes.push_back(std::move(attribute));
}

// CSS animations and transitions through the cascade: an @keyframes rule
// named by animation-name, paused a quarter in by a negative delay; and a
// transition the style change between two updates starts, which runs
// from the value before to the value after.
void test_css_animations_and_transitions()
{
    auto document = html::parse_document(std::string_view("<!doctype html><div id=a></div><div id=t></div>"));
    dom::Element* const a = find_by_id(*document, "a");
    dom::Element* const t = find_by_id(*document, "t");
    CHECK(a && t);
    if (!a || !t)
        return;
    css::StyleSet const set(std::vector<css::SheetSource> { { std::string(
        "@keyframes fade { from { opacity: 0 } to { opacity: 1 } }"
        "#a { animation: fade 1s linear -0.25s paused }"
        "#t { transition: opacity 1s linear; opacity: 0 }"
        "#t.on { opacity: 1 }"), std::nullopt } });
    css::DocumentAnimations& animations = css::DocumentAnimations::of(*document);
    animations.now = 0;
    css::StyleMap styles;
    css::StyleRecord record;
    static_cast<void>(css::update_styles(*document, set, styles, record));
    CHECK(near(styles[a].opacity, 0.25, 1e-6));
    std::vector<std::shared_ptr<css::Animation>> const running = animations.relevant(a, false);
    CHECK(running.size() == 1 && running[0]->kind == css::Animation::Kind::CssAnimation && running[0]->css_name == "fade");
    // The first style starts nothing; the change does.
    CHECK(animations.relevant(t, false).empty());
    set_attribute(*t, "class", "on");
    static_cast<void>(css::update_styles(*document, set, styles, record));
    std::vector<std::shared_ptr<css::Animation>> const transitions = animations.relevant(t, false);
    CHECK(transitions.size() == 1 && transitions[0]->kind == css::Animation::Kind::CssTransition);
    CHECK(near(styles[t].opacity, 0, 1e-6)); // pending: at its start
    animations.update(16);
    animations.update(516);
    t->mark_style_self();
    static_cast<void>(css::update_styles(*document, set, styles, record));
    CHECK(near(styles[t].opacity, 0.5, 1e-3));
    // The same styles again start nothing more.
    t->mark_style_self();
    static_cast<void>(css::update_styles(*document, set, styles, record));
    CHECK(animations.relevant(t, false).size() == 1);
    // Taken out of the list, the transition ends.
    set_attribute(*t, "style", "transition: none");
    static_cast<void>(css::update_styles(*document, set, styles, record));
    CHECK(animations.relevant(t, false).empty());
    CHECK(near(styles[t].opacity, 1, 1e-6));
}

// A number written as calc(): the evaluator reads a length context, which
// must outlive it (the sanitizer lane is the test that sees it).
void test_calc_numbers()
{
    auto document = html::parse_document(std::string_view(R"(<!doctype html><html><head><style>
  #c { animation: none 1s; animation-iteration-count: calc(1 + 2), calc(0.5 * 3) }
  #d { animation: none 1s; animation-iteration-count: calc(10 + (sign(2cqw - 10px) * 5)) }
</style></head><body><div id=c></div><div id=d></div></body></html>)"));
    dom::Element* const c = find_by_id(*document, "c");
    dom::Element* const d = find_by_id(*document, "d");
    CHECK(c && d);
    if (!c || !d)
        return;
    css::StyleMap styles = css::resolve_styles(*document);
    // The suite's own input: a length inside is read against the context.
    // sign() is not written, so the count may be the initial one; whatever
    // it is, it is not negative.
    std::shared_ptr<css::AnimationLists const> const probe = styles[d].animation;
    CHECK(probe && !probe->iteration_counts.empty() && probe->iteration_counts[0] >= 0);
    std::shared_ptr<css::AnimationLists const> const lists = styles[c].animation;
    CHECK(lists && lists->iteration_counts.size() == 2);
    if (!lists || lists->iteration_counts.size() != 2)
        return;
    CHECK(near(lists->iteration_counts[0], 3, 1e-9));
    CHECK(near(lists->iteration_counts[1], 1.5, 1e-9));
}

void test_play_state()
{
    auto document = html::parse_document(std::string_view("<!doctype html><div id=t></div>"));
    dom::Element* const target = find_by_id(*document, "t");
    CHECK(target != nullptr);
    if (!target)
        return;
    css::DocumentAnimations& animations = css::DocumentAnimations::of(*document);
    animations.now = 100;
    auto effect = std::make_shared<css::KeyframeEffect>();
    effect->set_target(target);
    effect->timing.duration = 1000;
    auto animation = std::make_shared<css::Animation>(animations, css::Animation::Kind::Script);
    animation->set_timeline(animations.timeline);
    animation->set_effect(effect);
    CHECK(animation->play_state() == css::PlayState::Idle);
    CHECK(!animation->play());
    // Pending until the next rendering update; then running from there.
    CHECK(animation->pending());
    CHECK(animation->ready_pending);
    CHECK(animation->current_time() && near(*animation->current_time(), 0));
    animations.now = 116;
    animations.update(116);
    CHECK(!animation->pending());
    CHECK(animation->start_time && near(*animation->start_time, 116));
    animations.update(616);
    CHECK(animation->play_state() == css::PlayState::Running);
    CHECK(animation->current_time() && near(*animation->current_time(), 500));
    // Past the end: finished, held at the end, the finished promise resolved.
    animations.update(2000);
    CHECK(animation->play_state() == css::PlayState::Finished);
    CHECK(animation->current_time() && near(*animation->current_time(), 1000));
    CHECK(animation->finished_resolved);
    bool finish_event = false;
    for (css::AnimationEvent const& event : animations.pending_events())
        finish_event = finish_event || event.type == "finish";
    CHECK(finish_event);
    // reverse() plays it back from the end at rate -1.
    CHECK(!animation->reverse());
    animations.update(2100);
    animations.update(2600);
    CHECK(animation->playback_rate == -1);
    CHECK(animation->current_time() && near(*animation->current_time(), 500));
    // cancel(): idle, no times.
    animation->cancel();
    CHECK(animation->play_state() == css::PlayState::Idle);
    CHECK(!animation->current_time());
    // finish() forwards with an infinite end is refused.
    animation->set_playback_rate(1);
    effect->timing.iterations = std::numeric_limits<double>::infinity();
    std::optional<css::Animation::Refusal> const refused = animation->finish();
    CHECK(refused && refused->name == "InvalidStateError");
}

}

int main()
{
    test_easing();
    test_timing();
    test_interpolation();
    test_cascade();
    test_play_state();
    test_css_animations_and_transitions();
    test_calc_numbers();
    return test::report("test_animations");
}
