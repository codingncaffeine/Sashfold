#include "bindings/Internal.h"

// The WebSocket interface (HTML §9.3): a page's two-way connection to a
// server, net::WebSocket underneath on a thread of its own. What the socket
// says on that thread comes to the page as remote tasks of its agent — open,
// each message, error, close — each checking first that the document that
// opened the socket is still there. A socket that is not CLOSED keeps its
// object alive (HTML §9.3.6, garbage collection): its listeners will hear
// it, whatever the page holds of it.

#include "net/Url.h"
#include "net/WebSocket.h"

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

enum class State : int { Connecting = 0, Open = 1, Closing = 2, Closed = 3 };

class WebSocketObject;

// What the socket's thread holds to reach the page: the agent's remote task
// queue, the realm's alive flag (read on the page's thread only), the realm
// itself, and the object, kept by a Persistent until the socket closes.
struct Link {
    std::shared_ptr<Agent::RemoteTasks> remote;
    std::weak_ptr<bool> alive;
    Realm::Internals* internals = nullptr;
    std::unique_ptr<js::Persistent> object; // the page's thread alone touches it

    void post(std::shared_ptr<Link> const& self, std::function<void(Realm::Internals&, WebSocketObject&)> step);
};

class WebSocketObject final : public EventTargetObject {
public:
    explicit WebSocketObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    std::string url;
    std::string protocol;
    State state = State::Connecting;
    double buffered = 0; // bytes sent once the socket was closing, which go nowhere
    bool arraybuffer = false; // binaryType
    bool failed = false; // an error was said, so close says a 1006
    std::shared_ptr<net::WebSocket> socket;
    std::shared_ptr<Link> link;
};

void Link::post(std::shared_ptr<Link> const& self, std::function<void(Realm::Internals&, WebSocketObject&)> step)
{
    auto run = [self, step = std::move(step)] {
        if (self->alive.expired() || !self->object)
            return;
        auto* target = static_cast<WebSocketObject*>(self->object->value().as_object());
        step(*self->internals, *target);
    };
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> const lock(remote->mutex);
        remote->tasks.push_back(std::move(run));
        wake = remote->wake;
    }
    if (wake)
        wake();
}

std::optional<WebSocketObject*> this_socket(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* socket = dynamic_cast<WebSocketObject*>(this_value.as_object()))
            return socket;
    }
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

void fire_simple(Realm::Internals& in, WebSocketObject& socket, std::string_view type)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&socket));
    EventObject* event = in.new_event("Event", type, false, false);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    in.dispatch(*event, &socket);
}

// The socket has closed: CLOSED, the close event, and the object let go.
void closed(Realm::Internals& in, WebSocketObject& socket, int code, std::string const& reason, bool clean)
{
    if (socket.state == State::Closed)
        return;
    socket.state = State::Closed;
    std::shared_ptr<Link> const link = socket.link;
    {
        js::Interpreter::Roots const roots(in.interpreter);
        in.interpreter.root(js::Value::object(&socket));
        EventObject* event = in.new_event("CloseEvent", "close", false, false);
        in.interpreter.root(js::Value::object(event));
        event->is_trusted = true;
        event->was_clean = clean;
        event->close_code = static_cast<std::uint16_t>(code);
        event->message = reason;
        in.dispatch(*event, &socket);
    }
    if (link)
        link->object.reset();
}

// The origin a message event says: the socket's URL's.
std::string origin_of(std::string const& url)
{
    std::optional<net::Url> const parsed = net::parse_url(url);
    return parsed ? parsed->serialize_origin() : std::string();
}

void deliver(Realm::Internals& in, WebSocketObject& socket, bool binary, std::vector<std::uint8_t> const& data)
{
    if (socket.state != State::Open)
        return;
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&socket));
    js::Value payload;
    if (!binary) {
        payload = in.string(std::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
    } else if (socket.arraybuffer) {
        std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(data.size()), std::nullopt);
        if (!buffer) {
            interp.clear_exception();
            return;
        }
        std::copy(data.begin(), data.end(), (*buffer)->data());
        payload = js::Value::object(*buffer);
    } else {
        payload = js::Value::object(new_blob(in, data, std::string()));
    }
    interp.root(payload);
    EventObject* event = in.new_event("MessageEvent", "message", false, false);
    interp.root(js::Value::object(event));
    event->is_trusted = true;
    event->detail_value = payload;
    event->origin = origin_of(socket.url);
    in.dispatch(*event, &socket);
}

// The protocols argument (HTML §9.3.1 step 6): each a token, none twice.
bool valid_protocol(std::string const& name)
{
    if (name.empty())
        return false;
    for (char const c : name) {
        unsigned char const u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F)
            return false;
        if (std::string_view("()<>@,;:\\\"/[]?={}").find(c) != std::string_view::npos)
            return false;
    }
    return true;
}

Native construct(js::Interpreter& interp, Args args, js::Object* new_target)
{
    (void)new_target;
    Realm::Internals& in = internals_of(interp);
    std::optional<std::string> const text = in.to_utf8(js::argument(args, 0));
    if (!text)
        return std::nullopt;
    // The URL (§9.3.1 steps 1–5): parsed against the document's base, http
    // and https taken as ws and wss, no fragment.
    std::optional<net::Url> url = net::parse_url(*text, &in.base_url());
    if (!url)
        return in.throw_dom_exception("SyntaxError", "The URL '" + *text + "' is invalid.");
    if (url->scheme == "http")
        url->scheme = "ws";
    else if (url->scheme == "https")
        url->scheme = "wss";
    if (url->scheme != "ws" && url->scheme != "wss")
        return in.throw_dom_exception("SyntaxError", "The URL's scheme must be either 'http', 'https', 'ws', or 'wss'. '" + url->scheme + "' is not allowed.");
    if (url->fragment)
        return in.throw_dom_exception("SyntaxError", "The URL contains a fragment identifier ('" + *url->fragment + "'). Fragment identifiers are not allowed in WebSocket URLs.");
    std::vector<std::string> protocols;
    if (args.size() > 1 && !args[1].is_undefined()) {
        js::Value const given = args[1];
        if (given.is_object()) {
            std::optional<std::vector<js::Value>> const list = interp.iterable_to_list(given);
            if (!list)
                return std::nullopt;
            for (js::Value const& item : *list) {
                std::optional<std::string> name = in.to_utf8(item);
                if (!name)
                    return std::nullopt;
                protocols.push_back(std::move(*name));
            }
        } else {
            std::optional<std::string> name = in.to_utf8(given);
            if (!name)
                return std::nullopt;
            protocols.push_back(std::move(*name));
        }
        for (std::size_t i = 0; i < protocols.size(); ++i) {
            if (!valid_protocol(protocols[i]))
                return in.throw_dom_exception("SyntaxError", "The subprotocol '" + protocols[i] + "' is invalid.");
            for (std::size_t j = 0; j < i; ++j) {
                if (protocols[j] == protocols[i])
                    return in.throw_dom_exception("SyntaxError", "The subprotocol '" + protocols[i] + "' is duplicated.");
            }
        }
    }
    // A secure page opens no unencrypted socket (Mixed Content §4.4).
    bool const secure_page = in.origin_url.scheme == "https" || in.origin_url.scheme == "wss";
    if (secure_page && url->scheme == "ws" && url->host != "localhost" && url->host != "127.0.0.1")
        return in.throw_dom_exception("SecurityError", "An insecure WebSocket connection may not be initiated from a page loaded over HTTPS.");

    js::Heap::NoCollect const no_collect(interp.heap());
    auto* socket = interp.heap().allocate<WebSocketObject>(in.prototype("WebSocket"));
    socket->url = url->serialize();
    auto link = std::make_shared<Link>();
    link->remote = in.agent.remote_tasks;
    link->alive = in.alive;
    link->internals = &in;
    link->object = std::make_unique<js::Persistent>(interp.heap(), js::Value::object(socket));
    socket->link = link;

    net::WebSocket::Request request;
    request.url = *url;
    request.protocols = protocols;
    request.origin = in.origin_url.serialize_origin();
    net::WebSocket::Events events;
    events.open = [link](std::string const& protocol) {
        link->post(link, [protocol](Realm::Internals& internals, WebSocketObject& object) {
            if (object.state != State::Connecting)
                return;
            object.state = State::Open;
            object.protocol = protocol;
            fire_simple(internals, object, "open");
        });
    };
    events.message = [link](bool binary, std::vector<std::uint8_t> data) {
        auto shared = std::make_shared<std::vector<std::uint8_t> const>(std::move(data));
        link->post(link, [binary, shared](Realm::Internals& internals, WebSocketObject& object) { deliver(internals, object, binary, *shared); });
    };
    events.failed = [link](std::string const& why) {
        link->post(link, [why](Realm::Internals& internals, WebSocketObject& object) {
            if (object.state == State::Closed || object.failed)
                return;
            object.failed = true;
            internals.console("error", "WebSocket connection to '" + object.url + "' failed: " + why);
            fire_simple(internals, object, "error");
        });
    };
    events.closed = [link](int code, std::string reason, bool clean) {
        link->post(link, [code, reason = std::move(reason), clean](Realm::Internals& internals, WebSocketObject& object) {
            closed(internals, object, object.failed ? 1006 : code, object.failed ? std::string() : reason, clean && !object.failed);
        });
    };

    std::string refused;
    net::RequestGuard const guard = in.hooks.policy ? in.hooks.policy->guard(net::ResourceKind::Xhr, std::string()) : net::RequestGuard {};
    if (in.hooks.open_websocket)
        socket->socket = in.hooks.open_websocket(std::move(request), events, guard, refused);
    else
        refused = "this host opens no WebSockets";
    if (!socket->socket) {
        // Refused before it went: an error and a close, as a connection that
        // failed (§9.3.1 step 10, the fetch failing).
        internals_of(interp).console("error", "WebSocket connection to '" + socket->url + "' failed: " + refused);
        events.failed(refused);
        events.closed(1006, std::string(), false);
    } else {
        std::weak_ptr<Link> const weak_link = link;
        std::weak_ptr<net::WebSocket> const weak_socket = socket->socket;
        in.websockets.push_back([weak_link, weak_socket] {
            if (std::shared_ptr<net::WebSocket> const open = weak_socket.lock())
                open->abort();
            if (std::shared_ptr<Link> const kept = weak_link.lock())
                kept->object.reset();
        });
    }
    return js::Value::object(socket);
}

// close() (§9.3.3): the code 1000 or 3000–4999, a reason of at most 123
// bytes; a connecting socket fails, an open one begins to close.
Native close(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
    if (!self)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    std::optional<int> code;
    if (!args.empty() && !args[0].is_undefined()) {
        std::optional<double> const number = interp.to_number(args[0]);
        if (!number)
            return std::nullopt;
        if (!std::isfinite(*number) || *number < 0 || *number > 65535)
            return interp.throw_type_error("Value is outside the 'unsigned short' value range.");
        int const value = static_cast<int>(*number);
        if (value != 1000 && (value < 3000 || value > 4999))
            return in.throw_dom_exception("InvalidAccessError", "The code must be either 1000, or between 3000 and 4999. " + std::to_string(value) + " is neither.");
        code = value;
    }
    std::string reason;
    if (args.size() > 1 && !args[1].is_undefined()) {
        std::optional<std::string> text = in.to_utf8(args[1]);
        if (!text)
            return std::nullopt;
        if (text->size() > 123)
            return in.throw_dom_exception("SyntaxError", "The message must not be greater than 123 bytes.");
        reason = std::move(*text);
    }
    WebSocketObject& socket = **self;
    if (socket.state == State::Closing || socket.state == State::Closed)
        return js::Value::undefined();
    bool const connecting = socket.state == State::Connecting;
    socket.state = State::Closing;
    if (socket.socket) {
        if (connecting) {
            // Failing a connection the page gave up on: an error, then 1006.
            socket.socket->close(std::nullopt, std::string());
        } else {
            socket.socket->close(code, reason);
        }
    }
    return js::Value::undefined();
}

Native send(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
    if (!self)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    WebSocketObject& socket = **self;
    if (socket.state == State::Connecting)
        return in.throw_dom_exception("InvalidStateError", "Still in CONNECTING state.");
    js::Value const data = js::argument(args, 0);
    std::vector<std::uint8_t> bytes;
    bool binary = true;
    if (std::optional<std::span<std::uint8_t const>> const view = buffer_source_bytes(data)) {
        bytes.assign(view->begin(), view->end());
    } else if (auto const* blob = data.is_object() ? dynamic_cast<BlobObject const*>(data.as_object()) : nullptr) {
        bytes = blob->bytes;
    } else {
        std::optional<std::string> text = in.to_utf8(data);
        if (!text)
            return std::nullopt;
        bytes.assign(text->begin(), text->end());
        binary = false;
    }
    // Once closing, what is sent goes nowhere and counts as buffered.
    if (socket.state != State::Open || !socket.socket || !socket.socket->send(binary, bytes))
        socket.buffered += static_cast<double>(bytes.size());
    return js::Value::undefined();
}

}

void abort_websockets(Realm::Internals& in)
{
    std::vector<std::function<void()>> const ends = std::move(in.websockets);
    in.websockets.clear();
    for (std::function<void()> const& end : ends)
        end();
}

void install_websockets(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* websocket = define_interface(in, "WebSocket", in.prototype("EventTarget"), construct, 1);
    js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key("WebSocket"));
    for (js::Object* target : { constructor.as_object(), websocket }) {
        for (auto const& [name, value] : { std::pair { "CONNECTING", 0 }, std::pair { "OPEN", 1 }, std::pair { "CLOSING", 2 }, std::pair { "CLOSED", 3 } })
            target->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }
    define_getter(in, *websocket, "url", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
        if (!self)
            return std::nullopt;
        return internals_of(interp).string((*self)->url);
    });
    define_getter(in, *websocket, "readyState", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
        if (!self)
            return std::nullopt;
        return js::Value::number(static_cast<int>((*self)->state));
    });
    define_getter(in, *websocket, "bufferedAmount", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
        if (!self)
            return std::nullopt;
        return js::Value::number((*self)->buffered);
    });
    define_getter(in, *websocket, "extensions", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_socket(interp, this_value))
            return std::nullopt;
        return internals_of(interp).string("");
    });
    define_getter(in, *websocket, "protocol", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
        if (!self)
            return std::nullopt;
        return internals_of(interp).string((*self)->protocol);
    });
    define_getter(
        in, *websocket, "binaryType",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
            if (!self)
                return std::nullopt;
            return internals_of(interp).string((*self)->arraybuffer ? "arraybuffer" : "blob");
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<WebSocketObject*> const self = this_socket(interp, this_value);
            if (!self)
                return std::nullopt;
            std::optional<std::string> const value = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            // An enumeration attribute: any other value is ignored.
            if (*value == "blob" || *value == "arraybuffer")
                (*self)->arraybuffer = *value == "arraybuffer";
            return js::Value::undefined();
        });
    define_operation(interpreter, *websocket, "close", 0, close);
    define_operation(interpreter, *websocket, "send", 1, send);
    static constexpr std::string_view events[] = { "open", "message", "error", "close" };
    define_event_handlers(in, *websocket, events);
}

}
