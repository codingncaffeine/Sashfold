#include "ui/Forms.h"

#include "core/Ascii.h"
#include "dom/Dom.h"
#include "html/DocumentBase.h"

namespace sashfold::ui {

namespace {

using layout::ControlKind;

template<typename Visit>
void for_each_element(dom::Node const& node, Visit const& visit)
{
    for (dom::Node const* child : node.children()) {
        if (!child->is_element())
            continue;
        auto const& element = static_cast<dom::Element const&>(*child);
        visit(element);
        for_each_element(element, visit);
    }
}

template<typename Match>
dom::Element const* find_element(dom::Node const& node, Match const& match)
{
    for (dom::Node const* child : node.children()) {
        if (!child->is_element())
            continue;
        auto const& element = static_cast<dom::Element const&>(*child);
        if (match(element))
            return &element;
        if (dom::Element const* found = find_element(element, match))
            return found;
    }
    return nullptr;
}

bool is_form_associated(dom::Element const& element)
{
    return element.is_html("input") || element.is_html("button") || element.is_html("select")
        || element.is_html("textarea");
}

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

// Newlines travel as CRLF in a form's data set.
std::string with_crlf(std::string const& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const c = text[i];
        if (c == '\r') {
            out += "\r\n";
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        } else if (c == '\n') {
            out += "\r\n";
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

dom::Element const* element_by_id(dom::Document const& document, std::string_view id)
{
    if (id.empty())
        return nullptr;
    return find_element(document, [&](dom::Element const& element) {
        dom::Attr const* attribute = element.find_attribute("id");
        return attribute && attribute->value == id;
    });
}

dom::Element const* form_owner(dom::Element const& control, dom::Document const& document)
{
    if (dom::Attr const* form = control.find_attribute("form")) {
        dom::Element const* named = element_by_id(document, form->value);
        return named && named->is_html("form") ? named : nullptr;
    }
    for (dom::Node const* node = control.parent(); node; node = node->parent()) {
        if (node->is_element() && static_cast<dom::Element const&>(*node).is_html("form"))
            return static_cast<dom::Element const*>(node);
    }
    return nullptr;
}

std::vector<FormField> form_data_set(dom::Element const& form, dom::Element const* submitter,
    layout::ControlStates const* states)
{
    std::vector<FormField> fields;
    for_each_element(form, [&](dom::Element const& element) {
        if (!is_form_associated(element) || element.has_attribute("disabled"))
            return;
        dom::Attr const* name = element.find_attribute("name");
        if (!name || name->value.empty())
            return;
        ControlKind const kind = layout::control_kind(element);
        std::string value;
        switch (kind) {
        case ControlKind::Submit:
        case ControlKind::Button:
            if (&element != submitter)
                return;
            value = element.is_html("button") ? attribute_or(element, "value", "")
                                              : attribute_or(element, "value", "Submit");
            break;
        case ControlKind::Checkbox:
        case ControlKind::Radio:
            if (!layout::control_checked(element, states))
                return;
            value = layout::control_value(element, states);
            break;
        case ControlKind::File:
            return;
        default:
            value = with_crlf(layout::control_value(element, states));
            break;
        }
        fields.push_back(FormField { name->value, std::move(value) });
    });
    return fields;
}

std::string urlencode_form(std::vector<FormField> const& fields)
{
    auto const encode = [](std::string const& text, std::string& out) {
        for (char const raw : text) {
            auto const c = static_cast<unsigned char>(raw);
            if (c == ' ') {
                out += '+';
            } else if (is_ascii_alphanumeric(c) || c == '*' || c == '-' || c == '.' || c == '_') {
                out += raw;
            } else {
                out += '%';
                out += "0123456789ABCDEF"[c >> 4];
                out += "0123456789ABCDEF"[c & 15];
            }
        }
    };
    std::string out;
    for (FormField const& field : fields) {
        if (!out.empty())
            out += '&';
        encode(field.name, out);
        out += '=';
        encode(field.value, out);
    }
    return out;
}

std::optional<net::Url> get_submission_url(dom::Element const& form, dom::Element const* submitter,
    layout::ControlStates const* states, net::Url const& document_url)
{
    // Where a GET lands is what its submission asks for; a form that posts
    // is not submitted by an address alone.
    std::optional<FormSubmission> asked = form_submission(form, submitter, states, document_url);
    if (!asked || asked->post)
        return std::nullopt;
    return std::move(asked->url);
}

std::string multipart_boundary_for(std::vector<FormField> const& fields)
{
    // A boundary is any text no part holds. This one is made of the fields'
    // own bytes, so the same form gives the same body, and lengthened until
    // nothing in the form contains it.
    std::uint32_t hash = 2166136261u;
    auto const mix = [&hash](std::string const& text) {
        for (char const c : text) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 16777619u;
        }
        hash ^= 0xff;
        hash *= 16777619u;
    };
    for (FormField const& field : fields) {
        mix(field.name);
        mix(field.value);
    }
    std::string boundary = "----SashfoldFormBoundary";
    for (int nibble = 0; nibble < 8; ++nibble)
        boundary += "0123456789abcdef"[(hash >> (4 * nibble)) & 15u];
    auto const held = [&] {
        for (FormField const& field : fields) {
            if (field.name.find(boundary) != std::string::npos || field.value.find(boundary) != std::string::npos)
                return true;
        }
        return false;
    };
    while (held())
        boundary += 'x';
    return boundary;
}

std::string multipart_form(std::vector<FormField> const& fields, std::string const& boundary)
{
    std::string out;
    for (FormField const& field : fields) {
        out += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"";
        for (char const c : field.name) {
            if (c == '"')
                out += "%22";
            else if (c == '\n')
                out += "%0A";
            else if (c == '\r')
                out += "%0D";
            else
                out += c;
        }
        out += "\"\r\n\r\n" + field.value + "\r\n";
    }
    out += "--" + boundary + "--\r\n";
    return out;
}

std::string plain_text_form(std::vector<FormField> const& fields)
{
    std::string out;
    for (FormField const& field : fields)
        out += field.name + "=" + field.value + "\r\n";
    return out;
}

std::optional<FormSubmission> form_submission(dom::Element const& form, dom::Element const* submitter,
    layout::ControlStates const* states, net::Url const& document_url)
{
    std::string method = lowercase(attribute_or(form, "method", "get"));
    std::string action = attribute_or(form, "action", "");
    std::string enctype = lowercase(attribute_or(form, "enctype", ""));
    if (submitter) {
        if (dom::Attr const* own = submitter->find_attribute("formmethod"))
            method = lowercase(own->value);
        if (dom::Attr const* own = submitter->find_attribute("formaction"))
            action = own->value;
        if (dom::Attr const* own = submitter->find_attribute("formenctype"))
            enctype = lowercase(own->value);
    }
    if (method == "dialog")
        return std::nullopt;
    // Anything that is not post is get: the invalid value default.
    bool const post = method == "post";
    // An action is resolved against the document base URL; with none, the
    // form goes to its document's own URL, whatever a base element says
    // (HTML §4.10.21.3).
    net::Url const base = html::document_base_url(form.document(), document_url);
    std::optional<net::Url> url = action.empty() ? std::optional<net::Url>(document_url) : net::parse_url(action, &base);
    if (!url)
        return std::nullopt;
    std::vector<FormField> const fields = form_data_set(form, submitter, states);
    FormSubmission submission;
    submission.post = post;
    url->fragment.reset();
    if (!post) {
        url->query = urlencode_form(fields);
        submission.url = std::move(*url);
        return submission;
    }
    submission.url = std::move(*url);
    std::string text;
    if (enctype == "multipart/form-data") {
        std::string const boundary = multipart_boundary_for(fields);
        text = multipart_form(fields, boundary);
        submission.content_type = "multipart/form-data; boundary=" + boundary;
    } else if (enctype == "text/plain") {
        text = plain_text_form(fields);
        submission.content_type = "text/plain";
    } else {
        text = urlencode_form(fields);
        submission.content_type = "application/x-www-form-urlencoded";
    }
    submission.body.assign(text.begin(), text.end());
    return submission;
}

dom::Element const* default_submitter(dom::Element const& form)
{
    return find_element(form, [](dom::Element const& element) {
        return (element.is_html("input") || element.is_html("button"))
            && layout::control_kind(element) == ControlKind::Submit
            && !element.has_attribute("disabled");
    });
}

std::vector<dom::Element const*> focusable_controls(dom::Document const& document)
{
    std::vector<dom::Element const*> controls;
    for_each_element(document, [&](dom::Element const& element) {
        if (layout::is_control(element) && !element.has_attribute("disabled"))
            controls.push_back(&element);
    });
    return controls;
}

dom::Element const* control_named(dom::Document const& document, std::string_view name)
{
    return find_element(document, [&](dom::Element const& element) {
        if (!is_form_associated(element))
            return false;
        dom::Attr const* attribute = element.find_attribute("name");
        return attribute && attribute->value == name;
    });
}

}
