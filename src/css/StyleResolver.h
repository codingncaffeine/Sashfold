#pragma once

// The cascade: UA stylesheet + document <style> sheets + style attributes,
// ordered by importance, origin, specificity, and source order; inheritance
// and value computation produce a ComputedStyle per element.

#include "css/ComputedStyle.h"
#include "css/Stylesheets.h"

#include <unordered_map>
#include <vector>

namespace sashfold::dom {
class Document;
class Element;
}

namespace sashfold::css {

using StyleMap = std::unordered_map<dom::Element const*, ComputedStyle>;

// Cascades the author sheets, in the order given, with the built-in UA
// stylesheet and the style="" attributes, and computes styles for every
// element (subtrees under display:none still get entries).
StyleMap resolve_styles(dom::Document const& document, std::vector<SheetSource> const& sheets);

// The same over the document's <style> elements alone: nothing is fetched.
StyleMap resolve_styles(dom::Document const& document);

}
