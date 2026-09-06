#include "bindings/Fetching.h"
#include "bindings/Internal.h"

// XMLHttpRequest (the XHR Standard): the five states, a request built
// call by call — open, setRequestHeader, send — carried out through the
// fetching core on the event loop's next task, or at once when asked for
// synchronously, and the events a page listens for: readystatechange at
// every state, then loadstart, progress, load and loadend, or error and
// abort, on the request and on its upload. A response is read as text,
// JSON, an ArrayBuffer or a Blob, as responseType asks.

#include "html/Encoding.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

class XhrObject final : public EventTargetObject {
public:
    explicit XhrObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    enum State : int { Unsent = 0, Opened = 1, HeadersReceived = 2, Loading = 3, Done = 4 };

    int state = Unsent;
    std::string method = "GET";
    net::Url url;
    bool async = true;
    std::vector<std::pair<std::string, std::string>> request_headers; // names lowercased
    bool send_flag = false;
    bool with_credentials = false;
    double timeout = 0;
    std::string response_type; // "", text, json, arraybuffer, blob, document
    std::string override_mime;
    // The response.
    int status = 0;
    std::string status_text;
    std::vector<net::Header> response_headers;
    std::vector<std::uint8_t> body;
    net::Url response_url;
    bool has_response_url = false;
    bool network_error = false;
    js::Value parsed_response; // the JSON, ArrayBuffer or Blob, made once
    bool response_parsed = false;
    EventTargetObject* upload = nullptr;
    // Bumped by open() and abort(): a delivery from an older send is dropped.
    std::uint64_t generation = 0;

    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(parsed_response);
        tracer.visit(upload);
    }
    std::size_t size_in_bytes() const override { return EventTargetObject::size_in_bytes() + body.capacity(); }

    void reset_response()
    {
        status = 0;
        status_text.clear();
        response_headers.clear();
        body.clear();
        has_response_url = false;
        network_error = false;
        parsed_response = js::Value::undefined();
        response_parsed = false;
    }
};

std::optional<XhrObject*> this_xhr(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* xhr = dynamic_cast<XhrObject*>(this_value.as_object()))
            return xhr;
    }
    return interp.throw_type_error("Illegal invocation");
}

bool is_token(std::string_view text)
{
    if (text.empty())
        return false;
    for (char const c : text) {
        bool const ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        if (!ok)
            return false;
    }
    return true;
}

bool is_forbidden_request_header(std::string_view lowered)
{
    static constexpr std::string_view names[] = { "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host",
        "keep-alive", "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via" };
    for (std::string_view const name : names) {
        if (lowered == name)
            return true;
    }
    return lowered.starts_with("proxy-") || lowered.starts_with("sec-");
}

std::string trim_http_whitespace(std::string_view value)
{
    auto const is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && is_space(value[begin]))
        ++begin;
    while (end > begin && is_space(value[end - 1]))
        --end;
    return std::string(value.substr(begin, end - begin));
}

// Fires an event at the request or its upload: a ProgressEvent for the
// progress family, a plain Event for readystatechange.
void fire(Realm::Internals& in, XhrObject& xhr, js::Object* target, std::string_view type, bool progress)
{
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&xhr));
    EventObject* event = in.new_event(progress ? "ProgressEvent" : "Event", type, false, false);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    if (progress) {
        event->length_computable = xhr.state >= XhrObject::HeadersReceived && !xhr.network_error;
        event->loaded = static_cast<double>(xhr.body.size());
        event->total = event->length_computable ? static_cast<double>(xhr.body.size()) : 0;
    }
    in.dispatch(*event, target);
}

void change_state(Realm::Internals& in, XhrObject& xhr, int state)
{
    xhr.state = state;
    fire(in, xhr, &xhr, "readystatechange", false);
}

// The request error steps (XHR §4.5.7): done, the send flag cleared, the
// response a network error, and the event that names what happened.
void request_error(Realm::Internals& in, XhrObject& xhr, std::string_view event, bool had_body)
{
    xhr.state = XhrObject::Done;
    xhr.send_flag = false;
    xhr.reset_response();
    xhr.network_error = true;
    fire(in, xhr, &xhr, "readystatechange", false);
    if (had_body && xhr.upload != nullptr) {
        fire(in, xhr, xhr.upload, event, true);
        fire(in, xhr, xhr.upload, "loadend", true);
    }
    fire(in, xhr, &xhr, event, true);
    fire(in, xhr, &xhr, "loadend", true);
}

// What a fetch answered, delivered as the states and events of §4.5.6.
void deliver(Realm::Internals& in, XhrObject& xhr, FetchOutcome const& outcome, bool had_body)
{
    if (!outcome.ok) {
        in.console("warn", "XMLHttpRequest " + xhr.url.serialize() + ": " + outcome.error);
        request_error(in, xhr, "error", had_body);
        return;
    }
    if (had_body && xhr.upload != nullptr) {
        fire(in, xhr, xhr.upload, "progress", true);
        fire(in, xhr, xhr.upload, "load", true);
        fire(in, xhr, xhr.upload, "loadend", true);
    }
    xhr.status = outcome.status;
    xhr.status_text = outcome.status_text;
    xhr.response_headers = outcome.headers;
    xhr.response_url = outcome.url;
    xhr.has_response_url = !outcome.url.scheme.empty();
    std::uint64_t const generation = xhr.generation;
    change_state(in, xhr, XhrObject::HeadersReceived);
    if (xhr.generation != generation)
        return; // abort() or open() from a listener
    xhr.body = outcome.body;
    change_state(in, xhr, XhrObject::Loading);
    if (xhr.generation != generation)
        return;
    fire(in, xhr, &xhr, "progress", true);
    if (xhr.generation != generation)
        return;
    xhr.send_flag = false;
    change_state(in, xhr, XhrObject::Done);
    fire(in, xhr, &xhr, "load", true);
    fire(in, xhr, &xhr, "loadend", true);
}

// The charset parameter of a MIME type, when it has one.
std::optional<std::string> charset_of(std::string_view mime)
{
    std::string const lowered = ascii_lower(mime);
    std::size_t const at = lowered.find("charset=");
    if (at == std::string::npos)
        return std::nullopt;
    std::string value = lowered.substr(at + 8);
    if (!value.empty() && value.front() == '"')
        value = value.substr(1, value.find('"', 1) - 1);
    if (std::size_t const semicolon = value.find(';'); semicolon != std::string::npos)
        value = value.substr(0, semicolon);
    return trim_http_whitespace(value);
}

std::string content_type_of(XhrObject const& xhr)
{
    if (!xhr.override_mime.empty())
        return xhr.override_mime;
    if (std::string const* const value = net::find_header(xhr.response_headers, "content-type"))
        return *value;
    return {};
}

// The response as text (§4.6.7 "get a text response"): the charset the
// type names, else UTF-8, errors replaced.
js::Value response_text(Realm::Internals& in, XhrObject const& xhr)
{
    html::Encoding encoding = html::Encoding::Utf8;
    if (std::optional<std::string> const charset = charset_of(content_type_of(xhr))) {
        if (std::optional<html::Encoding> const named = html::encoding_from_label(*charset))
            encoding = *named;
    }
    std::string_view const bytes(reinterpret_cast<char const*>(xhr.body.data()), xhr.body.size());
    std::u16string text;
    for (char32_t const code_point : html::decode(bytes, encoding))
        js::append_code_point(text, code_point);
    return js::Value::string(in.interpreter.string(text));
}

// The response as responseType asks (§4.6.6), made once and kept.
Native response_value(Realm::Internals& in, XhrObject& xhr)
{
    js::Interpreter& interp = in.interpreter;
    if (xhr.response_type.empty() || xhr.response_type == "text") {
        if (xhr.state < XhrObject::Loading || xhr.network_error)
            return in.string("");
        return response_text(in, xhr);
    }
    if (xhr.state != XhrObject::Done || xhr.network_error)
        return js::Value::null();
    if (xhr.response_parsed)
        return xhr.parsed_response;
    js::Interpreter::Roots const roots(interp);
    interp.root(js::Value::object(&xhr));
    js::Value made = js::Value::null();
    if (xhr.response_type == "json") {
        js::Value const json = js::Value::object(interp.intrinsics().json);
        std::optional<js::Value> const parse = interp.get(json, "parse");
        if (!parse)
            return std::nullopt;
        js::Value const arguments[1] = { interp.root(response_text(in, xhr)) };
        std::optional<js::Value> const parsed = interp.call(*parse, json, arguments);
        if (parsed)
            made = *parsed;
        else
            interp.clear_exception(); // a body that is not JSON reads as null
    } else if (xhr.response_type == "arraybuffer") {
        std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(xhr.body.size()), std::nullopt);
        if (!buffer)
            return std::nullopt;
        if (!xhr.body.empty())
            std::memcpy((*buffer)->data(), xhr.body.data(), xhr.body.size());
        made = js::Value::object(*buffer);
    } else if (xhr.response_type == "blob") {
        std::string type = content_type_of(xhr);
        if (std::size_t const semicolon = type.find(';'); semicolon != std::string::npos)
            type = type.substr(0, semicolon);
        made = js::Value::object(new_blob(in, xhr.body, ascii_lower(trim_http_whitespace(type))));
    }
    xhr.parsed_response = made;
    xhr.response_parsed = true;
    return made;
}

Native send(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    // send(body) (§4.5.6): the body extracted, the send flag set, loadstart
    // fired, then the fetch — on the next task, or now when synchronous.
    std::optional<XhrObject*> const found = this_xhr(interp, this_value);
    if (!found)
        return std::nullopt;
    XhrObject& xhr = **found;
    Realm::Internals& internals = internals_of(interp);
    if (xhr.state != XhrObject::Opened || xhr.send_flag)
        return internals.throw_dom_exception("InvalidStateError", "Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.");
    js::Interpreter::Roots const roots(interp);
    interp.root(this_value);
    js::Value body_value = js::argument(args, 0);
    interp.root(body_value);
    if (xhr.method == "GET" || xhr.method == "HEAD")
        body_value = js::Value::null();
    PageRequest page_request;
    page_request.url = xhr.url;
    page_request.method = xhr.method;
    for (auto const& [name, value] : xhr.request_headers)
        page_request.headers.push_back({ name, value });
    page_request.mode = FetchMode::Cors;
    page_request.credentials = xhr.with_credentials ? FetchCredentials::Include : FetchCredentials::SameOrigin;
    page_request.redirect = FetchRedirect::Follow;
    bool had_body = false;
    if (!body_value.is_null() && !body_value.is_undefined()) {
        std::string type;
        std::vector<std::uint8_t> bytes;
        if (body_value.is_object()) {
            if (auto const* blob = dynamic_cast<BlobObject const*>(body_value.as_object())) {
                bytes = blob->bytes;
                type = blob->type;
            } else if (std::optional<std::span<std::uint8_t const>> const source = buffer_source_bytes(body_value)) {
                bytes.assign(source->begin(), source->end());
            } else if (auto const* params = dynamic_cast<SearchParamsObject const*>(body_value.as_object())) {
                // Through the URLSearchParams serializer the page sees.
                std::optional<js::Value> const to_string = interp.get(body_value, "toString");
                if (!to_string)
                    return std::nullopt;
                std::optional<js::Value> const text = interp.call(*to_string, body_value, {});
                if (!text)
                    return std::nullopt;
                std::optional<std::string> const encoded = internals.to_utf8(*text);
                if (!encoded)
                    return std::nullopt;
                bytes.assign(encoded->begin(), encoded->end());
                type = "application/x-www-form-urlencoded;charset=UTF-8";
                (void)params;
            } else {
                // FormData and anything else: fetch's Request extracts it.
                js::Value const request_constructor = *interp.get(*interp.global(), interp.key("Request"));
                js::Heap::NoCollect const no_collect(interp.heap());
                js::Object* init = interp.new_object();
                init->put(interp.key("method"), internals.string("POST"));
                init->put(interp.key("body"), body_value);
                js::Value const arguments[2] = { internals.string(xhr.url.serialize()), js::Value::object(init) };
                std::optional<js::Value> const request = interp.construct(request_constructor, arguments);
                if (!request)
                    return std::nullopt;
                std::optional<js::Value> const headers = interp.get(*request, "headers");
                if (!headers)
                    return std::nullopt;
                std::optional<js::Value> const get = interp.get(*headers, "get");
                if (!get)
                    return std::nullopt;
                js::Value const name[1] = { internals.string("content-type") };
                std::optional<js::Value> const content_type = interp.call(*get, *headers, name);
                if (!content_type)
                    return std::nullopt;
                if (content_type->is_string())
                    type = content_type->as_string()->to_utf8();
                std::optional<js::Value> const text_method = interp.get(*request, "arrayBuffer");
                if (!text_method)
                    return std::nullopt;
                std::optional<js::Value> const promise = interp.call(*text_method, *request, {});
                if (!promise)
                    return std::nullopt;
                // The promise is already settled with the buffer: read it
                // through its result rather than a reaction.
                auto* settled = static_cast<js::PromiseObject*>(promise->as_object());
                if (settled->state() == js::PromiseObject::State::Fulfilled && settled->result().is_object()) {
                    if (std::optional<std::span<std::uint8_t const>> const settled_bytes = buffer_source_bytes(settled->result()))
                        bytes.assign(settled_bytes->begin(), settled_bytes->end());
                }
            }
        } else {
            std::optional<js::JsString*> const text = interp.to_string(body_value);
            if (!text)
                return std::nullopt;
            std::string const encoded = encode_utf8((*text)->view());
            bytes.assign(encoded.begin(), encoded.end());
            type = "text/plain;charset=UTF-8";
        }
        had_body = true;
        page_request.body = std::move(bytes);
        page_request.body_type = type;
    }
    xhr.send_flag = true;
    xhr.reset_response();
    fire(internals, xhr, &xhr, "loadstart", true);
    if (had_body && xhr.upload != nullptr)
        fire(internals, xhr, xhr.upload, "loadstart", true);
    if (!xhr.send_flag)
        return js::Value::undefined(); // aborted from loadstart
    if (!xhr.async) {
        FetchOutcome const outcome = perform_fetch(internals, page_request);
        deliver(internals, xhr, outcome, had_body);
        return js::Value::undefined();
    }
    auto held = std::make_shared<js::Persistent>(interp.heap(), this_value);
    std::uint64_t const generation = xhr.generation;
    auto shared_request = std::make_shared<PageRequest>(std::move(page_request));
    internals.post_task([&internals, held, generation, shared_request, had_body] {
        Realm::Internals::Entry const entry(internals);
        auto& request = *static_cast<XhrObject*>(held->value().as_object());
        if (request.generation != generation || !request.send_flag)
            return;
        FetchOutcome const outcome = perform_fetch(internals, *shared_request);
        if (request.generation != generation || !request.send_flag)
            return;
        deliver(internals, request, outcome, had_body);
    });
    return js::Value::undefined();
}

Native open(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    // open(method, url, async = true, username, password) (§4.5.1): the
    // method checked and normalized, the URL parsed against the document,
    // any fetch in flight forgotten, the state moved to OPENED.
    std::optional<XhrObject*> const found = this_xhr(interp, this_value);
    if (!found)
        return std::nullopt;
    XhrObject& xhr = **found;
    Realm::Internals& internals = internals_of(interp);
    if (args.size() < 2)
        return interp.throw_type_error("Failed to execute 'open' on 'XMLHttpRequest': 2 arguments required, but only " + std::to_string(args.size()) + " present.");
    std::optional<std::string> const method = internals.to_utf8(args[0]);
    if (!method)
        return std::nullopt;
    if (!is_token(*method))
        return internals.throw_dom_exception("SyntaxError", "Failed to execute 'open' on 'XMLHttpRequest': '" + *method + "' is not a valid HTTP method.");
    std::string const upper = ascii_upper(*method);
    if (upper == "CONNECT" || upper == "TRACE" || upper == "TRACK")
        return internals.throw_dom_exception("SecurityError", "Failed to execute 'open' on 'XMLHttpRequest': '" + *method + "' HTTP method is unsupported.");
    std::string normalized = *method;
    if (upper == "DELETE" || upper == "GET" || upper == "HEAD" || upper == "OPTIONS" || upper == "POST" || upper == "PUT")
        normalized = upper;
    std::optional<std::string> const url_text = internals.to_utf8(args[1]);
    if (!url_text)
        return std::nullopt;
    std::optional<net::Url> parsed = net::parse_url(*url_text, &internals.url);
    if (!parsed)
        return internals.throw_dom_exception("SyntaxError", "Failed to execute 'open' on 'XMLHttpRequest': Invalid URL");
    if (args.size() > 3 && !args[3].is_undefined() && !args[3].is_null()) {
        std::optional<std::string> const user = internals.to_utf8(args[3]);
        if (!user)
            return std::nullopt;
        parsed->username = *user;
    }
    if (args.size() > 4 && !args[4].is_undefined() && !args[4].is_null()) {
        std::optional<std::string> const password = internals.to_utf8(args[4]);
        if (!password)
            return std::nullopt;
        parsed->password = *password;
    }
    bool const async = args.size() < 3 || args[2].is_undefined() || js::Interpreter::to_boolean(args[2]);
    if (!async && (!xhr.response_type.empty() || xhr.timeout != 0))
        return internals.throw_dom_exception("InvalidAccessError", "Failed to execute 'open' on 'XMLHttpRequest': Synchronous requests must not set a responseType or a timeout.");
    ++xhr.generation; // the fetch in flight, if any, delivers nothing
    xhr.send_flag = false;
    xhr.request_headers.clear();
    xhr.method = normalized;
    xhr.url = *parsed;
    xhr.async = async;
    xhr.reset_response();
    if (xhr.state != XhrObject::Opened) {
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        change_state(internals, xhr, XhrObject::Opened);
    }
    return js::Value::undefined();
}

} // namespace

void install_xhr(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;

    js::Object* event_target = define_interface(in, "XMLHttpRequestEventTarget", in.prototype("EventTarget"));
    static constexpr std::string_view progress_events[] = { "loadstart", "progress", "abort", "error", "load", "timeout", "loadend" };
    define_event_handlers(in, *event_target, progress_events);
    define_interface(in, "XMLHttpRequestUpload", event_target);

    js::Object* xhr = define_interface(in, "XMLHttpRequest", event_target,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            js::Heap::NoCollect const no_collect(interp.heap());
            auto* object = interp.heap().allocate<XhrObject>(internals.prototype("XMLHttpRequest"));
            object->upload = interp.heap().allocate<EventTargetObject>(internals.prototype("XMLHttpRequestUpload"));
            return js::Value::object(object);
        },
        0);
    static constexpr std::string_view state_events[] = { "readystatechange" };
    define_event_handlers(in, *xhr, state_events);
    js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key("XMLHttpRequest"));
    for (auto const& [name, value] : { std::pair { "UNSENT", 0 }, std::pair { "OPENED", 1 }, std::pair { "HEADERS_RECEIVED", 2 },
             std::pair { "LOADING", 3 }, std::pair { "DONE", 4 } }) {
        constructor.as_object()->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        xhr->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
    }

    js::define_method(interpreter, *xhr, "open", 2, open);
    js::define_method(interpreter, *xhr, "send", 0, send);
    js::define_method(interpreter, *xhr, "setRequestHeader", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        // §4.5.2: only between open and send; a forbidden name is quietly
        // dropped, a second value joins the first.
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        XhrObject& object = **found;
        if (object.state != XhrObject::Opened || object.send_flag)
            return internals.throw_dom_exception("InvalidStateError", "Failed to execute 'setRequestHeader' on 'XMLHttpRequest': The object's state must be OPENED.");
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        std::optional<std::string> const raw = name ? internals.to_utf8(js::argument(args, 1)) : std::nullopt;
        if (!name || !raw)
            return std::nullopt;
        std::string const value = trim_http_whitespace(*raw);
        if (!is_token(*name) || value.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos)
            return internals.throw_dom_exception("SyntaxError", "Failed to execute 'setRequestHeader' on 'XMLHttpRequest': '" + *name + "' is not a valid HTTP header field name or value.");
        std::string const lowered = ascii_lower(*name);
        if (is_forbidden_request_header(lowered))
            return js::Value::undefined();
        auto const existing = std::find_if(object.request_headers.begin(), object.request_headers.end(), [&](auto const& entry) { return entry.first == lowered; });
        if (existing == object.request_headers.end())
            object.request_headers.emplace_back(lowered, value);
        else
            existing->second += ", " + value;
        return js::Value::undefined();
    });
    js::define_method(interpreter, *xhr, "abort", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        // §4.5.7: a request in flight ends with abort and loadend; a
        // finished one goes quietly back to UNSENT.
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        XhrObject& object = **found;
        ++object.generation;
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        bool const in_flight = (object.state == XhrObject::Opened && object.send_flag) || object.state == XhrObject::HeadersReceived
            || object.state == XhrObject::Loading;
        if (in_flight)
            request_error(internals, object, "abort", false);
        if (object.state == XhrObject::Done) {
            object.state = XhrObject::Unsent;
            object.send_flag = false;
        }
        return js::Value::undefined();
    });
    js::define_method(interpreter, *xhr, "getResponseHeader", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::string const lowered = ascii_lower(*name);
        if ((*found)->network_error || lowered == "set-cookie" || lowered == "set-cookie2")
            return js::Value::null();
        std::optional<std::string> combined;
        for (net::Header const& header : (*found)->response_headers) {
            if (ascii_lower(header.name) != lowered)
                continue;
            if (!combined)
                combined = trim_http_whitespace(header.value);
            else
                *combined += ", " + trim_http_whitespace(header.value);
        }
        return combined ? internals.string(*combined) : js::Value::null();
    });
    js::define_method(interpreter, *xhr, "getAllResponseHeaders", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if ((*found)->network_error)
            return internals.string("");
        std::vector<std::pair<std::string, std::string>> headers;
        for (net::Header const& header : (*found)->response_headers) {
            std::string const lowered = ascii_lower(header.name);
            if (lowered == "set-cookie" || lowered == "set-cookie2")
                continue;
            auto const existing = std::find_if(headers.begin(), headers.end(), [&](auto const& entry) { return entry.first == lowered; });
            if (existing == headers.end())
                headers.emplace_back(lowered, trim_http_whitespace(header.value));
            else
                existing->second += ", " + trim_http_whitespace(header.value);
        }
        std::sort(headers.begin(), headers.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        std::string out;
        for (auto const& [name, value] : headers)
            out += name + ": " + value + "\r\n";
        return internals.string(out);
    });
    js::define_method(interpreter, *xhr, "overrideMimeType", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if ((*found)->state == XhrObject::Loading || (*found)->state == XhrObject::Done)
            return internals.throw_dom_exception("InvalidStateError", "Failed to execute 'overrideMimeType' on 'XMLHttpRequest': MimeType cannot be overridden when the state is LOADING or DONE.");
        std::optional<std::string> const mime = internals.to_utf8(js::argument(args, 0));
        if (!mime)
            return std::nullopt;
        (*found)->override_mime = *mime;
        return js::Value::undefined();
    });

    define_getter(in, *xhr, "readyState", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        return found ? Native(js::Value::number((*found)->state)) : std::nullopt;
    });
    define_getter(in, *xhr, "status", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        return found ? Native(js::Value::number((*found)->network_error ? 0 : (*found)->status)) : std::nullopt;
    });
    define_getter(in, *xhr, "statusText", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        return found ? Native(internals_of(interp).string((*found)->network_error ? std::string() : (*found)->status_text)) : std::nullopt;
    });
    define_getter(in, *xhr, "responseURL", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        return internals_of(interp).string((*found)->has_response_url ? (*found)->response_url.serialize(true) : std::string());
    });
    define_getter(in, *xhr, "responseText", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        if (!(*found)->response_type.empty() && (*found)->response_type != "text")
            return internals.throw_dom_exception("InvalidStateError", "Failed to read the 'responseText' property from 'XMLHttpRequest': The value is only accessible if the object's 'responseType' is '' or 'text'.");
        if ((*found)->state < XhrObject::Loading || (*found)->network_error)
            return internals.string("");
        return response_text(internals, **found);
    });
    define_getter(in, *xhr, "response", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        if (!found)
            return std::nullopt;
        return response_value(internals_of(interp), **found);
    });
    define_getter(in, *xhr, "responseXML", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        return found ? Native(js::Value::null()) : std::nullopt;
    });
    define_getter(in, *xhr, "upload", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<XhrObject*> const found = this_xhr(interp, this_value);
        return found ? Native(js::Value::object((*found)->upload)) : std::nullopt;
    });
    define_getter(
        in, *xhr, "responseType",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<XhrObject*> const found = this_xhr(interp, this_value);
            return found ? Native(internals_of(interp).string((*found)->response_type)) : std::nullopt;
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<XhrObject*> const found = this_xhr(interp, this_value);
            if (!found)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            XhrObject& object = **found;
            if (object.state == XhrObject::Loading || object.state == XhrObject::Done)
                return internals.throw_dom_exception("InvalidStateError", "Failed to set the 'responseType' property on 'XMLHttpRequest': The response type cannot be set if the object's state is LOADING or DONE.");
            if (!object.async && object.state != XhrObject::Unsent)
                return internals.throw_dom_exception("InvalidAccessError", "Failed to set the 'responseType' property on 'XMLHttpRequest': The response type cannot be changed for synchronous requests made from a document.");
            std::optional<std::string> const value = internals.to_utf8(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            for (std::string_view const allowed : { "", "arraybuffer", "blob", "document", "json", "text" }) {
                if (*value == allowed) {
                    object.response_type = *value;
                    break;
                }
            }
            return js::Value::undefined();
        });
    define_getter(
        in, *xhr, "timeout",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<XhrObject*> const found = this_xhr(interp, this_value);
            return found ? Native(js::Value::number((*found)->timeout)) : std::nullopt;
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<XhrObject*> const found = this_xhr(interp, this_value);
            if (!found)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            if (!(*found)->async && (*found)->state != XhrObject::Unsent)
                return internals.throw_dom_exception("InvalidAccessError", "Failed to set the 'timeout' property on 'XMLHttpRequest': Timeouts cannot be set for synchronous requests made from a document.");
            std::optional<double> const value = interp.to_number(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            (*found)->timeout = std::isnan(*value) || *value < 0 ? 0 : std::floor(*value);
            return js::Value::undefined();
        });
    define_getter(
        in, *xhr, "withCredentials",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<XhrObject*> const found = this_xhr(interp, this_value);
            return found ? Native(js::Value::boolean((*found)->with_credentials)) : std::nullopt;
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<XhrObject*> const found = this_xhr(interp, this_value);
            if (!found)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            if (((*found)->state != XhrObject::Unsent && (*found)->state != XhrObject::Opened) || (*found)->send_flag)
                return internals.throw_dom_exception("InvalidStateError", "Failed to set the 'withCredentials' property on 'XMLHttpRequest': The value may only be set if the object's state is UNSENT or OPENED.");
            (*found)->with_credentials = js::Interpreter::to_boolean(js::argument(args, 0));
            return js::Value::undefined();
        });
}

}
