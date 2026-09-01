#include "css/Parser.h"

#include "core/Ascii.h"
#include "css/Tokenizer.h"

#include <utility>

namespace sashfold::css {

namespace {

// §5.3 token stream, over an eagerly tokenized vector (stylesheets are small;
// marks and restores become index bookkeeping).
class TokenStream {
public:
    explicit TokenStream(std::string_view utf8)
        : m_tokens(Tokenizer::tokenize(utf8))
    {
    }

    Token const& peek(std::size_t offset = 0) const
    {
        static Token const eof {}; // a default Token is the EOF token
        if (m_index + offset >= m_tokens.size())
            return eof;
        return m_tokens[m_index + offset];
    }

    Token consume()
    {
        if (m_index >= m_tokens.size())
            return Token {};
        return m_tokens[m_index++];
    }

    void discard()
    {
        if (m_index < m_tokens.size())
            ++m_index;
    }

    void discard_whitespace()
    {
        while (peek().type == Token::Type::Whitespace)
            discard();
    }

    bool at_end() const { return m_index >= m_tokens.size(); }

    std::size_t mark() const { return m_index; }
    void restore(std::size_t mark) { m_index = mark; }

private:
    std::vector<Token> m_tokens;
    std::size_t m_index = 0;
};

Token::Type mirror_of(Token::Type open)
{
    switch (open) {
    case Token::Type::OpenBrace: return Token::Type::CloseBrace;
    case Token::Type::OpenSquare: return Token::Type::CloseSquare;
    default: return Token::Type::CloseParen;
    }
}

struct Parser {
    TokenStream input;

    explicit Parser(std::string_view utf8)
        : input(utf8)
    {
    }

    // §5.5.8. Consume a component value.
    ComponentValue consume_component_value()
    {
        Token::Type const type = input.peek().type;
        if (type == Token::Type::OpenBrace || type == Token::Type::OpenSquare
            || type == Token::Type::OpenParen)
            return ComponentValue { consume_simple_block() };
        if (type == Token::Type::Function)
            return ComponentValue { consume_function() };
        return ComponentValue { input.consume() };
    }

    // §5.5.9. Consume a simple block.
    SimpleBlock consume_simple_block()
    {
        SimpleBlock block;
        block.open = input.peek().type;
        Token::Type const closing = mirror_of(block.open);
        input.discard();
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::EndOfFile)
                return block; // parse error
            if (type == closing) {
                input.discard();
                return block;
            }
            block.values.push_back(consume_component_value());
        }
    }

    // §5.5.10. Consume a function.
    FunctionValue consume_function()
    {
        FunctionValue function;
        function.name = input.consume().value;
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::EndOfFile) // parse error
                return function;
            if (type == Token::Type::CloseParen) {
                input.discard();
                return function;
            }
            function.values.push_back(consume_component_value());
        }
    }

    // §5.5.7. Consume a list of component values.
    std::vector<ComponentValue> consume_component_value_list(Token::Type stop, bool nested)
    {
        std::vector<ComponentValue> values;
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::EndOfFile || type == stop)
                return values;
            if (type == Token::Type::CloseBrace) {
                if (nested)
                    return values;
                values.push_back(ComponentValue { input.consume() }); // parse error
                continue;
            }
            values.push_back(consume_component_value());
        }
    }

    // §5.5.6's tail: consume the remnants of a bad declaration.
    void consume_bad_declaration_remnants(bool nested)
    {
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::EndOfFile || type == Token::Type::Semicolon) {
                input.discard();
                return;
            }
            if (type == Token::Type::CloseBrace) {
                if (nested)
                    return;
                input.discard();
                continue;
            }
            (void)consume_component_value();
        }
    }

    // §5.5.6. Consume a declaration.
    bool consume_declaration(Declaration& out, bool nested)
    {
        if (input.peek().type != Token::Type::Ident) {
            consume_bad_declaration_remnants(nested);
            return false;
        }
        out.name = input.consume().value;
        input.discard_whitespace();
        if (input.peek().type != Token::Type::Colon) {
            consume_bad_declaration_remnants(nested);
            return false;
        }
        input.discard();
        input.discard_whitespace();
        out.value = consume_component_value_list(Token::Type::Semicolon, nested);

        // The last two non-whitespace values being "!" then "important" set the
        // flag; whitespace may sit anywhere around them.
        auto const last_non_whitespace = [&](std::size_t from) -> std::ptrdiff_t {
            for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(from) - 1; i >= 0; --i) {
                if (!out.value[static_cast<std::size_t>(i)].is_token(Token::Type::Whitespace))
                    return i;
            }
            return -1;
        };
        std::ptrdiff_t const important_index = last_non_whitespace(out.value.size());
        if (important_index >= 0) {
            ComponentValue const& maybe_important = out.value[static_cast<std::size_t>(important_index)];
            if (maybe_important.is_token(Token::Type::Ident)
                && ascii_ci_equals(maybe_important.token().value, "important")) {
                std::ptrdiff_t const bang_index = last_non_whitespace(static_cast<std::size_t>(important_index));
                if (bang_index >= 0) {
                    ComponentValue const& maybe_bang = out.value[static_cast<std::size_t>(bang_index)];
                    if (maybe_bang.is_token(Token::Type::Delim) && maybe_bang.token().delim == U'!') {
                        out.value.erase(out.value.begin() + important_index);
                        out.value.erase(out.value.begin() + bang_index);
                        out.important = true;
                    }
                }
            }
        }
        while (!out.value.empty() && out.value.back().is_token(Token::Type::Whitespace))
            out.value.pop_back();

        bool const custom_property = out.name.size() >= 2 && out.name[0] == '-' && out.name[1] == '-';
        if (!custom_property) {
            // A top-level {}-block must be the entire value.
            bool has_brace_block = false;
            bool has_other = false;
            for (ComponentValue const& value : out.value) {
                if (value.is_block() && value.block().open == Token::Type::OpenBrace)
                    has_brace_block = true;
                else if (!value.is_token(Token::Type::Whitespace))
                    has_other = true;
            }
            if (has_brace_block && has_other)
                return false;
        }
        // (unicode-range descriptor re-tokenization arrives with @font-face.)
        return true;
    }

    // §5.5.3. Consume a qualified rule. Distinguishes "nothing" from an
    // invalid-rule error (the caller's declaration flushing differs). The
    // result travels as the concrete QualifiedRule — callers wrap it into a
    // Rule by construction, never by variant assignment (gcc 13's
    // -Wmaybe-uninitialized misfires on the variant move-assign path).
    enum class QualifiedResult {
        Rule,
        Nothing,
        Invalid,
    };
    QualifiedResult consume_qualified_rule(QualifiedRule& out, Token::Type stop, bool nested)
    {
        QualifiedRule rule;
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::EndOfFile || (stop != Token::Type::EndOfFile && type == stop))
                return QualifiedResult::Nothing; // parse error
            if (type == Token::Type::CloseBrace) {
                if (nested)
                    return QualifiedResult::Nothing; // parse error
                rule.prelude.push_back(ComponentValue { input.consume() });
                continue;
            }
            if (type == Token::Type::OpenBrace) {
                // A prelude that reads as a custom property declaration is
                // never a rule (--foo:hover { } stays invalid everywhere).
                std::size_t first = 0;
                while (first < rule.prelude.size()
                    && rule.prelude[first].is_token(Token::Type::Whitespace))
                    ++first;
                bool custom_property_shaped = false;
                if (first + 1 < rule.prelude.size()
                    && rule.prelude[first].is_token(Token::Type::Ident)
                    && rule.prelude[first].token().value.starts_with("--")) {
                    std::size_t second = first + 1;
                    while (second < rule.prelude.size()
                        && rule.prelude[second].is_token(Token::Type::Whitespace))
                        ++second;
                    custom_property_shaped = second < rule.prelude.size()
                        && rule.prelude[second].is_token(Token::Type::Colon);
                }
                if (custom_property_shaped) {
                    if (nested) {
                        consume_bad_declaration_remnants(true);
                        return QualifiedResult::Nothing;
                    }
                    (void)consume_block();
                    return QualifiedResult::Nothing;
                }
                std::vector<Rule> contents = consume_block();
                if (!contents.empty() && contents.front().is_nested_declarations()) {
                    rule.declarations = std::move(
                        std::get<NestedDeclarations>(contents.front().value).declarations);
                    contents.erase(contents.begin());
                }
                rule.child_rules = std::move(contents);
                out = std::move(rule);
                return QualifiedResult::Rule;
            }
            rule.prelude.push_back(consume_component_value());
        }
    }

    // §5.5.2. Consume an at-rule. Same concrete-type contract as above.
    bool consume_at_rule(AtRule& out, bool nested)
    {
        AtRule rule;
        rule.name = input.consume().value;
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::Semicolon || type == Token::Type::EndOfFile) {
                input.discard();
                out = std::move(rule);
                return true;
            }
            if (type == Token::Type::CloseBrace) {
                if (nested) {
                    out = std::move(rule);
                    return true;
                }
                rule.prelude.push_back(ComponentValue { input.consume() }); // parse error
                continue;
            }
            if (type == Token::Type::OpenBrace) {
                rule.child_rules = consume_block();
                rule.has_block = true;
                out = std::move(rule);
                return true;
            }
            rule.prelude.push_back(consume_component_value());
        }
    }

    // §5.5.4. Consume a block ("{" is next).
    std::vector<Rule> consume_block()
    {
        input.discard(); // {
        std::vector<Rule> contents = consume_blocks_contents();
        input.discard(); // } (or EOF)
        return contents;
    }

    // §5.5.5. Consume a block's contents: rules and declaration runs,
    // interleaved. Declaration runs come back wrapped in NestedDeclarations;
    // the qualified-rule caller unwraps a leading run into its declarations.
    // (The draft's exit step forgets to flush the pending run — flushing is
    // the only reading under which "p { color: red }" keeps its declaration.)
    std::vector<Rule> consume_blocks_contents()
    {
        std::vector<Rule> rules;
        std::vector<Declaration> declarations;
        auto const flush = [&] {
            if (!declarations.empty()) {
                rules.push_back(Rule { NestedDeclarations { std::move(declarations) } });
                declarations = {};
            }
        };
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::Whitespace || type == Token::Type::Semicolon) {
                input.discard();
                continue;
            }
            if (type == Token::Type::EndOfFile || type == Token::Type::CloseBrace) {
                flush();
                return rules;
            }
            if (type == Token::Type::AtKeyword) {
                flush();
                AtRule at_rule;
                if (consume_at_rule(at_rule, true))
                    rules.push_back(Rule { std::move(at_rule) });
                continue;
            }
            std::size_t const mark = input.mark();
            Declaration declaration;
            if (consume_declaration(declaration, true)) {
                declarations.push_back(std::move(declaration));
                continue;
            }
            input.restore(mark);
            QualifiedRule qualified;
            switch (consume_qualified_rule(qualified, Token::Type::Semicolon, true)) {
            case QualifiedResult::Nothing:
                break;
            case QualifiedResult::Invalid:
                flush();
                break;
            case QualifiedResult::Rule:
                flush();
                rules.push_back(Rule { std::move(qualified) });
                break;
            }
        }
    }

    // §5.5.1. Consume a stylesheet's contents.
    std::vector<Rule> consume_stylesheet_contents()
    {
        std::vector<Rule> rules;
        while (true) {
            Token::Type const type = input.peek().type;
            if (type == Token::Type::Whitespace || type == Token::Type::CDO
                || type == Token::Type::CDC) {
                input.discard();
                continue;
            }
            if (type == Token::Type::EndOfFile)
                return rules;
            if (type == Token::Type::AtKeyword) {
                AtRule at_rule;
                if (consume_at_rule(at_rule, false))
                    rules.push_back(Rule { std::move(at_rule) });
                continue;
            }
            QualifiedRule qualified;
            if (consume_qualified_rule(qualified, Token::Type::EndOfFile, false)
                == QualifiedResult::Rule)
                rules.push_back(Rule { std::move(qualified) });
        }
    }
};

} // namespace

Stylesheet parse_stylesheet(std::string_view utf8)
{
    Parser parser(utf8);
    Stylesheet stylesheet;
    stylesheet.rules = parser.consume_stylesheet_contents();
    return stylesheet;
}

std::vector<Rule> parse_blocks_contents_rules(std::string_view utf8,
    std::vector<Declaration>& leading_declarations)
{
    Parser parser(utf8);
    std::vector<Rule> contents = parser.consume_blocks_contents();
    if (!contents.empty() && contents.front().is_nested_declarations()) {
        leading_declarations = std::move(
            std::get<NestedDeclarations>(contents.front().value).declarations);
        contents.erase(contents.begin());
    }
    return contents;
}

std::vector<Declaration> parse_declaration_list(std::string_view utf8)
{
    // A style attribute keeps every declaration run and drops rules (the
    // CSSOM materialization for element.style).
    Parser parser(utf8);
    std::vector<Declaration> declarations;
    for (Rule& rule : parser.consume_blocks_contents()) {
        if (rule.is_nested_declarations()) {
            auto& run = std::get<NestedDeclarations>(rule.value).declarations;
            for (Declaration& declaration : run)
                declarations.push_back(std::move(declaration));
        }
    }
    return declarations;
}

}
