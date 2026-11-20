#include "a11y/Semantics.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "css/ComputedStyle.h"
#include "dom/Forms.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>

namespace sashfold::a11y {

using dom::Element;
using dom::Node;

namespace {

bool ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
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

std::optional<std::string_view> attribute(Element const& element, std::string_view name)
{
    if (dom::Attr const* found = element.find_attribute(name))
        return std::string_view(found->value);
    return std::nullopt;
}

bool blank(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), ascii_space);
}

bool is_html(Node const& node, std::string_view name)
{
    return node.is_element() && static_cast<Element const&>(node).is_html(name);
}

Element const* element_of(Node const* node)
{
    return node && node->is_element() ? static_cast<Element const*>(node) : nullptr;
}

// The nearest ancestor element in the flat tree, else in the shadow-
// including tree (a node outside the flat tree still has a host).
Element const* parent_element(Node const& node)
{
    for (Node const* at = dom::flat_parent(node); at; at = dom::flat_parent(*at)) {
        if (at->is_element())
            return static_cast<Element const*>(at);
    }
    return nullptr;
}

// Labelable elements (HTML §4.10.2).
bool labelable(Element const& element)
{
    if (!element.is_html())
        return false;
    std::string const& name = element.local_name();
    if (name == "input")
        return dom::forms::input_type(element) != "hidden";
    return name == "button" || name == "meter" || name == "output" || name == "progress" || name == "select" || name == "textarea"
        || element.document().form_associated_custom.contains(&element);
}

// The labeled control of a label element (HTML §4.10.4): what its for
// attribute names, else its first labelable descendant.
Element const* labeled_control(Element const& label, Semantics& semantics)
{
    if (std::optional<std::string_view> const target = attribute(label, "for")) {
        Element const* found = semantics.element_by_id(label, *target);
        return found && labelable(*found) ? found : nullptr;
    }
    Element const* result = nullptr;
    auto const walk = [&](auto const& self, Node const& node) -> void {
        for (Node const* child : node.children()) {
            if (result)
                return;
            if (!child->is_element())
                continue;
            auto const& element = static_cast<Element const&>(*child);
            if (labelable(element)) {
                result = &element;
                return;
            }
            self(self, element);
        }
    };
    walk(walk, label);
    return result;
}

bool block_level(css::ComputedStyle const* style, Element const& element)
{
    if (style) {
        switch (style->display) {
        case css::Display::Inline:
        case css::Display::Contents:
        case css::Display::None:
            return false;
        default:
            return true;
        }
    }
    // Hidden content (named through aria-labelledby) has no box: its
    // elements are taken at their HTML default.
    static constexpr std::string_view blocks[] = { "address", "article", "aside", "blockquote", "dd", "details", "dialog", "div", "dl",
        "dt", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "legend",
        "li", "main", "menu", "nav", "ol", "p", "pre", "search", "section", "summary", "table", "tr", "td", "th", "ul" };
    if (!element.is_html())
        return false;
    return std::find(std::begin(blocks), std::end(blocks), element.local_name()) != std::end(blocks);
}

std::string transformed(std::string_view text, css::TextTransform transform)
{
    if (transform == css::TextTransform::None || transform == css::TextTransform::MathAuto)
        return std::string(text);
    std::u32string const decoded = decode_utf8(text, true);
    std::u32string out;
    out.reserve(decoded.size());
    bool at_word_start = true;
    for (char32_t const c : decoded) {
        switch (transform) {
        case css::TextTransform::Uppercase:
            out.push_back(to_uppercase(c));
            break;
        case css::TextTransform::Lowercase:
            out.push_back(to_lowercase(c));
            break;
        case css::TextTransform::Capitalize: {
            bool const letter = to_uppercase(c) != c || to_lowercase(c) != c;
            bool const in_word = letter || (c >= U'0' && c <= U'9') || is_combining_mark(c) || c == U'\'' || c == U'’';
            out.push_back(at_word_start && letter ? to_titlecase(c) : c);
            at_word_start = !in_word;
            break;
        }
        default:
            out.push_back(c);
        }
    }
    return to_utf8(out);
}

// The global states and properties that, present on an element, resolve
// a role of none or presentation back to its implicit role (ARIA 1.2 §9.3,
// "Presentational Roles Conflict Resolution").
bool has_global_aria(Element const& element)
{
    AriaSet const globals = global_attributes();
    for (dom::Attr const& attr : element.attributes()) {
        if (!attr.namespace_uri.empty() || !attr.prefix.empty())
            continue;
        std::optional<Aria> const aria = aria_from_attribute(attr.local_name);
        if (!aria || !(globals & bit(*aria)))
            continue;
        if (*aria == Aria::Hidden)
            continue;
        if (!blank(attr.value))
            return true;
    }
    return false;
}

bool is_sectioning(Element const& element)
{
    return element.is_html("article") || element.is_html("aside") || element.is_html("nav") || element.is_html("section");
}

}

std::string collapse_whitespace(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool pending = false;
    for (char const c : text) {
        if (ascii_space(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending)
            out += ' ';
        pending = false;
        out += c;
    }
    return out;
}

std::vector<std::string_view> role_tokens(std::string_view value)
{
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < value.size()) {
        while (at < value.size() && ascii_space(value[at]))
            ++at;
        std::size_t const start = at;
        while (at < value.size() && !ascii_space(value[at]))
            ++at;
        if (at > start)
            out.push_back(value.substr(start, at - start));
    }
    return out;
}

// Where a text alternative computation stands (AccName 1.2 §4.3.2): which
// traversal it is in, whether the node that started that traversal was
// hidden (so hidden content below it counts), and every node it has taken
// already, which it does not take twice.
struct Semantics::NameContext {
    Node const* root = nullptr;
    bool in_labelledby = false;
    bool in_describedby = false;
    bool in_label = false; // a label, legend, caption or figcaption naming another element
    bool include_hidden = false; // the node that started this traversal was hidden
    bool description = false;
    Element const* labelledby_of = nullptr; // whose aria-labelledby is being followed: it may name itself
    std::unordered_set<Node const*> visited;
};

// --- Construction and caches -----------------------------------------------------------------

Semantics::Semantics(PageSource* source)
    : m_source(source)
{
}

void Semantics::invalidate()
{
    m_ids.clear();
    m_owner_of.clear();
    m_owned.clear();
    m_owners_built = false;
    m_roles.clear();
    m_labels.clear();
    m_labels_built = false;
}

void Semantics::refresh_caches(dom::Document const& document)
{
    std::uint64_t const generation = m_source ? m_source->generation() : 0;
    if (m_cached_document == &document && m_tree_version == document.tree_version() && m_attribute_version == document.attribute_version()
        && m_source_generation == generation && m_state_version == document.state_version())
        return;
    invalidate();
    m_cached_document = &document;
    m_tree_version = document.tree_version();
    m_attribute_version = document.attribute_version();
    m_state_version = document.state_version();
    m_source_generation = generation;
}

css::ComputedStyle const* Semantics::style(Element const& element)
{
    return m_source ? m_source->style(element) : nullptr;
}

Element* Semantics::element_by_id(Node const& scope, std::string_view id)
{
    if (id.empty())
        return nullptr;
    refresh_caches(scope.document());
    Node const& root = scope.root();
    auto found = m_ids.find(&root);
    if (found == m_ids.end()) {
        std::unordered_map<std::string, Element*> map;
        auto const walk = [&](auto const& self, Node const& node) -> void {
            for (Node* child : node.children()) {
                if (!child->is_element())
                    continue;
                auto& element = static_cast<Element&>(*child);
                if (dom::Attr const* attr = element.find_attribute("id"); attr && !attr->value.empty())
                    map.emplace(attr->value, &element);
                self(self, element);
            }
        };
        walk(walk, root);
        found = m_ids.emplace(&root, std::move(map)).first;
    }
    auto const it = found->second.find(std::string(id));
    return it == found->second.end() ? nullptr : it->second;
}

static bool reference_in_scope(Element const& element, Element const& candidate)
{
    // HTML §2.6.1: an explicitly set element counts when it is a descendant
    // of one of the element's shadow-including ancestors: in the element's
    // own tree, or in a tree the element's tree is inside.
    Node const& candidate_root = candidate.root();
    for (Node const* root = &element.root();;) {
        if (root == &candidate_root)
            return true;
        if (!root->is_shadow_root())
            return false;
        root = &static_cast<dom::ShadowRoot const&>(*root).host().root();
    }
}

static std::optional<std::vector<Element*>> explicit_references(Element const& element, std::string_view name)
{
    // The elements a script set stand while the content attribute is absent
    // or the empty string the setter wrote; any other value written since is
    // the author's IDs again.
    std::optional<std::string_view> const value = attribute(element, name);
    if (value && !value->empty())
        return std::nullopt;
    auto const& all = element.document().aria_element_references;
    auto const found = all.find(&element);
    if (found == all.end())
        return std::nullopt;
    auto const list = found->second.find(std::string(name));
    if (list == found->second.end())
        return std::nullopt;
    std::vector<Element*> out;
    for (Element* candidate : list->second) {
        if (reference_in_scope(element, *candidate))
            out.push_back(candidate);
    }
    return out;
}

std::vector<Element*> Semantics::idrefs(Element const& element, std::string_view name)
{
    if (std::optional<std::vector<Element*>> set = explicit_references(element, name))
        return std::move(*set);
    std::vector<Element*> out;
    std::optional<std::string_view> const value = attribute(element, name);
    if (!value)
        return out;
    for (std::string_view const token : role_tokens(*value)) {
        if (Element* found = element_by_id(element, token)) {
            if (std::find(out.begin(), out.end(), found) == out.end())
                out.push_back(found);
        }
    }
    return out;
}

Element* Semantics::idref(Element const& element, std::string_view name)
{
    if (std::optional<std::vector<Element*>> set = explicit_references(element, name))
        return set->empty() ? nullptr : set->front();
    std::optional<std::string_view> const value = attribute(element, name);
    if (!value)
        return nullptr;
    return element_by_id(element, collapse_whitespace(*value));
}

void Semantics::build_owners(dom::Document const& document)
{
    if (m_owners_built)
        return;
    m_owners_built = true;
    // Every element with aria-owns, in shadow-including tree order: an
    // element claimed twice belongs to the first that claims it (ARIA 1.2
    // §6.6 aria-owns: an element can have only one owner), and a claim that
    // would make the owner its own descendant is left out.
    std::vector<Element const*> owners;
    dom::for_each_shadow_including(static_cast<Node const&>(document), [&](Node const& node) {
        if (node.is_element()) {
            auto const& element = static_cast<Element const&>(node);
            if (element.has_attribute("aria-owns"))
                owners.push_back(&element);
        }
    });
    for (Element const* owner : owners) {
        // An owner hidden from everyone owns nothing: what it names stays
        // where it is (the engines agree; accname/aria-owns.html).
        if (hidden(*owner))
            continue;
        for (Element* target : idrefs(*owner, "aria-owns")) {
            if (target == owner || m_owner_of.contains(target))
                continue;
            // The target must not be the owner or one of its ancestors,
            // through the ownership already made.
            bool cycle = false;
            for (Node const* at = owner; at; at = content_parent(*at)) {
                if (at == target) {
                    cycle = true;
                    break;
                }
            }
            if (cycle)
                continue;
            m_owner_of[target] = const_cast<Element*>(owner);
            m_owned[owner].push_back(target);
        }
    }
}

Element* Semantics::aria_owner(Element const& element)
{
    refresh_caches(element.document());
    build_owners(element.document());
    auto const it = m_owner_of.find(&element);
    return it == m_owner_of.end() ? nullptr : it->second;
}

Node* Semantics::content_parent(Node const& node)
{
    if (!m_building_owners) {
        refresh_caches(node.document());
        m_building_owners = true;
        build_owners(node.document());
        m_building_owners = false;
    }
    if (node.is_element()) {
        auto const it = m_owner_of.find(static_cast<Element const*>(&node));
        if (it != m_owner_of.end())
            return it->second;
    }
    return dom::flat_parent(node);
}

std::vector<Node*> Semantics::content_children(Node const& node)
{
    refresh_caches(node.document());
    build_owners(node.document());
    std::vector<Node*> out;
    for (Node* child : dom::flat_children(node)) {
        if (child->is_element()) {
            auto const it = m_owner_of.find(static_cast<Element const*>(child));
            if (it != m_owner_of.end() && it->second != &node)
                continue;
            out.push_back(child);
        } else if (child->is_text()) {
            out.push_back(child);
        }
    }
    if (node.is_element()) {
        auto const it = m_owned.find(static_cast<Element const*>(&node));
        if (it != m_owned.end()) {
            for (Element* owned : it->second) {
                if (std::find(out.begin(), out.end(), owned) == out.end())
                    out.push_back(owned);
            }
        }
    }
    return out;
}

std::optional<std::string_view> Semantics::aria(Element const& element, std::string_view name)
{
    if (std::optional<std::string_view> const value = attribute(element, name))
        return value;
    // ElementInternals' default semantics, under the element's own.
    auto const& defaults = element.document().internals_aria;
    if (defaults.empty())
        return std::nullopt;
    auto const found = defaults.find(&element);
    if (found == defaults.end())
        return std::nullopt;
    auto const value = found->second.find(std::string(name));
    if (value == found->second.end())
        return std::nullopt;
    return std::string_view(value->second);
}

std::optional<bool> Semantics::aria_bool(Element const& element, std::string_view name)
{
    std::optional<std::string_view> const value = aria(element, name);
    if (!value)
        return std::nullopt;
    std::string const trimmed = collapse_whitespace(*value);
    if (ascii_ci_equals(trimmed, "true"))
        return true;
    if (ascii_ci_equals(trimmed, "false"))
        return false;
    return std::nullopt;
}

// --- Visibility and focus ---------------------------------------------------------------------

bool Semantics::not_rendered(Node const& node)
{
    if (!node.is_connected())
        return true;
    Element const* element = node.is_element() ? static_cast<Element const*>(&node) : nullptr;
    if (!element) {
        if (!dom::flat_parent(node))
            return true;
        element = parent_element(node);
        if (!element)
            return false;
    }
    // Elements HTML renders as nothing whatever their style says, where the
    // built-in sheet has no rule for them yet (HTML §15.3.1, §15.5.15).
    for (Element const* at = element; at; at = parent_element(*at)) {
        if (at->is_html("dialog") && !at->has_attribute("open"))
            return true;
        if (at->is_html("datalist"))
            return true;
        if (at->is_html("details") && !at->has_attribute("open") && at != element) {
            // Only the details element's first summary child is shown.
            Element const* child = element;
            while (child && parent_element(*child) != at)
                child = parent_element(*child);
            Element const* summary = nullptr;
            for (Node const* c : at->children()) {
                if (is_html(*c, "summary")) {
                    summary = static_cast<Element const*>(c);
                    break;
                }
            }
            if (child && child != summary)
                return true;
        }
    }
    // A host's child no slot takes is in no flat tree, and drawn nowhere.
    if (element->parent() != &element->document() && !dom::flat_parent(*element))
        return true;
    if (!m_source) {
        for (Element const* at = element; at; at = parent_element(*at)) {
            if (at->is_html()) {
                std::string const& name = at->local_name();
                if (name == "head" || name == "script" || name == "style" || name == "template" || name == "title" || name == "meta"
                    || name == "link" || name == "base" || name == "noscript" || at->has_attribute("hidden"))
                    return true;
                if (name == "input" && dom::forms::input_type(*at) == "hidden")
                    return true;
            }
        }
        return false;
    }
    // An option of a select is shown in its select's place (a drop-down's
    // list is drawn on demand): it is there when the select is.
    if (element->is_html("option") || element->is_html("optgroup")) {
        for (Element const* at = parent_element(*element); at; at = parent_element(*at)) {
            if (at->is_html("select"))
                return not_rendered(*at);
        }
    }
    css::ComputedStyle const* const computed = style(*element);
    if (!computed)
        return true;
    if (computed->display == css::Display::None || computed->undisplayed)
        return true;
    if (computed->visibility == css::Visibility::Hidden)
        return true;
    return false;
}

bool Semantics::visibility_hidden_only(Element const& element)
{
    css::ComputedStyle const* const computed = style(element);
    return computed && computed->display != css::Display::None && !computed->undisplayed
        && computed->visibility == css::Visibility::Hidden;
}

bool Semantics::aria_hidden(Node const& node)
{
    for (Node const* at = &node; at; at = content_parent(*at)) {
        if (at->is_element()) {
            std::optional<bool> const value = aria_bool(static_cast<Element const&>(*at), "aria-hidden");
            if (value && *value)
                return true;
        }
    }
    return false;
}

bool Semantics::hidden(Node const& node)
{
    return not_rendered(node) || aria_hidden(node);
}

bool Semantics::inert(Element const& element)
{
    if (css::ComputedStyle const* computed = style(element))
        return computed->inert;
    for (Element const* at = &element; at; at = parent_element(*at)) {
        if (at->has_attribute("inert"))
            return true;
    }
    return false;
}

bool Semantics::focusable(Element const& element)
{
    if (not_rendered(element) || inert(element))
        return false;
    if (std::optional<std::string_view> const tabindex = attribute(element, "tabindex")) {
        std::string const text = collapse_whitespace(*tabindex);
        char* end = nullptr;
        (void)std::strtol(text.c_str(), &end, 10);
        if (!text.empty() && end && *end == '\0')
            return true;
    }
    if (element.is_html()) {
        std::string const& name = element.local_name();
        if (name == "a" || name == "area")
            return element.has_attribute("href");
        if (name == "button" || name == "select" || name == "textarea")
            return !dom::forms::actually_disabled(element);
        if (name == "input")
            return dom::forms::input_type(element) != "hidden" && !dom::forms::actually_disabled(element);
        if (name == "iframe" || name == "frame")
            return true;
        if ((name == "audio" || name == "video") && element.has_attribute("controls"))
            return true;
        if (name == "summary") {
            Node const* parent = element.parent();
            if (parent && is_html(*parent, "details")) {
                for (Node const* child : parent->children()) {
                    if (is_html(*child, "summary"))
                        return child == &element;
                }
            }
        }
    }
    if (element.is_svg("a"))
        return element.has_attribute("href") || element.find_attribute("xlink:href");
    if (dom::forms::editing_host_editable(element)) {
        Element const* parent = parent_element(element);
        return !parent || !dom::forms::editing_host_editable(*parent);
    }
    return false;
}

bool Semantics::disabled(Element const& element)
{
    if (dom::forms::actually_disabled(element))
        return true;
    for (Element const* at = &element; at; at = element_of(content_parent(*at))) {
        std::optional<bool> const value = aria_bool(*at, "aria-disabled");
        if (value)
            return *value;
    }
    return false;
}

// --- Roles ---------------------------------------------------------------------------------

std::optional<Role> Semantics::explicit_role(Element const& element)
{
    std::optional<std::string_view> const value = aria(element, "role");
    if (!value)
        return std::nullopt;
    for (std::string_view const token : role_tokens(*value)) {
        std::optional<Role> const role = role_from_token(token);
        if (!role || is_abstract(*role))
            continue;
        // A region or a form without an accessible name is no landmark
        // (ARIA 1.2 §5.4 region: "authors MUST give each element with a
        // region role a brief label"; engines take the next token).
        if ((*role == Role::Region || *role == Role::Form) && !has_author_name(element))
            continue;
        return role;
    }
    return std::nullopt;
}

bool Semantics::has_author_name(Element const& element)
{
    // An accessible name an author gave the element, which is what makes a
    // section a region and an aside a landmark (HTML-AAM §3.4.1): its
    // aria-labelledby text, its aria-label, its title.
    std::optional<std::string_view> const label = aria(element, "aria-label");
    if (label && !blank(*label))
        return true;
    std::vector<Element*> const refs = idrefs(element, "aria-labelledby");
    if (!refs.empty()) {
        NameContext context;
        context.root = &element;
        context.in_labelledby = true;
        context.visited.insert(&element);
        std::string text;
        for (Element* ref : refs) {
            context.include_hidden = hidden(*ref);
            text += text_alternative(*ref, context, false);
            text += ' ';
        }
        if (!blank(text))
            return true;
    }
    std::optional<std::string_view> const title = attribute(element, "title");
    return title && !blank(*title);
}

bool Semantics::presentational_conflict(Element const& element)
{
    return focusable(element) || has_global_aria(element);
}

Role Semantics::role(Element const& element)
{
    refresh_caches(element.document());
    auto const cached = m_roles.find(&element);
    if (cached != m_roles.end())
        return cached->second;
    if (m_roles_in_progress.contains(&element))
        return implicit_role(element);
    m_roles_in_progress.insert(&element);
    Role const computed = role_uncached(element);
    m_roles_in_progress.erase(&element);
    m_roles[&element] = computed;
    return computed;
}

Role Semantics::role_uncached(Element const& element)
{
    std::optional<Role> const explicit_value = explicit_role(element);
    Role result = Role::Unknown;
    bool explicit_none = false;
    if (explicit_value && *explicit_value != Role::None) {
        result = *explicit_value;
    } else {
        Role const implicit = implicit_role(element);
        if (explicit_value && *explicit_value == Role::None) {
            explicit_none = true;
            // ARIA 1.2 §9.3: a focusable element, or one with a global ARIA
            // attribute, keeps its implicit role; so does one whose role
            // could not be presentational anyway (none has nothing to undo).
            result = presentational_conflict(element) ? implicit : Role::None;
        } else {
            result = implicit;
        }
    }
    // HTML-AAM §3.4.2 "minimum role": a generic or presentational element
    // that carries attributes which need a role to be exposed is a group.
    if ((result == Role::Generic || result == Role::None || (explicit_none && result == Role::Generic))
        && element.is_html()
        && (element.has_attribute("autofocus") || element.has_attribute("draggable") || element.has_attribute("popover")))
        result = Role::Group;
    return result;
}

bool Semantics::inherits_presentation(Element const& element, Role implicit)
{
    // ARIA 1.2 §9.3: the required owned elements of an element whose role
    // is presentational are presentational too, unless they have an
    // explicit role of their own. HTML-AAM applies it to list items of a
    // presentational list and the parts of a presentational table.
    auto const presentational = [&](Element const& ancestor) {
        std::optional<Role> const role = explicit_role(ancestor);
        return role && *role == Role::None && !presentational_conflict(ancestor);
    };
    switch (implicit) {
    case Role::Row:
    case Role::Cell:
    case Role::ColumnHeader:
    case Role::RowHeader:
    case Role::GridCell:
    case Role::RowGroup:
    case Role::Caption:
        for (Element const* at = parent_element(element); at; at = parent_element(*at)) {
            if (at->is_html("table"))
                return presentational(*at);
        }
        return false;
    case Role::Term:
    case Role::Definition:
        if (Element const* parent = parent_element(element); parent && parent->is_html("dl"))
            return presentational(*parent);
        return false;
    default:
        return false;
    }
}

Role Semantics::implicit_role(Element const& element)
{
    if (element.is_html()) {
        Role const role = html_role(element);
        if (role != Role::Unknown && inherits_presentation(element, role))
            return Role::None;
        return role;
    }
    if (element.namespace_uri() == dom::ns::svg)
        return svg_role(element);
    if (element.is_mathml("math"))
        return Role::Math;
    if (element.namespace_uri() == dom::ns::mathml)
        return Role::Generic;
    return Role::Generic;
}

Role Semantics::html_role(Element const& element)
{
    std::string const& name = element.local_name();
    auto const has_list = [&] {
        std::optional<std::string_view> const list = attribute(element, "list");
        if (!list)
            return false;
        Element const* target = element_by_id(element, *list);
        return target && target->is_html("datalist");
    };
    if (name == "a" || name == "area")
        return element.has_attribute("href") ? Role::Link : Role::Generic;
    if (name == "input") {
        std::string const type = dom::forms::input_type(element);
        if (type == "button" || type == "image" || type == "reset" || type == "submit")
            return Role::Button;
        if (type == "checkbox")
            return element.has_attribute("switch") ? Role::Switch : Role::Checkbox;
        if (type == "radio")
            return Role::Radio;
        if (type == "range")
            return Role::Slider;
        if (type == "number")
            return Role::SpinButton;
        if (type == "search")
            return has_list() ? Role::Combobox : Role::SearchBox;
        if (type == "email" || type == "tel" || type == "text" || type == "url")
            return has_list() ? Role::Combobox : Role::TextBox;
        if (type == "password")
            return Role::TextBox;
        if (type == "hidden")
            return Role::Unknown;
        if (type == "file")
            return Role::Button;
        if (type == "color")
            return Role::Button;
        // date, datetime-local, month, time, week: a group of fields.
        return Role::Group;
    }
    if (name == "img") {
        std::optional<std::string_view> const alt = attribute(element, "alt");
        if (alt && alt->empty()) {
            std::optional<std::string_view> const label = aria(element, "aria-label");
            if (label && !blank(*label))
                return Role::Image;
            std::vector<Element*> const refs = idrefs(element, "aria-labelledby");
            if (!refs.empty()) {
                NameContext context;
                context.root = &element;
                context.in_labelledby = true;
                context.visited.insert(&element);
                std::string text;
                for (Element* ref : refs) {
                    context.include_hidden = hidden(*ref);
                    text += text_alternative(*ref, context, false);
                }
                if (!blank(text))
                    return Role::Image;
            }
            return Role::None;
        }
        return Role::Image;
    }
    if (name == "select") {
        if (element.has_attribute("multiple"))
            return Role::ListBox;
        if (std::optional<std::string_view> const size = attribute(element, "size")) {
            if (std::atoi(std::string(*size).c_str()) > 1)
                return Role::ListBox;
        }
        return Role::Combobox;
    }
    if (name == "section")
        return has_author_name(element) ? Role::Region : Role::Generic;
    if (name == "aside") {
        if (has_author_name(element))
            return Role::Complementary;
        for (Element const* at = parent_element(element); at; at = parent_element(*at)) {
            if (at->is_html("main") || at->is_html("body"))
                break;
            if (is_sectioning(*at))
                return Role::Generic;
        }
        return Role::Complementary;
    }
    if (name == "header" || name == "footer") {
        bool const header = name == "header";
        for (Element const* at = parent_element(element); at; at = parent_element(*at)) {
            if (at->is_html("body"))
                break;
            if (is_sectioning(*at) || at->is_html("main")) {
                if (has_author_name(element) || at->is_html("main") || is_sectioning(*at))
                    return header ? Role::SectionHeader : Role::SectionFooter;
            }
            if (std::optional<Role> const role = explicit_role(*at);
                role && (*role == Role::Article || *role == Role::Complementary || *role == Role::Main || *role == Role::Navigation || *role == Role::Region))
                return header ? Role::SectionHeader : Role::SectionFooter;
        }
        return header ? Role::Banner : Role::ContentInfo;
    }
    if (name == "li") {
        for (Node const* at = content_parent(element); at; at = content_parent(*at)) {
            Element const* parent = element_of(at);
            if (!parent)
                break;
            if (parent->is_html("ul") || parent->is_html("ol") || parent->is_html("menu") || parent->is_html("dir")) {
                Role const list = role(*parent);
                if (list == Role::None)
                    return Role::None;
                return list == Role::List || list == Role::Generic ? Role::ListItem : Role::Generic;
            }
            Role const role_there = role(*parent);
            if (role_there == Role::List)
                return Role::ListItem;
            if (role_there != Role::Generic && role_there != Role::None)
                break;
        }
        return Role::Generic;
    }
    if (name == "th") {
        if (std::optional<std::string_view> const scope = attribute(element, "scope")) {
            std::string const value = lowered(collapse_whitespace(*scope));
            if (value == "row" || value == "rowgroup")
                return Role::RowHeader;
            if (value == "col" || value == "colgroup")
                return Role::ColumnHeader;
        }
        Element const* row = parent_element(element);
        if (row && row->is_html("tr")) {
            Element const* group = parent_element(*row);
            if (group && group->is_html("thead"))
                return Role::ColumnHeader;
            for (Node const* cell : row->children()) {
                if (is_html(*cell, "td"))
                    return Role::RowHeader;
            }
        }
        return Role::ColumnHeader;
    }
    if (name == "td") {
        for (Element const* at = parent_element(element); at; at = parent_element(*at)) {
            if (at->is_html("table")) {
                Role const table = role(*at);
                return table == Role::Grid || table == Role::TreeGrid ? Role::GridCell : Role::Cell;
            }
        }
        return Role::Cell;
    }
    if (name == "option") {
        return Role::Option;
    }
    if (name == "summary") {
        Node const* parent = element.parent();
        if (parent && is_html(*parent, "details")) {
            for (Node const* child : parent->children()) {
                if (is_html(*child, "summary"))
                    return child == &element ? Role::DisclosureTriangle : Role::Generic;
            }
        }
        return Role::Generic;
    }
    if (name == "form")
        return Role::Form;
    static std::unordered_map<std::string_view, Role> const simple = {
        { "address", Role::Group },
        { "article", Role::Article },
        { "audio", Role::Audio },
        { "b", Role::Generic },
        { "bdi", Role::Generic },
        { "bdo", Role::Generic },
        { "blockquote", Role::Blockquote },
        { "body", Role::Generic },
        { "br", Role::LineBreak },
        { "button", Role::Button },
        { "canvas", Role::Canvas },
        { "caption", Role::Caption },
        { "cite", Role::Cite },
        { "code", Role::Code },
        { "data", Role::Generic },
        { "datalist", Role::ListBox },
        { "dd", Role::Definition },
        { "del", Role::Deletion },
        { "details", Role::Group },
        { "dfn", Role::Term },
        { "dialog", Role::Dialog },
        { "dir", Role::List },
        { "div", Role::Generic },
        { "dl", Role::DescriptionList },
        { "dt", Role::Term },
        { "em", Role::Emphasis },
        { "embed", Role::Embed },
        { "fieldset", Role::Group },
        { "figcaption", Role::FigCaption },
        { "figure", Role::Figure },
        { "frame", Role::IFrame },
        { "h1", Role::Heading },
        { "h2", Role::Heading },
        { "h3", Role::Heading },
        { "h4", Role::Heading },
        { "h5", Role::Heading },
        { "h6", Role::Heading },
        { "hgroup", Role::Group },
        { "hr", Role::Separator },
        { "html", Role::Generic },
        { "i", Role::Generic },
        { "iframe", Role::IFrame },
        { "ins", Role::Insertion },
        { "kbd", Role::Kbd },
        { "label", Role::Label },
        { "legend", Role::Legend },
        { "main", Role::Main },
        { "mark", Role::Mark },
        { "menu", Role::List },
        { "meter", Role::Meter },
        { "nav", Role::Navigation },
        { "object", Role::Embed },
        { "ol", Role::List },
        { "optgroup", Role::Group },
        { "output", Role::Status },
        { "p", Role::Paragraph },
        { "picture", Role::Generic },
        { "pre", Role::Generic },
        { "progress", Role::ProgressBar },
        { "q", Role::Generic },
        { "rp", Role::Generic },
        { "rt", Role::Generic },
        { "ruby", Role::Ruby },
        { "s", Role::Deletion },
        { "samp", Role::Generic },
        { "search", Role::Search },
        { "small", Role::Generic },
        { "span", Role::Generic },
        { "strong", Role::Strong },
        { "sub", Role::Subscript },
        { "sup", Role::Superscript },
        { "table", Role::Table },
        { "tbody", Role::RowGroup },
        { "textarea", Role::TextBox },
        { "tfoot", Role::RowGroup },
        { "thead", Role::RowGroup },
        { "time", Role::Time },
        { "tr", Role::Row },
        { "u", Role::Generic },
        { "ul", Role::List },
        { "var", Role::Var },
        { "video", Role::Video },
        { "abbr", Role::Abbr },
        { "center", Role::Generic },
        { "font", Role::Generic },
        { "big", Role::Generic },
        { "tt", Role::Generic },
        { "nobr", Role::Generic },
        { "strike", Role::Deletion },
        { "acronym", Role::Abbr },
        { "listing", Role::Generic },
        { "xmp", Role::Generic },
        { "plaintext", Role::Generic },
        { "marquee", Role::Marquee },
    };
    auto const found = simple.find(name);
    if (found != simple.end())
        return found->second;
    static constexpr std::string_view unmapped[] = { "head", "script", "style", "template", "title", "meta", "link", "base", "noscript",
        "param", "source", "track", "col", "colgroup", "slot", "map", "wbr", "noframes", "frameset" };
    if (std::find(std::begin(unmapped), std::end(unmapped), name) != std::end(unmapped))
        return Role::Unknown;
    return Role::Generic;
}

Role Semantics::svg_role(Element const& element)
{
    std::string const& name = element.local_name();
    // SVG-AAM §5.1: whether the element has a title child with text, or an
    // ARIA name: a graphics element is exposed only when it says something.
    auto const titled = [&] {
        for (Node const* child : element.children()) {
            if (child->is_element() && static_cast<Element const&>(*child).is_svg("title"))
                return true;
        }
        return false;
    };
    auto const named = [&] {
        if (has_author_name(element))
            return true;
        for (Node const* child : element.children()) {
            if (child->is_element() && (static_cast<Element const&>(*child).is_svg("title") || static_cast<Element const&>(*child).is_svg("desc"))) {
                std::string text;
                for (Node const* t : child->children()) {
                    if (t->is_text())
                        text += static_cast<dom::Text const&>(*t).data;
                }
                if (!blank(text))
                    return true;
            }
        }
        return false;
    };
    if (name == "svg")
        return Role::GraphicsDocument;
    if (name == "a")
        return element.has_attribute("href") || element.find_attribute("xlink:href") ? Role::Link : Role::Group;
    if (name == "g" || name == "foreignObject" || name == "text" || name == "textPath" || name == "tspan")
        return named() ? Role::Group : Role::Generic;
    if (name == "circle" || name == "ellipse" || name == "line" || name == "path" || name == "polygon" || name == "polyline" || name == "rect"
        || name == "mesh")
        return named() ? Role::GraphicsSymbol : Role::Generic;
    if (name == "image") {
        if (titled() && !named())
            return Role::Generic;
        return Role::Image;
    }
    if (name == "use")
        return named() ? Role::GraphicsObject : Role::Generic;
    if (name == "canvas" || name == "video" || name == "audio" || name == "iframe")
        return Role::Generic;
    return Role::Unknown;
}

std::string Semantics::computed_role_string(Element const& element)
{
    Role const computed = role(element);
    return std::string(role_name(computed));
}

// --- Names and descriptions -----------------------------------------------------------------

std::string Semantics::rendered_text(dom::Text const& text)
{
    Element const* parent = parent_element(text);
    css::ComputedStyle const* computed = parent ? style(*parent) : nullptr;
    css::WhiteSpace const mode = computed ? computed->white_space : css::WhiteSpace::Normal;
    std::string out;
    if (mode == css::WhiteSpace::Pre || mode == css::WhiteSpace::PreWrap || mode == css::WhiteSpace::BreakSpaces) {
        out = text.data;
    } else {
        bool space = false;
        for (char const c : text.data) {
            if (ascii_space(c)) {
                space = true;
                continue;
            }
            if (space)
                out += ' ';
            space = false;
            out += c;
        }
        if (space)
            out += ' ';
    }
    if (computed)
        out = transformed(out, computed->text_transform);
    return out;
}

std::string Semantics::pseudo_text(Element const& element, bool before)
{
    css::ComputedStyle const* computed = style(element);
    if (!computed || !computed->generated)
        return {};
    std::optional<css::GeneratedBox> const& box = before ? computed->generated->before : computed->generated->after;
    if (!box || box->style.display == css::Display::None)
        return {};
    // CSS Generated Content 3 §1.2: the alternative text after a slash
    // stands for the content in the accessibility tree.
    std::string text = box->alt ? *box->alt : box->text;
    text = transformed(text, box->style.text_transform);
    // Alternative text stands as a word of its own beside the element's
    // content, as the engines expose it (a text leaf of its own).
    if (box->alt && !text.empty())
        text = before ? text + " " : " " + text;
    if (box->style.display != css::Display::Inline && box->style.display != css::Display::Contents)
        text = " " + text + " ";
    return text;
}

std::string Semantics::name_from_content(Element const& element, NameContext& context)
{
    std::string out;
    // A list item's marker comes first (AccName 1.2 §4.3.2 step 2F.ii:
    // the ::marker pseudo-element's text): the number with its period, or
    // the bullet.
    if (css::ComputedStyle const* computed = style(element);
        computed && computed->display == css::Display::ListItem && computed->list_style_type != css::ListStyleType::None) {
        css::ListStyleType const type = computed->list_style_type;
        out = css::format_counter(computed->list_item_value, type);
        if (type != css::ListStyleType::Disc && type != css::ListStyleType::Circle && type != css::ListStyleType::Square)
            out += '.';
        out += ' ';
    }
    out += pseudo_text(element, true);
    // The elements the element's aria-actions names are actions of it, not
    // part of its name (ARIA 1.3 aria-actions).
    std::vector<Element*> const actions = idrefs(element, "aria-actions");
    for (Node* child : content_children(element)) {
        if (child->is_element() && std::find(actions.begin(), actions.end(), child) != actions.end())
            continue;
        std::string piece = text_alternative(*child, context, false);
        if (child->is_element()) {
            auto const& child_element = static_cast<Element const&>(*child);
            if (child_element.is_html("br"))
                piece = "\n";
            else if (block_level(style(child_element), child_element))
                piece = " " + piece + " ";
        }
        out += piece;
    }
    out += pseudo_text(element, false);
    return out;
}

std::string Semantics::subtree_text(Node const& node, NameContext& context)
{
    // A label, legend, caption or figcaption naming another element: its
    // text alternative, in a traversal that counts as a native text
    // alternative (AccName 1.2 §4.3.2 step 2A: hidden descendants count
    // when the element itself is hidden).
    bool const was_label = context.in_label;
    bool const was_hidden = context.include_hidden;
    context.in_label = true;
    context.include_hidden = hidden(node);
    std::string const text = text_alternative(node, context, false);
    context.in_label = was_label;
    context.include_hidden = was_hidden;
    return text;
}

std::string Semantics::label_elements_text(Element const& control, NameContext& context)
{
    refresh_caches(control.document());
    if (!m_labels_built) {
        m_labels_built = true;
        dom::for_each_shadow_including(static_cast<Node const&>(control.document()), [&](Node const& node) {
            if (is_html(node, "label")) {
                auto const& label = static_cast<Element const&>(node);
                if (Element const* target = labeled_control(label, *this))
                    m_labels[target].push_back(&label);
            }
        });
    }
    auto const found = m_labels.find(&control);
    if (found == m_labels.end())
        return {};
    std::string out;
    for (Element const* label : found->second) {
        // A hidden label names nothing (HTML-AAM §4.1.1, as the engines
        // read it: the label must be rendered to be the control's caption).
        if (hidden(*label))
            continue;
        std::string const text = collapse_whitespace(subtree_text(*label, context));
        if (text.empty())
            continue;
        if (!out.empty())
            out += ' ';
        out += text;
    }
    return out;
}

std::string Semantics::embedded_control_value(Element const& element, Role role_value, NameContext& context)
{
    (void)context;
    switch (role_value) {
    case Role::TextBox:
    case Role::SearchBox:
        if (element.is_html("input") || element.is_html("textarea"))
            return dom::forms::control_value(element);
        {
            std::string text;
            for (Node const* child : content_children(element)) {
                if (child->is_text())
                    text += static_cast<dom::Text const&>(*child).data;
            }
            return text;
        }
    case Role::Combobox:
    case Role::ListBox: {
        if (element.is_html("input"))
            return dom::forms::control_value(element);
        std::string out;
        auto const add = [&](Element const& option) {
            std::string const text = collapse_whitespace(name(option));
            if (text.empty())
                return;
            if (!out.empty())
                out += ' ';
            out += text;
        };
        if (element.is_html("select")) {
            dom::for_each_shadow_including(element, [&](Node const& node) {
                if (is_html(node, "option") && dom::forms::option_selected(static_cast<Element const&>(node)))
                    add(static_cast<Element const&>(node));
            });
            return out;
        }
        // An ARIA listbox: its selected options; a combobox, its value
        // (the text of the textbox it is, or of what it controls).
        dom::for_each_shadow_including(element, [&](Node const& node) {
            if (!node.is_element() || &node == &element)
                return;
            auto const& option = static_cast<Element const&>(node);
            if (role(option) == Role::Option && aria_bool(option, "aria-selected").value_or(false))
                add(option);
        });
        if (out.empty() && role_value == Role::Combobox) {
            for (Node const* child : content_children(element)) {
                if (child->is_text())
                    out += static_cast<dom::Text const&>(*child).data;
            }
        }
        return out;
    }
    case Role::Slider:
    case Role::SpinButton:
    case Role::ProgressBar:
    case Role::ScrollBar:
    case Role::Meter: {
        if (std::optional<std::string_view> const text = aria(element, "aria-valuetext"); text && !blank(*text))
            return std::string(*text);
        if (std::optional<std::string_view> const now = aria(element, "aria-valuenow"); now && !blank(*now))
            return collapse_whitespace(*now);
        if (element.is_html("input"))
            return dom::forms::control_value(element);
        if (element.is_html("progress") || element.is_html("meter")) {
            if (std::optional<std::string_view> const value = attribute(element, "value"))
                return collapse_whitespace(*value);
        }
        return {};
    }
    default:
        return {};
    }
}

std::optional<std::string> Semantics::host_language_label(Element const& element, NameContext& context, bool is_root, NameSource* source)
{
    auto const set = [&](NameSource s) {
        if (source)
            *source = s;
    };
    auto const first_child = [&](std::string_view tag) -> Element const* {
        for (Node const* child : element.children()) {
            if (is_html(*child, tag))
                return static_cast<Element const*>(child);
        }
        return nullptr;
    };
    if (element.namespace_uri() == dom::ns::svg) {
        // SVG-AAM §8.1: a title child is the element's name.
        for (Node const* child : element.children()) {
            if (child->is_element() && static_cast<Element const&>(*child).is_svg("title")) {
                std::string text;
                for (Node const* t : child->children()) {
                    if (t->is_text())
                        text += static_cast<dom::Text const&>(*t).data;
                }
                set(NameSource::Title);
                return collapse_whitespace(text);
            }
        }
        if (element.is_svg("a")) {
            if (dom::Attr const* title = element.find_attribute("xlink:title"); title && !blank(title->value)) {
                set(NameSource::Title);
                return title->value;
            }
        }
        return std::nullopt;
    }
    if (!element.is_html())
        return std::nullopt;
    std::string const& name = element.local_name();
    if (name == "input") {
        std::string const type = dom::forms::input_type(element);
        if (type == "button" || type == "submit" || type == "reset") {
            // HTML-AAM §4.1.3: its labels, then the value; a submit or reset
            // button without one has the label the browser draws on it.
            if (std::string labels = label_elements_text(element, context); !labels.empty()) {
                set(NameSource::Label);
                return labels;
            }
            if (std::optional<std::string_view> const value = attribute(element, "value"); value && !value->empty()) {
                set(NameSource::Value);
                return std::string(*value);
            }
            if (type == "submit") {
                set(NameSource::Value);
                return std::string("Submit");
            }
            if (type == "reset") {
                set(NameSource::Value);
                return std::string("Reset");
            }
            return std::nullopt;
        }
        if (type == "image") {
            // HTML-AAM §4.1.4: its labels, alt, then value, then title,
            // then "Submit".
            if (std::string labels = label_elements_text(element, context); !labels.empty()) {
                set(NameSource::Label);
                return labels;
            }
            if (std::optional<std::string_view> const alt = attribute(element, "alt"); alt && !blank(*alt)) {
                set(NameSource::Alt);
                return std::string(*alt);
            }
            if (std::optional<std::string_view> const value = attribute(element, "value"); value && !blank(*value)) {
                set(NameSource::Value);
                return std::string(*value);
            }
            if (std::optional<std::string_view> const title = attribute(element, "title"); title && !blank(*title)) {
                set(NameSource::Title);
                return std::string(*title);
            }
            set(NameSource::Value);
            return std::string("Submit");
        }
    }
    if (labelable(element)) {
        if (std::string labels = label_elements_text(element, context); !labels.empty()) {
            set(NameSource::Label);
            return labels;
        }
        if (name == "input" || name == "textarea" || name == "select") {
            // HTML-AAM §4.1.2: title, then placeholder, then aria-placeholder.
            if (std::optional<std::string_view> const title = attribute(element, "title"); title && !blank(*title)) {
                set(NameSource::Title);
                return std::string(*title);
            }
            bool const takes_placeholder = name == "textarea" || (name == "input" && dom::forms::takes_placeholder(dom::forms::input_type(element)));
            if (takes_placeholder) {
                if (std::optional<std::string_view> const placeholder = attribute(element, "placeholder"); placeholder && !blank(*placeholder)) {
                    set(NameSource::Placeholder);
                    return std::string(*placeholder);
                }
            }
            if (std::optional<std::string_view> const placeholder = aria(element, "aria-placeholder"); placeholder && !blank(*placeholder)) {
                set(NameSource::Placeholder);
                return std::string(*placeholder);
            }
        }
        return std::nullopt;
    }
    if (name == "img" || name == "area") {
        // HTML-AAM §4.1.6: alt, even empty, is the name: a decorative image
        // is not named by its title.
        if (std::optional<std::string_view> const alt = attribute(element, "alt")) {
            set(NameSource::Alt);
            if (alt->empty())
                return std::string();
            return std::string(*alt);
        }
        return std::nullopt;
    }
    if (name == "fieldset") {
        // The fieldset's first legend child (HTML §4.10.16), unless it is
        // hidden: a hidden legend names nothing.
        if (Element const* legend = first_child("legend"); legend && !hidden(*legend)) {
            std::string text = collapse_whitespace(subtree_text(*legend, context));
            if (!text.empty()) {
                set(NameSource::Caption);
                return text;
            }
        }
        return std::nullopt;
    }
    if (name == "table") {
        // The first caption child that is not hidden.
        for (Node const* child : element.children()) {
            if (!is_html(*child, "caption") || hidden(*child))
                continue;
            std::string text = collapse_whitespace(subtree_text(*child, context));
            if (!text.empty()) {
                set(NameSource::Caption);
                return text;
            }
            break;
        }
        return std::nullopt;
    }
    // A figure is not named by its figcaption: the caption covers what the
    // figure holds and stays in the tree as its own text, as the HTML-AAM
    // revision under test (figure-name-no-figcaption) and the engines have it.
    if (name == "optgroup" || name == "option") {
        if (std::optional<std::string_view> const label = attribute(element, "label"); label && !blank(*label)) {
            set(NameSource::Attribute);
            return std::string(*label);
        }
        return std::nullopt;
    }
    (void)is_root;
    return std::nullopt;
}

std::string Semantics::text_alternative(Node const& node, NameContext& context, bool is_root)
{
    if (node.is_text()) {
        if (!context.include_hidden && hidden(node))
            return {};
        return rendered_text(static_cast<dom::Text const&>(node));
    }
    if (!node.is_element())
        return {};
    auto const& element = static_cast<Element const&>(node);
    if (!is_root) {
        if (context.visited.contains(&element) && &element != context.labelledby_of)
            return {};
    }
    context.visited.insert(&element);

    // 2A: hidden and not part of a traversal whose start was hidden. An
    // element hidden by visibility alone is skipped itself, but what in it
    // is visible again (visibility: visible inside) still counts.
    if (!is_root && !context.include_hidden && hidden(element)) {
        if (aria_hidden(element) || !visibility_hidden_only(element))
            return {};
        std::string out;
        for (Node* child : content_children(element)) {
            if (child->is_element())
                out += text_alternative(*child, context, false);
        }
        return out;
    }

    // A slot is no node of the tree: what it shows is its content.
    if (element.is_slot())
        return name_from_content(element, context);

    // 2B: aria-labelledby, once per traversal; in a description,
    // aria-describedby takes its place at the root.
    if (!context.in_labelledby && !context.description) {
        std::vector<Element*> const refs = idrefs(element, "aria-labelledby");
        if (!refs.empty()) {
            NameContext inner;
            inner.root = context.root;
            inner.in_labelledby = true;
            inner.labelledby_of = &element;
            inner.visited = context.visited;
            std::string out;
            for (Element* ref : refs) {
                inner.include_hidden = hidden(*ref);
                std::string const text = collapse_whitespace(text_alternative(*ref, inner, false));
                if (text.empty())
                    continue;
                if (!out.empty())
                    out += ' ';
                out += text;
            }
            for (Node const* seen : inner.visited)
                context.visited.insert(seen);
            if (!out.empty()) {
                if (is_root)
                    m_last_source = NameSource::LabelledBy;
                return out;
            }
        }
    }

    Role const role_value = role(element);

    // 2C: a control embedded in the label of another: its value.
    if (!is_root) {
        switch (role_value) {
        case Role::TextBox:
        case Role::SearchBox:
        case Role::Combobox:
        case Role::ListBox:
        case Role::Slider:
        case Role::SpinButton:
        case Role::ProgressBar:
        case Role::ScrollBar:
        case Role::Meter:
            return embedded_control_value(element, role_value, context);
        default:
            break;
        }
    }

    // 2D: aria-label.
    if (std::optional<std::string_view> const label = aria(element, "aria-label"); label && !blank(*label)) {
        if (is_root)
            m_last_source = NameSource::Attribute;
        return std::string(*label);
    }

    // A presentational element has no name of its own (ARIA 1.2 §5.4 none:
    // name prohibited); below the root its content still counts.
    if (is_root && role_value == Role::None)
        return {};

    // 2E: the host language's own text alternative, unless presentational.
    if (role_value != Role::None) {
        NameSource source = NameSource::None;
        if (std::optional<std::string> text = host_language_label(element, context, is_root, &source)) {
            if (!blank(*text) || source == NameSource::Alt) {
                if (is_root)
                    m_last_source = source;
                if (!blank(*text) || (source == NameSource::Alt && !is_root))
                    return *text;
                // An empty alt names an image nothing, and the tooltip does
                // not stand in for it.
                return {};
            }
        }
    }

    // 2F, 2H: the content, for a role that takes its name from it, for a
    // node named in a traversal, and for every node below one.
    // HTML's table rows and row groups are named by their author alone
    // (HTML-AAM §4.1: tr, thead, tbody, tfoot), whatever ARIA's row allows.
    bool allows_content = info(role_value).name_from & NameFromContents;
    if (element.is_html("tr") || element.is_html("thead") || element.is_html("tbody") || element.is_html("tfoot"))
        allows_content = false;
    bool const content = is_root ? allows_content : true;
    if (content && !element.is_html("select") && !element.is_html("textarea")) {
        std::string const text = name_from_content(element, context);
        // At the root a name of white space is none; below it the white
        // space is what separates the words around it.
        if (is_root ? !blank(text) : !text.empty()) {
            if (is_root)
                m_last_source = NameSource::Contents;
            return text;
        }
    }

    // 2I: the tooltip.
    if (element.is_html() || element.namespace_uri() == dom::ns::mathml) {
        if (std::optional<std::string_view> const title = attribute(element, "title"); title && !blank(*title)) {
            if (is_root)
                m_last_source = NameSource::Title;
            return std::string(*title);
        }
    }
    return {};
}

std::string Semantics::name(Element const& element)
{
    refresh_caches(element.document());
    NameContext context;
    context.root = &element;
    m_last_source = NameSource::None;
    std::string const text = text_alternative(element, context, true);
    return collapse_whitespace(text);
}

Semantics::NameSource Semantics::name_source(Element const& element)
{
    (void)name(element);
    return m_last_source;
}

std::string Semantics::description(Element const& element)
{
    refresh_caches(element.document());
    NameSource const source = name_source(element);
    std::vector<Element*> const refs = idrefs(element, "aria-describedby");
    if (!refs.empty()) {
        NameContext context;
        context.root = &element;
        context.description = true;
        context.in_describedby = true;
        context.visited.insert(&element);
        std::string out;
        for (Element* ref : refs) {
            context.include_hidden = hidden(*ref);
            std::string const text = collapse_whitespace(text_alternative(*ref, context, false));
            if (text.empty())
                continue;
            if (!out.empty())
                out += ' ';
            out += text;
        }
        if (!out.empty())
            return out;
    }
    if (std::optional<std::string_view> const text = aria(element, "aria-description"); text && !blank(*text))
        return collapse_whitespace(*text);
    // The host language's description (HTML-AAM §4.2): a table's caption
    // when the name came from elsewhere, then the title attribute when the
    // name did not come from it.
    if (element.is_html("table") && source != NameSource::Caption) {
        for (Node const* child : element.children()) {
            if (is_html(*child, "caption")) {
                NameContext context;
                context.root = &element;
                context.description = true;
                context.visited.insert(&element);
                std::string const text = collapse_whitespace(subtree_text(*child, context));
                if (!text.empty())
                    return text;
                break;
            }
        }
    }
    if (source != NameSource::Title) {
        if (std::optional<std::string_view> const title = attribute(element, "title"); title && !blank(*title))
            return collapse_whitespace(*title);
    }
    return {};
}

}
