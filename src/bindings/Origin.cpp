#include "bindings/Internal.h"

// The Origin interface (HTML §7.1.1): an origin as a script object. A new
// Origin is a new opaque origin; Origin.from extracts one from a value — a
// string parsed as a URL, or a platform object with an origin to give: an
// Origin, a URL, a hyperlink element, a message a window posted, a window of
// the caller's origin — and throws a TypeError for anything else. Two opaque
// origins are the same only when they are one origin, so each carries an
// identity: a document keeps one for its own origin as long as its realm
// lasts, and a posted message carries its sender's.

#include "dom/Dom.h"
#include "js/Object.h"
#include "net/Filters.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

// The identities of opaque origins: realms on several threads make them at
// once, and none is ever made twice.
std::atomic<std::uint64_t> next_opaque_identity { 1 };

std::uint64_t new_opaque_identity()
{
    return next_opaque_identity.fetch_add(1, std::memory_order_relaxed);
}

Origin new_opaque_origin()
{
    Origin origin;
    origin.opaque_id = new_opaque_identity();
    return origin;
}

// A URL's origin when it is a tuple (URL §4.7): the scheme, host and port of
// an ftp, http, https, ws or wss URL, or the origin of the URL a blob: URL's
// path holds when that is an http, https or file URL. Nothing for the rest —
// file: among them — whose origin is a new opaque origin.
std::optional<Origin> tuple_origin_of(net::Url const& url)
{
    if (url.scheme == "blob") {
        std::optional<net::Url> const inner = net::parse_url(url.serialize_path());
        if (inner && (inner->scheme == "http" || inner->scheme == "https" || inner->scheme == "file"))
            return tuple_origin_of(*inner);
        return std::nullopt;
    }
    if (url.scheme != "ftp" && url.scheme != "http" && url.scheme != "https" && url.scheme != "ws" && url.scheme != "wss")
        return std::nullopt;
    Origin origin;
    origin.scheme = url.scheme;
    origin.host = url.serialize_host();
    origin.port = url.port;
    return origin;
}

// Same origin (HTML §7.1.1): one opaque origin, or two tuples with the same
// scheme, host and port.
bool same_origin(Origin const& a, Origin const& b)
{
    if (a.opaque() || b.opaque())
        return a.opaque_id == b.opaque_id;
    return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

// Same site (HTML §7.1.1): the sites obtained from the two are the same. An
// opaque origin is its own site; a tuple's is its scheme with its host's
// registrable domain, or with the host itself when that has none. The engine
// has no public suffix list: the registrable domain is net::registrable_domain's
// reading of the host — the last two labels, or three under a country code's
// public second level — and an IP address is taken whole, which gives the
// site the host itself would.
bool same_site(Origin const& a, Origin const& b)
{
    if (a.opaque() || b.opaque())
        return a.opaque_id == b.opaque_id;
    return a.scheme == b.scheme && net::registrable_domain(a.host) == net::registrable_domain(b.host);
}

class OriginObject final : public js::Object {
public:
    OriginObject(js::Object* prototype, Origin the_origin)
        : Object(prototype, Class::Host)
        , origin(std::move(the_origin))
    {
    }
    Origin origin;
    std::size_t size_in_bytes() const override { return sizeof(*this) + origin.scheme.capacity() + origin.host.capacity(); }
};

std::optional<OriginObject*> this_origin(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* origin = dynamic_cast<OriginObject*>(this_value.as_object()))
            return origin;
    }
    return interp.throw_type_error("Illegal invocation");
}

// A new Origin object of the realm, over this origin.
js::Value make_origin(Realm::Internals& in, Origin origin)
{
    return js::Value::object(in.interpreter.heap().allocate<OriginObject>(in.prototype("Origin"), std::move(origin)));
}

// The realm whose window a script value is, among the realms of the caller's
// agent — the page's and every frame's — or null for any other value. A
// window is its realm's global object, which scripts reach through its
// WindowProxy: a WindowProxy is the window it stands for.
Realm::Internals* window_realm_of(Realm::Internals& current, js::Value const& value)
{
    js::Object* window = nullptr;
    if (WindowProxyObject* const proxy = as_window_proxy(value))
        window = proxy->record().intrinsics.global;
    else if (value.is_object() && value.as_object()->class_id() == js::Object::Class::Global)
        window = value.as_object();
    if (window == nullptr)
        return nullptr;
    Realm::Internals* page = &current;
    while (page->parent_realm != nullptr)
        page = page->parent_realm;
    std::vector<Realm::Internals*> pending { page };
    while (!pending.empty()) {
        Realm::Internals* const at = pending.back();
        pending.pop_back();
        if (at->realm_record->intrinsics.global == window)
            return at;
        for (ChildFrame const& listed : at->child_frames)
            pending.push_back(&listed.realm->internals());
    }
    return nullptr;
}

// A hyperlink element's extract-an-origin steps (HTML §4.6.4): the origin of
// its URL, and null when it has none — no href, or one that does not parse
// against its document's URL. The HTML <a> and <area> are hyperlink elements,
// and so is the SVG <a>, by its href or xlink:href, as the Web Platform Tests
// have it; a MathML element is not one.
std::optional<Origin> hyperlink_origin(NodeWrapper& wrapper)
{
    dom::Node& node = wrapper.node();
    if (!node.is_element())
        return std::nullopt;
    auto const& element = static_cast<dom::Element const&>(node);
    std::string const* href = nullptr;
    if (element.is_html("a") || element.is_html("area")) {
        dom::Attr const* const attribute = element.find_attribute("href");
        href = attribute ? &attribute->value : nullptr;
    } else if (element.is_svg("a")) {
        href = svg_href(element);
    }
    if (href == nullptr)
        return std::nullopt;
    std::optional<net::Url> const url = net::parse_url(*href, &wrapper.realm().internals().base_url());
    if (!url)
        return std::nullopt;
    return origin_of_url(*url);
}

// Origin.from(value) (HTML §7.1.1): a platform object's extracted origin when
// it has one, a string's origin as a URL when it parses, else a TypeError. A
// window gives its document's origin only to a caller of the same origin; to
// any other, as a Location always does, it gives null.
Native origin_from(js::Interpreter& interp, js::Value const&, Args args)
{
    Realm::Internals& current = internals_of(interp);
    js::Value const value = js::argument(args, 0);
    std::optional<Origin> origin;
    if (value.is_object()) {
        js::Object* const object = value.as_object();
        if (auto* const given = dynamic_cast<OriginObject*>(object)) {
            origin = given->origin;
        } else if (auto* const url = dynamic_cast<UrlObject*>(object)) {
            origin = origin_of_url(url->url);
        } else if (auto* const event = dynamic_cast<EventObject*>(object)) {
            origin = event->sender_origin;
        } else if (NodeWrapper* const wrapper = current.wrapper_of(value)) {
            origin = hyperlink_origin(*wrapper);
        } else if (Realm::Internals* const window = window_realm_of(current, value)) {
            Origin theirs = document_origin(*window);
            if (same_origin(document_origin(current), theirs))
                origin = std::move(theirs);
        }
        if (!origin)
            return interp.throw_type_error("Failed to execute 'from' on 'Origin': The object has no origin to extract.");
    } else if (value.is_string()) {
        std::optional<std::string> const text = current.to_utf8(value);
        if (!text)
            return std::nullopt;
        std::optional<net::Url> const url = net::parse_url(*text);
        if (!url)
            return interp.throw_type_error("Failed to execute 'from' on 'Origin': Invalid URL '" + *text + "'.");
        origin = origin_of_url(*url);
    } else {
        return interp.throw_type_error("Failed to execute 'from' on 'Origin': The value is neither a string nor an object with an origin.");
    }
    return make_origin(current, std::move(*origin));
}

// isSameOrigin(other) and isSameSite(other): this and the argument must both
// be Origin objects, of any realm.
js::NativeFunction::Callback comparison(bool (*relation)(Origin const&, Origin const&), std::string method)
{
    return [relation, method](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<OriginObject*> const self = this_origin(interp, this_value);
        if (!self)
            return std::nullopt;
        js::Value const other_value = js::argument(args, 0);
        auto* const other = other_value.is_object() ? dynamic_cast<OriginObject*>(other_value.as_object()) : nullptr;
        if (other == nullptr)
            return interp.throw_type_error("Failed to execute '" + method + "' on 'Origin': parameter 1 is not of type 'Origin'.");
        return js::Value::boolean(relation((*self)->origin, other->origin));
    };
}

} // namespace

Origin origin_of_url(net::Url const& url)
{
    if (std::optional<Origin> tuple = tuple_origin_of(url))
        return std::move(*tuple);
    return new_opaque_origin();
}

Origin document_origin(Realm::Internals& in)
{
    if (std::optional<Origin> tuple = tuple_origin_of(in.origin_url))
        return std::move(*tuple);
    if (in.opaque_origin_id == 0)
        in.opaque_origin_id = new_opaque_identity();
    Origin origin;
    origin.opaque_id = in.opaque_origin_id;
    return origin;
}

void install_origin(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* origin = define_interface(in, "Origin", nullptr,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            return make_origin(internals_of(interp), new_opaque_origin());
        },
        0);
    define_getter(in, *origin, "opaque", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<OriginObject*> const self = this_origin(interp, this_value);
        if (!self)
            return std::nullopt;
        return js::Value::boolean((*self)->origin.opaque());
    });
    define_operation(interpreter, *origin, "isSameOrigin", 1, comparison(same_origin, "isSameOrigin"));
    define_operation(interpreter, *origin, "isSameSite", 1, comparison(same_site, "isSameSite"));
    js::Value const constructor = *interpreter.get(*interpreter.global(), interpreter.key("Origin"));
    define_operation(interpreter, *constructor.as_object(), "from", 1, origin_from);
}

}
