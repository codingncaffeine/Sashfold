#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"
#include "dom/Forms.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ElementInternals (HTML §4.13.7): what a custom element's class holds to
// take part in the platform as the built-in elements do. attachInternals()
// hands it out once, to the class; through it a form-associated element
// (one whose class says `static formAssociated = true`) gives its form a
// value, says whether it is valid, and learns its form, its labels and
// whether it is disabled — and any custom element keeps custom states that
// :state() matches, and says its role and ARIA properties.
//
// The component libraries build their form controls on it: a design
// system's text field calls attachInternals() in its constructor, and an
// engine without it fails every such constructor, which leaves the field
// an empty tag.

namespace sashfold::bindings {

namespace {

// ARIAMixin's string attributes (ARIA §10.1), which ElementInternals
// reflects as values of its own: the default semantics the element's
// attributes override.
constexpr std::string_view aria_names[] = { "role", "ariaAtomic", "ariaAutoComplete", "ariaBrailleLabel", "ariaBrailleRoleDescription",
    "ariaBusy", "ariaChecked", "ariaColCount", "ariaColIndex", "ariaColIndexText", "ariaColSpan", "ariaCurrent", "ariaDescription",
    "ariaDisabled", "ariaExpanded", "ariaHasPopup", "ariaHidden", "ariaInvalid", "ariaKeyShortcuts", "ariaLabel", "ariaLevel", "ariaLive",
    "ariaModal", "ariaMultiLine", "ariaMultiSelectable", "ariaOrientation", "ariaPlaceholder", "ariaPosInSet", "ariaPressed",
    "ariaReadOnly", "ariaRelevant", "ariaRequired", "ariaRoleDescription", "ariaRowCount", "ariaRowIndex", "ariaRowIndexText",
    "ariaRowSpan", "ariaSelected", "ariaSetSize", "ariaSort", "ariaValueMax", "ariaValueMin", "ariaValueNow", "ariaValueText" };

// The key the element's wrapper keeps its internals under.
constexpr std::string_view internals_key = "[[attachedInternals]]";

class ElementInternalsObject final : public js::Object {
public:
    ElementInternalsObject(js::Object* prototype, NodeWrapper& the_target)
        : Object(prototype, Class::Host)
        , target(&the_target)
    {
    }
    // Held: the internals keep their element as long as a page keeps them.
    NodeWrapper* target;
    // setFormValue's submission value: none (null), one value submitted
    // under the element's name — `entries` holds it, nameless — or a list
    // of entries named by themselves.
    bool has_value = false;
    bool value_is_list = false;
    std::vector<FormEntry> entries;
    std::array<std::optional<std::string>, std::size(aria_names)> aria;
    js::Object* states = nullptr;
    js::Object* validity = nullptr;

    dom::Element* element() const { return target->detached() ? nullptr : static_cast<dom::Element*>(&target->node()); }
    js::RealmRecord* home_realm() const override { return target->home_realm(); }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(target);
        for (FormEntry const& entry : entries)
            tracer.visit(entry.file);
        tracer.visit(states);
        tracer.visit(validity);
    }
};

// A CustomStateSet: the element's custom states, kept by the document
// (dom::Document::custom_states) where :state() reads them.
class CustomStateSetObject final : public js::Object {
public:
    CustomStateSetObject(js::Object* prototype, NodeWrapper& the_target)
        : Object(prototype, Class::Host)
        , target(&the_target)
    {
    }
    NodeWrapper* target;
    dom::Element* element() const { return target->detached() ? nullptr : static_cast<dom::Element*>(&target->node()); }
    js::RealmRecord* home_realm() const override { return target->home_realm(); }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(target);
    }
};

ElementInternalsObject* internals_this(js::Value const& this_value)
{
    return this_value.is_object() ? dynamic_cast<ElementInternalsObject*>(this_value.as_object()) : nullptr;
}

bool form_associated(dom::Element const& element)
{
    return element.document().form_associated_custom.contains(&element);
}

// The target of `this`, when it is a form-associated custom element; a
// TypeError or a NotSupportedError pending otherwise.
std::optional<dom::Element*> form_target(js::Interpreter& interp, js::Value const& this_value, std::string_view member)
{
    ElementInternalsObject* const self = internals_this(this_value);
    if (self == nullptr) {
        interp.throw_type_error("Illegal invocation");
        return std::nullopt;
    }
    dom::Element* const element = self->element();
    if (element == nullptr || !form_associated(*element)) {
        internals_of(interp).throw_dom_exception("NotSupportedError",
            "Failed to execute '" + std::string(member) + "' on 'ElementInternals': The target element is not a form-associated custom element.");
        return std::nullopt;
    }
    return element;
}

void call_form_callback(Realm::Internals& in, dom::Element& element, js::Value CustomElementDefinition::* callback, js::Value argument,
    bool with_argument, std::string_view where)
{
    NodeWrapper& wrapper = wrapper_for(in, element);
    CustomElementDefinition const* const definition = wrapper.custom_definition;
    if (definition == nullptr)
        return;
    js::Value const& function = definition->*callback;
    if (!function.is_object() || !function.as_object()->is_callable())
        return;
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&wrapper));
    in.interpreter.root(argument);
    js::Value const arguments[] = { argument };
    in.call_reporting(function, js::Value::object(&wrapper), with_argument ? Args(arguments) : Args(), where);
}

// Tells one element what changed since it was last told: its form, then
// whether it is disabled.
void check_one_element(Realm::Internals& in, dom::Element& element)
{
    auto const found = in.form_associated.find(&element);
    if (found == in.form_associated.end())
        return;
    dom::Element* const owner = form_owner_of(in, element);
    bool const disabled = dom::forms::actually_disabled(element);
    Realm::Internals::FormAssociatedState const was = found->second;
    found->second = { owner, disabled };
    if (owner != was.owner) {
        call_form_callback(in, element, &CustomElementDefinition::form_associated_callback,
            owner != nullptr ? js::Value::object(in.wrap(*owner)) : js::Value::null(), true, "formAssociatedCallback");
    }
    if (disabled != was.disabled)
        call_form_callback(in, element, &CustomElementDefinition::form_disabled, js::Value::boolean(disabled), true, "formDisabledCallback");
}

// The states changed: :state() reads them, in the element, below it, and
// in its shadow tree (:host(:state(x)) is how a component styles itself).
void states_changed(Realm::Internals& in, dom::Element& element)
{
    element.mark_style_subtree();
    if (dom::ShadowRoot* const shadow = element.shadow_root())
        shadow->mark_style_subtree();
    element.document().note_state_change();
    in.realm.note_mutation();
}

CustomStateSetObject* states_this(js::Interpreter& interp, js::Value const& this_value)
{
    auto* const self = this_value.is_object() ? dynamic_cast<CustomStateSetObject*>(this_value.as_object()) : nullptr;
    if (self == nullptr)
        interp.throw_type_error("Illegal invocation");
    return self;
}

// The set's states as script sees them, for its iterators and forEach.
std::vector<std::string> states_of(CustomStateSetObject const& set)
{
    dom::Element const* const element = set.element();
    if (element == nullptr)
        return {};
    auto const found = element->document().custom_states.find(element);
    return found != element->document().custom_states.end() ? found->second : std::vector<std::string> {};
}

// An iterator over a CustomStateSet: where it is in the set's order, and
// the state it handed out last, so that it follows the set as it changes.
class CustomStateSetIteratorObject final : public js::Object {
public:
    CustomStateSetIteratorObject(js::Object* prototype, CustomStateSetObject& the_set, bool the_pairs)
        : Object(prototype, Class::Host)
        , set(&the_set)
        , pairs(the_pairs)
    {
    }
    CustomStateSetObject* set; // null once it is done
    bool pairs;
    std::size_t position = 0;
    std::optional<std::string> last;
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(set);
    }
};

Native states_iterator(js::Interpreter& interp, js::Value const& this_value, bool pairs)
{
    CustomStateSetObject* const self = states_this(interp, this_value);
    if (self == nullptr)
        return std::nullopt;
    return js::Value::object(
        interp.heap().allocate<CustomStateSetIteratorObject>(internals_of(interp).prototype("CustomStateSet Iterator"), *self, pairs));
}

void install_custom_state_set(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* const set = define_interface(in, "CustomStateSet", nullptr);
    define_getter(in, *set, "size", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        CustomStateSetObject* const self = states_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        return js::Value::number(static_cast<double>(states_of(*self).size()));
    });
    define_operation(interpreter, *set, "add", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        CustomStateSetObject* const self = states_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        std::optional<std::string> const state = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!state)
            return std::nullopt;
        if (dom::Element* const element = self->element()) {
            std::vector<std::string>& states = element->document().custom_states[element];
            if (std::find(states.begin(), states.end(), *state) == states.end()) {
                states.push_back(*state);
                states_changed(internals_of(interp), *element);
            }
        }
        return this_value;
    });
    define_operation(interpreter, *set, "delete", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        CustomStateSetObject* const self = states_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        std::optional<std::string> const state = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!state)
            return std::nullopt;
        dom::Element* const element = self->element();
        if (element == nullptr)
            return js::Value::boolean(false);
        auto const found = element->document().custom_states.find(element);
        if (found == element->document().custom_states.end())
            return js::Value::boolean(false);
        auto const at = std::find(found->second.begin(), found->second.end(), *state);
        if (at == found->second.end())
            return js::Value::boolean(false);
        found->second.erase(at);
        states_changed(internals_of(interp), *element);
        return js::Value::boolean(true);
    });
    define_operation(interpreter, *set, "has", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        CustomStateSetObject* const self = states_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        std::optional<std::string> const state = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!state)
            return std::nullopt;
        std::vector<std::string> const states = states_of(*self);
        return js::Value::boolean(std::find(states.begin(), states.end(), *state) != states.end());
    });
    define_operation(interpreter, *set, "clear", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        CustomStateSetObject* const self = states_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        if (dom::Element* const element = self->element()) {
            if (element->document().custom_states.erase(element) != 0)
                states_changed(internals_of(interp), *element);
        }
        return js::Value::undefined();
    });
    define_operation(interpreter, *set, "forEach", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        CustomStateSetObject* const self = states_this(interp, this_value);
        if (self == nullptr)
            return std::nullopt;
        js::Value const callback = js::argument(args, 0);
        if (!callback.is_object() || !callback.as_object()->is_callable())
            return interp.throw_type_error("Failed to execute 'forEach' on 'CustomStateSet': The callback provided as parameter 1 is not a function.");
        Realm::Internals& internals = internals_of(interp);
        for (std::string const& state : states_of(*self)) {
            js::Interpreter::Roots const roots(interp);
            js::Value const value = interp.root(internals.string(state));
            js::Value const arguments[] = { value, value, this_value };
            if (!interp.call(callback, js::argument(args, 1), arguments))
                return std::nullopt;
        }
        return js::Value::undefined();
    });
    // values, and keys and @@iterator as the same function (WebIDL §3.7.12);
    // entries pairs each state with itself. The iterators are live, as a
    // Set's are: one that has handed out a state that is then deleted goes
    // on from where that state was.
    js::Object* const iterator = interpreter.heap().allocate<js::Object>(interpreter.intrinsics().iterator_prototype);
    in.prototypes["CustomStateSet Iterator"] = iterator;
    iterator->put(js::PropertyKey::symbol(interpreter.atoms().symbol_to_string_tag), in.string("CustomStateSet Iterator"),
        js::Configurable);
    define_operation(interpreter, *iterator, "next", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const self = this_value.is_object() ? dynamic_cast<CustomStateSetIteratorObject*>(this_value.as_object()) : nullptr;
        if (self == nullptr)
            return interp.throw_type_error("Illegal invocation");
        std::vector<std::string> const states = self->set != nullptr ? states_of(*self->set) : std::vector<std::string> {};
        if (self->last) {
            auto const at = std::find(states.begin(), states.end(), *self->last);
            if (at != states.end())
                self->position = static_cast<std::size_t>(at - states.begin()) + 1;
            else if (self->position > 0)
                --self->position;
        }
        if (self->set == nullptr || self->position >= states.size()) {
            self->set = nullptr;
            return js::Value::object(interp.create_iter_result(js::Value::undefined(), true));
        }
        std::string const& state = states[self->position++];
        self->last = state;
        js::Interpreter::Roots const roots(interp);
        js::Value value = interp.root(internals_of(interp).string(state));
        if (self->pairs) {
            js::Value const pair[2] = { value, value };
            value = interp.root(js::Value::object(interp.new_array(pair)));
        }
        return js::Value::object(interp.create_iter_result(value, false));
    });
    for (bool const pairs : { false, true }) {
        js::NativeFunction* const method = define_operation(interpreter, *set, pairs ? "entries" : "values", 0,
            pairs ? +[](js::Interpreter& interp, js::Value const& this_value, Args) -> Native { return states_iterator(interp, this_value, true); }
                  : +[](js::Interpreter& interp, js::Value const& this_value, Args) -> Native { return states_iterator(interp, this_value, false); });
        if (!pairs) {
            set->put(interpreter.key("keys"), js::Value::object(method), js::default_attributes);
            set->put(js::PropertyKey::symbol(interpreter.atoms().symbol_iterator), js::Value::object(method), js::builtin_attributes);
        }
    }
}

// --- ElementInternals ------------------------------------------------------------------

Native internals_aria_get(js::Interpreter& interp, js::Value const& this_value, Args)
{
    ElementInternalsObject* const self = internals_this(this_value);
    if (self == nullptr)
        return interp.throw_type_error("Illegal invocation");
    js::NativeFunction const* const accessor = interp.active_native();
    std::uint32_t const index = accessor != nullptr && accessor->spec() != nullptr ? accessor->spec()->datum : 0;
    std::optional<std::string> const& value = self->aria[index];
    return value ? internals_of(interp).string(*value) : js::Value::null();
}

Native internals_aria_set(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    ElementInternalsObject* const self = internals_this(this_value);
    if (self == nullptr)
        return interp.throw_type_error("Illegal invocation");
    js::NativeFunction const* const accessor = interp.active_native();
    std::uint32_t const index = accessor != nullptr && accessor->spec() != nullptr ? accessor->spec()->datum : 0;
    js::Value const given = js::argument(args, 0);
    if (given.is_null()) {
        self->aria[index].reset();
        return js::Value::undefined();
    }
    std::optional<std::string> const text = internals_of(interp).to_utf8(given);
    if (!text)
        return std::nullopt;
    self->aria[index] = *text;
    return js::Value::undefined();
}

// "\r\n" and a lone "\r" become "\n" (setValidity's message, §4.13.7.2).
std::string normalize_newlines(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

Native set_form_value(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    std::optional<dom::Element*> const element = form_target(interp, this_value, "setFormValue");
    if (!element)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    ElementInternalsObject& self = *internals_this(this_value);
    js::Value const value = js::argument(args, 0);
    dom::Document::InternalsFormValue shown;
    std::vector<FormEntry> entries;
    bool has_value = true;
    bool is_list = false;
    if (std::optional<std::vector<FormEntry>> listed = form_data_entries(value)) {
        entries = std::move(*listed);
        is_list = true;
    } else if (value.is_null() || value.is_undefined()) {
        has_value = false;
    } else if (auto* const file = value.is_object() ? dynamic_cast<BlobObject*>(value.as_object()) : nullptr; file != nullptr && file->is_file) {
        entries.push_back({ {}, {}, file, file->name });
    } else {
        std::optional<std::string> text = in.to_utf8(value);
        if (!text)
            return std::nullopt;
        entries.push_back({ {}, std::move(*text), nullptr, {} });
    }
    self.has_value = has_value;
    self.value_is_list = is_list;
    self.entries = std::move(entries);
    // The host's copy, which a submission from the page's own form reads: a
    // file by its name.
    dom::Document& document = (*element)->document();
    if (!has_value) {
        document.internals_form_values.erase(*element);
    } else {
        shown.is_list = is_list;
        for (FormEntry const& entry : self.entries) {
            std::string text = entry.file != nullptr ? entry.filename : entry.value;
            if (is_list)
                shown.entries.emplace_back(entry.name, std::move(text));
            else
                shown.value = std::move(text);
        }
        document.internals_form_values[*element] = std::move(shown);
    }
    return js::Value::undefined();
}

Native set_validity(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    std::optional<dom::Element*> const element = form_target(interp, this_value, "setValidity");
    if (!element)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    // ValidityStateFlags, its members read in the dictionary's (sorted) order.
    dom::forms::Validity flags;
    js::Value const given = js::argument(args, 0);
    if (!given.is_undefined() && !given.is_null() && !given.is_object())
        return interp.throw_type_error("Failed to execute 'setValidity' on 'ElementInternals': The provided value is not of type 'ValidityStateFlags'.");
    if (given.is_object()) {
        struct Member {
            char const* name;
            bool dom::forms::Validity::* flag;
        };
        static constexpr Member members[] = { { "badInput", &dom::forms::Validity::bad_input },
            { "customError", &dom::forms::Validity::custom_error }, { "patternMismatch", &dom::forms::Validity::pattern_mismatch },
            { "rangeOverflow", &dom::forms::Validity::range_overflow }, { "rangeUnderflow", &dom::forms::Validity::range_underflow },
            { "stepMismatch", &dom::forms::Validity::step_mismatch }, { "tooLong", &dom::forms::Validity::too_long },
            { "tooShort", &dom::forms::Validity::too_short }, { "typeMismatch", &dom::forms::Validity::type_mismatch },
            { "valueMissing", &dom::forms::Validity::value_missing } };
        for (Member const& member : members) {
            std::optional<js::Value> const read = interp.get(*given.as_object(), interp.key(member.name));
            if (!read)
                return std::nullopt;
            flags.*member.flag = !read->is_undefined() && js::Interpreter::to_boolean(*read);
        }
    }
    std::string message;
    js::Value const message_value = js::argument(args, 1);
    if (!message_value.is_undefined()) {
        std::optional<std::string> text = in.to_utf8(message_value);
        if (!text)
            return std::nullopt;
        message = std::move(*text);
    }
    if (!flags.valid() && message.empty()) {
        return interp.throw_type_error(
            "Failed to execute 'setValidity' on 'ElementInternals': The second argument should not be empty if one or more flags in the first argument are true.");
    }
    // The anchor, when given, is an HTML element inside the element's own
    // tree, its shadow trees included, or the element itself.
    js::Value const anchor = js::argument(args, 2);
    if (!anchor.is_undefined()) {
        NodeWrapper* const wrapper = in.wrapper_of(anchor);
        if (wrapper == nullptr || !wrapper->node().is_element() || !static_cast<dom::Element const&>(wrapper->node()).is_html())
            return interp.throw_type_error("Failed to execute 'setValidity' on 'ElementInternals': parameter 3 is not of type 'HTMLElement'.");
        bool inside = &wrapper->node() == *element;
        for (dom::Node const* node = &wrapper->node(); node != nullptr && !inside;) {
            dom::Node const* parent = node->parent();
            if (parent == nullptr && node->is_shadow_root())
                parent = &static_cast<dom::ShadowRoot const&>(*node).host();
            inside = parent == *element;
            node = parent;
        }
        if (!inside)
            return in.throw_dom_exception("NotFoundError", "Failed to execute 'setValidity' on 'ElementInternals': The validation anchor is not a shadow-including inclusive descendant of the element.");
    }
    dom::Document& document = (*element)->document();
    document.internals_validity[*element] = { flags.bits(), flags.valid() ? std::string() : normalize_newlines(std::move(message)) };
    (*element)->mark_style_self();
    document.note_state_change();
    in.realm.note_mutation();
    return js::Value::undefined();
}

void install_internals_interface(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* const proto = define_interface(in, "ElementInternals", nullptr);
    define_getter(in, *proto, "shadowRoot", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        ElementInternalsObject* const self = internals_this(this_value);
        if (self == nullptr)
            return interp.throw_type_error("Illegal invocation");
        dom::Element* const element = self->element();
        dom::ShadowRoot* const shadow = element != nullptr ? element->shadow_root() : nullptr;
        if (shadow == nullptr || !shadow->available_to_element_internals)
            return js::Value::null();
        return js::Value::object(internals_of(interp).wrap(*shadow));
    });
    define_operation(interpreter, *proto, "setFormValue", 1, set_form_value);
    define_getter(in, *proto, "form", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Element*> const element = form_target(interp, this_value, "form");
        if (!element)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        return internals.realm.wrap_or_null(form_owner_of(internals, **element));
    });
    define_operation(interpreter, *proto, "setValidity", 0, set_validity);
    define_getter(in, *proto, "willValidate", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Element*> const element = form_target(interp, this_value, "willValidate");
        if (!element)
            return std::nullopt;
        return js::Value::boolean(will_validate_element(**element));
    });
    define_getter(in, *proto, "validity", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Element*> const element = form_target(interp, this_value, "validity");
        if (!element)
            return std::nullopt;
        ElementInternalsObject& self = *internals_this(this_value);
        if (self.validity == nullptr)
            self.validity = new_validity_state(internals_of(interp), *self.target);
        return js::Value::object(self.validity);
    });
    define_getter(in, *proto, "validationMessage", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Element*> const element = form_target(interp, this_value, "validationMessage");
        if (!element)
            return std::nullopt;
        return internals_of(interp).string(will_validate_element(**element) ? dom::forms::validation_message(**element) : std::string());
    });
    for (std::string_view const name : { "checkValidity", "reportValidity" }) {
        define_operation(interpreter, *proto, name, 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
            std::optional<dom::Element*> const element = form_target(interp, this_value, "checkValidity");
            if (!element)
                return std::nullopt;
            return js::Value::boolean(check_validity_of(internals_of(interp), **element));
        });
    }
    define_getter(in, *proto, "labels", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<dom::Element*> const element = form_target(interp, this_value, "labels");
        if (!element)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        return node_list(internals, labels_of(internals, **element));
    });
    define_getter(in, *proto, "states", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        ElementInternalsObject* const self = internals_this(this_value);
        if (self == nullptr)
            return interp.throw_type_error("Illegal invocation");
        if (self->states == nullptr) {
            Realm::Internals& internals = internals_of(interp);
            self->states = interp.heap().allocate<CustomStateSetObject>(internals.prototype("CustomStateSet"), *self->target);
        }
        return js::Value::object(self->states);
    });
    for (std::size_t i = 0; i < std::size(aria_names); ++i)
        define_plain_attribute(interpreter, *proto, aria_names[i], internals_aria_get, internals_aria_set, MemberKind::Plain, static_cast<std::uint32_t>(i));
}

// attachInternals() (§4.13.7.1): once per element, for an autonomous
// custom element whose definition allows it, as it is constructed or after.
Native attach_internals(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    Realm::Internals& in = internals_of(interp);
    NodeWrapper* const wrapper = in.wrapper_of(this_value);
    if (wrapper == nullptr || !wrapper->node().is_element())
        return interp.throw_type_error("Illegal invocation");
    auto& element = static_cast<dom::Element&>(wrapper->node());
    auto const refuse = [&](std::string_view why) -> Native {
        return in.throw_dom_exception("NotSupportedError", "Failed to execute 'attachInternals' on 'HTMLElement': " + std::string(why));
    };
    if (element.find_attribute("is") != nullptr && wrapper->custom_definition == nullptr)
        return refuse("Unable to attach ElementInternals to a customized built-in element.");
    CustomElementDefinition const* const definition
        = element.is_html() && &element.document() == in.document ? custom_element_definition(in, element.local_name()) : nullptr;
    if (definition == nullptr)
        return refuse("Unable to attach ElementInternals to non-custom elements.");
    if (definition->disable_internals)
        return refuse("ElementInternals is disabled by disabledFeatures static field.");
    if (wrapper->same_object(internals_key) != nullptr)
        return refuse("ElementInternals for the specified element was already attached.");
    if (wrapper->custom_definition == nullptr && !wrapper->custom_constructing)
        return refuse("The element is not a custom element yet.");
    // The options' behaviors (a proposal: the built-in behaviors a class may
    // take on), of which none is known here: an item is not one.
    if (js::Value const options = js::argument(args, 0); options.is_object()) {
        std::optional<js::Value> const behaviors = interp.get(*options.as_object(), interp.key("behaviors"));
        if (!behaviors)
            return std::nullopt;
        if (!behaviors->is_undefined()) {
            if (!behaviors->is_object())
                return interp.throw_type_error("Failed to execute 'attachInternals' on 'HTMLElement': behaviors is not a sequence.");
            std::optional<js::Value> const length = interp.get(*behaviors->as_object(), interp.key("length"));
            if (!length)
                return std::nullopt;
            std::optional<double> const count = interp.to_number(*length);
            if (!count)
                return std::nullopt;
            if (*count > 0)
                return interp.throw_type_error("Failed to execute 'attachInternals' on 'HTMLElement': The provided value is not of type 'ElementBehavior'.");
        }
    }
    js::Object* const made = interp.heap().allocate<ElementInternalsObject>(in.prototype("ElementInternals"), *wrapper);
    wrapper->keep_same_object(internals_key, made);
    return js::Value::object(made);
}

ElementInternalsObject* attached_internals(dom::Element const& element)
{
    auto* const wrapper = element.wrapper != nullptr ? dynamic_cast<NodeWrapper*>(element.wrapper) : nullptr;
    if (wrapper == nullptr)
        return nullptr;
    return dynamic_cast<ElementInternalsObject*>(wrapper->same_object(internals_key));
}

}

dom::Element* form_owner_of(Realm::Internals&, dom::Element& element)
{
    if (dom::Attr const* const named = element.find_attribute("form"); named != nullptr && element.is_connected()) {
        dom::Element* const found = named->value.empty() ? nullptr : element_by_id(element.root(), named->value);
        return found != nullptr && found->is_html("form") ? found : nullptr;
    }
    for (dom::Node* node = element.parent(); node != nullptr; node = node->parent()) {
        if (node->is_element() && static_cast<dom::Element*>(node)->is_html("form"))
            return static_cast<dom::Element*>(node);
    }
    return nullptr;
}

void install_attach_internals(Realm::Internals& in, js::Object& html_element_prototype)
{
    define_operation(in.interpreter, html_element_prototype, "attachInternals", 0, attach_internals);
}

void install_element_internals(Realm::Internals& in)
{
    js::Heap::NoCollect const no_collect(in.interpreter.heap());
    install_internals_interface(in);
    install_custom_state_set(in);
}

void form_associated_became(Realm::Internals& in, dom::Element& element)
{
    element.document().form_associated_custom.insert(&element);
    in.form_associated.try_emplace(&element);
    element.mark_style_self(); // :disabled and :enabled now apply
    check_one_element(in, element);
}

void form_associated_check(Realm::Internals& in)
{
    if (in.form_associated.empty())
        return;
    // A callback may change the tree again, which checks again within.
    std::vector<dom::Element*> elements;
    elements.reserve(in.form_associated.size());
    for (auto const& [element, state] : in.form_associated)
        elements.push_back(element);
    for (dom::Element* const element : elements)
        check_one_element(in, *element);
}

namespace {

// Whether a subtree that entered or left a tree holds what decides a form
// owner or a disabled state: a form, a fieldset, a form-associated element.
bool decides_form_association(dom::Node const& node)
{
    if (node.is_element()) {
        auto const& element = static_cast<dom::Element const&>(node);
        if (element.is_html("form") || element.is_html("fieldset") || form_associated(element))
            return true;
    }
    for (dom::Node const* child : node.children()) {
        if (decides_form_association(*child))
            return true;
    }
    return false;
}

}

void form_associated_tree_changed(Realm::Internals& in, std::span<dom::Node* const> subtrees)
{
    if (in.form_associated.empty())
        return;
    for (dom::Node const* subtree : subtrees) {
        if (decides_form_association(*subtree)) {
            form_associated_check(in);
            return;
        }
    }
}

void form_associated_reset(Realm::Internals& in, dom::Element& element)
{
    call_form_callback(in, element, &CustomElementDefinition::form_reset, js::Value::undefined(), false, "formResetCallback");
}

void internals_form_entries(Realm::Internals&, dom::Element const& element, std::vector<FormEntry>& out)
{
    ElementInternalsObject const* const internals = attached_internals(element);
    if (internals == nullptr || !internals->has_value)
        return;
    if (internals->value_is_list) {
        out.insert(out.end(), internals->entries.begin(), internals->entries.end());
        return;
    }
    dom::Attr const* const name = element.find_attribute("name");
    if (name == nullptr || name->value.empty() || internals->entries.empty())
        return;
    FormEntry entry = internals->entries.front();
    entry.name = name->value;
    out.push_back(std::move(entry));
}

}
