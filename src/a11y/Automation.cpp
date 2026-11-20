#include "a11y/Automation.h"

#include "a11y/Semantics.h"

namespace sashfold::a11y {

std::optional<std::string> automation_answer(Semantics& semantics, Tree* tree, std::string_view command, dom::Node* node,
    std::string_view argument)
{
    (void)tree;
    (void)argument;
    if (command == "computed_role") {
        if (!node || !node->is_element())
            return std::nullopt;
        return semantics.computed_role_string(static_cast<dom::Element const&>(*node));
    }
    if (command == "computed_label") {
        if (!node || !node->is_element())
            return std::nullopt;
        return semantics.name(static_cast<dom::Element const&>(*node));
    }
    return std::nullopt;
}

std::string_view testdriver_vendor_script()
{
    return R"JS(window.test_driver_internal.in_automation = true;
(function () {
  var automation = window.__sashfold_automation;
  delete window.__sashfold_automation;
  if (typeof automation !== "function")
    return;
  var internal = window.test_driver_internal;
  function ask(command, node, argument) {
    var answer = automation(command, node, argument === undefined ? null : argument);
    if (answer === null)
      throw new Error(command + " has no answer for this node");
    return answer;
  }
  internal.get_computed_role = async function (element) { return ask("computed_role", element); };
  internal.get_computed_label = async function (element) { return ask("computed_label", element); };
  internal.get_accessibility_properties_for_element = async function (element) {
    return JSON.parse(ask("properties", element));
  };
  internal.get_accessibility_properties_for_accessibility_node = async function (id) {
    return JSON.parse(ask("node_properties", null, String(id)));
  };
})();
)JS";
}

}
