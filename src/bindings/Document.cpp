#include "bindings/NodeSupport.h"

// The Document interface (DOM §4.5, HTML §3.1), DOMParser, and the
// DOMImplementation behind document.implementation.

#include "core/Unicode.h"
#include "html/Serializer.h"
#include "html/TreeBuilder.h"

#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

bool is_valid_element_name(std::string_view name)
{
    if (name.empty())
        return false;
    char const first = name[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_' || first == ':'
            || static_cast<unsigned char>(first) >= 0x80))
        return false;
    for (char const c : name) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == '/' || c == '>' || c == '<' || c == '='
            || c == '"' || c == '\'' || c == '\0')
            return false;
    }
    return true;
}

std::string document_title_text(dom::Document& document)
{
    std::vector<dom::Node*> descendants;
    collect_descendants(document, descendants);
    for (dom::Node* node : descendants) {
        if (node->is_element() && static_cast<dom::Element*>(node)->is_html("title")) {
            std::string const text = html::text_content(*node);
            std::string out;
            bool pending = false;
            for (char const c : text) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                    pending = !out.empty();
                    continue;
                }
                if (pending)
                    out += ' ';
                pending = false;
                out += c;
            }
            return out;
        }
    }
    return "";
}

// A collection of the elements a predicate picks, in tree order.
template<typename Predicate>
Native collection_of(Realm::Internals& in, dom::Node& root, Predicate predicate)
{
    std::vector<dom::Node*> descendants;
    collect_descendants(root, descendants);
    std::vector<dom::Node*> found;
    for (dom::Node* node : descendants) {
        if (node->is_element() && predicate(static_cast<dom::Element&>(*node)))
            found.push_back(node);
    }
    js::Interpreter::Roots const roots(in.interpreter);
    js::Value const list = in.interpreter.root(node_list(in, found));
    list.as_object()->set_prototype(in.prototype("HTMLCollection"));
    return list;
}

// A document a script asked for, owned by the realm.
dom::Document& new_extra_document(Realm::Internals& in)
{
    in.extra_documents.push_back(std::make_unique<dom::Document>());
    return *in.extra_documents.back();
}

// document.cookie through the hooks, else the realm's own jar.
std::string cookie_string(Realm::Internals& in)
{
    if (in.hooks.cookie_get)
        return in.hooks.cookie_get();
    std::string out;
    for (auto const& [name, value] : in.cookies) {
        if (!out.empty())
            out += "; ";
        out += name + "=" + value;
    }
    return out;
}

void set_cookie(Realm::Internals& in, std::string_view text)
{
    if (in.hooks.cookie_set) {
        in.hooks.cookie_set(text);
        return;
    }
    // name=value; attributes — the pair kept, a Max-Age of zero or an
    // expiry in the past removing it.
    std::size_t const semicolon = text.find(';');
    std::string_view const pair = text.substr(0, semicolon);
    std::size_t const equals = pair.find('=');
    std::string name(equals == std::string_view::npos ? "" : pair.substr(0, equals));
    std::string value(equals == std::string_view::npos ? pair : pair.substr(equals + 1));
    auto const trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
    };
    trim(name);
    trim(value);
    if (name.empty() && value.empty())
        return;
    bool expired = false;
    std::string const rest = ascii_lower(semicolon == std::string_view::npos ? "" : std::string(text.substr(semicolon + 1)));
    if (rest.find("max-age=0") != std::string::npos || rest.find("max-age=-") != std::string::npos
        || rest.find("expires=thu, 01 jan 1970") != std::string::npos)
        expired = true;
    for (auto it = in.cookies.begin(); it != in.cookies.end(); ++it) {
        if (it->first == name) {
            if (expired)
                in.cookies.erase(it);
            else
                it->second = value;
            return;
        }
    }
    if (!expired)
        in.cookies.emplace_back(std::move(name), std::move(value));
}

} // namespace

void install_document(Realm::Internals& in, js::Object& node_prototype)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* document = define_interface(in, "Document", &node_prototype,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            return js::Value::object(internals.wrap(new_extra_document(internals)));
        });
    in.prototypes["HTMLDocument"] = document;
    interpreter.global()->put(interpreter.key("HTMLDocument"),
        *interpreter.global()->get(interpreter, interpreter.key("Document"), js::Value::object(interpreter.global())), js::builtin_attributes);
    in.prototypes["XMLDocument"] = document;

    // ParentNode over the document, and the on<type> handlers.
    static constexpr std::string_view document_event_types[] = { "readystatechange", "visibilitychange", "selectionchange", "click",
        "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "mouseout", "mouseenter", "mouseleave", "keydown", "keyup",
        "keypress", "input", "change", "submit", "reset", "focus", "blur", "focusin", "focusout", "scroll", "wheel", "contextmenu",
        "touchstart", "touchend", "touchmove", "touchcancel", "pointerdown", "pointerup", "pointermove", "pointerover", "pointerout",
        "pointerenter", "pointerleave", "pointercancel", "load", "error", "copy", "cut", "paste", "drag", "dragstart", "dragend",
        "dragover", "dragenter", "dragleave", "drop", "animationend", "animationstart", "animationiteration", "transitionend",
        "fullscreenchange", "fullscreenerror", "securitypolicyviolation", "beforeinput", "toggle", "select" };
    define_event_handlers(in, *document, document_event_types);
    // Everything ParentNode gives an element, the document has too.
    for (std::string_view const name : { "children", "childElementCount", "firstElementChild", "lastElementChild" }) {
        std::optional<js::PropertyDescriptor> const descriptor = in.prototype("Element")->get_own_property(interpreter.key(name));
        if (descriptor && descriptor->get)
            document->put_accessor(interpreter.key(name), *descriptor->get, descriptor->set.value_or(nullptr), js::Configurable);
    }
    for (std::string_view const name : { "append", "prepend", "replaceChildren", "querySelector", "querySelectorAll", "getElementsByTagName",
             "getElementsByTagNameNS", "getElementsByClassName" }) {
        std::optional<js::PropertyDescriptor> const descriptor = in.prototype("Element")->get_own_property(interpreter.key(name));
        if (descriptor && descriptor->value)
            document->put(interpreter.key(name), *descriptor->value, js::builtin_attributes);
    }

    document_getter(in, *document, "documentElement", [](Realm::Internals& internals, dom::Document& d) -> Native { return internals.realm.wrap_or_null(document_element(d)); });
    document_getter(in, *document, "head", [](Realm::Internals& internals, dom::Document& d) -> Native { return internals.realm.wrap_or_null(head_element(d)); });
    define_getter(
        in, *document, "body",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Document*> const d = this_document(interp, this_value);
            if (!d)
                return std::nullopt;
            return internals_of(interp).realm.wrap_or_null(body_element(**d));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Document*> const d = this_document(interp, this_value);
            if (!d)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            dom::Node* replacement = internals.realm.node_of(js::argument(args, 0));
            if (!replacement || !replacement->is_element()
                || !(static_cast<dom::Element*>(replacement)->is_html("body") || static_cast<dom::Element*>(replacement)->is_html("frameset")))
                return internals.throw_dom_exception("HierarchyRequestError", "The new body element is of type '"
                    + (replacement && replacement->is_element() ? tag_name_of(*static_cast<dom::Element*>(replacement)) : std::string("?")) + "'. It must be either a 'BODY' or 'FRAMESET' element.");
            dom::Element* html = document_element(**d);
            if (!html)
                return internals.throw_dom_exception("HierarchyRequestError", "No document element");
            if (dom::Element* old = body_element(**d)) {
                dom::Node* reference = next_sibling_of(*old);
                remove_node(internals, *old);
                return pre_insert(internals, *html, *replacement, reference) ? js::Value::undefined() : Native(std::nullopt);
            }
            return pre_insert(internals, *html, *replacement, nullptr) ? js::Value::undefined() : Native(std::nullopt);
        });
    define_getter(
        in, *document, "title",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Document*> const d = this_document(interp, this_value);
            if (!d)
                return std::nullopt;
            return internals_of(interp).string(document_title_text(**d));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Document*> const d = this_document(interp, this_value);
            if (!d)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            std::vector<dom::Node*> descendants;
            collect_descendants(**d, descendants);
            dom::Element* title = nullptr;
            for (dom::Node* n : descendants) {
                if (n->is_element() && static_cast<dom::Element*>(n)->is_html("title")) {
                    title = static_cast<dom::Element*>(n);
                    break;
                }
            }
            if (!title) {
                dom::Element* head = head_element(**d);
                if (!head)
                    return js::Value::undefined();
                title = (*d)->create<dom::Element>(std::string(dom::ns::html), "title");
                head->append_child(*title);
            }
            replace_children_with_text(internals, *title, *text);
            return js::Value::undefined();
        });
    document_getter(in, *document, "URL", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return internals.string(&d == &internals.document ? internals.url.serialize() : "about:blank");
    });
    document_getter(in, *document, "documentURI", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return internals.string(&d == &internals.document ? internals.url.serialize() : "about:blank");
    });
    document_getter(in, *document, "location", [](Realm::Internals& internals, dom::Document& d) -> Native {
        if (&d != &internals.document || !internals.location)
            return js::Value::null();
        return js::Value::object(internals.location);
    });
    document_getter(in, *document, "defaultView", [](Realm::Internals& internals, dom::Document& d) -> Native {
        if (&d != &internals.document)
            return js::Value::null();
        return js::Value::object(internals.interpreter.global());
    });
    document_getter(in, *document, "readyState", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return internals.string(&d == &internals.document ? internals.ready_state : "complete");
    });
    for (std::string_view const name : { "characterSet", "charset", "inputEncoding" })
        document_getter(in, *document, name, [](Realm::Internals& internals, dom::Document&) -> Native { return internals.string("UTF-8"); });
    document_getter(in, *document, "contentType", [](Realm::Internals& internals, dom::Document&) -> Native { return internals.string("text/html"); });
    document_getter(in, *document, "compatMode", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return internals.string(d.quirks_mode == dom::QuirksMode::Yes ? "BackCompat" : "CSS1Compat");
    });
    document_getter(in, *document, "doctype", [](Realm::Internals& internals, dom::Document& d) -> Native {
        for (dom::Node* child : d.children()) {
            if (child->type() == dom::NodeType::DocumentType)
                return js::Value::object(internals.wrap(*child));
        }
        return js::Value::null();
    });
    define_getter(
        in, *document, "cookie",
        [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Document*> const d = this_document(interp, this_value);
            if (!d)
                return std::nullopt;
            return internals_of(interp).string(cookie_string(internals_of(interp)));
        },
        [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Document*> const d = this_document(interp, this_value);
            if (!d)
                return std::nullopt;
            std::optional<std::string> const text = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!text)
                return std::nullopt;
            set_cookie(internals_of(interp), *text);
            return js::Value::undefined();
        });
    document_getter(in, *document, "referrer", [](Realm::Internals& internals, dom::Document&) -> Native { return internals.string(""); });
    document_getter(in, *document, "domain", [](Realm::Internals& internals, dom::Document&) -> Native { return internals.string(internals.url.serialize_host()); });
    document_getter(in, *document, "lastModified", [](Realm::Internals& internals, dom::Document&) -> Native { return internals.string("01/01/1970 00:00:00"); });
    document_getter(in, *document, "hidden", [](Realm::Internals&, dom::Document&) -> Native { return js::Value::boolean(false); });
    document_getter(in, *document, "visibilityState", [](Realm::Internals& internals, dom::Document&) -> Native { return internals.string("visible"); });
    document_getter(in, *document, "dir", [](Realm::Internals& internals, dom::Document& d) -> Native {
        dom::Element* html = document_element(d);
        return internals.string(html ? attribute_or_empty(*html, "dir") : "");
    });
    document_getter(in, *document, "designMode", [](Realm::Internals& internals, dom::Document&) -> Native { return internals.string("off"); });
    document_getter(in, *document, "activeElement", [](Realm::Internals& internals, dom::Document& d) -> Native {
        dom::Element const* focused = focused_element(internals);
        if (focused && focused->is_connected())
            return js::Value::object(internals.wrap(const_cast<dom::Element&>(*focused)));
        return internals.realm.wrap_or_null(body_element(d));
    });
    document_getter(in, *document, "scrollingElement", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return internals.realm.wrap_or_null(d.quirks_mode == dom::QuirksMode::Yes ? static_cast<dom::Node*>(body_element(d)) : document_element(d));
    });
    document_getter(in, *document, "currentScript", [](Realm::Internals& internals, dom::Document&) -> Native {
        return internals.realm.wrap_or_null(internals.current_script);
    });
    document_getter(in, *document, "fullscreenElement", [](Realm::Internals&, dom::Document&) -> Native { return js::Value::null(); });
    document_getter(in, *document, "pointerLockElement", [](Realm::Internals&, dom::Document&) -> Native { return js::Value::null(); });
    document_getter(in, *document, "fullscreenEnabled", [](Realm::Internals&, dom::Document&) -> Native { return js::Value::boolean(false); });
    document_getter(in, *document, "styleSheets", [](Realm::Internals& internals, dom::Document&) -> Native {
        return js::Value::object(internals.interpreter.new_array());
    });
    document_getter(in, *document, "fonts", [](Realm::Internals& internals, dom::Document&) -> Native {
        js::Heap::NoCollect const no_collect(internals.interpreter.heap());
        js::Object* fonts = internals.interpreter.new_object();
        fonts->put(internals.interpreter.key("status"), internals.string("loaded"));
        fonts->put(internals.interpreter.key("size"), js::Value::number(0));
        js::define_method(internals.interpreter, *fonts, "check", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(true); });
        js::define_method(internals.interpreter, *fonts, "load", 1, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
        js::define_method(internals.interpreter, *fonts, "addEventListener", 2, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
        js::define_method(internals.interpreter, *fonts, "removeEventListener", 2, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::undefined(); });
        return js::Value::object(fonts);
    });
    document_getter(in, *document, "implementation", [](Realm::Internals& internals, dom::Document&) -> Native {
        js::Heap::NoCollect const no_collect(internals.interpreter.heap());
        js::Object* implementation = internals.interpreter.new_object(internals.prototype("DOMImplementation"));
        return js::Value::object(implementation);
    });
    document_getter(in, *document, "timeline", [](Realm::Internals& internals, dom::Document&) -> Native {
        js::Heap::NoCollect const no_collect(internals.interpreter.heap());
        js::Object* timeline = internals.interpreter.new_object();
        timeline->put(internals.interpreter.key("currentTime"), js::Value::number(internals.now() - internals.time_origin));
        return js::Value::object(timeline);
    });

    // The element collections.
    document_getter(in, *document, "forms", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return collection_of(internals, d, [](dom::Element& e) { return e.is_html("form"); });
    });
    document_getter(in, *document, "images", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return collection_of(internals, d, [](dom::Element& e) { return e.is_html("img"); });
    });
    document_getter(in, *document, "links", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return collection_of(internals, d, [](dom::Element& e) { return (e.is_html("a") || e.is_html("area")) && e.has_attribute("href"); });
    });
    document_getter(in, *document, "anchors", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return collection_of(internals, d, [](dom::Element& e) { return e.is_html("a") && e.has_attribute("name"); });
    });
    document_getter(in, *document, "scripts", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return collection_of(internals, d, [](dom::Element& e) { return e.is_html("script"); });
    });
    document_getter(in, *document, "embeds", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return collection_of(internals, d, [](dom::Element& e) { return e.is_html("embed"); });
    });
    document_getter(in, *document, "plugins", [](Realm::Internals& internals, dom::Document& d) -> Native {
        return collection_of(internals, d, [](dom::Element& e) { return e.is_html("embed"); });
    });

    // Creating nodes.
    document_method(in, *document, "createElement", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        if (!is_valid_element_name(*name))
            return internals.throw_dom_exception("InvalidCharacterError", "The tag name provided ('" + *name + "') is not a valid name.");
        dom::Element* element = d.create<dom::Element>(std::string(dom::ns::html), ascii_lower(*name));
        return js::Value::object(internals.wrap(*element));
    });
    document_method(in, *document, "createElementNS", 2, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        js::Value const namespace_value = js::argument(args, 0);
        std::optional<std::string> const namespace_uri = namespace_value.is_nullish() ? std::optional<std::string>("") : internals.to_utf8(namespace_value);
        std::optional<std::string> const qualified = internals.to_utf8(js::argument(args, 1));
        if (!namespace_uri || !qualified)
            return std::nullopt;
        if (!is_valid_element_name(*qualified))
            return internals.throw_dom_exception("InvalidCharacterError", "The qualified name provided ('" + *qualified + "') is not a valid name.");
        std::size_t const colon = qualified->find(':');
        std::string const local = colon == std::string::npos ? *qualified : qualified->substr(colon + 1);
        dom::Element* element = d.create<dom::Element>(*namespace_uri, local);
        return js::Value::object(internals.wrap(*element));
    });
    document_method(in, *document, "createTextNode", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        std::optional<std::string> data = internals.to_utf8(js::argument(args, 0));
        if (!data)
            return std::nullopt;
        dom::Text* text = d.create<dom::Text>();
        text->data = std::move(*data);
        return js::Value::object(internals.wrap(*text));
    });
    document_method(in, *document, "createComment", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        std::optional<std::string> data = internals.to_utf8(js::argument(args, 0));
        if (!data)
            return std::nullopt;
        dom::Comment* comment = d.create<dom::Comment>();
        comment->data = std::move(*data);
        return js::Value::object(internals.wrap(*comment));
    });
    document_method(in, *document, "createDocumentFragment", 0, [](Realm::Internals& internals, dom::Document& d, Args) -> Native {
        return js::Value::object(internals.wrap(*d.create<dom::DocumentFragment>()));
    });
    document_method(in, *document, "createAttribute", 1, [](Realm::Internals& internals, dom::Document&, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        js::Heap::NoCollect const no_collect(internals.interpreter.heap());
        js::Object* attr = internals.interpreter.new_object(internals.prototype("Attr"));
        attr->put(internals.interpreter.key("name"), internals.string(ascii_lower(*name)), js::Enumerable);
        attr->put(internals.interpreter.key("localName"), internals.string(ascii_lower(*name)), js::Enumerable);
        attr->put(internals.interpreter.key("value"), internals.string(""), js::Enumerable | js::Writable);
        attr->put(internals.interpreter.key("nodeType"), js::Value::number(2), js::Enumerable);
        return js::Value::object(attr);
    });
    document_method(in, *document, "createEvent", 1, [](Realm::Internals& internals, dom::Document&, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::string const lower = ascii_lower(*name);
        std::string interface;
        if (lower == "event" || lower == "events" || lower == "htmlevents" || lower == "svgevents")
            interface = "Event";
        else if (lower == "customevent")
            interface = "CustomEvent";
        else if (lower == "mouseevent" || lower == "mouseevents")
            interface = "MouseEvent";
        else if (lower == "keyboardevent")
            interface = "KeyboardEvent";
        else if (lower == "uievent" || lower == "uievents")
            interface = "UIEvent";
        else if (lower == "focusevent")
            interface = "FocusEvent";
        else
            return internals.throw_dom_exception("NotSupportedError", "The provided event type ('" + *name + "') is invalid.");
        EventObject* event = internals.new_event(interface, "", false, false);
        event->initialized = false;
        return js::Value::object(event);
    });
    document_method(in, *document, "createRange", 0, [](Realm::Internals& internals, dom::Document&, Args) -> Native {
        return internals.throw_dom_exception("NotSupportedError", "Ranges are not supported");
    });
    document_method(in, *document, "createTreeWalker", 1, [](Realm::Internals& internals, dom::Document&, Args) -> Native {
        return internals.throw_dom_exception("NotSupportedError", "Tree walkers are not supported");
    });
    document_method(in, *document, "createNodeIterator", 1, [](Realm::Internals& internals, dom::Document&, Args) -> Native {
        return internals.throw_dom_exception("NotSupportedError", "Node iterators are not supported");
    });
    document_method(in, *document, "importNode", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        dom::Node* node = internals.realm.node_of(js::argument(args, 0));
        if (!node)
            return internals.interpreter.throw_type_error("parameter 1 is not of type 'Node'");
        if (node->type() == dom::NodeType::Document)
            return internals.throw_dom_exception("NotSupportedError", "The node provided is a document, which may not be imported.");
        bool const deep = js::Interpreter::to_boolean(js::argument(args, 1));
        dom::Node* clone = deep ? dom::clone_subtree(*node, d) : clone_node(internals, *node, false);
        d.adopt(*clone);
        return js::Value::object(internals.wrap(*clone));
    });
    document_method(in, *document, "adoptNode", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        dom::Node* node = internals.realm.node_of(js::argument(args, 0));
        if (!node)
            return internals.interpreter.throw_type_error("parameter 1 is not of type 'Node'");
        if (node->type() == dom::NodeType::Document)
            return internals.throw_dom_exception("NotSupportedError", "The node provided is a document, which may not be adopted.");
        d.adopt(*node);
        internals.realm.note_mutation();
        return js::Value::object(internals.wrap(*node));
    });

    // Finding elements.
    document_method(in, *document, "getElementById", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        std::optional<std::string> const id = internals.to_utf8(js::argument(args, 0));
        if (!id)
            return std::nullopt;
        return internals.realm.wrap_or_null(element_by_id(d, *id));
    });
    document_method(in, *document, "getElementsByName", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::vector<dom::Node*> descendants;
        collect_descendants(d, descendants);
        std::vector<dom::Node*> found;
        for (dom::Node* node : descendants) {
            if (node->is_element() && attribute_or_empty(static_cast<dom::Element&>(*node), "name") == *name
                && static_cast<dom::Element&>(*node).has_attribute("name"))
                found.push_back(node);
        }
        return node_list(internals, found);
    });
    document_method(in, *document, "elementFromPoint", 2, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::null(); });
    document_method(in, *document, "elementsFromPoint", 2, [](Realm::Internals& internals, dom::Document&, Args) -> Native {
        return js::Value::object(internals.interpreter.new_array());
    });
    document_method(in, *document, "caretRangeFromPoint", 2, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::null(); });
    document_method(in, *document, "hasFocus", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::boolean(true); });
    document_method(in, *document, "hasStorageAccess", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::undefined(); });
    document_method(in, *document, "execCommand", 1, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::boolean(false); });
    document_method(in, *document, "queryCommandSupported", 1, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::boolean(false); });
    document_method(in, *document, "queryCommandEnabled", 1, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::boolean(false); });
    document_method(in, *document, "exitFullscreen", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::undefined(); });
    document_method(in, *document, "exitPointerLock", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::undefined(); });
    document_method(in, *document, "getSelection", 0, [](Realm::Internals& internals, dom::Document&, Args) -> Native {
        return internals.interpreter.get(js::Value::object(internals.interpreter.global()), "getSelection").and_then(
            [&](js::Value const& getter) -> Native { return internals.interpreter.call(getter, js::Value::object(internals.interpreter.global()), {}); });
    });
    document_method(in, *document, "getAnimations", 0, [](Realm::Internals& internals, dom::Document&, Args) -> Native {
        return js::Value::object(internals.interpreter.new_array());
    });

    // document.write while the parser runs: the text goes at the insertion
    // point and is parsed next. Afterwards it would replace the document,
    // which is not written; the call is reported and ignored.
    auto const write = [](bool newline) {
        return [newline](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
            std::string text;
            for (js::Value const& argument : args) {
                std::optional<std::string> const piece = internals.to_utf8(argument);
                if (!piece)
                    return std::nullopt;
                text += *piece;
            }
            if (newline)
                text += '\n';
            if (&d == &internals.document && internals.active_parser) {
                internals.active_parser->insert_input(decode_utf8(text));
                return js::Value::undefined();
            }
            internals.console("warn", "document.write after the document was parsed is not supported; the text was dropped");
            return js::Value::undefined();
        };
    };
    document_method(in, *document, "write", 1, write(false));
    document_method(in, *document, "writeln", 1, write(true));
    document_method(in, *document, "open", 0, [](Realm::Internals& internals, dom::Document& d, Args) -> Native { return js::Value::object(internals.wrap(d)); });
    document_method(in, *document, "close", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::undefined(); });
    document_method(in, *document, "clear", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::undefined(); });
    document_method(in, *document, "captureEvents", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::undefined(); });
    document_method(in, *document, "releaseEvents", 0, [](Realm::Internals&, dom::Document&, Args) -> Native { return js::Value::undefined(); });

    // DOMImplementation.
    js::Object* implementation = define_interface(in, "DOMImplementation", nullptr);
    js::define_method(interpreter, *implementation, "createHTMLDocument", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> title;
        if (!args.empty() && !args[0].is_undefined()) {
            title = internals.to_utf8(args[0]);
            if (!title)
                return std::nullopt;
        }
        dom::Document& d = new_extra_document(internals);
        std::string markup = "<!DOCTYPE html><html><head>";
        if (title) {
            markup += "<title></title>";
        }
        markup += "</head><body></body></html>";
        html::parse_document_into(d, decode_utf8(markup));
        if (title) {
            std::vector<dom::Node*> descendants;
            collect_descendants(d, descendants);
            for (dom::Node* n : descendants) {
                if (n->is_element() && static_cast<dom::Element*>(n)->is_html("title")) {
                    dom::Text* text = d.create<dom::Text>();
                    text->data = *title;
                    n->append_child(*text);
                    break;
                }
            }
        }
        return js::Value::object(internals.wrap(d));
    });
    js::define_method(interpreter, *implementation, "createDocument", 2, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(internals.wrap(new_extra_document(internals)));
    });
    js::define_method(interpreter, *implementation, "createDocumentType", 3, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> name = internals.to_utf8(js::argument(args, 0));
        std::optional<std::string> public_id = internals.to_utf8(js::argument(args, 1));
        std::optional<std::string> system_id = internals.to_utf8(js::argument(args, 2));
        if (!name || !public_id || !system_id)
            return std::nullopt;
        dom::DocumentType* doctype = internals.document.create<dom::DocumentType>();
        doctype->name = std::move(*name);
        doctype->public_identifier = std::move(*public_id);
        doctype->system_identifier = std::move(*system_id);
        return js::Value::object(internals.wrap(*doctype));
    });
    js::define_method(interpreter, *implementation, "hasFeature", 0, [](js::Interpreter&, js::Value const&, Args) -> Native { return js::Value::boolean(true); });

    // DOMParser: parseFromString makes a document of its own, with
    // scripting off, as the specification says.
    js::Object* dom_parser = define_interface(in, "DOMParser", nullptr,
        [](js::Interpreter& interp, Args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            return js::Value::object(interp.new_object(internals.prototype("DOMParser")));
        });
    js::define_method(interpreter, *dom_parser, "parseFromString", 2, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const text = internals.to_utf8(js::argument(args, 0));
        std::optional<std::string> const type = internals.to_utf8(js::argument(args, 1));
        if (!text || !type)
            return std::nullopt;
        // The XML types are parsed by the HTML parser too: there is no XML
        // parser yet, and a tree is more use than a throw.
        dom::Document& d = new_extra_document(internals);
        html::parse_document_into(d, decode_utf8(*text));
        return js::Value::object(internals.wrap(d));
    });
}

}
