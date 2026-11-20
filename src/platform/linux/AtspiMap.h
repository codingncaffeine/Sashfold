#pragma once

// The accessibility model in AT-SPI2's terms: its roles, states and
// relations by the numbers the protocol's interface descriptions give them
// (at-spi2-core's xml/Accessible.xml documents GetRole, GetState and
// GetRelationSet), and the mapping of the model's roles and states to them
// that Core-AAM 1.2 (§3.4 "Role Mapping Tables", §3.5 states and
// properties: the ATK/AT-SPI column), HTML-AAM (§3.5 the elements' ATK
// rows), DPUB-AAM and Graphics-AAM give.

#include "a11y/Tree.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::platform::atspi {

// AtspiRole, by its number.
namespace role {
inline constexpr std::uint32_t invalid = 0, alert = 2, canvas = 6, check_box = 7, check_menu_item = 8, column_header = 10,
    combo_box = 11, dialog = 16, filler = 20, frame = 23, image = 27, internal_frame = 28, label = 29, list = 31, list_item = 32,
    menu = 33, menu_bar = 34, menu_item = 35, page_tab = 37, page_tab_list = 38, panel = 39, password_text = 40,
    popup_menu = 41, progress_bar = 42, push_button = 43, radio_button = 44, radio_menu_item = 45, row_header = 47,
    scroll_bar = 48, scroll_pane = 49, separator = 50, slider = 51, spin_button = 52, status_bar = 54, table = 55,
    table_cell = 56, text = 61, toggle_button = 62, tool_bar = 63, tool_tip = 64, tree = 65, tree_table = 66, unknown = 67,
    window = 69, header = 71, footer = 72, paragraph = 73, application = 75, autocomplete = 76, embedded = 78, entry = 79,
    caption = 81, document_frame = 82, heading = 83, section = 85, form = 87, link = 88, table_row = 90, tree_item = 91,
    document_web = 95, comment = 97, list_box = 98, grouping = 99, image_map = 100, notification = 101, info_bar = 102,
    level_bar = 103, title_bar = 104, block_quote = 105, audio = 106, video = 107, definition = 108, article = 109,
    landmark = 110, log = 111, marquee = 112, math = 113, timer = 115, static_ = 116, subscript = 119, superscript = 120,
    description_list = 121, description_term = 122, description_value = 123, footnote = 124, content_deletion = 125,
    content_insertion = 126, mark = 127, suggestion = 128, push_button_menu = 129, switch_ = 130;
}

// AtspiStateType, by its number: bit n of the two 32-bit words GetState answers.
namespace state {
inline constexpr unsigned active = 1, armed = 2, busy = 3, checked = 4, collapsed = 5, defunct = 6, editable = 7, enabled = 8,
    expandable = 9, expanded = 10, focusable = 11, focused = 12, horizontal = 14, iconified = 15, modal = 16, multi_line = 17,
    multiselectable = 18, opaque = 19, pressed = 20, resizable = 21, selectable = 22, selected = 23, sensitive = 24,
    showing = 25, single_line = 26, stale = 27, transient = 28, vertical = 29, visible = 30, manages_descendants = 31,
    indeterminate = 32, required = 33, truncated = 34, animated = 35, invalid_entry = 36, supports_autocompletion = 37,
    selectable_text = 38, is_default = 39, visited = 40, checkable = 41, has_popup = 42, read_only = 43;
}
using StateSet = std::uint64_t;
constexpr StateSet bit(unsigned s) { return StateSet { 1 } << s; }
// The state names libatspi and Core-AAM use ("focused", "multi-line").
std::string_view state_name(unsigned s);

// The role's name as GetRoleName answers it ("push button", "document web").
std::string_view role_name(std::uint32_t role);

// What the bridge knows of a node beyond its data, for its role and states.
struct Context {
    bool chrome = false; // one of the window's own nodes, not a page's
    bool frame = false; // the window itself
    bool window_active = true;
    bool document_shown = true; // the document is the tab in front
    bool has_text = false; // it offers the Text interface
    bool editable_text = false;
};

// The node's AT-SPI role (Core-AAM's and HTML-AAM's mapping).
std::uint32_t role_of(a11y::NodeData const& node, Context const& context);
// Its states, as GetState's two words read as one.
StateSet states_of(a11y::NodeData const& node, Context const& context);
// The model's state named in a StateChanged event, as AT-SPI names it
// (empty when it has no AT-SPI state), and the state number.
std::pair<std::string_view, unsigned> state_of_event(a11y::State state, a11y::NodeData const* node);
// AtspiRelationType of a relation of the model's; 0 when it has none.
std::uint32_t relation_of(a11y::Relation relation);
// The node's object attributes (Core-AAM "Object Attribute" rows):
// xml-roles, tag, level, posinset, setsize, live, relevant, atomic,
// roledescription, keyshortcuts, current, haspopup, autocomplete,
// placeholder-text, sort, invalid, text-input-type.
std::vector<std::pair<std::string, std::string>> attributes_of(a11y::NodeData const& node);

}
