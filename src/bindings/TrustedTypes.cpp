#include "bindings/Internal.h"

// Trusted Types (W3C, the API): window.trustedTypes — and a worker's — is
// a TrustedTypePolicyFactory; a policy made with createPolicy turns
// strings into TrustedHTML, TrustedScript and TrustedScriptURL through the
// rules the page gave it; the factory says which attributes and
// properties take which type, and tells a trusted value from a string.
// Under `require-trusted-types-for 'script'` the attribute sinks are
// enforced (setAttribute, setAttributeNS, an Attr given to an element, an
// Attr's value): a plain string goes through the default policy or is
// refused. The other sinks still take a string, and take a trusted value
// through its toString, as every sink does in a browser.

#include "js/Runtime.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

namespace {

enum class TrustedKind : std::uint8_t { Html, Script, ScriptUrl };

std::string_view kind_name(TrustedKind kind)
{
    switch (kind) {
    case TrustedKind::Html: return "TrustedHTML";
    case TrustedKind::Script: return "TrustedScript";
    case TrustedKind::ScriptUrl: return "TrustedScriptURL";
    }
    return "TrustedHTML";
}

// A trusted value: the string a policy made, which cannot be made any
// other way.
class TrustedValueObject final : public js::Object {
public:
    TrustedValueObject(js::Object* prototype, js::RealmRecord* realm, TrustedKind the_kind, std::string the_data)
        : Object(prototype, Class::Host)
        , kind(the_kind)
        , data(std::move(the_data))
        , m_realm(realm)
    {
    }
    TrustedKind kind;
    std::string data;
    js::RealmRecord* home_realm() const override { return m_realm; }
    std::size_t size_in_bytes() const override { return sizeof(*this) + data.capacity(); }

private:
    js::RealmRecord* m_realm;
};

// A policy: its name and the three rules, any of which may be missing.
class TrustedPolicyObject final : public js::Object {
public:
    TrustedPolicyObject(js::Object* prototype, js::RealmRecord* realm, std::string the_name)
        : Object(prototype, Class::Host)
        , name(std::move(the_name))
        , m_realm(realm)
    {
    }
    std::string name;
    js::Value create_html; // undefined for no rule
    js::Value create_script;
    js::Value create_script_url;
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(create_html);
        tracer.visit(create_script);
        tracer.visit(create_script_url);
    }

private:
    js::RealmRecord* m_realm;
};

// The factory: the policies made (by name, for the directive's duplicate
// rule when it is written), the default policy, the two empty values.
class TrustedTypeFactoryObject final : public js::Object {
public:
    TrustedTypeFactoryObject(js::Object* prototype, js::RealmRecord* realm)
        : Object(prototype, Class::Host)
        , m_realm(realm)
    {
    }
    std::vector<std::string> names;
    js::Value default_policy; // undefined for none
    js::Value empty_html;
    js::Value empty_script;
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(default_policy);
        tracer.visit(empty_html);
        tracer.visit(empty_script);
    }

private:
    js::RealmRecord* m_realm;
};

std::optional<TrustedTypeFactoryObject*> this_factory(js::Interpreter& interp, js::Value const& this_value)
{
    auto* found = this_value.is_object() ? dynamic_cast<TrustedTypeFactoryObject*>(this_value.as_object()) : nullptr;
    if (found == nullptr) {
        interp.throw_type_error("Illegal invocation");
        return std::nullopt;
    }
    return found;
}

std::optional<TrustedPolicyObject*> this_policy(js::Interpreter& interp, js::Value const& this_value)
{
    auto* found = this_value.is_object() ? dynamic_cast<TrustedPolicyObject*>(this_value.as_object()) : nullptr;
    if (found == nullptr) {
        interp.throw_type_error("Illegal invocation");
        return std::nullopt;
    }
    return found;
}

TrustedValueObject* new_trusted_value(Realm::Internals& in, TrustedKind kind, std::string data)
{
    return in.interpreter.heap().allocate<TrustedValueObject>(in.prototype(kind_name(kind)), in.realm_record, kind, std::move(data));
}

// "Create a Trusted Type" (§4.1): the rule for the kind, called with the
// input as a string and the rest of the arguments; nothing from it is the
// empty string; no rule is a TypeError.
Native create_trusted(js::Interpreter& interp, js::Value const& this_value, Args args, TrustedKind kind)
{
    std::optional<TrustedPolicyObject*> const found = this_policy(interp, this_value);
    if (!found)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    TrustedPolicyObject& policy = **found;
    js::Value const rule = kind == TrustedKind::Html ? policy.create_html : kind == TrustedKind::Script ? policy.create_script : policy.create_script_url;
    std::string_view const rule_name = kind == TrustedKind::Html ? "createHTML" : kind == TrustedKind::Script ? "createScript" : "createScriptURL";
    if (!js::Interpreter::is_callable(rule))
        return interp.throw_type_error("Failed to execute '" + std::string(rule_name) + "' on 'TrustedTypePolicy': Policy " + policy.name + "'s " + std::string(kind_name(kind)) + " constructor has not been defined.");
    js::Interpreter::Roots const roots(interp);
    interp.root(this_value);
    std::optional<std::string> const input = in.to_utf8(js::argument(args, 0));
    if (!input)
        return std::nullopt;
    std::vector<js::Value> arguments;
    arguments.push_back(in.string(*input));
    interp.root(arguments.front());
    for (std::size_t i = 1; i < args.size(); ++i) {
        arguments.push_back(args[i]);
        interp.root(args[i]);
    }
    std::optional<js::Value> const result = interp.call(rule, js::Value::undefined(), arguments);
    if (!result)
        return std::nullopt;
    std::string data;
    if (!result->is_nullish()) {
        interp.root(*result);
        std::optional<std::string> const text = in.to_utf8(*result);
        if (!text)
            return std::nullopt;
        data = *text;
    }
    return js::Value::object(new_trusted_value(in, kind, std::move(data)));
}

// The attributes and properties that take a trusted type (§3.4, the
// tables): an element in the HTML namespace is matched by its lowercased
// name; an event handler content attribute of any element takes a
// TrustedScript.
bool is_html_namespace(std::string_view ns)
{
    return ns.empty() || ns == "http://www.w3.org/1999/xhtml";
}

// An event handler content attribute (HTML §8.1.8.2): "on" and the name of
// one of the handlers every element, or the window through the body, has,
// or the two a media element adds (Encrypted Media Extensions). An element
// of no namespace those handlers belong to has none.
bool is_event_handler_attribute(std::string_view element_ns, std::string const& attribute)
{
    if (!is_html_namespace(element_ns) && element_ns != dom::ns::svg && element_ns != dom::ns::mathml)
        return false;
    if (attribute.size() <= 2 || !attribute.starts_with("on"))
        return false;
    std::string_view const type = std::string_view(attribute).substr(2);
    if (type == "encrypted" || type == "waitingforkey")
        return true;
    for (std::span<std::string_view const> const names : { global_event_handler_types(), window_event_handler_types() }) {
        if (std::find(names.begin(), names.end(), type) != names.end())
            return true;
    }
    return false;
}

// The table of §4.3.4 "Get Trusted Type data for attribute". (The embed's
// src and the object's data and codebase left it: w3c/trusted-types#554.)
std::optional<std::string_view> attribute_type_of(std::string_view element_ns, std::string const& tag, std::string const& attribute, std::string_view attribute_ns)
{
    if (element_ns == dom::ns::svg && tag == "script" && attribute == "href" && (attribute_ns.empty() || attribute_ns == dom::ns::xlink))
        return "TrustedScriptURL";
    if (!attribute_ns.empty())
        return std::nullopt;
    if (is_event_handler_attribute(element_ns, attribute))
        return "TrustedScript";
    if (!is_html_namespace(element_ns))
        return std::nullopt;
    if (tag == "iframe" && attribute == "srcdoc")
        return "TrustedHTML";
    if (tag == "script" && attribute == "src")
        return "TrustedScriptURL";
    return std::nullopt;
}

std::optional<std::string_view> property_type_of(std::string_view element_ns, std::string const& tag, std::string const& property)
{
    if (property == "innerHTML" || property == "outerHTML")
        return "TrustedHTML";
    if (!is_html_namespace(element_ns))
        return std::nullopt;
    if (tag == "iframe" && property == "srcdoc")
        return "TrustedHTML";
    if (tag == "script" && (property == "innerText" || property == "text" || property == "textContent"))
        return "TrustedScript";
    if (tag == "script" && property == "src")
        return "TrustedScriptURL";
    return std::nullopt;
}

// A nullable namespace argument: undefined or null is the empty string
// (the HTML namespace for an element, no namespace for an attribute).
std::optional<std::string> namespace_argument(Realm::Internals& in, js::Value const& value)
{
    if (value.is_nullish())
        return std::string();
    return in.to_utf8(value);
}

void install_trusted_value(Realm::Internals& in, TrustedKind kind)
{
    js::Object* prototype = define_interface(in, kind_name(kind), nullptr);
    for (std::string_view const name : { "toString", "toJSON" }) {
        define_operation(in.interpreter, *prototype, name, 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            auto* found = this_value.is_object() ? dynamic_cast<TrustedValueObject*>(this_value.as_object()) : nullptr;
            if (found == nullptr)
                return interp.throw_type_error("Illegal invocation");
            return internals_of(interp).string(found->data);
        });
    }
}

void install_policy(Realm::Internals& in)
{
    js::Object* policy = define_interface(in, "TrustedTypePolicy", nullptr);
    define_getter(in, *policy, "name", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TrustedPolicyObject*> const found = this_policy(interp, this_value);
        return found ? Native(internals_of(interp).string((*found)->name)) : std::nullopt;
    });
    define_operation(in.interpreter, *policy, "createHTML", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return create_trusted(interp, this_value, args, TrustedKind::Html);
    });
    define_operation(in.interpreter, *policy, "createScript", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return create_trusted(interp, this_value, args, TrustedKind::Script);
    });
    define_operation(in.interpreter, *policy, "createScriptURL", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        return create_trusted(interp, this_value, args, TrustedKind::ScriptUrl);
    });
}

void install_factory(Realm::Internals& outer)
{
    js::Interpreter& interpreter = outer.interpreter;
    js::Object* factory = define_interface(outer, "TrustedTypePolicyFactory", nullptr);
    // createPolicy(name, options): the rules read from the dictionary
    // (§4.2 "Create a Trusted Type Policy"); a second default policy is a
    // TypeError. The `trusted-types` directive's name list and its
    // duplicate rule are not read yet: every name is allowed.
    define_operation(interpreter, *factory, "createPolicy", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<TrustedTypeFactoryObject*> const found = this_factory(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& in = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<std::string> const name = in.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        js::Value const options = js::argument(args, 1);
        interp.root(options);
        if (!options.is_undefined() && !options.is_object())
            return interp.throw_type_error("Failed to execute 'createPolicy' on 'TrustedTypePolicyFactory': parameter 2 is not of type 'TrustedTypePolicyOptions'.");
        if (*name == "default" && !(*found)->default_policy.is_undefined())
            return interp.throw_type_error("Failed to execute 'createPolicy' on 'TrustedTypePolicyFactory': Policy with name \"default\" already exists.");
        js::Value rules[3] = { js::Value::undefined(), js::Value::undefined(), js::Value::undefined() };
        std::string_view const rule_names[3] = { "createHTML", "createScript", "createScriptURL" };
        if (options.is_object()) {
            for (std::size_t i = 0; i < 3; ++i) {
                std::optional<js::Value> const rule = interp.get(options, rule_names[i]);
                if (!rule)
                    return std::nullopt;
                if (rule->is_nullish())
                    continue;
                if (!js::Interpreter::is_callable(*rule))
                    return interp.throw_type_error("Failed to execute 'createPolicy' on 'TrustedTypePolicyFactory': Failed to read the '" + std::string(rule_names[i]) + "' property from 'TrustedTypePolicyOptions': The provided value is not of type 'Function'.");
                rules[i] = *rule;
                interp.root(*rule);
            }
        }
        auto* policy = interp.heap().allocate<TrustedPolicyObject>(in.prototype("TrustedTypePolicy"), in.realm_record, *name);
        policy->create_html = rules[0];
        policy->create_script = rules[1];
        policy->create_script_url = rules[2];
        (*found)->names.push_back(*name);
        if (*name == "default")
            (*found)->default_policy = js::Value::object(policy);
        return js::Value::object(policy);
    });
    struct Check {
        std::string_view name;
        TrustedKind kind;
    };
    for (Check const check : { Check { "isHTML", TrustedKind::Html }, Check { "isScript", TrustedKind::Script }, Check { "isScriptURL", TrustedKind::ScriptUrl } }) {
        define_operation(interpreter, *factory, check.name, 1, [kind = check.kind](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            if (!this_factory(interp, this_value))
                return std::nullopt;
            js::Value const value = js::argument(args, 0);
            auto* trusted = value.is_object() ? dynamic_cast<TrustedValueObject*>(value.as_object()) : nullptr;
            return js::Value::boolean(trusted != nullptr && trusted->kind == kind);
        });
    }
    define_getter(outer, *factory, "emptyHTML", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TrustedTypeFactoryObject*> const found = this_factory(interp, this_value);
        if (!found)
            return std::nullopt;
        if ((*found)->empty_html.is_undefined())
            (*found)->empty_html = js::Value::object(new_trusted_value(internals_of(interp), TrustedKind::Html, ""));
        return (*found)->empty_html;
    });
    define_getter(outer, *factory, "emptyScript", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TrustedTypeFactoryObject*> const found = this_factory(interp, this_value);
        if (!found)
            return std::nullopt;
        if ((*found)->empty_script.is_undefined())
            (*found)->empty_script = js::Value::object(new_trusted_value(internals_of(interp), TrustedKind::Script, ""));
        return (*found)->empty_script;
    });
    define_getter(outer, *factory, "defaultPolicy", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<TrustedTypeFactoryObject*> const found = this_factory(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->default_policy.is_undefined() ? js::Value::null() : (*found)->default_policy;
    });
    define_operation(interpreter, *factory, "getAttributeType", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        if (!this_factory(interp, this_value))
            return std::nullopt;
        Realm::Internals& in = internals_of(interp);
        std::optional<std::string> const tag = in.to_utf8(js::argument(args, 0));
        std::optional<std::string> const attribute = in.to_utf8(js::argument(args, 1));
        std::optional<std::string> const element_ns = namespace_argument(in, js::argument(args, 2));
        std::optional<std::string> const attribute_ns = namespace_argument(in, js::argument(args, 3));
        if (!tag || !attribute || !element_ns || !attribute_ns)
            return std::nullopt;
        std::string const lowered_tag = is_html_namespace(*element_ns) ? ascii_lower(*tag) : *tag;
        std::string const lowered_attribute = attribute_ns->empty() ? ascii_lower(*attribute) : *attribute;
        std::optional<std::string_view> const type = attribute_type_of(*element_ns, lowered_tag, lowered_attribute, *attribute_ns);
        return type ? in.string(*type) : js::Value::null();
    });
    define_operation(interpreter, *factory, "getPropertyType", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        if (!this_factory(interp, this_value))
            return std::nullopt;
        Realm::Internals& in = internals_of(interp);
        std::optional<std::string> const tag = in.to_utf8(js::argument(args, 0));
        std::optional<std::string> const property = in.to_utf8(js::argument(args, 1));
        std::optional<std::string> const element_ns = namespace_argument(in, js::argument(args, 2));
        if (!tag || !property || !element_ns)
            return std::nullopt;
        std::string const lowered_tag = is_html_namespace(*element_ns) ? ascii_lower(*tag) : *tag;
        std::optional<std::string_view> const type = property_type_of(*element_ns, lowered_tag, *property);
        return type ? in.string(*type) : js::Value::null();
    });
}

} // namespace

std::optional<std::string> trusted_script_code(js::Value const& value)
{
    auto* trusted = value.is_object() ? dynamic_cast<TrustedValueObject*>(value.as_object()) : nullptr;
    if (trusted == nullptr || trusted->kind != TrustedKind::Script)
        return std::nullopt;
    return trusted->data;
}

namespace {

TrustedTypeFactoryObject& factory_for(Realm::Internals& in)
{
    return *static_cast<TrustedTypeFactoryObject*>(same_object(in, "trustedTypes", [&] {
        auto* made = in.interpreter.heap().allocate<TrustedTypeFactoryObject>(in.prototype("TrustedTypePolicyFactory"), in.realm_record);
        made->default_policy = js::Value::undefined();
        return made;
    }));
}

// The sink an attribute's data names (§4.1, the attribute table).
std::string attribute_sink(std::string_view element_ns, std::string const& tag, std::string const& attribute)
{
    if (attribute.size() > 2 && attribute[0] == 'o' && attribute[1] == 'n')
        return "Element " + attribute;
    if (element_ns == dom::ns::svg)
        return "SVGScriptElement href";
    if (tag == "iframe")
        return "HTMLIFrameElement srcdoc";
    return "HTMLScriptElement src";
}

// "Get Trusted Type compliant string" (§4.3.1) for a value that is not of
// the type: as given when no policy requires Trusted Types for scripts;
// else what the default policy makes of it, or, with none or nothing from
// it, a TypeError under an enforced policy and the input under one that
// only reports.
std::optional<std::string> compliant_string(Realm::Internals& in, TrustedKind kind, std::string input, std::string_view sink)
{
    bool enforced = false;
    bool reported = false;
    if (in.hooks.policy != nullptr) {
        for (net::ContentSecurityPolicy::Policy const& policy : in.hooks.policy->policies()) {
            net::ContentSecurityPolicy::Directive const* const directive = policy.find("require-trusted-types-for");
            if (directive == nullptr)
                continue;
            bool const script = std::any_of(directive->values.begin(), directive->values.end(),
                [](std::string const& value) { return ascii_lower(value) == "'script'"; });
            if (script)
                (policy.report_only ? reported : enforced) = true;
        }
    }
    if (!enforced && !reported)
        return input;
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    js::Value const policy_value = interp.root(factory_for(in).default_policy);
    if (auto* const policy = policy_value.is_object() ? dynamic_cast<TrustedPolicyObject*>(policy_value.as_object()) : nullptr) {
        js::Value const rule = kind == TrustedKind::Html ? policy->create_html : kind == TrustedKind::Script ? policy->create_script : policy->create_script_url;
        if (js::Interpreter::is_callable(rule)) {
            interp.root(rule);
            std::vector<js::Value> const arguments { interp.root(in.string(input)), interp.root(in.string(kind_name(kind))),
                interp.root(in.string(sink)) };
            std::optional<js::Value> const result = interp.call(rule, js::Value::undefined(), arguments);
            if (!result)
                return std::nullopt;
            if (!result->is_nullish()) {
                interp.root(*result);
                return in.to_utf8(*result);
            }
        }
    }
    if (enforced) {
        interp.throw_type_error("Failed to set '" + std::string(sink) + "': This document requires '" + std::string(kind_name(kind)) + "' assignment.");
        return std::nullopt;
    }
    return input;
}

}

bool is_trusted_value(js::Value const& value)
{
    return value.is_object() && dynamic_cast<TrustedValueObject*>(value.as_object()) != nullptr;
}

std::optional<std::string> trusted_attribute_value(Realm::Internals& in, dom::Element const& element, std::string_view attribute_ns,
    std::string_view local_name, js::Value const& value)
{
    std::string const& element_ns = element.namespace_uri();
    std::string const tag = is_html_namespace(element_ns) ? ascii_lower(element.local_name()) : element.local_name();
    std::string const attribute(local_name);
    std::optional<std::string_view> const type = attribute_type_of(element_ns, tag, attribute, attribute_ns);
    if (!type)
        return in.to_utf8(value);
    TrustedKind const kind = *type == "TrustedHTML" ? TrustedKind::Html : *type == "TrustedScript" ? TrustedKind::Script : TrustedKind::ScriptUrl;
    if (auto* const trusted = value.is_object() ? dynamic_cast<TrustedValueObject*>(value.as_object()) : nullptr) {
        if (trusted->kind == kind)
            return trusted->data;
    }
    std::optional<std::string> input = in.to_utf8(value);
    if (!input)
        return std::nullopt;
    return compliant_string(in, kind, std::move(*input), attribute_sink(element_ns, tag, attribute));
}

void install_trusted_types(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    install_trusted_value(in, TrustedKind::Html);
    install_trusted_value(in, TrustedKind::Script);
    install_trusted_value(in, TrustedKind::ScriptUrl);
    install_policy(in);
    install_factory(in);
    // The global's trustedTypes (WindowOrWorkerGlobalScope): the same
    // factory every read.
    define_attribute(interpreter, *interpreter.global(), "trustedTypes", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(same_object(internals, "trustedTypes", [&] {
            auto* made = internals.interpreter.heap().allocate<TrustedTypeFactoryObject>(internals.prototype("TrustedTypePolicyFactory"), internals.realm_record);
            made->default_policy = js::Value::undefined();
            return made;
        }));
    });
}

}
