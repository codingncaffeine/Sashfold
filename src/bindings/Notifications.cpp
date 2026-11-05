#include "bindings/Internal.h"

// The Notifications API (https://notifications.spec.whatwg.org/), as a
// browser whose reader has said no answers it: the interface is here, the
// permission is "denied", asking for it answers "denied" without a prompt,
// and a notification a page makes all the same is told `error` in a task
// (§2.9 "If permission is not granted, queue a task to fire an event named
// error on this") and is never shown. navigator.permissions says the same
// of "notifications". What it buys: a page that reads Notification.permission
// or calls requestPermission() unguarded no longer stops on a missing name.
// Showing notifications through the desktop's own service, with a prompt,
// is the step after this one.

#include <memory>
#include <optional>
#include <string>

namespace sashfold::bindings {

namespace {

class NotificationObject final : public EventTargetObject {
public:
    explicit NotificationObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    std::string title;
    std::string dir = "auto";
    std::string lang;
    std::string body;
    std::string tag;
    std::string icon; // resolved against the document's base; "" when it does not parse
    bool require_interaction = false;
    std::optional<bool> silent;
    js::Value data = js::Value::null();

    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(data);
    }
};

std::optional<NotificationObject*> this_notification(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<NotificationObject*>(this_value.as_object()))
            return found;
    }
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

// A member of the options dictionary as a string, the default when it is
// not given; nothing when reading or converting it threw.
std::optional<std::string> string_member(js::Interpreter& interp, js::Value const& options, std::string_view name, std::string fallback)
{
    if (!options.is_object())
        return fallback;
    std::optional<js::Value> const value = interp.get(options, name);
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return fallback;
    return internals_of(interp).to_utf8(*value);
}

}

void install_notifications(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* const proto = define_interface(in, "Notification", in.prototype("EventTarget"),
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.empty())
                return interp.throw_type_error("Failed to construct 'Notification': 1 argument required, but only 0 present.");
            js::Interpreter::Roots const roots(interp);
            std::optional<std::string> title = internals.to_utf8(args[0]);
            if (!title)
                return std::nullopt;
            js::Value const options = js::argument(args, 1);
            interp.root(options);
            if (!options.is_nullish() && !options.is_object())
                return interp.throw_type_error("Failed to construct 'Notification': parameter 2 is not of type 'NotificationOptions'.");
            // The members in the dictionary's order (WebIDL reads them
            // alphabetically): body, data, dir, icon, lang,
            // requireInteraction, silent, tag.
            std::optional<std::string> body = string_member(interp, options, "body", "");
            if (!body)
                return std::nullopt;
            js::Value data = js::Value::null();
            if (options.is_object()) {
                std::optional<js::Value> const given = interp.get(options, "data");
                if (!given)
                    return std::nullopt;
                if (!given->is_undefined())
                    data = *given;
            }
            interp.root(data);
            std::optional<std::string> dir = string_member(interp, options, "dir", "auto");
            if (!dir)
                return std::nullopt;
            if (*dir != "auto" && *dir != "ltr" && *dir != "rtl")
                return interp.throw_type_error("Failed to construct 'Notification': Failed to read the 'dir' property from 'NotificationOptions': The provided value '"
                    + *dir + "' is not a valid enum value of type NotificationDirection.");
            std::optional<std::string> icon = string_member(interp, options, "icon", "");
            if (!icon)
                return std::nullopt;
            std::optional<std::string> lang = string_member(interp, options, "lang", "");
            if (!lang)
                return std::nullopt;
            bool require_interaction = false;
            std::optional<bool> silent;
            if (options.is_object()) {
                std::optional<js::Value> const sticky = interp.get(options, "requireInteraction");
                if (!sticky)
                    return std::nullopt;
                require_interaction = interp.to_boolean(*sticky);
                std::optional<js::Value> const quiet = interp.get(options, "silent");
                if (!quiet)
                    return std::nullopt;
                if (!quiet->is_nullish())
                    silent = interp.to_boolean(*quiet);
            }
            std::optional<std::string> tag = string_member(interp, options, "tag", "");
            if (!tag)
                return std::nullopt;

            auto* const made = interp.heap().allocate<NotificationObject>(internals.prototype("Notification"));
            made->title = std::move(*title);
            made->body = std::move(*body);
            made->data = data;
            made->dir = std::move(*dir);
            if (!icon->empty()) {
                if (std::optional<net::Url> const resolved = net::parse_url(*icon, &internals.base_url()))
                    made->icon = resolved->serialize();
            }
            made->lang = std::move(*lang);
            made->require_interaction = require_interaction;
            made->silent = silent;
            made->tag = std::move(*tag);
            // Permission is not granted: the notification hears `error`, in
            // a task, and nothing is shown.
            auto held = std::make_shared<js::Persistent>(interp.heap(), js::Value::object(made));
            internals.post_task([&internals, held] {
                Realm::Internals::Entry const entry(internals);
                js::Interpreter& inner = internals.interpreter;
                js::Interpreter::Roots const task_roots(inner);
                EventObject* const event = internals.new_event("Event", "error", false, false);
                inner.root(js::Value::object(event));
                event->is_trusted = true;
                internals.dispatch(*event, held->value().as_object());
            });
            return js::Value::object(made);
        },
        1);

    struct Text {
        std::string_view name;
        std::string NotificationObject::* member;
    };
    for (Text const text : { Text { "title", &NotificationObject::title }, Text { "dir", &NotificationObject::dir },
             Text { "lang", &NotificationObject::lang }, Text { "body", &NotificationObject::body },
             Text { "tag", &NotificationObject::tag }, Text { "icon", &NotificationObject::icon } }) {
        define_getter(in, *proto, text.name, [member = text.member](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<NotificationObject*> const found = this_notification(interp, this_value);
            if (!found)
                return std::nullopt;
            return internals_of(interp).string((*found)->*member);
        });
    }
    define_getter(in, *proto, "requireInteraction", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<NotificationObject*> const found = this_notification(interp, this_value);
        if (!found)
            return std::nullopt;
        return js::Value::boolean((*found)->require_interaction);
    });
    define_getter(in, *proto, "silent", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<NotificationObject*> const found = this_notification(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->silent ? js::Value::boolean(*(*found)->silent) : js::Value::null();
    });
    define_getter(in, *proto, "data", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<NotificationObject*> const found = this_notification(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->data;
    });
    // Nothing was shown, so there is nothing to take down.
    define_operation(interpreter, *proto, "close", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_notification(interp, this_value))
            return std::nullopt;
        return js::Value::undefined();
    });
    static constexpr std::string_view events[] = { "click", "show", "error", "close" };
    define_event_handlers(in, *proto, events);

    js::Object& constructor = *interpreter.get(*interpreter.global(), interpreter.key("Notification"))->as_object();
    define_getter(in, constructor, "permission", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).string("denied");
    });
    // requestPermission(deprecatedCallback): the answer is known without
    // asking. The old callback hears it in a task; the promise has it.
    define_promise_operation(interpreter, constructor, "requestPermission", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Value const callback = js::argument(args, 0);
        if (!callback.is_nullish() && !js::Interpreter::is_callable(callback)) {
            // WebIDL §3.7.7: what the steps of a promise-returning operation
            // throw is a rejection.
            interp.throw_type_error("Failed to execute 'requestPermission' on 'Notification': parameter 1 is not of type 'Function'.");
            return rejected_promise(interp, interp.take_exception());
        }
        if (js::Interpreter::is_callable(callback)) {
            auto held = std::make_shared<js::Persistent>(interp.heap(), callback);
            internals.post_task([&internals, held] {
                Realm::Internals::Entry const entry(internals);
                js::Interpreter& inner = internals.interpreter;
                js::Interpreter::Roots const task_roots(inner);
                js::Value const arguments[] = { inner.root(internals.string("denied")) };
                internals.call_reporting(held->value(), js::Value::undefined(), arguments, "notification permission");
            });
        }
        js::Interpreter::Roots const roots(interp);
        js::Value const answer = interp.root(internals.string("denied"));
        return resolved_promise(interp, answer);
    });
}

}
