#include "dom/Focus.h"

#include "core/Ascii.h"
#include "dom/Dom.h"
#include "dom/Forms.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace sashfold::dom::focus {

namespace {

Element const* parent_element(Node const& node)
{
    Node const* const parent = node.parent();
    return parent && parent->is_element() ? static_cast<Element const*>(parent) : nullptr;
}

ElementFacts facts_of(Element const& element, Environment const& env)
{
    if (env.facts)
        return env.facts(element);
    // Without style: the hidden attribute is the UA sheet's display: none
    // (HTML §15.3.1), but for hidden=until-found, which keeps a box.
    ElementFacts facts;
    if (Attr const* const hidden = element.find_attribute("hidden"))
        facts.display_none = !ascii_ci_equals(hidden->value, "until-found");
    // And a slot is display: contents there (HTML §15.3.1).
    facts.display_contents = element.is_html("slot");
    return facts;
}

bool attribute_ci(Element const& element, std::string_view name, std::string_view value)
{
    Attr const* const attribute = element.find_attribute(name);
    return attribute && ascii_ci_equals(attribute->value, value);
}

// The first summary element child of a details element (HTML §4.11.2).
Element const* first_summary(Element const& details)
{
    for (Node const* child : details.children()) {
        if (child->is_element() && static_cast<Element const*>(child)->is_html("summary"))
            return static_cast<Element const*>(child);
    }
    return nullptr;
}

// A link: an a or area element with an href (HTML §4.6.1), an SVG a with
// href or xlink:href.
bool is_link(Element const& element)
{
    if (element.is_html("a") || element.is_html("area"))
        return element.has_attribute("href");
    if (element.is_svg("a"))
        return element.has_attribute("href") || element.has_attribute("xlink:href");
    return false;
}

// The elements focusable by what they are, before rendering, inertness and
// the tabindex attribute are looked at (HTML §6.6.2's table, and the
// scrollable regions both shipping engines make focusable).
bool focusable_by_kind(Element const& element)
{
    if (is_link(element))
        return true;
    if (element.is_html("input"))
        return !attribute_ci(element, "type", "hidden") && !forms::actually_disabled(element);
    if (element.is_html("button") || element.is_html("select") || element.is_html("textarea"))
        return !forms::actually_disabled(element);
    if (element.is_html("summary")) {
        Element const* const details = parent_element(element);
        return details && details->is_html("details") && first_summary(*details) == &element;
    }
    if (element.is_html("iframe") || element.is_html("frame"))
        return true;
    if (element.is_html("audio") || element.is_html("video"))
        return element.has_attribute("controls");
    return is_editing_host(element);
}

// A disabled form control is no focusable area whatever its tabindex says
// (HTML §6.6.2: "a disabled element" is not focusable).
bool disabled(Element const& element)
{
    return forms::can_be_disabled(element) && !element.is_html("fieldset") && !element.is_html("option")
        && !element.is_html("optgroup") && forms::actually_disabled(element);
}

bool holds_focusable_area(Element const& element, Environment const& env)
{
    for (Node const* child : flat_children(element)) {
        if (!child->is_element())
            continue;
        auto const& inner = static_cast<Element const&>(*child);
        if (facts_of(inner, env).display_none)
            continue;
        if (is_focusable_area(inner, env) || holds_focusable_area(inner, env))
            return true;
    }
    return false;
}

bool is_scope_owner(Element const& element)
{
    return element.shadow_root() != nullptr || element.is_html("slot");
}

bool delegates_focus(Element const& element)
{
    ShadowRoot const* const shadow = element.shadow_root();
    return shadow && shadow->delegates_focus;
}

// One member of a focus navigation scope (HTML §6.6.3): an element that is
// a stop, or a scope owner whose own scope goes in at its place, ordered by
// its tabindex — a scope owner without one counts as zero.
struct Member {
    Element const* element = nullptr;
    long key = 0;
    bool stop = false;
    std::vector<Member> scope;
};

void collect_scope(Node const& under, Environment const& env, std::vector<Member>& members)
{
    for (Node const* child : flat_children(under)) {
        if (!child->is_element())
            continue;
        auto const& element = static_cast<Element const&>(*child);
        // Nothing under display: none has a box: the whole subtree is out.
        if (facts_of(element, env).display_none)
            continue;
        std::optional<long> const tabindex = tabindex_value(element);
        bool const stop = is_sequentially_focusable(element, env);
        if (is_scope_owner(element)) {
            // An owner taken out of the order takes its scope with it.
            if (tabindex && *tabindex < 0)
                continue;
            Member member { &element, tabindex.value_or(0), stop, {} };
            collect_scope(element, env, member.scope);
            members.push_back(std::move(member));
            continue;
        }
        if (stop)
            members.push_back(Member { &element, tabindex.value_or(0), true, {} });
        collect_scope(element, env, members);
    }
}

// Positive values first, ascending, then zero; ties keep flat-tree order.
void flatten(std::vector<Member>& members, std::vector<Element const*>& out)
{
    std::stable_sort(members.begin(), members.end(), [](Member const& a, Member const& b) {
        bool const a_positive = a.key > 0;
        bool const b_positive = b.key > 0;
        if (a_positive != b_positive)
            return a_positive;
        return a_positive && a.key < b.key;
    });
    for (Member& member : members) {
        if (member.stop)
            out.push_back(member.element);
        flatten(member.scope, out);
    }
}

// Every node's place in a preorder walk of the flat tree, for comparing a
// starting point with the entries of the order.
void number_flat_tree(Node const& node, std::unordered_map<Node const*, std::size_t>& places, std::size_t& next)
{
    places.emplace(&node, next++);
    for (Node const* child : flat_children(node))
        number_flat_tree(*child, places, next);
}

// The place of a node that is not in the flat tree itself (a host's child no
// slot takes, a node inside a display: none subtree is still numbered): its
// nearest inclusive ancestor that is.
std::optional<std::size_t> place_of(Node const* node, std::unordered_map<Node const*, std::size_t> const& places)
{
    for (; node; node = node->parent_or_host()) {
        if (auto const it = places.find(node); it != places.end())
            return it->second;
    }
    return std::nullopt;
}

bool is_radio(Element const& element)
{
    return element.is_html("input") && attribute_ci(element, "type", "radio");
}

bool same_radio_group(Element const& a, Element const& b)
{
    bool same = false;
    forms::each_in_radio_group(a, [&](Element const& member) {
        if (&member == &b)
            same = true;
    });
    return same;
}

// A radio button Tab passes over (HTML §4.10.5.1.15, as every engine does
// it): one of a group the focus is in already, or one of a group whose
// checked button is another.
bool radio_passed_over(Element const& radio, Node const* from)
{
    if (from && from->is_element()) {
        auto const& focused = static_cast<Element const&>(*from);
        if (&focused != &radio && is_radio(focused) && same_radio_group(focused, radio))
            return true;
    }
    bool other_checked = false;
    forms::each_in_radio_group(radio, [&](Element const& member) {
        if (&member != &radio && forms::checkedness(member))
            other_checked = true;
    });
    return other_checked && !forms::checkedness(radio);
}

Document const* content_of(Element const& element, Environment const& env)
{
    if (!env.content_document || !(element.is_html("iframe") || element.is_html("frame")))
        return nullptr;
    return env.content_document(element);
}

// The first (backward: last) stop of a document, frames entered; depth
// bounds a frame tree that would show itself.
Element const* edge_stop(Document const& document, bool backward, Environment const& env, int depth)
{
    Element const* const candidate = next_in_document(document, nullptr, backward, env);
    if (!candidate)
        return nullptr;
    if (Document const* const inner = depth < 16 ? content_of(*candidate, env) : nullptr) {
        if (Element const* const inside = edge_stop(*inner, backward, env, depth + 1))
            return inside;
    }
    return candidate;
}

}

std::optional<long> tabindex_value(Element const& element)
{
    Attr const* const attribute = element.find_attribute("tabindex");
    if (!attribute)
        return std::nullopt;
    // The rules for parsing integers: leading ASCII whitespace, a sign, one
    // digit at least; whatever follows the digits is ignored.
    std::string_view text = attribute->value;
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\f' || text[i] == '\r'))
        ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i >= text.size() || !is_ascii_digit(static_cast<unsigned char>(text[i])))
        return std::nullopt;
    long long value = 0;
    while (i < text.size() && is_ascii_digit(static_cast<unsigned char>(text[i]))) {
        value = std::min<long long>(value * 10 + (text[i] - '0'), static_cast<long long>(LONG_MAX));
        ++i;
    }
    return static_cast<long>(negative ? -value : value);
}

bool is_editing_host(Element const& element)
{
    Attr const* const attribute = element.find_attribute("contenteditable");
    if (!attribute)
        return false;
    std::string const& value = attribute->value;
    bool const editable = value.empty() || ascii_ci_equals(value, "true") || ascii_ci_equals(value, "plaintext-only");
    if (!editable)
        return false;
    Element const* const parent = parent_element(element);
    return !parent || !forms::editing_host_editable(*parent);
}

bool is_rendered_for_focus(Element const& element, Environment const& env)
{
    if (!element.is_connected())
        return false;
    ElementFacts const own = facts_of(element, env);
    // An area has no box of its own: its image's map gives it its place.
    if (!element.is_html("area") && (own.display_contents || !own.visible))
        return false;
    Node const* node = &element;
    while (node && node->is_element()) {
        if (facts_of(static_cast<Element const&>(*node), env).display_none)
            return false;
        Node const* const up = flat_parent(*node);
        // A closed details shows its summary alone (HTML §4.11.1).
        if (up && up->is_element()) {
            auto const& parent = static_cast<Element const&>(*up);
            if (parent.is_html("details") && !parent.has_attribute("open") && node != first_summary(parent))
                return false;
        }
        node = up;
    }
    // Out of the flat tree (a host's child that no slot takes): no box.
    return node != nullptr;
}

bool is_focusable_area(Element const& element, Environment const& env)
{
    if (disabled(element) || !is_rendered_for_focus(element, env))
        return false;
    ElementFacts const facts = facts_of(element, env);
    if (facts.inert)
        return false;
    if (focusable_by_kind(element) || facts.editing_host || tabindex_value(element))
        return true;
    // A scroll container with something to scroll and no focusable area
    // inside it takes the focus itself, so the keys can scroll it.
    return facts.scrollable && !holds_focusable_area(element, env);
}

bool is_sequentially_focusable(Element const& element, Environment const& env)
{
    if (delegates_focus(element))
        return false;
    std::optional<long> const tabindex = tabindex_value(element);
    if (tabindex && *tabindex < 0)
        return false;
    return is_focusable_area(element, env);
}

std::vector<Element const*> navigation_order(Document const& document, Environment const& env)
{
    std::vector<Member> members;
    collect_scope(document, env, members);
    std::vector<Element const*> order;
    flatten(members, order);
    return order;
}

Element const* next_in_document(Document const& document, Node const* from, bool backward, Environment const& env)
{
    std::vector<Element const*> const order = navigation_order(document, env);
    if (order.empty())
        return nullptr;
    std::ptrdiff_t const size = static_cast<std::ptrdiff_t>(order.size());
    std::ptrdiff_t const step = backward ? -1 : 1;
    std::ptrdiff_t start = backward ? size - 1 : 0;
    if (from) {
        auto const found = std::find(order.begin(), order.end(), from);
        if (found != order.end()) {
            start = (found - order.begin()) + step;
        } else {
            // A starting point that is no stop: the next stop after it in
            // the flat tree (before it, going back).
            std::unordered_map<Node const*, std::size_t> places;
            std::size_t next = 0;
            number_flat_tree(document, places, next);
            std::optional<std::size_t> const at = place_of(from, places);
            if (at) {
                std::ptrdiff_t best = -1;
                for (std::ptrdiff_t i = 0; i < size; ++i) {
                    std::optional<std::size_t> const place = place_of(order[static_cast<std::size_t>(i)], places);
                    if (!place)
                        continue;
                    bool const beyond = backward ? *place < *at : *place > *at;
                    if (!beyond)
                        continue;
                    if (best < 0) {
                        best = i;
                        continue;
                    }
                    std::size_t const best_place = *place_of(order[static_cast<std::size_t>(best)], places);
                    if (backward ? *place > best_place : *place < best_place)
                        best = i;
                }
                if (best < 0)
                    return nullptr;
                start = best;
            }
        }
    }
    for (std::ptrdiff_t i = start; i >= 0 && i < size; i += step) {
        Element const& candidate = *order[static_cast<std::size_t>(i)];
        if (is_radio(candidate) && radio_passed_over(candidate, from))
            continue;
        return &candidate;
    }
    return nullptr;
}

Element const* sequential_next(Document const& top, Node const* from, bool backward, Environment const& env)
{
    if (!from)
        return edge_stop(top, backward, env, 0);
    Document const* document = &from->document();
    Node const* at = from;
    for (int depth = 0; depth < 32; ++depth) {
        if (Element const* const candidate = next_in_document(*document, at, backward, env)) {
            if (Document const* const inner = content_of(*candidate, env)) {
                if (Element const* const inside = edge_stop(*inner, backward, env, 0))
                    return inside;
            }
            return candidate;
        }
        // This document is done: on from its container, in the document
        // that holds it.
        if (document == &top || !env.container_of)
            return nullptr;
        Element const* const container = env.container_of(*document);
        if (!container)
            return nullptr;
        document = &container->document();
        at = container;
    }
    return nullptr;
}

Element const* focus_delegate(Element const& host, Environment const& env)
{
    ShadowRoot const* const shadow = host.shadow_root();
    if (!shadow || !shadow->delegates_focus)
        return nullptr;
    // The autofocus delegate first, then the first focusable area; a host
    // inside that delegates too gives its own (HTML §6.6.4).
    for (bool const autofocus_only : { true, false }) {
        Element const* found = nullptr;
        auto const walk = [&](auto const& self, Node const& node) -> void {
            for (Node const* child : node.children()) {
                if (found)
                    return;
                if (!child->is_element())
                    continue;
                auto const& element = static_cast<Element const&>(*child);
                bool const wanted = !autofocus_only || element.has_attribute("autofocus");
                if (wanted && is_focusable_area(element, env)) {
                    found = &element;
                    return;
                }
                if (wanted && delegates_focus(element)) {
                    if (Element const* const inner = focus_delegate(element, env)) {
                        found = inner;
                        return;
                    }
                }
                self(self, element);
            }
        };
        walk(walk, *shadow);
        if (found)
            return found;
    }
    return nullptr;
}

Element const* click_focus_target(Node const& node, Environment const& env)
{
    for (Node const* at = &node; at; at = flat_parent(*at)) {
        if (!at->is_element())
            continue;
        auto const& element = static_cast<Element const&>(*at);
        if (delegates_focus(element)) {
            // The press is on the host or on something of its tree that
            // takes no focus: the focus goes where the host delegates it.
            if (Element const* const delegate = focus_delegate(element, env))
                return delegate;
        }
        if (is_focusable_area(element, env))
            return &element;
    }
    return nullptr;
}

}
