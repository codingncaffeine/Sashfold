#include "Test.h"

#include "css/Parser.h"

#include <string>

using namespace sashfold;
using css::ComponentValue;
using css::Declaration;
using css::Rule;
using css::Token;

namespace {

// Prelude spelled compactly: idents/delims/etc joined without whitespace tokens.
std::string spell_prelude(std::vector<ComponentValue> const& prelude)
{
    std::string out;
    for (ComponentValue const& value : prelude) {
        if (value.is_token(Token::Type::Whitespace)) {
            out += ' ';
            continue;
        }
        if (value.is_token()) {
            Token const& token = value.token();
            switch (token.type) {
            case Token::Type::Ident: out += token.value; break;
            case Token::Type::Hash: out += '#' + token.value; break;
            case Token::Type::Delim: out += static_cast<char>(token.delim); break;
            case Token::Type::Colon: out += ':'; break;
            case Token::Type::Comma: out += ','; break;
            default: out += '?'; break;
            }
            continue;
        }
        if (value.is_block()) {
            out += value.block().open == Token::Type::OpenSquare ? "[...]" : "(...)";
            continue;
        }
        out += value.function().name + "(...)";
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

std::string spell_value(std::vector<ComponentValue> const& values)
{
    std::string out;
    for (ComponentValue const& value : values) {
        if (value.is_token(Token::Type::Whitespace)) {
            out += ' ';
        } else if (value.is_token()) {
            Token const& token = value.token();
            switch (token.type) {
            case Token::Type::Ident: out += token.value; break;
            case Token::Type::Number:
            case Token::Type::Dimension:
            case Token::Type::Percentage: {
                std::string number = std::to_string(token.numeric_value);
                number.erase(number.find_last_not_of('0') + 1);
                if (!number.empty() && number.back() == '.')
                    number.pop_back();
                out += number;
                if (token.type == Token::Type::Percentage)
                    out += '%';
                out += token.unit;
                break;
            }
            case Token::Type::String: out += '"' + token.value + '"'; break;
            case Token::Type::Comma: out += ','; break;
            case Token::Type::Delim: out += static_cast<char>(token.delim); break;
            default: out += '?'; break;
            }
        } else if (value.is_function()) {
            out += value.function().name + '(' + spell_value(value.function().values) + ')';
        } else {
            out += '{' + spell_value(value.block().values) + '}';
        }
    }
    return out;
}

} // namespace

int main()
{
    // --- A plain stylesheet --------------------------------------------------
    {
        auto sheet = css::parse_stylesheet("p { color: red; margin: 0 auto }\n"
                                           "h1, .big { font-size: 2em !important }");
        CHECK_EQ(sheet.rules.size(), std::size_t { 2 });
        if (sheet.rules.size() == 2) {
            CHECK(sheet.rules[0].is_qualified());
            auto const& p = sheet.rules[0].qualified();
            CHECK_EQ(spell_prelude(p.prelude), "p");
            CHECK_EQ(p.declarations.size(), std::size_t { 2 });
            if (p.declarations.size() == 2) {
                CHECK_EQ(p.declarations[0].name, "color");
                CHECK_EQ(spell_value(p.declarations[0].value), "red");
                CHECK(!p.declarations[0].important);
                CHECK_EQ(p.declarations[1].name, "margin");
                CHECK_EQ(spell_value(p.declarations[1].value), "0 auto");
            }
            auto const& h = sheet.rules[1].qualified();
            CHECK_EQ(spell_prelude(h.prelude), "h1, .big");
            CHECK_EQ(h.declarations.size(), std::size_t { 1 });
            if (h.declarations.size() == 1) {
                CHECK(h.declarations[0].important);
                CHECK_EQ(spell_value(h.declarations[0].value), "2em");
            }
        }
    }

    // --- At-rules: statement and block forms ---------------------------------
    {
        auto sheet = css::parse_stylesheet("@import \"x.css\"; @media (min-width: 10px) { a { b: c } }");
        CHECK_EQ(sheet.rules.size(), std::size_t { 2 });
        if (sheet.rules.size() == 2) {
            CHECK(sheet.rules[0].is_at_rule());
            CHECK_EQ(sheet.rules[0].at_rule().name, "import");
            CHECK(!sheet.rules[0].at_rule().has_block);
            auto const& media = sheet.rules[1].at_rule();
            CHECK_EQ(media.name, "media");
            CHECK(media.has_block);
            CHECK_EQ(media.child_rules.size(), std::size_t { 1 });
            if (media.child_rules.size() == 1) {
                CHECK(media.child_rules[0].is_qualified());
                CHECK_EQ(spell_prelude(media.child_rules[0].qualified().prelude), "a");
            }
        }
    }

    // --- CDO/CDC ignored at top level ---------------------------------------
    CHECK_EQ(css::parse_stylesheet("<!-- p{a:b} -->").rules.size(), std::size_t { 1 });

    // --- Nesting: rules inside rules, declaration runs preserved -------------
    {
        auto sheet = css::parse_stylesheet("a { color: red; & b { x: y } color: blue }");
        CHECK_EQ(sheet.rules.size(), std::size_t { 1 });
        if (!sheet.rules.empty()) {
            auto const& a = sheet.rules[0].qualified();
            CHECK_EQ(a.declarations.size(), std::size_t { 1 }); // leading run
            CHECK_EQ(a.child_rules.size(), std::size_t { 2 }); // nested rule + trailing run
            if (a.child_rules.size() == 2) {
                CHECK(a.child_rules[0].is_qualified());
                CHECK(a.child_rules[1].is_nested_declarations());
                if (a.child_rules[1].is_nested_declarations()) {
                    auto const& trailing = a.child_rules[1].nested_declarations().declarations;
                    CHECK_EQ(trailing.size(), std::size_t { 1 });
                    if (!trailing.empty())
                        CHECK_EQ(spell_value(trailing[0].value), "blue");
                }
            }
        }
    }

    // --- Error recovery ------------------------------------------------------
    {
        // A busted declaration is skipped to the semicolon; the rest survives.
        auto decls = css::parse_declaration_list("color:red; 4px; background: green");
        CHECK_EQ(decls.size(), std::size_t { 2 });
        if (decls.size() == 2) {
            CHECK_EQ(decls[0].name, "color");
            CHECK_EQ(decls[1].name, "background");
        }
    }
    {
        // Unclosed block at EOF: parse error, contents still returned.
        auto sheet = css::parse_stylesheet("p { color: red");
        CHECK_EQ(sheet.rules.size(), std::size_t { 1 });
        if (!sheet.rules.empty())
            CHECK_EQ(sheet.rules[0].qualified().declarations.size(), std::size_t { 1 });
    }
    {
        // A stray } at the top level lands in a prelude, not a crash.
        auto sheet = css::parse_stylesheet("} p { a: b }");
        CHECK_EQ(sheet.rules.size(), std::size_t { 1 });
    }
    {
        // Custom-property-shaped rule is discarded wholesale.
        auto sheet = css::parse_stylesheet("--x:hover { a: b } p { c: d }");
        CHECK_EQ(sheet.rules.size(), std::size_t { 1 });
        if (!sheet.rules.empty())
            CHECK_EQ(spell_prelude(sheet.rules[0].qualified().prelude), "p");
    }

    // --- Declarations: custom properties and blocks --------------------------
    {
        auto decls = css::parse_declaration_list("--theme: { nested: stuff }; width: 10px");
        CHECK_EQ(decls.size(), std::size_t { 2 });
        if (decls.size() == 2) {
            CHECK_EQ(decls[0].name, "--theme");
            CHECK_EQ(decls[1].name, "width");
        }
    }
    {
        // A non-custom property with a {}-block plus other junk is invalid.
        auto decls = css::parse_declaration_list("width: {a} b; height: 2px");
        CHECK_EQ(decls.size(), std::size_t { 1 });
        if (!decls.empty())
            CHECK_EQ(decls[0].name, "height");
    }

    // --- important edge cases ------------------------------------------------
    {
        auto decls = css::parse_declaration_list("a: b !ImPoRtAnT ; c: d ! important; e: !important");
        CHECK_EQ(decls.size(), std::size_t { 3 });
        if (decls.size() == 3) {
            CHECK(decls[0].important);
            CHECK_EQ(spell_value(decls[0].value), "b");
            CHECK(decls[1].important); // whitespace between ! and important is fine
            CHECK(decls[2].important);
            CHECK_EQ(spell_value(decls[2].value), ""); // nothing left of the value
        }
    }

    // --- Functions and blocks in values --------------------------------------
    {
        auto decls = css::parse_declaration_list("background: url(x.png), linear-gradient(red, blue)");
        CHECK_EQ(decls.size(), std::size_t { 1 });
        if (!decls.empty())
            CHECK_EQ(spell_value(decls[0].value), "?, linear-gradient(red, blue)"); // url token spells as ?
    }

    // --- style="" attribute entry -------------------------------------------
    {
        auto decls = css::parse_declaration_list("color: red; font-weight: bold");
        CHECK_EQ(decls.size(), std::size_t { 2 });
    }

    return sashfold::test::report("css-parser");
}
