#include "a11y/Automation.h"

#include "a11y/Semantics.h"
#include "a11y/Tree.h"

#include <cstdlib>
#include <string>

namespace sashfold::a11y {

namespace {

std::string json_string(std::string_view text)
{
    std::string out = "\"";
    for (char const c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

// The properties testdriver hands a test: the WebDriver-style names its
// accessibility commands use (accessibilityId, role, label, the states as
// "true", "false" or "mixed").
std::string properties_json(NodeData const& data)
{
    std::string out = "{";
    auto field = [&](std::string_view name, std::string const& value) {
        if (out.size() > 1)
            out += ',';
        out += json_string(name);
        out += ':';
        out += value;
    };
    field("accessibilityId", std::to_string(data.id));
    field("parent", std::to_string(data.parent));
    std::string children = "[";
    for (std::size_t i = 0; i < data.children.size(); ++i)
        children += (i ? "," : "") + std::to_string(data.children[i]);
    field("children", children + "]");
    std::string const role(role_name(data.role));
    field("role", json_string(role));
    field("label", json_string(data.name));
    field("description", json_string(data.description));
    auto const has = [&](State s) { return (data.states & state_bit(s)) != 0; };
    if (has(State::Checkable))
        field("checked", json_string(has(State::Mixed) ? "mixed" : has(State::Checked) ? "true" : "false"));
    if (has(State::Pressable))
        field("pressed", json_string(has(State::Mixed) ? "mixed" : has(State::Pressed) ? "true" : "false"));
    if (has(State::Selectable))
        field("selected", json_string(has(State::Selected) ? "true" : "false"));
    if (has(State::Expandable))
        field("expanded", json_string(has(State::Expanded) ? "true" : "false"));
    field("disabled", json_string(has(State::Disabled) ? "true" : "false"));
    field("focusable", json_string(has(State::Focusable) ? "true" : "false"));
    if (data.level)
        field("level", std::to_string(data.level));
    return out + "}";
}

}

std::optional<std::string> automation_answer(Semantics& semantics, Tree* tree, std::string_view command, dom::Node* node,
    std::string_view argument)
{
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
    if (command == "properties" && tree) {
        if (!node || !node->is_element())
            return std::nullopt;
        std::optional<NodeData> const data = tree->data_for(static_cast<dom::Element const&>(*node));
        if (!data)
            return std::nullopt;
        return properties_json(*data);
    }
    if (command == "node_properties" && tree) {
        NodeId const id = std::strtoull(std::string(argument).c_str(), nullptr, 10);
        NodeData const* data = tree->node(id);
        if (!data)
            return std::nullopt;
        return properties_json(*data);
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
