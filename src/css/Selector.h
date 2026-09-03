#pragma once

// Selectors (selectors-4, the reader-web subset): parsing from a rule
// prelude's component values, specificity, and matching against the DOM.
//
// Supported: type, universal, class, id; attribute selectors with every
// matcher and the i/s flags; descendant/child/sibling combinators; selector
// lists; :not/:is/:where (forgiving inside :is/:where); the structural
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
        Not,
        Is,
        Where,
        // known interactive/state pseudos: parse fine, never match (yet)
        NeverMatches,
    };

    Kind kind = Kind::Universal;
    PseudoKind pseudo = PseudoKind::None;
    std::string name; // type/class/id/attribute-less display name, pseudo name lowercased
    AttributeSelector attribute; // Kind::Attribute
    std::unique_ptr<SelectorList> argument; // :not/:is/:where
    int nth_a = 0; // :nth-*(an+b)
    int nth_b = 0;
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
    };

    // compounds.size() == combinators.size() + 1; combinators[i] joins
    // compounds[i] and compounds[i+1]. Matching is right-to-left.
    std::vector<CompoundSelector> compounds;
    std::vector<Combinator> combinators;
    Specificity specificity;
    PseudoElement pseudo_element = PseudoElement::None;
};

// Parses a rule prelude as a selector list. nullopt when any selector in the
// list is invalid (the caller drops the rule).
std::optional<SelectorList> parse_selector_list(std::vector<ComponentValue> const& prelude);

// True when the element matches any selector of the list; `matched` (when
// given) receives the specificity of the best matching selector.
bool matches(SelectorList const& list, dom::Element const& element, Specificity* matched = nullptr);
bool matches(ComplexSelector const& selector, dom::Element const& element);

}
