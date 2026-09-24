#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"

#include "css/ComputedStyle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

// IntersectionObserver (W3C Intersection Observer): a page asks to be told
// when an element comes into view, or leaves it, or crosses a fraction of
// its area it named — and is told at the end of a rendering update, in one
// batch, rather than having to measure for itself.
//
// Nearly every page that loads what it shows as the reader reaches it — the
// thumbnails of a video site, the next screen of a feed, the pictures of a
// long article — hangs that loading on one of these. An observer that says
// everything is in view loads everything at once; one that never speaks
// loads nothing; and a page that finds the interface incomplete brings its
// own, which measures by reading layout from script again and again.
//
// The algorithm is the specification's: each observation keeps the
// threshold its target last stood at and whether it was intersecting, and
// an entry is queued only when either changes. The rectangles are the
// target's border box, clipped by every ancestor on its containing-block
// chain that clips its overflow, then by the root's rectangle grown or
// shrunk by rootMargin.

namespace sashfold::bindings {

namespace {

struct Rect {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;

    double width() const { return right - left; }
    double height() const { return bottom - top; }
};

Rect rect_of(LayoutBox const& box)
{
    auto const x = static_cast<double>(box.x);
    auto const y = static_cast<double>(box.y);
    return Rect { x, y, x + static_cast<double>(box.width), y + static_cast<double>(box.height) };
}

// The overlap of two rectangles, or nothing when they are apart. Rectangles
// that only touch overlap in a line, which counts: an element whose edge
// meets the viewport's is intersecting with a ratio of zero.
std::optional<Rect> overlap(Rect const& a, Rect const& b)
{
    Rect const r { std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom) };
    if (r.right < r.left || r.bottom < r.top)
        return std::nullopt;
    return r;
}

struct MarginSide {
    double value = 0;
    bool percent = false;
};

// rootMargin (§3.1, "parse a root margin"): one to four lengths in pixels
// or percentages, expanded like the margin shorthand: top, right, bottom,
// left.
std::optional<std::array<MarginSide, 4>> parse_root_margin(std::string_view text)
{
    std::vector<MarginSide> sides;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == '\f'))
            ++i;
        std::size_t const start = i;
        while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == '\f'))
            ++i;
        if (i == start)
            break;
        std::string token(text.substr(start, i - start));
        MarginSide side;
        std::string number;
        if (token.size() > 2 && (token.ends_with("px") || token.ends_with("PX") || token.ends_with("Px") || token.ends_with("pX"))) {
            number = token.substr(0, token.size() - 2);
        } else if (token.size() > 1 && token.back() == '%') {
            number = token.substr(0, token.size() - 1);
            side.percent = true;
        } else {
            return std::nullopt;
        }
        char* end = nullptr;
        double const value = std::strtod(number.c_str(), &end);
        if (end == number.c_str() || *end != '\0' || !std::isfinite(value))
            return std::nullopt;
        side.value = value;
        sides.push_back(side);
    }
    if (sides.empty() || sides.size() > 4)
        return std::nullopt;
    std::array<MarginSide, 4> out {};
    out[0] = sides[0];
    out[1] = sides.size() > 1 ? sides[1] : sides[0];
    out[2] = sides.size() > 2 ? sides[2] : sides[0];
    out[3] = sides.size() > 3 ? sides[3] : out[1];
    return out;
}

std::string serialize_root_margin(std::array<MarginSide, 4> const& margin)
{
    std::string out;
    for (MarginSide const& side : margin) {
        if (!out.empty())
            out += ' ';
        out += js::number_to_utf8(side.value);
        out += side.percent ? "%" : "px";
    }
    return out;
}

struct Observation {
    dom::Element* target = nullptr;
    js::Value wrapper; // the target's wrapper, kept while it is observed
    // What the last update found: the threshold's index (none before the
    // first) and whether it was intersecting.
    int previous_threshold_index = -1;
    bool previous_is_intersecting = false;
};

class IntersectionObserverObject final : public js::Object {
public:
    IntersectionObserverObject(js::Object* prototype, js::Value the_callback)
        : Object(prototype, Class::Host)
        , callback(the_callback)
    {
    }
    js::Value callback;
    js::Value root = js::Value::null(); // an element, a document, or null for the viewport
    std::array<MarginSide, 4> margin {};
    std::vector<double> thresholds { 0.0 };
    std::vector<Observation> observations;
    std::vector<js::Value> entries; // queued, not yet delivered

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(callback);
        tracer.visit(root);
        for (Observation const& observation : observations)
            tracer.visit(observation.wrapper);
        for (js::Value const& entry : entries)
            tracer.visit(entry);
    }
    std::size_t size_in_bytes() const override
    {
        return Object::size_in_bytes() + observations.size() * sizeof(Observation) + entries.size() * sizeof(js::Value)
            + thresholds.size() * sizeof(double);
    }
};

class IntersectionEntryObject final : public js::Object {
public:
    explicit IntersectionEntryObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    double time = 0;
    js::Value root_bounds = js::Value::null();
    js::Value bounding_client_rect;
    js::Value intersection_rect;
    bool is_intersecting = false;
    double ratio = 0;
    js::Value target;

    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(root_bounds);
        tracer.visit(bounding_client_rect);
        tracer.visit(intersection_rect);
        tracer.visit(target);
    }
};

std::optional<IntersectionObserverObject*> this_observer(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<IntersectionObserverObject*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

std::optional<IntersectionEntryObject*> this_entry(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<IntersectionEntryObject*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

// Whether a box clips what it holds: overflow other than visible on either
// axis, where overflow applies at all.
bool clips(css::ComputedStyle const& style)
{
    return style.overflow_applies
        && (style.overflow_x != css::Overflow::Visible || style.overflow_y != css::Overflow::Visible);
}

// The viewport of the observer's document, in client coordinates.
Rect viewport_rect(Realm::Internals& in)
{
    return Rect { 0, 0, static_cast<double>(in.hooks.viewport_width), static_cast<double>(in.hooks.viewport_height) };
}

// The root intersection rectangle (§3.2.4): the viewport for the implicit
// root and for a document, the root element's box otherwise, each grown or
// shrunk by rootMargin — a percentage of the rectangle's own width for the
// left and right sides, of its height for the top and bottom.
std::optional<Rect> root_rect(Realm::Internals& in, IntersectionObserverObject const& observer)
{
    Rect base;
    dom::Node* const root = observer.root.is_null() ? nullptr : in.realm.node_of(observer.root);
    if (root == nullptr || !root->is_element()) {
        base = viewport_rect(in);
    } else {
        std::optional<LayoutBox> const box = client_box(in, static_cast<dom::Element const&>(*root));
        if (!box)
            return std::nullopt;
        base = rect_of(*box);
    }
    auto const amount = [](MarginSide const& side, double extent) { return side.percent ? side.value * extent / 100.0 : side.value; };
    double const width = base.width();
    double const height = base.height();
    return Rect { base.left - amount(observer.margin[3], width), base.top - amount(observer.margin[0], height),
        base.right + amount(observer.margin[1], width), base.bottom + amount(observer.margin[2], height) };
}

struct Measured {
    Rect target;
    Rect intersection;
    std::optional<Rect> root;
    bool is_intersecting = false;
    double ratio = 0;
};

// Computes one target against its observer's root (§3.2.8 steps 1-11).
Measured measure(Realm::Internals& in, IntersectionObserverObject const& observer, dom::Element& target)
{
    Measured out;
    out.root = root_rect(in, observer);
    if (!target.is_connected() || !out.root)
        return out;
    dom::Node* const root = observer.root.is_null() ? nullptr : in.realm.node_of(observer.root);
    dom::Element const* const root_element = root != nullptr && root->is_element() ? static_cast<dom::Element const*>(root) : nullptr;
    // An explicit root sees only what is inside it.
    if (root_element != nullptr) {
        bool inside = false;
        for (dom::Node* ancestor = target.parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
            if (ancestor == root_element) {
                inside = true;
                break;
            }
        }
        if (!inside)
            return out;
    }
    std::optional<LayoutBox> const box = client_box(in, target);
    if (!box)
        return out;
    out.target = rect_of(*box);

    // The containing-block chain from the target up to the root: an
    // absolutely positioned box skips the static ancestors between it and
    // its positioned one, and a fixed box answers to the viewport alone.
    std::optional<Rect> clipped = out.target;
    auto const style_of = [&in](dom::Element const& element) -> css::ComputedStyle const* {
        return in.hooks.computed_style ? in.hooks.computed_style(element) : nullptr;
    };
    css::ComputedStyle const* const own = style_of(target);
    bool skip_static = own != nullptr && own->position == css::Position::Absolute;
    bool to_viewport = own != nullptr && own->position == css::Position::Fixed;
    for (dom::Node* node = target.parent(); node != nullptr && clipped && !to_viewport; node = node->parent()) {
        if (!node->is_element())
            break;
        auto const& ancestor = static_cast<dom::Element const&>(*node);
        if (&ancestor == root_element)
            break;
        css::ComputedStyle const* const style = style_of(ancestor);
        if (style == nullptr)
            continue;
        if (skip_static && style->position == css::Position::Static)
            continue;
        // The root element's overflow belongs to the viewport, which the
        // root rectangle already stands for.
        if (clips(*style) && ancestor.parent() != static_cast<dom::Node const*>(in.document)) {
            if (std::optional<LayoutBox> const clip = client_box(in, ancestor))
                clipped = overlap(*clipped, rect_of(*clip));
            else
                clipped.reset();
        }
        skip_static = style->position == css::Position::Absolute;
        to_viewport = style->position == css::Position::Fixed;
    }
    if (clipped)
        clipped = overlap(*clipped, *out.root);
    if (!clipped)
        return out;
    out.intersection = *clipped;
    out.is_intersecting = true;
    double const target_area = out.target.width() * out.target.height();
    double const intersection_area = out.intersection.width() * out.intersection.height();
    out.ratio = target_area > 0 ? std::min(1.0, intersection_area / target_area) : 1.0;
    return out;
}

// The index of the first threshold above the ratio, or the count when none
// is; zero for a target that is not intersecting at all.
int threshold_index(std::vector<double> const& thresholds, Measured const& measured)
{
    if (!measured.is_intersecting)
        return 0;
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        if (thresholds[i] > measured.ratio)
            return static_cast<int>(i);
    }
    return static_cast<int>(thresholds.size());
}

js::Value rect_value(Realm::Internals& in, Rect const& rect)
{
    return make_rect(in, rect.left, rect.top, rect.width(), rect.height());
}

// Delivers what every observer of this document has queued, each its own
// entries in the order they were queued (§3.2.6 "notify intersection
// observers").
void arrange_delivery(Realm::Internals& in)
{
    if (in.intersection.delivery_pending)
        return;
    in.intersection.delivery_pending = true;
    in.post_task([&in] {
        in.intersection.delivery_pending = false;
        js::Interpreter& interpreter = in.interpreter;
        // Copied first: a callback may make observers or drop them.
        std::vector<js::Object*> const observers = in.intersection.observers;
        for (js::Object* const object : observers) {
            auto* const observer = static_cast<IntersectionObserverObject*>(object);
            if (observer->entries.empty())
                continue;
            js::Interpreter::Roots const roots(interpreter);
            interpreter.root(js::Value::object(observer));
            js::ArrayObject* const list = interpreter.new_array();
            interpreter.root(js::Value::object(list));
            for (js::Value const& entry : observer->entries)
                list->push(entry);
            observer->entries.clear();
            js::Value const arguments[2] = { js::Value::object(list), js::Value::object(observer) };
            in.call_reporting(observer->callback, js::Value::object(observer), arguments, "IntersectionObserver callback");
        }
    });
}

void forget_idle_observers(Realm::Internals& in)
{
    std::erase_if(in.intersection.observers, [](js::Object* object) {
        auto* const observer = static_cast<IntersectionObserverObject*>(object);
        return observer->observations.empty() && observer->entries.empty();
    });
}

} // namespace

bool update_intersection_observations(Realm::Internals& in, bool force)
{
    if (in.intersection.observers.empty())
        return false;
    // Nothing a target's place depends on has moved, and it was not long
    // ago: the answer would be the same. Pictures and fonts arriving move
    // boxes without a change to the tree, so the answer is also refreshed
    // on a clock.
    double const now = in.now();
    std::pair<int, int> const scroll = in.hooks.scroll_position ? in.hooks.scroll_position(*in.document) : std::pair<int, int> { 0, 0 };
    std::uint64_t const mutations = in.realm.tree_mutation_count();
    auto& state = in.intersection;
    // A new target is measured at once. Otherwise a moved tree, scroll or
    // viewport is measured at most every fiftieth of a second of the page's
    // clock — each measurement lays the page out, and an application that
    // changes its tree every turn would pay for a layout every turn — and
    // an unmoved page every quarter second.
    bool const moved = state.mutations != mutations || state.scroll != scroll
        || state.viewport_width != in.hooks.viewport_width || state.viewport_height != in.hooks.viewport_height;
    if (!force && !state.dirty && now - state.updated_at < (moved ? 50 : 250))
        return false;
    state.dirty = false;
    state.mutations = mutations;
    state.scroll = scroll;
    state.viewport_width = in.hooks.viewport_width;
    state.viewport_height = in.hooks.viewport_height;
    state.updated_at = now;

    js::Interpreter& interpreter = in.interpreter;
    bool queued = false;
    std::vector<js::Object*> const observers = in.intersection.observers;
    for (js::Object* const object : observers) {
        auto* const observer = static_cast<IntersectionObserverObject*>(object);
        js::Interpreter::Roots const roots(interpreter);
        interpreter.root(js::Value::object(observer));
        for (std::size_t i = 0; i < observer->observations.size(); ++i) {
            Observation& observation = observer->observations[i];
            Measured const measured = measure(in, *observer, *observation.target);
            int const index = threshold_index(observer->thresholds, measured);
            if (index == observation.previous_threshold_index && measured.is_intersecting == observation.previous_is_intersecting)
                continue;
            observation.previous_threshold_index = index;
            observation.previous_is_intersecting = measured.is_intersecting;
            // The entry is the observer's the moment it is made; the rects
            // are rooted until then.
            js::Interpreter::Roots const inner(interpreter);
            js::Value const target_rect = interpreter.root(rect_value(in, measured.target));
            js::Value const intersection_rect = interpreter.root(rect_value(in, measured.is_intersecting ? measured.intersection : Rect {}));
            js::Value const root_bounds = interpreter.root(measured.root ? rect_value(in, *measured.root) : js::Value::null());
            auto* const entry = interpreter.heap().allocate<IntersectionEntryObject>(in.prototype("IntersectionObserverEntry"));
            entry->time = now - in.time_origin;
            entry->root_bounds = root_bounds;
            entry->bounding_client_rect = target_rect;
            entry->intersection_rect = intersection_rect;
            entry->is_intersecting = measured.is_intersecting;
            entry->ratio = measured.is_intersecting ? measured.ratio : 0;
            entry->target = observation.wrapper;
            observer->entries.push_back(js::Value::object(entry));
            queued = true;
        }
    }
    if (queued)
        arrange_delivery(in);
    return queued;
}

void trace_intersection_observers(Realm::Internals const& in, js::Tracer& tracer)
{
    for (js::Object* const observer : in.intersection.observers)
        tracer.visit(observer);
}

void install_intersection_observer(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* proto = define_interface(in, "IntersectionObserver", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Value const callback = js::argument(args, 0);
            if (!js::Interpreter::is_callable(callback))
                return interp.throw_type_error("Failed to construct 'IntersectionObserver': parameter 1 is not of type 'Function'.");
            js::Interpreter::Roots const roots(interp);
            interp.root(callback);
            auto* const observer = interp.heap().allocate<IntersectionObserverObject>(internals.prototype("IntersectionObserver"), callback);
            interp.root(js::Value::object(observer));
            js::Value const options = js::argument(args, 1);
            if (options.is_object()) {
                std::optional<js::Value> const root = interp.get(*options.as_object(), interp.key("root"));
                if (!root)
                    return std::nullopt;
                if (!root->is_undefined() && !root->is_null()) {
                    dom::Node* const node = internals.realm.node_of(*root);
                    if (node == nullptr || !(node->is_element() || node->type() == dom::NodeType::Document))
                        return interp.throw_type_error("Failed to construct 'IntersectionObserver': The provided value is not of type '(Document or Element)'.");
                    observer->root = *root;
                }
                std::optional<js::Value> const margin = interp.get(*options.as_object(), interp.key("rootMargin"));
                if (!margin)
                    return std::nullopt;
                if (!margin->is_undefined()) {
                    std::optional<std::string> const text = internals.to_utf8(*margin);
                    if (!text)
                        return std::nullopt;
                    std::optional<std::array<MarginSide, 4>> const parsed = parse_root_margin(*text);
                    if (!parsed)
                        return internals.throw_dom_exception("SyntaxError",
                            "Failed to construct 'IntersectionObserver': rootMargin must be specified in pixels or percent.");
                    observer->margin = *parsed;
                }
                std::optional<js::Value> const threshold = interp.get(*options.as_object(), interp.key("threshold"));
                if (!threshold)
                    return std::nullopt;
                if (!threshold->is_undefined()) {
                    std::vector<double> values;
                    if (threshold->is_object()) {
                        std::optional<js::Value> const length = interp.get(*threshold->as_object(), interp.key("length"));
                        if (!length)
                            return std::nullopt;
                        std::optional<double> const count = interp.to_number(*length);
                        if (!count)
                            return std::nullopt;
                        for (std::size_t i = 0; i < static_cast<std::size_t>(std::max(0.0, *count)); ++i) {
                            std::optional<js::Value> const item = interp.get(*threshold->as_object(), js::PropertyKey::index(i));
                            if (!item)
                                return std::nullopt;
                            std::optional<double> const number = interp.to_number(*item);
                            if (!number)
                                return std::nullopt;
                            values.push_back(*number);
                        }
                    } else {
                        std::optional<double> const number = interp.to_number(*threshold);
                        if (!number)
                            return std::nullopt;
                        values.push_back(*number);
                    }
                    for (double const value : values) {
                        if (std::isnan(value) || value < 0 || value > 1)
                            return internals.throw_dom_exception("RangeError",
                                "Failed to construct 'IntersectionObserver': Threshold values must be numbers between 0 and 1");
                    }
                    std::sort(values.begin(), values.end());
                    if (values.empty())
                        values.push_back(0);
                    observer->thresholds = std::move(values);
                }
            }
            return js::Value::object(observer);
        },
        1);

    define_operation(interpreter, *proto, "observe", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<IntersectionObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Value const target = js::argument(args, 0);
        dom::Node* const node = internals.realm.node_of(target);
        if (node == nullptr || !node->is_element())
            return interp.throw_type_error("Failed to execute 'observe' on 'IntersectionObserver': parameter 1 is not of type 'Element'.");
        auto& observations = (*observer)->observations;
        if (std::any_of(observations.begin(), observations.end(), [node](Observation const& o) { return o.target == node; }))
            return js::Value::undefined();
        observations.push_back(Observation { static_cast<dom::Element*>(node), target });
        auto& observers = internals.intersection.observers;
        if (std::find(observers.begin(), observers.end(), *observer) == observers.end())
            observers.push_back(*observer);
        // The next update measures it, whatever else has changed.
        internals.intersection.dirty = true;
        return js::Value::undefined();
    });
    define_operation(interpreter, *proto, "unobserve", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<IntersectionObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        dom::Node* const node = internals.realm.node_of(js::argument(args, 0));
        if (node == nullptr || !node->is_element())
            return interp.throw_type_error("Failed to execute 'unobserve' on 'IntersectionObserver': parameter 1 is not of type 'Element'.");
        std::erase_if((*observer)->observations, [node](Observation const& o) { return o.target == node; });
        forget_idle_observers(internals);
        return js::Value::undefined();
    });
    define_operation(interpreter, *proto, "disconnect", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IntersectionObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        (*observer)->observations.clear();
        (*observer)->entries.clear();
        forget_idle_observers(internals_of(interp));
        return js::Value::undefined();
    });
    define_operation(interpreter, *proto, "takeRecords", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IntersectionObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        js::ArrayObject* const list = interp.new_array();
        interp.root(js::Value::object(list));
        for (js::Value const& entry : (*observer)->entries)
            list->push(entry);
        (*observer)->entries.clear();
        return js::Value::object(list);
    });
    define_getter(in, *proto, "root", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IntersectionObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        return (*observer)->root;
    });
    define_getter(in, *proto, "rootMargin", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IntersectionObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        return internals_of(interp).string(serialize_root_margin((*observer)->margin));
    });
    define_getter(in, *proto, "scrollMargin", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_observer(interp, this_value))
            return std::nullopt;
        return internals_of(interp).string("0px 0px 0px 0px");
    });
    define_getter(in, *proto, "thresholds", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<IntersectionObserverObject*> const observer = this_observer(interp, this_value);
        if (!observer)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        js::ArrayObject* const list = interp.new_array();
        interp.root(js::Value::object(list));
        for (double const value : (*observer)->thresholds)
            list->push(js::Value::number(value));
        return js::Value::object(list);
    });
    define_getter(in, *proto, "delay", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_observer(interp, this_value))
            return std::nullopt;
        return js::Value::number(0);
    });
    define_getter(in, *proto, "trackVisibility", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_observer(interp, this_value))
            return std::nullopt;
        return js::Value::boolean(false);
    });

    // The entries, whose members live on the prototype where a page that
    // checks for them looks ('intersectionRatio' in the prototype decides
    // whether a page brings its own observer).
    js::Object* entry = define_interface(in, "IntersectionObserverEntry", nullptr);
    auto const member = [&in, entry](std::string_view name, js::Value (*read)(Realm::Internals&, IntersectionEntryObject&)) {
        define_getter(in, *entry, name, [read](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<IntersectionEntryObject*> const found = this_entry(interp, this_value);
            if (!found)
                return std::nullopt;
            return read(internals_of(interp), **found);
        });
    };
    member("time", [](Realm::Internals&, IntersectionEntryObject& e) { return js::Value::number(e.time); });
    member("rootBounds", [](Realm::Internals&, IntersectionEntryObject& e) { return e.root_bounds; });
    member("boundingClientRect", [](Realm::Internals&, IntersectionEntryObject& e) { return e.bounding_client_rect; });
    member("intersectionRect", [](Realm::Internals&, IntersectionEntryObject& e) { return e.intersection_rect; });
    member("isIntersecting", [](Realm::Internals&, IntersectionEntryObject& e) { return js::Value::boolean(e.is_intersecting); });
    member("isVisible", [](Realm::Internals&, IntersectionEntryObject&) { return js::Value::boolean(false); });
    member("intersectionRatio", [](Realm::Internals&, IntersectionEntryObject& e) { return js::Value::number(e.ratio); });
    member("target", [](Realm::Internals&, IntersectionEntryObject& e) { return e.target; });
}

}
