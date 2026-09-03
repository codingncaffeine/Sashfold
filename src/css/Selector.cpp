#include "css/Selector.h"

#include "core/Ascii.h"
#include "dom/Dom.h"

#include <algorithm>
#include <cmath>

namespace sashfold::css {

namespace {

// --- Parsing ------------------------------------------------------------------

struct Cursor {
    std::vector<ComponentValue> const& values;
    std::size_t index = 0;

    bool at_end() const { return index >= values.size(); }
    ComponentValue const* peek(std::size_t offset = 0) const
    {
        return index + offset < values.size() ? &values[index + offset] : nullptr;
    }
    ComponentValue const* consume() { return at_end() ? nullptr : &values[index++]; }
    bool skip_whitespace()
    {
        bool skipped = false;
        while (!at_end() && values[index].is_token(Token::Type::Whitespace)) {
            ++index;
            skipped = true;
        }
        return skipped;
    }
};

bool is_delim(ComponentValue const* value, char32_t delim)
{
    return value && value->is_token(Token::Type::Delim) && value->token().delim == delim;
}

// An+B (css-syntax §6.2) over the component values of an :nth-* argument.
// Returns false on garbage.
bool parse_an_plus_b(Cursor& cursor, int& a, int& b)
{
    a = 0;
    b = 0;
    cursor.skip_whitespace();
    ComponentValue const* first = cursor.consume();
    if (!first || !first->is_token())
        return false;
    Token const& token = first->token();

    auto const digits_suffix = [](std::string_view text, std::size_t from, int& out) {
        if (from >= text.size())
            return false;
        long value = 0;
        for (std::size_t i = from; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9')
                return false;
            value = value * 10 + (text[i] - '0');
            if (value > 1000000)
                return false;
        }
        out = static_cast<int>(value);
        return true;
    };

    // Consumes the optional "+ b" / "- b" / signed-integer tail once an
    // n-term with no attached digits has been seen.
    auto const parse_b_tail = [&](int& out) {
        std::size_t const saved = cursor.index;
        cursor.skip_whitespace();
        ComponentValue const* next = cursor.peek();
        if (!next || !next->is_token()) {
            cursor.index = saved;
            out = 0;
            return true;
        }
        Token const& tail = next->token();
        if (tail.type == Token::Type::Number && tail.numeric_type == Token::NumericType::Integer
            && tail.has_sign) {
            cursor.consume();
            out = static_cast<int>(tail.numeric_value);
            return true;
        }
        if (tail.type == Token::Type::Delim && (tail.delim == U'+' || tail.delim == U'-')) {
            int const sign = tail.delim == U'+' ? 1 : -1;
            cursor.consume();
            cursor.skip_whitespace();
            ComponentValue const* number = cursor.consume();
            if (!number || !number->is_token(Token::Type::Number))
                return false;
            Token const& value = number->token();
            if (value.numeric_type != Token::NumericType::Integer || value.has_sign)
                return false;
            out = sign * static_cast<int>(value.numeric_value);
            return true;
        }
        cursor.index = saved;
        out = 0;
        return true;
    };

    if (token.type == Token::Type::Ident) {
        std::string const name = [&] {
            std::string lowered;
            for (char const c : token.value)
                lowered += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
            return lowered;
        }();
        if (name == "odd") {
            a = 2;
            b = 1;
            return true;
        }
        if (name == "even") {
            a = 2;
            b = 0;
            return true;
        }
        if (name == "n" || name == "-n") {
            a = name[0] == '-' ? -1 : 1;
            return parse_b_tail(b);
        }
        if (name == "n-" || name == "-n-") {
            a = name[0] == '-' ? -1 : 1;
            cursor.skip_whitespace();
            ComponentValue const* number = cursor.consume();
            if (!number || !number->is_token(Token::Type::Number))
                return false;
            Token const& value = number->token();
            if (value.numeric_type != Token::NumericType::Integer || value.has_sign)
                return false;
            b = -static_cast<int>(value.numeric_value);
            return true;
        }
        if (name.starts_with("n-")) {
            a = 1;
            int digits = 0;
            if (!digits_suffix(name, 2, digits))
                return false;
            b = -digits;
            return true;
        }
        if (name.starts_with("-n-")) {
            a = -1;
            int digits = 0;
            if (!digits_suffix(name, 3, digits))
                return false;
            b = -digits;
            return true;
        }
        return false;
    }

    if (token.type == Token::Type::Delim && token.delim == U'+') {
        // '+' immediately followed by an n ident (no whitespace between).
        ComponentValue const* next = cursor.consume();
        if (!next || !next->is_token(Token::Type::Ident))
            return false;
        std::string lowered;
        for (char const c : next->token().value)
            lowered += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
        if (lowered == "n") {
            a = 1;
            return parse_b_tail(b);
        }
        if (lowered == "n-") {
            a = 1;
            cursor.skip_whitespace();
            ComponentValue const* number = cursor.consume();
            if (!number || !number->is_token(Token::Type::Number))
                return false;
            Token const& value = number->token();
            if (value.numeric_type != Token::NumericType::Integer || value.has_sign)
                return false;
            b = -static_cast<int>(value.numeric_value);
            return true;
        }
        if (lowered.starts_with("n-")) {
            a = 1;
            int digits = 0;
            if (!digits_suffix(lowered, 2, digits))
                return false;
            b = -digits;
            return true;
        }
        return false;
    }

    if (token.type == Token::Type::Number) {
        if (token.numeric_type != Token::NumericType::Integer)
            return false;
        b = static_cast<int>(token.numeric_value);
        return true;
    }

    if (token.type == Token::Type::Dimension) {
        if (token.numeric_type != Token::NumericType::Integer)
            return false;
        std::string lowered;
        for (char const c : token.unit)
            lowered += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
        a = static_cast<int>(token.numeric_value);
        if (lowered == "n")
            return parse_b_tail(b);
        if (lowered == "n-") {
            cursor.skip_whitespace();
            ComponentValue const* number = cursor.consume();
            if (!number || !number->is_token(Token::Type::Number))
                return false;
            Token const& value = number->token();
            if (value.numeric_type != Token::NumericType::Integer || value.has_sign)
                return false;
            b = -static_cast<int>(value.numeric_value);
            return true;
        }
        if (lowered.starts_with("n-")) {
            int digits = 0;
            if (!digits_suffix(lowered, 2, digits))
                return false;
            b = -digits;
            return true;
        }
        return false;
    }

    return false;
}

std::string lowercased(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        out += static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

struct PseudoName {
    std::string_view name;
    SimpleSelector::PseudoKind kind;
};

constexpr PseudoName pseudo_classes[] = {
    { "root", SimpleSelector::PseudoKind::Root },
    { "empty", SimpleSelector::PseudoKind::Empty },
    { "first-child", SimpleSelector::PseudoKind::FirstChild },
    { "last-child", SimpleSelector::PseudoKind::LastChild },
    { "only-child", SimpleSelector::PseudoKind::OnlyChild },
    { "first-of-type", SimpleSelector::PseudoKind::FirstOfType },
    { "last-of-type", SimpleSelector::PseudoKind::LastOfType },
    { "only-of-type", SimpleSelector::PseudoKind::OnlyOfType },
    { "any-link", SimpleSelector::PseudoKind::AnyLink },
    { "link", SimpleSelector::PseudoKind::Link },
    // Interactive and state pseudo-classes: valid selectors, no matches yet.
    { "hover", SimpleSelector::PseudoKind::NeverMatches },
    { "active", SimpleSelector::PseudoKind::NeverMatches },
    { "focus", SimpleSelector::PseudoKind::NeverMatches },
    { "focus-within", SimpleSelector::PseudoKind::NeverMatches },
    { "focus-visible", SimpleSelector::PseudoKind::NeverMatches },
    { "visited", SimpleSelector::PseudoKind::NeverMatches },
    { "target", SimpleSelector::PseudoKind::NeverMatches },
    { "checked", SimpleSelector::PseudoKind::NeverMatches },
    { "disabled", SimpleSelector::PseudoKind::NeverMatches },
    { "enabled", SimpleSelector::PseudoKind::NeverMatches },
    { "required", SimpleSelector::PseudoKind::NeverMatches },
    { "optional", SimpleSelector::PseudoKind::NeverMatches },
    { "read-only", SimpleSelector::PseudoKind::NeverMatches },
    { "read-write", SimpleSelector::PseudoKind::NeverMatches },
    { "placeholder-shown", SimpleSelector::PseudoKind::NeverMatches },
    { "default", SimpleSelector::PseudoKind::NeverMatches },
    { "indeterminate", SimpleSelector::PseudoKind::NeverMatches },
    { "scope", SimpleSelector::PseudoKind::NeverMatches },
};

constexpr std::string_view pseudo_elements[] = {
    "before", "after", "first-line", "first-letter", "marker", "selection",
    "placeholder", "backdrop", "file-selector-button",
};

std::optional<SelectorList> parse_selector_list_internal(
    std::vector<ComponentValue> const& values, bool forgiving);

// One compound selector; cursor sits at its first simple selector.
bool parse_compound(Cursor& cursor, CompoundSelector& compound, Specificity& specificity)
{
    bool first = true;
    while (!cursor.at_end()) {
        ComponentValue const* value = cursor.peek();
        if (value->is_token(Token::Type::Whitespace) || value->is_token(Token::Type::Comma))
            break;

        // Universal or type (first position only).
        if (is_delim(value, U'*')) {
            if (!first)
                return false;
            cursor.consume();
            SimpleSelector simple;
            simple.kind = SimpleSelector::Kind::Universal;
            compound.simples.push_back(std::move(simple));
            first = false;
            continue;
        }
        if (value->is_token(Token::Type::Ident)) {
            if (!first)
                return false;
            SimpleSelector simple;
            simple.kind = SimpleSelector::Kind::Type;
            simple.name = cursor.consume()->token().value;
            specificity.c += 1;
            compound.simples.push_back(std::move(simple));
            first = false;
            continue;
        }
        first = false;

        // #id — requires the hash to be ident-shaped.
        if (value->is_token(Token::Type::Hash)) {
            Token const& token = value->token();
            if (token.hash_type != Token::HashType::Id)
                return false;
            cursor.consume();
            SimpleSelector simple;
            simple.kind = SimpleSelector::Kind::Id;
            simple.name = token.value;
            specificity.a += 1;
            compound.simples.push_back(std::move(simple));
            continue;
        }

        // .class
        if (is_delim(value, U'.')) {
            cursor.consume();
            ComponentValue const* name = cursor.consume();
            if (!name || !name->is_token(Token::Type::Ident))
                return false;
            SimpleSelector simple;
            simple.kind = SimpleSelector::Kind::Class;
            simple.name = name->token().value;
            specificity.b += 1;
            compound.simples.push_back(std::move(simple));
            continue;
        }

        // [attribute...]
        if (value->is_block() && value->block().open == Token::Type::OpenSquare) {
            SimpleBlock const& block = cursor.consume()->block();
            Cursor inner { block.values, 0 };
            inner.skip_whitespace();
            ComponentValue const* name = inner.consume();
            if (!name || !name->is_token(Token::Type::Ident))
                return false;
            SimpleSelector simple;
            simple.kind = SimpleSelector::Kind::Attribute;
            simple.attribute.name = lowercased(name->token().value);
            inner.skip_whitespace();
            if (!inner.at_end()) {
                using Match = AttributeSelector::Match;
                ComponentValue const* matcher = inner.consume();
                Match match = Match::Exact;
                if (is_delim(matcher, U'~'))
                    match = Match::Includes;
                else if (is_delim(matcher, U'|'))
                    match = Match::Dash;
                else if (is_delim(matcher, U'^'))
                    match = Match::Prefix;
                else if (is_delim(matcher, U'$'))
                    match = Match::Suffix;
                else if (is_delim(matcher, U'*'))
                    match = Match::Substring;
                else if (!is_delim(matcher, U'='))
                    return false;
                if (match != Match::Exact) {
                    // The '=' must follow the modifier immediately.
                    ComponentValue const* equals = inner.consume();
                    if (!is_delim(equals, U'='))
                        return false;
                }
                simple.attribute.match = match;
                inner.skip_whitespace();
                ComponentValue const* attr_value = inner.consume();
                if (!attr_value || !attr_value->is_token())
                    return false;
                Token const& value_token = attr_value->token();
                if (value_token.type != Token::Type::String && value_token.type != Token::Type::Ident)
                    return false;
                simple.attribute.value = value_token.value;
                inner.skip_whitespace();
                if (!inner.at_end()) {
                    ComponentValue const* flag = inner.consume();
                    if (!flag || !flag->is_token(Token::Type::Ident))
                        return false;
                    std::string const flag_name = lowercased(flag->token().value);
                    if (flag_name == "i")
                        simple.attribute.case_insensitive = true;
                    else if (flag_name != "s")
                        return false;
                    inner.skip_whitespace();
                }
                if (!inner.at_end())
                    return false;
            }
            specificity.b += 1;
            compound.simples.push_back(std::move(simple));
            continue;
        }

        // Pseudo-classes and pseudo-elements.
        if (value->is_token(Token::Type::Colon)) {
            cursor.consume();
            bool element_form = false;
            if (cursor.peek() && cursor.peek()->is_token(Token::Type::Colon)) {
                cursor.consume();
                element_form = true;
            }
            ComponentValue const* name_value = cursor.consume();
            if (!name_value)
                return false;

            if (name_value->is_token(Token::Type::Ident)) {
                std::string const name = lowercased(name_value->token().value);
                bool const legacy_element = !element_form
                    && (name == "before" || name == "after" || name == "first-line"
                        || name == "first-letter");
                if (element_form || legacy_element) {
                    if (std::find(std::begin(pseudo_elements), std::end(pseudo_elements), name)
                        == std::end(pseudo_elements))
                        return false;
                    SimpleSelector simple;
                    simple.kind = SimpleSelector::Kind::PseudoElement;
                    simple.name = name;
                    specificity.c += 1;
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                auto const it = std::find_if(std::begin(pseudo_classes), std::end(pseudo_classes),
                    [&](PseudoName const& entry) { return entry.name == name; });
                if (it == std::end(pseudo_classes))
                    return false;
                SimpleSelector simple;
                simple.kind = SimpleSelector::Kind::PseudoClass;
                simple.pseudo = it->kind;
                simple.name = name;
                specificity.b += 1;
                compound.simples.push_back(std::move(simple));
                continue;
            }

            if (name_value->is_function()) {
                if (element_form)
                    return false; // no functional pseudo-elements supported
                FunctionValue const& function = name_value->function();
                std::string const name = lowercased(function.name);
                SimpleSelector simple;
                simple.kind = SimpleSelector::Kind::PseudoClass;
                simple.name = name;
                if (name == "not" || name == "is" || name == "where") {
                    bool const forgiving = name != "not";
                    auto argument = parse_selector_list_internal(function.values, forgiving);
                    if (!argument)
                        return false;
                    simple.pseudo = name == "not" ? SimpleSelector::PseudoKind::Not
                        : name == "is"           ? SimpleSelector::PseudoKind::Is
                                                 : SimpleSelector::PseudoKind::Where;
                    Specificity best;
                    for (ComplexSelector const& inner_selector : argument->selectors)
                        best = std::max(best, inner_selector.specificity);
                    if (simple.pseudo != SimpleSelector::PseudoKind::Where)
                        specificity = specificity + best;
                    simple.argument = std::make_unique<SelectorList>(std::move(*argument));
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type"
                    || name == "nth-last-of-type") {
                    Cursor argument { function.values, 0 };
                    if (!parse_an_plus_b(argument, simple.nth_a, simple.nth_b))
                        return false;
                    argument.skip_whitespace();
                    if (!argument.at_end())
                        return false; // ("of S" not supported yet)
                    simple.pseudo = name == "nth-child" ? SimpleSelector::PseudoKind::NthChild
                        : name == "nth-last-child"      ? SimpleSelector::PseudoKind::NthLastChild
                        : name == "nth-of-type"         ? SimpleSelector::PseudoKind::NthOfType
                                                        : SimpleSelector::PseudoKind::NthLastOfType;
                    specificity.b += 1;
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                return false; // unknown functional pseudo-class
            }
            return false;
        }

        return false; // unrecognized construct in a compound
    }
    return !compound.simples.empty();
}

// A pseudo-element may only end a selector: one in any compound but the
// last, or two in the last, makes the selector invalid. ::before, ::after
// and ::first-letter are lifted out of the last compound into the
// selector's pseudo_element; the rest stay and never match.
bool settle_pseudo_elements(ComplexSelector& selector)
{
    for (std::size_t i = 0; i + 1 < selector.compounds.size(); ++i) {
        for (SimpleSelector const& simple : selector.compounds[i].simples) {
            if (simple.kind == SimpleSelector::Kind::PseudoElement)
                return false;
        }
    }
    std::vector<SimpleSelector>& last = selector.compounds.back().simples;
    std::size_t count = 0;
    for (SimpleSelector const& simple : last) {
        if (simple.kind == SimpleSelector::Kind::PseudoElement)
            ++count;
    }
    if (count > 1)
        return false;
    for (std::size_t i = 0; i < last.size(); ++i) {
        if (last[i].kind != SimpleSelector::Kind::PseudoElement)
            continue;
        if (last[i].name == "before")
            selector.pseudo_element = ComplexSelector::PseudoElement::Before;
        else if (last[i].name == "after")
            selector.pseudo_element = ComplexSelector::PseudoElement::After;
        else if (last[i].name == "first-letter")
            selector.pseudo_element = ComplexSelector::PseudoElement::FirstLetter;
        else
            break;
        last.erase(last.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    return true;
}

std::optional<ComplexSelector> parse_complex(Cursor& cursor)
{
    ComplexSelector selector;
    cursor.skip_whitespace();
    while (true) {
        CompoundSelector compound;
        if (!parse_compound(cursor, compound, selector.specificity))
            return std::nullopt;
        selector.compounds.push_back(std::move(compound));

        bool const whitespace = cursor.skip_whitespace();
        if (cursor.at_end() || cursor.peek()->is_token(Token::Type::Comma)) {
            if (!settle_pseudo_elements(selector))
                return std::nullopt;
            return selector;
        }

        Combinator combinator = Combinator::Descendant;
        ComponentValue const* next = cursor.peek();
        if (is_delim(next, U'>')) {
            combinator = Combinator::Child;
            cursor.consume();
        } else if (is_delim(next, U'+')) {
            combinator = Combinator::NextSibling;
            cursor.consume();
        } else if (is_delim(next, U'~')) {
            combinator = Combinator::SubsequentSibling;
            cursor.consume();
        } else if (!whitespace) {
            return std::nullopt; // two compounds with no separation
        }
        cursor.skip_whitespace();
        if (cursor.at_end() || cursor.peek()->is_token(Token::Type::Comma))
            return std::nullopt; // dangling combinator
        selector.combinators.push_back(combinator);
    }
}

std::optional<SelectorList> parse_selector_list_internal(
    std::vector<ComponentValue> const& values, bool forgiving)
{
    SelectorList list;
    Cursor cursor { values, 0 };
    cursor.skip_whitespace();
    if (cursor.at_end())
        return forgiving ? std::optional<SelectorList>(std::move(list)) : std::nullopt;
    while (true) {
        std::optional<ComplexSelector> selector = parse_complex(cursor);
        if (selector) {
            list.selectors.push_back(std::move(*selector));
        } else if (forgiving) {
            // Skip to the next top-level comma.
            while (!cursor.at_end() && !cursor.peek()->is_token(Token::Type::Comma))
                cursor.consume();
        } else {
            return std::nullopt;
        }
        cursor.skip_whitespace();
        if (cursor.at_end())
            return list;
        if (!cursor.peek()->is_token(Token::Type::Comma))
            return std::nullopt;
        cursor.consume();
        cursor.skip_whitespace();
        if (cursor.at_end())
            return std::nullopt; // trailing comma
    }
}

// --- Matching -----------------------------------------------------------------

dom::Element const* parent_element(dom::Element const& element)
{
    dom::Node const* parent = element.parent();
    if (parent && parent->is_element())
        return static_cast<dom::Element const*>(parent);
    return nullptr;
}

dom::Element const* previous_element_sibling(dom::Element const& element)
{
    for (dom::Node const* node = element.previous_sibling(); node; node = node->previous_sibling()) {
        if (node->is_element())
            return static_cast<dom::Element const*>(node);
    }
    return nullptr;
}

bool same_type(dom::Element const& a, dom::Element const& b)
{
    return a.namespace_uri() == b.namespace_uri() && a.local_name() == b.local_name();
}

// 1-based position among element siblings; of_type restricts to same-type.
int sibling_index(dom::Element const& element, bool from_end, bool of_type)
{
    dom::Node const* parent = element.parent();
    if (!parent)
        return 1;
    int index = 0;
    auto const& siblings = parent->children();
    if (!from_end) {
        for (dom::Node const* node : siblings) {
            if (!node->is_element())
                continue;
            auto const* sibling = static_cast<dom::Element const*>(node);
            if (of_type && !same_type(*sibling, element))
                continue;
            ++index;
            if (sibling == &element)
                return index;
        }
    } else {
        for (std::size_t i = siblings.size(); i-- > 0;) {
            if (!siblings[i]->is_element())
                continue;
            auto const* sibling = static_cast<dom::Element const*>(siblings[i]);
            if (of_type && !same_type(*sibling, element))
                continue;
            ++index;
            if (sibling == &element)
                return index;
        }
    }
    return index;
}

bool an_plus_b_matches(int a, int b, int index)
{
    if (a == 0)
        return index == b;
    int const delta = index - b;
    if ((delta < 0 && a > 0) || (delta > 0 && a < 0))
        return false;
    return delta % a == 0;
}

bool attribute_value_matches(AttributeSelector const& selector, std::string_view actual)
{
    auto const equals = [&](std::string_view a, std::string_view b) {
        return selector.case_insensitive ? ascii_ci_equals(a, b) : a == b;
    };
    std::string_view const wanted = selector.value;
    switch (selector.match) {
    case AttributeSelector::Match::Presence:
        return true;
    case AttributeSelector::Match::Exact:
        return equals(actual, wanted);
    case AttributeSelector::Match::Includes: {
        if (wanted.empty())
            return false;
        std::size_t start = 0;
        while (start < actual.size()) {
            while (start < actual.size() && is_tokenizer_whitespace(static_cast<unsigned char>(actual[start])))
                ++start;
            std::size_t end = start;
            while (end < actual.size() && !is_tokenizer_whitespace(static_cast<unsigned char>(actual[end])))
                ++end;
            if (end > start && equals(actual.substr(start, end - start), wanted))
                return true;
            start = end;
        }
        return false;
    }
    case AttributeSelector::Match::Dash:
        if (equals(actual, wanted))
            return true;
        return actual.size() > wanted.size() && actual[wanted.size()] == '-'
            && equals(actual.substr(0, wanted.size()), wanted);
    case AttributeSelector::Match::Prefix:
        return !wanted.empty() && actual.size() >= wanted.size()
            && equals(actual.substr(0, wanted.size()), wanted);
    case AttributeSelector::Match::Suffix:
        return !wanted.empty() && actual.size() >= wanted.size()
            && equals(actual.substr(actual.size() - wanted.size()), wanted);
    case AttributeSelector::Match::Substring: {
        if (wanted.empty() || actual.size() < wanted.size())
            return false;
        if (!selector.case_insensitive)
            return actual.find(wanted) != std::string_view::npos;
        for (std::size_t i = 0; i + wanted.size() <= actual.size(); ++i) {
            if (ascii_ci_equals(actual.substr(i, wanted.size()), wanted))
                return true;
        }
        return false;
    }
    }
    return false;
}

bool class_list_contains(std::string_view class_attribute, std::string_view wanted, bool insensitive)
{
    std::size_t start = 0;
    while (start < class_attribute.size()) {
        while (start < class_attribute.size()
            && is_tokenizer_whitespace(static_cast<unsigned char>(class_attribute[start])))
            ++start;
        std::size_t end = start;
        while (end < class_attribute.size()
            && !is_tokenizer_whitespace(static_cast<unsigned char>(class_attribute[end])))
            ++end;
        if (end > start) {
            std::string_view const item = class_attribute.substr(start, end - start);
            if (insensitive ? ascii_ci_equals(item, wanted) : item == wanted)
                return true;
        }
        start = end;
    }
    return false;
}

bool matches_compound(CompoundSelector const& compound, dom::Element const& element);

bool matches_simple(SimpleSelector const& simple, dom::Element const& element)
{
    bool const quirks = element.document().quirks_mode == dom::QuirksMode::Yes;
    switch (simple.kind) {
    case SimpleSelector::Kind::Universal:
        return true;
    case SimpleSelector::Kind::Type:
        if (element.is_html())
            return ascii_ci_equals(simple.name, element.local_name());
        return simple.name == element.local_name();
    case SimpleSelector::Kind::Class: {
        dom::Attr const* attribute = element.find_attribute("class");
        return attribute && class_list_contains(attribute->value, simple.name, quirks);
    }
    case SimpleSelector::Kind::Id: {
        dom::Attr const* attribute = element.find_attribute("id");
        if (!attribute)
            return false;
        return quirks ? ascii_ci_equals(attribute->value, simple.name)
                      : attribute->value == simple.name;
    }
    case SimpleSelector::Kind::Attribute: {
        dom::Attr const* attribute = element.find_attribute(simple.attribute.name);
        return attribute && attribute_value_matches(simple.attribute, attribute->value);
    }
    case SimpleSelector::Kind::PseudoElement:
        return false; // no generated content yet
    case SimpleSelector::Kind::PseudoClass:
        break;
    }

    switch (simple.pseudo) {
    case SimpleSelector::PseudoKind::None:
    case SimpleSelector::PseudoKind::NeverMatches:
        return false;
    case SimpleSelector::PseudoKind::Root:
        return !parent_element(element) && element.parent() != nullptr;
    case SimpleSelector::PseudoKind::Empty:
        for (dom::Node const* child : element.children()) {
            if (child->is_element())
                return false;
            if (child->is_text() && !static_cast<dom::Text const*>(child)->data.empty())
                return false;
        }
        return true;
    case SimpleSelector::PseudoKind::FirstChild:
        return sibling_index(element, false, false) == 1;
    case SimpleSelector::PseudoKind::LastChild:
        return sibling_index(element, true, false) == 1;
    case SimpleSelector::PseudoKind::OnlyChild:
        return sibling_index(element, false, false) == 1 && sibling_index(element, true, false) == 1;
    case SimpleSelector::PseudoKind::FirstOfType:
        return sibling_index(element, false, true) == 1;
    case SimpleSelector::PseudoKind::LastOfType:
        return sibling_index(element, true, true) == 1;
    case SimpleSelector::PseudoKind::OnlyOfType:
        return sibling_index(element, false, true) == 1 && sibling_index(element, true, true) == 1;
    case SimpleSelector::PseudoKind::NthChild:
        return an_plus_b_matches(simple.nth_a, simple.nth_b, sibling_index(element, false, false));
    case SimpleSelector::PseudoKind::NthLastChild:
        return an_plus_b_matches(simple.nth_a, simple.nth_b, sibling_index(element, true, false));
    case SimpleSelector::PseudoKind::NthOfType:
        return an_plus_b_matches(simple.nth_a, simple.nth_b, sibling_index(element, false, true));
    case SimpleSelector::PseudoKind::NthLastOfType:
        return an_plus_b_matches(simple.nth_a, simple.nth_b, sibling_index(element, true, true));
    case SimpleSelector::PseudoKind::AnyLink:
    case SimpleSelector::PseudoKind::Link:
        return element.is_html() && (element.local_name() == "a" || element.local_name() == "area")
            && element.has_attribute("href");
    case SimpleSelector::PseudoKind::Not:
        if (!simple.argument)
            return false;
        for (ComplexSelector const& inner : simple.argument->selectors) {
            if (matches(inner, element))
                return false;
        }
        return true;
    case SimpleSelector::PseudoKind::Is:
    case SimpleSelector::PseudoKind::Where:
        if (!simple.argument)
            return false;
        for (ComplexSelector const& inner : simple.argument->selectors) {
            if (matches(inner, element))
                return true;
        }
        return false;
    }
    return false;
}

bool matches_compound(CompoundSelector const& compound, dom::Element const& element)
{
    for (SimpleSelector const& simple : compound.simples) {
        if (!matches_simple(simple, element))
            return false;
    }
    return true;
}

bool matches_from(ComplexSelector const& selector, std::size_t compound_index,
    dom::Element const& element)
{
    if (!matches_compound(selector.compounds[compound_index], element))
        return false;
    if (compound_index == 0)
        return true;

    Combinator const combinator = selector.combinators[compound_index - 1];
    switch (combinator) {
    case Combinator::Child: {
        dom::Element const* parent = parent_element(element);
        return parent && matches_from(selector, compound_index - 1, *parent);
    }
    case Combinator::Descendant: {
        for (dom::Element const* ancestor = parent_element(element); ancestor;
            ancestor = parent_element(*ancestor)) {
            if (matches_from(selector, compound_index - 1, *ancestor))
                return true;
        }
        return false;
    }
    case Combinator::NextSibling: {
        dom::Element const* sibling = previous_element_sibling(element);
        return sibling && matches_from(selector, compound_index - 1, *sibling);
    }
    case Combinator::SubsequentSibling: {
        for (dom::Element const* sibling = previous_element_sibling(element); sibling;
            sibling = previous_element_sibling(*sibling)) {
            if (matches_from(selector, compound_index - 1, *sibling))
                return true;
        }
        return false;
    }
    }
    return false;
}

} // namespace

std::optional<SelectorList> parse_selector_list(std::vector<ComponentValue> const& prelude)
{
    return parse_selector_list_internal(prelude, false);
}

bool matches(ComplexSelector const& selector, dom::Element const& element)
{
    if (selector.compounds.empty())
        return false;
    return matches_from(selector, selector.compounds.size() - 1, element);
}

bool matches(SelectorList const& list, dom::Element const& element, Specificity* matched)
{
    bool any = false;
    Specificity best;
    for (ComplexSelector const& selector : list.selectors) {
        // A pseudo-element selector matches no element as such: it addresses
        // a box the element generates, which the cascade asks about per
        // selector.
        if (selector.pseudo_element != ComplexSelector::PseudoElement::None)
            continue;
        if (matches(selector, element)) {
            if (!matched)
                return true;
            any = true;
            best = std::max(best, selector.specificity);
        }
    }
    if (any && matched)
        *matched = best;
    return any;
}

}
