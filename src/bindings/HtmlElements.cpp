#include "bindings/NodeSupport.h"

// The HTML element interfaces (HTML §4): HTMLElement itself and one
// interface per tag family, each mostly reflecting attributes; the form
// controls read their live value through the host.

#include "html/Serializer.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::bindings {

// --- Control state -----------------------------------------------------------------

namespace {

std::string default_value_of(dom::Element const& element)
{
    if (element.is_html("textarea"))
        return html::text_content(element);
    if (element.is_html("select")) {
        // The first selected option, else the first option.
        std::vector<dom::Node*> descendants;
        collect_descendants(const_cast<dom::Element&>(element), descendants);
        dom::Element const* first = nullptr;
        for (dom::Node* node : descendants) {
            if (!node->is_element() || !static_cast<dom::Element*>(node)->is_html("option"))
                continue;
            auto const* option = static_cast<dom::Element const*>(node);
            if (option->has_attribute("selected"))
                return option->has_attribute("value") ? attribute_or_empty(*option, "value") : html::text_content(*option);
            if (!first)
                first = option;
        }
        if (first)
            return first->has_attribute("value") ? attribute_or_empty(*first, "value") : html::text_content(*first);
        return "";
    }
    if (element.is_html("input")) {
        std::string const type = ascii_lower(attribute_or_empty(element, "type"));
        if ((type == "checkbox" || type == "radio") && !element.has_attribute("value"))
            return "on";
        if (type == "file")
            return "";
    }
    if (element.is_html("option") && !element.has_attribute("value"))
        return html::text_content(element);
    return attribute_or_empty(element, "value");
}

} // namespace

std::string control_value_of(Realm::Internals& in, dom::Element const& element)
{
    if (in.hooks.control_value) {
        if (std::optional<std::string> value = in.hooks.control_value(element))
            return *value;
    } else {
        auto const it = in.fallback_values.find(&element);
        if (it != in.fallback_values.end())
            return it->second;
    }
    return default_value_of(element);
}

void set_control_value_of(Realm::Internals& in, dom::Element const& element, std::string value)
{
    if (in.hooks.set_control_value)
        in.hooks.set_control_value(element, value);
    else
        in.fallback_values[&element] = std::move(value);
    in.realm.note_mutation();
}

bool control_checked_of(Realm::Internals& in, dom::Element const& element)
{
    if (in.hooks.control_checked) {
        if (std::optional<bool> const checked = in.hooks.control_checked(element))
            return *checked;
    } else {
        auto const it = in.fallback_checked.find(&element);
        if (it != in.fallback_checked.end())
            return it->second;
    }
    return element.has_attribute("checked");
}

void set_control_checked_of(Realm::Internals& in, dom::Element const& element, bool checked)
{
    if (in.hooks.set_control_checked)
        in.hooks.set_control_checked(element, checked);
    else
        in.fallback_checked[&element] = checked;
    in.realm.note_mutation();
}

dom::Element const* focused_element(Realm::Internals& in)
{
    if (in.hooks.focused)
        return in.hooks.focused();
    return in.fallback_focus;
}

void move_focus(Realm::Internals& in, dom::Element const* element)
{
    dom::Element const* previous = focused_element(in);
    if (previous == element)
        return;
    if (in.hooks.focus)
        in.hooks.focus(element);
    else
        in.fallback_focus = element;
    if (previous) {
        in.realm.dispatch_event(const_cast<dom::Element*>(previous), "blur");
        in.realm.dispatch_event(const_cast<dom::Element*>(previous), "focusout", Realm::EventInit { true, false, true });
    }
    if (element) {
        in.realm.dispatch_event(const_cast<dom::Element*>(element), "focus");
        in.realm.dispatch_event(const_cast<dom::Element*>(element), "focusin", Realm::EventInit { true, false, true });
    }
}

namespace {

// --- Helpers ---------------------------------------------------------------------------

std::optional<std::string> string_of(Realm::Internals& in, js::Value const& value)
{
    return in.to_utf8(value);
}

// The parts of a URL-valued element (a, area): protocol, host, ...
void install_url_parts(Realm::Internals& in, js::Object& proto)
{
    auto const part = [](std::string_view which) {
        return [which](Realm::Internals& internals, dom::Element& e) -> Native {
            dom::Attr const* href = e.find_attribute("href");
            std::optional<net::Url> const url = href ? net::parse_url(href->value, &internals.url) : std::nullopt;
            if (!url)
                return internals.string("");
            if (which == "protocol")
                return internals.string(url->protocol());
            if (which == "host")
                return internals.string(url->host_with_port());
            if (which == "hostname")
                return internals.string(url->serialize_host());
            if (which == "port")
                return internals.string(url->port_string());
            if (which == "pathname")
                return internals.string(url->serialize_path());
            if (which == "search")
                return internals.string(url->search());
            if (which == "hash")
                return internals.string(url->hash());
            if (which == "origin")
                return internals.string(url->serialize_origin());
            return internals.string(url->serialize());
        };
    };
    for (std::string_view const name : { "protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin" })
        element_getter(in, proto, name, part(name));
    element_method(in, proto, "toString", 0, [part](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        return part("href")(internals, e);
    });
}

void install_labels(Realm::Internals& in, js::Object& proto)
{
    element_getter(in, proto, "labels", [](Realm::Internals& internals, dom::Element& e) -> Native {
        std::vector<dom::Node*> descendants;
        collect_descendants(internals.document, descendants);
        std::vector<dom::Node*> labels;
        std::string const id = attribute_or_empty(e, "id");
        for (dom::Node* node : descendants) {
            if (!node->is_element() || !static_cast<dom::Element*>(node)->is_html("label"))
                continue;
            auto& label = static_cast<dom::Element&>(*node);
            if (!id.empty() && attribute_or_empty(label, "for") == id)
                labels.push_back(node);
            else if (!label.has_attribute("for") && is_inclusive_ancestor(label, e) && &label != &e)
                labels.push_back(node);
        }
        return node_list(internals, labels);
    });
}

// form, validity and the validation methods every control carries.
void install_form_control_common(Realm::Internals& in, js::Object& proto)
{
    element_getter(in, proto, "form", [](Realm::Internals& internals, dom::Element& e) -> Native {
        if (dom::Attr const* form_id = e.find_attribute("form")) {
            if (dom::Element* owner = element_by_id(internals.document, form_id->value))
                return js::Value::object(internals.wrap(*owner));
        }
        for (dom::Node* node = e.parent(); node; node = node->parent()) {
            if (node->is_element() && static_cast<dom::Element*>(node)->is_html("form"))
                return js::Value::object(internals.wrap(*node));
        }
        return js::Value::null();
    });
    element_getter(in, proto, "validity", [](Realm::Internals& internals, dom::Element&) -> Native {
        js::Heap::NoCollect const guard(internals.interpreter.heap());
        js::Object* validity = internals.interpreter.new_object();
        for (std::string_view const name : { "valueMissing", "typeMismatch", "patternMismatch", "tooLong", "tooShort", "rangeUnderflow",
                 "rangeOverflow", "stepMismatch", "badInput", "customError" })
            validity->put(internals.interpreter.key(name), js::Value::boolean(false));
        validity->put(internals.interpreter.key("valid"), js::Value::boolean(true));
        return js::Value::object(validity);
    });
    element_getter(in, proto, "validationMessage", [](Realm::Internals& internals, dom::Element&) -> Native { return internals.string(""); });
    element_getter(in, proto, "willValidate", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::boolean(true); });
    element_method(in, proto, "checkValidity", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(true); });
    element_method(in, proto, "reportValidity", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(true); });
    element_method(in, proto, "setCustomValidity", 1, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    install_labels(in, proto);
}

void install_value_accessor(Realm::Internals& in, js::Object& proto)
{
    element_accessor(
        in, proto, "value", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(control_value_of(internals, e)); },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> text = value.is_nullish() ? std::optional<std::string>("") : string_of(internals, value);
            if (!text)
                return std::nullopt;
            set_control_value_of(internals, e, std::move(*text));
            return js::Value::undefined();
        });
}

// The option elements under a select, in tree order.
std::vector<dom::Element*> options_of(dom::Element& select)
{
    std::vector<dom::Node*> descendants;
    collect_descendants(select, descendants);
    std::vector<dom::Element*> options;
    for (dom::Node* node : descendants) {
        if (node->is_element() && static_cast<dom::Element*>(node)->is_html("option"))
            options.push_back(static_cast<dom::Element*>(node));
    }
    return options;
}

dom::Element* select_of(dom::Element& option)
{
    for (dom::Node* node = option.parent(); node; node = node->parent()) {
        if (node->is_element() && static_cast<dom::Element*>(node)->is_html("select"))
            return static_cast<dom::Element*>(node);
    }
    return nullptr;
}

std::string option_value(dom::Element const& option)
{
    return option.has_attribute("value") ? attribute_or_empty(option, "value") : html::text_content(option);
}

// The form controls of a form, in tree order (the listed elements).
std::vector<dom::Node*> form_controls(Realm::Internals& in, dom::Element& form)
{
    std::vector<dom::Node*> descendants;
    collect_descendants(in.document, descendants);
    std::vector<dom::Node*> controls;
    std::string const form_id = attribute_or_empty(form, "id");
    for (dom::Node* node : descendants) {
        if (!node->is_element())
            continue;
        auto& element = static_cast<dom::Element&>(*node);
        if (!(element.is_html("input") || element.is_html("select") || element.is_html("textarea") || element.is_html("button")
                || element.is_html("fieldset") || element.is_html("output") || element.is_html("object")))
            continue;
        bool owned = false;
        if (dom::Attr const* owner = element.find_attribute("form"))
            owned = !form_id.empty() && owner->value == form_id;
        else
            owned = is_inclusive_ancestor(form, element) && &element != &form;
        if (owned)
            controls.push_back(node);
    }
    return controls;
}

} // namespace

// --- The interfaces ---------------------------------------------------------------------

void install_html_elements(Realm::Internals& in, js::Object& html_element)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());

    // HTMLElement.
    element_accessor(
        in, html_element, "tabIndex",
        [](Realm::Internals&, dom::Element& e) -> Native {
            if (dom::Attr const* attribute = e.find_attribute("tabindex")) {
                char* end = nullptr;
                long const value = std::strtol(attribute->value.c_str(), &end, 10);
                if (end != attribute->value.c_str())
                    return js::Value::number(static_cast<double>(value));
            }
            bool const focusable = e.is_html("a") || e.is_html("button") || e.is_html("input") || e.is_html("select")
                || e.is_html("textarea") || e.is_html("area") || e.is_html("iframe") || e.is_html("summary");
            return js::Value::number(focusable ? 0 : -1);
        },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<double> const number = internals.interpreter.to_number(value);
            if (!number)
                return std::nullopt;
            set_attribute(internals, e, "tabindex", js::number_to_utf8(js::Interpreter::to_integer_or_infinity(*number)));
            return js::Value::undefined();
        });
    element_accessor(
        in, html_element, "contentEditable",
        [](Realm::Internals& internals, dom::Element& e) -> Native {
            dom::Attr const* attribute = e.find_attribute("contenteditable");
            return internals.string(attribute ? (attribute->value.empty() ? "true" : attribute->value) : "inherit");
        },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> text = string_of(internals, value);
            if (!text)
                return std::nullopt;
            set_attribute(internals, e, "contenteditable", std::move(*text));
            return js::Value::undefined();
        });
    element_getter(in, html_element, "isContentEditable", [](Realm::Internals&, dom::Element& e) -> Native {
        dom::Attr const* attribute = e.find_attribute("contenteditable");
        return js::Value::boolean(attribute && (attribute->value.empty() || ascii_lower(attribute->value) == "true"));
    });
    element_accessor(
        in, html_element, "innerText",
        [](Realm::Internals& internals, dom::Element& e) -> Native {
            // Rendered text needs layout; the text content stands in.
            return internals.string(html::text_content(e));
        },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> const text = value.is_nullish() ? std::optional<std::string>("") : string_of(internals, value);
            if (!text)
                return std::nullopt;
            replace_children_with_text(internals, e, *text);
            return js::Value::undefined();
        });
    element_accessor(
        in, html_element, "outerText",
        [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(html::text_content(e)); },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> const text = value.is_nullish() ? std::optional<std::string>("") : string_of(internals, value);
            if (!text)
                return std::nullopt;
            dom::Node* parent = e.parent();
            if (!parent)
                return js::Value::undefined();
            dom::Text* node = e.document().create<dom::Text>();
            node->data = *text;
            parent->insert_before(*node, &e);
            remove_node(internals, e);
            return js::Value::undefined();
        });
    element_getter(in, html_element, "style", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_style_declaration(internals, &e, false); });
    element_getter(in, html_element, "dataset", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_dataset(internals, e); });
    element_getter(in, html_element, "offsetParent", [](Realm::Internals& internals, dom::Element& e) -> Native {
        if (e.is_html("body") || e.is_html("html"))
            return js::Value::null();
        return internals.realm.wrap_or_null(body_element(internals.document));
    });
    auto const offset = [](int which) {
        return [which](Realm::Internals& internals, dom::Element& e) -> Native {
            if (!internals.hooks.layout_box)
                return js::Value::number(0);
            std::optional<LayoutBox> const box = internals.hooks.layout_box(e);
            if (!box)
                return js::Value::number(0);
            switch (which) {
            case 0: return js::Value::number(std::round(static_cast<double>(box->width)));
            case 1: return js::Value::number(std::round(static_cast<double>(box->height)));
            case 2: return js::Value::number(std::round(static_cast<double>(box->y)));
            default: return js::Value::number(std::round(static_cast<double>(box->x)));
            }
        };
    };
    element_getter(in, html_element, "offsetWidth", offset(0));
    element_getter(in, html_element, "offsetHeight", offset(1));
    element_getter(in, html_element, "offsetTop", offset(2));
    element_getter(in, html_element, "offsetLeft", offset(3));
    element_method(in, html_element, "click", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        if (e.has_attribute("disabled") && (e.is_html("button") || e.is_html("input") || e.is_html("select") || e.is_html("textarea")))
            return js::Value::undefined();
        Realm::MouseInit init;
        internals.realm.dispatch_mouse_event(e, "click", init);
        return js::Value::undefined();
    });
    element_method(in, html_element, "focus", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        move_focus(internals, &e);
        return js::Value::undefined();
    });
    element_method(in, html_element, "blur", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
        if (focused_element(internals) == &e)
            move_focus(internals, nullptr);
        return js::Value::undefined();
    });
    for (std::string_view const name : { "showPopover", "hidePopover", "togglePopover" })
        element_method(in, html_element, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });

    // One interface per tag family, and every attribute reflected from a
    // content attribute: generated from idl/html-elements.idl (see
    // generated/HtmlElements.gen.cpp). Anything not named there is an
    // HTMLElement; an unknown tag is an HTMLUnknownElement. What follows
    // here is the hand-written rest of each interface.
    generated::install_html_element_interfaces(in);
    generated::install_reflected_attributes(in);
    js::Object* media_element = in.prototype("HTMLMediaElement");
    auto const proto_of = [&](std::string_view name) -> js::Object& { return *in.prototype(name); };

    // Anchors and areas.
    for (std::string_view const name : { "HTMLAnchorElement", "HTMLAreaElement" }) {
        js::Object& proto = proto_of(name);
        element_getter(in, proto, "relList", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_token_list(internals, e, "rel"); });
        install_url_parts(in, proto);
    }
    element_accessor(
        in, proto_of("HTMLAnchorElement"), "text",
        [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(html::text_content(e)); },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> const text = string_of(internals, value);
            if (!text)
                return std::nullopt;
            replace_children_with_text(internals, e, *text);
            return js::Value::undefined();
        });

    // Base.

    // Images.
    {
        js::Object& proto = proto_of("HTMLImageElement");
        auto const dimension = [](bool width) {
            return [width](Realm::Internals& internals, dom::Element& e) -> Native {
                if (internals.hooks.layout_box) {
                    if (std::optional<LayoutBox> const box = internals.hooks.layout_box(e))
                        return js::Value::number(std::round(static_cast<double>(width ? box->width : box->height)));
                }
                dom::Attr const* attribute = e.find_attribute(width ? "width" : "height");
                if (attribute) {
                    char* end = nullptr;
                    long const value = std::strtol(attribute->value.c_str(), &end, 10);
                    if (end != attribute->value.c_str())
                        return js::Value::number(static_cast<double>(value));
                }
                if (internals.hooks.image_size) {
                    if (auto const size = internals.hooks.image_size(e))
                        return js::Value::number(width ? size->first : size->second);
                }
                return js::Value::number(0);
            };
        };
        auto const set_dimension = [](bool width) {
            return [width](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<double> const number = internals.interpreter.to_number(value);
                if (!number)
                    return std::nullopt;
                set_attribute(internals, e, width ? "width" : "height", js::number_to_utf8(js::Interpreter::to_integer_or_infinity(*number)));
                return js::Value::undefined();
            };
        };
        element_accessor(in, proto, "width", dimension(true), set_dimension(true));
        element_accessor(in, proto, "height", dimension(false), set_dimension(false));
        auto const natural = [](bool width) {
            return [width](Realm::Internals& internals, dom::Element& e) -> Native {
                if (internals.hooks.image_size) {
                    if (auto const size = internals.hooks.image_size(e))
                        return js::Value::number(width ? size->first : size->second);
                }
                return js::Value::number(0);
            };
        };
        element_getter(in, proto, "naturalWidth", natural(true));
        element_getter(in, proto, "naturalHeight", natural(false));
        element_getter(in, proto, "complete", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::boolean(true); });
        element_getter(in, proto, "currentSrc", [](Realm::Internals& internals, dom::Element& e) -> Native {
            dom::Attr const* src = e.find_attribute("src");
            if (!src)
                return internals.string("");
            std::optional<net::Url> const url = net::parse_url(src->value, &internals.url);
            return internals.string(url ? url->serialize() : src->value);
        });
        element_getter(in, proto, "x", [](Realm::Internals& internals, dom::Element& e) -> Native {
            std::optional<LayoutBox> const box = client_box(internals, e);
            return js::Value::number(box ? std::round(static_cast<double>(box->x)) : 0);
        });
        element_getter(in, proto, "y", [](Realm::Internals& internals, dom::Element& e) -> Native {
            std::optional<LayoutBox> const box = client_box(internals, e);
            return js::Value::number(box ? std::round(static_cast<double>(box->y)) : 0);
        });
        element_method(in, proto, "decode", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    }

    // Inputs.
    {
        js::Object& proto = proto_of("HTMLInputElement");
        install_form_control_common(in, proto);
        install_value_accessor(in, proto);
        element_accessor(
            in, proto, "type",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                std::string type = ascii_lower(attribute_or_empty(e, "type"));
                static constexpr std::string_view known[] = { "hidden", "text", "search", "tel", "url", "email", "password", "date", "month",
                    "week", "time", "datetime-local", "number", "range", "color", "checkbox", "radio", "file", "submit", "image", "reset",
                    "button" };
                bool found = false;
                for (std::string_view const candidate : known)
                    found = found || candidate == type;
                return internals.string(found ? type : "text");
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                set_attribute(internals, e, "type", std::move(*text));
                return js::Value::undefined();
            });
        element_accessor(
            in, proto, "checked", [](Realm::Internals& internals, dom::Element& e) -> Native { return js::Value::boolean(control_checked_of(internals, e)); },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                set_control_checked_of(internals, e, js::Interpreter::to_boolean(value));
                return js::Value::undefined();
            });
        element_getter(in, proto, "files", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
        element_getter(in, proto, "list", [](Realm::Internals& internals, dom::Element& e) -> Native {
            dom::Attr const* list = e.find_attribute("list");
            return internals.realm.wrap_or_null(list ? element_by_id(internals.document, list->value) : nullptr);
        });
        element_accessor(
            in, proto, "indeterminate", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::boolean(false); },
            [](Realm::Internals&, dom::Element&, js::Value const&) -> Native { return js::Value::undefined(); });
        element_accessor(
            in, proto, "valueAsNumber",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                return js::Value::number(js::string_to_number(js::utf16_from_utf8(control_value_of(internals, e))));
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<double> const number = internals.interpreter.to_number(value);
                if (!number)
                    return std::nullopt;
                set_control_value_of(internals, e, std::isnan(*number) ? "" : js::number_to_utf8(*number));
                return js::Value::undefined();
            });
        for (std::string_view const name : { "selectionStart", "selectionEnd" }) {
            element_accessor(
                in, proto, name,
                [](Realm::Internals& internals, dom::Element& e) -> Native {
                    return js::Value::number(static_cast<double>(js::utf16_from_utf8(control_value_of(internals, e)).size()));
                },
                [](Realm::Internals&, dom::Element&, js::Value const&) -> Native { return js::Value::undefined(); });
        }
        element_accessor(
            in, proto, "selectionDirection", [](Realm::Internals& internals, dom::Element&) -> Native { return internals.string("none"); },
            [](Realm::Internals&, dom::Element&, js::Value const&) -> Native { return js::Value::undefined(); });
        for (std::string_view const name : { "select", "setSelectionRange", "setRangeText", "stepUp", "stepDown", "showPicker" })
            element_method(in, proto, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    }

    // Text areas.
    {
        js::Object& proto = proto_of("HTMLTextAreaElement");
        install_form_control_common(in, proto);
        install_value_accessor(in, proto);
        element_accessor(
            in, proto, "defaultValue", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(html::text_content(e)); },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> const text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                replace_children_with_text(internals, e, *text);
                return js::Value::undefined();
            });
        element_getter(in, proto, "type", [](Realm::Internals& internals, dom::Element&) -> Native { return internals.string("textarea"); });
        element_getter(in, proto, "textLength", [](Realm::Internals& internals, dom::Element& e) -> Native {
            return js::Value::number(static_cast<double>(js::utf16_from_utf8(control_value_of(internals, e)).size()));
        });
        for (std::string_view const name : { "select", "setSelectionRange", "setRangeText" })
            element_method(in, proto, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
    }

    // Selects and options.
    {
        js::Object& proto = proto_of("HTMLSelectElement");
        install_form_control_common(in, proto);
        install_value_accessor(in, proto);
        element_getter(in, proto, "type", [](Realm::Internals& internals, dom::Element& e) -> Native {
            return internals.string(e.has_attribute("multiple") ? "select-multiple" : "select-one");
        });
        element_getter(in, proto, "options", [](Realm::Internals& internals, dom::Element& e) -> Native {
            std::vector<dom::Node*> nodes;
            for (dom::Element* option : options_of(e))
                nodes.push_back(option);
            js::Interpreter::Roots const roots(internals.interpreter);
            js::Value const list = internals.interpreter.root(node_list(internals, nodes));
            list.as_object()->set_prototype(internals.prototype("HTMLCollection"));
            return list;
        });
        element_getter(in, proto, "length", [](Realm::Internals&, dom::Element& e) -> Native {
            return js::Value::number(static_cast<double>(options_of(e).size()));
        });
        element_getter(in, proto, "selectedOptions", [](Realm::Internals& internals, dom::Element& e) -> Native {
            std::string const value = control_value_of(internals, e);
            std::vector<dom::Node*> nodes;
            for (dom::Element* option : options_of(e)) {
                if (option_value(*option) == value) {
                    nodes.push_back(option);
                    break;
                }
            }
            return node_list(internals, nodes);
        });
        element_accessor(
            in, proto, "selectedIndex",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                std::string const value = control_value_of(internals, e);
                std::vector<dom::Element*> const options = options_of(e);
                for (std::size_t i = 0; i < options.size(); ++i) {
                    if (option_value(*options[i]) == value)
                        return js::Value::number(static_cast<double>(i));
                }
                return js::Value::number(-1);
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<double> const index = internals.interpreter.to_number(value);
                if (!index)
                    return std::nullopt;
                std::vector<dom::Element*> const options = options_of(e);
                if (*index >= 0 && *index < static_cast<double>(options.size()))
                    set_control_value_of(internals, e, option_value(*options[static_cast<std::size_t>(*index)]));
                return js::Value::undefined();
            });
        element_method(in, proto, "item", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            std::optional<double> const index = internals.interpreter.to_number(js::argument(args, 0));
            if (!index)
                return std::nullopt;
            std::vector<dom::Element*> const options = options_of(e);
            if (*index < 0 || *index >= static_cast<double>(options.size()))
                return js::Value::null();
            return js::Value::object(internals.wrap(*options[static_cast<std::size_t>(*index)]));
        });
        element_method(in, proto, "namedItem", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            std::optional<std::string> const name = string_of(internals, js::argument(args, 0));
            if (!name)
                return std::nullopt;
            for (dom::Element* option : options_of(e)) {
                if (attribute_or_empty(*option, "id") == *name || attribute_or_empty(*option, "name") == *name)
                    return js::Value::object(internals.wrap(*option));
            }
            return js::Value::null();
        });
        element_method(in, proto, "add", 1, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            dom::Node* option = internals.realm.node_of(js::argument(args, 0));
            if (!option)
                return internals.interpreter.throw_type_error("parameter 1 is not of type 'HTMLOptionElement'");
            js::Value const before = js::argument(args, 1);
            dom::Node* reference = internals.realm.node_of(before);
            if (!reference && before.is_number()) {
                std::vector<dom::Element*> const options = options_of(e);
                auto const index = static_cast<std::size_t>(std::max(0.0, before.as_number()));
                if (index < options.size())
                    reference = options[index];
            }
            if (!pre_insert(internals, e, *option, reference))
                return std::nullopt;
            return js::Value::undefined();
        });
        element_method(in, proto, "remove", 0, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            if (args.empty()) {
                remove_node(internals, e);
                return js::Value::undefined();
            }
            std::optional<double> const index = internals.interpreter.to_number(args[0]);
            if (!index)
                return std::nullopt;
            std::vector<dom::Element*> const options = options_of(e);
            if (*index >= 0 && *index < static_cast<double>(options.size()))
                remove_node(internals, *options[static_cast<std::size_t>(*index)]);
            return js::Value::undefined();
        });

        js::Object& option = proto_of("HTMLOptionElement");
        element_accessor(
            in, option, "value", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(option_value(e)); },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                set_attribute(internals, e, "value", std::move(*text));
                return js::Value::undefined();
            });
        element_accessor(
            in, option, "text",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                std::string text = html::text_content(e);
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
                return internals.string(out);
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> const text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                replace_children_with_text(internals, e, *text);
                return js::Value::undefined();
            });
        element_accessor(
            in, option, "selected",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                dom::Element* select = select_of(e);
                if (!select)
                    return js::Value::boolean(e.has_attribute("selected"));
                return js::Value::boolean(control_value_of(internals, *select) == option_value(e));
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                dom::Element* select = select_of(e);
                if (js::Interpreter::to_boolean(value)) {
                    if (select)
                        set_control_value_of(internals, *select, option_value(e));
                    else
                        set_attribute(internals, e, "selected", "");
                } else if (!select) {
                    remove_attribute(internals, e, "selected");
                }
                return js::Value::undefined();
            });
        element_getter(in, option, "index", [](Realm::Internals&, dom::Element& e) -> Native {
            dom::Element* select = select_of(e);
            if (!select)
                return js::Value::number(0);
            std::vector<dom::Element*> const options = options_of(*select);
            for (std::size_t i = 0; i < options.size(); ++i) {
                if (options[i] == &e)
                    return js::Value::number(static_cast<double>(i));
            }
            return js::Value::number(0);
        });
        element_getter(in, option, "form", [](Realm::Internals& internals, dom::Element& e) -> Native {
            for (dom::Node* node = e.parent(); node; node = node->parent()) {
                if (node->is_element() && static_cast<dom::Element*>(node)->is_html("form"))
                    return js::Value::object(internals.wrap(*node));
            }
            return js::Value::null();
        });
    }

    // Buttons.
    {
        js::Object& proto = proto_of("HTMLButtonElement");
        install_form_control_common(in, proto);
        element_accessor(
            in, proto, "type",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                std::string const type = ascii_lower(attribute_or_empty(e, "type"));
                return internals.string(type == "reset" || type == "button" ? type : "submit");
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                set_attribute(internals, e, "type", std::move(*text));
                return js::Value::undefined();
            });
    }

    // Forms.
    {
        js::Object& proto = proto_of("HTMLFormElement");
        element_accessor(
            in, proto, "action",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                dom::Attr const* action = e.find_attribute("action");
                if (!action || action->value.empty())
                    return internals.string(internals.url.serialize());
                std::optional<net::Url> const url = net::parse_url(action->value, &internals.url);
                return internals.string(url ? url->serialize() : action->value);
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                set_attribute(internals, e, "action", std::move(*text));
                return js::Value::undefined();
            });
        element_accessor(
            in, proto, "method",
            [](Realm::Internals& internals, dom::Element& e) -> Native {
                std::string const method = ascii_lower(attribute_or_empty(e, "method"));
                return internals.string(method == "post" || method == "dialog" ? method : "get");
            },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                set_attribute(internals, e, "method", std::move(*text));
                return js::Value::undefined();
            });
        element_getter(in, proto, "elements", [](Realm::Internals& internals, dom::Element& e) -> Native {
            js::Interpreter::Roots const roots(internals.interpreter);
            js::Value const list = internals.interpreter.root(node_list(internals, form_controls(internals, e)));
            list.as_object()->set_prototype(internals.prototype("HTMLCollection"));
            return list;
        });
        element_getter(in, proto, "length", [](Realm::Internals& internals, dom::Element& e) -> Native {
            return js::Value::number(static_cast<double>(form_controls(internals, e).size()));
        });
        element_method(in, proto, "submit", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
            if (internals.hooks.submit_form)
                internals.hooks.submit_form(e, nullptr);
            return js::Value::undefined();
        });
        element_method(in, proto, "requestSubmit", 0, [](Realm::Internals& internals, dom::Element& e, Args args) -> Native {
            dom::Node* submitter = internals.realm.node_of(js::argument(args, 0));
            if (internals.realm.dispatch_event(&e, "submit", Realm::EventInit { true, true, false }) && internals.hooks.submit_form)
                internals.hooks.submit_form(e, submitter && submitter->is_element() ? static_cast<dom::Element*>(submitter) : nullptr);
            return js::Value::undefined();
        });
        element_method(in, proto, "reset", 0, [](Realm::Internals& internals, dom::Element& e, Args) -> Native {
            if (!internals.realm.dispatch_event(&e, "reset", Realm::EventInit { true, true, false }))
                return js::Value::undefined();
            for (dom::Node* control : form_controls(internals, e)) {
                auto& element = static_cast<dom::Element&>(*control);
                set_control_value_of(internals, element, default_value_of(element));
                if (element.is_html("input"))
                    set_control_checked_of(internals, element, element.has_attribute("checked"));
            }
            return js::Value::undefined();
        });
        element_method(in, proto, "checkValidity", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(true); });
        element_method(in, proto, "reportValidity", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::boolean(true); });
    }

    // Labels, fieldsets, legends, outputs.
    {
        js::Object& label = proto_of("HTMLLabelElement");
        element_getter(in, label, "control", [](Realm::Internals& internals, dom::Element& e) -> Native {
            if (dom::Attr const* target = e.find_attribute("for"))
                return internals.realm.wrap_or_null(element_by_id(internals.document, target->value));
            std::vector<dom::Node*> descendants;
            collect_descendants(e, descendants);
            for (dom::Node* node : descendants) {
                if (!node->is_element())
                    continue;
                auto& element = static_cast<dom::Element&>(*node);
                if (element.is_html("input") || element.is_html("select") || element.is_html("textarea") || element.is_html("button"))
                    return js::Value::object(internals.wrap(element));
            }
            return js::Value::null();
        });
        element_getter(in, label, "form", [](Realm::Internals& internals, dom::Element& e) -> Native {
            for (dom::Node* node = e.parent(); node; node = node->parent()) {
                if (node->is_element() && static_cast<dom::Element*>(node)->is_html("form"))
                    return js::Value::object(internals.wrap(*node));
            }
            return js::Value::null();
        });
        install_form_control_common(in, proto_of("HTMLFieldSetElement"));
        element_getter(in, proto_of("HTMLFieldSetElement"), "type", [](Realm::Internals& internals, dom::Element&) -> Native { return internals.string("fieldset"); });
        element_getter(in, proto_of("HTMLFieldSetElement"), "elements", [](Realm::Internals& internals, dom::Element& e) -> Native {
            std::vector<dom::Node*> descendants;
            collect_descendants(e, descendants);
            std::vector<dom::Node*> controls;
            for (dom::Node* node : descendants) {
                if (!node->is_element())
                    continue;
                auto& element = static_cast<dom::Element&>(*node);
                if (element.is_html("input") || element.is_html("select") || element.is_html("textarea") || element.is_html("button"))
                    controls.push_back(node);
            }
            return node_list(internals, controls);
        });
        install_form_control_common(in, proto_of("HTMLOutputElement"));
        install_value_accessor(in, proto_of("HTMLOutputElement"));
    }

    // Scripts, styles, links, metas.
    {
        js::Object& script = proto_of("HTMLScriptElement");
        element_accessor(
            in, script, "text", [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(html::text_content(e)); },
            [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                std::optional<std::string> const text = string_of(internals, value);
                if (!text)
                    return std::nullopt;
                replace_children_with_text(internals, e, *text);
                return js::Value::undefined();
            });
        js::Object& style = proto_of("HTMLStyleElement");
        element_getter(in, style, "sheet", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
        js::Object& link = proto_of("HTMLLinkElement");
        element_getter(in, link, "relList", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_token_list(internals, e, "rel"); });
        element_getter(in, link, "sizes", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_token_list(internals, e, "sizes"); });
        element_getter(in, link, "sheet", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
    }

    // Frames, embeds, objects, canvas, media.
    {
        js::Object& iframe = proto_of("HTMLIFrameElement");
        element_getter(in, iframe, "sandbox", [](Realm::Internals& internals, dom::Element& e) -> Native { return make_token_list(internals, e, "sandbox"); });
        element_getter(in, iframe, "contentWindow", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
        element_getter(in, iframe, "contentDocument", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
        for (std::string_view const name : { "HTMLEmbedElement", "HTMLObjectElement" }) {
            js::Object& proto = proto_of(name);
            element_getter(in, proto, "contentWindow", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
            element_getter(in, proto, "contentDocument", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
        }
        js::Object& canvas = proto_of("HTMLCanvasElement");
        element_method(in, canvas, "getContext", 1, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::null(); });
        element_method(in, canvas, "toDataURL", 0, [](Realm::Internals& internals, dom::Element&, Args) -> Native { return internals.string("data:,"); });
        element_method(in, canvas, "toBlob", 1, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
        element_method(in, canvas, "captureStream", 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::null(); });

        element_getter(in, *media_element, "currentSrc", [](Realm::Internals& internals, dom::Element& e) -> Native {
            dom::Attr const* src = e.find_attribute("src");
            std::optional<net::Url> const url = src ? net::parse_url(src->value, &internals.url) : std::nullopt;
            return internals.string(url ? url->serialize() : "");
        });
        for (auto const& [name, value] : { std::pair { "paused", 1.0 }, std::pair { "ended", 0.0 }, std::pair { "seeking", 0.0 } }) {
            bool const truth = value != 0.0;
            element_getter(in, *media_element, name, [truth](Realm::Internals&, dom::Element&) -> Native { return js::Value::boolean(truth); });
        }
        for (std::string_view const name : { "muted", "defaultPlaybackRate", "playbackRate", "volume", "currentTime" }) {
            double const initial = name == "muted" ? 0 : (name == "currentTime" ? 0 : 1);
            bool const boolean = name == "muted";
            element_accessor(
                in, *media_element, name,
                [initial, boolean](Realm::Internals&, dom::Element&) -> Native {
                    return boolean ? js::Value::boolean(false) : js::Value::number(initial);
                },
                [](Realm::Internals&, dom::Element&, js::Value const&) -> Native { return js::Value::undefined(); });
        }
        element_getter(in, *media_element, "duration", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(std::nan("")); });
        element_getter(in, *media_element, "readyState", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(0); });
        element_getter(in, *media_element, "networkState", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(0); });
        element_getter(in, *media_element, "error", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::null(); });
        for (std::string_view const name : { "play", "pause", "load" })
            element_method(in, *media_element, name, 0, [](Realm::Internals&, dom::Element&, Args) -> Native { return js::Value::undefined(); });
        element_method(in, *media_element, "canPlayType", 1, [](Realm::Internals& internals, dom::Element&, Args) -> Native { return internals.string(""); });
        for (auto const& [name, value] : { std::pair { "NETWORK_EMPTY", 0 }, std::pair { "NETWORK_IDLE", 1 }, std::pair { "NETWORK_LOADING", 2 },
                 std::pair { "NETWORK_NO_SOURCE", 3 }, std::pair { "HAVE_NOTHING", 0 }, std::pair { "HAVE_METADATA", 1 },
                 std::pair { "HAVE_CURRENT_DATA", 2 }, std::pair { "HAVE_FUTURE_DATA", 3 }, std::pair { "HAVE_ENOUGH_DATA", 4 } })
            media_element->put(interpreter.key(name), js::Value::number(value), js::Enumerable);
        js::Object& video = proto_of("HTMLVideoElement");
        element_getter(in, video, "videoWidth", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(0); });
        element_getter(in, video, "videoHeight", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(0); });
    }

    // Tables.
    {
        auto const rows_of = [](Realm::Internals& internals, dom::Element& e, bool own_sections_only) -> Native {
            std::vector<dom::Node*> descendants;
            collect_descendants(e, descendants);
            std::vector<dom::Node*> rows;
            for (dom::Node* node : descendants) {
                if (!node->is_element() || !static_cast<dom::Element*>(node)->is_html("tr"))
                    continue;
                // Rows of nested tables are not this table's.
                dom::Node* owner = node->parent();
                while (owner && owner != &e && !(owner->is_element() && static_cast<dom::Element*>(owner)->is_html("table")))
                    owner = owner->parent();
                if (owner == &e || (!own_sections_only && owner == nullptr))
                    rows.push_back(node);
            }
            js::Interpreter::Roots const roots(internals.interpreter);
            js::Value const list = internals.interpreter.root(node_list(internals, rows));
            list.as_object()->set_prototype(internals.prototype("HTMLCollection"));
            return list;
        };
        js::Object& table = proto_of("HTMLTableElement");
        element_getter(in, table, "rows", [rows_of](Realm::Internals& internals, dom::Element& e) -> Native { return rows_of(internals, e, true); });
        element_getter(in, table, "tBodies", [](Realm::Internals& internals, dom::Element& e) -> Native {
            std::vector<dom::Node*> bodies;
            for (dom::Node* child : e.children()) {
                if (child->is_element() && static_cast<dom::Element*>(child)->is_html("tbody"))
                    bodies.push_back(child);
            }
            return node_list(internals, bodies);
        });
        for (auto const& [property, tag] : { std::pair { "tHead", "thead" }, std::pair { "tFoot", "tfoot" }, std::pair { "caption", "caption" } }) {
            std::string const tag_name(tag);
            element_getter(in, table, property, [tag_name](Realm::Internals& internals, dom::Element& e) -> Native {
                for (dom::Node* child : e.children()) {
                    if (child->is_element() && static_cast<dom::Element*>(child)->is_html(tag_name))
                        return js::Value::object(internals.wrap(*child));
                }
                return js::Value::null();
            });
        }
        js::Object& section = proto_of("HTMLTableSectionElement");
        element_getter(in, section, "rows", [rows_of](Realm::Internals& internals, dom::Element& e) -> Native { return rows_of(internals, e, true); });
        js::Object& row = proto_of("HTMLTableRowElement");
        element_getter(in, row, "cells", [](Realm::Internals& internals, dom::Element& e) -> Native {
            std::vector<dom::Node*> cells;
            for (dom::Node* child : e.children()) {
                if (child->is_element() && (static_cast<dom::Element*>(child)->is_html("td") || static_cast<dom::Element*>(child)->is_html("th")))
                    cells.push_back(child);
            }
            js::Interpreter::Roots const roots(internals.interpreter);
            js::Value const list = internals.interpreter.root(node_list(internals, cells));
            list.as_object()->set_prototype(internals.prototype("HTMLCollection"));
            return list;
        });
        element_getter(in, row, "rowIndex", [](Realm::Internals&, dom::Element& e) -> Native {
            dom::Node* owner_table = e.parent();
            while (owner_table && !(owner_table->is_element() && static_cast<dom::Element*>(owner_table)->is_html("table")))
                owner_table = owner_table->parent();
            if (!owner_table)
                return js::Value::number(-1);
            std::vector<dom::Node*> descendants;
            collect_descendants(*owner_table, descendants);
            int index = 0;
            for (dom::Node* node : descendants) {
                if (node == &e)
                    return js::Value::number(index);
                if (node->is_element() && static_cast<dom::Element*>(node)->is_html("tr"))
                    ++index;
            }
            return js::Value::number(-1);
        });
        element_getter(in, row, "sectionRowIndex", [](Realm::Internals&, dom::Element& e) -> Native {
            dom::Node* parent = e.parent();
            if (!parent)
                return js::Value::number(-1);
            int index = 0;
            for (dom::Node* child : parent->children()) {
                if (child == &e)
                    return js::Value::number(index);
                if (child->is_element() && static_cast<dom::Element*>(child)->is_html("tr"))
                    ++index;
            }
            return js::Value::number(-1);
        });
        js::Object& cell = proto_of("HTMLTableCellElement");
        element_getter(in, cell, "cellIndex", [](Realm::Internals&, dom::Element& e) -> Native {
            dom::Node* parent = e.parent();
            if (!parent)
                return js::Value::number(-1);
            int index = 0;
            for (dom::Node* child : parent->children()) {
                if (child == &e)
                    return js::Value::number(index);
                if (child->is_element() && (static_cast<dom::Element*>(child)->is_html("td") || static_cast<dom::Element*>(child)->is_html("th")))
                    ++index;
            }
            return js::Value::number(-1);
        });
    }

    // The rest: single attributes.
    for (std::string_view const name : { "show", "showModal", "close" }) {
        bool const open = name != "close";
        element_method(in, proto_of("HTMLDialogElement"), name, 0, [open](Realm::Internals& internals, dom::Element& e, Args) -> Native {
            if (open)
                set_attribute(internals, e, "open", "");
            else
                remove_attribute(internals, e, "open");
            return js::Value::undefined();
        });
    }
    for (std::string_view const name : { "HTMLProgressElement", "HTMLMeterElement" }) {
        js::Object& proto = proto_of(name);
        for (std::string_view const attribute : { "value", "max", "min", "low", "high", "optimum" }) {
            std::string const attribute_name(attribute);
            element_accessor(
                in, proto, attribute,
                [attribute_name](Realm::Internals&, dom::Element& e) -> Native {
                    dom::Attr const* found = e.find_attribute(attribute_name);
                    if (!found)
                        return js::Value::number(attribute_name == "max" ? 1 : 0);
                    return js::Value::number(js::string_to_number(js::utf16_from_utf8(found->value)));
                },
                [attribute_name](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
                    std::optional<double> const number = internals.interpreter.to_number(value);
                    if (!number)
                        return std::nullopt;
                    set_attribute(internals, e, attribute_name, js::number_to_utf8(*number));
                    return js::Value::undefined();
                });
        }
        element_getter(in, proto, "position", [](Realm::Internals&, dom::Element&) -> Native { return js::Value::number(-1); });
    }
    element_getter(in, proto_of("HTMLTemplateElement"), "content", [](Realm::Internals& internals, dom::Element& e) -> Native {
        return js::Value::object(internals.wrap(content_container(e)));
    });
    element_accessor(
        in, proto_of("HTMLTitleElement"), "text",
        [](Realm::Internals& internals, dom::Element& e) -> Native { return internals.string(html::text_content(e)); },
        [](Realm::Internals& internals, dom::Element& e, js::Value const& value) -> Native {
            std::optional<std::string> const text = string_of(internals, value);
            if (!text)
                return std::nullopt;
            replace_children_with_text(internals, e, *text);
            return js::Value::undefined();
        });

    // The Image and Option constructors (§4.8.3, §4.10.10).
    js::NativeFunction* image = interpreter.new_native("Image", 0,
        [](js::Interpreter& interp, js::Value const&, Args) -> Native { return interp.throw_type_error("Please use the 'new' operator"); },
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            dom::Element* img = internals.document.create<dom::Element>(std::string(dom::ns::html), "img");
            for (std::size_t i = 0; i < 2 && i < args.size(); ++i) {
                if (args[i].is_undefined())
                    continue;
                std::optional<double> const number = interp.to_number(args[i]);
                if (!number)
                    return std::nullopt;
                img->attributes().push_back(dom::Attr { i == 0 ? "width" : "height", js::number_to_utf8(js::Interpreter::to_integer_or_infinity(*number)), "", "" });
            }
            return js::Value::object(internals.wrap(*img));
        });
    image->put(interpreter.key("prototype"), js::Value::object(&proto_of("HTMLImageElement")), js::frozen_attributes);
    interpreter.global()->put(interpreter.key("Image"), js::Value::object(image), js::builtin_attributes);
    js::NativeFunction* option_constructor = interpreter.new_native("Option", 0,
        [](js::Interpreter& interp, js::Value const&, Args) -> Native { return interp.throw_type_error("Please use the 'new' operator"); },
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            dom::Element* option = internals.document.create<dom::Element>(std::string(dom::ns::html), "option");
            if (!args.empty() && !args[0].is_undefined()) {
                std::optional<std::string> text = internals.to_utf8(args[0]);
                if (!text)
                    return std::nullopt;
                dom::Text* node = internals.document.create<dom::Text>();
                node->data = std::move(*text);
                option->append_child(*node);
            }
            if (args.size() > 1 && !args[1].is_undefined()) {
                std::optional<std::string> value = internals.to_utf8(args[1]);
                if (!value)
                    return std::nullopt;
                option->attributes().push_back(dom::Attr { "value", std::move(*value), "", "" });
            }
            if (args.size() > 2 && js::Interpreter::to_boolean(args[2]))
                option->attributes().push_back(dom::Attr { "selected", "", "", "" });
            return js::Value::object(internals.wrap(*option));
        });
    option_constructor->put(interpreter.key("prototype"), js::Value::object(&proto_of("HTMLOptionElement")), js::frozen_attributes);
    interpreter.global()->put(interpreter.key("Option"), js::Value::object(option_constructor), js::builtin_attributes);
}

}
