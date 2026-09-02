#pragma once

// Form controls as the page shows them: which elements are controls, the
// kind each one is, and the value, checkedness and caption each carries —
// from the live state the shell keeps while the user types and clicks,
// else from the markup's defaults. Layout draws from this, and the shell's
// submission reads the same answers.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::layout {

enum class ControlKind : std::uint8_t {
    Text, // input: text, search, url, email, tel, number, and the types this engine does not know
    Password,
    Checkbox,
    Radio,
    Submit, // input type=submit or image, and <button> without another type
    Button, // input type=button or reset, <button type=button|reset>
    Select,
    TextArea,
    Hidden, // submitted, never drawn
    File, // drawn as a disabled button, never submitted
};

ControlKind control_kind(dom::Element const& element);

// input (but hidden), button, select, textarea: the elements drawn as controls.
bool is_control(dom::Element const& element);

// Kinds that take typed text.
bool is_text_kind(ControlKind kind);

// What the user has done to a control since the page loaded.
struct ControlState {
    std::optional<std::string> value; // UTF-8; nullopt = the markup's default
    std::optional<bool> checked;
    std::size_t caret = 0; // code points from the start of the value
};

struct ControlStates {
    std::unordered_map<dom::Element const*, ControlState> states;
    dom::Element const* focused = nullptr;

    ControlState const* find(dom::Element const& element) const;
};

// The control's value (UTF-8): the live one, else the value attribute, a
// textarea's text, a select's selected option, or "on" for a checkbox or
// radio without a value.
std::string control_value(dom::Element const& element, ControlStates const* states);
bool control_checked(dom::Element const& element, ControlStates const* states);

// What is drawn: the value for text kinds, the caption for buttons (the
// value attribute, a <button>'s text, or Submit/Reset), the selected
// option's label for a select.
std::string control_caption(dom::Element const& element, ControlStates const* states);

// A select's options in order, and which one is selected.
struct SelectOptions {
    std::vector<std::string> labels;
    std::vector<std::string> values;
    std::size_t selected = 0;
};
SelectOptions select_options(dom::Element const& select, ControlStates const* states);

// The descendant text of an element, whitespace collapsed and trimmed.
std::string element_text(dom::Element const& element);

}
