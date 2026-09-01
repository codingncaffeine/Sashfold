#pragma once

// The cascade: UA stylesheet + document <style> sheets + style attributes,
// ordered by importance, origin, specificity, and source order; inheritance
// and value computation produce a ComputedStyle per element.

#include "css/ComputedStyle.h"

#include <unordered_map>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::css {

using StyleMap = std::unordered_map<dom::Element const*, ComputedStyle>;

// Gathers <style> sheet text and style="" attributes from the document,
// cascades them with the built-in UA stylesheet, and computes styles for
// every element (subtrees under display:none still get entries).
StyleMap resolve_styles(dom::Document const& document);

}
