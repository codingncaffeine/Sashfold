#include "bindings/Internal.h"

// The Notifications API (https://notifications.spec.whatwg.org/). The
// permission is the reader's, kept by the host for each site: a page's own
// document in a secure context reads it through
// HostHooks::notification_permission, asks for it through
// ask_notification_permission — only while the reader has just acted on
// the page, as Firefox asks, so that a page cannot greet a visitor with
// the question — and, once allowed, has its notifications shown by the
// host (show_notification), which says what became of each: `show`,
// `click`, `close`, or `error`. A notification made without permission is
// told `error` in a task (§2.9) and is never shown.
//
// A frame's document, a document that is not a secure context and a host
// with no notifier are all answered "denied", with no question asked.
// Not here: service workers' persistent notifications, actions, badges,
// images, vibration and the icon (the desktop shows the browser's).

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
    std::uint64_t shown_as = 0; // the host's name for it while it shows

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
            if (notification_permission(internals) == "granted" && internals.hooks.show_notification) {
                NotificationRequest request;
                request.title = made->title;
                request.body = made->body;
                request.tag = made->tag;
                request.require_interaction = made->require_interaction;
                request.silent = made->silent.value_or(false);
                made->shown_as = internals.hooks.show_notification(request);
                if (made->shown_as != 0) {
                    // Kept for as long as it shows: its events have
                    // somewhere to go though the page holds it nowhere.
                    internals.shown_notifications[made->shown_as] = made;
                    return js::Value::object(made);
                }
            }
            // Permission is not granted, or the host could not show it: the
            // notification hears `error`, in a task, and nothing is shown.
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
    // Taken down, if it shows: the host says `close` when it is gone.
    define_operation(interpreter, *proto, "close", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<NotificationObject*> const found = this_notification(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if ((*found)->shown_as != 0 && internals.hooks.close_notification)
            internals.hooks.close_notification((*found)->shown_as);
        return js::Value::undefined();
    });
    static constexpr std::string_view events[] = { "click", "show", "error", "close" };
    define_event_handlers(in, *proto, events);

    js::Object& constructor = *interpreter.get(*interpreter.global(), interpreter.key("Notification"))->as_object();
    define_getter(in, constructor, "permission", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return internals.string(notification_permission(internals));
    });
    // requestPermission(deprecatedCallback). An answer the reader has
    // already given is the answer: the old callback hears it in a task,
    // the promise has it. With none yet, the host asks the reader — if
    // they have just acted on the page; a page that asks unbidden is
    // answered "default" and may ask again at a click — and the promise
    // waits for Realm::notification_permission_answered.
    define_promise_operation(interpreter, constructor, "requestPermission", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Value const callback = js::argument(args, 0);
        if (!callback.is_nullish() && !js::Interpreter::is_callable(callback)) {
            // WebIDL §3.7.7: what the steps of a promise-returning operation
            // throw is a rejection.
            interp.throw_type_error("Failed to execute 'requestPermission' on 'Notification': parameter 1 is not of type 'Function'.");
            return rejected_promise(interp, interp.take_exception());
        }
        std::string state = notification_permission(internals);
        bool const acted = !internals.hooks.user_activation || internals.hooks.user_activation();
        if (state == "default" && internals.hooks.ask_notification_permission && acted) {
            js::Interpreter::Roots const roots(interp);
            interp.root(callback);
            std::optional<js::PromiseCapability> const capability
                = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
            if (!capability)
                return std::nullopt;
            bool const asking = !internals.notification_asks.empty();
            internals.notification_asks.push_back({ capability->resolve, js::Interpreter::is_callable(callback) ? callback : js::Value::undefined() });
            if (!asking)
                internals.hooks.ask_notification_permission();
            return capability->promise;
        }
        if (state == "default" && internals.hooks.ask_notification_permission && internals.hooks.console)
            internals.hooks.console("warn", "Notification.requestPermission() was not asked of the reader: it may only be called while handling a click or a key.");
        if (js::Interpreter::is_callable(callback)) {
            auto held = std::make_shared<js::Persistent>(interp.heap(), callback);
            internals.post_task([&internals, held, state] {
                Realm::Internals::Entry const entry(internals);
                js::Interpreter& inner = internals.interpreter;
                js::Interpreter::Roots const task_roots(inner);
                js::Value const arguments[] = { inner.root(internals.string(state)) };
                internals.call_reporting(held->value(), js::Value::undefined(), arguments, "notification permission");
            });
        }
        js::Interpreter::Roots const roots(interp);
        js::Value const answer = interp.root(internals.string(state));
        return resolved_promise(interp, answer);
    });
}

std::string notification_permission(Realm::Internals& in)
{
    net::Url const& url = in.url;
    // As isSecureContext says it (Window.cpp).
    bool const secure = url.scheme == "https" || url.scheme == "file" || url.scheme == "about" || url.host == "localhost";
    if (in.parent_realm != nullptr || !secure || !in.hooks.notification_permission)
        return "denied";
    std::string state = in.hooks.notification_permission();
    return state == "granted" || state == "default" ? state : std::string("denied");
}

void trace_notifications(Realm::Internals& in, js::Tracer& tracer)
{
    for (Realm::Internals::NotificationAsk const& ask : in.notification_asks) {
        tracer.visit(ask.resolve);
        tracer.visit(ask.callback);
    }
    for (auto const& [id, notification] : in.shown_notifications)
        tracer.visit(notification);
}

void Realm::notification_permission_answered(std::string_view state)
{
    Internals& in = *m_internals;
    if (in.notification_asks.empty())
        return;
    Internals::Entry const entry(in);
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    std::vector<Internals::NotificationAsk> const asks = std::exchange(in.notification_asks, {});
    for (Internals::NotificationAsk const& ask : asks) {
        interp.root(ask.resolve);
        interp.root(ask.callback);
    }
    js::Value const answer = interp.root(in.string(std::string(state)));
    for (Internals::NotificationAsk const& ask : asks) {
        js::Value const arguments[] = { answer };
        if (js::Interpreter::is_callable(ask.callback))
            in.call_reporting(ask.callback, js::Value::undefined(), arguments, "notification permission");
        static_cast<void>(interp.call(ask.resolve, js::Value::undefined(), arguments));
    }
}

void Realm::notification_event(std::uint64_t id, std::string_view type)
{
    Internals& in = *m_internals;
    auto const found = in.shown_notifications.find(id);
    if (found == in.shown_notifications.end())
        return;
    Internals::Entry const entry(in);
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    js::Object* const notification = found->second;
    interp.root(js::Value::object(notification));
    // Gone from the desktop, it is no longer kept on its account.
    if (type == "close" || type == "error") {
        if (auto* const object = dynamic_cast<NotificationObject*>(notification))
            object->shown_as = 0;
        in.shown_notifications.erase(found);
    }
    EventObject* const event = in.new_event("Event", type, false, type == "click");
    interp.root(js::Value::object(event));
    event->is_trusted = true;
    in.dispatch(*event, notification);
}

}
