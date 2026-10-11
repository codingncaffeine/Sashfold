#include "bindings/Internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The performance timeline (Performance Timeline, User Timing, Navigation
// Timing): the entries a page reads back from `performance` — the
// document's own load as one PerformanceNavigationTiming, and the marks and
// measures the page made — and the PerformanceObservers told of them.
//
// Pages read the load's timing to report it, and a page that finds no
// navigation entry where every browser has one stops with a TypeError
// (getEntriesByType('navigation')[0].domContentLoadedEventStart). The
// framework builds mark and measure their work and read the duration back.
//
// What the network did is not measured here: the entry's network
// milestones stand at the start of the fetch, and its document milestones
// are when the realm reached them.

namespace sashfold::bindings {

namespace {

class PerformanceEntryObject final : public js::Object {
public:
    enum class Kind : std::uint8_t { Navigation, Mark, Measure };
    PerformanceEntryObject(js::Object* prototype, Kind the_kind)
        : Object(prototype, Class::Host)
        , kind(the_kind)
    {
    }
    Kind kind;
    std::string name;
    double start_time = 0;
    double duration = 0;
    js::Value detail = js::Value::null();
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(detail);
    }
};

std::string_view entry_type(PerformanceEntryObject::Kind kind)
{
    switch (kind) {
    case PerformanceEntryObject::Kind::Navigation: return "navigation";
    case PerformanceEntryObject::Kind::Mark: return "mark";
    case PerformanceEntryObject::Kind::Measure: return "measure";
    }
    return "";
}

class PerformanceObserverObject final : public js::Object {
public:
    PerformanceObserverObject(js::Object* prototype, js::Value the_callback)
        : Object(prototype, Class::Host)
        , callback(the_callback)
    {
    }
    js::Value callback;
    std::vector<std::string> types; // what it observes
    std::vector<js::Object*> buffer; // entries not yet delivered
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(callback);
        for (js::Object* const entry : buffer)
            tracer.visit(entry);
    }
};

// A PerformanceObserverEntryList: the entries one delivery hands over.
class EntryListObject final : public js::Object {
public:
    explicit EntryListObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::vector<js::Object*> entries;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        for (js::Object* const entry : entries)
            tracer.visit(entry);
    }
};

PerformanceEntryObject* entry_this(js::Interpreter& interp, js::Value const& this_value)
{
    auto* const entry = this_value.is_object() ? dynamic_cast<PerformanceEntryObject*>(this_value.as_object()) : nullptr;
    if (entry == nullptr)
        interp.throw_type_error("Illegal invocation");
    return entry;
}

// The document's navigation entry, made the first time it is asked for;
// its document milestones are read from the realm when they are read.
PerformanceEntryObject& navigation_entry(Realm::Internals& in)
{
    for (js::Object* const object : in.performance_entries) {
        auto& entry = static_cast<PerformanceEntryObject&>(*object);
        if (entry.kind == PerformanceEntryObject::Kind::Navigation)
            return entry;
    }
    auto* const made = in.interpreter.heap().allocate<PerformanceEntryObject>(
        in.prototype("PerformanceNavigationTiming"), PerformanceEntryObject::Kind::Navigation);
    made->name = in.url.serialize();
    in.performance_entries.insert(in.performance_entries.begin(), made);
    return *made;
}

double navigation_duration(Realm::Internals const& in)
{
    return in.navigation_timing.load_end;
}

// The entries of a kind, a name or both, by their start time.
std::vector<js::Object*> entries_matching(Realm::Internals& in, std::vector<js::Object*> const& from, std::optional<std::string> const& name,
    std::optional<std::string> const& type)
{
    std::vector<js::Object*> out;
    for (js::Object* const object : from) {
        auto const& entry = static_cast<PerformanceEntryObject const&>(*object);
        if (type && entry_type(entry.kind) != *type)
            continue;
        if (name && entry.name != *name)
            continue;
        out.push_back(object);
    }
    std::stable_sort(out.begin(), out.end(), [](js::Object const* a, js::Object const* b) {
        return static_cast<PerformanceEntryObject const*>(a)->start_time < static_cast<PerformanceEntryObject const*>(b)->start_time;
    });
    static_cast<void>(in);
    return out;
}

js::Value array_of(js::Interpreter& interp, std::vector<js::Object*> const& objects)
{
    js::Interpreter::Roots const roots(interp);
    js::ArrayObject* const list = interp.new_array();
    interp.root(js::Value::object(list));
    for (js::Object* const object : objects)
        list->push(js::Value::object(object));
    return js::Value::object(list);
}

// Hands the queued entries to their observers in a task (Performance
// Timeline §5.2 "queue a PerformanceObserver task").
void arrange_delivery(Realm::Internals& in)
{
    if (in.performance_delivery_pending)
        return;
    in.performance_delivery_pending = true;
    in.post_task([&in] {
        in.performance_delivery_pending = false;
        std::vector<js::Object*> const observers = in.performance_observers;
        for (js::Object* const object : observers) {
            auto* const observer = static_cast<PerformanceObserverObject*>(object);
            if (observer->buffer.empty())
                continue;
            js::Interpreter& interpreter = in.interpreter;
            js::Interpreter::Roots const roots(interpreter);
            interpreter.root(js::Value::object(observer));
            auto* const list = interpreter.heap().allocate<EntryListObject>(in.prototype("PerformanceObserverEntryList"));
            interpreter.root(js::Value::object(list));
            list->entries = std::move(observer->buffer);
            observer->buffer.clear();
            js::Object* const options = interpreter.new_object();
            interpreter.root(js::Value::object(options));
            options->put(interpreter.key("droppedEntriesCount"), js::Value::number(0));
            js::Value const arguments[3] = { js::Value::object(list), js::Value::object(observer), js::Value::object(options) };
            in.call_reporting(observer->callback, js::Value::object(observer), arguments, "PerformanceObserver callback");
            if (interpreter.terminated())
                break;
        }
    });
}

// A new entry reaches the observers watching for its kind.
void queue_entry(Realm::Internals& in, PerformanceEntryObject& entry)
{
    bool queued = false;
    for (js::Object* const object : in.performance_observers) {
        auto* const observer = static_cast<PerformanceObserverObject*>(object);
        if (std::find(observer->types.begin(), observer->types.end(), entry_type(entry.kind)) == observer->types.end())
            continue;
        observer->buffer.push_back(&entry);
        queued = true;
    }
    if (queued)
        arrange_delivery(in);
}

// The milestones Navigation Timing names (PerformanceTiming's attributes),
// which measure() takes as a start or an end by name, in ms from the time
// origin: 0 for one not reached (or never, as no unload or redirect is).
std::optional<double> timing_named(Realm::Internals& in, std::string_view name)
{
    auto const& t = in.navigation_timing;
    if (name == "navigationStart" || name == "fetchStart" || name == "domainLookupStart" || name == "domainLookupEnd" || name == "connectStart"
        || name == "connectEnd" || name == "requestStart" || name == "responseStart" || name == "responseEnd" || name == "domLoading")
        return 0.0;
    if (name == "unloadEventStart" || name == "unloadEventEnd" || name == "redirectStart" || name == "redirectEnd" || name == "secureConnectionStart")
        return 0.0;
    if (name == "domInteractive")
        return t.dom_interactive;
    if (name == "domContentLoadedEventStart")
        return t.dom_content_loaded_start;
    if (name == "domContentLoadedEventEnd")
        return t.dom_content_loaded_end;
    if (name == "domComplete")
        return t.dom_complete;
    if (name == "loadEventStart")
        return t.load_start;
    if (name == "loadEventEnd")
        return t.load_end;
    return std::nullopt;
}

// A mark's time by its name — the latest of that name — or a milestone's;
// nothing (a SyntaxError pending) for neither.
std::optional<double> time_of_mark(Realm::Internals& in, std::string const& name)
{
    for (auto it = in.performance_entries.rbegin(); it != in.performance_entries.rend(); ++it) {
        auto const& entry = static_cast<PerformanceEntryObject const&>(**it);
        if (entry.kind == PerformanceEntryObject::Kind::Mark && entry.name == name)
            return entry.start_time;
    }
    if (std::optional<double> const milestone = timing_named(in, name)) {
        // User Timing §4.2 "convert a name to a timestamp": a milestone not
        // reached is refused; navigationStart is the origin itself.
        bool const origin = name == "navigationStart" || name == "fetchStart" || name == "domainLookupStart" || name == "domainLookupEnd"
            || name == "connectStart" || name == "connectEnd" || name == "requestStart" || name == "responseStart" || name == "responseEnd"
            || name == "domLoading";
        if (*milestone == 0 && !origin) {
            in.throw_dom_exception("InvalidAccessError", "Failed to execute 'measure' on 'Performance': '" + name + "' is empty: either the event hasn't happened yet, or it would provide cross-origin timing information.");
            return std::nullopt;
        }
        return milestone;
    }
    in.throw_dom_exception("SyntaxError", "Failed to execute 'measure' on 'Performance': The mark '" + name + "' does not exist.");
    return std::nullopt;
}

// A measure's start or end as given: a mark's name or a time.
std::optional<double> time_of(Realm::Internals& in, js::Value const& value)
{
    if (value.is_string()) {
        std::optional<std::string> const name = in.to_utf8(value);
        if (!name)
            return std::nullopt;
        return time_of_mark(in, *name);
    }
    std::optional<double> const number = in.interpreter.to_number(value);
    if (!number)
        return std::nullopt;
    if (*number < 0 || std::isnan(*number)) {
        in.interpreter.throw_type_error("Failed to execute 'measure' on 'Performance': A time must not be negative.");
        return std::nullopt;
    }
    return *number;
}

// A PerformanceMark from mark()'s arguments (User Timing §3.1.1, the
// constructor's steps), or nothing with an exception pending.
PerformanceEntryObject* make_mark(js::Interpreter& interp, Args args)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<std::string> name = in.to_utf8(js::argument(args, 0));
    if (!name)
        return nullptr;
    if (timing_named(in, *name)) {
        in.throw_dom_exception("SyntaxError", "Failed to execute 'mark' on 'Performance': '" + *name + "' is part of the PerformanceTiming interface, and cannot be used as a mark name.");
        return nullptr;
    }
    double start = in.now() - in.time_origin;
    js::Value detail = js::Value::null();
    js::Value const options = js::argument(args, 1);
    if (options.is_object()) {
        std::optional<js::Value> const given = interp.get(options, "startTime");
        std::optional<js::Value> const given_detail = given ? interp.get(options, "detail") : std::nullopt;
        if (!given_detail)
            return nullptr;
        if (!given->is_undefined()) {
            std::optional<double> const number = interp.to_number(*given);
            if (!number)
                return nullptr;
            if (*number < 0 || std::isnan(*number)) {
                interp.throw_type_error("Failed to execute 'mark' on 'Performance': '" + *name + "' cannot have a negative start time.");
                return nullptr;
            }
            start = *number;
        }
        if (!given_detail->is_undefined())
            detail = *given_detail;
    }
    js::Interpreter::Roots const roots(interp);
    interp.root(detail);
    auto* const entry = interp.heap().allocate<PerformanceEntryObject>(in.prototype("PerformanceMark"), PerformanceEntryObject::Kind::Mark);
    entry->name = std::move(*name);
    entry->start_time = start;
    entry->detail = detail;
    return entry;
}

Native mark(js::Interpreter& interp, js::Value const&, Args args)
{
    PerformanceEntryObject* const entry = make_mark(interp, args);
    if (entry == nullptr)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    in.performance_entries.push_back(entry);
    queue_entry(in, *entry);
    return js::Value::object(entry);
}

Native measure(js::Interpreter& interp, js::Value const&, Args args)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<std::string> name = in.to_utf8(js::argument(args, 0));
    if (!name)
        return std::nullopt;
    double const now = in.now() - in.time_origin;
    std::optional<double> start;
    std::optional<double> end;
    std::optional<double> duration;
    js::Value detail = js::Value::null();
    js::Value const second = js::argument(args, 1);
    if (second.is_object() && !second.is_string()) {
        // MeasureOptions: start, end, duration (two at most), detail.
        for (auto const& [key, out] : { std::pair { "start", &start }, std::pair { "end", &end } }) {
            std::optional<js::Value> const given = interp.get(second, key);
            if (!given)
                return std::nullopt;
            if (!given->is_undefined()) {
                std::optional<double> const time = time_of(in, *given);
                if (!time)
                    return std::nullopt;
                *out = *time;
            }
        }
        std::optional<js::Value> const given_duration = interp.get(second, "duration");
        std::optional<js::Value> const given_detail = given_duration ? interp.get(second, "detail") : std::nullopt;
        if (!given_detail)
            return std::nullopt;
        if (!given_duration->is_undefined()) {
            std::optional<double> const number = interp.to_number(*given_duration);
            if (!number)
                return std::nullopt;
            duration = *number;
        }
        if (start && end && duration)
            return interp.throw_type_error("Failed to execute 'measure' on 'Performance': If a non-empty PerformanceMeasureOptions object was passed, it must not have all of start, duration and end.");
        if (!given_detail->is_undefined())
            detail = *given_detail;
        if (js::argument(args, 2).is_undefined() == false)
            return interp.throw_type_error("Failed to execute 'measure' on 'Performance': If a non-empty PerformanceMeasureOptions object was passed, |end_mark| must not be passed.");
    } else {
        if (!second.is_undefined()) {
            std::optional<double> const time = time_of(in, second);
            if (!time)
                return std::nullopt;
            start = *time;
        }
        if (!js::argument(args, 2).is_undefined()) {
            std::optional<double> const time = time_of(in, js::argument(args, 2));
            if (!time)
                return std::nullopt;
            end = *time;
        }
    }
    double const from = start ? *start : (end && duration ? *end - *duration : 0.0);
    double const to = end ? *end : (duration ? from + *duration : now);
    js::Interpreter::Roots const roots(interp);
    interp.root(detail);
    auto* const entry = interp.heap().allocate<PerformanceEntryObject>(in.prototype("PerformanceMeasure"), PerformanceEntryObject::Kind::Measure);
    entry->name = std::move(*name);
    entry->start_time = from;
    entry->duration = to - from;
    entry->detail = detail;
    in.performance_entries.push_back(entry);
    queue_entry(in, *entry);
    return js::Value::object(entry);
}

// clearMarks and clearMeasures: every one of the kind, or those of a name.
Native clear(js::Interpreter& interp, Args args, PerformanceEntryObject::Kind kind)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<std::string> name;
    if (!js::argument(args, 0).is_undefined()) {
        name = in.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
    }
    std::erase_if(in.performance_entries, [&](js::Object* object) {
        auto const& entry = static_cast<PerformanceEntryObject const&>(*object);
        return entry.kind == kind && (!name || entry.name == *name);
    });
    return js::Value::undefined();
}

// getEntries, getEntriesByType and getEntriesByName, for `performance`
// (which holds the navigation entry) and for an observer's entry list.
std::vector<js::Object*> listed_entries(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* const list = dynamic_cast<EntryListObject*>(this_value.as_object()))
            return list->entries;
    }
    Realm::Internals& in = internals_of(interp);
    navigation_entry(in);
    return in.performance_entries;
}

Native get_entries(js::Interpreter& interp, js::Value const& this_value, Args)
{
    return array_of(interp, entries_matching(internals_of(interp), listed_entries(interp, this_value), std::nullopt, std::nullopt));
}

Native get_entries_by_type(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    std::optional<std::string> const type = internals_of(interp).to_utf8(js::argument(args, 0));
    if (!type)
        return std::nullopt;
    return array_of(interp, entries_matching(internals_of(interp), listed_entries(interp, this_value), std::nullopt, *type));
}

Native get_entries_by_name(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<std::string> const name = in.to_utf8(js::argument(args, 0));
    if (!name)
        return std::nullopt;
    std::optional<std::string> type;
    if (!js::argument(args, 1).is_undefined()) {
        type = in.to_utf8(js::argument(args, 1));
        if (!type)
            return std::nullopt;
    }
    return array_of(interp, entries_matching(in, listed_entries(interp, this_value), *name, type));
}

// The navigation entry's timing members, by the datum each getter carries:
// the network's at the fetch's start, the document's from the realm.
constexpr std::string_view navigation_numbers[] = { "workerStart", "redirectStart", "redirectEnd", "fetchStart", "domainLookupStart",
    "domainLookupEnd", "connectStart", "connectEnd", "secureConnectionStart", "requestStart", "responseStart", "responseEnd",
    "transferSize", "encodedBodySize", "decodedBodySize", "unloadEventStart", "unloadEventEnd", "domInteractive",
    "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete", "loadEventStart", "loadEventEnd", "redirectCount",
    "activationStart", "firstInterimResponseStart", "responseStatus" };

Native navigation_number(js::Interpreter& interp, js::Value const& this_value, Args)
{
    PerformanceEntryObject* const entry = entry_this(interp, this_value);
    if (entry == nullptr)
        return std::nullopt;
    js::NativeFunction const* const accessor = interp.active_native();
    std::uint32_t const index = accessor != nullptr && accessor->spec() != nullptr ? accessor->spec()->datum : 0;
    std::string_view const name = navigation_numbers[index];
    if (name == "responseStatus")
        return js::Value::number(200);
    std::optional<double> const milestone = timing_named(internals_of(interp), name);
    return js::Value::number(milestone.value_or(0));
}

}

void performance_navigation_done(Realm::Internals& in)
{
    if (in.performance_observers.empty())
        return;
    queue_entry(in, navigation_entry(in));
}

void trace_performance(Realm::Internals const& in, js::Tracer& tracer)
{
    for (js::Object* const entry : in.performance_entries)
        tracer.visit(entry);
    for (js::Object* const observer : in.performance_observers)
        tracer.visit(observer);
}

void install_performance_timeline(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object& performance = *in.prototype("Performance");
    define_operation(interpreter, performance, "mark", 1, mark);
    define_operation(interpreter, performance, "measure", 1, measure);
    define_operation(interpreter, performance, "clearMarks", 0,
        [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return clear(interp, args, PerformanceEntryObject::Kind::Mark); });
    define_operation(interpreter, performance, "clearMeasures", 0,
        [](js::Interpreter& interp, js::Value const&, Args args) -> Native { return clear(interp, args, PerformanceEntryObject::Kind::Measure); });
    define_operation(interpreter, performance, "getEntries", 0, get_entries);
    define_operation(interpreter, performance, "getEntriesByType", 1, get_entries_by_type);
    define_operation(interpreter, performance, "getEntriesByName", 1, get_entries_by_name);

    js::Object* const entry = define_interface(in, "PerformanceEntry", nullptr);
    define_getter(in, *entry, "name", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        PerformanceEntryObject* const self = entry_this(interp, this_value);
        return self == nullptr ? Native(std::nullopt) : Native(internals_of(interp).string(self->name));
    });
    define_getter(in, *entry, "entryType", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        PerformanceEntryObject* const self = entry_this(interp, this_value);
        return self == nullptr ? Native(std::nullopt) : Native(internals_of(interp).string(entry_type(self->kind)));
    });
    define_getter(in, *entry, "startTime", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        PerformanceEntryObject* const self = entry_this(interp, this_value);
        return self == nullptr ? Native(std::nullopt) : Native(js::Value::number(self->start_time));
    });
    define_getter(in, *entry, "duration", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        PerformanceEntryObject* const self = entry_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        return js::Value::number(self->kind == PerformanceEntryObject::Kind::Navigation ? navigation_duration(internals_of(interp)) : self->duration);
    });
    // toJSON: the members a page serializes, read through the getters.
    define_operation(interpreter, *entry, "toJSON", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        PerformanceEntryObject* const self = entry_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        js::Interpreter::Roots const roots(interp);
        js::Object* const out = interp.new_object();
        interp.root(js::Value::object(out));
        auto const copy = [&](std::span<std::string_view const> names) {
            for (std::string_view const name : names) {
                std::optional<js::Value> const value = interp.get(this_value, name);
                if (!value)
                    return false;
                out->put(interp.key(name), *value);
            }
            return true;
        };
        static constexpr std::string_view every_entry[] = { "name", "entryType", "startTime", "duration" };
        static constexpr std::string_view navigation_only[] = { "initiatorType", "type" };
        static constexpr std::string_view user_timing_only[] = { "detail" };
        bool const navigation = self->kind == PerformanceEntryObject::Kind::Navigation;
        if (!copy(every_entry) || !(navigation ? copy(navigation_only) && copy(navigation_numbers) : copy(user_timing_only)))
            return std::nullopt;
        return js::Value::object(out);
    });
    for (std::string_view const name : { "PerformanceMark", "PerformanceMeasure" }) {
        // A PerformanceMark can be made by a script, outside the timeline.
        js::Object* const prototype = name == "PerformanceMark"
            ? define_interface(in, name, entry,
                  [](js::Interpreter& interp, Args args, js::Object*) -> Native {
                      PerformanceEntryObject* const made = make_mark(interp, args);
                      return made == nullptr ? Native(std::nullopt) : Native(js::Value::object(made));
                  },
                  1)
            : define_interface(in, name, entry);
        define_getter(in, *prototype, "detail", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            PerformanceEntryObject* const self = entry_this(interp, this_value);
            return self == nullptr ? Native(std::nullopt) : Native(self->detail);
        });
    }
    js::Object* const resource = define_interface(in, "PerformanceResourceTiming", entry);
    js::Object* const navigation = define_interface(in, "PerformanceNavigationTiming", resource);
    for (std::size_t i = 0; i < std::size(navigation_numbers); ++i)
        define_plain_attribute(interpreter, i < 15 ? *resource : *navigation, navigation_numbers[i], navigation_number, nullptr, MemberKind::Plain,
            static_cast<std::uint32_t>(i));
    define_getter(in, *resource, "initiatorType", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        return entry_this(interp, this_value) == nullptr ? Native(std::nullopt) : Native(internals_of(interp).string("navigation"));
    });
    for (std::string_view const name : { "nextHopProtocol", "renderBlockingStatus", "deliveryType" }) {
        define_getter(in, *resource, name, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            return entry_this(interp, this_value) == nullptr ? Native(std::nullopt) : Native(internals_of(interp).string(""));
        });
    }
    define_getter(in, *resource, "serverTiming", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        return entry_this(interp, this_value) == nullptr ? Native(std::nullopt) : Native(js::Value::object(interp.new_array()));
    });
    define_getter(in, *navigation, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        return entry_this(interp, this_value) == nullptr ? Native(std::nullopt) : Native(internals_of(interp).string("navigate"));
    });

    // PerformanceObserver.
    js::Object* const observer = define_interface(in, "PerformanceObserver", nullptr,
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            js::Value const callback = js::argument(args, 0);
            if (!js::Interpreter::is_callable(callback))
                return interp.throw_type_error("Failed to construct 'PerformanceObserver': parameter 1 is not of type 'Function'.");
            return js::Value::object(interp.heap().allocate<PerformanceObserverObject>(internals_of(interp).prototype("PerformanceObserver"), callback));
        },
        1);
    define_operation(interpreter, *observer, "observe", 0, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        auto* const self = this_value.is_object() ? dynamic_cast<PerformanceObserverObject*>(this_value.as_object()) : nullptr;
        if (self == nullptr)
            return interp.throw_type_error("Illegal invocation");
        Realm::Internals& internals = internals_of(interp);
        js::Value const options = js::argument(args, 0);
        if (!options.is_object())
            return interp.throw_type_error("Failed to execute 'observe' on 'PerformanceObserver': An observe() call must include either entryTypes or type arguments.");
        std::optional<js::Value> const entry_types = interp.get(options, "entryTypes");
        std::optional<js::Value> const type = entry_types ? interp.get(options, "type") : std::nullopt;
        std::optional<js::Value> const buffered = type ? interp.get(options, "buffered") : std::nullopt;
        if (!buffered)
            return std::nullopt;
        std::vector<std::string> types;
        if (!entry_types->is_undefined()) {
            if (!type->is_undefined())
                return interp.throw_type_error("Failed to execute 'observe' on 'PerformanceObserver': An observe() call must not include both entryTypes and type arguments.");
            std::optional<js::Value> const length = entry_types->is_object() ? interp.get(*entry_types, "length") : js::Value::number(0);
            std::optional<double> const count = length ? interp.to_number(*length) : std::nullopt;
            if (!count)
                return std::nullopt;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(std::max(0.0, *count)); ++i) {
                std::optional<js::Value> const item = interp.get(*entry_types, js::PropertyKey::index(i));
                std::optional<std::string> const text = item ? internals.to_utf8(*item) : std::nullopt;
                if (!text)
                    return std::nullopt;
                types.push_back(*text);
            }
        } else if (!type->is_undefined()) {
            std::optional<std::string> const text = internals.to_utf8(*type);
            if (!text)
                return std::nullopt;
            types.push_back(*text);
        } else {
            return interp.throw_type_error("Failed to execute 'observe' on 'PerformanceObserver': An observe() call must include either entryTypes or type arguments.");
        }
        // What this engine records; any other kind is accepted and never comes.
        for (std::string const& kind : types) {
            if (std::find(self->types.begin(), self->types.end(), kind) == self->types.end())
                self->types.push_back(kind);
        }
        auto& observers = internals.performance_observers;
        if (std::find(observers.begin(), observers.end(), self) == observers.end())
            observers.push_back(self);
        // buffered: what is already there of the kind comes with the first delivery.
        if (!type->is_undefined() && js::Interpreter::to_boolean(*buffered)) {
            if (types.front() == "navigation" && internals.navigation_timing.load_end > 0)
                navigation_entry(internals);
            for (js::Object* const object : entries_matching(internals, internals.performance_entries, std::nullopt, types.front()))
                self->buffer.push_back(object);
            if (!self->buffer.empty())
                arrange_delivery(internals);
        }
        return js::Value::undefined();
    });
    define_operation(interpreter, *observer, "disconnect", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const self = this_value.is_object() ? dynamic_cast<PerformanceObserverObject*>(this_value.as_object()) : nullptr;
        if (self == nullptr)
            return interp.throw_type_error("Illegal invocation");
        self->types.clear();
        self->buffer.clear();
        std::erase(internals_of(interp).performance_observers, self);
        return js::Value::undefined();
    });
    define_operation(interpreter, *observer, "takeRecords", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const self = this_value.is_object() ? dynamic_cast<PerformanceObserverObject*>(this_value.as_object()) : nullptr;
        if (self == nullptr)
            return interp.throw_type_error("Illegal invocation");
        js::Value const out = array_of(interp, self->buffer);
        self->buffer.clear();
        return out;
    });
    {
        std::optional<js::Value> const constructor = observer->get(interpreter, interpreter.key("constructor"), js::Value::object(observer));
        // A static attribute: an accessor on the interface object, as WebIDL puts it.
        if (constructor && constructor->is_object())
            define_getter(in, *constructor->as_object(), "supportedEntryTypes", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
                js::Interpreter::Roots const roots(interp);
                js::ArrayObject* const list = interp.new_array();
                interp.root(js::Value::object(list));
                for (std::string_view const kind : { "mark", "measure", "navigation" })
                    list->push(interp.root(internals_of(interp).string(kind)));
                list->prevent_extensions();
                return js::Value::object(list);
            });
    }
    js::Object* const list = define_interface(in, "PerformanceObserverEntryList", nullptr);
    define_operation(interpreter, *list, "getEntries", 0, get_entries);
    define_operation(interpreter, *list, "getEntriesByType", 1, get_entries_by_type);
    define_operation(interpreter, *list, "getEntriesByName", 1, get_entries_by_name);
}

}
