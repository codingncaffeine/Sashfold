#pragma once

// The tokenizer (§12). Works on UTF-16 code units, which is what a script
// is. Two things only the parser knows are passed in: whether a `/` may
// start a regular expression here, and when the `}` that closes a template
// substitution has been consumed so the template's next span is wanted.
// Identifiers with `\u` escapes are decoded and flagged; a keyword spelled
// with an escape is not a keyword (§12.7.2).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "js/Ast.h" // SourcePosition

namespace sashfold::js {

enum class TokenType : std::uint8_t {
    EndOfInput,
    Identifier, // includes the contextual words: let, of, get, set, static, async, await, yield
    Keyword, // the always-reserved words
    Punctuator,
    Number,
    String,
    Template, // one span of a template: see Token::template_tail
    RegExp,
    Invalid, // Token::message says why
};

enum class Keyword : std::uint8_t {
    Break,
    Case,
    Catch,
    Class,
    Const,
    Continue,
    Debugger,
    Default,
    Delete,
    Do,
    Else,
    Enum,
    Export,
    Extends,
    False,
    Finally,
    For,
    Function,
    If,
    Import,
    In,
    Instanceof,
    New,
    Null,
    Return,
    Super,
    Switch,
    This,
    Throw,
    True,
    Try,
    Typeof,
    Var,
    Void,
    While,
    With,
};

enum class Punctuator : std::uint8_t {
    LeftBrace,
    RightBrace,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    Dot,
    Ellipsis,
    Semicolon,
    Comma,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Equal,
    NotEqual,
    StrictEqual,
    StrictNotEqual,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    StarStar,
    PlusPlus,
    MinusMinus,
    LeftShift,
    RightShift,
    UnsignedRightShift,
    Ampersand,
    Pipe,
    Caret,
    Exclamation,
    Tilde,
    AmpersandAmpersand,
    PipePipe,
    QuestionQuestion,
    Question,
    QuestionDot,
    Colon,
    Assign,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    StarStarAssign,
    LeftShiftAssign,
    RightShiftAssign,
    UnsignedRightShiftAssign,
    AmpersandAssign,
    PipeAssign,
    CaretAssign,
    AndAssign,
    OrAssign,
    NullishAssign,
    Arrow,
};

struct Token {
    TokenType type = TokenType::EndOfInput;
    Keyword keyword = Keyword::Break;
    Punctuator punctuator = Punctuator::Semicolon;
    // Identifier: the name with escapes decoded. String: the cooked value.
    // Template: the cooked text of this span. RegExp: the body between the
    // slashes. Number: the source spelling (for `use strict` octal checks).
    std::u16string value;
    // Template: the raw text of this span. RegExp: the flags.
    std::u16string raw;
    double number = 0;
    SourcePosition position;
    std::uint32_t end_offset = 0;
    bool newline_before = false; // a line terminator precedes it: ASI, restricted productions
    bool has_escape = false; // identifier/keyword spelled with \u; string with any escape
    bool template_tail = false; // this span ends the literal (`) rather than opening a substitution (${)
    bool cooked_valid = true; // a template span with a bad escape has no cooked value (§12.9.6.1)
    bool legacy_octal = false; // a number like 017 or a string with \1: forbidden in strict code
    std::string message; // when Invalid

    bool is(Punctuator p) const { return type == TokenType::Punctuator && punctuator == p; }
    bool is(Keyword k) const { return type == TokenType::Keyword && keyword == k; }
    bool is_identifier(std::u16string_view name) const { return type == TokenType::Identifier && value == name; }
};

class Lexer {
public:
    explicit Lexer(std::u16string_view source);

    // The next token. A `/` starts a regular expression only where the
    // grammar allows one; the parser knows, the lexer does not.
    Token next(bool regex_allowed);
    // After the parser has consumed the `}` closing a `${…}` substitution,
    // the remainder of the template is lexed with this instead of next().
    Token next_template_continuation();

    struct State {
        std::size_t offset = 0;
        std::uint32_t line = 1;
        std::uint32_t column = 1;
    };
    State save() const { return m_state; }
    void restore(State state) { m_state = state; }
    std::u16string_view source() const { return m_source; }

    static std::optional<Keyword> keyword_for(std::u16string_view);
    static bool is_identifier_start(char32_t);
    static bool is_identifier_part(char32_t);
    static bool is_line_terminator(char32_t); // LF, CR, LS, PS
    static bool is_whitespace(char32_t); // §12.2: TAB VT FF SP NBSP ZWNBSP and Zs

private:
    std::u16string_view m_source;
    State m_state;
};

}
