#pragma once

// The HTML fragment serialization algorithm (WHATWG HTML §13.3): the text
// innerHTML and outerHTML give back for a tree. Text is escaped as the
// specification says — & < > and the no-break space in text, & " and the
// no-break space in attribute values — except inside the elements whose
// content the parser took raw (style, script, xmp, iframe, noembed,
// noframes, plaintext, and noscript when scripting is on); void elements
// have no end tag; a template's contents stand in for its children.

#include "dom/Dom.h"

#include <string>

namespace sashfold::html {

// The children of `node` serialized in order (innerHTML).
std::string serialize_children(dom::Node const& node, bool scripting = true);

// The node itself with its children (outerHTML).
std::string serialize_node(dom::Node const& node, bool scripting = true);

// The text content of a node (DOM §4.4 textContent): the concatenation of
// every descendant Text node's data, in tree order.
std::string text_content(dom::Node const& node);

}
