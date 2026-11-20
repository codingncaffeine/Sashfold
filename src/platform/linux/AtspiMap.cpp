#include "platform/linux/AtspiMap.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace sashfold::platform::atspi {

using a11y::NodeData;
using a11y::Role;
using a11y::State;

namespace {

// The roles' names, in the order of their numbers (Accessible.xml, GetRole).
constexpr std::array<std::string_view, 131> role_names = {
    "invalid", "accelerator label", "alert", "animation", "arrow", "calendar", "canvas", "check box", "check menu item",
    "color chooser", "column header", "combo box", "date editor", "desktop icon", "desktop frame", "dial", "dialog",
    "directory pane", "drawing area", "file chooser", "filler", "focus traversable", "font chooser", "frame", "glass pane",
    "html container", "icon", "image", "internal frame", "label", "layered pane", "list", "list item", "menu", "menu bar",
    "menu item", "option pane", "page tab", "page tab list", "panel", "password text", "popup menu", "progress bar",
    "push button", "radio button", "radio menu item", "root pane", "row header", "scroll bar", "scroll pane", "separator",
    "slider", "spin button", "split pane", "status bar", "table", "table cell", "table column header", "table row header",
    "tearoff menu item", "terminal", "text", "toggle button", "tool bar", "tool tip", "tree", "tree table", "unknown",
    "viewport", "window", "extended", "header", "footer", "paragraph", "ruler", "application", "autocomplete", "editbar",
    "embedded", "entry", "chart", "caption", "document frame", "heading", "page", "section", "redundant object", "form",
    "link", "input method window", "table row", "tree item", "document spreadsheet", "document presentation",
    "document text", "document web", "document email", "comment", "list box", "grouping", "image map", "notification",
    "info bar", "level bar", "title bar", "block quote", "audio", "video", "definition", "article", "landmark", "log",
    "marquee", "math", "rating", "timer", "static", "math fraction", "math root", "subscript", "superscript",
    "description list", "description term", "description value", "footnote", "content deletion", "content insertion",
    "mark", "suggestion", "push button menu", "switch",
};

constexpr std::array<std::string_view, 44> state_names = {
    "invalid", "active", "armed", "busy", "checked", "collapsed", "defunct", "editable", "enabled", "expandable",
    "expanded", "focusable", "focused", "has-tooltip", "horizontal", "iconified", "modal", "multi-line", "multiselectable",
    "opaque", "pressed", "resizable", "selectable", "selected", "sensitive", "showing", "single-line", "stale", "transient",
    "vertical", "visible", "manages-descendants", "indeterminate", "required", "truncated", "animated", "invalid-entry",
    "supports-autocompletion", "selectable-text", "is-default", "visited", "checkable", "has-popup", "read-only",
};

bool has(NodeData const& node, State s) { return (node.states & a11y::state_bit(s)) != 0; }

}

std::string_view state_name(unsigned s) { return s < state_names.size() ? state_names[s] : std::string_view(); }

std::string_view role_name(std::uint32_t r) { return r < role_names.size() ? role_names[r] : std::string_view("unknown"); }

std::uint32_t role_of(NodeData const& node, Context const& context)
{
    if (context.frame)
        return role::frame;
    switch (node.role) {
    // Core-AAM §3.4.
    case Role::Alert: return role::notification;
    case Role::AlertDialog: return role::alert;
    case Role::Application: return role::embedded;
    case Role::Article: return role::article;
    case Role::Banner:
    case Role::Complementary:
    case Role::ContentInfo:
    case Role::Main:
    case Role::Navigation:
    case Role::Search:
        return role::landmark;
    case Role::Blockquote: return role::block_quote;
    case Role::Button:
        // A toggle button is one that has aria-pressed (role-map-button-pressed);
        // one with a popup is a push button (role-map-button-haspopup).
        return has(node, State::Pressable) ? role::toggle_button : role::push_button;
    case Role::Caption: return role::caption;
    case Role::Cell:
    case Role::GridCell:
        return role::table_cell;
    case Role::Checkbox: return role::check_box;
    case Role::Code:
    case Role::Emphasis:
    case Role::Strong:
    case Role::Time:
        return role::static_;
    case Role::ColumnHeader: return role::column_header;
    case Role::Combobox: return role::combo_box;
    case Role::Comment:
    case Role::Note:
        return role::comment;
    case Role::Definition: return role::description_value;
    case Role::Deletion: return role::content_deletion;
    case Role::Insertion: return role::content_insertion;
    case Role::Dialog: return role::dialog;
    case Role::Document: return role::document_frame;
    case Role::Feed:
    case Role::Figure:
    case Role::Group:
    case Role::RadioGroup:
    case Role::RowGroup:
        return role::panel;
    // A form or a region with no name is no landmark: its element's own
    // mapping (HTML-AAM form: ROLE_FORM; section: ROLE_SECTION).
    case Role::Form: return node.name.empty() ? role::form : role::landmark;
    case Role::Region: return node.name.empty() ? role::section : role::landmark;
    case Role::Generic: return role::section;
    case Role::Grid:
    case Role::Table:
        return role::table;
    case Role::Heading: return role::heading;
    case Role::Image: return role::image;
    case Role::Link: return role::link;
    case Role::List: return role::list;
    case Role::ListBox: return role::list_box;
    case Role::ListItem:
    case Role::Option:
        return role::list_item;
    case Role::Log: return role::log;
    case Role::Mark: return role::mark;
    case Role::Marquee: return role::marquee;
    case Role::Math: return role::math;
    case Role::Menu: return role::menu;
    case Role::MenuBar: return role::menu_bar;
    case Role::MenuItem: return role::menu_item;
    case Role::MenuItemCheckbox: return role::check_menu_item;
    case Role::MenuItemRadio: return role::radio_menu_item;
    case Role::Meter: return role::level_bar;
    case Role::Paragraph: return role::paragraph;
    case Role::ProgressBar: return role::progress_bar;
    case Role::Radio: return role::radio_button;
    case Role::Row: return role::table_row;
    case Role::RowHeader: return role::row_header;
    case Role::ScrollBar: return role::scroll_bar;
    case Role::SearchBox:
    case Role::TextBox:
        return role::entry;
    case Role::SectionFooter: return role::footer;
    case Role::SectionHeader: return role::header;
    case Role::Separator: return role::separator;
    case Role::Slider: return role::slider;
    case Role::SpinButton: return role::spin_button;
    case Role::Status: return role::status_bar;
    case Role::Subscript: return role::subscript;
    case Role::Superscript: return role::superscript;
    case Role::Suggestion: return role::suggestion;
    case Role::Switch: return role::toggle_button; // role-map-switch, with xml-roles:switch
    case Role::Tab: return role::page_tab;
    case Role::TabList: return role::page_tab_list;
    case Role::TabPanel: return role::scroll_pane;
    case Role::Term: return role::description_term;
    case Role::Timer: return role::timer;
    case Role::Toolbar: return role::tool_bar;
    case Role::Tooltip: return role::tool_tip;
    case Role::Tree: return role::tree;
    case Role::TreeGrid: return role::tree_table;
    case Role::TreeItem: return role::tree_item;
    case Role::Window: return context.chrome ? role::window : role::panel;
    // DPUB-AAM 1.1: the sections that are landmarks, the ones that are
    // not, and the few that have a role of their own; xml-roles on all.
    case Role::DocAcknowledgments:
    case Role::DocAfterword:
    case Role::DocAppendix:
    case Role::DocBibliography:
    case Role::DocChapter:
    case Role::DocConclusion:
    case Role::DocCredits:
    case Role::DocEndnotes:
    case Role::DocEpilogue:
    case Role::DocErrata:
    case Role::DocForeword:
    case Role::DocGlossary:
    case Role::DocIndex:
    case Role::DocIntroduction:
    case Role::DocPagelist:
    case Role::DocPart:
    case Role::DocPreface:
    case Role::DocPrologue:
    case Role::DocToc:
        return role::landmark;
    case Role::DocAbstract:
    case Role::DocColophon:
    case Role::DocCredit:
    case Role::DocDedication:
    case Role::DocEpigraph:
    case Role::DocExample:
    case Role::DocPullquote:
    case Role::DocQna:
    case Role::DocPagefooter:
    case Role::DocPageheader:
        return role::section;
    case Role::DocBacklink:
    case Role::DocBiblioref:
    case Role::DocGlossref:
    case Role::DocNoteref:
        return role::link;
    case Role::DocBiblioentry:
    case Role::DocEndnote:
        return role::list_item;
    case Role::DocCover: return role::image;
    case Role::DocFootnote: return role::footnote;
    case Role::DocNotice:
    case Role::DocTip:
        return role::comment;
    case Role::DocPagebreak: return role::separator;
    case Role::DocSubtitle: return role::heading;
    // Graphics-AAM.
    case Role::GraphicsDocument: return role::document_frame;
    case Role::GraphicsObject: return role::panel;
    case Role::GraphicsSymbol: return role::image;
    // HTML-AAM's elements with no ARIA role.
    case Role::RootWebArea: return context.chrome ? role::panel : role::document_web;
    case Role::StaticText: return role::static_;
    case Role::LineBreak: return role::static_;
    case Role::Label:
    case Role::Legend:
        return role::label;
    case Role::FigCaption: return role::caption;
    case Role::Abbr:
    case Role::Cite:
    case Role::Kbd:
    case Role::Var:
    case Role::Ruby:
        return role::static_;
    case Role::Audio: return role::audio;
    case Role::Video: return role::video;
    case Role::Canvas: return role::canvas;
    case Role::Embed: return role::embedded;
    case Role::IFrame: return role::internal_frame;
    case Role::DescriptionList: return role::description_list;
    case Role::ListMarker: return role::static_;
    case Role::DisclosureTriangle: return role::toggle_button;
    case Role::ImageMap: return role::image_map;
    // What no element computes (abstract roles, none), should one come.
    default: return role::section;
    }
}

StateSet states_of(NodeData const& node, Context const& context)
{
    StateSet set = 0;
    if (context.frame) {
        set |= bit(state::enabled) | bit(state::sensitive) | bit(state::resizable) | bit(state::showing) | bit(state::visible);
        if (context.window_active)
            set |= bit(state::active);
        return set;
    }
    if (!has(node, State::Disabled))
        set |= bit(state::enabled) | bit(state::sensitive);
    // Core-AAM §5.6 "State and Property Mapping", the ATK/AT-SPI column.
    struct Pair {
        State from;
        unsigned to;
    };
    static constexpr Pair direct[] = {
        { State::Focusable, state::focusable }, { State::Focused, state::focused }, { State::Checkable, state::checkable },
        { State::Mixed, state::indeterminate }, { State::Selectable, state::selectable },
        { State::Selected, state::selected }, { State::Expandable, state::expandable }, { State::Expanded, state::expanded },
        { State::Required, state::required }, { State::Invalid, state::invalid_entry }, { State::ReadOnly, state::read_only },
        { State::Editable, state::editable }, { State::Multiline, state::multi_line },
        { State::Multiselectable, state::multiselectable }, { State::Horizontal, state::horizontal },
        { State::Vertical, state::vertical }, { State::Modal, state::modal }, { State::Busy, state::busy },
        { State::Visited, state::visited }, { State::HasPopup, state::has_popup }, { State::Default, state::is_default },
    };
    for (Pair const& pair : direct) {
        if (has(node, pair.from))
            set |= bit(pair.to);
    }
    // aria-checked true: checked; aria-pressed true: pressed (a toggle
    // button); mixed is indeterminate either way.
    if (has(node, State::Checked))
        set |= bit(state::checked);
    if (has(node, State::Pressed))
        set |= bit(state::pressed);
    // Expandable and not expanded: collapsed.
    if (has(node, State::Expandable) && !has(node, State::Expanded))
        set |= bit(state::collapsed);
    // A text field that is not multi-line is single-line (role-map-textbox).
    if (context.editable_text && !has(node, State::Multiline))
        set |= bit(state::single_line);
    if (context.has_text)
        set |= bit(state::selectable_text);
    if (!node.autocomplete.empty() && node.autocomplete != "none" && node.autocomplete != "off")
        set |= bit(state::supports_autocompletion);
    // Visible: rendered (visibility: hidden content is kept but is not);
    // showing: visible and on the screen, in the tab in front.
    if (!has(node, State::Invisible)) {
        set |= bit(state::visible);
        if (!has(node, State::Offscreen) && context.document_shown)
            set |= bit(state::showing);
    }
    // A document is focusable, and read-only unless its content is editable.
    if (node.role == Role::RootWebArea && !context.chrome) {
        set |= bit(state::focusable);
        if (!has(node, State::Editable))
            set |= bit(state::read_only);
    }
    return set;
}

std::pair<std::string_view, unsigned> state_of_event(State s, NodeData const* node)
{
    switch (s) {
    case State::Focusable: return { "focusable", state::focusable };
    case State::Focused: return { "focused", state::focused };
    case State::Disabled: return { "enabled", state::enabled }; // reported as its opposite
    case State::Checkable: return { "checkable", state::checkable };
    case State::Checked: return { "checked", state::checked };
    case State::Mixed: return { "indeterminate", state::indeterminate };
    case State::Pressable: return { {}, 0 };
    case State::Pressed: return { node && node->role == Role::Button ? "pressed" : "checked", node && node->role == Role::Button ? state::pressed : state::checked };
    case State::Selectable: return { "selectable", state::selectable };
    case State::Selected: return { "selected", state::selected };
    case State::Expandable: return { "expandable", state::expandable };
    case State::Expanded: return { "expanded", state::expanded };
    case State::Required: return { "required", state::required };
    case State::Invalid: return { "invalid-entry", state::invalid_entry };
    case State::ReadOnly: return { "read-only", state::read_only };
    case State::Editable: return { "editable", state::editable };
    case State::Multiline: return { "multi-line", state::multi_line };
    case State::Multiselectable: return { "multiselectable", state::multiselectable };
    case State::Horizontal: return { "horizontal", state::horizontal };
    case State::Vertical: return { "vertical", state::vertical };
    case State::Modal: return { "modal", state::modal };
    case State::Busy: return { "busy", state::busy };
    case State::Visited: return { "visited", state::visited };
    case State::HasPopup: return { "has-popup", state::has_popup };
    case State::Default: return { "is-default", state::is_default };
    case State::Offscreen: return { "showing", state::showing }; // reported as its opposite
    case State::Invisible: return { "visible", state::visible }; // reported as its opposite
    default: return { {}, 0 };
    }
}

std::uint32_t relation_of(a11y::Relation relation)
{
    using a11y::Relation;
    switch (relation) {
    case Relation::LabelFor: return 1;
    case Relation::LabelledBy: return 2;
    case Relation::Controls: return 3; // controller-for
    case Relation::ControlledBy: return 4;
    case Relation::FlowsTo: return 10;
    case Relation::FlowsFrom: return 11;
    case Relation::DescriptionFor: return 17;
    case Relation::DescribedBy: return 18;
    case Relation::Details: return 19;
    case Relation::DetailsFor: return 20;
    case Relation::ErrorMessage: return 21;
    case Relation::ErrorFor: return 22;
    // aria-owns re-parents the owned node, and aria-activedescendant is
    // an event (active-descendant-changed) and a Collection question, not
    // a relation, in Core-AAM's ATK column.
    default: return 0;
    }
}

std::vector<std::pair<std::string, std::string>> attributes_of(NodeData const& node)
{
    std::vector<std::pair<std::string, std::string>> attributes;
    auto const add = [&attributes](std::string name, std::string value) {
        if (!value.empty())
            attributes.emplace_back(std::move(name), std::move(value));
    };
    auto const number = [](int value) { return value > 0 ? std::to_string(value) : std::string(); };
    // xml-roles: the ARIA role a node computes, which Core-AAM lists for
    // the roles AT-SPI has no role of its own for, and every engine gives
    // for every ARIA role; not for the generic container or text.
    std::string_view const aria = a11y::role_name(node.role);
    if (!aria.empty() && node.role != Role::Generic && node.role != Role::None && node.role != Role::StaticText)
        add("xml-roles", std::string(aria));
    add("tag", node.tag);
    add("level", number(node.level));
    add("posinset", number(node.pos_in_set));
    add("setsize", number(node.set_size));
    if (!node.live.empty()) {
        add("live", node.live);
        add("container-live", node.live);
        add("relevant", node.relevant);
        add("container-relevant", node.relevant);
        add("atomic", node.atomic ? "true" : "false");
        add("container-atomic", node.atomic ? "true" : "false");
        std::string_view const live_role = a11y::implicit_live(node.role);
        if (!live_role.empty())
            add("container-live-role", std::string(aria));
    }
    add("roledescription", node.role_description);
    add("keyshortcuts", node.key_shortcuts);
    add("current", node.current);
    add("haspopup", node.has_popup);
    add("autocomplete", node.autocomplete);
    add("placeholder-text", node.placeholder);
    add("sort", node.sort);
    add("invalid", node.invalid);
    if (node.role == Role::SearchBox)
        add("text-input-type", "search");
    if (node.role == Role::Table || node.role == Role::Grid || node.role == Role::TreeGrid) {
        add("rowcount", number(node.row_count));
        add("colcount", number(node.column_count));
    }
    if (node.value_now && !node.value_text.empty() && node.role != Role::TextBox && node.role != Role::SearchBox)
        add("valuetext", node.value_text);
    return attributes;
}

}
