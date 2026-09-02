#include "layout/Controls.h"

#include "core/Ascii.h"
#include "dom/Dom.h"

namespace sashfold::layout {

namespace {

std::string attribute_or(dom::Element const& element, std::string_view name, std::string fallback)
{
    dom::Attr const* attribute = element.find_attribute(name);
    return attribute ? attribute->value : std::move(fallback);
}

std::string lowercase(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return text;
}

bool is_html_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

void gather_text(dom::Node const& node, std::string& out)
{
    for (dom::Node const* child : node.children()) {
        if (child->is_text())
            out += static_cast<dom::Text const*>(child)->data;
        else if (child->is_element())
            gather_text(*child, out);
    }
}

// The <option> children of a select or an optgroup, in order.
void collect_options(dom::Node const& node, SelectOptions& options, std::optional<std::size_t>& marked)
{
    for (dom::Node const* child : node.children()) {
        if (!child->is_element())
            continue;
        auto const& element = static_cast<dom::Element const&>(*child);
        if (element.is_html("option")) {
            std::string const text = element_text(element);
            options.labels.push_back(attribute_or(element, "label", text));
            options.values.push_back(attribute_or(element, "value", text));
            if (!marked && element.has_attribute("selected"))
                marked = options.labels.size() - 1;
        } else if (element.is_html("optgroup")) {
            collect_options(element, options, marked);
        }
    }
}

} // namespace

ControlKind control_kind(dom::Element const& element)
{
    if (element.is_html("textarea"))
        return ControlKind::TextArea;
    if (element.is_html("select"))
        return ControlKind::Select;
    std::string const type = lowercase(attribute_or(element, "type", ""));
    if (element.is_html("button"))
        return type == "button" || type == "reset" ? ControlKind::Button : ControlKind::Submit;
    if (type == "password")
        return ControlKind::Password;
    if (type == "checkbox")
        return ControlKind::Checkbox;
    if (type == "radio")
        return ControlKind::Radio;
    if (type == "submit" || type == "image")
        return ControlKind::Submit;
    if (type == "button" || type == "reset")
        return ControlKind::Button;
    if (type == "hidden")
        return ControlKind::Hidden;
    if (type == "file")
        return ControlKind::File;
    return ControlKind::Text;
}

bool is_control(dom::Element const& element)
{
    if (element.is_html("button") || element.is_html("select") || element.is_html("textarea"))
        return true;
    return element.is_html("input") && control_kind(element) != ControlKind::Hidden;
}

bool is_text_kind(ControlKind kind)
{
    return kind == ControlKind::Text || kind == ControlKind::Password
        || kind == ControlKind::TextArea;
}

ControlState const* ControlStates::find(dom::Element const& element) const
{
    auto const it = states.find(&element);
    return it == states.end() ? nullptr : &it->second;
}

std::string element_text(dom::Element const& element)
{
    std::string raw;
    gather_text(element, raw);
    std::string out;
    bool pending_space = false;
    for (char const c : raw) {
        if (is_html_space(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space)
            out += ' ';
        pending_space = false;
        out += c;
    }
    return out;
}

SelectOptions select_options(dom::Element const& select, ControlStates const* states)
{
    SelectOptions options;
    std::optional<std::size_t> marked;
    collect_options(select, options, marked);
    options.selected = marked.value_or(0);
    // The live value names the selected option.
    if (ControlState const* state = states ? states->find(select) : nullptr;
        state && state->value) {
        for (std::size_t i = 0; i < options.values.size(); ++i) {
            if (options.values[i] == *state->value) {
                options.selected = i;
                break;
            }
        }
    }
    return options;
}

std::string control_value(dom::Element const& element, ControlStates const* states)
{
    if (ControlState const* state = states ? states->find(element) : nullptr;
        state && state->value)
        return *state->value;
    switch (control_kind(element)) {
    case ControlKind::TextArea: {
        std::string text;
        gather_text(element, text);
        return text;
    }
    case ControlKind::Select: {
        SelectOptions const options = select_options(element, states);
        return options.values.empty() ? std::string() : options.values[options.selected];
    }
    case ControlKind::Checkbox:
    case ControlKind::Radio:
        return attribute_or(element, "value", "on");
    default:
        return attribute_or(element, "value", "");
    }
}

bool control_checked(dom::Element const& element, ControlStates const* states)
{
    if (ControlState const* state = states ? states->find(element) : nullptr;
        state && state->checked)
        return *state->checked;
    return element.has_attribute("checked");
}

std::string control_caption(dom::Element const& element, ControlStates const* states)
{
    ControlKind const kind = control_kind(element);
    switch (kind) {
    case ControlKind::Submit:
    case ControlKind::Button: {
        if (element.is_html("button"))
            return element_text(element);
        if (dom::Attr const* value = element.find_attribute("value"))
            return value->value;
        if (lowercase(attribute_or(element, "type", "")) == "reset")
            return "Reset";
        return kind == ControlKind::Submit ? "Submit" : "";
    }
    case ControlKind::Select: {
        SelectOptions const options = select_options(element, states);
        return options.labels.empty() ? std::string() : options.labels[options.selected];
    }
    case ControlKind::File:
        return "Browse...";
    case ControlKind::Checkbox:
    case ControlKind::Radio:
    case ControlKind::Hidden:
        return "";
    default:
        return control_value(element, states);
    }
}

}
