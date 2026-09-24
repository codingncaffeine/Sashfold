#pragma once

// The CSS parser, css-syntax-3 §5 (current editor's draft): token stream in,
// rules and declarations out — including the nesting-era mixed block contents
// (declarations and child rules interleaved). Grammar-level validity ("valid
// in the current context") is the consumers' business; syntactically well-
// formed constructs all survive to the output.

#include "css/Token.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sashfold::css {

struct ComponentValue;

// function-name( ...component values... )
struct FunctionValue {
    std::string name;
    std::vector<ComponentValue> values;
};

// A {}-, []-, or ()-block and its contents.
struct SimpleBlock {
    Token::Type open = Token::Type::OpenBrace; // OpenBrace, OpenSquare, or OpenParen
    std::vector<ComponentValue> values;
};

struct ComponentValue {
    std::variant<Token, FunctionValue, SimpleBlock> value;

    bool is_token() const { return std::holds_alternative<Token>(value); }
    bool is_function() const { return std::holds_alternative<FunctionValue>(value); }
    bool is_block() const { return std::holds_alternative<SimpleBlock>(value); }
    Token const& token() const { return std::get<Token>(value); }
    FunctionValue const& function() const { return std::get<FunctionValue>(value); }
    SimpleBlock const& block() const { return std::get<SimpleBlock>(value); }
    bool is_token(Token::Type type) const { return is_token() && token().type == type; }
};

struct Declaration {
    std::string name;
    std::vector<ComponentValue> value;
    bool important = false;
};

struct Rule;

// prelude { declarations; child rules } — a style rule once selectors land.
struct QualifiedRule {
    std::vector<ComponentValue> prelude;
    std::vector<Declaration> declarations;
    std::vector<Rule> child_rules;
};

// @name prelude; or @name prelude { mixed contents }
struct AtRule {
    std::string name;
    std::vector<ComponentValue> prelude;
    bool has_block = false;
    std::vector<Rule> child_rules; // declaration runs arrive as NestedDeclarations
};

// A run of declarations that follows nested child rules (CSSNestedDeclarations).
struct NestedDeclarations {
    std::vector<Declaration> declarations;
};

struct Rule {
    std::variant<QualifiedRule, AtRule, NestedDeclarations> value;

    bool is_qualified() const { return std::holds_alternative<QualifiedRule>(value); }
    bool is_at_rule() const { return std::holds_alternative<AtRule>(value); }
    bool is_nested_declarations() const { return std::holds_alternative<NestedDeclarations>(value); }
    QualifiedRule const& qualified() const { return std::get<QualifiedRule>(value); }
    AtRule const& at_rule() const { return std::get<AtRule>(value); }
    NestedDeclarations const& nested_declarations() const { return std::get<NestedDeclarations>(value); }
};

struct Stylesheet {
    std::vector<Rule> rules;
};

// §5.4.3. Parse a stylesheet.
Stylesheet parse_stylesheet(std::string_view utf8);

// The same, kept: a sheet's rules by its text, for the sixty-four texts
// read last, shared by everything that reads a sheet's rules — the
// resolver's rule sets, the @import scan, the @font-face gathering — so
// a page's sheets are parsed once however often they are collected. The
// shell collects a page's sheets again whenever a script adds one, and a
// page carrying twenty-six megabytes of CSS in twenty-six sheets had them
// parsed some three times per collection, sixteen collections over: four
// seconds of its load. Shipping engines keep one parsed sheet per text
// the same way. What comes back is immutable and may be kept.
std::shared_ptr<Stylesheet const> parse_stylesheet_shared(std::string_view utf8);

// How many times text has been parsed into a stylesheet since the
// program started, the kept parses not counted: what a test holds still.
std::size_t stylesheets_parsed();

// §5.4.5. Parse a block's contents — the style="" attribute entry point.
// Returns the mixed contents; use the helper below when only the
// declarations matter.
std::vector<Rule> parse_blocks_contents_rules(std::string_view utf8,
    std::vector<Declaration>& leading_declarations);

// Declarations of a style attribute (leading run only, the common case).
std::vector<Declaration> parse_declaration_list(std::string_view utf8);

// §5.4.9. Parse a list of component values — an attribute that holds CSS
// values but no declarations (sizes, media conditions).
std::vector<ComponentValue> parse_component_value_list(std::string_view utf8);

}
