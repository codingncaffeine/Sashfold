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
#include <memory>
#include <vector>
#include <optional>
#include <string>
#include <cstddef>
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
// otherwise barred; a form-associated custom element is one, its validity
// what its ElementInternals said.
bool validation_candidate(Element const& element);

// A text control's selection (HTML §4.10.18.6), in code points of its value.
// It is clamped to the value whenever the value changes (a clean textarea's
// text, a form reset, a type change), moved to the end when script sets
// another value, and set with the value by the user's edits.
struct TextSelection {
    enum class Direction { None, Forward, Backward };
    std::size_t start = 0;
    std::size_t end = 0;
    Direction direction = Direction::None;
    // The end the caret is at (what moves when the selection is extended),
    // and the other.
    std::size_t focus() const { return direction == Direction::Backward ? start : end; }
    std::size_t anchor() const { return direction == Direction::Backward ? end : start; }
    bool collapsed() const { return start == end; }
    bool same_range(TextSelection const& other) const
    {
        return start == other.start && end == other.end && direction == other.direction;
    }
    // The same selection held within a value of `length` code points.
    TextSelection clamped(std::size_t length) const;
};
// A file the reader chose for an <input type=file> (or dropped on one): its
// path, the name it goes by, and what the file system said of it when it
// was chosen (File API §4: a snapshot). `relative` is its path under the
// folder chosen, for webkitdirectory.
struct ChosenFile {
    std::string path;
    std::string name;
    std::uint64_t size = 0;
    double modified = 0; // seconds since the epoch
    std::string type;
    std::string relative;
    // A file that is not on disk (a picture pasted from the clipboard): its
    // bytes, and no path.
    std::shared_ptr<std::vector<std::uint8_t> const> bytes = {};
};

// When a file was last changed, in seconds since the epoch (the same value
// for the same file time all through a run), or -1 when it cannot be read.
double file_modified_seconds(std::string const& path);

// Where the selection APIs apply (§4.10.5.4 table): selectionStart, End,
// Direction, setRangeText and setSelectionRange on text, search, url, tel
// and password inputs and on a textarea; select() on those and the other
// typed inputs too.
bool selection_applies(Element const& element);
bool select_applies(Element const& element);
// The same for an input of a type attribute's lowercased keyword.
bool select_applies_to_type(std::string const& keyword);
bool selection_applies_to_type(std::string const& keyword);
// The value sanitization algorithm of an input type (§4.10.5.1): newlines
// out of a text field's, a url or email trimmed, a number, date or time that
// is not a valid one emptied, a color lowercased or black.
std::string sanitize_value(std::string const& type, std::string value);
// valueAsNumber of an input's value: a number, range, date, month, week,
// time or datetime-local as the number it means, else NaN.
double value_as_number(std::string const& type, std::string_view value);
// A textarea's API value from its raw value: CR LF and CR become LF.
std::string normalize_newlines(std::string value);

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
