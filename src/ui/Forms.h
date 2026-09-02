#pragma once

// Forms without scripts: which form a control belongs to, the form's data
// set, its urlencoding, and where a GET submission lands. The shell drives
// these; layout draws the controls themselves (layout/Controls.h).

#include "layout/Controls.h"
#include "net/Url.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::ui {

// The form a control belongs to: the element its form attribute names,
// else its nearest <form> ancestor; nullptr when it has none.
dom::Element const* form_owner(dom::Element const& control, dom::Document const& document);

struct FormField {
    std::string name;
    std::string value;
};

// The form's data set in tree order: every named control that is not
// disabled — text values, checked boxes, the selected option, hidden
// fields — and of the buttons only the submitter, when it has a name.
std::vector<FormField> form_data_set(dom::Element const& form, dom::Element const* submitter,
    layout::ControlStates const* states);

// application/x-www-form-urlencoded: name=value pairs joined by &, spaces
// as +, everything but letters, digits and * - . _ as %XX of its UTF-8 bytes.
std::string urlencode_form(std::vector<FormField> const& fields);

// Where a GET form lands: its action (the document's own URL when absent)
// with the data set as the query. nullopt for a form that posts or names a
// dialog — those are not written yet.
std::optional<net::Url> get_submission_url(dom::Element const& form, dom::Element const* submitter,
    layout::ControlStates const* states, net::Url const& document_url);

// A form's first submit button: what Enter in a text field presses.
dom::Element const* default_submitter(dom::Element const& form);

// The page's controls that can take focus, in tree order.
std::vector<dom::Element const*> focusable_controls(dom::Document const& document);

// The first form control with this name (the --script handle), hidden ones included.
dom::Element const* control_named(dom::Document const& document, std::string_view name);

// The element with this id, anywhere in the document.
dom::Element const* element_by_id(dom::Document const& document, std::string_view id);

}
