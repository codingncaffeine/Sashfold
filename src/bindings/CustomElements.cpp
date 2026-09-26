#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Custom elements (HTML §4.13): a page names a class of its own, and from
// then on every element of that name in the tree IS an object of that
// class — the constructor's own `this` — with the callbacks the tree owes
// it as it arrives, changes and leaves.
//
// Almost every page built out of components rests on this. Without it their
// markup is a shell of empty tags: the script defines a thousand classes,
// nothing upgrades, and the reader sees an empty page.
//
// The piece that makes it work is the HTMLElement constructor. A page's
// class extends it, so `super()` lands here, and what it answers depends on
// why it was called: while an element is being upgraded, the element on the
// definition's construction stack is handed back — so the class's `this` is
// the element already in the tree — and otherwise a new element of the
// definition's name is made.

namespace sashfold::bindings {

struct CustomElementDefinition {
    std::string name; // the local name, which is also what the page defined
    js::Value constructor;
    js::Value connected;
    js::Value disconnected;
    js::Value adopted;
    js::Value attribute_changed;
    std::vector<std::string> observed_attributes;
    // The elements being upgraded through this definition, innermost last.
    // A null entry is one whose constructor has already taken it.
    std::vector<dom::Element*> construction_stack;
};

namespace {

// A name a page may define (§4.13.1): a lowercase ASCII start, a dash
// somewhere after it, no uppercase, and none of the hyphenated names SVG
// and MathML took first.
bool is_valid_custom_element_name(std::string_view name)
{
    if (name.empty() || name.front() < 'a' || name.front() > 'z')
        return false;
    if (name.find('-') == std::string_view::npos)
        return false;
    for (char const c : name) {
        if (c >= 'A' && c <= 'Z')
            return false;
        bool const ordinary = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        // The rest of what the production allows is non-ASCII, which is
        // taken as it comes; the ASCII it forbids is what is checked here.
        if (!ordinary && static_cast<unsigned char>(c) < 0x80)
            return false;
    }
    static constexpr std::string_view taken[] = { "annotation-xml", "color-profile", "font-face", "font-face-src", "font-face-uri",
        "font-face-format", "font-face-name", "missing-glyph" };
    return std::find(std::begin(taken), std::end(taken), name) == std::end(taken);
}

// The registry's own account on stderr, under SASHFOLD_CE_TRACE=1: what a
// page defined and what actually became one of its elements. A page built
// out of components that defines a thousand names and upgrades a handful
// is a page that will look empty, and this says which handful.
bool tracing()
{
    static bool const enabled = [] {
        char const* const value = std::getenv("SASHFOLD_CE_TRACE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void trace(std::string const& line)
{
    if (tracing())
        std::cerr << "custom-element: " << line << "\n";
}

CustomElementDefinition* definition_for_constructor(Realm::Internals& in, js::Object const* constructor)
{
    if (constructor == nullptr)
        return nullptr;
    for (auto const& definition : in.custom_element_definitions) {
        if (definition->constructor.is_object() && definition->constructor.as_object() == constructor)
            return definition.get();
    }
    return nullptr;
}

// Only an element of the definition's own name and the HTML namespace is
// one of its elements.
bool matches(dom::Element const& element, CustomElementDefinition const& definition)
{
    return element.is_html() && element.local_name() == definition.name;
}

void collect_elements(dom::Node& node, std::vector<dom::Element*>& out)
{
    if (node.is_element())
        out.push_back(static_cast<dom::Element*>(&node));
    for (dom::Node* child : node.children())
        collect_elements(*child, out);
}

// The wrapper an element already has, without making one: an element that
// was never upgraded has nothing to say here.
NodeWrapper* existing_wrapper(dom::Node& node)
{
    return node.wrapper != nullptr ? dynamic_cast<NodeWrapper*>(node.wrapper) : nullptr;
}

void call_callback(Realm::Internals& in, js::Value const& callback, js::Value const& element, std::span<js::Value const> arguments,
    std::string_view where)
{
    if (!callback.is_object() || !callback.as_object()->is_callable())
        return;
    in.call_reporting(callback, element, arguments, where);
}

void fire_attribute_changed(Realm::Internals& in, NodeWrapper& wrapper, CustomElementDefinition& definition, std::string_view name,
    std::optional<std::string> const& old_value, std::optional<std::string> const& new_value)
{
    if (std::find(definition.observed_attributes.begin(), definition.observed_attributes.end(), name)
        == definition.observed_attributes.end())
        return;
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    js::Value const element = js::Value::object(&wrapper);
    interpreter.root(element);
    // Each string is rooted as it is made: making the next one allocates,
    // and a collection there would take any that nothing is yet holding.
    js::Value const named = in.string(name);
    interpreter.root(named);
    js::Value const before = old_value ? in.string(*old_value) : js::Value::null();
    interpreter.root(before);
    js::Value const after = new_value ? in.string(*new_value) : js::Value::null();
    interpreter.root(after);
    js::Value const arguments[] = { named, before, after, js::Value::null() };
    if (tracing())
        trace("attributeChanged <" + definition.name + "> " + std::string(name) + ": " + old_value.value_or("(none)") + " -> " + new_value.value_or("(none)"));
    call_callback(in, definition.attribute_changed, element, arguments, "attributeChangedCallback");
}

void fire_connected(Realm::Internals& in, NodeWrapper& wrapper, CustomElementDefinition& definition)
{
    js::Interpreter::Roots const roots(in.interpreter);
    js::Value const element = js::Value::object(&wrapper);
    in.interpreter.root(element);
    if (tracing())
        trace("connected <" + definition.name + ">");
    call_callback(in, definition.connected, element, {}, "connectedCallback");
}

void fire_disconnected(Realm::Internals& in, NodeWrapper& wrapper, CustomElementDefinition& definition)
{
    js::Interpreter::Roots const roots(in.interpreter);
    js::Value const element = js::Value::object(&wrapper);
    in.interpreter.root(element);
    if (tracing())
        trace("disconnected <" + definition.name + ">");
    call_callback(in, definition.disconnected, element, {}, "disconnectedCallback");
}

// --- The registry ------------------------------------------------------------------------

Native define(js::Interpreter& interp, js::Value const&, Args args)
{
    Realm::Internals& in = internals_of(interp);
    std::optional<std::string> const name = in.to_utf8(js::argument(args, 0));
    if (!name)
        return std::nullopt;
    js::Value const constructor = js::argument(args, 1);
    if (!constructor.is_object() || !constructor.as_object()->is_constructor())
        return interp.throw_type_error("Failed to execute 'define' on 'CustomElementRegistry': parameter 2 is not a constructor.");
    if (!is_valid_custom_element_name(*name)) {
        return in.throw_dom_exception("SyntaxError",
            "Failed to execute 'define' on 'CustomElementRegistry': \"" + *name + "\" is not a valid custom element name.");
    }
    for (auto const& already : in.custom_element_definitions) {
        if (already->name == *name) {
            return in.throw_dom_exception("NotSupportedError",
                "Failed to execute 'define' on 'CustomElementRegistry': the name \"" + *name + "\" has already been used.");
        }
        if (already->constructor.is_object() && already->constructor.as_object() == constructor.as_object()) {
            return in.throw_dom_exception("NotSupportedError",
                "Failed to execute 'define' on 'CustomElementRegistry': this constructor has already been used.");
        }
    }

    js::Interpreter::Roots const roots(interp);
    interp.root(constructor);
    std::optional<js::Value> const prototype = interp.get(*constructor.as_object(), interp.key("prototype"));
    if (!prototype)
        return std::nullopt;
    if (!prototype->is_object())
        return interp.throw_type_error("Failed to execute 'define' on 'CustomElementRegistry': the constructor's prototype is not an object.");
    interp.root(*prototype);

    auto definition = std::make_shared<CustomElementDefinition>();
    definition->name = *name;
    definition->constructor = constructor;
    // The callbacks are read once, now, as the definition is made.
    struct Named {
        char const* key;
        js::Value CustomElementDefinition::* into;
    };
    static constexpr Named callbacks[] = { { "connectedCallback", &CustomElementDefinition::connected },
        { "disconnectedCallback", &CustomElementDefinition::disconnected },
        { "adoptedCallback", &CustomElementDefinition::adopted },
        { "attributeChangedCallback", &CustomElementDefinition::attribute_changed } };
    for (Named const& callback : callbacks) {
        std::optional<js::Value> const found = interp.get(*prototype->as_object(), interp.key(callback.key));
        if (!found)
            return std::nullopt;
        if (found->is_object() && found->as_object()->is_callable())
            definition.get()->*callback.into = *found;
    }
    if (definition->attribute_changed.is_object()) {
        std::optional<js::Value> const observed = interp.get(*constructor.as_object(), interp.key("observedAttributes"));
        if (!observed)
            return std::nullopt;
        if (observed->is_object()) {
            std::optional<js::Value> const length = interp.get(*observed->as_object(), interp.key("length"));
            if (!length)
                return std::nullopt;
            std::optional<double> const count = interp.to_number(*length);
            if (!count)
                return std::nullopt;
            for (std::size_t i = 0; i < static_cast<std::size_t>(std::max(0.0, *count)); ++i) {
                std::optional<js::Value> const entry = interp.get(*observed->as_object(), js::PropertyKey::index(i));
                if (!entry)
                    return std::nullopt;
                std::optional<std::string> const text = in.to_utf8(*entry);
                if (!text)
                    return std::nullopt;
                definition->observed_attributes.push_back(*text);
            }
        }
    }
    CustomElementDefinition& stored = *definition;
    trace("defined <" + stored.name + "> (" + std::to_string(in.custom_element_definitions.size() + 1) + ")");
    in.custom_element_definitions.push_back(std::move(definition));
    ++in.stats.custom_elements_defined;

    // Everything of that name already in the document becomes one of them.
    std::vector<dom::Element*> elements;
    collect_elements(*in.document, elements);
    for (dom::Element* element : elements) {
        if (matches(*element, stored))
            upgrade_custom_element(in, *element);
    }
    return js::Value::undefined();
}

// --- The HTMLElement constructor ------------------------------------------------------------

Native construct_html_element(js::Interpreter& interp, Args, js::Object* new_target)
{
    Realm::Internals& in = internals_of(interp);
    CustomElementDefinition* const definition = definition_for_constructor(in, new_target);
    if (definition == nullptr) {
        // HTMLElement itself is not constructible; only a class a page has
        // defined reaches this, through its own super().
        return interp.throw_type_error("Illegal constructor");
    }
    std::optional<js::Value> const prototype = interp.get(*new_target, interp.key("prototype"));
    if (!prototype)
        return std::nullopt;
    js::Object* const prototype_object = prototype->is_object() ? prototype->as_object() : in.prototype("HTMLElement");

    if (!definition->construction_stack.empty()) {
        // An upgrade: the element already in the tree is what the class gets.
        dom::Element* const element = definition->construction_stack.back();
        if (element == nullptr)
            return interp.throw_type_error("This custom element has already been constructed.");
        definition->construction_stack.back() = nullptr;
        NodeWrapper& wrapper = wrapper_for(in, *element);
        wrapper.set_prototype(prototype_object);
        return js::Value::object(&wrapper);
    }

    // Called by the page, or by createElement: a new element of the
    // definition's name, outside any tree until something inserts it.
    dom::Element* const made = in.document->create<dom::Element>(std::string(dom::ns::html), definition->name);
    NodeWrapper& wrapper = wrapper_for(in, *made);
    wrapper.set_prototype(prototype_object);
    wrapper.custom_definition = definition;
    return js::Value::object(&wrapper);
}

}

// --- What the rest of the engine calls ---------------------------------------------------

CustomElementDefinition* custom_element_definition(Realm::Internals& in, std::string_view local_name)
{
    if (local_name.find('-') == std::string_view::npos)
        return nullptr;
    for (auto const& definition : in.custom_element_definitions) {
        if (definition->name == local_name)
            return definition.get();
    }
    return nullptr;
}

void upgrade_custom_element(Realm::Internals& in, dom::Element& element)
{
    CustomElementDefinition* const definition = custom_element_definition(in, element.local_name());
    if (definition == nullptr || !element.is_html())
        return;
    NodeWrapper& wrapper = wrapper_for(in, element);
    if (wrapper.custom_definition != nullptr || wrapper.custom_failed)
        return;
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(js::Value::object(&wrapper));
    interpreter.root(definition->constructor);

    definition->construction_stack.push_back(&element);
    std::optional<js::Value> const result = interpreter.construct(definition->constructor, {}, definition->constructor.as_object());
    definition->construction_stack.pop_back();
    if (!result) {
        ++in.stats.custom_elements_failed;
        wrapper.custom_failed = true;
        in.report_uncaught(interpreter.take_exception(), "custom element constructor");
        return;
    }
    if (!result->is_object() || result->as_object() != &wrapper) {
        ++in.stats.custom_elements_failed;
        wrapper.custom_failed = true;
        in.console("error", "the constructor of <" + definition->name + "> did not answer with the element it was given");
        return;
    }
    ++in.stats.custom_elements_upgraded;
    trace("upgraded <" + definition->name + ">");
    wrapper.custom_definition = definition;

    // What it missed while it was an ordinary element: the attributes it
    // watches, and the document it is already in. The attributes are read
    // out first: a callback may set one, and setting one grows the
    // element's attribute list under a loop over it.
    std::vector<std::pair<std::string, std::string>> watched;
    for (dom::Attr const& attribute : element.attributes()) {
        if (attribute.namespace_uri.empty())
            watched.emplace_back(attribute.local_name, attribute.value);
    }
    for (auto const& [name, value] : watched)
        fire_attribute_changed(in, wrapper, *definition, name, std::nullopt, value);
    if (element.is_connected())
        fire_connected(in, wrapper, *definition);
}

void custom_elements_inserted(Realm::Internals& in, dom::Node& subtree)
{
    if (in.custom_element_definitions.empty())
        return;
    std::vector<dom::Element*> elements;
    collect_elements(subtree, elements);
    for (dom::Element* const element : elements) {
        if (!element->is_connected())
            continue;
        NodeWrapper* const wrapper = existing_wrapper(*element);
        if (wrapper != nullptr && wrapper->custom_definition != nullptr) {
            fire_connected(in, *wrapper, *wrapper->custom_definition);
            continue;
        }
        // One whose definition arrived while it was out of the tree.
        if (custom_element_definition(in, element->local_name()) != nullptr)
            upgrade_custom_element(in, *element);
    }
}

void custom_elements_removed(Realm::Internals& in, dom::Node& subtree)
{
    if (in.custom_element_definitions.empty())
        return;
    std::vector<dom::Element*> elements;
    collect_elements(subtree, elements);
    for (dom::Element* const element : elements) {
        NodeWrapper* const wrapper = existing_wrapper(*element);
        if (wrapper != nullptr && wrapper->custom_definition != nullptr)
            fire_disconnected(in, *wrapper, *wrapper->custom_definition);
    }
}

void custom_elements_cloned(Realm::Internals& in, dom::Node& subtree)
{
    // Only this document's registry knows the names: a clone in a template's
    // contents, whose document has no window, is left as it is.
    if (in.custom_element_definitions.empty() || &subtree.document() != in.document)
        return;
    std::vector<dom::Element*> elements;
    collect_elements(subtree, elements);
    // The clone is held while the constructors run: one may take a node out
    // of it, and nothing else would keep the rest.
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(in.wrap(subtree)));
    for (dom::Element* const element : elements) {
        if (custom_element_definition(in, element->local_name()) != nullptr)
            upgrade_custom_element(in, *element);
    }
}

void custom_element_attribute_changed(Realm::Internals& in, dom::Element& element, std::string_view name,
    std::optional<std::string> const& old_value)
{
    NodeWrapper* const wrapper = existing_wrapper(element);
    if (wrapper == nullptr || wrapper->custom_definition == nullptr)
        return;
    dom::Attr const* const attribute = element.find_attribute(name);
    fire_attribute_changed(in, *wrapper, *wrapper->custom_definition, name, old_value,
        attribute != nullptr ? std::optional<std::string>(attribute->value) : std::nullopt);
}

Native construct_custom_element(Realm::Internals& in, std::string_view local_name)
{
    CustomElementDefinition* const definition = custom_element_definition(in, local_name);
    if (definition == nullptr)
        return js::Value::undefined();
    js::Interpreter& interpreter = in.interpreter;
    js::Interpreter::Roots const roots(interpreter);
    interpreter.root(definition->constructor);
    std::optional<js::Value> const made = interpreter.construct(definition->constructor, {}, definition->constructor.as_object());
    if (!made)
        return std::nullopt;
    // A class that answered with something other than an element of its own
    // name is not one this document can hold.
    NodeWrapper* const wrapper = in.wrapper_of(*made);
    if (wrapper == nullptr || !wrapper->node().is_element()
        || static_cast<dom::Element&>(wrapper->node()).local_name() != definition->name) {
        return interpreter.throw_type_error("The constructor of <" + definition->name + "> did not answer with an element of that name.");
    }
    return *made;
}

void trace_custom_elements(Realm::Internals const& in, js::Tracer& tracer)
{
    for (auto const& definition : in.custom_element_definitions) {
        tracer.visit(definition->constructor);
        tracer.visit(definition->connected);
        tracer.visit(definition->disconnected);
        tracer.visit(definition->adopted);
        tracer.visit(definition->attribute_changed);
    }
}

void install_custom_elements(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const no_collect(interpreter.heap());

    // HTMLElement is made constructible, keeping the prototype object the
    // element interfaces already hang from, so that a page's class extends
    // the same HTMLElement everything else is an instance of.
    js::Object* const html_element_prototype = in.prototype("HTMLElement");
    if (html_element_prototype != nullptr) {
        js::NativeFunction* constructor = interpreter.new_native("HTMLElement", 0,
            [](js::Interpreter& interp, js::Value const&, Args) -> Native {
                return interp.throw_type_error("Failed to construct 'HTMLElement': Please use the 'new' operator");
            },
            construct_html_element);
        constructor->put(interpreter.key("prototype"), js::Value::object(html_element_prototype), js::frozen_attributes);
        html_element_prototype->put(interpreter.key("constructor"), js::Value::object(constructor), js::builtin_attributes);
        interpreter.global()->put(interpreter.key("HTMLElement"), js::Value::object(constructor), js::builtin_attributes);
    }

    js::Object* const registry = define_interface(in, "CustomElementRegistry", nullptr);
    define_operation(interpreter, *registry, "define", 2, define);
    define_operation(interpreter, *registry, "get", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        CustomElementDefinition* const definition = custom_element_definition(internals, *name);
        return definition != nullptr ? definition->constructor : js::Value::undefined();
    });
    define_operation(interpreter, *registry, "getName", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Value const given = js::argument(args, 0);
        CustomElementDefinition* const definition
            = given.is_object() ? definition_for_constructor(internals, given.as_object()) : nullptr;
        return definition != nullptr ? internals.string(definition->name) : js::Value::null();
    });
    define_operation(interpreter, *registry, "upgrade", 1, [](js::Interpreter& interp, js::Value const& , Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<dom::Node*> const root = this_node(interp, js::argument(args, 0));
        if (!root)
            return std::nullopt;
        std::vector<dom::Element*> elements;
        collect_elements(**root, elements);
        for (dom::Element* const element : elements)
            upgrade_custom_element(internals, *element);
        return js::Value::undefined();
    });
    // whenDefined(): settled as the definition arrives, and at once for one
    // already made.
    define_operation(interpreter, *registry, "whenDefined", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::optional<std::string> const name = internals.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        if (!is_valid_custom_element_name(*name)) {
            return rejected_promise(interp,
                dom_exception_value(internals, "SyntaxError", "\"" + *name + "\" is not a valid custom element name."));
        }
        CustomElementDefinition* const definition = custom_element_definition(internals, *name);
        if (definition != nullptr)
            return resolved_promise(interp, definition->constructor);
        return pending_promise(interp);
    });

    js::Object* const registry_object = interpreter.heap().allocate<PlainPlatformObject>(registry);
    interpreter.global()->put(interpreter.key("customElements"), js::Value::object(registry_object), js::builtin_attributes);
}

}
