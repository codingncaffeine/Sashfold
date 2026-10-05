#pragma once

// Selectors (selectors-4, the reader-web subset): parsing from a rule
// prelude's component values, specificity, and matching against the DOM.
//
// Supported: type, universal, class, id; attribute selectors with every
// matcher and the i/s flags; descendant/child/sibling combinators; selector
// lists; :not/:is/:where (forgiving inside :is/:where); :has with relative
// selectors, and :scope for what it is relative to; :nth-child and
// :nth-last-child with `of S`; the structural
// pseudo-classes incl. an+b; pseudo-elements parse (::before et al) but
// never match until generated content lands. An unknown pseudo makes the
// selector invalid, which invalidates its whole list — the spec's behavior.

#include "css/Parser.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::dom {
class Element;
class Node;
}

namespace sashfold::css {

struct Specificity {
    int a = 0; // ids
    int b = 0; // classes, attributes, pseudo-classes
    int c = 0; // types, pseudo-elements

    friend auto operator<=>(Specificity const&, Specificity const&) = default;
    Specificity operator+(Specificity const& other) const
    {
        return { a + other.a, b + other.b, c + other.c };
    }
};

struct ComplexSelector;

struct SelectorList {
    std::vector<ComplexSelector> selectors;
};

struct AttributeSelector {
    enum class Match {
        Presence, // [attr]
        Exact, // [attr=v]
        Includes, // [attr~=v]
        Dash, // [attr|=v]
        Prefix, // [attr^=v]
        Suffix, // [attr$=v]
        Substring, // [attr*=v]
    };
    std::string name; // lowercased (HTML documents)
    Match match = Match::Presence;
    std::string value;
    bool case_insensitive = false; // [attr=v i]
};

struct SimpleSelector {
    enum class Kind {
        Universal,
        Type,
        Class,
        Id,
        Attribute,
        PseudoClass,
        PseudoElement,
    };
    enum class PseudoKind {
        None,
        // structural, matched statically
        Root,
        Empty,
        FirstChild,
        LastChild,
        OnlyChild,
        FirstOfType,
        LastOfType,
        OnlyOfType,
        NthChild,
        NthLastChild,
        NthOfType,
        NthLastOfType,
        AnyLink,
        Link,
        // logical
        Scope, // what a :has() argument is relative to (internal)
        ScopeRoot, // :scope, and `&` outside a nested rule: the scoping root
        Has,
        Not,
        Is,
        Where,
        // the states the document and the form controls hold (selectors-4
        // §9-§14), and :defined
        Hover,
        Active,
        Focus,
        FocusWithin,
        FocusVisible,
        Target,
        Fullscreen,
        Checked,
        Indeterminate,
        Default,
        Disabled,
        Enabled,
        Required,
        Optional,
        ReadOnly,
        ReadWrite,
        PlaceholderShown,
        Valid,
        Invalid,
        InRange,
        OutOfRange,
        Open,
        Defined,
        Lang, // :lang(), its list in `languages`
        Dir, // :dir(), `languages` holds the one direction
        // css-scoping-1 §3.2: the shadow host, from inside its shadow tree —
        // :host, :host(<compound>) with the compound in `argument`, and
        // :host-context(<compound>) for the host or anything above it.
        Host,
        HostContext,
        // valid selectors that match nothing here: :visited, the autofill,
        // a modal or popover, a user-interaction state, a vendor name
        NeverMatches,
    };

    Kind kind = Kind::Universal;
    PseudoKind pseudo = PseudoKind::None;
    std::string name; // type/class/id/attribute-less display name, pseudo name lowercased
    AttributeSelector attribute; // Kind::Attribute
    // :not/:is/:where and :has take a selector list; :nth-child and
    // :nth-last-child take the `of S` one, which counts their siblings.
    // Shared and never changed once read: the `&` of every rule nested in
    // one parent holds that parent's list, which is read once.
    std::shared_ptr<SelectorList const> argument;
    int nth_a = 0; // :nth-*(an+b)
    int nth_b = 0;
    std::vector<std::string> languages; // :lang() ranges, lower case; :dir()'s direction
};

struct CompoundSelector {
    std::vector<SimpleSelector> simples;
};

enum class Combinator {
    Descendant,
    Child, // >
    NextSibling, // +
    SubsequentSibling, // ~
};

struct ComplexSelector {
    // The pseudo-element the selector addresses, if any: ::before, ::after
    // and ::first-letter are taken out of the last compound at parse time,
    // so that the rest of the selector matches the originating element and
    // the cascade files the rule under that box; other pseudo-elements stay
    // in the compound and never match.
    enum class PseudoElement : std::uint8_t {
        None,
        Before,
        After,
        FirstLetter,
        // css-scoping-1 §3.3 and css-shadow-parts-1: the rest of the
        // selector matches a slot, or a shadow host, and the rule styles
        // the elements assigned to that slot which match `slotted`, or the
        // elements of that host's shadow tree whose part attribute has
        // every name in `parts` and which are in the states `part_states`
        // names (`::part(tab):hover`).
        Slotted,
        Part,
    };

    // compounds.size() == combinators.size() + 1; combinators[i] joins
    // compounds[i] and compounds[i+1]. Matching is right-to-left.
    std::vector<CompoundSelector> compounds;
    std::vector<Combinator> combinators;
    Specificity specificity;
    PseudoElement pseudo_element = PseudoElement::None;
    std::shared_ptr<SelectorList const> slotted;
    std::vector<std::string> parts;
    std::vector<SimpleSelector> part_states;
};

// Parses a rule prelude as a selector list. nullopt when any selector in the
// list is invalid (the caller drops the rule).
std::optional<SelectorList> parse_selector_list(std::vector<ComponentValue> const& prelude);

// A nested style rule's prelude (css-nesting-1 §2): a relative selector
// list, each selector made absolute against the parent rule's list. `&`
// stands for that list as :is() would; a selector that opens with a
// combinator, or writes no `&` at all, gets `& ` (or `& <combinator>`) in
// front. nullopt when any selector is invalid.
std::optional<SelectorList> parse_nested_selector_list(
    std::vector<ComponentValue> const& prelude, SelectorList const& parent);

// The element `:scope` (and `&` in a rule nested in nothing) means while
// the current thread matches: the root of an @scope, or nothing, which is
// the document's root element. Set and restored by the caller.
void set_scope_root(dom::Element const* root);
dom::Element const* scope_root();

// The shadow root whose sheets the selectors being matched on the current
// thread come from, or null for the document's: inside it :host is its
// host, which no other simple selector matches, and a combinator walks from
// the tree's topmost elements up to that host. Set and restored by the caller.
void set_shadow_scope(dom::Node const* shadow_root);
dom::Node const* shadow_scope();

// Whether an element is one a ::slotted() or ::part() selector styles:
// the argument of ::slotted(), or the names and the states of ::part().
bool matches_slotted(ComplexSelector const& selector, dom::Element const& element);
bool matches_part(ComplexSelector const& selector, dom::Element const& element);

// @supports selector(...) (css-conditional-3 §6): true when `prelude` is
// exactly one complex selector, valid down to every :is()/:where() argument
// with none of their usual forgiveness for a piece that fails to parse.
bool selector_list_is_strictly_valid(std::vector<ComponentValue> const& prelude);

// True when the element matches any selector of the list; `matched` (when
// given) receives the specificity of the best matching selector.
bool matches(SelectorList const& list, dom::Element const& element, Specificity* matched = nullptr);
bool matches(ComplexSelector const& selector, dom::Element const& element);

// True when the element matches every simple selector of the compound but
// its :has() ones, and those holding one in their arguments (:not(:has())):
// an element a :has() in the compound is tested on.
bool matches_compound_but_has(CompoundSelector const& compound, dom::Element const& element);

// Whether a simple selector is a :has() or holds one in its arguments.
bool contains_has(SimpleSelector const& simple);

// Whether the element is in the state a pseudo-class with no argument
// names (:hover, :checked, :disabled...): what an invalidation asks of an
// element whose state may have turned.
bool element_in_state(SimpleSelector::PseudoKind kind, dom::Element const& element);

}
