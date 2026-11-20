#include "a11y/Tree.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "css/ComputedStyle.h"
#include "dom/Forms.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace sashfold::a11y {

using dom::Element;
using dom::Node;

namespace {

std::atomic<std::uint64_t> g_instances { 0 };
std::atomic<NodeId> g_next_id { 1 };

NodeId new_id()
{
    return g_next_id.fetch_add(1, std::memory_order_relaxed);
}

std::optional<std::string_view> attribute(Element const& element, std::string_view name)
{
    if (dom::Attr const* found = element.find_attribute(name))
        return std::string_view(found->value);
    return std::nullopt;
}

bool blank(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; });
}

std::string lowered(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// The number of UTF-16 code units the UTF-8 text is.
std::uint32_t utf16_length(std::string_view text)
{
    std::uint32_t units = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto const byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xC0) == 0x80)
            continue;
        units += byte >= 0xF0 ? 2 : 1;
    }
    return units;
}

// The byte offset at a code point boundary at or before `at`.
std::size_t boundary_before(std::string_view text, std::size_t at)
{
    while (at > 0 && at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        --at;
    return at;
}

std::optional<double> number(std::optional<std::string_view> text)
{
    if (!text)
        return std::nullopt;
    double value = 0;
    std::string const trimmed = collapse_whitespace(*text);
    if (trimmed.empty() || !dom::forms::parse_number(trimmed, value))
        return std::nullopt;
    return value;
}

bool is_text_field(Element const& element)
{
    if (element.is_html("textarea"))
        return true;
    if (!element.is_html("input"))
        return false;
    std::string const type = dom::forms::input_type(element);
    return type == "text" || type == "search" || type == "url" || type == "tel" || type == "email" || type == "password" || type == "number";
}

bool is_cell(Role role)
{
    return role == Role::Cell || role == Role::GridCell || role == Role::ColumnHeader || role == Role::RowHeader;
}

bool is_table(Role role)
{
    return role == Role::Table || role == Role::Grid || role == Role::TreeGrid;
}

// The names the dump and the bridge give the roles ARIA has no name for.
std::string_view platform_role_name(Role role)
{
    switch (role) {
    case Role::RootWebArea: return "RootWebArea";
    case Role::StaticText: return "StaticText";
    case Role::LineBreak: return "LineBreak";
    case Role::Label: return "Label";
    case Role::Legend: return "Legend";
    case Role::FigCaption: return "FigCaption";
    case Role::Abbr: return "Abbr";
    case Role::Audio: return "Audio";
    case Role::Video: return "Video";
    case Role::Canvas: return "Canvas";
    case Role::Embed: return "Embed";
    case Role::IFrame: return "Iframe";
    case Role::Cite: return "Cite";
    case Role::Kbd: return "Kbd";
    case Role::Var: return "Var";
    case Role::Ruby: return "Ruby";
    case Role::DescriptionList: return "DescriptionList";
    case Role::ListMarker: return "ListMarker";
    case Role::DisclosureTriangle: return "DisclosureTriangle";
    case Role::ImageMap: return "ImageMap";
    default: return role_name(role);
    }
}

constexpr std::array<std::string_view, static_cast<std::size_t>(State::Count_)> state_names = {
    "focusable", "focused", "disabled", "checkable", "checked", "mixed", "pressable", "pressed", "selectable", "selected",
    "expandable", "expanded", "required", "invalid", "readonly", "editable", "multiline", "multiselectable", "horizontal",
    "vertical", "modal", "busy", "visited", "linked", "haspopup", "default", "offscreen", "invisible",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Relation::Count_)> relation_names = {
    "labelled-by", "label-for", "described-by", "description-for", "details", "details-for", "controls", "controlled-by",
    "flows-to", "flows-from", "error-message", "error-for", "active-descendant", "owns", "owned-by",
};

// What identifies a node across updates: the DOM node, and what of it.
struct Key {
    void const* dom = nullptr;
    NodeData::Kind kind = NodeData::Kind::Element;
    int part = 0; // ::before 0, ::after 1; a presentational child's text leaf, its index
    bool operator==(Key const& other) const { return dom == other.dom && kind == other.kind && part == other.part; }
};
struct KeyHash {
    std::size_t operator()(Key const& key) const
    {
        return std::hash<void const*>()(key.dom) ^ (static_cast<std::size_t>(key.kind) << 3) ^ (static_cast<std::size_t>(key.part) << 7);
    }
};

}

std::string_view state_name(State state)
{
    return state_names[static_cast<std::size_t>(state)];
}

std::string_view relation_name(Relation relation)
{
    return relation_names[static_cast<std::size_t>(relation)];
}

std::string_view event_name(Event::Type type)
{
    switch (type) {
    case Event::Type::ChildrenChanged: return "children-changed";
    case Event::Type::NameChanged: return "name-changed";
    case Event::Type::DescriptionChanged: return "description-changed";
    case Event::Type::RoleChanged: return "role-changed";
    case Event::Type::StateChanged: return "state-changed";
    case Event::Type::ValueChanged: return "value-changed";
    case Event::Type::TextInserted: return "text-inserted";
    case Event::Type::TextRemoved: return "text-removed";
    case Event::Type::CaretMoved: return "caret-moved";
    case Event::Type::TextSelectionChanged: return "text-selection-changed";
    case Event::Type::SelectionChanged: return "selection-changed";
    case Event::Type::Focus: return "focus";
    case Event::Type::BoundsChanged: return "bounds-changed";
    case Event::Type::Announcement: return "announcement";
    case Event::Type::LoadComplete: return "load-complete";
    case Event::Type::TreeChanged: return "tree-changed";
    }
    return "";
}

// --- The tree's state -----------------------------------------------------------------------

struct Tree::Impl {
    Tree& tree;
    explicit Impl(Tree& owner)
        : tree(owner)
    {
    }

    bool built = false;
    std::uint64_t tree_version = 0;
    std::uint64_t attribute_version = 0;
    std::uint64_t state_version = 0;
    std::uint64_t generation = 0;
    std::uint32_t read_clock = 0; // the document's style clock at the last update
    std::uint64_t updates = 0;
    // The selection of the focused text field, which moves without the
    // document noting it.
    std::optional<dom::forms::TextSelection> field_selection;
    Element const* focused_element = nullptr;

    NodeId root = 0;
    std::unordered_map<Key, NodeId, KeyHash> ids;
    std::unordered_map<NodeId, Key> keys;
    std::unordered_map<NodeId, NodeData> nodes;
    std::unordered_map<dom::Node const*, NodeId> by_dom; // elements and text nodes only

    // Names kept from the last update, with what they were computed under.
    struct CachedName {
        std::string name;
        std::string description;
        Semantics::NameSource source = Semantics::NameSource::None;
    };
    std::unordered_map<Element const*, CachedName> names;

    std::shared_ptr<Snapshot const> published;
    std::unordered_set<NodeId> changed; // since the last publication

    std::mutex query_lock;
    std::vector<Query> queries;
    std::function<void()> page_wake;

    // --- Building ---
    std::unordered_map<NodeId, NodeData> next;
    std::unordered_map<dom::Node const*, NodeId> next_by_dom;
    std::uint32_t since = 0; // the clock marks are compared with this update
    bool everything = false; // styles or layout moved: nothing kept
    Element const* modal = nullptr;

    NodeId id_for(Key const& key)
    {
        auto const found = ids.find(key);
        if (found != ids.end())
            return found->second;
        NodeId const id = new_id();
        ids.emplace(key, id);
        keys.emplace(id, key);
        return id;
    }
};

Tree::Tree(dom::Document& document, PageSource* source)
    : m_impl(std::make_unique<Impl>(*this))
    , m_document(&document)
    , m_semantics(source)
{
    g_instances.fetch_add(1, std::memory_order_relaxed);
}

Tree::~Tree() = default;

std::uint64_t Tree::instances()
{
    return g_instances.load(std::memory_order_relaxed);
}

bool Tree::stale() const
{
    Impl const& in = *m_impl;
    if (!in.built)
        return true;
    PageSource* source = m_semantics.source();
    std::uint64_t const generation = source ? source->generation() : 0;
    if (in.tree_version != m_document->tree_version() || in.attribute_version != m_document->attribute_version()
        || in.state_version != m_document->state_version() || in.generation != generation)
        return true;
    if (in.focused_element != m_document->focused())
        return true;
    if (!m_document->aria_notifications.empty())
        return true;
    // A text node's data changing moves no version: the style marks it
    // leaves on its way up to the document say it.
    Node::StyleMarks const& marks = m_document->style_marks();
    if (marks.descendants >= in.read_clock || marks.children >= in.read_clock || marks.self >= in.read_clock)
        return true;
    if (in.focused_element && source && is_text_field(*in.focused_element)) {
        std::optional<dom::forms::TextSelection> const now = source->text_selection(*in.focused_element);
        if (now.has_value() != in.field_selection.has_value() || (now && !now->same_range(*in.field_selection)))
            return true;
    }
    return false;
}

// --- Building one update -------------------------------------------------------------------

namespace {

struct Builder {
    Tree::Impl& in;
    Semantics& semantics;
    dom::Document& document;
    PageSource* source;

    css::ComputedStyle const* style(Element const& element) { return semantics.style(element); }

    // Whether the element's own change marks, or those of what is below it,
    // say it changed since the last update.
    bool self_changed(Node const& node) const
    {
        Node::StyleMarks const& marks = node.style_marks();
        return marks.self >= in.since || marks.subtree >= in.since;
    }
    bool below_changed(Node const& node) const
    {
        Node::StyleMarks const& marks = node.style_marks();
        return marks.children >= in.since || marks.descendants >= in.since;
    }

    // Whether an element stands in the tree itself, rather than leaving its
    // place to its children.
    bool included(Element const& element, Role role)
    {
        if (element.is_html("html") || element.is_html("body"))
            return false;
        if (role == Role::Unknown || role == Role::None)
            return false;
        if (role == Role::Generic) {
            if (semantics.focusable(element))
                return true;
            if (std::optional<std::string_view> const live = semantics.aria(element, "aria-live"); live && !blank(*live))
                return true;
            if (element.has_attribute("aria-owns") || semantics.aria_owner(element))
                return true;
            if (std::optional<std::string_view> const label = semantics.aria(element, "aria-label"); label && !blank(*label))
                return true;
            if (element.has_attribute("aria-labelledby") || element.has_attribute("aria-describedby"))
                return true;
            if (dom::forms::editing_host_editable(element))
                return true;
            return false;
        }
        return true;
    }

    NodeData& make(Key const& key, NodeId parent)
    {
        NodeId const id = in.id_for(key);
        NodeData& data = in.next[id];
        data = NodeData {};
        data.id = id;
        data.parent = parent;
        data.kind = key.kind;
        if (parent != 0)
            in.next[parent].children.push_back(id);
        return data;
    }

    void add_text_leaf(std::string text, NodeData::Kind kind, dom::Node const* dom, void const* key_dom, int part, NodeId parent)
    {
        if (text.empty())
            return;
        NodeData& leaf = make(Key { key_dom, kind, part }, parent);
        leaf.role = kind == NodeData::Kind::Marker ? Role::ListMarker : Role::StaticText;
        leaf.dom = dom;
        leaf.name = text;
        leaf.text = std::move(text);
        if (dom && dom->is_text() && source) {
            std::vector<Rect> const boxes = source->text_boxes(*dom, 0, utf16_length(static_cast<dom::Text const&>(*dom).data));
            if (!boxes.empty()) {
                Rect all = boxes.front();
                for (Rect const& r : boxes) {
                    float const right = std::max(all.x + all.width, r.x + r.width);
                    float const bottom = std::max(all.y + all.height, r.y + r.height);
                    all.x = std::min(all.x, r.x);
                    all.y = std::min(all.y, r.y);
                    all.width = right - all.x;
                    all.height = bottom - all.y;
                }
                leaf.bounds = all;
            }
        }
    }

    // The text a text node contributes; nothing for white space that is
    // not drawn.
    std::optional<std::string> text_of(dom::Text const& text)
    {
        if (semantics.hidden(text))
            return std::nullopt;
        std::string rendered = semantics.rendered_text(text);
        if (blank(rendered)) {
            // White space is a node only where layout drew it.
            if (!source || source->text_boxes(text, 0, utf16_length(text.data)).empty())
                return std::nullopt;
        }
        return rendered;
    }

    void generated(Element const& element, bool before, NodeId parent)
    {
        css::ComputedStyle const* computed = style(element);
        if (!computed || !computed->generated)
            return;
        std::optional<css::GeneratedBox> const& box = before ? computed->generated->before : computed->generated->after;
        if (!box || box->style.display == css::Display::None)
            return;
        std::string text = box->alt ? *box->alt : box->text;
        if (text.empty())
            return;
        add_text_leaf(std::move(text), NodeData::Kind::Generated, &element, &element, before ? 0 : 1, parent);
    }

    void marker(Element const& element, NodeId parent)
    {
        css::ComputedStyle const* computed = style(element);
        if (!computed || computed->display != css::Display::ListItem || computed->list_style_type == css::ListStyleType::None)
            return;
        css::ListStyleType const type = computed->list_style_type;
        std::string text = css::format_counter(computed->list_item_value, type);
        if (type != css::ListStyleType::Disc && type != css::ListStyleType::Circle && type != css::ListStyleType::Square)
            text += '.';
        text += ' ';
        add_text_leaf(std::move(text), NodeData::Kind::Marker, &element, &element, 0, parent);
    }

    // All the text below a node with presentational children, as leaves of it.
    void flatten_text(Node const& node, NodeId parent, int& part)
    {
        for (Node* child : semantics.content_children(node)) {
            if (child->is_text()) {
                if (std::optional<std::string> text = text_of(static_cast<dom::Text const&>(*child))) {
                    add_text_leaf(std::move(*text), NodeData::Kind::Text, child, child, 0, parent);
                    in.next_by_dom[child] = in.next[parent].children.back();
                }
            } else if (child->is_element()) {
                auto const& element = static_cast<Element const&>(*child);
                if (semantics.hidden(element) && !semantics.visibility_hidden_only(element))
                    continue;
                flatten_text(element, parent, part);
            }
        }
    }

    void children_of(Node const& node, NodeId parent)
    {
        for (Node* child : semantics.content_children(node))
            visit(*child, parent);
    }

    void visit(Node const& node, NodeId parent)
    {
        if (node.is_text()) {
            if (std::optional<std::string> text = text_of(static_cast<dom::Text const&>(node))) {
                add_text_leaf(std::move(*text), NodeData::Kind::Text, &node, &node, 0, parent);
                in.next_by_dom[&node] = in.next[parent].children.back();
            }
            return;
        }
        if (!node.is_element())
            return;
        auto const& element = static_cast<Element const&>(node);
        if (semantics.hidden(element)) {
            // visibility: hidden leaves the element out, not what inside
            // it is visible again.
            if (!semantics.aria_hidden(element) && semantics.visibility_hidden_only(element))
                children_of(element, parent);
            return;
        }
        if (semantics.inert(element))
            return;
        Role const role = semantics.role(element);
        if (!included(element, role)) {
            generated(element, true, parent);
            children_of(element, parent);
            generated(element, false, parent);
            return;
        }
        NodeData& data = make(Key { &element, NodeData::Kind::Element, 0 }, parent);
        NodeId const id = data.id;
        data.role = role;
        data.dom = &element;
        in.next_by_dom[&element] = id;
        if (element.is_html("br")) {
            in.next[id].kind = NodeData::Kind::LineBreak;
            in.next[id].text = "\n";
            return;
        }
        if (is_text_field(element) || (element.is_html("input")) || element.is_html("img"))
            return; // leaves
        if (info(role).children_presentational) {
            int part = 0;
            flatten_text(element, id, part);
            return;
        }
        if (PageSource* frames = source; frames && (element.is_html("iframe") || element.is_html("frame") || element.is_html("object"))) {
            if (dom::Document* inner = frames->frame_document(element)) {
                NodeData& sub = make(Key { inner, NodeData::Kind::Document, 0 }, id);
                sub.role = Role::RootWebArea;
                sub.dom = inner;
                NodeId const sub_id = sub.id;
                children_of(*inner, sub_id);
                return;
            }
        }
        marker(element, id);
        generated(element, true, id);
        children_of(element, id);
        generated(element, false, id);
    }

    // --- Properties ---

    void fill(NodeData& data)
    {
        if (data.kind == NodeData::Kind::Document) {
            data.states |= state_bit(State::Focusable);
            if (data.dom == &document && !document.focused())
                data.states |= state_bit(State::Focused);
            // The document's title: the first <title> in it.
            dom::for_each_shadow_including(*data.dom, [&](Node const& node) {
                if (data.name.empty() && node.is_element() && static_cast<Element const&>(node).is_html("title")) {
                    std::string text;
                    for (Node const* t : node.children()) {
                        if (t->is_text())
                            text += static_cast<dom::Text const&>(*t).data;
                    }
                    data.name = collapse_whitespace(text);
                }
            });
            return;
        }
        if (data.kind != NodeData::Kind::Element && data.kind != NodeData::Kind::LineBreak)
            return;
        auto const& element = static_cast<Element const&>(*data.dom);
        data.tag = element.local_name();
        fill_name(data, element);
        fill_states(data, element);
        fill_properties(data, element);
        fill_value(data, element);
        if (source)
            data.bounds = source->box(element);
        fill_actions(data, element);
    }

    void fill_name(NodeData& data, Element const& element)
    {
        // A name is kept from the last update when nothing it can read has
        // changed: the element and what is below it unmarked, no restyle,
        // and no relation or label that reads other parts of the page.
        bool const reads_elsewhere = element.has_attribute("aria-labelledby") || element.has_attribute("aria-describedby")
            || element.has_attribute("aria-owns") || element.has_attribute("aria-actions") || element.is_html("input")
            || element.is_html("select") || element.is_html("textarea") || element.is_html("button") || element.is_html("meter")
            || element.is_html("output") || element.is_html("progress") || element.document().form_associated_custom.contains(&element);
        auto const kept = in.names.find(&element);
        if (!in.everything && !reads_elsewhere && kept != in.names.end() && !self_changed(element) && !below_changed(element)
            && !ancestor_changed(element)) {
            data.name = kept->second.name;
            data.description = kept->second.description;
            data.name_from = kept->second.source;
            return;
        }
        Tree::Impl::CachedName computed;
        computed.name = semantics.name(element);
        computed.source = semantics.name_source(element);
        computed.description = semantics.description(element);
        data.name = computed.name;
        data.description = computed.description;
        data.name_from = computed.source;
        in.names[&element] = std::move(computed);
    }

    bool ancestor_changed(Element const& element) const
    {
        for (Node const* at = dom::flat_parent(element); at; at = dom::flat_parent(*at)) {
            if (at->style_marks().self >= in.since || at->style_marks().subtree >= in.since)
                return true;
        }
        return false;
    }

    void fill_states(NodeData& data, Element const& element)
    {
        Role const role = data.role;
        States& states = data.states;
        auto set = [&](State s, bool on = true) {
            if (on)
                states |= state_bit(s);
        };
        set(State::Focusable, semantics.focusable(element));
        set(State::Focused, document.focused() == &element);
        bool const native_control = element.is_html("input") || element.is_html("button") || element.is_html("select")
            || element.is_html("textarea") || element.is_html("option") || element.is_html("optgroup") || element.is_html("fieldset");
        if (native_control || is_widget(role) || element.has_attribute("aria-disabled"))
            set(State::Disabled, semantics.disabled(element));

        // Checked: ARIA's aria-checked, a native checkbox or radio's
        // checkedness (Core-AAM §5.6 aria-checked; HTML-AAM §5.2).
        bool const checkable = role == Role::Checkbox || role == Role::Switch || role == Role::Radio || role == Role::MenuItemCheckbox
            || role == Role::MenuItemRadio || (role == Role::Option && element.has_attribute("aria-checked"))
            || (role == Role::TreeItem && element.has_attribute("aria-checked"));
        if (checkable) {
            set(State::Checkable);
            std::optional<std::string_view> const value = semantics.aria(element, "aria-checked");
            bool const native = element.is_html("input")
                && (dom::forms::input_type(element) == "checkbox" || dom::forms::input_type(element) == "radio");
            if (native) {
                set(State::Checked, dom::forms::checkedness(element));
            } else if (value) {
                std::string const token = lowered(collapse_whitespace(*value));
                set(State::Checked, token == "true");
                set(State::Mixed, token == "mixed" && role != Role::Radio && role != Role::Switch && role != Role::MenuItemRadio);
            }
        }
        // Pressed: a toggle button.
        if (role == Role::Button) {
            if (std::optional<std::string_view> const value = semantics.aria(element, "aria-pressed")) {
                std::string const token = lowered(collapse_whitespace(*value));
                if (token == "true" || token == "false" || token == "mixed") {
                    set(State::Pressable);
                    set(State::Pressed, token == "true");
                    set(State::Mixed, token == "mixed");
                }
            }
        }
        // Selected.
        bool const selectable = role == Role::Option || role == Role::Tab || role == Role::Row || role == Role::GridCell
            || role == Role::TreeItem || role == Role::ColumnHeader || role == Role::RowHeader;
        if (selectable) {
            if (element.is_html("option")) {
                set(State::Selectable);
                set(State::Selected, dom::forms::option_selected(element));
            } else {
                std::optional<bool> const selected = semantics.aria_bool(element, "aria-selected");
                if (selected || role == Role::Option || role == Role::Tab || role == Role::TreeItem)
                    set(State::Selectable);
                set(State::Selected, selected.value_or(false));
            }
        }
        // Expanded.
        if (std::optional<bool> const expanded = semantics.aria_bool(element, "aria-expanded")) {
            set(State::Expandable);
            set(State::Expanded, *expanded);
        } else if (element.is_html("details")) {
            set(State::Expandable);
            set(State::Expanded, element.has_attribute("open"));
        } else if (role == Role::DisclosureTriangle) {
            set(State::Expandable);
            Node const* parent = element.parent();
            set(State::Expanded, parent && parent->is_element() && static_cast<Element const&>(*parent).has_attribute("open"));
        } else if (role == Role::Combobox && element.is_html("select")) {
            set(State::Expandable);
        }
        // Required, read-only, editable, multi-line, multi-selectable.
        std::optional<bool> const required = semantics.aria_bool(element, "aria-required");
        bool const native_required = (element.is_html("input") || element.is_html("select") || element.is_html("textarea"))
            && element.has_attribute("required");
        set(State::Required, required.value_or(false) || native_required);
        bool const text_field = is_text_field(element);
        bool const readonly_attr = text_field && element.has_attribute("readonly");
        set(State::ReadOnly, semantics.aria_bool(element, "aria-readonly").value_or(false) || readonly_attr);
        bool const editing = dom::forms::editing_host_editable(element);
        if ((text_field && !readonly_attr && !semantics.disabled(element)) || editing)
            set(State::Editable);
        set(State::Multiline, element.is_html("textarea") || semantics.aria_bool(element, "aria-multiline").value_or(false)
                || (editing && role != Role::TextBox));
        set(State::Multiselectable, (element.is_html("select") && element.has_attribute("multiple"))
                || semantics.aria_bool(element, "aria-multiselectable").value_or(false));
        // Invalid: aria-invalid's token, or the control's own validity,
        // where its value is wrong rather than merely absent.
        if (std::optional<std::string_view> const invalid = semantics.aria(element, "aria-invalid")) {
            std::string const token = lowered(collapse_whitespace(*invalid));
            if (!token.empty() && token != "false") {
                data.invalid = token == "grammar" || token == "spelling" ? token : "true";
                set(State::Invalid);
            }
        } else if (dom::forms::validation_candidate(element)) {
            dom::forms::Validity validity = dom::forms::validity_state(element);
            validity.value_missing = false;
            if (!validity.valid()) {
                data.invalid = "true";
                set(State::Invalid);
            }
        }
        // Orientation.
        std::string orientation;
        if (std::optional<std::string_view> const value = semantics.aria(element, "aria-orientation"))
            orientation = lowered(collapse_whitespace(*value));
        if (orientation != "horizontal" && orientation != "vertical") {
            orientation = std::string(implicit_value(role, Aria::Orientation));
            if (element.is_html("input") && dom::forms::input_type(element) == "range")
                orientation = "horizontal";
        }
        set(State::Horizontal, orientation == "horizontal");
        set(State::Vertical, orientation == "vertical");
        set(State::Modal, (role == Role::Dialog || role == Role::AlertDialog) && semantics.aria_bool(element, "aria-modal").value_or(false));
        set(State::Busy, semantics.aria_bool(element, "aria-busy").value_or(false));
        if (role == Role::Link) {
            set(State::Linked);
            set(State::Visited, source && source->visited(element));
        }
        if (std::optional<std::string_view> const popup = semantics.aria(element, "aria-haspopup")) {
            std::string const token = lowered(collapse_whitespace(*popup));
            if (!token.empty() && token != "false") {
                data.has_popup = token == "true" || token == "menu" ? "menu" : token;
                set(State::HasPopup);
            }
        } else if (role == Role::Combobox) {
            data.has_popup = "listbox";
            set(State::HasPopup);
        }
        if (element.is_html("button") || (element.is_html("input") && dom::forms::input_type(element) == "submit")) {
            // The form's default button: its first submit button.
            if (dom::forms::attribute_lower(element, "type") == "submit" || (element.is_html("button") && dom::forms::attribute_lower(element, "type").empty())) {
                Element const* form = nullptr;
                for (Element const* at = dom::forms::element_parent(element); at; at = dom::forms::element_parent(*at)) {
                    if (at->is_html("form")) {
                        form = at;
                        break;
                    }
                }
                if (form) {
                    Element const* first = nullptr;
                    dom::for_each_shadow_including(*form, [&](Node const& node) {
                        if (first || !node.is_element())
                            return;
                        auto const& candidate = static_cast<Element const&>(node);
                        std::string const type = dom::forms::attribute_lower(candidate, "type");
                        if ((candidate.is_html("button") && (type.empty() || type == "submit"))
                            || (candidate.is_html("input") && (type == "submit" || type == "image")))
                            first = &candidate;
                    });
                    set(State::Default, first == &element);
                }
            }
        }
    }

    void fill_properties(NodeData& data, Element const& element)
    {
        Role const role = data.role;
        auto text = [&](std::string_view name) -> std::string {
            std::optional<std::string_view> const value = semantics.aria(element, name);
            return value ? collapse_whitespace(*value) : std::string();
        };
        data.role_description = text("aria-roledescription");
        data.key_shortcuts = text("aria-keyshortcuts");
        data.placeholder = text("aria-placeholder");
        if (data.placeholder.empty() && (element.is_html("input") || element.is_html("textarea"))) {
            if (std::optional<std::string_view> const placeholder = attribute(element, "placeholder"))
                data.placeholder = collapse_whitespace(*placeholder);
        }
        data.autocomplete = lowered(text("aria-autocomplete"));
        if (data.autocomplete == "none")
            data.autocomplete.clear();
        if (data.autocomplete.empty() && (role == Role::Combobox && element.is_html("input")))
            data.autocomplete = "list";
        std::string const current = lowered(text("aria-current"));
        if (!current.empty() && current != "false") {
            static constexpr std::string_view tokens[] = { "page", "step", "location", "date", "time", "true" };
            data.current = std::find(std::begin(tokens), std::end(tokens), current) != std::end(tokens) ? current : "true";
        }
        data.sort = lowered(text("aria-sort"));
        if (data.sort == "none")
            data.sort.clear();
        // Live regions: the element's own aria-live or its role's, then the
        // region it is in passes its settings down (ARIA 1.2 §6.6 aria-live).
        std::string live = lowered(text("aria-live"));
        if (live.empty())
            live = std::string(implicit_live(role));
        if (!live.empty()) {
            data.live = live;
            std::string const atomic = lowered(text("aria-atomic"));
            data.atomic = atomic.empty() ? implicit_value(role, Aria::Atomic) == "true" : atomic == "true";
            data.relevant = lowered(text("aria-relevant"));
            if (data.relevant.empty())
                data.relevant = "additions text";
        }
        // Heading level (HTML-AAM §5.2: h1 to h6; aria-level wins).
        if (std::optional<double> const level = number(semantics.aria(element, "aria-level")); level && *level >= 1)
            data.level = static_cast<int>(*level);
        else if (role == Role::Heading) {
            std::string const& name = element.local_name();
            data.level = element.is_html() && name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' ? name[1] - '0' : 2;
        }
        if (role == Role::Link) {
            if (std::optional<std::string_view> const href = attribute(element, "href"))
                data.url = std::string(*href);
            else if (dom::Attr const* xlink = element.find_attribute("xlink:href"))
                data.url = xlink->value;
        }
        for (Element const* at = &element; at; at = dom::forms::element_parent(*at)) {
            if (std::optional<std::string_view> const lang = attribute(*at, "lang")) {
                data.language = std::string(*lang);
                break;
            }
        }
    }

    void fill_value(NodeData& data, Element const& element)
    {
        Role const role = data.role;
        bool const range = is_a(role, Role::Range) || role == Role::ScrollBar || (role == Role::Separator && semantics.focusable(element));
        if (range) {
            data.value_now = number(semantics.aria(element, "aria-valuenow"));
            data.value_min = number(semantics.aria(element, "aria-valuemin"));
            data.value_max = number(semantics.aria(element, "aria-valuemax"));
            if (element.is_html("input")) {
                std::string const type = dom::forms::input_type(element);
                double const value = dom::forms::value_as_number(type, dom::forms::control_value(element));
                if (!std::isnan(value) && !data.value_now)
                    data.value_now = value;
                if (!data.value_min)
                    data.value_min = number(attribute(element, "min"));
                if (!data.value_max)
                    data.value_max = number(attribute(element, "max"));
                if (type == "range") {
                    if (!data.value_min)
                        data.value_min = 0;
                    if (!data.value_max)
                        data.value_max = 100;
                }
                data.value_step = number(attribute(element, "step"));
            } else if (element.is_html("progress")) {
                if (!data.value_max)
                    data.value_max = number(attribute(element, "max")).value_or(1);
                if (!data.value_min)
                    data.value_min = 0;
                if (!data.value_now)
                    data.value_now = number(attribute(element, "value"));
            } else if (element.is_html("meter")) {
                if (!data.value_min)
                    data.value_min = number(attribute(element, "min")).value_or(0);
                if (!data.value_max)
                    data.value_max = number(attribute(element, "max")).value_or(1);
                if (!data.value_now)
                    data.value_now = number(attribute(element, "value")).value_or(0);
            } else {
                // ARIA's defaults (ARIA 1.2 §6.6 aria-valuemin/max): 0 and
                // 100, the value half way between when it is required and
                // missing (a slider, a scrollbar).
                if (!data.value_min && !implicit_value(role, Aria::ValueMin).empty())
                    data.value_min = 0;
                if (!data.value_max && !implicit_value(role, Aria::ValueMax).empty())
                    data.value_max = 100;
                if (!data.value_now && (role == Role::Slider || role == Role::ScrollBar) && data.value_min && data.value_max)
                    data.value_now = (*data.value_min + *data.value_max) / 2;
            }
            if (std::optional<std::string_view> const text = semantics.aria(element, "aria-valuetext"))
                data.value_text = collapse_whitespace(*text);
        }
        if (is_text_field(element)) {
            std::string value = dom::forms::control_value(element);
            if (element.is_html("input") && dom::forms::input_type(element) == "password") {
                // A password field's text is its mask: one bullet a character.
                std::size_t const count = decode_utf8(value, true).size();
                value.clear();
                for (std::size_t i = 0; i < count; ++i)
                    value += "•";
            }
            data.text = value;
            data.value_text = value;
            if (source) {
                if (std::optional<dom::forms::TextSelection> const selection = source->text_selection(element)) {
                    std::u32string const decoded = decode_utf8(value, true);
                    auto const units = [&](std::size_t points) {
                        std::uint32_t out = 0;
                        for (std::size_t i = 0; i < points && i < decoded.size(); ++i)
                            out += decoded[i] > 0xFFFF ? 2 : 1;
                        return out;
                    };
                    data.text_selection = std::make_pair(units(selection->start), units(selection->end));
                    data.caret = units(selection->focus());
                }
            }
        } else if (role == Role::Combobox && element.is_html("select")) {
            std::string selected;
            dom::for_each_shadow_including(element, [&](Node const& node) {
                if (selected.empty() && node.is_element() && static_cast<Element const&>(node).is_html("option")
                    && dom::forms::option_selected(static_cast<Element const&>(node)))
                    selected = semantics.name(static_cast<Element const&>(node));
            });
            data.value_text = selected;
        } else if (dom::forms::editing_host_editable(element)) {
            std::string all;
            dom::for_each_shadow_including(element, [&](Node const& node) {
                if (node.is_text())
                    all += static_cast<dom::Text const&>(node).data;
            });
            data.text = all;
        }
    }

    void fill_actions(NodeData& data, Element const& element)
    {
        Role const role = data.role;
        std::vector<std::string>& actions = data.actions;
        if (role == Role::Link)
            actions.emplace_back("jump");
        else if (role == Role::Button || role == Role::MenuItem)
            actions.emplace_back("press");
        else if (data.states & state_bit(State::Checkable))
            actions.emplace_back(data.states & state_bit(State::Checked) ? "uncheck" : "check");
        else if (role == Role::Option || role == Role::Tab || role == Role::TreeItem)
            actions.emplace_back("select");
        else if (role == Role::DisclosureTriangle || (data.states & state_bit(State::Expandable)))
            actions.emplace_back(data.states & state_bit(State::Expanded) ? "collapse" : "expand");
        else if (is_text_field(element) || data.states & state_bit(State::Editable))
            actions.emplace_back("activate");
        if (data.states & state_bit(State::Focusable))
            actions.emplace_back("focus");
        if (data.bounds)
            actions.emplace_back("scroll-into-view");
    }

    // --- After the structure: what reads the other nodes ---

    void relations(NodeData& data)
    {
        if (data.kind != NodeData::Kind::Element)
            return;
        auto const& element = static_cast<Element const&>(*data.dom);
        auto const ids = [&](std::vector<Element*> const& elements) {
            std::vector<NodeId> out;
            for (Element* target : elements) {
                auto const found = in.next_by_dom.find(target);
                if (found != in.next_by_dom.end())
                    out.push_back(found->second);
            }
            return out;
        };
        auto const add = [&](Relation relation, std::vector<NodeId> targets) {
            if (targets.empty())
                return;
            data.relations.emplace_back(relation, targets);
            Relation reverse = Relation::Count_;
            switch (relation) {
            case Relation::LabelledBy: reverse = Relation::LabelFor; break;
            case Relation::DescribedBy: reverse = Relation::DescriptionFor; break;
            case Relation::Details: reverse = Relation::DetailsFor; break;
            case Relation::Controls: reverse = Relation::ControlledBy; break;
            case Relation::FlowsTo: reverse = Relation::FlowsFrom; break;
            case Relation::ErrorMessage: reverse = Relation::ErrorFor; break;
            case Relation::Owns: reverse = Relation::OwnedBy; break;
            default: break;
            }
            if (reverse == Relation::Count_)
                return;
            for (NodeId target : targets) {
                auto& list = in.next[target].relations;
                auto found = std::find_if(list.begin(), list.end(), [&](auto const& entry) { return entry.first == reverse; });
                if (found == list.end())
                    list.emplace_back(reverse, std::vector<NodeId> { data.id });
                else
                    found->second.push_back(data.id);
            }
        };
        std::vector<NodeId> labelled = ids(semantics.idrefs(element, "aria-labelledby"));
        if (labelled.empty() && data.name_from == Semantics::NameSource::Label) {
            // The label elements that name a control.
            dom::for_each_shadow_including(static_cast<Node const&>(document), [&](Node const& node) {
                if (node.is_element() && static_cast<Element const&>(node).is_html("label")) {
                    auto const& label = static_cast<Element const&>(node);
                    std::optional<std::string_view> const target = attribute(label, "for");
                    bool labels_this = target ? semantics.element_by_id(label, *target) == &element : false;
                    if (!target) {
                        for (Node const* at = element.parent(); at; at = at->parent()) {
                            if (at == &label) {
                                labels_this = true;
                                break;
                            }
                        }
                    }
                    if (labels_this) {
                        auto const found = in.next_by_dom.find(&label);
                        if (found != in.next_by_dom.end())
                            labelled.push_back(found->second);
                    }
                }
            });
        }
        add(Relation::LabelledBy, labelled);
        add(Relation::DescribedBy, ids(semantics.idrefs(element, "aria-describedby")));
        add(Relation::Details, ids(semantics.idrefs(element, "aria-details")));
        add(Relation::Controls, ids(semantics.idrefs(element, "aria-controls")));
        add(Relation::FlowsTo, ids(semantics.idrefs(element, "aria-flowto")));
        add(Relation::ErrorMessage, ids(semantics.idrefs(element, "aria-errormessage")));
        if (Element* active = semantics.idref(element, "aria-activedescendant")) {
            std::vector<NodeId> target = ids({ active });
            if (!target.empty())
                data.relations.emplace_back(Relation::ActiveDescendant, target);
        }
        add(Relation::Owns, ids(semantics.idrefs(element, "aria-owns")));
    }

    // posinset and setsize (ARIA 1.2 §6.6: computed from the DOM when the
    // author did not give them): the item's place among its siblings of
    // the same role, inside the same container.
    void positions(NodeData& data)
    {
        Role const role = data.role;
        bool const item = role == Role::ListItem || role == Role::Option || role == Role::TreeItem || role == Role::Tab
            || role == Role::MenuItem || role == Role::MenuItemCheckbox || role == Role::MenuItemRadio || role == Role::Radio
            || (role == Role::Article && in.next[data.parent].role == Role::Feed) || (role == Role::Row && is_a(role, Role::Row));
        if (!item || data.kind != NodeData::Kind::Element)
            return;
        auto const& element = static_cast<Element const&>(*data.dom);
        std::optional<double> const pos = number(semantics.aria(element, "aria-posinset"));
        std::optional<double> const size = number(semantics.aria(element, "aria-setsize"));
        if (role == Role::Row) {
            // Rows count only in a tree grid, where they are a tree's items.
            NodeId container = data.parent;
            while (container && !is_table(in.next[container].role))
                container = in.next[container].parent;
            if (!container || in.next[container].role != Role::TreeGrid)
                return;
        }
        std::vector<NodeId> peers;
        if (role == Role::Radio && element.is_html("input")) {
            dom::forms::each_in_radio_group(element, [&](Element const& radio) {
                auto const found = in.next_by_dom.find(&radio);
                if (found != in.next_by_dom.end())
                    peers.push_back(found->second);
            });
        } else {
            // The siblings in the container, through nodes that are only
            // groupings of the same items (an optgroup's options count in
            // their group).
            NodeId const parent = data.parent;
            if (parent) {
                for (NodeId sibling : in.next[parent].children) {
                    Role const other = in.next[sibling].role;
                    bool const same = other == role
                        || (is_a(role, Role::MenuItem) && is_a(other, Role::MenuItem))
                        || (role == Role::ListItem && other == Role::ListItem);
                    if (same)
                        peers.push_back(sibling);
                }
            }
        }
        auto const at = std::find(peers.begin(), peers.end(), data.id);
        data.pos_in_set = pos ? static_cast<int>(*pos) : at == peers.end() ? 0 : static_cast<int>(at - peers.begin()) + 1;
        data.set_size = size ? static_cast<int>(*size) : static_cast<int>(peers.size());
        // A list item's or a tree item's level: how many lists or groups
        // of items it is nested in.
        if (!data.level && (role == Role::ListItem || role == Role::TreeItem)) {
            int level = 0;
            for (NodeId up = data.parent; up; up = in.next[up].parent) {
                Role const r = in.next[up].role;
                if ((role == Role::ListItem && r == Role::List) || (role == Role::TreeItem && (r == Role::Group || r == Role::Tree)))
                    ++level;
                if (role == Role::TreeItem && r == Role::Tree)
                    break;
            }
            data.level = std::max(1, level);
        }
    }

    // Tables (Core-AAM §5.8, HTML's table model §4.9.12 for the spans):
    // the rows and cells of each table placed on a grid.
    void table(NodeData& table)
    {
        if (!is_table(table.role))
            return;
        std::vector<NodeId> rows;
        auto const collect = [&](auto const& self, NodeId at) -> void {
            for (NodeId child : in.next[at].children) {
                NodeData const& data = in.next[child];
                if (data.role == Role::Row)
                    rows.push_back(child);
                else if (!is_table(data.role) && data.kind == NodeData::Kind::Element)
                    self(self, child);
            }
        };
        collect(collect, table.id);
        std::vector<std::vector<bool>> taken;
        int columns = 0;
        auto const span = [&](NodeData const& cell, std::string_view aria, std::string_view html) {
            auto const& element = static_cast<Element const&>(*cell.dom);
            std::optional<double> value = number(semantics.aria(element, aria));
            if (!value && element.is_html())
                value = number(attribute(element, html));
            return value && *value >= 1 ? static_cast<int>(*value) : 1;
        };
        for (std::size_t r = 0; r < rows.size(); ++r) {
            NodeData& row = in.next[rows[r]];
            row.row_index = static_cast<int>(r);
            if (taken.size() <= r)
                taken.resize(r + 1);
            int column = 0;
            std::vector<NodeId> cells;
            auto const cells_of = [&](auto const& self, NodeId at) -> void {
                for (NodeId child : in.next[at].children) {
                    if (is_cell(in.next[child].role))
                        cells.push_back(child);
                    else if (in.next[child].kind == NodeData::Kind::Element && !is_table(in.next[child].role) && in.next[child].role != Role::Row)
                        self(self, child);
                }
            };
            cells_of(cells_of, rows[r]);
            for (NodeId id : cells) {
                NodeData& cell = in.next[id];
                while (column < static_cast<int>(taken[r].size()) && taken[r][static_cast<std::size_t>(column)])
                    ++column;
                cell.row_index = static_cast<int>(r);
                cell.column_index = column;
                cell.column_span = span(cell, "aria-colspan", "colspan");
                cell.row_span = span(cell, "aria-rowspan", "rowspan");
                for (int dr = 0; dr < cell.row_span; ++dr) {
                    std::size_t const rr = r + static_cast<std::size_t>(dr);
                    if (rr >= rows.size())
                        break;
                    if (taken.size() <= rr)
                        taken.resize(rr + 1);
                    auto& line = taken[rr];
                    if (line.size() < static_cast<std::size_t>(column + cell.column_span))
                        line.resize(static_cast<std::size_t>(column + cell.column_span), false);
                    for (int dc = 0; dc < cell.column_span; ++dc)
                        line[static_cast<std::size_t>(column + dc)] = true;
                }
                column += cell.column_span;
                columns = std::max(columns, column);
            }
        }
        auto const& element = static_cast<Element const&>(*table.dom);
        std::optional<double> const row_count = number(semantics.aria(element, "aria-rowcount"));
        std::optional<double> const column_count = number(semantics.aria(element, "aria-colcount"));
        table.row_count = row_count ? static_cast<int>(*row_count) : static_cast<int>(rows.size());
        table.column_count = column_count ? static_cast<int>(*column_count) : columns;
        // The author's own indices (1-based) stand in for the computed ones.
        for (NodeId row_id : rows) {
            NodeData& row = in.next[row_id];
            std::optional<double> const index = number(semantics.aria(static_cast<Element const&>(*row.dom), "aria-rowindex"));
            if (index && *index >= 1)
                row.row_index = static_cast<int>(*index) - 1;
            for (NodeId cell_id : row.children) {
                NodeData& cell = in.next[cell_id];
                if (!is_cell(cell.role))
                    continue;
                auto const& cell_element = static_cast<Element const&>(*cell.dom);
                if (std::optional<double> const own = number(semantics.aria(cell_element, "aria-rowindex")); own && *own >= 1)
                    cell.row_index = static_cast<int>(*own) - 1;
                else if (index && *index >= 1)
                    cell.row_index = row.row_index;
                if (std::optional<double> const own = number(semantics.aria(cell_element, "aria-colindex")); own && *own >= 1)
                    cell.column_index = static_cast<int>(*own) - 1;
            }
        }
        // Each cell's header cells: the HTML headers attribute, else the
        // column headers of its column and the row headers of its row.
        std::vector<NodeId> all_cells;
        for (NodeId row_id : rows) {
            for (NodeId cell_id : in.next[row_id].children) {
                if (is_cell(in.next[cell_id].role))
                    all_cells.push_back(cell_id);
            }
        }
        // The headers by the columns and rows they cover, so that finding a
        // cell's is a look at its own column and row.
        std::unordered_map<int, std::vector<NodeId>> column_headers;
        std::unordered_map<int, std::vector<NodeId>> row_headers;
        for (NodeId cell_id : all_cells) {
            NodeData const& cell = in.next[cell_id];
            if (cell.role == Role::ColumnHeader) {
                for (int c = 0; c < cell.column_span; ++c)
                    column_headers[cell.column_index + c].push_back(cell_id);
            } else if (cell.role == Role::RowHeader) {
                for (int r = 0; r < cell.row_span; ++r)
                    row_headers[cell.row_index + r].push_back(cell_id);
            }
        }
        for (NodeId cell_id : all_cells) {
            NodeData& cell = in.next[cell_id];
            auto const& cell_element = static_cast<Element const&>(*cell.dom);
            if (cell_element.is_html() && cell_element.has_attribute("headers")) {
                for (Element* header : semantics.idrefs(cell_element, "headers")) {
                    auto const found = in.next_by_dom.find(header);
                    if (found != in.next_by_dom.end())
                        cell.header_cells.push_back(found->second);
                }
                continue;
            }
            if (auto const found = column_headers.find(cell.column_index); found != column_headers.end()) {
                for (NodeId header : found->second) {
                    if (header != cell_id && in.next[header].row_index < cell.row_index)
                        cell.header_cells.push_back(header);
                }
            }
            if (auto const found = row_headers.find(cell.row_index); found != row_headers.end()) {
                for (NodeId header : found->second) {
                    if (header != cell_id && in.next[header].column_index < cell.column_index)
                        cell.header_cells.push_back(header);
                }
            }
        }
    }
};

}

// --- Update ----------------------------------------------------------------------------------

void Tree::update()
{
    Impl& in = *m_impl;
    if (in.built && !stale())
        return;
    PageSource* source = m_semantics.source();
    std::uint64_t const generation = source ? source->generation() : 0;
    in.everything = !in.built || in.generation != generation;
    in.since = in.read_clock;
    in.read_clock = m_document->advance_style_clock();
    in.tree_version = m_document->tree_version();
    in.attribute_version = m_document->attribute_version();
    in.state_version = m_document->state_version();
    in.generation = generation;
    ++in.updates;

    // Build the structure afresh, keeping each node's id.
    in.next.clear();
    in.next_by_dom.clear();
    Builder builder { in, m_semantics, *m_document, source };
    NodeData& root = builder.make(Key { m_document, NodeData::Kind::Document, 0 }, 0);
    root.role = Role::RootWebArea;
    root.dom = m_document;
    NodeId const root_id = root.id;
    in.next_by_dom[m_document] = root_id;
    builder.children_of(*m_document, root_id);

    // An aria-modal dialog that is shown leaves the rest of the page out
    // (Core-AAM §5.4: the platforms are told only of the dialog).
    NodeId modal = 0;
    for (auto const& [id, data] : in.next) {
        if ((data.role == Role::Dialog || data.role == Role::AlertDialog) && data.kind == NodeData::Kind::Element
            && m_semantics.aria_bool(static_cast<Element const&>(*data.dom), "aria-modal").value_or(false)) {
            modal = id;
            break;
        }
    }
    if (modal) {
        std::unordered_set<NodeId> keep;
        auto const mark = [&](auto const& self, NodeId id) -> void {
            keep.insert(id);
            for (NodeId child : in.next[id].children)
                self(self, child);
        };
        mark(mark, modal);
        for (NodeId up = in.next[modal].parent; up; up = in.next[up].parent)
            keep.insert(up);
        for (auto it = in.next.begin(); it != in.next.end();) {
            if (!keep.contains(it->first)) {
                if (it->second.dom)
                    in.next_by_dom.erase(it->second.dom);
                it = in.next.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& [id, data] : in.next) {
            std::erase_if(data.children, [&](NodeId child) { return !keep.contains(child); });
        }
    }

    for (auto& [id, data] : in.next)
        builder.fill(data);
    for (auto& [id, data] : in.next)
        builder.relations(data);
    for (auto& [id, data] : in.next)
        builder.positions(data);
    for (auto& [id, data] : in.next)
        builder.table(data);

    // --- The changes, as events ---
    std::vector<Event> events;
    auto const post = [&](Event event) { events.push_back(std::move(event)); };
    std::unordered_map<NodeId, std::string> announced; // region → text
    auto const region_of = [&](NodeId id) -> NodeId {
        for (NodeId at = id; at; at = in.next.contains(at) ? in.next[at].parent : 0) {
            NodeData const& data = in.next[at];
            if (!data.live.empty())
                return data.live == "off" ? 0 : at;
        }
        return 0;
    };
    auto const subtree_text = [&](auto const& self, NodeId id) -> std::string {
        NodeData const& data = in.next[id];
        if (data.kind == NodeData::Kind::Text || data.kind == NodeData::Kind::Generated || data.kind == NodeData::Kind::Marker)
            return data.text;
        std::string out;
        if (data.children.empty() && !data.name.empty())
            return data.name;
        for (NodeId child : data.children) {
            std::string const piece = self(self, child);
            if (!piece.empty() && !out.empty() && out.back() != ' ' && piece.front() != ' ')
                out += ' ';
            out += piece;
        }
        return out;
    };
    auto const announce = [&](NodeId changed, std::string const& text, bool addition) {
        NodeId const region = region_of(changed);
        if (!region)
            return;
        NodeData const& data = in.next[region];
        if (data.states & state_bit(State::Busy))
            return;
        std::string const& relevant = data.relevant;
        bool const wanted = relevant.find("all") != std::string::npos
            || (addition ? relevant.find("additions") != std::string::npos || relevant.find("text") != std::string::npos
                         : relevant.find("text") != std::string::npos);
        if (!wanted)
            return;
        std::string& out = announced[region];
        if (data.atomic) {
            out = collapse_whitespace(subtree_text(subtree_text, region));
            return;
        }
        std::string const piece = collapse_whitespace(text);
        if (piece.empty())
            return;
        if (!out.empty())
            out += ' ';
        out += piece;
    };

    if (in.built) {
        // Children and new nodes.
        for (auto const& [id, data] : in.next) {
            auto const old = in.nodes.find(id);
            if (old == in.nodes.end()) {
                // A new node: its parent's children changed (said below);
                // in a live region, its text is an addition.
                if (data.parent && in.nodes.contains(data.parent) && !in.next[data.parent].children.empty())
                    announce(id, subtree_text(subtree_text, id), true);
                continue;
            }
            if (old->second.children != data.children)
                post({ Event::Type::ChildrenChanged, id });
        }
        for (auto const& [id, data] : in.next) {
            auto const old_it = in.nodes.find(id);
            if (old_it == in.nodes.end())
                continue;
            NodeData const& old = old_it->second;
            if (old.role != data.role)
                post({ Event::Type::RoleChanged, id });
            // A text leaf's name is its text, whose change the text events say.
            if (old.name != data.name && data.kind != NodeData::Kind::Text && data.kind != NodeData::Kind::Generated)
                post({ Event::Type::NameChanged, id });
            if (old.description != data.description)
                post({ Event::Type::DescriptionChanged, id });
            States const flipped = old.states ^ data.states;
            for (std::size_t s = 0; s < static_cast<std::size_t>(State::Count_); ++s) {
                State const state = static_cast<State>(s);
                if (state == State::Focused || !(flipped & state_bit(state)))
                    continue;
                Event event { Event::Type::StateChanged, id };
                event.state = state;
                event.on = (data.states & state_bit(state)) != 0;
                post(std::move(event));
                if (state == State::Selected && data.parent)
                    post({ Event::Type::SelectionChanged, data.parent });
            }
            if (old.value_now != data.value_now || old.value_text != data.value_text || old.value_min != data.value_min
                || old.value_max != data.value_max)
                post({ Event::Type::ValueChanged, id });
            if (old.text != data.text) {
                // What was removed and what was put in, between the common
                // start and the common end.
                std::string_view const a = old.text;
                std::string_view const b = data.text;
                std::size_t prefix = 0;
                while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
                    ++prefix;
                prefix = boundary_before(a, prefix);
                prefix = std::min(prefix, boundary_before(b, prefix));
                std::size_t suffix = 0;
                while (suffix < a.size() - prefix && suffix < b.size() - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
                    ++suffix;
                while (suffix > 0 && (static_cast<unsigned char>(a[a.size() - suffix]) & 0xC0) == 0x80)
                    --suffix;
                std::uint32_t const offset = utf16_length(a.substr(0, prefix));
                std::string const removed(a.substr(prefix, a.size() - prefix - suffix));
                std::string const inserted(b.substr(prefix, b.size() - prefix - suffix));
                if (!removed.empty()) {
                    Event event { Event::Type::TextRemoved, id };
                    event.offset = offset;
                    event.text = removed;
                    post(std::move(event));
                }
                if (!inserted.empty()) {
                    Event event { Event::Type::TextInserted, id };
                    event.offset = offset;
                    event.text = inserted;
                    post(std::move(event));
                    announce(id, inserted, false);
                }
            }
            if (old.caret != data.caret && data.caret) {
                Event event { Event::Type::CaretMoved, id };
                event.offset = *data.caret;
                post(std::move(event));
            }
            if (old.text_selection != data.text_selection && data.text_selection && data.text_selection->first != data.text_selection->second)
                post({ Event::Type::TextSelectionChanged, id });
            if (old.bounds != data.bounds)
                post({ Event::Type::BoundsChanged, id });
        }
        // Focus last: the platforms read the focused node's state as it is now.
        Element const* now = m_document->focused();
        if (now != in.focused_element) {
            NodeId target = root_id;
            if (now) {
                auto const found = in.next_by_dom.find(now);
                if (found != in.next_by_dom.end())
                    target = found->second;
            }
            Event focused { Event::Type::StateChanged, target };
            focused.state = State::Focused;
            focused.on = true;
            post(std::move(focused));
            post({ Event::Type::Focus, target });
        }
        for (auto const& [region, text] : announced) {
            if (text.empty())
                continue;
            Event event { Event::Type::Announcement, region };
            event.text = text;
            event.politeness = in.next[region].live == "assertive" ? "assertive" : "polite";
            post(std::move(event));
        }
    }
    // What the page asked to be said (ariaNotify), in the order asked: at
    // its element's node, or the document's.
    for (dom::Document::AriaNotification const& notification : m_document->aria_notifications) {
        NodeId target = root_id;
        if (notification.element) {
            auto const found = in.next_by_dom.find(notification.element);
            if (found != in.next_by_dom.end())
                target = found->second;
        }
        Event event { Event::Type::Announcement, target };
        event.text = notification.text;
        event.politeness = notification.priority == "high" ? "assertive" : "polite";
        post(std::move(event));
    }
    m_document->aria_notifications.clear();

    // Keep the new tree; forget the ids of the nodes that went.
    for (auto const& [id, data] : in.next) {
        auto const old = in.nodes.find(id);
        if (old == in.nodes.end())
            in.changed.insert(id);
        else
            in.changed.insert(id); // compared at publication
    }
    for (auto it = in.keys.begin(); it != in.keys.end();) {
        if (!in.next.contains(it->first)) {
            in.ids.erase(it->second);
            it = in.keys.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = in.names.begin(); it != in.names.end();) {
        if (!in.next_by_dom.contains(it->first))
            it = in.names.erase(it);
        else
            ++it;
    }
    std::swap(in.nodes, in.next);
    std::swap(in.by_dom, in.next_by_dom);
    in.next.clear();
    in.next_by_dom.clear();
    in.root = root_id;
    in.built = true;
    in.focused_element = m_document->focused();
    in.field_selection.reset();
    if (in.focused_element && source && is_text_field(*in.focused_element))
        in.field_selection = source->text_selection(*in.focused_element);

    // Publish: a node that compares equal to its published copy is shared.
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->root = root_id;
    snapshot->generation = in.updates;
    for (auto const& [id, data] : in.nodes) {
        std::shared_ptr<NodeData const> kept;
        if (in.published) {
            auto const found = in.published->nodes.find(id);
            if (found != in.published->nodes.end()) {
                NodeData const& before = *found->second;
                if (before.children == data.children && before.name == data.name && before.description == data.description
                    && before.states == data.states && before.role == data.role && before.text == data.text
                    && before.value_now == data.value_now && before.value_text == data.value_text && before.bounds == data.bounds
                    && before.relations == data.relations && before.parent == data.parent && before.pos_in_set == data.pos_in_set
                    && before.set_size == data.set_size && before.row_index == data.row_index && before.column_index == data.column_index
                    && before.header_cells == data.header_cells && before.caret == data.caret && before.text_selection == data.text_selection
                    && before.level == data.level && before.row_count == data.row_count && before.column_count == data.column_count
                    && before.actions == data.actions && before.live == data.live && before.invalid == data.invalid)
                    kept = found->second;
            }
        }
        snapshot->nodes.emplace(id, kept ? kept : std::make_shared<NodeData const>(data));
    }
    in.published = std::move(snapshot);
    in.changed.clear();

    if (m_observer) {
        m_observer->on_published();
        for (Event const& event : events)
            m_observer->on_event(event);
    }
}

NodeId Tree::root()
{
    update();
    return m_impl->root;
}

NodeData const* Tree::node(NodeId id)
{
    update();
    auto const found = m_impl->nodes.find(id);
    return found == m_impl->nodes.end() ? nullptr : &found->second;
}

NodeId Tree::id_of(dom::Node const& node)
{
    update();
    auto const found = m_impl->by_dom.find(&node);
    return found == m_impl->by_dom.end() ? 0 : found->second;
}

std::optional<NodeData> Tree::data_for(dom::Element const& element)
{
    if (NodeId const id = id_of(element))
        return *node(id);
    // Not in the tree: its own data, computed as a node's would be.
    Impl& in = *m_impl;
    Builder builder { in, m_semantics, *m_document, m_semantics.source() };
    NodeData data;
    data.kind = NodeData::Kind::Element;
    data.dom = &element;
    data.role = m_semantics.role(element);
    in.everything = true;
    builder.fill(data);
    return data;
}

std::shared_ptr<Snapshot const> Tree::snapshot() const
{
    return m_impl->published;
}

void Tree::load_complete()
{
    update();
    if (m_observer)
        m_observer->on_event({ Event::Type::LoadComplete, m_impl->root });
}

void Tree::set_page_wake(std::function<void()> wake)
{
    std::lock_guard<std::mutex> const lock(m_impl->query_lock);
    m_impl->page_wake = std::move(wake);
}

void Tree::post(Query query)
{
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> const lock(m_impl->query_lock);
        m_impl->queries.push_back(std::move(query));
        wake = m_impl->page_wake;
    }
    if (wake)
        wake();
}

void Tree::serve()
{
    std::vector<Query> queries;
    {
        std::lock_guard<std::mutex> const lock(m_impl->query_lock);
        queries.swap(m_impl->queries);
    }
    for (Query& query : queries)
        query(*this);
}

// --- Geometry and actions ------------------------------------------------------------------

NodeId Tree::hit_test(float x, float y)
{
    update();
    PageSource* source = m_semantics.source();
    if (!source)
        return m_impl->root;
    Element const* element = source->element_at(x, y);
    NodeId found = m_impl->root;
    for (Node const* at = element; at; at = m_semantics.content_parent(*at)) {
        auto const it = m_impl->by_dom.find(at);
        if (it != m_impl->by_dom.end()) {
            found = it->second;
            break;
        }
    }
    // Then the text leaf under the point, when the node holds one.
    auto const descend = [&](auto const& self, NodeId id) -> NodeId {
        NodeData const& data = m_impl->nodes[id];
        for (NodeId child : data.children) {
            NodeData const& c = m_impl->nodes[child];
            if (c.bounds && x >= c.bounds->x && x < c.bounds->x + c.bounds->width && y >= c.bounds->y && y < c.bounds->y + c.bounds->height) {
                if (c.kind == NodeData::Kind::Text || c.kind == NodeData::Kind::Generated)
                    return child;
                if (c.kind == NodeData::Kind::Element && c.role != Role::None)
                    return self(self, child);
            }
        }
        return id;
    };
    return descend(descend, found);
}

std::vector<Rect> Tree::text_bounds(NodeId id, std::uint32_t from, std::uint32_t to)
{
    NodeData const* data = node(id);
    PageSource* source = m_semantics.source();
    if (!data || !source || !data->dom || data->kind != NodeData::Kind::Text)
        return {};
    // The leaf's text is the node's data with white space collapsed: map the
    // offsets back through the collapse.
    auto const& text = static_cast<dom::Text const&>(*data->dom);
    std::u32string const raw = decode_utf8(text.data, true);
    std::vector<std::uint32_t> raw_units; // the UTF-16 offset in the raw data of each rendered unit
    std::uint32_t raw_unit = 0;
    bool space = false;
    bool collapse = true;
    if (dom::Node const* parent = dom::flat_parent(text); parent && parent->is_element()) {
        if (css::ComputedStyle const* style = m_semantics.style(static_cast<Element const&>(*parent))) {
            css::WhiteSpace const mode = style->white_space;
            collapse = !(mode == css::WhiteSpace::Pre || mode == css::WhiteSpace::PreWrap || mode == css::WhiteSpace::BreakSpaces);
        }
    }
    for (char32_t const c : raw) {
        std::uint32_t const units = c > 0xFFFF ? 2 : 1;
        bool const is_space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (collapse && is_space) {
            if (!space)
                raw_units.push_back(raw_unit);
            space = true;
        } else {
            space = false;
            for (std::uint32_t u = 0; u < units; ++u)
                raw_units.push_back(raw_unit + u);
        }
        raw_unit += units;
    }
    raw_units.push_back(raw_unit);
    from = std::min<std::uint32_t>(from, static_cast<std::uint32_t>(raw_units.size() - 1));
    to = std::min<std::uint32_t>(std::max(to, from), static_cast<std::uint32_t>(raw_units.size() - 1));
    return source->text_boxes(text, raw_units[from], raw_units[to]);
}

std::vector<Tree::InlineTextBox> Tree::inline_text_boxes(NodeId id)
{
    std::vector<InlineTextBox> out;
    NodeData const* data = node(id);
    if (!data || data->kind != NodeData::Kind::Text)
        return out;
    std::uint32_t const length = utf16_length(data->text);
    // A run ends where the next character's box starts on another line.
    for (std::uint32_t i = 0; i < length; ++i) {
        std::vector<Rect> const boxes = text_bounds(id, i, i + 1);
        if (boxes.empty())
            continue;
        Rect const& box = boxes.front();
        if (!out.empty() && std::fabs(out.back().bounds.y - box.y) < 0.5f && out.back().end == i) {
            InlineTextBox& last = out.back();
            float const right = std::max(last.bounds.x + last.bounds.width, box.x + box.width);
            last.bounds.x = std::min(last.bounds.x, box.x);
            last.bounds.width = right - last.bounds.x;
            last.bounds.height = std::max(last.bounds.height, box.height);
            last.end = i + 1;
            continue;
        }
        out.push_back({ i, i + 1, box });
    }
    return out;
}

bool Tree::do_action(NodeId id, std::string_view action)
{
    NodeData const* data = node(id);
    if (!data || !m_sink || data->kind != NodeData::Kind::Element)
        return false;
    if (std::find(data->actions.begin(), data->actions.end(), action) == data->actions.end())
        return false;
    auto& element = const_cast<Element&>(static_cast<Element const&>(*data->dom));
    if (action == "focus") {
        m_sink->focus(element);
        return true;
    }
    if (action == "scroll-into-view") {
        m_sink->scroll_into_view(element);
        return true;
    }
    // An option of a select is chosen by giving the select its value.
    if (action == "select" && element.is_html("option")) {
        for (Element const* at = dom::forms::element_parent(element); at; at = dom::forms::element_parent(*at)) {
            if (at->is_html("select")) {
                std::optional<std::string_view> const value = attribute(element, "value");
                std::string text;
                for (Node const* child : element.children()) {
                    if (child->is_text())
                        text += static_cast<dom::Text const&>(*child).data;
                }
                m_sink->set_value(const_cast<Element&>(*at), value ? std::string(*value) : collapse_whitespace(text));
                return true;
            }
        }
    }
    if (action == "activate") {
        m_sink->focus(element);
        return true;
    }
    m_sink->click(element);
    return true;
}

bool Tree::set_value(NodeId id, std::string const& value)
{
    NodeData const* data = node(id);
    if (!data || !m_sink || data->kind != NodeData::Kind::Element)
        return false;
    if (!(data->states & state_bit(State::Editable)) && !data->value_now)
        return false;
    m_sink->set_value(const_cast<Element&>(static_cast<Element const&>(*data->dom)), value);
    return true;
}

bool Tree::focus(NodeId id)
{
    NodeData const* data = node(id);
    if (!data || !m_sink || data->kind != NodeData::Kind::Element || !(data->states & state_bit(State::Focusable)))
        return false;
    m_sink->focus(const_cast<Element&>(static_cast<Element const&>(*data->dom)));
    return true;
}

bool Tree::set_text_selection(NodeId id, std::uint32_t start, std::uint32_t end)
{
    NodeData const* data = node(id);
    if (!data || !m_sink || data->kind != NodeData::Kind::Element || !is_text_field(static_cast<Element const&>(*data->dom)))
        return false;
    m_sink->set_text_selection(const_cast<Element&>(static_cast<Element const&>(*data->dom)), start, end);
    return true;
}

// --- The dump ------------------------------------------------------------------------------

std::string Tree::dump(bool with_bounds)
{
    update();
    std::ostringstream out;
    auto const quoted = [](std::string const& text) {
        std::string written = "\"";
        for (char const c : text) {
            if (c == '"' || c == '\\')
                written += '\\';
            if (c == '\n')
                written += "\\n";
            else
                written += c;
        }
        return written + "\"";
    };
    auto const number_text = [](double value) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%g", value);
        return std::string(buffer);
    };
    auto const write = [&](auto const& self, NodeId id, int depth) -> void {
        NodeData const& data = m_impl->nodes[id];
        out << std::string(static_cast<std::size_t>(depth) * 2, ' ') << platform_role_name(data.role);
        if (!data.name.empty())
            out << ' ' << quoted(data.name);
        if (!data.description.empty())
            out << " description=" << quoted(data.description);
        for (std::size_t s = 0; s < static_cast<std::size_t>(State::Count_); ++s) {
            if (data.states & state_bit(static_cast<State>(s)))
                out << ' ' << state_name(static_cast<State>(s));
        }
        if (data.level)
            out << " level=" << data.level;
        if (data.pos_in_set)
            out << " posinset=" << data.pos_in_set << " setsize=" << data.set_size;
        if (data.row_count || data.column_count)
            out << " rows=" << data.row_count << " columns=" << data.column_count;
        if (data.column_index >= 0)
            out << " cell=" << data.row_index << "," << data.column_index;
        else if (data.row_index >= 0)
            out << " row=" << data.row_index;
        if (data.value_now)
            out << " value=" << number_text(*data.value_now);
        if (data.value_min)
            out << " min=" << number_text(*data.value_min);
        if (data.value_max)
            out << " max=" << number_text(*data.value_max);
        if (!data.value_text.empty() && data.kind == NodeData::Kind::Element)
            out << " valuetext=" << quoted(data.value_text);
        if (!data.live.empty())
            out << " live=" << data.live << (data.atomic ? " atomic" : "");
        if (!data.invalid.empty() && data.invalid != "true")
            out << " invalid=" << data.invalid;
        if (!data.has_popup.empty())
            out << " haspopup=" << data.has_popup;
        if (!data.current.empty())
            out << " current=" << data.current;
        if (!data.role_description.empty())
            out << " roledescription=" << quoted(data.role_description);
        if (!data.placeholder.empty())
            out << " placeholder=" << quoted(data.placeholder);
        if (!data.url.empty())
            out << " url=" << quoted(data.url);
        for (auto const& [relation, targets] : data.relations) {
            out << ' ' << relation_name(relation) << '=';
            for (std::size_t i = 0; i < targets.size(); ++i)
                out << (i ? "," : "") << platform_role_name(m_impl->nodes[targets[i]].role);
        }
        if (with_bounds && data.bounds)
            out << " (" << number_text(static_cast<double>(data.bounds->x)) << "," << number_text(static_cast<double>(data.bounds->y)) << " "
                << number_text(static_cast<double>(data.bounds->width)) << "x" << number_text(static_cast<double>(data.bounds->height)) << ")";
        out << "\n";
        for (NodeId child : data.children)
            self(self, child, depth + 1);
    };
    if (m_impl->root)
        write(write, m_impl->root, 0);
    return out.str();
}

// --- The bridge's end ----------------------------------------------------------------------

Bridge::Bridge(Tree& tree, std::function<void()> wake, std::size_t capacity)
    : m_tree(&tree)
    , m_wake(std::move(wake))
    , m_capacity(std::max<std::size_t>(capacity, 2))
{
    tree.set_observer(this);
    on_published();
}

Bridge::~Bridge()
{
    m_tree->set_observer(nullptr);
}

void Bridge::on_event(Event const& event)
{
    {
        std::lock_guard<std::mutex> const lock(m_lock);
        if (m_events.size() >= m_capacity) {
            // Too far behind: what is queued says less than "read again".
            m_dropped.fetch_add(m_events.size(), std::memory_order_relaxed);
            m_events.clear();
            m_events.push_back({ Event::Type::TreeChanged, 0 });
        }
        m_events.push_back(event);
    }
    if (m_wake)
        m_wake();
}

void Bridge::on_published()
{
    std::lock_guard<std::mutex> const lock(m_lock);
    m_snapshot = m_tree->snapshot();
}

std::vector<Event> Bridge::take_events()
{
    std::lock_guard<std::mutex> const lock(m_lock);
    std::vector<Event> out(m_events.begin(), m_events.end());
    m_events.clear();
    return out;
}

std::shared_ptr<Snapshot const> Bridge::snapshot() const
{
    std::lock_guard<std::mutex> const lock(m_lock);
    return m_snapshot;
}

void Bridge::post(Tree::Query query)
{
    m_tree->post(std::move(query));
}

}
