#pragma once

// WebDriver's accessibility commands (WebDriver §12.4.9 "Get Computed
// Role", §12.4.10 "Get Computed Label"), and the tentative accessibility
// properties ones testdriver.js asks for, answered from the model: what a
// test runner's testdriver vendor script calls through
// bindings::Realm::install_automation. They exist for automation alone; no
// page reaches them.

#include "dom/Dom.h"

#include <optional>
#include <string>
#include <string_view>

namespace sashfold::a11y {

class Semantics;
class Tree;

// "computed_role" and "computed_label" of an element; "properties" of an
// element and "node_properties" of a node by its id (`argument`), as the
// JSON object testdriver hands the test (role, label, description, the
// states, and `children`: the ids of the node's children in the tree).
// Nothing for a command it does not know or a node that is not an element.
std::optional<std::string> automation_answer(Semantics& semantics, Tree* tree, std::string_view command, dom::Node* node,
    std::string_view argument);

// The testdriver vendor script that puts the commands in testdriver's hands.
std::string_view testdriver_vendor_script();

}
