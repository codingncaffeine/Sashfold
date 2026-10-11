#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"

#include "css/ComputedStyle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// ResizeObserver (W3C Resize Observer): a page asks to be told when an
// element's box changes size, and is told after layout, in one batch per
// observer, with the sizes of the box it asked about.
//
// The components that size themselves to their container lean on it: a
// carousel works out how many slides fit, a virtual list how tall each row
// came out, a chart how wide to draw. One that is told once and never again
// keeps the size it saw first while the window, the fonts and the pictures
// arriving move everything around it.
//
// The algorithm is the specification's (§3.4): each observation keeps the
// size it last reported, starting from none, so that the first update
// reports every target; an update gathers the observations whose size
// moved, delivers them shallowest first, and goes round again for the ones
// deeper than anything delivered, so that a callback that resizes what is
// inside its target is answered in the same update and one that resizes
// its own ancestors is not answered forever.

namespace sashfold::bindings {

namespace {

enum class ObservedBox : std::uint8_t { Content, Border, DevicePixelContent };

struct Size {
    double inline_size = 0;
    double block_size = 0;
    bool operator==(Size const&) const = default;
};

// What a box measures now: its content box (and where that sits inside the
// border box), its border box, and its content box in device pixels.
struct Measured {
    double content_left = 0;
    double content_top = 0;
    double content_width = 0;
    double content_height = 0;
    Size content;
    Size border;
    Size device;
};

struct Observation {
    dom::Element* target = nullptr;
    js::Value wrapper; // the target's wrapper, kept while it is observed
    ObservedBox box = ObservedBox::Content;
    // What the last delivery said of the observed box; none at first, so
    // that the first update reports it whatever its size.
    std::optional<Size> last;
};

class ResizeObserverObject final : public js::Object {
public:
    ResizeObserverObject(js::Object* prototype, js::Value the_callback)
        : Object(prototype, Class::Host)
        , callback(the_callback)
    {
    }
    js::Value callback;
    std::vector<Observation> observations;

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(callback);
        for (Observation const& observation : observations)
            tracer.visit(observation.wrapper);
    }
    std::size_t size_in_bytes() const override { return Object::size_in_bytes() + observations.size() * sizeof(Observation); }
};

// A ResizeObserverEntry: its target and what it measured, the size lists
// frozen arrays of ResizeObserverSize made when the entry was.
class ResizeEntryObject final : public js::Object {
public:
    explicit ResizeEntryObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    js::Value target;
    js::Value content_rect;
    js::Value border_box_size;
    js::Value content_box_size;
    js::Value device_pixel_content_box_size;

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(target);
        tracer.visit(content_rect);
        tracer.visit(border_box_size);
        tracer.visit(content_box_size);
        tracer.visit(device_pixel_content_box_size);
    }
};

class ResizeSizeObject final : public js::Object {
public:
    ResizeSizeObject(js::Object* prototype, Size the_size)
        : Object(prototype, Class::Host)
        , size(the_size)
    {
    }
    Size size;
};

std::optional<ResizeObserverObject*> this_observer(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<ResizeObserverObject*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

// Whether the element's box is a non-replaced inline one, which has no size
// to observe (§3.1: its sizes are all 0).
bool non_replaced_inline(dom::Element const& target, css::ComputedStyle const& style)
{
    if (style.display != css::Display::Inline || !target.is_html())
        return false;
    for (std::string_view const replaced : { "img", "video", "canvas", "iframe", "embed", "object", "input", "select", "textarea", "button", "audio" }) {
        if (target.local_name() == replaced)
            return false;
    }
    return true;
}

Measured measure(Realm::Internals& in, dom::Element& target)
{
    Measured out;
    if (!target.is_connected())
        return out;
    css::ComputedStyle const* const style = in.hooks.computed_style ? in.hooks.computed_style(target) : nullptr;
    if (style != nullptr && non_replaced_inline(target, *style))
        return out;
    std::optional<LayoutBox> const box = client_box(in, target);
    if (!box)
        return out;
    double const width = static_cast<double>(box->width);
    double const height = static_cast<double>(box->height);
    double const scale = in.hooks.device_scale > 0 ? static_cast<double>(in.hooks.device_scale) : 1.0;
    bool const vertical = style != nullptr && css::is_vertical(style->writing_mode);
    // An element inside an <svg> has no padding or border box: its content
    // box is its bounding box. Any other's padding is as layout used it.
    bool const svg_inner = !target.is_html() && target.namespace_uri() == dom::ns::svg && !target.is_svg("svg");
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    if (style != nullptr && !svg_inner) {
        out.content_left = static_cast<double>(box->padding_left);
        out.content_top = static_cast<double>(box->padding_top);
        left = static_cast<double>(style->border_left.width) / scale + out.content_left;
        right = static_cast<double>(style->border_right.width) / scale + static_cast<double>(box->padding_right);
        top = static_cast<double>(style->border_top.width) / scale + out.content_top;
        bottom = static_cast<double>(style->border_bottom.width) / scale + static_cast<double>(box->padding_bottom);
    }
    out.content_width = std::max(0.0, width - left - right);
    out.content_height = std::max(0.0, height - top - bottom);
    auto const sized = [vertical](double w, double h) { return vertical ? Size { h, w } : Size { w, h }; };
    out.content = sized(out.content_width, out.content_height);
    out.border = sized(width, height);
    out.device = sized(std::round(out.content_width * scale), std::round(out.content_height * scale));
    return out;
}

Size observed_size(Measured const& measured, ObservedBox box)
{
    switch (box) {
    case ObservedBox::Border: return measured.border;
    case ObservedBox::DevicePixelContent: return measured.device;
    case ObservedBox::Content: break;
    }
    return measured.content;
}

// How deep a node stands in the flat tree: what a delivery's shallowest
// target is measured against (§3.4.3).
std::size_t depth_of(dom::Node const& node)
{
    std::size_t depth = 0;
    for (dom::Node const* at = node.parent_or_host(); at != nullptr; at = at->parent_or_host())
        ++depth;
    return depth;
}

js::Value size_list(Realm::Internals& in, Size size)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    js::Value const one = interpreter.root(js::Value::object(interpreter.heap().allocate<ResizeSizeObject>(in.prototype("ResizeObserverSize"), size)));
    js::Value const items[1] = { one };
    js::ArrayObject* const list = interpreter.new_array(items);
    interpreter.root(js::Value::object(list));
    static_cast<void>(js::set_integrity_level(interpreter, *list, true)); // a FrozenArray
    return js::Value::object(list);
}

// The observations left undelivered are reported as an exception at the
// window (§3.4.5 step 2.2), an ErrorEvent whose error is null, as the
// browsers report it; said on the console when nothing cancels it.
void report_loop_limit(Realm::Internals& in)
{
    static constexpr std::string_view message = "ResizeObserver loop completed with undelivered notifications.";
    if (!in.reporting_exception && !in.ended) {
        in.reporting_exception = true;
        js::Interpreter::Roots const roots(in.interpreter);
        EventObject* const event = in.new_event("ErrorEvent", "error", false, true);
        in.interpreter.root(js::Value::object(event));
        event->is_trusted = true;
        event->message = std::string(message);
        event->detail_value = js::Value::null();
        bool const handled = !in.dispatch(*event, in.window_proxy());
        in.reporting_exception = false;
        if (handled)
            return;
    }
    in.console("error", std::string(message));
}

void forget_idle_observers(Realm::Internals& in)
{
    std::erase_if(in.resize_observation.observers, [](js::Object* object) {
        return static_cast<ResizeObserverObject*>(object)->observations.empty();
    });
}

// One round (§3.4.2-§3.4.4): the observations deeper than `shallower_than`
// whose size moved are delivered, each observer's at once; answers the
// shallowest depth delivered, or nothing when nothing was.
std::optional<std::size_t> deliver_round(Realm::Internals& in, std::size_t shallower_than, bool& skipped)
{
    js::Interpreter& interpreter = in.interpreter;
    std::optional<std::size_t> shallowest;
    std::vector<js::Object*> const observers = in.resize_observation.observers;
    for (js::Object* const object : observers) {
        auto* const observer = static_cast<ResizeObserverObject*>(object);
        js::Interpreter::Roots const roots(interpreter);
        interpreter.root(js::Value::object(observer));
        js::ArrayObject* const entries = interpreter.new_array();
        interpreter.root(js::Value::object(entries));
        for (Observation& observation : observer->observations) {
            Measured const measured = measure(in, *observation.target);
            Size const now = observed_size(measured, observation.box);
            if (observation.last && *observation.last == now)
                continue;
            std::size_t const depth = depth_of(*observation.target);
            if (depth <= shallower_than && shallower_than != std::numeric_limits<std::size_t>::max()) {
                skipped = true;
                continue;
            }
            observation.last = now;
            shallowest = std::min(shallowest.value_or(depth), depth);
            js::Interpreter::Roots const inner(interpreter);
            auto* const entry = interpreter.heap().allocate<ResizeEntryObject>(in.prototype("ResizeObserverEntry"));
            interpreter.root(js::Value::object(entry));
            entry->target = observation.wrapper;
            entry->content_rect = make_rect(in, measured.content_left, measured.content_top, measured.content_width, measured.content_height);
            entry->border_box_size = size_list(in, measured.border);
            entry->content_box_size = size_list(in, measured.content);
            entry->device_pixel_content_box_size = size_list(in, measured.device);
            entries->push(js::Value::object(entry));
        }
        if (entries->length() == 0)
            continue;
        js::Value const arguments[2] = { js::Value::object(entries), js::Value::object(observer) };
        in.call_reporting(observer->callback, js::Value::object(observer), arguments, "ResizeObserver callback");
        if (interpreter.terminated())
            break;
    }
    return shallowest;
}

}

bool update_resize_observations(Realm::Internals& in)
{
    auto& state = in.resize_observation;
    if (state.observers.empty())
        return false;
    // Measured again when something a size depends on moved — the tree, the
    // viewport — and otherwise on a clock, as pictures and fonts arriving
    // resize boxes with no change to the tree.
    // What the last update left undelivered waits for the next rendering
    // update (§3.4.5: "in the next event loop"), when the page has frames.
    if (state.waiting_for_frame && state.frame_at == in.agent.last_rendering_update && next_rendering_update(in.agent))
        return false;
    state.waiting_for_frame = false;
    double const now = in.now();
    std::uint64_t const mutations = in.realm.tree_mutation_count();
    bool const moved = state.mutations != mutations || state.viewport_width != in.hooks.viewport_width
        || state.viewport_height != in.hooks.viewport_height;
    if (!state.dirty && now - state.updated_at < (moved ? 16 : 250))
        return false;
    state.dirty = false;
    state.mutations = mutations;
    state.viewport_width = in.hooks.viewport_width;
    state.viewport_height = in.hooks.viewport_height;
    state.updated_at = now;
    bool delivered = false;
    bool skipped = false;
    std::size_t depth = std::numeric_limits<std::size_t>::max();
    for (int round = 0; round < 16 && !in.interpreter.terminated(); ++round) {
        std::optional<std::size_t> const shallowest = deliver_round(in, depth, skipped);
        if (!shallowest)
            break;
        delivered = true;
        depth = *shallowest;
    }
    // What was left for being no deeper than what was delivered waits for
    // the next update, and the page is told (§3.4.5), as the browsers tell it.
    if (skipped && !in.interpreter.terminated()) {
        state.dirty = true;
        state.waiting_for_frame = true;
        state.frame_at = in.agent.last_rendering_update;
        report_loop_limit(in);
    }
    return delivered;
}

void trace_resize_observers(Realm::Internals const& in, js::Tracer& tracer)
{
    for (js::Object* const observer : in.resize_observation.observers)
        tracer.visit(observer);
}

void install_resize_observer(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* const proto = define_interface(in, "ResizeObserver", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            js::Value const callback = js::argument(args, 0);
            if (!js::Interpreter::is_callable(callback))
                return interp.throw_type_error("Failed to construct 'ResizeObserver': parameter 1 is not of type 'Function'.");
            return js::Value::object(interp.heap().allocate<ResizeObserverObject>(internals_of(interp).prototype("ResizeObserver"), callback));
        },
        1);
    define_operation(interpreter, *proto, "observe", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<ResizeObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Value const target = js::argument(args, 0);
        dom::Node* const node = internals.realm.node_of(target);
        if (node == nullptr || !node->is_element())
            return interp.throw_type_error("Failed to execute 'observe' on 'ResizeObserver': parameter 1 is not of type 'Element'.");
        ObservedBox box = ObservedBox::Content;
        js::Value const options = js::argument(args, 1);
        if (options.is_object()) {
            std::optional<js::Value> const given = interp.get(options, "box");
            if (!given)
                return std::nullopt;
            if (!given->is_undefined()) {
                std::optional<std::string> const name = internals.to_utf8(*given);
                if (!name)
                    return std::nullopt;
                if (*name == "border-box")
                    box = ObservedBox::Border;
                else if (*name == "device-pixel-content-box")
                    box = ObservedBox::DevicePixelContent;
                else if (*name != "content-box")
                    return interp.throw_type_error("Failed to execute 'observe' on 'ResizeObserver': The provided value '" + *name
                        + "' is not a valid enum value of type ResizeObserverBoxOptions.");
            }
        }
        // Observing a target again replaces its observation (§3.3.2), unless
        // it is of the same box: then it is left as it is, with the size it
        // last reported, as Chromium and Gecko leave it.
        auto& observations = (*observer)->observations;
        auto const same = std::find_if(observations.begin(), observations.end(), [node](Observation const& o) { return o.target == node; });
        if (same != observations.end() && same->box == box)
            return js::Value::undefined();
        std::erase_if(observations, [node](Observation const& o) { return o.target == node; });
        observations.push_back(Observation { static_cast<dom::Element*>(node), target, box, std::nullopt });
        auto& observers = internals.resize_observation.observers;
        if (std::find(observers.begin(), observers.end(), *observer) == observers.end())
            observers.push_back(*observer);
        internals.resize_observation.dirty = true;
        return js::Value::undefined();
    });
    define_operation(interpreter, *proto, "unobserve", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<ResizeObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        dom::Node* const node = internals.realm.node_of(js::argument(args, 0));
        if (node == nullptr || !node->is_element())
            return interp.throw_type_error("Failed to execute 'unobserve' on 'ResizeObserver': parameter 1 is not of type 'Element'.");
        std::erase_if((*observer)->observations, [node](Observation const& o) { return o.target == node; });
        forget_idle_observers(internals);
        return js::Value::undefined();
    });
    define_operation(interpreter, *proto, "disconnect", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<ResizeObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        (*observer)->observations.clear();
        forget_idle_observers(internals_of(interp));
        return js::Value::undefined();
    });

    // The entries, their members on the prototype where a page that checks
    // for them looks.
    js::Object* const entry = define_interface(in, "ResizeObserverEntry", nullptr);
    auto const member = [&in, entry](std::string_view name, js::Value ResizeEntryObject::* field) {
        define_getter(in, *entry, name, [field](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            auto* const self = this_value.is_object() ? dynamic_cast<ResizeEntryObject*>(this_value.as_object()) : nullptr;
            if (self == nullptr)
                return interp.throw_type_error("Illegal invocation");
            return self->*field;
        });
    };
    member("target", &ResizeEntryObject::target);
    member("contentRect", &ResizeEntryObject::content_rect);
    member("borderBoxSize", &ResizeEntryObject::border_box_size);
    member("contentBoxSize", &ResizeEntryObject::content_box_size);
    member("devicePixelContentBoxSize", &ResizeEntryObject::device_pixel_content_box_size);
    js::Object* const size = define_interface(in, "ResizeObserverSize", nullptr);
    for (bool const block : { false, true }) {
        define_getter(in, *size, block ? "blockSize" : "inlineSize",
            block ? +[](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                auto* const self = this_value.is_object() ? dynamic_cast<ResizeSizeObject*>(this_value.as_object()) : nullptr;
                return self == nullptr ? interp.throw_type_error("Illegal invocation") : Native(js::Value::number(self->size.block_size));
            }
                  : +[](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
                        auto* const self = this_value.is_object() ? dynamic_cast<ResizeSizeObject*>(this_value.as_object()) : nullptr;
                        return self == nullptr ? interp.throw_type_error("Illegal invocation") : Native(js::Value::number(self->size.inline_size));
                    });
    }
}

}
