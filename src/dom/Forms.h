#pragma once
// The form controls' states (HTML §4.10, §4.16.3) as the tree and the
// document's live state give them: a control's type, value and checkedness,
// whether it is disabled, read-only or a candidate for constraint
// validation, and its validity (§4.10.20.1) — the flags ValidityState
// reports and the message validationMessage gives. One model, read by the
// selectors (:valid, :invalid, :in-range, :required...) and by the
// script interfaces (validity, checkValidity, the form's submission), so
// the two cannot disagree.
#include "dom/Dom.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sashfold::dom::forms {

// The attribute lowercased, empty when absent.
std::string attribute_lower(Element const& element, std::string_view name);
// An input's type: its type attribute's keyword, "text" for none or one
// not known.
std::string input_type(Element const& element);
bool takes_readonly(std::string const& type);
bool takes_placeholder(std::string const& type);
bool takes_required(std::string const& type);
// The nearest ancestor that is an element, or null.
Element const* element_parent(Element const& element);
bool disabled_by_fieldset(Element const& element);
bool can_be_disabled(Element const& element);
bool actually_disabled(Element const& element);
Element const* select_of(Element const& option);
bool option_selected(Element const& option);
bool checkedness(Element const& element);
// A text control's value as the host keeps it, else as the tree says.
std::string control_value(Element const& element);
bool editing_host_editable(Element const& element);
bool read_write(Element const& element);
bool parse_number(std::string_view text, double& out);
// Whether a number or range input's value is outside min/max; nothing for
// a control that has no range.
std::optional<bool> out_of_range(Element const& element);
bool plausible_email(std::string const& value);

// Every radio button in the group of `radio` (§4.10.5.1.15): those of its
// name in its form, or in its tree when it has none; just itself unnamed.
template<typename Each>
void each_in_radio_group(Element const& radio, Each const& each)
{
    Attr const* name = radio.find_attribute("name");
    if (!name || name->value.empty()) {
        each(radio);
        return;
    }
    Node const* root = &radio;
    for (Element const* ancestor = element_parent(radio); ancestor; ancestor = element_parent(*ancestor)) {
        root = ancestor;
        if (ancestor->is_html("form"))
            break;
    }
    // A radio in no tree is its own group's root, and counts.
    if (root == &radio) {
        each(radio);
        return;
    }
    auto const walk = [&](auto const& self, Node const& node) -> void {
        for (Node const* child : node.children()) {
            if (!child->is_element())
                continue;
            auto const& candidate = static_cast<Element const&>(*child);
            if (candidate.is_html("input") && input_type(candidate) == "radio") {
                Attr const* other = candidate.find_attribute("name");
                if (other && other->value == name->value)
                    each(candidate);
            }
            self(self, candidate);
        }
    };
    walk(walk, *root);
}

// §4.10.20: a submittable element that is not disabled, read-only or
// otherwise barred; a form-associated custom element with internals counts
// when the document has its validity.
bool validation_candidate(Element const& element);

// ValidityState's flags (§4.10.20.1), in the order the interface lists them.
struct Validity {
    bool value_missing = false;
    bool type_mismatch = false;
    bool pattern_mismatch = false;
    bool too_long = false;
    bool too_short = false;
    bool range_underflow = false;
    bool range_overflow = false;
    bool step_mismatch = false;
    bool bad_input = false;
    bool custom_error = false;
    bool valid() const
    {
        return !(value_missing || type_mismatch || pattern_mismatch || too_long || too_short || range_underflow || range_overflow
            || step_mismatch || bad_input || custom_error);
    }
    std::uint16_t bits() const;
    static Validity from_bits(std::uint16_t bits);
};

// The flags of a candidate; all clear for an element that is not one.
Validity validity_state(Element const& element);
// validationMessage: what the browsers say for the first flag set (Firefox's
// words, the user agent this engine names), the custom message for a custom
// error; empty when the element is valid or not a candidate.
std::string validation_message(Element const& element);
// Whether a form, fieldset or control matches :valid (true) or :invalid
// (false); nothing for an element neither matches.
std::optional<bool> validity(Element const& element);

}
