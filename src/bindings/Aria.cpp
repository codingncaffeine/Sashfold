// ARIAMixin (WAI-ARIA 1.2 §10.1) on Element: the role and ARIA attributes
// reflected as nullable strings, the element-reference attributes
// (ariaLabelledByElements and the rest) reflected as HTML §2.6.1 reflects
// Element? and FrozenArray<Element>?, and ariaNotify() on Element and
// Document. The elements a script sets, and the announcements it asks for,
// are kept on the document (dom::Document::aria_element_references,
// aria_notifications), where the accessibility model reads them.

#include "bindings/NodeSupport.h"

#include "js/Runtime.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

namespace {

// The string attributes: property and content attribute.
constexpr std::pair<std::string_view, std::string_view> aria_strings[] = {
    { "role", "role" },
    { "ariaAtomic", "aria-atomic" },
    { "ariaAutoComplete", "aria-autocomplete" },
    { "ariaBrailleLabel", "aria-braillelabel" },
    { "ariaBrailleRoleDescription", "aria-brailleroledescription" },
    { "ariaBusy", "aria-busy" },
    { "ariaChecked", "aria-checked" },
    { "ariaColCount", "aria-colcount" },
    { "ariaColIndex", "aria-colindex" },
    { "ariaColIndexText", "aria-colindextext" },
    { "ariaColSpan", "aria-colspan" },
    { "ariaCurrent", "aria-current" },
    { "ariaDescription", "aria-description" },
    { "ariaDisabled", "aria-disabled" },
    { "ariaExpanded", "aria-expanded" },
    { "ariaHasPopup", "aria-haspopup" },
    { "ariaHidden", "aria-hidden" },
    { "ariaInvalid", "aria-invalid" },
    { "ariaKeyShortcuts", "aria-keyshortcuts" },
    { "ariaLabel", "aria-label" },
    { "ariaLevel", "aria-level" },
    { "ariaLive", "aria-live" },
    { "ariaModal", "aria-modal" },
    { "ariaMultiLine", "aria-multiline" },
    { "ariaMultiSelectable", "aria-multiselectable" },
    { "ariaOrientation", "aria-orientation" },
    { "ariaPlaceholder", "aria-placeholder" },
    { "ariaPosInSet", "aria-posinset" },
    { "ariaPressed", "aria-pressed" },
    { "ariaReadOnly", "aria-readonly" },
    { "ariaRelevant", "aria-relevant" },
    { "ariaRequired", "aria-required" },
    { "ariaRoleDescription", "aria-roledescription" },
    { "ariaRowCount", "aria-rowcount" },
    { "ariaRowIndex", "aria-rowindex" },
    { "ariaRowIndexText", "aria-rowindextext" },
    { "ariaRowSpan", "aria-rowspan" },
    { "ariaSelected", "aria-selected" },
    { "ariaSetSize", "aria-setsize" },
    { "ariaSort", "aria-sort" },
    { "ariaValueMax", "aria-valuemax" },
    { "ariaValueMin", "aria-valuemin" },
    { "ariaValueNow", "aria-valuenow" },
    { "ariaValueText", "aria-valuetext" },
};

struct ElementReference {
    std::string_view property;
    std::string_view attribute;
    bool list;
};
constexpr ElementReference element_references[] = {
    { "ariaActiveDescendantElement", "aria-activedescendant", false },
    { "ariaActionsElements", "aria-actions", true },
    { "ariaControlsElements", "aria-controls", true },
    { "ariaDescribedByElements", "aria-describedby", true },
    { "ariaDetailsElements", "aria-details", true },
    { "ariaErrorMessageElements", "aria-errormessage", true },
    { "ariaFlowToElements", "aria-flowto", true },
    { "ariaLabelledByElements", "aria-labelledby", true },
    { "ariaOwnsElements", "aria-owns", true },
};

bool ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// HTML §2.6.1: an explicitly set element counts while it is a descendant
// of one of the element's shadow-including ancestors.
bool in_scope(dom::Element const& element, dom::Element const& candidate)
{
    dom::Node const& candidate_root = candidate.root();
    for (dom::Node const* root = &element.root();;) {
        if (root == &candidate_root)
            return true;
        if (!root->is_shadow_root())
            return false;
        root = &static_cast<dom::ShadowRoot const&>(*root).host().root();
    }
}

// The attr-associated elements (HTML §2.6.1): the explicitly set ones that
// are in scope, while the content attribute is still the empty string the
// setter left; else the elements its IDs name in the element's tree; nothing
// for an absent attribute.
std::optional<std::vector<dom::Element*>> associated(dom::Element& element, std::string_view attribute)
{
    // The explicitly set elements stand while the attribute is absent or
    // the empty string the setter wrote: removing the attribute leaves them
    // (as the engines do); writing IDs to it puts the IDs in charge.
    dom::Attr const* const content = element.find_attribute(attribute);
    std::vector<dom::Element*> out;
    if (!content || content->value.empty()) {
        auto const& all = element.document().aria_element_references;
        if (auto const found = all.find(&element); found != all.end()) {
            if (auto const list = found->second.find(std::string(attribute)); list != found->second.end()) {
                for (dom::Element* candidate : list->second) {
                    if (in_scope(element, *candidate))
                        out.push_back(candidate);
                }
                return out;
            }
        }
    }
    if (!content)
        return std::nullopt;
    std::string_view const value = content->value;
    std::size_t at = 0;
    while (at < value.size()) {
        while (at < value.size() && ascii_space(value[at]))
            ++at;
        std::size_t const start = at;
        while (at < value.size() && !ascii_space(value[at]))
            ++at;
        if (at == start)
            continue;
        if (dom::Element* found = element_by_id(element.root(), value.substr(start, at - start)))
            out.push_back(found);
    }
    return out;
}

void forget_explicit(dom::Element& element, std::string_view attribute)
{
    auto& all = element.document().aria_element_references;
    if (auto found = all.find(&element); found != all.end()) {
        found->second.erase(std::string(attribute));
        if (found->second.empty())
            all.erase(found);
    }
}

Native read_reference(Realm::Internals& in, dom::Element& element, ElementReference const& reference)
{
    std::optional<std::vector<dom::Element*>> const elements = associated(element, reference.attribute);
    if (!reference.list) {
        if (!elements || elements->empty())
            return js::Value::null();
        return js::Value::object(in.wrap(*elements->front()));
    }
    if (!elements)
        return js::Value::null();
    // The same frozen array while the elements are the same ("cached
    // attr-associated elements").
    auto* wrapper = static_cast<NodeWrapper*>(in.wrap(element));
    std::string const key = "aria:" + std::string(reference.attribute);
    js::Interpreter& interpreter = in.interpreter;
    if (js::Object* cached = wrapper->same_object(key)) {
        std::optional<js::Value> const length = interpreter.get(*cached, interpreter.key("length"));
        bool same = length && length->is_number() && static_cast<std::size_t>(length->as_number()) == elements->size();
        for (std::size_t i = 0; same && i < elements->size(); ++i) {
            std::optional<js::Value> const item = interpreter.get(*cached, js::PropertyKey::index(static_cast<std::uint32_t>(i)));
            same = item && item->is_object() && item->as_object() == in.wrap(*(*elements)[i]);
        }
        if (same)
            return js::Value::object(cached);
    }
    js::Interpreter::Roots const roots(interpreter);
    js::ArrayObject* array = interpreter.new_array();
    interpreter.root(js::Value::object(array));
    for (dom::Element* item : *elements)
        array->push(js::Value::object(in.wrap(*item)));
    if (!js::set_integrity_level(interpreter, *array, true))
        return std::nullopt;
    wrapper->keep_same_object(key, array);
    return js::Value::object(array);
}

Native write_reference(Realm::Internals& in, dom::Element& element, js::Value const& value, ElementReference const& reference)
{
    std::string const attribute(reference.attribute);
    if (value.is_nullish()) {
        forget_explicit(element, attribute);
        remove_attribute(in, element, attribute);
        return js::Value::undefined();
    }
    std::vector<dom::Element*> elements;
    auto const element_of = [&](js::Value const& item) -> dom::Element* {
        dom::Node* node = in.realm.node_of(item);
        return node && node->is_element() ? static_cast<dom::Element*>(node) : nullptr;
    };
    if (reference.list) {
        std::optional<std::vector<js::Value>> const items = in.interpreter.iterable_to_list(value);
        if (!items)
            return std::nullopt;
        for (js::Value const& item : *items) {
            dom::Element* target = element_of(item);
            if (!target)
                return in.interpreter.throw_type_error("Failed to set '" + std::string(reference.property) + "': an item is not an Element");
            elements.push_back(target);
        }
    } else {
        dom::Element* target = element_of(value);
        if (!target)
            return in.interpreter.throw_type_error("Failed to set '" + std::string(reference.property) + "': the value is not an Element");
        elements.push_back(target);
    }
    // An element of another document is never in scope (a document's
    // nodes are its own): only this document's are kept, so that none is
    // read after its own document ended.
    std::erase_if(elements, [&](dom::Element* target) { return &target->document() != &element.document(); });
    // The content attribute becomes the empty string, which says the
    // explicit elements stand (HTML §2.6.1, the setter's steps).
    set_attribute(in, element, attribute, "");
    element.document().aria_element_references[&element][attribute] = std::move(elements);
    element.mark_style_self();
    element.document().note_state_change();
    return js::Value::undefined();
}

// ariaNotify(announcement, options) (ARIA Notify, the explainer's
// AriaNotificationOptions: priority "normal" or "high"): queued on the
// document for its accessibility tree, the last 64 kept while none takes
// them.
Native aria_notify(Realm::Internals& in, dom::Document& document, dom::Element const* element, Args args)
{
    std::optional<std::string> const text = in.to_utf8(js::argument(args, 0));
    if (!text)
        return std::nullopt;
    std::string priority = "normal";
    js::Value const options = js::argument(args, 1);
    if (options.is_object()) {
        std::optional<js::Value> const given = in.interpreter.get(*options.as_object(), in.interpreter.key("priority"));
        if (!given)
            return std::nullopt;
        if (!given->is_undefined()) {
            std::optional<std::string> const value = in.to_utf8(*given);
            if (!value)
                return std::nullopt;
            if (*value != "normal" && *value != "high")
                return in.interpreter.throw_type_error("The provided value '" + *value + "' is not a valid AriaNotifyPriority");
            priority = *value;
        }
    } else if (!options.is_nullish()) {
        return in.interpreter.throw_type_error("Failed to execute 'ariaNotify': the options are not an object");
    }
    auto& queue = document.aria_notifications;
    if (queue.size() >= 64)
        queue.erase(queue.begin());
    queue.push_back(dom::Document::AriaNotification { element, *text, priority });
    document.note_state_change();
    return js::Value::undefined();
}

}

void install_aria_mixin(Realm::Internals& in, js::Object& element)
{
    for (auto const& [property, attribute] : aria_strings) {
        std::string const name(attribute);
        element_accessor(
            in, element, property,
            [name](Realm::Internals& internals, dom::Element& e) -> Native {
                dom::Attr const* found = e.find_attribute(name);
                return found ? internals.string(found->value) : js::Value::null();
            },
            [name](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                if (value.is_null()) {
                    remove_attribute(internals, e, name);
                    return js::Value::undefined();
                }
                std::optional<std::string> const text = internals.to_utf8(value);
                if (!text)
                    return std::nullopt;
                set_attribute(internals, e, name, *text);
                return js::Value::undefined();
            });
    }
    for (ElementReference const& reference : element_references) {
        element_accessor(
            in, element, reference.property,
            [reference](Realm::Internals& internals, dom::Element& e) -> Native { return read_reference(internals, e, reference); },
            [reference](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                return write_reference(internals, e, value, reference);
            });
    }
    element_method(in, element, "ariaNotify", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
        return aria_notify(internals, e.document(), &e, args);
    });
}

void install_document_aria_notify(Realm::Internals& in, js::Object& document)
{
    document_method(in, document, "ariaNotify", 1, [](Realm::Internals& internals, dom::Document& d, Args args) -> Native {
        return aria_notify(internals, d, nullptr, args);
    });
}

}
