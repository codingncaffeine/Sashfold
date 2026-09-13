#include "bindings/Internal.h"

#include "bindings/Fetching.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "html/Serializer.h"
#include "js/Module.h"
#include "js/Strings.h"

#include <algorithm>
#include <chrono>
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

NodeWrapper::NodeWrapper(js::Object* prototype, Realm& realm, dom::Node& node)
    : EventTargetObject(prototype)
    , m_realm(&realm)
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

void NodeWrapper::trace(js::Tracer& tracer)
{
    EventTargetObject::trace(tracer);
    if (!m_node)
        return;
    // A detached subtree lives as long as a wrapper into it is reachable
    // (ADR 0001 §3): reaching any node of it keeps its root's wrapper, and a
    // document a script made keeps that document's wrapper. The connected
    // tree is marked by the realm as one root.
    dom::Node& root = m_node->root();
    if (&root != &m_realm->document() && root.wrapper && root.wrapper != this)
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

void TokenListObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    if (element->wrapper)
        tracer.visit(element->wrapper);
}

void StyleDeclarationObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    if (element && element->wrapper)
        tracer.visit(element->wrapper);
}

void DatasetObject::trace(js::Tracer& tracer)
{
    Object::trace(tracer);
    if (element->wrapper)
        tracer.visit(element->wrapper);
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

std::string attribute_or_empty(dom::Element const& element, std::string_view name)
{
    dom::Attr const* attribute = element.find_attribute(name);
    return attribute ? attribute->value : std::string();
}

void set_attribute(Realm::Internals& in, dom::Element& element, std::string_view name, std::string value)
{
    for (dom::Attr& attribute : element.attributes()) {
        if (attribute.local_name == name && attribute.prefix.empty()) {
            attribute.value = std::move(value);
            attribute.from_cssom = false; // set by a script as text: inline style again
            in.realm.note_mutation();
            return;
        }
    }
    element.attributes().push_back(dom::Attr { std::string(name), std::move(value), "", "" });
    in.realm.note_mutation();
}

bool remove_attribute(Realm::Internals& in, dom::Element& element, std::string_view name)
{
    auto& attributes = element.attributes();
    auto const it = std::find_if(attributes.begin(), attributes.end(),
        [name](dom::Attr const& attribute) { return attribute.local_name == name && attribute.prefix.empty(); });
    if (it == attributes.end())
        return false;
    attributes.erase(it);
    in.realm.note_mutation();
    return true;
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
    return hooks.policy != nullptr && !hooks.policy->sandbox_allows_scripts();
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
        std::optional<js::Value> const result = interpreter.call(callee, js::Value::object(interpreter.global()), arguments);
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
    : internals(the_internals)
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
    double const ended = static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
    internals.stats.script_ms += ended - started;
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
            return prototype("SVGElement");
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
    NodeWrapper* wrapper = interpreter.heap().allocate<NodeWrapper>(proto, realm, node);
    node.wrapper = wrapper;
    return wrapper;
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
    install_events(in);
    install_nodes(in);
    install_style(in);
    install_window(in);
    install_binary(in);
    install_fetch(in);
    install_xhr(in);
    install_tasks(in);
}

// Lets every wrapper into a tree go of its node: the realm that made them is
// ending before the heap they live in.
void detach_wrappers(dom::Node& node)
{
    if (node.wrapper) {
        static_cast<NodeWrapper*>(node.wrapper)->detach();
        node.wrapper = nullptr;
    }
    for (dom::Node* const child : node.children())
        detach_wrappers(*child);
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

Realm::~Realm()
{
    // The wrappers go with the heap, before the documents they point into:
    // the agent and its interpreter are destroyed here, the extra documents
    // after them (member order), the page's document by whoever owns it,
    // later. What this realm left on the agent's event loop goes first.
    Internals& in = *m_internals;
    std::erase_if(in.agent.timers, [&in](Timer const& timer) { return timer.owner == &in; });
    std::erase_if(in.agent.tasks, [&in](Task const& task) { return task.owner == &in; });
    // Its frames end first, while this realm and the agent are whole.
    in.child_frames.clear();
    if (in.own_agent) {
        in.interpreter.clear_jobs();
    } else {
        // A frame's realm ends before the heap its wrappers live in: they let
        // go of the nodes they point into, and the interpreter of this realm.
        detach_wrappers(in.document);
        for (std::unique_ptr<dom::Document> const& extra : in.extra_documents)
            detach_wrappers(*extra);
        in.realm_record->host_defined = nullptr;
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
    html::TreeBuilder* const previous = in.active_parser;
    in.active_parser = &builder;
    in.prepare_script(script, true);
    in.active_parser = previous;
}

void Realm::run_inserted_script(dom::Element& script)
{
    m_internals->prepare_script(script, false);
}

js::Outcome Realm::run(std::string_view utf8_source, std::string name)
{
    Internals& in = *m_internals;
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

} // namespace

void Realm::document_parsed()
{
    Internals& in = *m_internals;
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
        in.open_frame(*frame);
        dispatch_event(frame, "load");
    }
    in.ready_state = "complete";
    dispatch_event(&in.document, "readystatechange");
    dispatch_event(nullptr, "load");
    dispatch_event(nullptr, "pageshow");
}

void Realm::Internals::open_frame(dom::Element& iframe)
{
    if (!hooks.frame_document)
        return;
    // The documents this frame is inside, the page first: the framing rules
    // read the chain, and it goes ten frames deep, as the painter draws them.
    std::vector<FrameAncestor> ancestors;
    for (Internals const* up = this; up != nullptr; up = up->parent_realm)
        ancestors.push_back(FrameAncestor { up->url.serialize(true), up->origin_url });
    if (ancestors.size() > 10)
        return;
    std::reverse(ancestors.begin(), ancestors.end());
    std::optional<FrameDocument> answer = hooks.frame_document(iframe, url, hooks.policy, ancestors);
    if (!answer)
        return;
    std::string const type = ascii_lower(answer->content_type);
    if (!type.empty() && !type.starts_with("text/html") && !type.starts_with("application/xhtml"))
        return;
    ChildFrame opened;
    opened.container = &iframe;
    opened.policy = answer->policy ? std::make_unique<net::ContentSecurityPolicy>(std::move(*answer->policy))
                                   : std::make_unique<net::ContentSecurityPolicy>(answer->url);
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
    frame_hooks.viewport_width = hooks.viewport_width;
    frame_hooks.viewport_height = hooks.viewport_height;
    frame_hooks.device_scale = hooks.device_scale;
    frame_hooks.user_agent = hooks.user_agent;
    opened.realm = std::make_unique<Realm>(*this, iframe, *opened.document, answer->url, std::move(frame_hooks));
    opened.realm->internals().origin_url = answer->origin;
    Realm& opened_realm = *opened.realm;
    dom::Document& opened_document = *opened.document;
    child_frames.push_back(std::move(opened));
    std::string_view const text(reinterpret_cast<char const*>(answer->bytes.data()), answer->bytes.size());
    if (answer->srcdoc)
        html::parse_document_into(opened_document, decode_utf8(text), &opened_realm);
    else
        html::parse_document_bytes_into(opened_document, text, &opened_realm);
    opened_realm.document_parsed();
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

// --- Events from the host ----------------------------------------------------------------

bool Realm::dispatch_event(dom::Node* target, std::string_view type, EventInit init)
{
    Internals& in = *m_internals;
    js::Interpreter::Roots const roots(in.interpreter);
    EventObject* event = in.new_event("Event", type, init.bubbles, init.cancelable);
    in.interpreter.root(js::Value::object(event));
    event->composed = init.composed;
    event->is_trusted = true;
    js::Object* target_object = target ? in.wrap(*target) : in.interpreter.global();
    return in.dispatch(*event, target_object);
}

bool Realm::dispatch_mouse_event(dom::Node& target, std::string_view type, MouseInit const& init)
{
    Internals& in = *m_internals;
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
    double const now = in.now();
    // Only the timers that exist now: one set while running waits for the
    // next pump, so a chain of zero-delay timers cannot hold the host.
    Agent& agent = in.agent;
    std::uint64_t const cutoff = agent.next_sequence;
    bool ran = false;
    // The tasks queued before this pump, oldest first; one a task queues
    // waits for the next pump, like a timer.
    while (!agent.tasks.empty() && agent.tasks.front().sequence < cutoff) {
        std::function<void()> task = std::move(agent.tasks.front().run);
        agent.tasks.pop_front();
        task();
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
        ran = true;
        js::Value const owner_window = js::Value::object(owner.realm_record->intrinsics.global);
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
    for (auto const& [name, prototype] : in.prototypes)
        tracer.visit(prototype);
    // Timers and microtasks hold their callbacks in Persistents, which the
    // heap roots by itself.
}

}
