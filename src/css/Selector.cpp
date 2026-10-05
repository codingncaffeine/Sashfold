#include "css/Selector.h"

#include "core/Ascii.h"
#include "dom/Dom.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace sashfold::css {

namespace {

// --- Parsing ------------------------------------------------------------------

// selector_list_is_strictly_valid's own request that :is()/:where() (:has()
// and the of-S list of :nth-child already parse this way unconditionally)
// treat an invalid piece as invalidating the whole selector instead of
// forgiving it — set only for the duration of that one call, and never
// shared across threads, matching a page's ordinary selector matching
// (::is(), :where() stay forgiving there, per selectors-4).
thread_local bool t_selectors_unforgiving = false;

// selectors-4 §4.2: ":has() is not valid within :has()", to keep its
// evaluation from having to consider its own effects. Tracked here (rather
// than threaded through every parse function) because it must hold across
// :is()/:where() too — :has(:is(:has(a))) is exactly as invalid.
thread_local bool t_inside_has = false;

// css-nesting-1 §3: what `&` stands for while a nested rule's selector is
// read — its parent rule's list, less the selectors that address a
// pseudo-element (which `&` cannot represent) — and the most specific of
// them, which is what `&` weighs, as :is() would. Null outside a nested
// rule, where `&` is the scoping root and weighs nothing.
thread_local std::shared_ptr<SelectorList const> t_nest_parent;
thread_local Specificity t_nest_specificity;

// Whether `&` is written anywhere in the values, inside functions and
// brackets too: such a selector is not made relative a second time.
bool writes_nesting(std::vector<ComponentValue> const& values)
{
    for (ComponentValue const& value : values) {
        if (value.is_token(Token::Type::Delim) && value.token().delim == U'&')
            return true;
        if (value.is_function() && writes_nesting(value.function().values))
            return true;
        if (value.is_block() && writes_nesting(value.block().values))
            return true;
    }
    return false;
}

// The simple selector `&` is read as.
SimpleSelector nesting_selector()
{
    SimpleSelector simple;
    simple.kind = SimpleSelector::Kind::PseudoClass;
    simple.name = "&";
    if (t_nest_parent) {
        simple.pseudo = SimpleSelector::PseudoKind::Is;
        simple.argument = t_nest_parent;
    } else {
        simple.pseudo = SimpleSelector::PseudoKind::ScopeRoot;
    }
    return simple;
}

// What :scope means while matching: see set_scope_root.
thread_local dom::Element const* t_scope_root = nullptr;

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
    { "-webkit-any-link", SimpleSelector::PseudoKind::AnyLink },
    // The user's actions and the document's states (selectors-4 §9, §10).
    { "hover", SimpleSelector::PseudoKind::Hover },
    { "active", SimpleSelector::PseudoKind::Active },
    { "focus", SimpleSelector::PseudoKind::Focus },
    { "focus-within", SimpleSelector::PseudoKind::FocusWithin },
    { "focus-visible", SimpleSelector::PseudoKind::FocusVisible },
    { "target", SimpleSelector::PseudoKind::Target },
    { "fullscreen", SimpleSelector::PseudoKind::Fullscreen },
    { "-webkit-full-screen", SimpleSelector::PseudoKind::Fullscreen },
    // The form controls' states (§14).
    { "checked", SimpleSelector::PseudoKind::Checked },
    { "indeterminate", SimpleSelector::PseudoKind::Indeterminate },
    { "default", SimpleSelector::PseudoKind::Default },
    { "disabled", SimpleSelector::PseudoKind::Disabled },
    { "enabled", SimpleSelector::PseudoKind::Enabled },
    { "required", SimpleSelector::PseudoKind::Required },
    { "optional", SimpleSelector::PseudoKind::Optional },
    { "read-only", SimpleSelector::PseudoKind::ReadOnly },
    { "read-write", SimpleSelector::PseudoKind::ReadWrite },
    { "placeholder-shown", SimpleSelector::PseudoKind::PlaceholderShown },
    { "valid", SimpleSelector::PseudoKind::Valid },
    { "invalid", SimpleSelector::PseudoKind::Invalid },
    { "in-range", SimpleSelector::PseudoKind::InRange },
    { "out-of-range", SimpleSelector::PseudoKind::OutOfRange },
    { "open", SimpleSelector::PseudoKind::Open },
    { "defined", SimpleSelector::PseudoKind::Defined },
    { "host", SimpleSelector::PseudoKind::Host },
    { "scope", SimpleSelector::PseudoKind::ScopeRoot },
    // Valid, and nothing here is ever in the state: a visited link (no
    // history is shown to a page), a field the browser filled, a modal
    // dialog or an open popover, a field the user has touched, a picture
    // in its own window, and Blink's vendor names, which a page writes in
    // a list beside others — an unknown one would drop the whole list.
    { "visited", SimpleSelector::PseudoKind::NeverMatches },
    { "autofill", SimpleSelector::PseudoKind::NeverMatches },
    { "-webkit-autofill", SimpleSelector::PseudoKind::NeverMatches },
    { "modal", SimpleSelector::PseudoKind::NeverMatches },
    { "popover-open", SimpleSelector::PseudoKind::NeverMatches },
    { "user-valid", SimpleSelector::PseudoKind::NeverMatches },
    { "user-invalid", SimpleSelector::PseudoKind::NeverMatches },
    { "picture-in-picture", SimpleSelector::PseudoKind::NeverMatches },
    { "-webkit-full-screen-document", SimpleSelector::PseudoKind::NeverMatches },
    { "-webkit-full-screen-ancestor", SimpleSelector::PseudoKind::NeverMatches },
    { "-webkit-full-page-media", SimpleSelector::PseudoKind::NeverMatches },
    { "-webkit-drag", SimpleSelector::PseudoKind::NeverMatches },
    { "window-inactive", SimpleSelector::PseudoKind::NeverMatches },
    { "horizontal", SimpleSelector::PseudoKind::NeverMatches },
    { "vertical", SimpleSelector::PseudoKind::NeverMatches },
    { "decrement", SimpleSelector::PseudoKind::NeverMatches },
    { "increment", SimpleSelector::PseudoKind::NeverMatches },
    { "start", SimpleSelector::PseudoKind::NeverMatches },
    { "end", SimpleSelector::PseudoKind::NeverMatches },
    { "double-button", SimpleSelector::PseudoKind::NeverMatches },
    { "single-button", SimpleSelector::PseudoKind::NeverMatches },
    { "no-button", SimpleSelector::PseudoKind::NeverMatches },
    { "corner-present", SimpleSelector::PseudoKind::NeverMatches },
};

constexpr std::string_view pseudo_elements[] = {
    "before", "after", "first-line", "first-letter", "marker", "selection",
    "placeholder", "backdrop", "file-selector-button", "cue", "grammar-error",
    "spelling-error", "target-text", "search-text", "details-content", "checkmark",
    "picker-icon", "scroll-marker", "scroll-marker-group", "column", "view-transition",
};

// Pseudo-elements that take an argument: valid, and matched by nothing
// here (shadow parts and slots, highlights, view transitions, pickers).
constexpr std::string_view functional_pseudo_elements[] = {
    "part", "slotted", "highlight", "cue", "picker", "scroll-button", "view-transition-group",
    "view-transition-image-pair", "view-transition-old", "view-transition-new",
};

std::optional<SelectorList> parse_selector_list_internal(
    std::vector<ComponentValue> const& values, bool forgiving);

std::optional<SelectorList> parse_relative_list(std::vector<ComponentValue> const& values);

// Whether any selector in the list carries a pseudo-element: not valid
// inside :is()/:where()/:not() or :has()'s relative list, all of which take
// selectors matched against an element, never a pseudo-element (selectors-4
// §4). Checked both ways: settle_pseudo_elements only lifts ::before,
// ::after and ::first-letter into .pseudo_element (the ones this engine
// matches); one it does not lift, such as ::first-line or ::marker, is
// still sitting among a compound's simples, unrecognized there without this.
bool has_pseudo_element(SelectorList const& list)
{
    for (ComplexSelector const& selector : list.selectors) {
        if (selector.pseudo_element != ComplexSelector::PseudoElement::None)
            return true;
        for (CompoundSelector const& compound : selector.compounds) {
            for (SimpleSelector const& simple : compound.simples) {
                if (simple.kind == SimpleSelector::Kind::PseudoElement)
                    return true;
            }
        }
    }
    return false;
}

// One compound selector; cursor sits at its first simple selector.
bool parse_compound(Cursor& cursor, CompoundSelector& compound, Specificity& specificity)
{
    bool first = true;
    while (!cursor.at_end()) {
        ComponentValue const* value = cursor.peek();
        // A combinator ends the compound whether or not whitespace comes first.
        if (value->is_token(Token::Type::Whitespace) || value->is_token(Token::Type::Comma) || is_delim(value, U'>')
            || is_delim(value, U'+') || is_delim(value, U'~'))
            break;

        // The nesting selector, anywhere in the compound. A type selector
        // may still follow it (`&div` is `div&`), so `first` stands.
        if (is_delim(value, U'&')) {
            cursor.consume();
            compound.simples.push_back(nesting_selector());
            if (t_nest_parent)
                specificity = specificity + t_nest_specificity;
            continue;
        }

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
                    // Blink takes every ::-webkit- name as a valid
                    // pseudo-element (its scrollbars, its controls' parts):
                    // a page writes them in lists beside others.
                    if (std::find(std::begin(pseudo_elements), std::end(pseudo_elements), name)
                            == std::end(pseudo_elements)
                        && !(element_form && name.starts_with("-webkit-") && !t_selectors_unforgiving))
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
                FunctionValue const& function = name_value->function();
                std::string const name = lowercased(function.name);
                if (element_form) {
                    // ::part(), ::slotted(), ::highlight() and the rest:
                    // valid, and matched by nothing here.
                    if (std::find(std::begin(functional_pseudo_elements), std::end(functional_pseudo_elements), name)
                        == std::end(functional_pseudo_elements))
                        return false;
                    bool has_argument = false;
                    for (ComponentValue const& part : function.values)
                        has_argument = has_argument || !part.is_token(Token::Type::Whitespace);
                    if (!has_argument)
                        return false; // an argument is required
                    SimpleSelector simple;
                    simple.kind = SimpleSelector::Kind::PseudoElement;
                    simple.name = name;
                    if (name == "slotted") {
                        // ::slotted(<compound-selector>): one compound, and
                        // its specificity counts with the pseudo-element's.
                        auto argument = parse_selector_list_internal(function.values, false);
                        if (!argument || argument->selectors.size() != 1 || !argument->selectors.front().combinators.empty()
                            || has_pseudo_element(*argument))
                            return false;
                        specificity = specificity + argument->selectors.front().specificity;
                        simple.argument = std::make_shared<SelectorList>(std::move(*argument));
                    } else if (name == "part") {
                        // ::part(<ident>+): the names, as written.
                        for (ComponentValue const& part : function.values) {
                            if (part.is_token(Token::Type::Ident))
                                simple.languages.push_back(part.token().value);
                            else if (!part.is_token(Token::Type::Whitespace))
                                return false;
                        }
                        if (simple.languages.empty())
                            return false;
                    }
                    specificity.c += 1;
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                SimpleSelector simple;
                simple.kind = SimpleSelector::Kind::PseudoClass;
                simple.name = name;
                if (name == "host" || name == "host-context") {
                    // :host(<compound>) and :host-context(<compound>)
                    // (css-scoping-1 §3.2): one compound, tested on the host.
                    auto argument = parse_selector_list_internal(function.values, false);
                    if (!argument || argument->selectors.size() != 1 || !argument->selectors.front().combinators.empty()
                        || has_pseudo_element(*argument))
                        return false;
                    simple.pseudo = name == "host" ? SimpleSelector::PseudoKind::Host : SimpleSelector::PseudoKind::HostContext;
                    specificity.b += 1;
                    specificity = specificity + argument->selectors.front().specificity;
                    simple.argument = std::make_shared<SelectorList>(std::move(*argument));
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                if (name == "lang" || name == "dir") {
                    // :lang(<ranges>), :dir(ltr | rtl) (selectors-4 §7).
                    for (ComponentValue const& part : function.values) {
                        if (part.is_token(Token::Type::Ident) || part.is_token(Token::Type::String))
                            simple.languages.push_back(lowercased(part.token().value));
                        else if (!part.is_token(Token::Type::Whitespace) && !part.is_token(Token::Type::Comma))
                            return false;
                    }
                    if (simple.languages.empty() || (name == "dir" && simple.languages.size() != 1))
                        return false;
                    simple.pseudo = name == "lang" ? SimpleSelector::PseudoKind::Lang : SimpleSelector::PseudoKind::Dir;
                    specificity.b += 1;
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                if (name == "-webkit-any") {
                    // Blink's older :is(), forgiving nothing.
                    auto argument = parse_selector_list_internal(function.values, false);
                    if (!argument || has_pseudo_element(*argument))
                        return false;
                    simple.pseudo = SimpleSelector::PseudoKind::Is;
                    specificity.b += 1;
                    simple.argument = std::make_shared<SelectorList>(std::move(*argument));
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                if (name == "state") {
                    // One name, a <custom-ident>, and nothing else.
                    int names = 0;
                    for (ComponentValue const& part : function.values) {
                        if (part.is_token(Token::Type::Ident))
                            ++names;
                        else if (!part.is_token(Token::Type::Whitespace))
                            return false;
                    }
                    if (names != 1)
                        return false;
                    // A custom element's own state (CustomStateSet): none set here.
                    simple.pseudo = SimpleSelector::PseudoKind::NeverMatches;
                    specificity.b += 1;
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                if (name == "not" || name == "is" || name == "where") {
                    bool const forgiving = name != "not" && !t_selectors_unforgiving;
                    auto argument = parse_selector_list_internal(function.values, forgiving);
                    // A pseudo-element cannot be argued about: :is()/:where()
                    // /:not() take complex selectors, none of which may end
                    // in one (selectors-4 §4).
                    if (!argument || has_pseudo_element(*argument))
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
                if (name == "has") {
                    // Nothing argues about a pseudo-element's inside.
                    for (SimpleSelector const& before : compound.simples) {
                        if (before.kind == SimpleSelector::Kind::PseudoElement)
                            return false;
                    }
                    // :has() may not occur inside :has() — directly, or
                    // through :is()/:where() in between — so having its own
                    // effects be part of what it considers is never a
                    // question (selectors-4 §4.2).
                    if (t_inside_has)
                        return false;
                    // A relative selector list: each selector may open with
                    // a combinator, and what it is relative to is the
                    // element being tested. Written here as an ordinary
                    // selector with :scope in front, so that the
                    // right-to-left walk ends on that element and nowhere
                    // else — and the leading combinator says where to look
                    // for candidates.
                    t_inside_has = true;
                    std::optional<SelectorList> relative = parse_relative_list(function.values);
                    t_inside_has = false;
                    if (!relative || has_pseudo_element(*relative))
                        return false;
                    simple.pseudo = SimpleSelector::PseudoKind::Has;
                    Specificity best;
                    for (ComplexSelector const& inner : relative->selectors)
                        best = std::max(best, inner.specificity);
                    specificity = specificity + best;
                    simple.argument = std::make_unique<SelectorList>(std::move(*relative));
                    compound.simples.push_back(std::move(simple));
                    continue;
                }
                if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type"
                    || name == "nth-last-of-type") {
                    Cursor argument { function.values, 0 };
                    if (!parse_an_plus_b(argument, simple.nth_a, simple.nth_b))
                        return false;
                    argument.skip_whitespace();
                    specificity.b += 1;
                    bool const of_type
                        = name == "nth-of-type" || name == "nth-last-of-type";
                    if (!argument.at_end()) {
                        // :nth-child(An+B of S) counts only the siblings
                        // that match S, and the element has to be one of
                        // them. The two of-type forms take no such list.
                        ComponentValue const* const word = argument.consume();
                        if (of_type || !word || !word->is_token(Token::Type::Ident)
                            || lowercased(word->token().value) != "of")
                            return false;
                        argument.skip_whitespace();
                        if (argument.at_end())
                            return false; // `of` with no selector after it
                        std::vector<ComponentValue> const rest(
                            function.values.begin() + static_cast<std::ptrdiff_t>(argument.index),
                            function.values.end());
                        std::optional<SelectorList> list = parse_selector_list_internal(rest, false);
                        if (!list)
                            return false;
                        Specificity best;
                        for (ComplexSelector const& inner : list->selectors)
                            best = std::max(best, inner.specificity);
                        specificity = specificity + best;
                        simple.argument = std::make_unique<SelectorList>(std::move(*list));
                    }
                    simple.pseudo = name == "nth-child" ? SimpleSelector::PseudoKind::NthChild
                        : name == "nth-last-child"      ? SimpleSelector::PseudoKind::NthLastChild
                        : name == "nth-of-type"         ? SimpleSelector::PseudoKind::NthOfType
                                                        : SimpleSelector::PseudoKind::NthLastOfType;
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
        else if (last[i].name == "placeholder" || last[i].name == "-webkit-input-placeholder" || last[i].name == "-moz-placeholder")
            selector.pseudo_element = ComplexSelector::PseudoElement::Placeholder;
        else if (last[i].name == "slotted" && last[i].argument && i + 1 == last.size()) {
            selector.pseudo_element = ComplexSelector::PseudoElement::Slotted;
            selector.slotted = last[i].argument;
        } else if (last[i].name == "part" && !last[i].languages.empty()) {
            // What follows ::part() are the states of the part itself.
            bool states_only = true;
            for (std::size_t j = i + 1; j < last.size(); ++j)
                states_only = states_only && last[j].kind == SimpleSelector::Kind::PseudoClass;
            if (!states_only)
                break;
            selector.pseudo_element = ComplexSelector::PseudoElement::Part;
            selector.parts = last[i].languages;
            selector.part_states.assign(last.begin() + static_cast<std::ptrdiff_t>(i) + 1, last.end());
            last.erase(last.begin() + static_cast<std::ptrdiff_t>(i), last.end());
            break;
        } else
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

// The relative selector list :has() takes (selectors-4 §4.2). Each selector
// in it may open with a combinator, and what it is relative to is the
// element being tested. Each is turned into an ordinary selector with a
// :scope compound in front, so the right-to-left walk has somewhere to end
// and ends on that element alone; a selector written with no combinator
// takes the descendant one, as `:has(p)` means a p somewhere inside.
std::optional<SelectorList> parse_relative_list(std::vector<ComponentValue> const& values)
{
    SelectorList list;
    std::size_t start = 0;
    auto const one = [&](std::size_t from, std::size_t to) {
        Cursor cursor { values, from };
        while (cursor.index < to && cursor.peek()->is_token(Token::Type::Whitespace))
            ++cursor.index;
        Combinator lead = Combinator::Descendant;
        ComponentValue const* const first = cursor.index < to ? cursor.peek() : nullptr;
        if (is_delim(first, U'>'))
            lead = Combinator::Child;
        else if (is_delim(first, U'+'))
            lead = Combinator::NextSibling;
        else if (is_delim(first, U'~'))
            lead = Combinator::SubsequentSibling;
        if (lead != Combinator::Descendant)
            ++cursor.index;
        std::vector<ComponentValue> const rest(values.begin() + static_cast<std::ptrdiff_t>(cursor.index),
            values.begin() + static_cast<std::ptrdiff_t>(to));
        std::optional<SelectorList> parsed = parse_selector_list_internal(rest, false);
        if (!parsed || parsed->selectors.size() != 1)
            return false;
        ComplexSelector selector = std::move(parsed->selectors.front());
        if (selector.pseudo_element != ComplexSelector::PseudoElement::None)
            return false; // a pseudo-element has nothing to be relative to
        SimpleSelector scope;
        scope.kind = SimpleSelector::Kind::PseudoClass;
        scope.pseudo = SimpleSelector::PseudoKind::Scope;
        scope.name = "scope";
        CompoundSelector anchor;
        anchor.simples.push_back(std::move(scope));
        selector.compounds.insert(selector.compounds.begin(), std::move(anchor));
        selector.combinators.insert(selector.combinators.begin(), lead);
        list.selectors.push_back(std::move(selector));
        return true;
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!values[i].is_token(Token::Type::Comma))
            continue;
        if (!one(start, i))
            return std::nullopt;
        start = i + 1;
    }
    if (!one(start, values.size()))
        return std::nullopt;
    return list;
}


// --- Matching -----------------------------------------------------------------

// The shadow root whose sheets the selectors being matched come from, and
// whether its host is being tested as an ordinary element — which it is
// only for the argument of :host() and :host-context().
thread_local dom::Node const* t_shadow_scope = nullptr;
thread_local bool t_host_as_itself = false;

// Whether the element is the host of the scope being matched in: an
// element nothing but :host, :host() and :host-context() matches
// (css-scoping-1 §3.2.1, "featureless").
bool is_scope_host(dom::Element const& element)
{
    return t_shadow_scope != nullptr && !t_host_as_itself && element.shadow_root() == t_shadow_scope;
}

// The element above this one, as a combinator sees it: its parent, and
// for a topmost element of the shadow tree being matched in, the host.
dom::Element const* parent_element(dom::Element const& element)
{
    dom::Node const* parent = element.parent();
    if (parent && parent->is_element())
        return static_cast<dom::Element const*>(parent);
    if (parent && parent == t_shadow_scope)
        return &static_cast<dom::ShadowRoot const*>(parent)->host();
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

// What a relative selector inside :has() is relative to, while that :has()
// is being tested. A nested one saves and restores it, so the inner list is
// relative to the inner element and the outer list gets its own back.
thread_local dom::Element const* relative_to = nullptr;

// Every element inside this one, and whether any of them answers.
template <typename Test> bool any_inside(dom::Element const& element, Test const& test)
{
    for (dom::Node const* child : element.children()) {
        if (!child->is_element())
            continue;
        auto const& candidate = static_cast<dom::Element const&>(*child);
        if (test(candidate) || any_inside(candidate, test))
            return true;
    }
    return false;
}

// Every element after this one among its siblings, each with everything
// inside it: `:has(+ div p)` is answered by a p inside the next sibling.
template <typename Test> bool any_after(dom::Element const& element, Test const& test)
{
    dom::Node const* const parent = element.parent();
    if (!parent)
        return false;
    bool past = false;
    for (dom::Node const* node : parent->children()) {
        if (node == &element) {
            past = true;
            continue;
        }
        if (!past || !node->is_element())
            continue;
        auto const& candidate = static_cast<dom::Element const&>(*node);
        if (test(candidate) || any_inside(candidate, test))
            return true;
    }
    return false;
}
bool same_type(dom::Element const& a, dom::Element const& b)
{
    return a.namespace_uri() == b.namespace_uri() && a.local_name() == b.local_name();
}

// 1-based position among element siblings; of_type restricts to same-type,
// and `of` (from :nth-child(An+B of S)) counts only the siblings matching a
// selector list. Whether the element is one of them is the caller's question:
// a position of zero is not an answer, since an+b can name it.
int sibling_index(dom::Element const& element, bool from_end, bool of_type, SelectorList const* of)
{
    dom::Node const* parent = element.parent();
    if (!parent)
        return 1;
    int index = 0;
    auto const& siblings = parent->children();
    auto const counts = [&](dom::Element const& sibling) {
        if (of_type && !same_type(sibling, element))
            return false;
        return !of || matches(*of, sibling);
    };
    if (!from_end) {
        for (dom::Node const* node : siblings) {
            if (!node->is_element())
                continue;
            auto const* sibling = static_cast<dom::Element const*>(node);
            if (!counts(*sibling))
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
            if (!counts(*sibling))
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

// --- The form controls' states (HTML §4.16.3, selectors-4 §14) -------------

std::string attribute_lower(dom::Element const& element, std::string_view name)
{
    dom::Attr const* attribute = element.find_attribute(name);
    return attribute ? lowercased(attribute->value) : std::string();
}

// An <input>'s type keyword: "text" when it is missing or unknown.
std::string input_type(dom::Element const& element)
{
    static constexpr std::string_view known[] = { "hidden", "text", "search", "tel", "url", "email", "password",
        "date", "month", "week", "time", "datetime-local", "number", "range", "color", "checkbox", "radio", "file",
        "submit", "image", "reset", "button" };
    std::string const type = attribute_lower(element, "type");
    for (std::string_view const keyword : known) {
        if (type == keyword)
            return type;
    }
    return "text";
}

// The types whose field takes text, and so readonly (§4.10.5.3.3).
bool takes_readonly(std::string const& type)
{
    return type == "text" || type == "search" || type == "url" || type == "tel" || type == "email" || type == "password"
        || type == "date" || type == "month" || type == "week" || type == "time" || type == "datetime-local" || type == "number";
}

bool takes_placeholder(std::string const& type)
{
    return type == "text" || type == "search" || type == "url" || type == "tel" || type == "email" || type == "password"
        || type == "number";
}

bool takes_required(std::string const& type)
{
    return type != "hidden" && type != "range" && type != "color" && type != "submit" && type != "image" && type != "reset"
        && type != "button";
}

dom::Element const* element_parent(dom::Element const& element)
{
    dom::Node const* parent = element.parent();
    return parent && parent->is_element() ? static_cast<dom::Element const*>(parent) : nullptr;
}

// A fieldset that is disabled disables what it holds, but what is in its
// first legend (§4.10.15).
bool disabled_by_fieldset(dom::Element const& element)
{
    dom::Node const* child = &element;
    for (dom::Element const* ancestor = element_parent(element); ancestor; child = ancestor, ancestor = element_parent(*ancestor)) {
        if (!ancestor->is_html("fieldset") || !ancestor->has_attribute("disabled"))
            continue;
        dom::Element const* first_legend = nullptr;
        for (dom::Node const* node : ancestor->children()) {
            if (node->is_element() && static_cast<dom::Element const*>(node)->is_html("legend")) {
                first_legend = static_cast<dom::Element const*>(node);
                break;
            }
        }
        if (child != first_legend)
            return true;
    }
    return false;
}

bool can_be_disabled(dom::Element const& element)
{
    return element.is_html("button") || element.is_html("input") || element.is_html("select") || element.is_html("textarea")
        || element.is_html("optgroup") || element.is_html("option") || element.is_html("fieldset");
}

bool actually_disabled(dom::Element const& element)
{
    if (element.is_html("optgroup"))
        return element.has_attribute("disabled");
    if (element.is_html("option")) {
        dom::Element const* parent = element_parent(element);
        return element.has_attribute("disabled") || (parent && parent->is_html("optgroup") && parent->has_attribute("disabled"));
    }
    return element.has_attribute("disabled") || disabled_by_fieldset(element);
}

// The select an option belongs to, through an optgroup.
dom::Element const* select_of(dom::Element const& option)
{
    dom::Element const* parent = element_parent(option);
    if (parent && parent->is_html("optgroup"))
        parent = element_parent(*parent);
    return parent && parent->is_html("select") ? parent : nullptr;
}

template<typename Each> void each_option(dom::Element const& select, Each const& each)
{
    for (dom::Node const* node : select.children()) {
        if (!node->is_element())
            continue;
        auto const& child = static_cast<dom::Element const&>(*node);
        if (child.is_html("option")) {
            if (!each(child))
                return;
        } else if (child.is_html("optgroup")) {
            for (dom::Node const* inner : child.children()) {
                if (inner->is_element() && static_cast<dom::Element const*>(inner)->is_html("option")
                    && !each(static_cast<dom::Element const&>(*inner)))
                    return;
            }
        }
    }
}

// An option's selectedness: its selected attribute, or — in a select that
// shows one option and has none selected — being the first that is not
// disabled (§4.10.7's selectedness setting algorithm).
bool option_selected(dom::Element const& option)
{
    if (option.has_attribute("selected"))
        return true;
    dom::Element const* select = select_of(option);
    if (!select || select->has_attribute("multiple"))
        return false;
    bool any_selected = false;
    dom::Element const* first = nullptr;
    each_option(*select, [&](dom::Element const& candidate) {
        if (candidate.has_attribute("selected"))
            any_selected = true;
        if (!first && !actually_disabled(candidate))
            first = &candidate;
        return true;
    });
    return !any_selected && first == &option;
}

bool checkedness(dom::Element const& element)
{
    if (element.is_html("option"))
        return option_selected(element);
    if (!element.is_html("input"))
        return false;
    std::string const type = input_type(element);
    if (type != "checkbox" && type != "radio")
        return false;
    if (element.document().live_checked) {
        if (std::optional<bool> const live = element.document().live_checked(element))
            return *live;
    }
    return element.has_attribute("checked");
}

std::string control_value(dom::Element const& element)
{
    if (element.document().live_value) {
        if (std::optional<std::string> live = element.document().live_value(element))
            return *live;
    }
    if (element.is_html("textarea")) {
        std::string text;
        for (dom::Node const* child : element.children()) {
            if (child->is_text())
                text += static_cast<dom::Text const*>(child)->data;
        }
        return text;
    }
    if (element.is_html("select")) {
        std::string value;
        each_option(element, [&](dom::Element const& option) {
            if (!option_selected(option))
                return true;
            dom::Attr const* attribute = option.find_attribute("value");
            if (attribute) {
                value = attribute->value;
            } else {
                for (dom::Node const* child : option.children()) {
                    if (child->is_text())
                        value += static_cast<dom::Text const*>(child)->data;
                }
            }
            return false;
        });
        return value;
    }
    dom::Attr const* attribute = element.find_attribute("value");
    return attribute ? attribute->value : std::string();
}

// The radios of one group: the same name, the same form (§4.10.5.1.18).
template<typename Each> void each_in_radio_group(dom::Element const& radio, Each const& each)
{
    dom::Attr const* name = radio.find_attribute("name");
    if (!name || name->value.empty()) {
        each(radio);
        return;
    }
    dom::Node const* root = &radio;
    for (dom::Element const* ancestor = element_parent(radio); ancestor; ancestor = element_parent(*ancestor)) {
        root = ancestor;
        if (ancestor->is_html("form"))
            break;
    }
    auto const walk = [&](auto const& self, dom::Node const& node) -> void {
        for (dom::Node const* child : node.children()) {
            if (!child->is_element())
                continue;
            auto const& candidate = static_cast<dom::Element const&>(*child);
            if (candidate.is_html("input") && input_type(candidate) == "radio") {
                dom::Attr const* other = candidate.find_attribute("name");
                if (other && other->value == name->value)
                    each(candidate);
            }
            self(self, candidate);
        }
    };
    walk(walk, *root);
}

bool editing_host_editable(dom::Element const& element)
{
    for (dom::Element const* ancestor = &element; ancestor; ancestor = element_parent(*ancestor)) {
        dom::Attr const* attribute = ancestor->find_attribute("contenteditable");
        if (!attribute)
            continue;
        std::string const value = lowercased(attribute->value);
        if (value.empty() || value == "true" || value == "plaintext-only")
            return true;
        if (value == "false")
            return false;
    }
    return false;
}

bool read_write(dom::Element const& element)
{
    if (element.is_html("input"))
        return takes_readonly(input_type(element)) && !element.has_attribute("readonly") && !actually_disabled(element);
    if (element.is_html("textarea"))
        return !element.has_attribute("readonly") && !actually_disabled(element);
    return editing_host_editable(element);
}

bool parse_number(std::string_view text, double& out)
{
    std::string const copy(text);
    if (copy.empty())
        return false;
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

// Whether a control falls outside its min and max (§4.10.5.3.7): number
// fields; a range field is held within them and never does.
std::optional<bool> out_of_range(dom::Element const& element)
{
    if (!element.is_html("input"))
        return std::nullopt;
    std::string const type = input_type(element);
    if (type != "number" && type != "range")
        return std::nullopt;
    dom::Attr const* min = element.find_attribute("min");
    dom::Attr const* max = element.find_attribute("max");
    if (!min && !max)
        return std::nullopt;
    if (type == "range")
        return false;
    double value = 0;
    if (!parse_number(control_value(element), value))
        return false;
    double limit = 0;
    if (min && parse_number(min->value, limit) && value < limit)
        return true;
    if (max && parse_number(max->value, limit) && value > limit)
        return true;
    return false;
}

// A control that takes part in constraint validation (§4.10.20.1): not
// disabled, not read-only, not a button that is no submit button.
bool validation_candidate(dom::Element const& element)
{
    if (element.is_html("input")) {
        std::string const type = input_type(element);
        if (type == "hidden" || type == "reset" || type == "button")
            return false;
        if (takes_readonly(type) && element.has_attribute("readonly"))
            return false;
        return !actually_disabled(element);
    }
    if (element.is_html("textarea"))
        return !element.has_attribute("readonly") && !actually_disabled(element);
    if (element.is_html("select"))
        return !actually_disabled(element);
    if (element.is_html("button"))
        return attribute_lower(element, "type") != "button" && attribute_lower(element, "type") != "reset" && !actually_disabled(element);
    return false;
}

bool plausible_email(std::string const& value)
{
    // §4.10.5.1.5's valid e-mail address, near enough: one @, text on
    // both sides, no spaces; a list when `multiple` is not here.
    std::size_t const at = value.find('@');
    return at != std::string::npos && at > 0 && at + 1 < value.size() && value.find('@', at + 1) == std::string::npos
        && value.find_first_of(" \t\n\r\f") == std::string::npos;
}

bool suffering(dom::Element const& element)
{
    if (!validation_candidate(element))
        return false;
    bool const required = element.has_attribute("required");
    if (element.is_html("input")) {
        std::string const type = input_type(element);
        if (type == "checkbox")
            return required && !checkedness(element);
        if (type == "radio") {
            bool group_required = false;
            bool group_checked = false;
            each_in_radio_group(element, [&](dom::Element const& radio) {
                group_required = group_required || radio.has_attribute("required");
                group_checked = group_checked || checkedness(radio);
            });
            return group_required && !group_checked;
        }
        std::string const value = control_value(element);
        if (required && takes_required(type) && value.empty())
            return true;
        if (!value.empty() && type == "email" && !plausible_email(value))
            return true;
        if (!value.empty() && type == "url" && value.find(':') == std::string::npos)
            return true;
        if (!value.empty() && type == "number") {
            double number = 0;
            if (!parse_number(value, number))
                return true;
        }
        return out_of_range(element).value_or(false);
    }
    if (element.is_html("textarea") || element.is_html("select"))
        return required && control_value(element).empty();
    return false;
}

// A form or fieldset is invalid when a control in it is.
bool holds_suffering(dom::Element const& element)
{
    for (dom::Node const* child : element.children()) {
        if (!child->is_element())
            continue;
        auto const& inner = static_cast<dom::Element const&>(*child);
        if (suffering(inner) || holds_suffering(inner))
            return true;
    }
    return false;
}

std::optional<bool> validity(dom::Element const& element)
{
    if (element.is_html("form") || element.is_html("fieldset"))
        return !holds_suffering(element);
    if (!validation_candidate(element))
        return std::nullopt;
    return !suffering(element);
}

// The form's default button (§4.10.22.4): its first submit button.
bool default_button(dom::Element const& element)
{
    bool const submit = (element.is_html("button") && (attribute_lower(element, "type").empty() || attribute_lower(element, "type") == "submit"))
        || (element.is_html("input") && (input_type(element) == "submit" || input_type(element) == "image"));
    if (!submit)
        return false;
    dom::Element const* form = element_parent(element);
    while (form && !form->is_html("form"))
        form = element_parent(*form);
    if (!form)
        return false;
    dom::Element const* first = nullptr;
    auto const walk = [&](auto const& self, dom::Node const& node) -> void {
        for (dom::Node const* child : node.children()) {
            if (first || !child->is_element())
                continue;
            auto const& candidate = static_cast<dom::Element const&>(*child);
            bool const candidate_submit = (candidate.is_html("button") && (attribute_lower(candidate, "type").empty() || attribute_lower(candidate, "type") == "submit"))
                || (candidate.is_html("input") && (input_type(candidate) == "submit" || input_type(candidate) == "image"));
            if (candidate_submit) {
                first = &candidate;
                return;
            }
            self(self, candidate);
        }
    };
    walk(walk, *form);
    return first == &element;
}

// The element's language (HTML §3.2.6.2): its own lang, or the nearest
// one above it; empty when none says.
std::string language_of(dom::Element const& element)
{
    for (dom::Element const* ancestor = &element; ancestor; ancestor = element_parent(*ancestor)) {
        for (dom::Attr const& attribute : ancestor->attributes()) {
            if ((attribute.local_name == "lang" && attribute.namespace_uri.empty())
                || (attribute.local_name == "lang" && attribute.namespace_uri == "http://www.w3.org/XML/1998/namespace"))
                return lowercased(attribute.value);
        }
    }
    return {};
}

bool language_matches(std::string const& language, std::string const& range)
{
    if (range == "*")
        return !language.empty();
    if (language == range)
        return true;
    return language.size() > range.size() && language.starts_with(range) && language[range.size()] == '-';
}

std::string direction_of(dom::Element const& element)
{
    for (dom::Element const* ancestor = &element; ancestor; ancestor = element_parent(*ancestor)) {
        std::string const dir = attribute_lower(*ancestor, "dir");
        if (dir == "ltr" || dir == "rtl")
            return dir;
    }
    return "ltr";
}

bool is_custom_element_name(std::string const& name)
{
    return !name.empty() && name[0] >= 'a' && name[0] <= 'z' && name.find('-') != std::string::npos;
}

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
    case SimpleSelector::PseudoKind::Hover:
        return element.document().holds_hover(element);
    case SimpleSelector::PseudoKind::Active:
        return element.document().holds_active(element);
    case SimpleSelector::PseudoKind::Focus:
        return element.document().focused() == &element;
    case SimpleSelector::PseudoKind::FocusWithin:
        return element.document().holds_focus(element);
    case SimpleSelector::PseudoKind::FocusVisible:
        // As Blink and Gecko decide it: a focus that came from the keyboard,
        // or any focus of a field that takes typing, shows itself.
        return element.document().focused() == &element
            && (element.document().focus_visible() || read_write(element));
    case SimpleSelector::PseudoKind::Target:
        return element.document().target() == &element;
    case SimpleSelector::PseudoKind::Fullscreen:
        return element.document().fullscreen() == &element;
    case SimpleSelector::PseudoKind::Checked:
        return checkedness(element);
    case SimpleSelector::PseudoKind::Indeterminate:
        if (element.is_html("progress"))
            return !element.has_attribute("value");
        if (element.is_html("input") && input_type(element) == "radio") {
            bool any = false;
            each_in_radio_group(element, [&](dom::Element const& radio) { any = any || checkedness(radio); });
            return !any;
        }
        return false;
    case SimpleSelector::PseudoKind::Default:
        if (element.is_html("input") && (input_type(element) == "checkbox" || input_type(element) == "radio"))
            return element.has_attribute("checked");
        if (element.is_html("option"))
            return element.has_attribute("selected");
        return default_button(element);
    case SimpleSelector::PseudoKind::Disabled:
        return can_be_disabled(element) && actually_disabled(element);
    case SimpleSelector::PseudoKind::Enabled:
        return can_be_disabled(element) && !actually_disabled(element);
    case SimpleSelector::PseudoKind::Required:
    case SimpleSelector::PseudoKind::Optional: {
        bool applies = element.is_html("select") || element.is_html("textarea");
        if (element.is_html("input"))
            applies = takes_required(input_type(element));
        if (!applies)
            return false;
        bool const required = element.has_attribute("required");
        return simple.pseudo == SimpleSelector::PseudoKind::Required ? required : !required;
    }
    case SimpleSelector::PseudoKind::ReadOnly:
        return !read_write(element);
    case SimpleSelector::PseudoKind::ReadWrite:
        return read_write(element);
    case SimpleSelector::PseudoKind::PlaceholderShown: {
        bool const takes = element.is_html("textarea") || (element.is_html("input") && takes_placeholder(input_type(element)));
        return takes && element.has_attribute("placeholder") && control_value(element).empty();
    }
    case SimpleSelector::PseudoKind::Valid:
    case SimpleSelector::PseudoKind::Invalid: {
        std::optional<bool> const valid = validity(element);
        if (!valid)
            return false;
        return simple.pseudo == SimpleSelector::PseudoKind::Valid ? *valid : !*valid;
    }
    case SimpleSelector::PseudoKind::InRange:
    case SimpleSelector::PseudoKind::OutOfRange: {
        std::optional<bool> const outside = out_of_range(element);
        if (!outside)
            return false;
        return simple.pseudo == SimpleSelector::PseudoKind::OutOfRange ? *outside : !*outside;
    }
    case SimpleSelector::PseudoKind::Open:
        return (element.is_html("details") || element.is_html("dialog")) && element.has_attribute("open");
    case SimpleSelector::PseudoKind::Defined:
        return !element.is_html() || !is_custom_element_name(element.local_name()) || element.custom_defined();
    case SimpleSelector::PseudoKind::Lang: {
        std::string const language = language_of(element);
        for (std::string const& range : simple.languages) {
            if (language_matches(language, range))
                return true;
        }
        return false;
    }
    case SimpleSelector::PseudoKind::Dir:
        return direction_of(element) == simple.languages.front();
    case SimpleSelector::PseudoKind::Root:
        return element.parent() != nullptr && element.parent()->type() == dom::NodeType::Document;
    case SimpleSelector::PseudoKind::Host:
    case SimpleSelector::PseudoKind::HostContext: {
        // The host of the tree these rules are in, and for the functional
        // forms a compound it — or, for :host-context(), it or something
        // above it, through any shadow root on the way — matches as itself.
        if (t_shadow_scope == nullptr || element.shadow_root() != t_shadow_scope)
            return false;
        if (!simple.argument || simple.argument->selectors.empty())
            return true;
        CompoundSelector const& wanted = simple.argument->selectors.front().compounds.back();
        bool const outer = t_host_as_itself;
        t_host_as_itself = true;
        bool found = false;
        if (simple.pseudo == SimpleSelector::PseudoKind::Host) {
            found = matches_compound(wanted, element);
        } else {
            for (dom::Node const* at = &element; at != nullptr && !found; at = at->parent_or_host())
                found = at->is_element() && matches_compound(wanted, static_cast<dom::Element const&>(*at));
        }
        t_host_as_itself = outer;
        return found;
    }
    case SimpleSelector::PseudoKind::Empty:
        for (dom::Node const* child : element.children()) {
            if (child->is_element())
                return false;
            if (child->is_text() && !static_cast<dom::Text const*>(child)->data.empty())
                return false;
        }
        return true;
    case SimpleSelector::PseudoKind::FirstChild:
        return sibling_index(element, false, false, nullptr) == 1;
    case SimpleSelector::PseudoKind::LastChild:
        return sibling_index(element, true, false, nullptr) == 1;
    case SimpleSelector::PseudoKind::OnlyChild:
        return sibling_index(element, false, false, nullptr) == 1 && sibling_index(element, true, false, nullptr) == 1;
    case SimpleSelector::PseudoKind::FirstOfType:
        return sibling_index(element, false, true, nullptr) == 1;
    case SimpleSelector::PseudoKind::LastOfType:
        return sibling_index(element, true, true, nullptr) == 1;
    case SimpleSelector::PseudoKind::OnlyOfType:
        return sibling_index(element, false, true, nullptr) == 1 && sibling_index(element, true, true, nullptr) == 1;
    case SimpleSelector::PseudoKind::NthChild:
    case SimpleSelector::PseudoKind::NthLastChild: {
        // With `of S`, only the siblings matching S are counted, and an
        // element that does not match one of them is nowhere in that count.
        if (simple.argument && !matches(*simple.argument, element))
            return false;
        bool const from_end = simple.pseudo == SimpleSelector::PseudoKind::NthLastChild;
        return an_plus_b_matches(simple.nth_a, simple.nth_b,
            sibling_index(element, from_end, false, simple.argument.get()));
    }
    case SimpleSelector::PseudoKind::NthOfType:
        return an_plus_b_matches(simple.nth_a, simple.nth_b, sibling_index(element, false, true, nullptr));
    case SimpleSelector::PseudoKind::NthLastOfType:
        return an_plus_b_matches(simple.nth_a, simple.nth_b, sibling_index(element, true, true, nullptr));
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
    case SimpleSelector::PseudoKind::Scope:
        return relative_to == &element;
    case SimpleSelector::PseudoKind::ScopeRoot:
        // selectors-4 §14.3: the scoping root, which with no @scope
        // around the rule is the document's root element.
        if (t_scope_root)
            return t_scope_root == &element;
        return element.parent() != nullptr && element.parent()->type() == dom::NodeType::Document;
    case SimpleSelector::PseudoKind::Has: {
        // A :has() inside another's argument cannot be written, but `&` can
        // bring one there from a parent rule; it matches nothing (csswg
        // issue 9600, as Gecko and Blink do).
        if (!simple.argument || relative_to)
            return false;
        // Each selector in the list already carries the :scope compound and
        // the leading combinator, so a candidate answers it only by walking
        // back to this element. The combinator says where to look: inside
        // for a descendant or a child, after it for a sibling.
        dom::Element const* const outer = relative_to;
        relative_to = &element;
        bool found = false;
        for (ComplexSelector const& inner : simple.argument->selectors) {
            if (inner.combinators.empty())
                continue;
            auto const test = [&](dom::Element const& candidate) { return matches(inner, candidate); };
            bool const sideways = inner.combinators.front() == Combinator::NextSibling
                || inner.combinators.front() == Combinator::SubsequentSibling;
            found = sideways ? any_after(element, test) : any_inside(element, test);
            if (found)
                break;
        }
        relative_to = outer;
        return found;
    }
    }
    return false;
}

bool matches_compound(CompoundSelector const& compound, dom::Element const& element)
{
    // The host of the scope is matched by its own pseudo-classes alone.
    bool const featureless = is_scope_host(element);
    bool named_host = false;
    for (SimpleSelector const& simple : compound.simples) {
        bool const host_class = simple.pseudo == SimpleSelector::PseudoKind::Host || simple.pseudo == SimpleSelector::PseudoKind::HostContext;
        if (featureless && !host_class)
            return false;
        named_host = named_host || host_class;
        if (!matches_simple(simple, element))
            return false;
    }
    return !featureless || named_host;
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

std::optional<SelectorList> parse_nested_selector_list(
    std::vector<ComponentValue> const& prelude, SelectorList const& parent)
{
    // What `&` stands for: the parent's selectors that address an element.
    auto stands_for = std::make_shared<SelectorList>();
    Specificity weight;
    for (ComplexSelector const& selector : parent.selectors) {
        if (selector.pseudo_element != ComplexSelector::PseudoElement::None)
            continue;
        stands_for->selectors.push_back(selector);
        weight = std::max(weight, selector.specificity);
    }
    struct Restore {
        std::shared_ptr<SelectorList const> parent = t_nest_parent;
        Specificity specificity = t_nest_specificity;
        ~Restore()
        {
            t_nest_parent = std::move(parent);
            t_nest_specificity = specificity;
        }
    } const restore;
    t_nest_parent = stands_for;
    t_nest_specificity = weight;

    SelectorList list;
    auto const one = [&](std::size_t from, std::size_t to) {
        while (from < to && prelude[from].is_token(Token::Type::Whitespace))
            ++from;
        std::optional<Combinator> lead;
        ComponentValue const* const first = from < to ? &prelude[from] : nullptr;
        if (is_delim(first, U'>'))
            lead = Combinator::Child;
        else if (is_delim(first, U'+'))
            lead = Combinator::NextSibling;
        else if (is_delim(first, U'~'))
            lead = Combinator::SubsequentSibling;
        if (lead)
            ++from;
        std::vector<ComponentValue> const rest(prelude.begin() + static_cast<std::ptrdiff_t>(from),
            prelude.begin() + static_cast<std::ptrdiff_t>(to));
        std::optional<SelectorList> parsed = parse_selector_list_internal(rest, false);
        if (!parsed || parsed->selectors.size() != 1)
            return false;
        ComplexSelector selector = std::move(parsed->selectors.front());
        // §2: a selector that opens with a combinator, or that writes no
        // `&`, is relative to the parent: `& ` (or the combinator) in front.
        // Written anywhere counts, even in a piece a forgiving :is() drops.
        if (lead || !writes_nesting(rest)) {
            CompoundSelector anchor;
            anchor.simples.push_back(nesting_selector());
            selector.compounds.insert(selector.compounds.begin(), std::move(anchor));
            selector.combinators.insert(selector.combinators.begin(), lead.value_or(Combinator::Descendant));
            selector.specificity = selector.specificity + weight;
        }
        list.selectors.push_back(std::move(selector));
        return true;
    };
    std::size_t start = 0;
    for (std::size_t i = 0; i < prelude.size(); ++i) {
        if (!prelude[i].is_token(Token::Type::Comma))
            continue;
        if (!one(start, i))
            return std::nullopt;
        start = i + 1;
    }
    if (!one(start, prelude.size()))
        return std::nullopt;
    return list;
}

void set_scope_root(dom::Element const* root) { t_scope_root = root; }

dom::Element const* scope_root() { return t_scope_root; }

void set_shadow_scope(dom::Node const* shadow_root) { t_shadow_scope = shadow_root; }

dom::Node const* shadow_scope() { return t_shadow_scope; }

bool matches_slotted(ComplexSelector const& selector, dom::Element const& element)
{
    if (!selector.slotted || selector.slotted->selectors.empty())
        return false;
    // The element is in another tree than the rule: it is tested as itself.
    dom::Node const* const scope = t_shadow_scope;
    t_shadow_scope = nullptr;
    bool const found = matches_compound(selector.slotted->selectors.front().compounds.back(), element);
    t_shadow_scope = scope;
    return found;
}

bool matches_part(ComplexSelector const& selector, dom::Element const& element)
{
    dom::Attr const* const part = element.find_attribute("part");
    if (part == nullptr)
        return false;
    for (std::string const& name : selector.parts) {
        if (!class_list_contains(part->value, name, false))
            return false;
    }
    dom::Node const* const scope = t_shadow_scope;
    t_shadow_scope = nullptr;
    bool found = true;
    for (SimpleSelector const& state : selector.part_states)
        found = found && matches_simple(state, element);
    t_shadow_scope = scope;
    return found;
}

// @supports selector( <complex-selector> ) (css-conditional-3 §6): a single
// complex selector, not a list, and with :is()/:where() held to their
// simple-selector grammar rather than allowed to forgive an invalid piece
// the way they would in a real selector — see t_selectors_unforgiving.
bool selector_list_is_strictly_valid(std::vector<ComponentValue> const& prelude)
{
    t_selectors_unforgiving = true;
    std::optional<SelectorList> const parsed = parse_selector_list_internal(prelude, false);
    t_selectors_unforgiving = false;
    return parsed.has_value() && parsed->selectors.size() == 1;
}

bool matches(ComplexSelector const& selector, dom::Element const& element)
{
    if (selector.compounds.empty())
        return false;
    return matches_from(selector, selector.compounds.size() - 1, element);
}

bool element_in_state(SimpleSelector::PseudoKind kind, dom::Element const& element)
{
    SimpleSelector simple;
    simple.kind = SimpleSelector::Kind::PseudoClass;
    simple.pseudo = kind;
    return matches_simple(simple, element);
}

bool contains_has(SimpleSelector const& simple)
{
    if (simple.pseudo == SimpleSelector::PseudoKind::Has)
        return true;
    if (!simple.argument)
        return false;
    for (ComplexSelector const& inner : simple.argument->selectors) {
        for (CompoundSelector const& compound : inner.compounds) {
            for (SimpleSelector const& nested : compound.simples) {
                if (contains_has(nested))
                    return true;
            }
        }
    }
    return false;
}

bool matches_compound_but_has(CompoundSelector const& compound, dom::Element const& element)
{
    // A :not(:has(...)) is as open as the :has() inside it: its answer now
    // is the one a change may have turned.
    for (SimpleSelector const& simple : compound.simples) {
        if (!contains_has(simple) && !matches_simple(simple, element))
            return false;
    }
    return true;
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

bool takes_text(dom::Element const& element)
{
    return element.is_html("textarea") || (element.is_html("input") && takes_placeholder(input_type(element)));
}

}
