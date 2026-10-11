#include "bindings/Internal.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// BroadcastChannel (HTML §9.6): a page's messages to every other context of
// its origin listening on a channel of the same name — its other tabs, its
// frames, its workers — each delivered in a task of the receiver's loop.
//
// Sites keep their tabs in step with it: a sign-out in one tab signs the
// others out, a cart or a setting changed in one shows in the rest.
//
// The channels open in the process are in one list, behind a lock, as the
// agents that hold them may run on other threads: a message is serialized
// once in the sender's realm and handed to each receiver's agent, which
// deserializes it in the receiving realm.

namespace sashfold::bindings {

namespace {

class BroadcastChannelObject;

// An open channel as every agent sees it. Its object and realm are touched
// only on the thread of the agent that holds it, and only while the realm
// lives and the channel is open.
struct Subscriber {
    std::string key; // the origin, then the name
    std::uint64_t order = 0; // creation order, which delivery follows
    std::shared_ptr<Agent::RemoteTasks> remote;
    std::weak_ptr<bool> alive;
    Realm::Internals* internals = nullptr;
    BroadcastChannelObject* object = nullptr;
    bool closed = false; // set by the owning thread, read under the list's lock
};

struct Subscribers {
    std::mutex mutex;
    std::vector<std::shared_ptr<Subscriber>> open;
    std::uint64_t next_order = 0;
};

Subscribers& subscribers()
{
    // Never destroyed: an agent's thread may still be closing a channel as
    // the process exits.
    static auto* const list = new Subscribers;
    return *list;
}

class BroadcastChannelObject final : public EventTargetObject {
public:
    explicit BroadcastChannelObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    std::string name;
    std::shared_ptr<Subscriber> subscriber;
    bool closed = false;
};

std::optional<BroadcastChannelObject*> this_channel(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* found = dynamic_cast<BroadcastChannelObject*>(this_value.as_object()))
            return found;
    }
    return interp.throw_type_error("Illegal invocation");
}

// The key channels meet by: the origin, and the name. An opaque origin is
// the same only as itself, so its identity stands in for it.
std::string channel_key(Realm::Internals& in, std::string const& name)
{
    Origin const origin = document_origin(in);
    std::string const said = origin.opaque() ? "null:" + std::to_string(origin.opaque_id) : in.origin_url.serialize_origin();
    return said + '\n' + name;
}

// "Eligible for messaging" (§9.6): a window's channel while its document is
// fully active, a worker's while the worker is not closing.
bool eligible(Realm::Internals const& in)
{
    return !in.ended && !in.discarded && !worker_closing(in);
}

void close_channel(Realm::Internals& in, BroadcastChannelObject& channel)
{
    if (channel.closed)
        return;
    channel.closed = true;
    if (channel.subscriber) {
        Subscribers& list = subscribers();
        std::lock_guard<std::mutex> const lock(list.mutex);
        channel.subscriber->closed = true;
        std::erase(list.open, channel.subscriber);
    }
    std::erase(in.broadcast_channels, &channel);
}

Native post_message(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    std::optional<BroadcastChannelObject*> const found = this_channel(interp, this_value);
    if (!found)
        return std::nullopt;
    BroadcastChannelObject& channel = **found;
    Realm::Internals& in = internals_of(interp);
    if (channel.closed)
        return in.throw_dom_exception("InvalidStateError", "Failed to execute 'postMessage' on 'BroadcastChannel': Channel is closed");
    if (!eligible(in))
        return js::Value::undefined();
    std::shared_ptr<SerializedMessage const> const message = structured_serialize_for_another_agent(in, js::argument(args, 0), {});
    if (!message)
        return std::nullopt;
    std::string const origin = in.origin_url.serialize_origin();
    std::vector<std::shared_ptr<Subscriber>> destinations;
    {
        Subscribers& list = subscribers();
        std::lock_guard<std::mutex> const lock(list.mutex);
        // A channel whose realm has ended is let go of here.
        std::erase_if(list.open, [](std::shared_ptr<Subscriber> const& s) { return s->alive.expired(); });
        for (std::shared_ptr<Subscriber> const& subscriber : list.open) {
            if (subscriber != channel.subscriber && subscriber->key == channel.subscriber->key)
                destinations.push_back(subscriber);
        }
    }
    std::sort(destinations.begin(), destinations.end(), [](auto const& a, auto const& b) { return a->order < b->order; });
    for (std::shared_ptr<Subscriber> const& destination : destinations) {
        // On the receiver's thread, in its loop: nothing if its realm ended
        // or it was closed since.
        auto deliver = [destination, message, origin] {
            if (destination->alive.expired())
                return;
            BroadcastChannelObject* const target = destination->object;
            if (target == nullptr || target->closed)
                return;
            Realm::Internals& internals = *destination->internals;
            if (!eligible(internals))
                return;
            Realm::Internals::Entry const entry(internals);
            js::Interpreter::Roots const roots(internals.interpreter);
            internals.interpreter.root(js::Value::object(target));
            std::optional<Deserialized> const received = structured_deserialize(internals, *message);
            if (!received) {
                deliver_message_error(internals, target, origin, js::Value::null());
                return;
            }
            deliver_message(internals, target, received->value, origin, js::Value::null(), {});
        };
        std::function<void()> wake;
        {
            std::lock_guard<std::mutex> const lock(destination->remote->mutex);
            destination->remote->tasks.push_back(std::move(deliver));
            wake = destination->remote->wake;
        }
        if (wake)
            wake();
    }
    return js::Value::undefined();
}

}

void trace_broadcast_channels(Realm::Internals const& in, js::Tracer& tracer)
{
    // An open channel may be sent a message at any time by a script no
    // collection here can see: it stays until it is closed.
    for (js::Object* const channel : in.broadcast_channels)
        tracer.visit(channel);
}

void install_broadcast_channel(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* const proto = define_interface(in, "BroadcastChannel", in.prototype("EventTarget"),
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.empty())
                return interp.throw_type_error("Failed to construct 'BroadcastChannel': 1 argument required, but only 0 present.");
            std::optional<std::string> name = internals.to_utf8(args[0]);
            if (!name)
                return std::nullopt;
            auto* const channel = interp.heap().allocate<BroadcastChannelObject>(internals.prototype("BroadcastChannel"));
            channel->name = std::move(*name);
            auto subscriber = std::make_shared<Subscriber>();
            subscriber->key = channel_key(internals, channel->name);
            subscriber->remote = internals.agent.remote_tasks;
            subscriber->alive = internals.alive;
            subscriber->internals = &internals;
            subscriber->object = channel;
            channel->subscriber = subscriber;
            internals.broadcast_channels.push_back(channel);
            Subscribers& list = subscribers();
            std::lock_guard<std::mutex> const lock(list.mutex);
            subscriber->order = list.next_order++;
            list.open.push_back(std::move(subscriber));
            return js::Value::object(channel);
        },
        1);
    define_getter(in, *proto, "name", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BroadcastChannelObject*> const found = this_channel(interp, this_value);
        if (!found)
            return std::nullopt;
        return internals_of(interp).string((*found)->name);
    });
    define_operation(interpreter, *proto, "postMessage", 1, post_message);
    define_operation(interpreter, *proto, "close", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<BroadcastChannelObject*> const found = this_channel(interp, this_value);
        if (!found)
            return std::nullopt;
        close_channel(internals_of(interp), **found);
        return js::Value::undefined();
    });
    static constexpr std::string_view events[] = { "message", "messageerror" };
    define_event_handlers(in, *proto, events);
}

}
