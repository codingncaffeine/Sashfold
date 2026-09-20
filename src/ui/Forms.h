#pragma once

// Forms without scripts: which form a control belongs to, the form's data
// set, its encodings, and what submitting it asks for — where a GET lands,
// and for a form that posts the body it sends. The shell drives these;
// layout draws the controls themselves (layout/Controls.h).

#include "layout/Controls.h"
#include "net/Url.h"

#include <cstdint>
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

// What submitting a form asks for (HTML §4.10.21.3): where it goes and, for
// a form that posts, the body and the type it is sent as. The method is the
// form's, or the submitter's formmethod; the encoding the form's enctype, or
// the submitter's formenctype — application/x-www-form-urlencoded unless it
// says multipart/form-data or text/plain. A GET's data set is the query of
// its address; a POST's address is the action as written, its data set the
// body. nullopt for a form that names a dialog, and for an action that is no
// address.
struct FormSubmission {
    net::Url url;
    bool post = false;
    std::string content_type;
    std::vector<std::uint8_t> body;
};

std::optional<FormSubmission> form_submission(dom::Element const& form, dom::Element const* submitter,
    layout::ControlStates const* states, net::Url const& document_url);

// multipart/form-data (RFC 7578) of the fields, between lines of
// `--boundary`: each a part named by its field, a quote, a line feed and a
// carriage return in the name written %22, %0A and %0D. And a boundary no
// field's text holds, the same for the same fields.
std::string multipart_form(std::vector<FormField> const& fields, std::string const& boundary);
std::string multipart_boundary_for(std::vector<FormField> const& fields);

// text/plain: name=value lines, each ended by CR LF, nothing escaped.
std::string plain_text_form(std::vector<FormField> const& fields);

// A form's first submit button: what Enter in a text field presses.
dom::Element const* default_submitter(dom::Element const& form);

// The page's controls that can take focus, in tree order.
std::vector<dom::Element const*> focusable_controls(dom::Document const& document);

// The first form control with this name (the --script handle), hidden ones included.
dom::Element const* control_named(dom::Document const& document, std::string_view name);

// The element with this id, anywhere in the document.
dom::Element const* element_by_id(dom::Document const& document, std::string_view id);

}
