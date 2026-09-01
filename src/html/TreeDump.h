#pragma once

// Serializes a parsed tree in the html5lib tree-construction format ("| "
// prefixed lines). This is both the conformance runner's comparator and the
// output of `sashfold --dump-dom`.

#include "dom/Dom.h"

#include <string>

namespace sashfold::html {

std::string dump_document(dom::Document const&);

// Dumps only the children of a node (fragment results hang under the
// builder's root <html> element).
std::string dump_children(dom::Node const&);

}
