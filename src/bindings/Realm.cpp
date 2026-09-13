#include "bindings/Internal.h"

#include "bindings/Fetching.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "html/Serializer.h"
#include "js/Module.h"
#include "js/Strings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace sashfold::bindings {

// --- Host object tracing ------------------------------------------------------

void EventTargetObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    for (ListenerEntry const& entry : listeners)
        tracer.visit(entry.listener.callback);
    for (auto const& [type, handler] : handlers)
        tracer.visit(handler.function);
}

NodeWrapper::NodeWrapper(js::Object* prototype, js::RealmRecord& record, dom::Node& node)
    : EventTargetObject(prototype)
    , m_record(&record)
    , m_node(&node)
{
}

NodeWrapper::~NodeWrapper()
{
    // The node outlives every wrapper (the realm goes before its documents),
    // so the slot can be cleared for the next script that reaches the node.
    if (m_node && m_node->wrapper == this)
        m_node->wrapper = nullptr;
}

js::Object* NodeWrapper::same_object(std::string_view attribute) const
{
    for (auto const& [name, object] : m_same_objects) {
        if (name == attribute)
            return object;
    }
    return nullptr;
}

void NodeWrapper::keep_same_object(std::string_view attribute, js::Object* object)
{
    m_same_objects.emplace_back(std::string(attribute), object);
}

void NodeWrapper::trace(js::Tracer& tracer)
{
    EventTargetObject::trace(tracer);
    tracer.visit(m_record);
    for (auto const& kept : m_same_objects)
        tracer.visit(kept.second);
    if (!m_node)
        return;
    // A detached subtree lives as long as a wrapper into it is reachable
    // (ADR 0001 §3): reaching any node of it keeps its root's wrapper, and a
    // document a script made keeps that document's wrapper. The connected
    // tree is marked by the realm as one root.
    dom::Node& root = m_node->root();
    if (&root != &realm().document() && root.wrapper && root.wrapper != this)
        tracer.visit(root.wrapper);
}

void EventObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(target);
    tracer.visit(current_target);
    tracer.visit(related_target);
    tracer.visit(detail_value);
    tracer.visit(source_value);
    tracer.visit(ports);
}

void ElementBackedObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(wrapper);
}

Realm::Internals& TokenListObject::internals() const { return wrapper->realm().internals(); }
Realm::Internals& DatasetObject::internals() const { return wrapper->realm().internals(); }

Realm::Internals& StyleDeclarationObject::internals() const
{
    return wrapper ? wrapper->realm().internals() : static_cast<Realm*>(record->host_defined)->internals();
}

void StyleDeclarationObject::trace(js::Tracer& tracer)
{
    ElementBackedObject::trace(tracer);
    tracer.visit(record);
}

void UrlObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(search_params);
}

void SearchParamsObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(owner);
}

// --- Small helpers --------------------------------------------------------------

std::string ascii_lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string ascii_upper(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

static bool is_html_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

std::vector<std::string> split_tokens(std::string_view text)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_html_space(text[i]))
            ++i;
        std::size_t const start = i;
        while (i < text.size() && !is_html_space(text[i]))
            ++i;
        if (i > start)
            tokens.emplace_back(text.substr(start, i - start));
    }
    return tokens;
}

std::string join_tokens(std::vector<std::string> const& tokens)
{
    std::string out;
    for (std::string const& token : tokens) {
        if (!out.empty())
            out += ' ';
        out += token;
    }
    return out;
}

std::uint32_t to_unsigned_long(double number)
{
    if (std::isnan(number) || std::isinf(number))
        return 0;
    double integer = std::fmod(std::trunc(number), 4294967296.0);
    if (integer < 0)
        integer += 4294967296.0;
    return static_cast<std::uint32_t>(integer);
}

std::string attribute_or_empty(dom::Element const& element, std::string_view name)
{
    dom::Attr const* attribute = element.find_attribute(name);
    return attribute ? attribute->value : std::string();
}

namespace {

// An attribute a script wrote is a mutation; an iframe's src or srcdoc, in no
// namespace, navigates its frame (HTML §4.8.5, "process the iframe
// attributes"), though not to what it already shows from those very
// attributes (see navigate_frame); a src beside an srcdoc names nothing.
void attribute_written(Realm::Internals& in, dom::Element& element, std::string_view namespace_uri, std::string_view local_name)
{
    in.realm.note_mutation();
    if (!namespace_uri.empty() || !element.is_html("iframe"))
        return;
    if (local_name == "srcdoc" || (local_name == "src" && !element.find_attribute("srcdoc")))
        in.schedule_frame_navigation(element, FrameNavigation {});
}

// Erases an attribute, then reports it written by its namespace and local
// name, copied first: the erase moves the attributes after it.
void erase_attribute(Realm::Internals& in, dom::Element& element, std::vector<dom::Attr>::iterator it)
{
    std::string const namespace_uri = it->namespace_uri;
    std::string const local_name = it->local_name;
    element.attributes().erase(it);
    attribute_written(in, element, namespace_uri, local_name);
}

} // namespace

void set_attribute(Realm::Internals& in, dom::Element& element, std::string_view name, std::string value)
{
    for (dom::Attr& attribute : element.attributes()) {
        if (attribute.has_qualified_name(name)) {
            attribute.value = std::move(value);
            attribute.from_cssom = false; // set by a script as text: inline style again
            attribute_written(in, element, attribute.namespace_uri, attribute.local_name);
            return;
        }
    }
    element.attributes().push_back(dom::Attr { std::string(name), std::move(value), "", "" });
    attribute_written(in, element, "", element.attributes().back().local_name);
}

bool remove_attribute(Realm::Internals& in, dom::Element& element, std::string_view name)
{
    auto& attributes = element.attributes();
    auto const it = std::find_if(attributes.begin(), attributes.end(),
        [name](dom::Attr const& attribute) { return attribute.has_qualified_name(name); });
    if (it == attributes.end())
        return false;
    erase_attribute(in, element, it);
    return true;
}

void set_attribute_ns(Realm::Internals& in, dom::Element& element, std::string_view namespace_uri, std::string_view prefix,
    std::string_view local_name, std::string value)
{
    for (dom::Attr& attribute : element.attributes()) {
        if (attribute.namespace_uri == namespace_uri && attribute.local_name == local_name) {
            attribute.value = std::move(value);
            attribute.from_cssom = false;
            attribute_written(in, element, attribute.namespace_uri, attribute.local_name);
            return;
        }
    }
    element.attributes().push_back(dom::Attr { std::string(local_name), std::move(value), std::string(prefix), std::string(namespace_uri) });
    dom::Attr const& added = element.attributes().back();
    attribute_written(in, element, added.namespace_uri, added.local_name);
}

bool remove_attribute_ns(Realm::Internals& in, dom::Element& element, std::string_view namespace_uri, std::string_view local_name)
{
    auto& attributes = element.attributes();
    auto const it = std::find_if(attributes.begin(), attributes.end(), [namespace_uri, local_name](dom::Attr const& attribute) {
        return attribute.namespace_uri == namespace_uri && attribute.local_name == local_name;
    });
    if (it == attributes.end())
        return false;
    erase_attribute(in, element, it);
    return true;
}

std::string frame_source(dom::Element const& iframe, net::Url const& base)
{
    if (dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc"))
        return "srcdoc:" + srcdoc->value;
    dom::Attr const* const src = iframe.find_attribute("src");
    if (!src || src->value.empty())
        return "";
    std::optional<net::Url> const url = net::parse_url(src->value, &base);
    if (!url || url->scheme == "about")
        return "";
    return "src:" + url->serialize();
}

std::uint32_t parse_sandboxing_directive(std::string_view text)
{
    std::vector<std::string> tokens;
    for (std::string const& token : split_tokens(text))
        tokens.push_back(ascii_lower(token));
    auto const has = [&tokens](std::string_view token) { return std::find(tokens.begin(), tokens.end(), token) != tokens.end(); };
    std::uint32_t flags = sandboxing::navigation | sandboxing::document_domain;
    if (!has("allow-popups"))
        flags |= sandboxing::auxiliary_navigation;
    if (!has("allow-top-navigation"))
        flags |= sandboxing::top_navigation;
    if (!has("allow-top-navigation-by-user-activation") && !has("allow-top-navigation"))
        flags |= sandboxing::top_navigation_by_user_activation;
    if (!has("allow-same-origin"))
        flags |= sandboxing::origin;
    if (!has("allow-forms"))
        flags |= sandboxing::forms;
    if (!has("allow-pointer-lock"))
        flags |= sandboxing::pointer_lock;
    if (!has("allow-scripts"))
        flags |= sandboxing::scripts | sandboxing::automatic_features;
    if (!has("allow-popups-to-escape-sandbox"))
        flags |= sandboxing::propagates_to_auxiliary;
    if (!has("allow-modals"))
        flags |= sandboxing::modals;
    if (!has("allow-orientation-lock"))
        flags |= sandboxing::orientation_lock;
    if (!has("allow-presentation"))
        flags |= sandboxing::presentation;
    if (!has("allow-downloads"))
        flags |= sandboxing::downloads;
    if (!has("allow-top-navigation-to-custom-protocols") && !has("allow-popups") && !has("allow-top-navigation"))
        flags |= sandboxing::custom_protocols_navigation;
    return flags;
}

std::optional<dom::Node*> this_node(js::Interpreter& interpreter, js::Value const& this_value)
{
    NodeWrapper* wrapper = internals_of(interpreter).wrapper_of(this_value);
    if (!wrapper)
        return interpreter.throw_type_error("Illegal invocation");
    return &wrapper->node();
}

std::optional<dom::Element*> this_element(js::Interpreter& interpreter, js::Value const& this_value)
{
    std::optional<dom::Node*> const node = this_node(interpreter, this_value);
    if (!node)
        return std::nullopt;
    if (!(*node)->is_element())
        return interpreter.throw_type_error("Illegal invocation");
    return static_cast<dom::Element*>(*node);
}

std::optional<dom::Document*> this_document(js::Interpreter& interpreter, js::Value const& this_value)
{
    std::optional<dom::Node*> const node = this_node(interpreter, this_value);
    if (!node)
        return std::nullopt;
    if ((*node)->type() != dom::NodeType::Document)
        return interpreter.throw_type_error("Illegal invocation");
    return static_cast<dom::Document*>(*node);
}

js::Value node_list(Realm::Internals& in, std::vector<dom::Node*> const& nodes)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    js::ArrayObject* list = interpreter.new_array();
    interpreter.root(js::Value::object(list));
    list->set_prototype(in.prototype("NodeList"));
    for (dom::Node* node : nodes)
        list->push(js::Value::object(in.wrap(*node)));
    return js::Value::object(list);
}

js::Object* define_interface(Realm::Internals& in, std::string_view name, js::Object* parent_prototype,
    js::NativeFunction::ConstructCallback construct, int length)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* prototype = interpreter.new_object(parent_prototype);
    std::string const interface_name(name);
    if (!construct) {
        construct = [interface_name](js::Interpreter& interp, Args, js::Object*) -> Native {
            return interp.throw_type_error("Illegal constructor");
        };
    }
    js::NativeFunction* constructor = interpreter.new_native(name, length,
        [interface_name](js::Interpreter& interp, js::Value const&, Args) -> Native {
            return interp.throw_type_error("Failed to construct '" + interface_name + "': Please use the 'new' operator");
        },
        std::move(construct));
    constructor->put(interpreter.key("prototype"), js::Value::object(prototype), js::frozen_attributes);
    prototype->put(interpreter.key("constructor"), js::Value::object(constructor), js::builtin_attributes);
    prototype->put(js::PropertyKey::symbol(interpreter.atoms().symbol_to_string_tag), in.string(name), js::Configurable);
    interpreter.global()->put(interpreter.key(name), js::Value::object(constructor), js::builtin_attributes);
    in.prototypes[interface_name] = prototype;
    return prototype;
}

void define_getter(Realm::Internals& in, js::Object& prototype, std::string_view name, js::NativeFunction::Callback getter,
    js::NativeFunction::Callback setter)
{
    js::define_accessor(in.interpreter, prototype, name, std::move(getter), std::move(setter));
}

void reflect_string(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            return internals_of(interp).string(attribute_or_empty(**element, attribute_name));
        },
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            std::optional<std::string> value = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            set_attribute(internals_of(interp), **element, attribute_name, std::move(*value));
            return js::Value::undefined();
        });
}

void reflect_boolean(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            return js::Value::boolean((*element)->has_attribute(attribute_name));
        },
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            if (js::Interpreter::to_boolean(js::argument(args, 0)))
                set_attribute(internals_of(interp), **element, attribute_name, "");
            else
                remove_attribute(internals_of(interp), **element, attribute_name);
            return js::Value::undefined();
        });
}

void reflect_url(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            Realm::Internals& internals = internals_of(interp);
            dom::Attr const* value = (*element)->find_attribute(attribute_name);
            if (!value)
                return internals.string("");
            if (std::optional<net::Url> const resolved = net::parse_url(value->value, &internals.url))
                return internals.string(resolved->serialize());
            return internals.string(value->value);
        },
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            std::optional<std::string> value = internals_of(interp).to_utf8(js::argument(args, 0));
            if (!value)
                return std::nullopt;
            set_attribute(internals_of(interp), **element, attribute_name, std::move(*value));
            return js::Value::undefined();
        });
}

void reflect_long(Realm::Internals& in, js::Object& prototype, std::string_view property, std::string_view attribute, int fallback)
{
    std::string const attribute_name(attribute);
    define_getter(in, prototype, property,
        [attribute_name, fallback](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            dom::Attr const* value = (*element)->find_attribute(attribute_name);
            if (!value)
                return js::Value::number(fallback);
            char* end = nullptr;
            long const parsed = std::strtol(value->value.c_str(), &end, 10);
            if (end == value->value.c_str())
                return js::Value::number(fallback);
            return js::Value::number(static_cast<double>(parsed));
        },
        [attribute_name](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<dom::Element*> const element = this_element(interp, this_value);
            if (!element)
                return std::nullopt;
            std::optional<double> const number = interp.to_number(js::argument(args, 0));
            if (!number)
                return std::nullopt;
            double const integer = js::Interpreter::to_integer_or_infinity(*number);
            set_attribute(internals_of(interp), **element, attribute_name, js::number_to_utf8(integer));
            return js::Value::undefined();
        });
}

// --- The internals ----------------------------------------------------------------

Realm::Internals::Internals(Realm& the_realm, dom::Document& the_document, net::Url the_url, HostHooks the_hooks)
    : realm(the_realm)
    , document(the_document)
    , url(std::move(the_url))
    , hooks(std::move(the_hooks))
    , own_agent(std::make_unique<Agent>())
    , agent(*own_agent)
    , interpreter(agent.interpreter)
    , realm_record(interpreter.current_realm())
    , origin_url(url)
{
}

Realm::Internals::Internals(Realm& the_realm, Agent& the_agent, dom::Document& the_document, net::Url the_url, HostHooks the_hooks)
    : realm(the_realm)
    , document(the_document)
    , url(std::move(the_url))
    , hooks(std::move(the_hooks))
    , agent(the_agent)
    , interpreter(the_agent.interpreter)
    , realm_record(the_agent.interpreter.create_realm())
    , origin_url(url)
{
}

js::Object* Realm::Internals::prototype(std::string_view name) const
{
    auto const it = prototypes.find(std::string(name));
    return it == prototypes.end() ? nullptr : it->second;
}

double Realm::Internals::now() const
{
    if (hooks.now)
        return hooks.now();
    using namespace std::chrono;
    return static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

void Realm::Internals::console(std::string_view level, std::string_view message) const
{
    if (hooks.console)
        hooks.console(level, message);
}

void Realm::Internals::trace(std::string_view message) const
{
    if (hooks.trace)
        hooks.trace(message);
}

// --- The document's Content Security Policy ----------------------------------------------

void adopt_meta_policies(net::ContentSecurityPolicy& policy, dom::Document const& document)
{
    // HTML §4.2.5.3: a <meta http-equiv=content-security-policy> that is a
    // child of the head, with a content attribute, enforces its policy.
    for (dom::Node const* child : document.children()) {
        if (!child->is_element() || !static_cast<dom::Element const*>(child)->is_html("html"))
            continue;
        for (dom::Node const* html_child : child->children()) {
            if (!html_child->is_element() || !static_cast<dom::Element const*>(html_child)->is_html("head"))
                continue;
            for (dom::Node const* head_child : html_child->children()) {
                if (!head_child->is_element())
                    continue;
                auto const& meta = *static_cast<dom::Element const*>(head_child);
                if (!meta.is_html("meta"))
                    continue;
                dom::Attr const* const equiv = meta.find_attribute("http-equiv");
                dom::Attr const* const content = meta.find_attribute("content");
                if (equiv && content && ascii_ci_equals(equiv->value, "content-security-policy"))
                    policy.add_meta(content->value);
            }
        }
    }
}

void Realm::Internals::adopt_meta_policies()
{
    if (hooks.policy)
        bindings::adopt_meta_policies(*hooks.policy, document);
}

net::RequestGuard Realm::Internals::request_guard(net::ResourceKind kind, std::string nonce, bool parser_inserted)
{
    if (!hooks.policy)
        return {};
    adopt_meta_policies();
    return hooks.policy->guard(kind, std::move(nonce), parser_inserted);
}

bool Realm::Internals::inline_refused(net::InlineKind kind, std::string_view nonce, std::string_view source)
{
    if (!hooks.policy)
        return false;
    adopt_meta_policies();
    return hooks.policy->inline_refusal(kind, nonce, source).has_value();
}

std::optional<std::string> Realm::Internals::compile_strings_refusal()
{
    if (!hooks.policy)
        return std::nullopt;
    adopt_meta_policies();
    return hooks.policy->eval_refusal();
}

bool Realm::Internals::scripts_sandboxed() const
{
    return (sandbox_flags & sandboxing::scripts) != 0 || (hooks.policy != nullptr && !hooks.policy->sandbox_allows_scripts());
}

net::Url const& Realm::Internals::base_url() const
{
    if (parent_realm != nullptr && url.scheme == "about") {
        std::string const address = url.serialize(true);
        if (address == "about:srcdoc" || address == "about:blank")
            return parent_realm->base_url();
    }
    return url;
}

void Realm::Internals::report_uncaught(js::Value const& thrown, std::string_view where)
{
    ++stats.uncaught_errors;
    std::string const description = interpreter.describe(thrown);
    // window.onerror(message, source, lineno, colno, error), when a page set
    // one; its listeners on "error" are not fired (that needs ErrorEvent).
    auto const handler = window_handlers.find("error");
    if (handler != window_handlers.end() && js::Interpreter::is_callable(handler->second.function)) {
        js::Interpreter::Roots const roots(interpreter);
        js::Value const callee = interpreter.root(handler->second.function);
        interpreter.root(thrown);
        js::Value const message = interpreter.root(string("Uncaught " + description));
        js::Value const source = interpreter.root(string(where));
        js::Value const arguments[5] = { message, source, js::Value::number(0), js::Value::number(0), thrown };
        std::optional<js::Value> const result = interpreter.call(callee, js::Value::object(window_proxy()), arguments);
        if (!result) {
            js::Value const inner = interpreter.take_exception();
            console("error", "Uncaught " + interpreter.describe(inner) + " (in window.onerror)");
            return;
        }
        if (result->is_boolean() && result->as_boolean())
            return; // the handler says it dealt with it
    }
    console("error", "Uncaught " + description + (where.empty() ? std::string() : " (" + std::string(where) + ")"));
}

Realm::Internals::Entry::Entry(Internals& the_internals)
    : host_entry(the_internals.agent)
    , internals(the_internals)
    , started(the_internals.now())
    , realm_scope(the_internals.interpreter, the_internals.realm_record)
{
    ++internals.agent.script_depth;
    // Time is measured on the steady clock even when timers run on a
    // virtual one: this is a cost, not a schedule.
    using namespace std::chrono;
    started = static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

Realm::Internals::Entry::~Entry()
{
    using namespace std::chrono;
    double const finished = static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
    internals.stats.script_ms += finished - started;
    if (--internals.agent.script_depth == 0)
        internals.realm.perform_microtask_checkpoint();
}

void Realm::Internals::call_reporting(js::Value const& callee, js::Value const& this_value, Args arguments, std::string_view where)
{
    Entry const entry(*this);
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(callee);
    interpreter.root(this_value);
    for (js::Value const& argument : arguments)
        interpreter.root(argument);
    js::Outcome const outcome = interpreter.call_outcome(callee, this_value, arguments);
    if (!outcome.ok) {
        if (interpreter.terminated())
            return; // the host stopped the script; nothing to report but the count
        report_uncaught(outcome.value, where);
    }
}

std::optional<std::string> Realm::Internals::to_utf8(js::Value const& value)
{
    std::optional<js::JsString*> const text = interpreter.to_string(value);
    if (!text)
        return std::nullopt;
    return (*text)->to_utf8();
}

Native Realm::Internals::throw_dom_exception(std::string_view name, std::string_view message)
{
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* error = interpreter.new_error(js::ErrorType::Error, message);
    if (js::Object* proto = prototype("DOMException"))
        error->set_prototype(proto);
    error->put(interpreter.key("name"), string(name), js::builtin_attributes);
    static constexpr std::pair<std::string_view, int> codes[] = {
        { "IndexSizeError", 1 }, { "HierarchyRequestError", 3 }, { "WrongDocumentError", 4 },
        { "InvalidCharacterError", 5 }, { "NoModificationAllowedError", 7 }, { "NotFoundError", 8 },
        { "NotSupportedError", 9 }, { "InvalidStateError", 11 }, { "SyntaxError", 12 },
        { "InvalidModificationError", 13 }, { "NamespaceError", 14 }, { "InvalidAccessError", 15 },
        { "TypeMismatchError", 17 }, { "SecurityError", 18 }, { "NetworkError", 19 }, { "AbortError", 20 },
        { "URLMismatchError", 21 }, { "QuotaExceededError", 22 }, { "TimeoutError", 23 },
        { "InvalidNodeTypeError", 24 }, { "DataCloneError", 25 }
    };
    int code = 0;
    for (auto const& [candidate, number] : codes) {
        if (candidate == name)
            code = number;
    }
    error->put(interpreter.key("code"), js::Value::number(code), js::builtin_attributes);
    return interpreter.throw_value(js::Value::object(error));
}

// --- Wrappers -----------------------------------------------------------------------

js::Object* Realm::Internals::prototype_for(dom::Node const& node) const
{
    switch (node.type()) {
    case dom::NodeType::Document:
        return prototype("Document");
    case dom::NodeType::DocumentFragment:
        return prototype("DocumentFragment");
    case dom::NodeType::Text:
        return prototype("Text");
    case dom::NodeType::Comment:
        return prototype("Comment");
    case dom::NodeType::DocumentType:
        return prototype("DocumentType");
    case dom::NodeType::Element: {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html()) {
            auto const it = tag_interfaces.find(element.local_name());
            if (it != tag_interfaces.end()) {
                if (js::Object* proto = prototype(it->second))
                    return proto;
            }
            return prototype("HTMLElement");
        }
        if (element.namespace_uri() == dom::ns::svg)
            return prototype(element.local_name() == "a" ? "SVGAElement" : "SVGElement");
        return prototype("Element");
    }
    }
    return prototype("Node");
}

js::Object* Realm::Internals::wrap(dom::Node& node)
{
    if (node.wrapper)
        return node.wrapper;
    js::Object* proto = prototype_for(node);
    NodeWrapper* wrapper = interpreter.heap().allocate<NodeWrapper>(proto, *realm_record, node);
    node.wrapper = wrapper;
    return wrapper;
}

NodeWrapper& wrapper_for(Realm::Internals& in, dom::Node& node)
{
    return *static_cast<NodeWrapper*>(in.wrap(node));
}

NodeWrapper* Realm::Internals::wrapper_of(js::Value const& value) const
{
    if (!value.is_object() || !value.as_object()->is_host())
        return nullptr;
    // Every host object is a js::Object subclass of ours; the node wrappers
    // are told apart by the cached slot pointing back at them.
    auto* wrapper = dynamic_cast<NodeWrapper*>(value.as_object());
    // A wrapper another document's realm made in this agent is accepted — a
    // frame's node reached or adopted by its page — but never a detached one.
    if (!wrapper || wrapper->detached() || &wrapper->realm().internals().agent != &agent)
        return nullptr;
    return wrapper;
}

// --- Scripts -------------------------------------------------------------------------

namespace {

bool is_javascript_type(std::string_view type)
{
    static constexpr std::string_view types[] = {
        "application/ecmascript", "application/javascript", "application/x-ecmascript", "application/x-javascript",
        "text/ecmascript", "text/javascript", "text/javascript1.0", "text/javascript1.1", "text/javascript1.2",
        "text/javascript1.3", "text/javascript1.4", "text/javascript1.5", "text/jscript", "text/livescript",
        "text/x-ecmascript", "text/x-javascript"
    };
    for (std::string_view const candidate : types) {
        if (candidate == type)
            return true;
    }
    return false;
}

std::string trimmed(std::string_view text)
{
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && is_html_space(text[start]))
        ++start;
    while (end > start && is_html_space(text[end - 1]))
        --end;
    return std::string(text.substr(start, end - start));
}

} // namespace

void Realm::Internals::prepare_script(dom::Element& script, bool from_parser)
{
    // §4.12.1.1, the parts a classic script needs.
    if (started_scripts.contains(&script))
        return;
    started_scripts.insert(&script);
    // Step 3 of the sandboxing: a document sandboxed without allow-scripts
    // runs none.
    if (scripts_sandboxed()) {
        ++stats.scripts_refused;
        return;
    }
    std::string type = ascii_lower(trimmed(attribute_or_empty(script, "type")));
    if (type.empty()) {
        std::string const language = ascii_lower(trimmed(attribute_or_empty(script, "language")));
        if (!language.empty() && language != "javascript" && language != "jscript" && language != "ecmascript"
            && !language.starts_with("javascript1.")) {
            ++stats.scripts_skipped;
            return;
        }
        type = "text/javascript";
    }
    if (type == "module") {
        prepare_module_script(script, from_parser);
        return;
    }
    if (!is_javascript_type(type)) {
        ++stats.scripts_skipped; // a data block: JSON, a template, an import map
        return;
    }
    std::string source;
    std::string name;
    std::string const nonce = attribute_or_empty(script, "nonce");
    if (dom::Attr const* src = script.find_attribute("src")) {
        std::optional<net::Url> const resolved = src->value.empty() ? std::nullopt : net::parse_url(src->value, &url);
        std::optional<std::string> fetched;
        if (resolved && hooks.fetch_script)
            fetched = hooks.fetch_script(*resolved, request_guard(net::ResourceKind::Script, nonce, from_parser));
        if (!fetched) {
            ++stats.external_failed;
            console("error", "script " + (resolved ? resolved->serialize() : src->value) + " could not be loaded");
            realm.dispatch_event(&script, "error");
            return;
        }
        ++stats.external_fetched;
        source = std::move(*fetched);
        // A byte-order mark is not source text.
        if (source.starts_with("\xEF\xBB\xBF"))
            source.erase(0, 3);
        name = resolved->serialize();
        bool const deferred = from_parser && (script.has_attribute("defer") || script.has_attribute("async"));
        if (deferred) {
            deferred_scripts.push_back(PendingScript { &script, std::move(source), std::move(name) });
            return;
        }
    } else {
        source = html::text_content(script);
        name = url.serialize() + " (inline)";
        // §4.12.1.1 step 20: the inline check, the text as written.
        if (inline_refused(net::InlineKind::Script, nonce, source)) {
            ++stats.scripts_refused;
            return;
        }
    }
    execute_script(script, source, name);
}

void Realm::Internals::execute_script(dom::Element& script, std::string const& source, std::string const& name)
{
    dom::Element* const previous = current_script;
    current_script = &script;
    ++stats.scripts_run;
    js::Outcome const outcome = realm.run(source, name);
    if (!outcome.ok)
        ++stats.scripts_failed;
    current_script = previous;
    if (script.has_attribute("src") && !interpreter.terminated())
        realm.dispatch_event(&script, "load");
}

// --- Module scripts (§4.12.1.1 for type=module; §8.1.7 for the fetch and the run) ----------

// The module map is keyed by URL. An inline module has no URL of its own,
// so its key is made unique per element, while its base — for a relative
// specifier, and for import.meta.url — stays the document's URL.
std::string Realm::Internals::inline_module_key()
{
    return url.serialize() + " (inline module " + std::to_string(++inline_modules) + ")";
}

net::Url Realm::Internals::module_base_of(std::string_view referrer_key) const
{
    if (auto const inline_base = inline_module_bases.find(std::string(referrer_key)); inline_base != inline_module_bases.end())
        return inline_base->second;
    if (std::optional<net::Url> const parsed = net::parse_url(referrer_key, nullptr))
        return *parsed;
    // A classic script's name ("…/page.html (inline)", "<test>"): the document's.
    return url;
}

void Realm::Internals::install_module_hooks()
{
    interpreter.set_module_hooks(
        // §8.1.7.1.3 "resolve a module specifier", without import maps: a URL,
        // or a relative reference that begins with "/", "./" or "../"
        // against the referrer's base. A bare specifier is what an import
        // map would settle, and import maps are not written.
        [&agent_interpreter = interpreter](std::string_view referrer_key, std::string_view specifier, std::string& error) -> std::optional<std::string> {
            std::optional<net::Url> resolved = net::parse_url(specifier, nullptr);
            if (!resolved && (specifier.starts_with("/") || specifier.starts_with("./") || specifier.starts_with("../"))) {
                net::Url const base = internals_of(agent_interpreter).module_base_of(referrer_key);
                resolved = net::parse_url(specifier, &base);
            }
            if (!resolved) {
                error = "Failed to resolve module specifier '" + std::string(specifier)
                    + "': a relative reference must start with \"/\", \"./\" or \"../\"";
                return std::nullopt;
            }
            return resolved->serialize();
        },
        [&agent_interpreter = interpreter](std::string_view key, std::string& error) -> std::optional<std::u16string> {
            // The realm whose module graph asks is the one running.
            Internals& asking = internals_of(agent_interpreter);
            std::optional<std::string> const source = asking.fetch_module_source(std::string(key), asking.module_credentials_include, error);
            if (!source)
                return std::nullopt;
            return js::utf16_from_utf8(*source);
        });
}

// §8.1.7.2.7 "fetch a single module script": a GET in CORS mode with the
// credentials the root element asked for, whose response is a JavaScript
// type; anything else is a network error for the whole graph.
std::optional<std::string> Realm::Internals::fetch_module_source(std::string const& key, bool include_credentials, std::string& error)
{
    std::optional<net::Url> const target = net::parse_url(key, nullptr);
    if (!target) {
        error = "not a URL";
        return std::nullopt;
    }
    PageRequest request;
    request.url = *target;
    request.mode = FetchMode::Cors;
    request.credentials = include_credentials ? FetchCredentials::Include : FetchCredentials::SameOrigin;
    request.destination = "script";
    request.nonce = module_nonce;
    request.parser_inserted = module_parser_inserted;
    FetchOutcome const outcome = perform_fetch(*this, request);
    if (!outcome.ok) {
        error = outcome.error.empty() ? std::string("it could not be fetched") : outcome.error;
        return std::nullopt;
    }
    if (outcome.status < 200 || outcome.status > 299) {
        error = "the server answered " + std::to_string(outcome.status);
        return std::nullopt;
    }
    std::string const* const content_type = net::find_header(outcome.headers, "content-type");
    std::string const essence = content_type != nullptr ? mime_essence(*content_type) : std::string();
    if (!is_javascript_type(essence)) {
        error = "its type is " + (essence.empty() ? std::string("unknown") : essence) + ", not a JavaScript type";
        return std::nullopt;
    }
    std::string source(outcome.body.begin(), outcome.body.end());
    if (source.starts_with("\xEF\xBB\xBF"))
        source.erase(0, 3);
    ++stats.external_fetched;
    return source;
}

// The module half of §4.12.1.1: the graph is fetched, parsed and loaded
// now, in the element's credentials mode; a failure anywhere in it is the
// element's error event and a console line. A parser-inserted module
// without `async` then waits for the parse, in document order with the
// deferred classic scripts; an `async` one, or one a script inserted,
// runs as soon as its graph is loaded — with a synchronous loader, now.
void Realm::Internals::prepare_module_script(dom::Element& script, bool from_parser)
{
    dom::Attr const* const cross = script.find_attribute("crossorigin");
    module_credentials_include = cross != nullptr && ascii_lower(trimmed(cross->value)) == "use-credentials";
    module_nonce = attribute_or_empty(script, "nonce");
    module_parser_inserted = from_parser;
    js::ModuleRecord* record = nullptr;
    std::string name;
    std::string error;
    if (dom::Attr const* src = script.find_attribute("src")) {
        std::optional<net::Url> const resolved = src->value.empty() ? std::nullopt : net::parse_url(src->value, &url);
        if (!resolved) {
            error = "its src is not a URL";
            name = src->value;
        } else {
            name = resolved->serialize();
            record = interpreter.find_module(name);
            if (record == nullptr) {
                std::optional<std::string> const source = fetch_module_source(name, module_credentials_include, error);
                if (source) {
                    record = interpreter.parse_module(js::utf16_from_utf8(*source), name);
                    if (record == nullptr)
                        error = interpreter.describe(interpreter.take_exception());
                }
            }
        }
        if (record == nullptr)
            ++stats.external_failed;
    } else {
        std::string const text = html::text_content(script);
        if (inline_refused(net::InlineKind::Script, module_nonce, text)) {
            ++stats.scripts_refused;
            return;
        }
        name = inline_module_key();
        inline_module_bases.emplace(name, url);
        record = interpreter.parse_module(js::utf16_from_utf8(text), name);
        if (record == nullptr)
            error = interpreter.describe(interpreter.take_exception());
    }
    if (record != nullptr && !interpreter.load_module(*record)) {
        error = interpreter.describe(interpreter.take_exception());
        record = nullptr;
    }
    if (record == nullptr) {
        console("error", "module script " + name + " could not be loaded: " + error);
        realm.dispatch_event(&script, "error");
        return;
    }
    if (from_parser && !script.has_attribute("async")) {
        deferred_scripts.push_back(PendingScript { &script, std::string(), std::move(name), record });
        return;
    }
    execute_module(script, *record, name);
}

// §8.1.7.3.2 "run a module script": linked if it never was, evaluated,
// its promise watched — a rejection is the module's uncaught error,
// reported once — and, for an external module, the load event, the way a
// classic script gets it. document.currentScript is null throughout.
void Realm::Internals::execute_module(dom::Element& script, js::ModuleRecord& record, std::string const& name)
{
    ++stats.scripts_run;
    ++stats.modules_run;
    dom::Element* const previous = current_script;
    current_script = nullptr;
    bool failed = false;
    {
        Entry const entry(*this);
        if (record.status() == js::ModuleRecord::Status::Unlinked && !interpreter.link_module(record)) {
            report_uncaught(interpreter.take_exception(), name);
            failed = true;
        } else if (std::optional<js::Value> const promise = interpreter.evaluate_module(record)) {
            watch_module_evaluation(*promise, name);
        } else {
            if (!interpreter.terminated())
                report_uncaught(interpreter.take_exception(), name);
            failed = true;
        }
    }
    if (failed)
        ++stats.scripts_failed;
    current_script = previous;
    if (script.has_attribute("src") && !interpreter.terminated())
        realm.dispatch_event(&script, "load");
}

void Realm::Internals::watch_module_evaluation(js::Value const& promise, std::string const& name)
{
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(promise);
    js::ClosureFunction* on_rejected = interpreter.new_closure("", 1, {},
        [this, name](js::Interpreter&, js::ClosureFunction&, js::Value const&, std::span<js::Value const> arguments) -> std::optional<js::Value> {
            ++stats.scripts_failed;
            report_uncaught(arguments.empty() ? js::Value::undefined() : arguments[0], name);
            return js::Value::undefined();
        });
    interpreter.root(js::Value::object(on_rejected));
    js::Value const arguments[2] = { js::Value::undefined(), js::Value::object(on_rejected) };
    if (!interpreter.invoke(promise, interpreter.key("then"), arguments))
        interpreter.clear_exception();
}

// --- The realm ------------------------------------------------------------------------

namespace {

// Every interface a document's realm has, made with that realm current.
void install_interfaces(Realm::Internals& in)
{
    // What the global object has before the Web's interfaces: the language's
    // own globals, which are no window's members.
    std::vector<js::PropertyKey> const language_globals = in.interpreter.global()->own_keys();
    install_events(in);
    install_nodes(in);
    install_style(in);
    install_window(in);
    install_binary(in);
    install_fetch(in);
    install_xhr(in);
    install_tasks(in);
    install_origin(in);
    install_window_proxy(in, language_globals);
}

// Lets every wrapper of a document's nodes go of its node, the document's own
// and those of every node it owns, in its tree or not: the realm ending before
// the heap they live in takes the document with it.
void detach_wrappers(dom::Document& document)
{
    auto const detach = [](dom::Node& node) {
        if (node.wrapper) {
            static_cast<NodeWrapper*>(node.wrapper)->detach();
            node.wrapper = nullptr;
        }
    };
    detach(document);
    for (std::unique_ptr<dom::Node> const& owned : document.owned_nodes())
        detach(*owned);
}

} // namespace

Realm::Realm(dom::Document& document, net::Url url, HostHooks hooks)
    : m_internals(std::make_unique<Internals>(*this, document, std::move(url), std::move(hooks)))
{
    Internals& in = *m_internals;
    js::Interpreter& interpreter = in.interpreter;
    interpreter.current_realm()->host_defined = this;
    interpreter.heap().add_root_provider(this);
    interpreter.on_console = [this](std::string_view level, std::string_view message) {
        m_internals->console(level, message);
    };
    // HostEnsureCanCompileStrings: the policy of the document whose realm is
    // running has its say on eval and Function, and on a string a timer
    // would compile.
    interpreter.on_compile_strings = [&interpreter]() { return internals_of(interpreter).compile_strings_refusal(); };
    // The module map is the document's: its keys are URLs, and the realm
    // resolves and fetches modules for the engine (§8.1.7).
    in.install_module_hooks();
    // HostGetImportMetaProperties: a module's `import.meta.url` is its own
    // URL, which is the key the module map named it by, except for an
    // inline module, whose URL is the document's.
    interpreter.set_module_meta_hook([](js::Interpreter& realm_interpreter, js::ModuleRecord& record, js::Object& meta) {
        Internals const& internals = internals_of(realm_interpreter);
        auto const inline_base = internals.inline_module_bases.find(record.key());
        std::string const url_text = inline_base != internals.inline_module_bases.end() ? inline_base->second.serialize() : record.key();
        meta.put(realm_interpreter.key("url"), js::Value::string(realm_interpreter.string(url_text)), js::default_attributes);
    });
    if (in.hooks.should_stop)
        interpreter.set_interrupt([this] { return m_internals->hooks.should_stop(); });
    in.time_origin = in.now();
    install_interfaces(in);
}

Realm::Realm(Internals& parent, dom::Element& container, dom::Document& document, net::Url url, HostHooks hooks)
    : m_internals(std::make_unique<Internals>(*this, parent.agent, document, std::move(url), std::move(hooks)))
{
    // The agent's own hooks stay its page's; this is one more realm in it.
    Internals& in = *m_internals;
    in.parent_realm = &parent;
    in.frame_element = &container;
    in.realm_record->host_defined = this;
    in.interpreter.heap().add_root_provider(this);
    in.time_origin = in.now();
    // Its interfaces are made of its own intrinsics.
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    install_interfaces(in);
}

void Realm::Internals::post_task(std::function<void()> task)
{
    agent.tasks.push_back(Task { agent.next_sequence++, this, std::move(task) });
}

Realm::Realm(StandIn, Agent& agent, dom::Document& document)
    : m_internals(std::make_unique<Internals>(*this, agent, document, *net::parse_url("about:blank"), HostHooks {}))
{
    Internals& in = *m_internals;
    in.ended = true;
    in.realm_record->host_defined = this;
    in.interpreter.heap().add_root_provider(this);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    install_interfaces(in);
}

namespace {

// The agent's stand-in, made the first time a realm ends while the page lives.
Realm* stand_in_of(Agent& agent)
{
    if (!agent.stand_in) {
        agent.stand_in_document = std::make_unique<dom::Document>();
        agent.stand_in = std::make_unique<Realm>(Realm::StandIn {}, agent, *agent.stand_in_document);
    }
    return agent.stand_in.get();
}

// Ends the frames closed while the host was inside the agent's realms: the
// last entry out calls this, when nothing of theirs is on the stack.
void end_closing(Agent& agent)
{
    while (!agent.closing.empty()) {
        ChildFrame frame = std::move(agent.closing.back());
        agent.closing.pop_back();
        frame.realm.reset(); // then its document and policy, with `frame`
    }
}

// The timers and tasks of a realm and of its frames' realms, erased.
void erase_loop_work(Realm::Internals& in)
{
    std::erase_if(in.agent.timers, [&in](Timer const& timer) { return timer.owner == &in; });
    std::erase_if(in.agent.tasks, [&in](Task const& task) { return task.owner == &in; });
    for (ChildFrame const& listed : in.child_frames)
        erase_loop_work(listed.realm->internals());
}

// The windows of a document whose navigable is destroyed, and of every
// document in its frames: a child navigable is destroyed with the document
// it is in (HTML §7.3.1, destroying a document), so each of them is closed,
// with no parent, top or name, from now on.
void discard_windows(Realm::Internals& in)
{
    in.discarded = true;
    for (ChildFrame const& listed : in.child_frames)
        discard_windows(listed.realm->internals());
}

} // namespace

Realm::Internals::HostEntry::HostEntry(Agent& the_agent)
    : agent(the_agent)
{
    ++agent.host_depth;
}

Realm::Internals::HostEntry::~HostEntry()
{
    if (--agent.host_depth == 0 && !agent.closing.empty())
        end_closing(agent);
}

Realm::~Realm()
{
    // The wrappers go with the heap, before the documents they point into:
    // the agent and its interpreter are destroyed here, the extra documents
    // after them (member order), the page's document by whoever owns it,
    // later. What this realm left on the agent's event loop goes first.
    Internals& in = *m_internals;
    if (in.own_agent)
        in.agent.ending = true;
    std::erase_if(in.agent.timers, [&in](Timer const& timer) { return timer.owner == &in; });
    std::erase_if(in.agent.tasks, [&in](Task const& task) { return task.owner == &in; });
    // Its frames end first, while this realm and the agent are whole.
    in.child_frames.clear();
    if (in.own_agent) {
        in.agent.closing.clear();
        in.agent.stand_in.reset();
        in.agent.stand_in_document.reset();
        in.interpreter.clear_jobs();
    } else {
        // A frame's realm ends before the heap its wrappers live in: they let
        // go of the nodes they point into. Its record passes to the agent's
        // stand-in, so a native of this realm that a script still holds
        // answers from an empty document rather than from freed memory.
        detach_wrappers(in.document);
        for (std::unique_ptr<dom::Document> const& extra : in.extra_documents)
            detach_wrappers(*extra);
        // What a script still holds of this window is judged by the origin
        // it had, not by the stand-in's.
        if (!in.agent.ending && !in.ended)
            in.agent.ended_origins[in.realm_record] = OriginSnapshot { in.origin_url.serialize_origin(), in.origin_url.scheme, in.domain.get() };
        in.realm_record->host_defined = in.agent.ending || in.ended ? nullptr : stand_in_of(in.agent);
        in.interpreter.release_realm(in.realm_record);
    }
    in.interpreter.heap().remove_root_provider(this);
}

js::Interpreter& Realm::interpreter() { return m_internals->interpreter; }
dom::Document& Realm::document() { return m_internals->document; }
net::Url const& Realm::url() const { return m_internals->url; }
HostHooks& Realm::hooks() { return m_internals->hooks; }
js::Object* Realm::wrap(dom::Node& node) { return m_internals->wrap(node); }
js::Object* Realm::window() const { return m_internals->realm_record->intrinsics.global; }
std::string const& Realm::ready_state() const { return m_internals->ready_state; }
std::uint64_t Realm::mutation_count() const { return m_internals->mutations; }
net::Url const& Realm::origin_url() const { return m_internals->origin_url; }

std::uint64_t Realm::tree_mutation_count() const
{
    std::uint64_t count = m_internals->mutations;
    for (ChildFrame const& listed : m_internals->child_frames)
        count += listed.realm->tree_mutation_count();
    return count;
}
void Realm::note_mutation() { ++m_internals->mutations; }
ScriptStats const& Realm::stats() const { return m_internals->stats; }

js::Value Realm::wrap_or_null(dom::Node* node)
{
    return node ? js::Value::object(wrap(*node)) : js::Value::null();
}

dom::Node* Realm::node_of(js::Value const& value) const
{
    NodeWrapper* wrapper = m_internals->wrapper_of(value);
    return wrapper ? &wrapper->node() : nullptr;
}

void Realm::run_script(dom::Element& script, html::TreeBuilder& builder)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    html::TreeBuilder* const previous = in.active_parser;
    in.active_parser = &builder;
    in.prepare_script(script, true);
    in.active_parser = previous;
}

void Realm::run_inserted_script(dom::Element& script)
{
    Internals::HostEntry const host(m_internals->agent);
    js::Interpreter::RealmScope const inside(m_internals->interpreter, m_internals->realm_record);
    m_internals->prepare_script(script, false);
}

js::Outcome Realm::run(std::string_view utf8_source, std::string name)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    Internals::Entry const entry(in);
    js::Outcome outcome = in.interpreter.run_script(utf8_source, name);
    if (!outcome.ok) {
        if (in.interpreter.terminated())
            in.console("error", "script stopped: " + name);
        else
            in.report_uncaught(outcome.value, name);
    }
    return outcome;
}

namespace {

void collect_frames(dom::Node const& node, std::vector<dom::Element*>& out)
{
    for (dom::Node* const child : node.children()) {
        if (child->is_element() && static_cast<dom::Element*>(child)->is_html("iframe"))
            out.push_back(static_cast<dom::Element*>(child));
        collect_frames(*child, out);
    }
}

// The javascript: URL an iframe's src names, when it has no srcdoc to show
// instead (HTML §4.8.5, "process the iframe attributes").
std::optional<net::Url> javascript_src(dom::Element const& iframe, net::Url const& base)
{
    if (iframe.find_attribute("srcdoc"))
        return std::nullopt;
    dom::Attr const* const src = iframe.find_attribute("src");
    std::optional<net::Url> url = src ? net::parse_url(src->value, &base) : std::nullopt;
    if (!url || url->scheme != "javascript")
        return std::nullopt;
    return url;
}

// The navigation a javascript: src makes as the page's parse ends: the page's,
// and the iframe's initial insertion.
FrameNavigation parsed_javascript_navigation(Realm::Internals const& in)
{
    FrameNavigation navigation;
    navigation.initiator_origin = in.origin_url;
    navigation.initial_insertion = true;
    return navigation;
}

} // namespace

void Realm::document_parsed()
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    // What the host does in this document's name happens in its realm, even
    // when a page's realm opens a frame's document from inside its own.
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    in.active_parser = nullptr;
    in.ready_state = "interactive";
    dispatch_event(&in.document, "readystatechange");
    // The deferred scripts, in order; each was fetched when the parser met
    // it, a module's graph with it.
    std::vector<Internals::PendingScript> deferred = std::move(in.deferred_scripts);
    in.deferred_scripts.clear();
    for (Internals::PendingScript& pending : deferred) {
        if (pending.module != nullptr)
            in.execute_module(*pending.element, *pending.module, pending.name);
        else
            in.execute_script(*pending.element, pending.source, pending.name);
    }
    dispatch_event(&in.document, "DOMContentLoaded", EventInit { true, false, false });
    // A page's load waits on its frames' documents, so every iframe the
    // parse left in the tree has fired its load event, in tree order, by the
    // time the window fires its own. A frame's document the host answers for
    // is parsed and loaded in a realm of its own first.
    std::vector<dom::Element*> frames;
    collect_frames(in.document, frames);
    for (dom::Element* const frame : frames) {
        auto const listed = std::find_if(in.child_frames.begin(), in.child_frames.end(),
            [frame](ChildFrame const& child) { return child.container == frame; });
        bool const opened = listed != in.child_frames.end();
        if (opened && !listed->awaiting_navigation) {
            // An about:blank frame, opened and loaded as it was inserted; or
            // the initial about:blank document of one whose src is a
            // javascript: URL, which runs now, before the page's load, unless
            // a script has asked for a navigation of the frame since.
            if (!in.frame_navigations.contains(frame)) {
                if (std::optional<net::Url> const script = javascript_src(*frame, in.url);
                    script && !in.inline_refused(net::InlineKind::Script, {}, script->serialize()))
                    in.run_javascript_url(*frame, *script, parsed_javascript_navigation(in));
            }
            continue;
        }
        // The initial about:blank document a src or srcdoc stood in for gives
        // way to what the attributes name now. What a script asked of the
        // frame meanwhile is done here: a src it set to a javascript: URL runs
        // in the initial about:blank document, before the page's load, as a
        // parsed one does.
        in.frame_navigations.erase(frame);
        if (std::optional<net::Url> const script = javascript_src(*frame, in.url)) {
            if (opened) {
                listed->awaiting_navigation = false;
                listed->source = frame_source(*frame, in.url);
            } else {
                in.open_blank_frame(*frame);
            }
            if (!in.inline_refused(net::InlineKind::Script, {}, script->serialize()))
                in.run_javascript_url(*frame, *script, parsed_javascript_navigation(in));
            continue;
        }
        std::uint64_t mutations_from = 0;
        if (opened) {
            mutations_from = listed->realm->tree_mutation_count() + 1;
            in.close_frame(*frame, true);
        }
        in.open_frame(*frame, mutations_from);
        // Not at an iframe its own document's load took out of the tree.
        if (frame->is_connected())
            in.fire_frame_load(*frame);
    }
    in.ready_state = "complete";
    dispatch_event(&in.document, "readystatechange");
    dispatch_event(nullptr, "load");
    dispatch_event(nullptr, "pageshow");
}

void Realm::Internals::open_frame(dom::Element& iframe, std::uint64_t mutations_from, std::optional<net::Url> const& target)
{
    if (realm.frame_realm(iframe) != nullptr)
        return;
    // The documents this frame is inside, the page first: the framing rules
    // read the chain, and it goes ten frames deep, as the painter draws them.
    std::vector<FrameAncestor> ancestors;
    for (Internals const* up = this; up != nullptr; up = up->parent_realm)
        ancestors.push_back(FrameAncestor { up->url.serialize(true), up->origin_url });
    if (ancestors.size() > 10)
        return;
    std::reverse(ancestors.begin(), ancestors.end());
    // Nothing to show at all — no src, an empty one, about:blank, or a
    // navigation to about:blank, the initial one included — is an about:blank
    // document (HTML §7.5.2), which is never fetched: empty, of this
    // document's origin, under this document's policy, as an srcdoc document
    // is. The host answers for everything else a frame shows.
    bool const blank = target ? target->scheme == "about" && target->serialize(true) == "about:blank"
                              : frame_source(iframe, url).empty();
    std::optional<FrameDocument> answer
        = !blank && hooks.frame_document ? hooks.frame_document(iframe, url, hooks.policy, ancestors, target) : std::nullopt;
    if (!answer) {
        answer = FrameDocument {};
        answer->content_type = "text/html";
        if (blank) {
            answer->url = target ? *target : *net::parse_url("about:blank");
            answer->origin = origin_url;
            answer->srcdoc = true;
            if (hooks.policy)
                answer->policy = *hooks.policy;
        } else {
            // Something to show that the host could not — a failed fetch, a
            // document that refuses to be framed, a refused frame-src — is an
            // error document, as HTML makes "a document for inline content
            // that doesn't have a DOM" of a network error: empty, at the URL
            // asked for, of a new opaque origin, so the frame keeps a window
            // that no other document reaches into.
            std::optional<net::Url> asked = target;
            if (!asked) {
                dom::Attr const* const src = iframe.find_attribute("srcdoc") ? nullptr : iframe.find_attribute("src");
                asked = src ? net::parse_url(src->value, &url) : net::parse_url("about:srcdoc");
            }
            answer->url = asked ? *asked : *net::parse_url("about:blank");
            answer->origin = *net::parse_url("about:blank");
        }
    }
    open_frame_document(iframe, std::move(*answer), target ? "url:" + target->serialize() : frame_source(iframe, url), mutations_from, false);
}

void Realm::Internals::open_frame_document(dom::Element& iframe, FrameDocument answer, std::string source, std::uint64_t mutations_from, bool initial_blank)
{
    // A document that is not markup — a picture, text, JSON — is shown here
    // as an empty one at its URL and of its origin, so the frame still has
    // its window.
    std::string const type = ascii_lower(answer.content_type);
    if (!type.empty() && !type.starts_with("text/html") && !type.starts_with("application/xhtml")) {
        answer.bytes.clear();
        answer.content_type = "text/html";
    }
    // The sandboxing flags the frame navigates with: its iframe's sandbox
    // attribute as it stands now, with this document's own. Without
    // allow-same-origin its document's origin is a new opaque one.
    std::uint32_t flags = sandbox_flags;
    if (dom::Attr const* const sandbox = iframe.find_attribute("sandbox"))
        flags |= parse_sandboxing_directive(sandbox->value);
    if (flags & sandboxing::origin)
        answer.origin = *net::parse_url("about:blank");
    ChildFrame opened;
    opened.container = &iframe;
    opened.source = std::move(source);
    opened.initial_blank = initial_blank;
    opened.policy = answer.policy ? std::make_unique<net::ContentSecurityPolicy>(std::move(*answer.policy))
                                  : std::make_unique<net::ContentSecurityPolicy>(answer.url);
    opened.document = std::make_unique<dom::Document>();
    // What the frame's realm asks its host for goes where the page's requests
    // go; the boxes, controls and navigation of the page are not the frame's.
    HostHooks frame_hooks;
    frame_hooks.fetch_script = hooks.fetch_script;
    frame_hooks.fetch_resource = hooks.fetch_resource;
    frame_hooks.policy = opened.policy.get();
    frame_hooks.now = hooks.now;
    frame_hooks.should_stop = hooks.should_stop;
    frame_hooks.cookie_get = hooks.cookie_get;
    frame_hooks.cookie_set = hooks.cookie_set;
    frame_hooks.console = hooks.console;
    frame_hooks.local_storage = hooks.local_storage;
    frame_hooks.frame_document = hooks.frame_document;
    frame_hooks.trace = hooks.trace;
    frame_hooks.viewport_width = hooks.viewport_width;
    frame_hooks.viewport_height = hooks.viewport_height;
    frame_hooks.device_scale = hooks.device_scale;
    frame_hooks.user_agent = hooks.user_agent;
    opened.realm = std::make_unique<Realm>(*this, iframe, *opened.document, answer.url, std::move(frame_hooks));
    opened.realm->internals().origin_url = answer.origin;
    opened.realm->internals().sandbox_flags = flags;
    opened.realm->internals().document_content_type = type.empty() ? std::string("text/html") : mime_essence(type);
    // An srcdoc or about:blank document has this document's origin itself,
    // and so the domain document.domain gives either of them; a sandbox has
    // already made a sandboxed one's origin its own.
    if (answer.srcdoc && answer.origin.serialize() == origin_url.serialize())
        opened.realm->internals().domain.share(domain);
    // The navigable the iframe's frame already has, when the frame goes on to
    // another document, and its WindowProxy with its name; a new one, with the
    // new realm's WindowProxy and the iframe's name, for a frame opened afresh.
    Internals& opened_internals = opened.realm->internals();
    auto const navigable = navigables.find(&iframe);
    if (navigable != navigables.end()) {
        navigable->second.window_proxy->stand_for(*opened_internals.realm_record);
        interpreter.set_global_this(*opened_internals.realm_record, navigable->second.window_proxy);
    } else {
        ChildNavigable& made = navigables[&iframe];
        made.window_proxy = static_cast<WindowProxyObject*>(opened_internals.realm_record->global_this);
        if (dom::Attr const* const frame_name = iframe.find_attribute("name"))
            made.target_name = frame_name->value;
    }
    // A reopened frame counts on from its predecessor, so no picture of the
    // old document stands for the new one.
    opened.realm->internals().mutations = mutations_from;
    Realm& opened_realm = *opened.realm;
    dom::Document& opened_document = *opened.document;
    trace("frame opened: " + (opened.source.empty() ? std::string("about:blank") : opened.source) + " in " + url.serialize(true));
    child_frames.push_back(std::move(opened));
    std::string_view const text(reinterpret_cast<char const*>(answer.bytes.data()), answer.bytes.size());
    if (answer.srcdoc)
        html::parse_document_into(opened_document, decode_utf8(text), &opened_realm);
    else
        html::parse_document_bytes_into(opened_document, text, &opened_realm);
    opened_realm.document_parsed();
}

std::string* Realm::Internals::navigable_target_name()
{
    if (ended || discarded)
        return nullptr;
    if (parent_realm == nullptr)
        return &top_level_target_name;
    auto const found = parent_realm->navigables.find(frame_element);
    if (found == parent_realm->navigables.end() || &found->second.window_proxy->record() != realm_record)
        return nullptr;
    return &found->second.target_name;
}

std::string const* Realm::Internals::child_target_name(ChildFrame const& child) const
{
    auto const found = navigables.find(child.container);
    return found == navigables.end() ? nullptr : &found->second.target_name;
}

ChildFrame const* Realm::Internals::frame_of(dom::Element const& iframe) const
{
    for (ChildFrame const& listed : child_frames) {
        if (listed.container != &iframe)
            continue;
        // The same origin as this document's, and not an opaque one.
        std::string const own = origin_url.serialize_origin();
        return own != "null" && listed.realm->internals().origin_url.serialize_origin() == own ? &listed : nullptr;
    }
    return nullptr;
}

Realm* Realm::frame_realm(dom::Element const& iframe)
{
    for (ChildFrame const& listed : m_internals->child_frames) {
        if (listed.container == &iframe)
            return listed.realm.get();
    }
    return nullptr;
}

void Realm::Internals::close_frame(dom::Element const& iframe, bool keep_window_proxy)
{
    frame_navigations.erase(&iframe);
    // An iframe leaving the tree takes its frame's WindowProxy with it: put
    // back in, it has a new frame and a new one.
    if (!keep_window_proxy)
        navigables.erase(&iframe);
    auto const it = std::find_if(child_frames.begin(), child_frames.end(),
        [&iframe](ChildFrame const& listed) { return listed.container == &iframe; });
    if (it == child_frames.end())
        return;
    // Nothing of its runs again, its own frames' included; the realm itself
    // ends when the host's last entry into the agent has returned, and its
    // window and its frames' windows are closed from now on.
    trace("frame closed: " + (it->source.empty() ? std::string("about:blank") : it->source));
    discard_windows(it->realm->internals());
    erase_loop_work(it->realm->internals());
    agent.closing.push_back(std::move(*it));
    child_frames.erase(it);
}

Realm::Internals* Realm::Internals::realm_of(dom::Document const& target)
{
    Internals* page = this;
    while (page->parent_realm != nullptr)
        page = page->parent_realm;
    std::vector<Internals*> pending { page };
    while (!pending.empty()) {
        Internals* const at = pending.back();
        pending.pop_back();
        if (&at->document == &target)
            return at;
        for (std::unique_ptr<dom::Document> const& extra : at->extra_documents) {
            if (extra.get() == &target)
                return at;
        }
        for (ChildFrame const& listed : at->child_frames)
            pending.push_back(&listed.realm->internals());
    }
    return nullptr;
}

namespace {

// Every element of a subtree, the root included, template contents too.
void walk_subtree(dom::Node& root, std::vector<dom::Node*>& out)
{
    std::vector<dom::Node*> pending { &root };
    while (!pending.empty()) {
        dom::Node* const current = pending.back();
        pending.pop_back();
        out.push_back(current);
        for (dom::Node* const child : current->children())
            pending.push_back(child);
        if (current->is_element()) {
            if (dom::Node* const content = static_cast<dom::Element*>(current)->template_content())
                pending.push_back(content);
        }
    }
}

} // namespace

void Realm::Internals::adopt_into(dom::Document& target, dom::Node& node)
{
    if (&node.document() == &target)
        return;
    if (node.parent())
        frames_removed(node);
    target.adopt(node);
    Internals* const home = realm_of(target);
    if (!home)
        return;
    std::vector<dom::Node*> nodes;
    walk_subtree(node, nodes);
    for (dom::Node* const moved : nodes) {
        if (moved->wrapper)
            static_cast<NodeWrapper*>(moved->wrapper)->rehome(*home->realm_record);
    }
}

void Realm::Internals::frames_removed(dom::Node& subtree)
{
    std::vector<dom::Node*> nodes;
    walk_subtree(subtree, nodes);
    for (dom::Node* const node : nodes) {
        if (!node->is_element() || !static_cast<dom::Element*>(node)->is_html("iframe"))
            continue;
        if (Internals* const owner = realm_of(node->document()))
            owner->close_frame(*static_cast<dom::Element*>(node));
    }
}

void Realm::Internals::frames_inserted(dom::Node& subtree)
{
    std::vector<dom::Node*> nodes;
    walk_subtree(subtree, nodes);
    for (dom::Node* const node : nodes) {
        if (!node->is_element() || !static_cast<dom::Element*>(node)->is_html("iframe"))
            continue;
        dom::Element& iframe = *static_cast<dom::Element*>(node);
        if (Internals* const owner = realm_of(node->document()); owner && !owner->open_blank_frame(iframe)) {
            FrameNavigation navigation;
            navigation.initial_insertion = true;
            owner->schedule_frame_navigation(iframe, std::move(navigation));
        }
    }
}

bool Realm::Internals::open_blank_frame(dom::Element& iframe)
{
    // Every iframe has its initial about:blank document before the next line
    // (HTML §4.8.5), and its load too when the iframe names nothing else: a
    // listener added after the insertion never sees that load. One whose src
    // or srcdoc names a document keeps it only until it navigates there, with
    // that document's load alone.
    if (!iframe.is_connected() || realm.frame_realm(iframe) != nullptr)
        return true;
    bool const script = javascript_src(iframe, url).has_value();
    bool const names_document = !script && !frame_source(iframe, url).empty();
    open_frame(iframe, 0, *net::parse_url("about:blank"));
    // Keyed by its attributes, as a frame they opened is, not by the
    // about:blank it was opened on — unless it awaits what they name.
    for (ChildFrame& listed : child_frames) {
        if (listed.container == &iframe) {
            listed.initial_blank = true;
            listed.awaiting_navigation = names_document;
            listed.source = names_document ? std::string() : frame_source(iframe, url);
        }
    }
    if (script || names_document)
        return false;
    if (realm.frame_realm(iframe) != nullptr)
        fire_frame_load(iframe);
    return true;
}

void Realm::Internals::fire_frame_load(dom::Element& iframe)
{
    realm.dispatch_event(&iframe, "load");
}

void Realm::frame_inserted(dom::Element& iframe)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    in.open_blank_frame(iframe);
}

void Realm::Internals::schedule_frame_navigation(dom::Element& iframe, FrameNavigation navigation)
{
    if (!iframe.is_connected())
        return;
    Internals* const owner = realm_of(iframe.document());
    if (!owner)
        return;
    // The iframe lives as long as its document, and the task as long as the
    // document's realm: it goes when that realm ends. Only the navigation
    // asked for last goes ahead, as a new navigation ends an ongoing one.
    std::uint64_t const number = owner->agent.next_sequence++;
    owner->frame_navigations[&iframe] = number;
    owner->post_task([owner, &iframe, number, navigation = std::move(navigation)] { owner->navigate_frame(iframe, number, navigation); });
}

void Realm::Internals::navigate_frame(dom::Element& iframe, std::uint64_t number, FrameNavigation const& navigation)
{
    // Only an iframe still in this document, and only the navigation of it
    // asked for last: a later one, or the parse's opening of the frame, has
    // taken this one's place.
    if (&iframe.document() != &document || !iframe.is_connected())
        return;
    auto const pending = frame_navigations.find(&iframe);
    if (pending == frame_navigations.end() || pending->second != number)
        return;
    frame_navigations.erase(pending);
    // A javascript: URL, navigated to or named by a src, runs in the frame's
    // document; a src's is run at the ask of the iframe's document, under its
    // policy.
    if (navigation.target && navigation.target->scheme == "javascript") {
        run_javascript_url(iframe, *navigation.target, navigation);
        return;
    }
    if (!navigation.target) {
        if (std::optional<net::Url> const script = javascript_src(iframe, url)) {
            if (inline_refused(net::InlineKind::Script, {}, script->serialize()))
                return;
            FrameNavigation from_attributes = navigation;
            from_attributes.initiator_origin = origin_url;
            run_javascript_url(iframe, *script, from_attributes);
            return;
        }
    }
    // Anything else closes the frame and opens it anew — though not for
    // attributes that name what the frame shows, opened from those very
    // attributes and not navigated elsewhere since, unless to reload it. HTML
    // navigates again even then; but until the old document's beforeunload
    // and unload are fired, a page that sets an iframe's src from that
    // iframe's own load would navigate it without end, so the frame is left
    // as it is.
    std::string const key = navigation.target ? navigation.target->serialize() : frame_source(iframe, url);
    auto const existing = std::find_if(child_frames.begin(), child_frames.end(),
        [&iframe](ChildFrame const& listed) { return listed.container == &iframe; });
    std::uint64_t mutations_from = 0;
    if (existing != child_frames.end()) {
        if (!navigation.target && !navigation.reload && existing->source == key && !existing->awaiting_navigation)
            return; // already showing it
        mutations_from = existing->realm->tree_mutation_count() + 1;
        close_frame(iframe, true);
    }
    trace("frame navigates: " + (key.empty() ? std::string("about:blank") : key));
    open_frame(iframe, mutations_from, navigation.target);
    if (iframe.is_connected())
        fire_frame_load(iframe);
}

void Realm::Internals::run_javascript_url(dom::Element& iframe, net::Url const& script_url, FrameNavigation const& navigation)
{
    auto const existing = std::find_if(child_frames.begin(), child_frames.end(),
        [&iframe](ChildFrame const& listed) { return listed.container == &iframe; });
    if (existing == child_frames.end())
        return; // no document for it to run in
    Realm& frame = *existing->realm;
    Internals& target = frame.internals();
    bool const initial_blank = existing->initial_blank;
    // Only a document of the origin of the frame's document runs script in it.
    std::string const active = target.origin_url.serialize_origin();
    if (!navigation.initiator_is_frame && (active == "null" || navigation.initiator_origin.serialize_origin() != active))
        return;
    // HTML's "evaluate a javascript: URL": what follows the scheme,
    // percent-decoded, run as a classic script in the frame's document — not
    // at all where its sandbox keeps scripts off — whose string completion is
    // the markup of the document that replaces it.
    std::string const text = script_url.serialize();
    auto const hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string source;
    for (std::size_t i = std::string_view("javascript:").size(); i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            source += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            source += text[i];
        }
    }
    std::optional<std::string> result;
    if (!target.scripts_sandboxed()) {
        Entry const entry(target);
        ++target.stats.scripts_run;
        js::Outcome const outcome = interpreter.run_script(std::string_view(source), "javascript: URL");
        if (!outcome.ok) {
            ++target.stats.scripts_failed;
            if (interpreter.terminated())
                target.console("error", "script stopped: javascript: URL");
            else
                target.report_uncaught(outcome.value, "javascript: URL");
        } else if (outcome.value.is_string()) {
            // Read before the entry's checkpoint can collect it.
            result = outcome.value.as_string()->to_utf8();
        }
    }
    // The script may have closed or replaced the frame itself.
    if (realm.frame_realm(iframe) != &frame || !iframe.is_connected())
        return;
    if (!result) {
        // The document stays; the insertion's own load event is still owed.
        if (navigation.initial_insertion && initial_blank)
            fire_frame_load(iframe);
        return;
    }
    // A new document from the string, at the old one's URL, of the origin of
    // the document that asked, under the old one's policy.
    FrameDocument answer;
    answer.bytes.assign(result->begin(), result->end());
    answer.content_type = "text/html";
    answer.url = target.url;
    answer.origin = navigation.initiator_origin;
    answer.srcdoc = true;
    if (target.hooks.policy)
        answer.policy = *target.hooks.policy;
    std::uint64_t const mutations_from = frame.tree_mutation_count() + 1;
    trace("frame navigates: " + text);
    close_frame(iframe, true);
    open_frame_document(iframe, std::move(answer), navigation.target ? "url:" + script_url.serialize() : frame_source(iframe, url), mutations_from, false);
    if (iframe.is_connected())
        fire_frame_load(iframe);
}

namespace {

// HTML's "allowed by sandboxing to navigate": script in `source`'s document
// may navigate its own document and its descendants' whatever its sandbox;
// another frame only without the sandboxed navigation flag; the top only
// without the sandboxed top-level navigation flag (no user activation is
// tracked, so the flag without it is the one asked).
bool allowed_by_sandboxing(Realm::Internals const& source, Realm::Internals const& target)
{
    if (&source == &target)
        return true;
    if (target.parent_realm == nullptr)
        return (source.sandbox_flags & sandboxing::top_navigation) == 0;
    for (Realm::Internals const* up = target.parent_realm; up != nullptr; up = up->parent_realm) {
        if (up == &source)
            return true;
    }
    return (source.sandbox_flags & sandboxing::navigation) == 0;
}

} // namespace

bool Realm::Internals::navigate_from_location(net::Url const& target_url, Internals& source, bool reload)
{
    // A reload asks no sandbox: Location's reload() reloads the navigable of
    // the Location's own document, which is not a navigation by `source`.
    if (!reload && !allowed_by_sandboxing(source, *this))
        return false;
    // A page with a host is navigated by its host, fragment and all.
    if (parent_realm == nullptr && hooks.navigate) {
        trace("navigate: " + target_url.serialize());
        hooks.navigate(target_url);
        return true;
    }
    // The fragment alone, on this document (HTML's "navigate to a fragment"):
    // the URL at once, and hashchange at the window after the script when the
    // fragment is not the one the URL had (HTML's "update document for history
    // step application").
    if (!reload && target_url.fragment && target_url.serialize(true) == url.serialize(true)) {
        trace("navigate to a fragment: " + target_url.serialize());
        bool const changed = target_url.fragment != url.fragment;
        url = target_url;
        if (changed)
            post_task([this] { realm.dispatch_event(nullptr, "hashchange"); });
        return true;
    }
    // A page with no host to navigate it keeps the URL alone.
    if (parent_realm == nullptr || frame_element == nullptr) {
        url = target_url;
        return true;
    }
    // A frame's navigation is its iframe's, in a task after the script; the
    // document of a frame that has closed navigates nothing.
    if (parent_realm->realm.frame_realm(*frame_element) != &realm)
        return true;
    if (target_url.scheme == "javascript" && source.inline_refused(net::InlineKind::Script, {}, target_url.serialize()))
        return true;
    FrameNavigation navigation;
    // A reload of an srcdoc document reads the srcdoc again.
    if (!reload || url.serialize(true) != "about:srcdoc")
        navigation.target = target_url;
    navigation.reload = reload;
    navigation.initiator_origin = source.origin_url;
    navigation.initiator_is_frame = &source == this;
    trace("navigate: " + target_url.serialize());
    parent_realm->schedule_frame_navigation(*frame_element, std::move(navigation));
    return true;
}

// --- Events from the host ----------------------------------------------------------------

bool Realm::dispatch_event(dom::Node* target, std::string_view type, EventInit init)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    js::Interpreter::Roots const roots(in.interpreter);
    EventObject* event = in.new_event("Event", type, init.bubbles, init.cancelable);
    in.interpreter.root(js::Value::object(event));
    event->composed = init.composed;
    event->is_trusted = true;
    js::Object* target_object = target ? in.wrap(*target) : in.window_proxy();
    return in.dispatch(*event, target_object);
}

bool Realm::dispatch_mouse_event(dom::Node& target, std::string_view type, MouseInit const& init)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    js::Interpreter::Roots const roots(in.interpreter);
    bool const bubbles = type != "mouseenter" && type != "mouseleave";
    bool const cancelable = type != "mouseenter" && type != "mouseleave" && type != "mousemove";
    EventObject* event = in.new_event("MouseEvent", type, bubbles, cancelable);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    event->composed = true;
    event->client_x = init.client_x;
    event->client_y = init.client_y;
    event->screen_x = init.client_x;
    event->screen_y = init.client_y;
    event->button = init.button;
    event->buttons = type == "mousedown" ? (init.button == 0 ? 1 : init.button == 1 ? 4 : 2) : 0;
    event->detail = init.detail;
    event->ctrl_key = init.ctrl;
    event->shift_key = init.shift;
    event->alt_key = init.alt;
    event->meta_key = init.meta;
    return in.dispatch(*event, in.wrap(target));
}

bool Realm::dispatch_key_event(dom::Node* target, std::string_view type, KeyInit const& init)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    js::Interpreter::Roots const roots(in.interpreter);
    EventObject* event = in.new_event("KeyboardEvent", type, true, true);
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    event->composed = true;
    event->key = init.key;
    event->code = init.code;
    event->key_code = init.key_code;
    event->ctrl_key = init.ctrl;
    event->shift_key = init.shift;
    event->alt_key = init.alt;
    event->meta_key = init.meta;
    event->repeat = init.repeat;
    js::Object* target_object = target ? in.wrap(*target) : in.wrap(in.document);
    return in.dispatch(*event, target_object);
}

bool Realm::dispatch_input_event(dom::Node& target, std::string_view type, InputInit const& init)
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    js::Interpreter::Roots const roots(in.interpreter);
    EventObject* event = in.new_event(type == "input" ? "InputEvent" : "Event", type, true, type == "beforeinput");
    in.interpreter.root(js::Value::object(event));
    event->is_trusted = true;
    event->composed = type == "input";
    event->data = init.data;
    event->input_type = init.input_type;
    return in.dispatch(*event, in.wrap(target));
}

// --- The event loop -------------------------------------------------------------------------

bool Realm::run_pending()
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    double const now = in.now();
    // Only the timers that exist now: one set while running waits for the
    // next pump, so a chain of zero-delay timers cannot hold the host.
    Agent& agent = in.agent;
    std::uint64_t const cutoff = agent.next_sequence;
    // What a script asked of an ended realm's window never runs.
    std::erase_if(agent.timers, [](Timer const& timer) { return timer.owner->ended; });
    std::erase_if(agent.tasks, [](Task const& task) { return task.owner != nullptr && task.owner->ended; });
    bool ran = false;
    // The tasks queued before this pump, oldest first; one a task queues
    // waits for the next pump, like a timer.
    while (!agent.tasks.empty() && agent.tasks.front().sequence < cutoff) {
        std::function<void()> task = std::move(agent.tasks.front().run);
        Internals* const task_owner = agent.tasks.front().owner;
        agent.tasks.pop_front();
        {
            // A task runs in the realm that queued it.
            if (task_owner)
                task_owner->trace("task runs");
            js::Interpreter::RealmScope const inside(agent.interpreter, task_owner ? task_owner->realm_record : nullptr);
            task();
        }
        ran = true;
        if (in.interpreter.terminated())
            return ran;
    }
    while (true) {
        std::size_t best = agent.timers.size();
        for (std::size_t i = 0; i < agent.timers.size(); ++i) {
            Timer const& timer = agent.timers[i];
            if (timer.due > now || timer.sequence >= cutoff)
                continue;
            if (best == agent.timers.size() || timer.due < agent.timers[best].due
                || (timer.due == agent.timers[best].due && timer.sequence < agent.timers[best].sequence))
                best = i;
        }
        if (best == agent.timers.size())
            break;
        Timer timer = std::move(agent.timers[best]);
        agent.timers.erase(agent.timers.begin() + static_cast<std::ptrdiff_t>(best));
        // A timer runs as the realm that set it, with that realm's window as
        // `this`.
        Internals& owner = *timer.owner;
        bool const animation_frame = timer.animation_frame;
        js::Interpreter::Roots const roots(agent.interpreter);
        js::Value const callback = agent.interpreter.root(timer.callback->value());
        std::vector<js::Value> arguments;
        for (auto const& argument : timer.arguments)
            arguments.push_back(agent.interpreter.root(argument->value()));
        if (timer.interval >= 0) {
            // Re-armed before it runs, under its own id, so that a
            // clearInterval inside the callback finds it.
            timer.due = now + timer.interval;
            timer.sequence = agent.next_sequence++;
            agent.timers.push_back(std::move(timer));
        }
        ++owner.stats.timers_fired;
        owner.trace("timer " + std::to_string(timer.id) + " fires at " + std::to_string(static_cast<long>(now)) + " ms");
        ran = true;
        js::Value const owner_window = js::Value::object(owner.window_proxy());
        if (callback.is_string()) {
            owner.realm.run(callback.as_string()->to_utf8(), "<timer>");
        } else if (animation_frame) {
            js::Value const timestamp[1] = { js::Value::number(now - owner.time_origin) };
            owner.call_reporting(callback, owner_window, timestamp, "animation frame");
        } else {
            owner.call_reporting(callback, owner_window, arguments, "timer");
        }
        if (in.interpreter.terminated())
            break;
    }
    return ran;
}

std::optional<double> Realm::next_timer_due() const
{
    // A queued task is due now.
    if (!m_internals->agent.tasks.empty())
        return m_internals->now();
    std::optional<double> due;
    for (Timer const& timer : m_internals->agent.timers) {
        if (!due || timer.due < *due)
            due = timer.due;
    }
    return due;
}

bool Realm::has_pending_timers() const
{
    return !m_internals->agent.timers.empty() || !m_internals->agent.tasks.empty();
}

void Realm::perform_microtask_checkpoint()
{
    Internals& in = *m_internals;
    Internals::HostEntry const host(in.agent);
    if (in.agent.in_checkpoint)
        return;
    in.agent.in_checkpoint = true;
    {
        // The queue is the interpreter's: queueMicrotask callbacks and
        // promise reactions in one order. The Entry accounts the time;
        // its own checkpoint on the way out finds in_checkpoint set.
        Internals::Entry const entry(in);
        in.interpreter.run_jobs([&in](js::Value const& thrown) {
            if (in.interpreter.terminated())
                return; // the host stopped the script; nothing to report but the count
            in.report_uncaught(thrown, "microtask");
        });
    }
    in.agent.in_checkpoint = false;
}

// --- Roots ------------------------------------------------------------------------------------

namespace {

void trace_tree(dom::Node const& node, js::Tracer& tracer)
{
    if (node.wrapper)
        tracer.visit(node.wrapper);
    if (node.is_element()) {
        if (dom::Node const* content = static_cast<dom::Element const&>(node).template_content())
            trace_tree(*content, tracer);
    }
    for (dom::Node const* child : node.children())
        trace_tree(*child, tracer);
}

} // namespace

void Realm::trace_roots(js::Tracer& tracer)
{
    Internals& in = *m_internals;
    // The connected tree is one opaque root (ADR 0001 §2).
    trace_tree(in.document, tracer);
    for (ListenerEntry const& entry : in.window_listeners)
        tracer.visit(entry.listener.callback);
    for (auto const& [type, handler] : in.window_handlers)
        tracer.visit(handler.function);
    tracer.visit(in.current_event);
    tracer.visit(in.history_state);
    tracer.visit(in.location);
    tracer.visit(in.local_storage_object);
    tracer.visit(in.session_storage_object);
    for (auto const& [name, prototype] : in.prototypes)
        tracer.visit(prototype);
    for (auto const& [iframe, navigable] : in.navigables)
        tracer.visit(navigable.window_proxy);
    for (auto const& [name, value] : in.window_values)
        tracer.visit(value);
    for (auto const& [name, member] : in.cross_origin_members) {
        if (member.value)
            tracer.visit(*member.value);
        if (member.get)
            tracer.visit(*member.get);
        if (member.set)
            tracer.visit(*member.set);
    }
    // Timers and microtasks hold their callbacks in Persistents, which the
    // heap roots by itself.
}

}
