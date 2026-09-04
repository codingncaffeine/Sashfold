#include "Test.h"

#include "js/Lexer.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;
using js::Keyword;
using js::Lexer;
using js::Punctuator;
using js::Token;
using js::TokenType;

namespace {

constexpr TokenType Id = TokenType::Identifier;
constexpr TokenType Kw = TokenType::Keyword;
constexpr TokenType Punct = TokenType::Punctuator;
constexpr TokenType Num = TokenType::Number;
constexpr TokenType Str = TokenType::String;
constexpr TokenType Tmpl = TokenType::Template;
constexpr TokenType Re = TokenType::RegExp;
constexpr TokenType Bad = TokenType::Invalid;
constexpr TokenType End = TokenType::EndOfInput;

char const* type_name(TokenType type)
{
    switch (type) {
    case TokenType::EndOfInput: return "end";
    case TokenType::Identifier: return "id";
    case TokenType::Keyword: return "kw";
    case TokenType::Punctuator: return "punct";
    case TokenType::Number: return "number";
    case TokenType::String: return "string";
    case TokenType::Template: return "template";
    case TokenType::RegExp: return "regexp";
    case TokenType::Invalid: return "invalid";
    }
    return "?";
}

char16_t const* punctuator_text(Punctuator p)
{
    switch (p) {
    case Punctuator::LeftBrace: return u"{";
    case Punctuator::RightBrace: return u"}";
    case Punctuator::LeftParen: return u"(";
    case Punctuator::RightParen: return u")";
    case Punctuator::LeftBracket: return u"[";
    case Punctuator::RightBracket: return u"]";
    case Punctuator::Dot: return u".";
    case Punctuator::Ellipsis: return u"...";
    case Punctuator::Semicolon: return u";";
    case Punctuator::Comma: return u",";
    case Punctuator::Less: return u"<";
    case Punctuator::Greater: return u">";
    case Punctuator::LessEqual: return u"<=";
    case Punctuator::GreaterEqual: return u">=";
    case Punctuator::Equal: return u"==";
    case Punctuator::NotEqual: return u"!=";
    case Punctuator::StrictEqual: return u"===";
    case Punctuator::StrictNotEqual: return u"!==";
    case Punctuator::Plus: return u"+";
    case Punctuator::Minus: return u"-";
    case Punctuator::Star: return u"*";
    case Punctuator::Slash: return u"/";
    case Punctuator::Percent: return u"%";
    case Punctuator::StarStar: return u"**";
    case Punctuator::PlusPlus: return u"++";
    case Punctuator::MinusMinus: return u"--";
    case Punctuator::LeftShift: return u"<<";
    case Punctuator::RightShift: return u">>";
    case Punctuator::UnsignedRightShift: return u">>>";
    case Punctuator::Ampersand: return u"&";
    case Punctuator::Pipe: return u"|";
    case Punctuator::Caret: return u"^";
    case Punctuator::Exclamation: return u"!";
    case Punctuator::Tilde: return u"~";
    case Punctuator::AmpersandAmpersand: return u"&&";
    case Punctuator::PipePipe: return u"||";
    case Punctuator::QuestionQuestion: return u"??";
    case Punctuator::Question: return u"?";
    case Punctuator::QuestionDot: return u"?.";
    case Punctuator::Colon: return u":";
    case Punctuator::Assign: return u"=";
    case Punctuator::PlusAssign: return u"+=";
    case Punctuator::MinusAssign: return u"-=";
    case Punctuator::StarAssign: return u"*=";
    case Punctuator::SlashAssign: return u"/=";
    case Punctuator::PercentAssign: return u"%=";
    case Punctuator::StarStarAssign: return u"**=";
    case Punctuator::LeftShiftAssign: return u"<<=";
    case Punctuator::RightShiftAssign: return u">>=";
    case Punctuator::UnsignedRightShiftAssign: return u">>>=";
    case Punctuator::AmpersandAssign: return u"&=";
    case Punctuator::PipeAssign: return u"|=";
    case Punctuator::CaretAssign: return u"^=";
    case Punctuator::AndAssign: return u"&&=";
    case Punctuator::OrAssign: return u"||=";
    case Punctuator::NullishAssign: return u"??" u"=";
    case Punctuator::Arrow: return u"=>";
    }
    return u"?";
}

char16_t const* keyword_text(Keyword k)
{
    switch (k) {
    case Keyword::Break: return u"break";
    case Keyword::Case: return u"case";
    case Keyword::Catch: return u"catch";
    case Keyword::Class: return u"class";
    case Keyword::Const: return u"const";
    case Keyword::Continue: return u"continue";
    case Keyword::Debugger: return u"debugger";
    case Keyword::Default: return u"default";
    case Keyword::Delete: return u"delete";
    case Keyword::Do: return u"do";
    case Keyword::Else: return u"else";
    case Keyword::Enum: return u"enum";
    case Keyword::Export: return u"export";
    case Keyword::Extends: return u"extends";
    case Keyword::False: return u"false";
    case Keyword::Finally: return u"finally";
    case Keyword::For: return u"for";
    case Keyword::Function: return u"function";
    case Keyword::If: return u"if";
    case Keyword::Import: return u"import";
    case Keyword::In: return u"in";
    case Keyword::Instanceof: return u"instanceof";
    case Keyword::New: return u"new";
    case Keyword::Null: return u"null";
    case Keyword::Return: return u"return";
    case Keyword::Super: return u"super";
    case Keyword::Switch: return u"switch";
    case Keyword::This: return u"this";
    case Keyword::Throw: return u"throw";
    case Keyword::True: return u"true";
    case Keyword::Try: return u"try";
    case Keyword::Typeof: return u"typeof";
    case Keyword::Var: return u"var";
    case Keyword::Void: return u"void";
    case Keyword::While: return u"while";
    case Keyword::With: return u"with";
    }
    return u"?";
}

// Lexes a whole source the way the parser drives the lexer: a `/` may
// start a regular expression after nothing, after a punctuator other
// than ) ] }, and after a keyword; the span after the } that closes a
// substitution is fetched with next_template_continuation(). Stops at
// the first Invalid token, as the parser would.
std::vector<Token> lex(std::u16string_view source)
{
    Lexer lexer(source);
    std::vector<Token> tokens;
    bool regex_allowed = true;
    bool continuation_pending = false;
    std::vector<int> substitution_depths; // the brace depth each open ${ closes at
    int depth = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        Token token = continuation_pending ? lexer.next_template_continuation() : lexer.next(regex_allowed);
        continuation_pending = false;
        tokens.push_back(token);
        if (token.type == End || token.type == Bad)
            break;
        if (token.type == Tmpl) {
            if (!token.template_tail)
                substitution_depths.push_back(depth);
            regex_allowed = false;
            continue;
        }
        if (token.is(Punctuator::LeftBrace))
            ++depth;
        if (token.is(Punctuator::RightBrace)) {
            if (!substitution_depths.empty() && depth == substitution_depths.back()) {
                substitution_depths.pop_back();
                continuation_pending = true;
            } else {
                --depth;
            }
        }
        if (token.type == Punct)
            regex_allowed = !token.is(Punctuator::RightParen) && !token.is(Punctuator::RightBracket)
                && !token.is(Punctuator::RightBrace);
        else
            regex_allowed = token.type == Kw;
    }
    return tokens;
}

// Printable ASCII as itself, every other unit as \uXXXX, so a failure
// prints something a person can read.
std::string ascii(std::u16string_view text)
{
    std::string out;
    for (char16_t const c : text) {
        if (c >= 0x20 && c <= 0x7E) {
            out.push_back(static_cast<char>(c));
        } else {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04X", static_cast<unsigned>(c));
            out += buffer;
        }
    }
    return out;
}

struct Expected {
    TokenType type;
    std::u16string value;
    bool newline = false;
};

std::string spell(TokenType type, std::u16string_view value, bool newline)
{
    std::string out = newline ? "^" : "";
    out += type_name(type);
    out += ':';
    out += ascii(value);
    return out;
}

std::u16string token_text(Token const& token)
{
    if (token.type == Punct)
        return punctuator_text(token.punctuator);
    if (token.type == Kw)
        return keyword_text(token.keyword);
    return token.value;
}

// The (type, text, newline_before) tuples of a source against the
// expected ones; the EndOfInput token is implied unless the list ends in
// an Invalid token.
bool check_tokens(std::u16string_view source, std::vector<Expected> expected, char const* file, int line)
{
    if (expected.empty() || (expected.back().type != Bad && expected.back().type != End))
        expected.push_back({ End, u"", false });
    std::string actual_text;
    for (Token const& token : lex(source))
        actual_text += spell(token.type, token_text(token), token.newline_before) + ' ';
    std::string expected_text;
    for (Expected const& e : expected)
        expected_text += spell(e.type, e.value, e.newline) + ' ';
    std::string const label = "tokens of " + ascii(source);
    return test::check_eq(actual_text, expected_text, label.c_str(), "expected", file, line);
}

#define CHECK_TOKENS(source, ...) check_tokens(source, std::vector<Expected> __VA_ARGS__, __FILE__, __LINE__)

Token first(std::u16string_view source, bool regex_allowed = true)
{
    Lexer lexer(source);
    return lexer.next(regex_allowed);
}

double number_of(std::u16string_view source)
{
    Token const token = first(source);
    if (token.type != Num)
        test::fail("not a number: " + ascii(source) + " (" + type_name(token.type) + ": " + token.message + ")", __FILE__, __LINE__);
    return token.number;
}

void test_punctuators()
{
    // Every punctuator in Lexer.h, spaced so each stands alone; `/` and
    // `/=` sit after a `)` so the regex rule reads them as division.
    CHECK_TOKENS(u"{ } ( ) [ ] . ... ; , < > <= >= == != === !== + - * ) / % ** ++ -- << >> >>> & | ^ ! ~ && || ?? ? ?. : = += -= *= ) /= %= **= <<= >>= >>>= &= |= ^= &&= ||= ??" u"= =>",
        { { Punct, u"{" }, { Punct, u"}" }, { Punct, u"(" }, { Punct, u")" }, { Punct, u"[" }, { Punct, u"]" },
            { Punct, u"." }, { Punct, u"..." }, { Punct, u";" }, { Punct, u"," }, { Punct, u"<" }, { Punct, u">" },
            { Punct, u"<=" }, { Punct, u">=" }, { Punct, u"==" }, { Punct, u"!=" }, { Punct, u"===" }, { Punct, u"!==" },
            { Punct, u"+" }, { Punct, u"-" }, { Punct, u"*" }, { Punct, u")" }, { Punct, u"/" }, { Punct, u"%" },
            { Punct, u"**" }, { Punct, u"++" }, { Punct, u"--" }, { Punct, u"<<" }, { Punct, u">>" }, { Punct, u">>>" },
            { Punct, u"&" }, { Punct, u"|" }, { Punct, u"^" }, { Punct, u"!" }, { Punct, u"~" }, { Punct, u"&&" },
            { Punct, u"||" }, { Punct, u"??" }, { Punct, u"?" }, { Punct, u"?." }, { Punct, u":" }, { Punct, u"=" },
            { Punct, u"+=" }, { Punct, u"-=" }, { Punct, u"*=" }, { Punct, u")" }, { Punct, u"/=" }, { Punct, u"%=" },
            { Punct, u"**=" }, { Punct, u"<<=" }, { Punct, u">>=" }, { Punct, u">>>=" }, { Punct, u"&=" }, { Punct, u"|=" },
            { Punct, u"^=" }, { Punct, u"&&=" }, { Punct, u"||=" }, { Punct, u"??" u"=" }, { Punct, u"=>" } });
    // Longest match with nothing between.
    CHECK_TOKENS(u"a>>>=b", { { Id, u"a" }, { Punct, u">>>=" }, { Id, u"b" } });
    CHECK_TOKENS(u"a===b!==c", { { Id, u"a" }, { Punct, u"===" }, { Id, u"b" }, { Punct, u"!==" }, { Id, u"c" } });
    CHECK_TOKENS(u"x**=2", { { Id, u"x" }, { Punct, u"**=" }, { Num, u"2" } });
    CHECK_TOKENS(u"a??" u"=b", { { Id, u"a" }, { Punct, u"??" u"=" }, { Id, u"b" } });
    // `?.` only when no digit follows (§12.8), so a conditional survives.
    CHECK_TOKENS(u"a?.b", { { Id, u"a" }, { Punct, u"?." }, { Id, u"b" } });
    CHECK_TOKENS(u"a?.5:b", { { Id, u"a" }, { Punct, u"?" }, { Num, u".5" }, { Punct, u":" }, { Id, u"b" } });
    // Two dots are two Dot tokens; `1..x` is the number `1.` then a dot.
    CHECK_TOKENS(u"a..b", { { Id, u"a" }, { Punct, u"." }, { Punct, u"." }, { Id, u"b" } });
    CHECK_TOKENS(u"1..x", { { Num, u"1." }, { Punct, u"." }, { Id, u"x" } });
    CHECK_TOKENS(u"[...a]", { { Punct, u"[" }, { Punct, u"..." }, { Id, u"a" }, { Punct, u"]" } });
    CHECK_TOKENS(u"x=>x", { { Id, u"x" }, { Punct, u"=>" }, { Id, u"x" } });
}

void test_numbers()
{
    CHECK_EQ(number_of(u"0x1F"), 31.0);
    CHECK_EQ(number_of(u"0X1f"), 31.0);
    CHECK_EQ(number_of(u"0o17"), 15.0);
    CHECK_EQ(number_of(u"0O7"), 7.0);
    CHECK_EQ(number_of(u"0b101"), 5.0);
    CHECK_EQ(number_of(u"0B11"), 3.0);
    CHECK_EQ(number_of(u"017"), 15.0);
    CHECK_EQ(number_of(u"08"), 8.0);
    CHECK_EQ(number_of(u"09.5"), 9.5);
    CHECK_EQ(number_of(u"0791"), 791.0);
    CHECK_EQ(number_of(u"1e3"), 1000.0);
    CHECK_EQ(number_of(u"1E+3"), 1000.0);
    CHECK_EQ(number_of(u"25e-1"), 2.5);
    CHECK_EQ(number_of(u".5"), 0.5);
    CHECK_EQ(number_of(u"1."), 1.0);
    CHECK_EQ(number_of(u"1_000_000"), 1000000.0);
    CHECK_EQ(number_of(u"0xF_F"), 255.0);
    CHECK_EQ(number_of(u"1_0.5_5e1_0"), 10.55e10);
    CHECK_EQ(number_of(u"5e-324"), std::numeric_limits<double>::denorm_min());
    CHECK_EQ(number_of(u"1.7976931348623157e308"), std::numeric_limits<double>::max());
    CHECK_EQ(number_of(u"0.1"), 0.1);
    CHECK_EQ(number_of(u"123.456e-2"), 1.23456);
    CHECK_EQ(number_of(u"0"), 0.0);
    CHECK_EQ(number_of(u"0.0"), 0.0);
    CHECK_EQ(number_of(u"0e5"), 0.0);
    CHECK_EQ(number_of(u"00"), 0.0);
    CHECK_EQ(number_of(u"9007199254740993"), 9007199254740992.0); // 2^53 + 1 rounds to even
    // Out of range either way: the spec's Number value is ±∞ or 0.
    CHECK(std::isinf(number_of(u"1e400")));
    CHECK(std::isinf(number_of(u"2e308")));
    CHECK_EQ(number_of(u"1e-400"), 0.0);
    CHECK_EQ(number_of(u"0.000e99999999999999999999"), 0.0);
    // Binary-radix literals round once, to nearest even, however long.
    CHECK_EQ(number_of(u"0x20000000000001"), 9007199254740992.0); // 2^53 + 1 → 2^53
    CHECK_EQ(number_of(u"0x20000000000003"), 9007199254740996.0); // 2^53 + 3 → 2^53 + 4
    CHECK_EQ(number_of(u"0xFFFFFFFFFFFFFFFFF"), std::ldexp(1.0, 68)); // 2^68 − 1 → 2^68
    CHECK_EQ(number_of(u"0x1FFFFFFFFFFFFF"), 9007199254740991.0);
    CHECK_EQ(number_of(u"0b" u"1" u"0000000000000000000000000000000000000000000000000000000000000000"), std::ldexp(1.0, 64));
    CHECK(std::isinf(number_of(u"0x1" + std::u16string(300, u'0'))));
    // The spelling survives for the parser's strict-mode octal check.
    CHECK_EQ(ascii(first(u"1_000_000").value), "1_000_000");
    CHECK_EQ(ascii(first(u"0x1F").value), "0x1F");
    CHECK_EQ(ascii(first(u".5e+2 ").value), ".5e+2");
    CHECK(first(u"017").legacy_octal);
    CHECK(first(u"08").legacy_octal);
    CHECK(first(u"00").legacy_octal);
    CHECK(!first(u"0").legacy_octal);
    CHECK(!first(u"0x10").legacy_octal);
    CHECK(!first(u"0.5").legacy_octal);
    CHECK(!first(u"8").legacy_octal);
    CHECK(!first(u"10").legacy_octal);
    CHECK_EQ(first(u"12 ").end_offset, 2u);
    // A legacy octal takes no fraction: `017.5` is two literals, which
    // the parser then rejects as two adjacent numbers.
    CHECK_TOKENS(u"017.5", { { Num, u"017" }, { Num, u".5" } });
    CHECK_TOKENS(u"1+2", { { Num, u"1" }, { Punct, u"+" }, { Num, u"2" } });
    CHECK_TOKENS(u"a.b", { { Id, u"a" }, { Punct, u"." }, { Id, u"b" } });
    CHECK_TOKENS(u"x=.5", { { Id, u"x" }, { Punct, u"=" }, { Num, u".5" } });
}

void test_strings()
{
    CHECK_TOKENS(u"'a' \"b\" ''", { { Str, u"a" }, { Str, u"b" }, { Str, u"" } });
    CHECK_TOKENS(u"'it\"s' \"it's\"", { { Str, u"it\"s" }, { Str, u"it's" } });
    Token token = first(u"'\\n\\t\\b\\f\\v\\r\\0'");
    CHECK_EQ(token.type == Str, true);
    CHECK(token.value == std::u16string({ u'\n', u'\t', u'\b', u'\f', u'\v', u'\r', u'\0' }));
    CHECK(token.has_escape);
    CHECK(!token.legacy_octal);
    CHECK(!first(u"'plain'").has_escape);
    CHECK_EQ(ascii(first(u"'\\x41\\u0042\\u{43}'").value), "ABC");
    CHECK_EQ(ascii(first(u"'\\'\\\"\\\\'").value), "'\"\\");
    CHECK_EQ(ascii(first(u"'\\q\\a'").value), "qa"); // NonEscapeCharacter: itself
    // \u{…} past the BMP comes out as a surrogate pair.
    token = first(u"'\\u{1F600}'");
    CHECK_EQ(token.value.size(), 2u);
    CHECK_EQ(static_cast<unsigned>(token.value[0]), 0xD83Du);
    CHECK_EQ(static_cast<unsigned>(token.value[1]), 0xDE00u);
    CHECK(token.value == u"\U0001F600");
    CHECK(first(u"'\\u{10FFFF}'").value == u"\U0010FFFF");
    CHECK(first(u"'\\u{0}'").value == std::u16string(1, u'\0'));
    // Line continuations contribute nothing; CR LF is one of them.
    token = first(u"'a\\\nb'");
    CHECK_EQ(ascii(token.value), "ab");
    CHECK(token.has_escape);
    CHECK_EQ(ascii(first(u"'a\\\r\nb'").value), "ab");
    CHECK_EQ(ascii(first(u"'a\\ b'").value), "ab");
    // Annex B.1.2 legacy octal escapes and \8 \9.
    token = first(u"'\\101'");
    CHECK_EQ(ascii(token.value), "A");
    CHECK(token.legacy_octal);
    CHECK(first(u"'\\1'").value == std::u16string(1, u'\1'));
    CHECK(first(u"'\\12'").value == std::u16string(1, u'\n'));
    CHECK_EQ(ascii(first(u"'\\400'").value), " 0"); // \40 then the digit 0
    CHECK_EQ(ascii(first(u"'\\1234'").value), "S4"); // \123 then the digit 4
    token = first(u"'\\08'");
    CHECK(token.value == std::u16string({ u'\0', u'8' }));
    CHECK(token.legacy_octal);
    token = first(u"'\\8\\9'");
    CHECK_EQ(ascii(token.value), "89");
    CHECK(token.legacy_octal);
    CHECK(!first(u"'\\0'").legacy_octal);
    CHECK(!first(u"'\\0a'").legacy_octal);
    // LS and PS are allowed raw; so is anything non-ASCII.
    CHECK(first(u"'a b'").value == u"a b");
    CHECK(first(u"'a b'").value == u"a b");
    CHECK(first(u"'é\U0001F600'").value == u"é\U0001F600");
    CHECK(first(u"'\\\U0001F600'").value == u"\U0001F600"); // an escaped pair is the pair
    // The token after a continuation sits on the next line.
    Lexer lexer(u"'a\\\nb' x");
    lexer.next(true);
    Token const x = lexer.next(false);
    CHECK_EQ(x.position.line, 2u);
    CHECK_EQ(x.position.column, 4u);
    CHECK(!x.newline_before); // the terminator was inside the literal, not between tokens
}

void test_templates()
{
    CHECK_TOKENS(u"`a${b}c${d}e`",
        { { Tmpl, u"a" }, { Id, u"b" }, { Punct, u"}" }, { Tmpl, u"c" }, { Id, u"d" }, { Punct, u"}" }, { Tmpl, u"e" } });
    std::vector<Token> tokens = lex(u"`a${b}c${d}e`");
    CHECK_EQ(tokens.size(), 8u);
    CHECK(!tokens[0].template_tail);
    CHECK(!tokens[3].template_tail);
    CHECK(tokens[6].template_tail);
    CHECK_EQ(tokens[3].position.offset, 6u); // just past the closing }
    CHECK_EQ(tokens[6].end_offset, 13u);
    CHECK_TOKENS(u"`plain`", { { Tmpl, u"plain" } });
    CHECK_TOKENS(u"``", { { Tmpl, u"" } });
    CHECK_TOKENS(u"`${x}`", { { Tmpl, u"" }, { Id, u"x" }, { Punct, u"}" }, { Tmpl, u"" } });
    // Braces inside a substitution do not end it.
    CHECK_TOKENS(u"`${ {a:1}.a }`",
        { { Tmpl, u"" }, { Punct, u"{" }, { Id, u"a" }, { Punct, u":" }, { Num, u"1" }, { Punct, u"}" }, { Punct, u"." },
            { Id, u"a" }, { Punct, u"}" }, { Tmpl, u"" } });
    // A template inside a substitution.
    CHECK_TOKENS(u"`a${`b${c}d`}e`",
        { { Tmpl, u"a" }, { Tmpl, u"b" }, { Id, u"c" }, { Punct, u"}" }, { Tmpl, u"d" }, { Punct, u"}" }, { Tmpl, u"e" } });
    // Cooked decodes the escapes; raw keeps them.
    Token token = first(u"`\\n\\u{41}\\x42`");
    CHECK(token.cooked_valid);
    CHECK_EQ(ascii(token.value), "\\u000AAB");
    CHECK_EQ(ascii(token.raw), "\\n\\u{41}\\x42");
    // §12.9.6.1: CR and CR LF become LF in both texts.
    token = first(u"`a\r\nb\rc\nd`");
    CHECK_EQ(ascii(token.value), "a\\u000Ab\\u000Ac\\u000Ad");
    CHECK_EQ(ascii(token.raw), "a\\u000Ab\\u000Ac\\u000Ad");
    CHECK_EQ(token.end_offset, 10u);
    // A line continuation: nothing cooked, backslash + LF raw.
    token = first(u"`a\\\r\nb`");
    CHECK_EQ(ascii(token.value), "ab");
    CHECK_EQ(ascii(token.raw), "a\\\\u000Ab");
    // Escaped delimiters, and a $ that opens nothing.
    token = first(u"`\\`\\${`");
    CHECK_EQ(ascii(token.value), "`${");
    CHECK_EQ(ascii(token.raw), "\\`\\${");
    CHECK(token.template_tail);
    CHECK_EQ(ascii(first(u"`a$b$`").value), "a$b$");
    // A bad escape does not end the span: cooked is undefined, raw stands.
    token = first(u"`\\unicode`");
    CHECK_EQ(token.type == Tmpl, true);
    CHECK(!token.cooked_valid);
    CHECK_EQ(ascii(token.raw), "\\unicode");
    CHECK(token.template_tail);
    CHECK(!first(u"`\\01`").cooked_valid);
    CHECK(!first(u"`\\1`").cooked_valid);
    CHECK(!first(u"`\\8`").cooked_valid);
    CHECK(!first(u"`\\x1`").cooked_valid);
    CHECK(!first(u"`\\u{110000}`").cooked_valid);
    CHECK(!first(u"`\\u{`").cooked_valid);
    CHECK(first(u"`\\0`").cooked_valid);
    CHECK(first(u"`\\0`").value == std::u16string(1, u'\0'));
    // The bad escape's raw text and the rest of the span still arrive.
    CHECK_TOKENS(u"`\\u{zz}${x}`", { { Tmpl, u"" }, { Id, u"x" }, { Punct, u"}" }, { Tmpl, u"" } });
    CHECK_EQ(ascii(lex(u"`\\u{zz}${x}`")[0].raw), "\\u{zz}");
    // `\u{` right before the closing backtick is a NotEscapeSequence, and
    // the backtick still closes the literal (§12.9.6).
    CHECK_TOKENS(u"`\\u{`", { { Tmpl, u"" } });
    CHECK_EQ(ascii(first(u"`\\u{`").raw), "\\u{");
    CHECK_EQ(first(u"`abc").type == Bad, true);
    CHECK_EQ(first(u"`a${b}").type == Tmpl, true);
    Lexer lexer(u"`a${b}");
    lexer.next(true);
    lexer.next(false);
    lexer.next(false);
    CHECK_EQ(lexer.next_template_continuation().type == Bad, true);
}

void test_regex()
{
    CHECK_TOKENS(u"/[/]+/g", { { Re, u"[/]+" } });
    CHECK_EQ(ascii(first(u"/[/]+/g").raw), "g");
    CHECK_EQ(ascii(first(u"/a\\/b/").value), "a\\/b");
    CHECK_EQ(ascii(first(u"/x/").raw), "");
    CHECK_EQ(ascii(first(u"/x/gimsuy").raw), "gimsuy");
    CHECK_EQ(ascii(first(u"/[\\]/]/").value), "[\\]/]");
    CHECK_EQ(ascii(first(u"/=/").value), "=");
    CHECK_EQ(ascii(first(u"/\\u{1F600}/u").value), "\\u{1F600}");
    CHECK(first(u"/\U0001F600/").value == u"\U0001F600");
    // Division against regular expression: the parser's call decides.
    CHECK_TOKENS(u"a / b / c", { { Id, u"a" }, { Punct, u"/" }, { Id, u"b" }, { Punct, u"/" }, { Id, u"c" } });
    CHECK_TOKENS(u"x = /re/i", { { Id, u"x" }, { Punct, u"=" }, { Re, u"re" } });
    CHECK_TOKENS(u"(1) / 2", { { Punct, u"(" }, { Num, u"1" }, { Punct, u")" }, { Punct, u"/" }, { Num, u"2" } });
    CHECK_TOKENS(u"a[0] /= 2", { { Id, u"a" }, { Punct, u"[" }, { Num, u"0" }, { Punct, u"]" }, { Punct, u"/=" }, { Num, u"2" } });
    CHECK_TOKENS(u"return /x/", { { Kw, u"return" }, { Re, u"x" } });
    CHECK_TOKENS(u"typeof /x/g.y", { { Kw, u"typeof" }, { Re, u"x" }, { Punct, u"." }, { Id, u"y" } });
    CHECK_TOKENS(u"f(/x/, /y/)", { { Id, u"f" }, { Punct, u"(" }, { Re, u"x" }, { Punct, u"," }, { Re, u"y" }, { Punct, u")" } });
    CHECK_TOKENS(u"/x/gi;", { { Re, u"x" }, { Punct, u";" } });
    CHECK_EQ(first(u"/=", false).is(Punctuator::SlashAssign), true);
    CHECK_EQ(first(u"/", false).is(Punctuator::Slash), true);
    CHECK_EQ(first(u"/abc").type == Bad, true);
    CHECK_EQ(first(u"/ab\nc/").type == Bad, true);
    CHECK_EQ(first(u"/[ab\ncd]/").type == Bad, true);
    CHECK_EQ(first(u"/ab\\\nc/").type == Bad, true);
    CHECK_EQ(first(u"/[abc").type == Bad, true);
    CHECK_EQ(first(u"/abc\\").type == Bad, true);
}

void test_comments_and_newlines()
{
    // ASI's flag: a line terminator between the tokens, comments included.
    CHECK_TOKENS(u"a /* \n */ b", { { Id, u"a" }, { Id, u"b", true } });
    CHECK_TOKENS(u"a /* x */ b", { { Id, u"a" }, { Id, u"b" } });
    CHECK_TOKENS(u"a // c\n b", { { Id, u"a" }, { Id, u"b", true } });
    CHECK_TOKENS(u"a\r\nb", { { Id, u"a" }, { Id, u"b", true } });
    CHECK_TOKENS(u"a b", { { Id, u"a" }, { Id, u"b", true } });
    CHECK_TOKENS(u"a b", { { Id, u"a" }, { Id, u"b", true } });
    CHECK_TOKENS(u"a/**//**/b", { { Id, u"a" }, { Id, u"b" } });
    CHECK_TOKENS(u"a/*/b", { { Id, u"a" }, { Bad, u"" } }); // `/*/` opens without closing
    CHECK_TOKENS(u"a\n", { { Id, u"a" }, { End, u"", true } });
    CHECK_TOKENS(u"a // eof", { { Id, u"a" } });
    // Every §12.2 whitespace character separates tokens.
    CHECK_TOKENS(u"a b﻿c　d e f g h\ti\vj\fk l m",
        { { Id, u"a" }, { Id, u"b" }, { Id, u"c" }, { Id, u"d" }, { Id, u"e" }, { Id, u"f" }, { Id, u"g" }, { Id, u"h" },
            { Id, u"i" }, { Id, u"j" }, { Id, u"k" }, { Id, u"l" }, { Id, u"m" } });
    // Annex B.1.1: <!-- anywhere; --> only at the start of a line.
    CHECK_TOKENS(u"x <!-- y\nz", { { Id, u"x" }, { Id, u"z", true } });
    CHECK_TOKENS(u"x<!--y", { { Id, u"x" } });
    CHECK_TOKENS(u"a --> b", { { Id, u"a" }, { Punct, u"--" }, { Punct, u">" }, { Id, u"b" } });
    CHECK_TOKENS(u"a\n--> b\nc", { { Id, u"a" }, { Id, u"c", true } });
    CHECK_TOKENS(u"a\n  /* x */ --> b\nc", { { Id, u"a" }, { Id, u"c", true } });
    CHECK_TOKENS(u"a /* \n */ --> b\nc", { { Id, u"a" }, { Id, u"c", true } });
    CHECK_TOKENS(u"--> x\ny", { { Id, u"y", true } });
    CHECK_TOKENS(u"  --> x", {});
    CHECK_TOKENS(u"a /**/ --> b", { { Id, u"a" }, { Punct, u"--" }, { Punct, u">" }, { Id, u"b" } });
    CHECK_TOKENS(u"a-->b", { { Id, u"a" }, { Punct, u"--" }, { Punct, u">" }, { Id, u"b" } });
    CHECK_TOKENS(u"a--\n>b", { { Id, u"a" }, { Punct, u"--" }, { Punct, u">", true }, { Id, u"b" } });
    // §12.5: a hashbang only at offset 0.
    CHECK_TOKENS(u"#!/usr/bin/env node\nlet x", { { Id, u"let", true }, { Id, u"x" } });
    CHECK_TOKENS(u"#! only", {});
    CHECK_TOKENS(u" #! x", { { Bad, u"" } });
    CHECK_TOKENS(u"a\n#! x", { { Id, u"a" }, { Bad, u"", true } });
    Token const comment = first(u"/* abc");
    CHECK_EQ(comment.type == Bad, true);
    CHECK_EQ(comment.message, "Unterminated comment");
    CHECK_EQ(comment.position.offset, 0u);
    CHECK_EQ(comment.end_offset, 6u);
    // Repeated next() at the end keeps answering EndOfInput.
    Lexer lexer(u"a");
    lexer.next(true);
    CHECK_EQ(lexer.next(true).type == End, true);
    CHECK_EQ(lexer.next(true).type == End, true);
    CHECK_EQ(Lexer(u"").next(true).type == End, true);
    CHECK_EQ(Lexer(u"").next_template_continuation().type == Bad, true);
}

void test_identifiers()
{
    CHECK_TOKENS(u"let of get set static async await yield implements interface package private protected public",
        { { Id, u"let" }, { Id, u"of" }, { Id, u"get" }, { Id, u"set" }, { Id, u"static" }, { Id, u"async" },
            { Id, u"await" }, { Id, u"yield" }, { Id, u"implements" }, { Id, u"interface" }, { Id, u"package" },
            { Id, u"private" }, { Id, u"protected" }, { Id, u"public" } });
    CHECK_TOKENS(u"if else var function true false null this new typeof in instanceof enum",
        { { Kw, u"if" }, { Kw, u"else" }, { Kw, u"var" }, { Kw, u"function" }, { Kw, u"true" }, { Kw, u"false" },
            { Kw, u"null" }, { Kw, u"this" }, { Kw, u"new" }, { Kw, u"typeof" }, { Kw, u"in" }, { Kw, u"instanceof" },
            { Kw, u"enum" } });
    CHECK_TOKENS(u"$_a1 _$ $$ a$b", { { Id, u"$_a1" }, { Id, u"_$" }, { Id, u"$$" }, { Id, u"a$b" } });
    CHECK_TOKENS(u"If IF ifx", { { Id, u"If" }, { Id, u"IF" }, { Id, u"ifx" } });
    CHECK_EQ(first(u"while").is(Keyword::While), true);
    CHECK(Lexer::keyword_for(u"while") == Keyword::While);
    CHECK(!Lexer::keyword_for(u"let"));
    CHECK(!Lexer::keyword_for(u"While"));
    CHECK(!Lexer::keyword_for(u""));
    // Escapes are decoded and flagged; an escaped keyword is an Identifier.
    Token token = first(u"\\u0061bc");
    CHECK_EQ(token.type == Id, true);
    CHECK_EQ(ascii(token.value), "abc");
    CHECK(token.has_escape);
    CHECK_EQ(token.end_offset, 8u);
    CHECK_EQ(ascii(first(u"\\u{62}").value), "b");
    CHECK_EQ(ascii(first(u"a\\u{000062}c").value), "abc");
    token = first(u"\\u0069f");
    CHECK_EQ(token.type == Id, true);
    CHECK_EQ(ascii(token.value), "if");
    CHECK(token.has_escape);
    CHECK_EQ(first(u"if").type == Kw, true);
    CHECK(!first(u"if").has_escape);
    CHECK_EQ(ascii(first(u"a\\u0030").value), "a0"); // a digit is fine after the start
    CHECK(first(u"\\u{1D400}x").value == u"\U0001D400x");
    // Non-ASCII: the documented over-approximation of ID_Start / ID_Continue.
    CHECK(first(u"été").value == u"été");
    CHECK(first(u"\U0001D400x").value == u"\U0001D400x");
    CHECK(first(u"a‍b‌c").value == u"a‍b‌c");
    CHECK_TOKENS(u"é è", { { Id, u"é" }, { Id, u"è" } });
    // The character classes.
    CHECK(Lexer::is_identifier_start(U'a'));
    CHECK(Lexer::is_identifier_start(U'Z'));
    CHECK(Lexer::is_identifier_start(U'$'));
    CHECK(Lexer::is_identifier_start(U'_'));
    CHECK(!Lexer::is_identifier_start(U'1'));
    CHECK(!Lexer::is_identifier_start(U'\\'));
    CHECK(!Lexer::is_identifier_start(U' '));
    CHECK(Lexer::is_identifier_start(0xE9));
    CHECK(Lexer::is_identifier_start(0x1D400));
    CHECK(!Lexer::is_identifier_start(0xA0));
    CHECK(!Lexer::is_identifier_start(0xFEFF));
    CHECK(!Lexer::is_identifier_start(0x3000));
    CHECK(!Lexer::is_identifier_start(0x2028));
    CHECK(!Lexer::is_identifier_start(0xD800));
    CHECK(!Lexer::is_identifier_start(0xDFFF));
    CHECK(!Lexer::is_identifier_start(0x110000));
    CHECK(!Lexer::is_identifier_start(0xFFFFFFFF));
    CHECK(Lexer::is_identifier_part(U'1'));
    CHECK(Lexer::is_identifier_part(0x200C));
    CHECK(!Lexer::is_identifier_part(U'-'));
    for (char32_t const c : { U'\t', U'\v', U'\f', U' ', static_cast<char32_t>(0xA0), static_cast<char32_t>(0xFEFF),
             static_cast<char32_t>(0x1680), static_cast<char32_t>(0x2000), static_cast<char32_t>(0x2005),
             static_cast<char32_t>(0x200A), static_cast<char32_t>(0x202F), static_cast<char32_t>(0x205F),
             static_cast<char32_t>(0x3000) })
        CHECK(Lexer::is_whitespace(c));
    CHECK(!Lexer::is_whitespace(U'\n'));
    CHECK(!Lexer::is_whitespace(0x200B)); // ZWSP is Cf, not Zs
    CHECK(!Lexer::is_whitespace(0x0085)); // NEL is neither
    CHECK(!Lexer::is_whitespace(U'a'));
    for (char32_t const c : { U'\n', U'\r', static_cast<char32_t>(0x2028), static_cast<char32_t>(0x2029) })
        CHECK(Lexer::is_line_terminator(c));
    CHECK(!Lexer::is_line_terminator(U'\v'));
    CHECK(!Lexer::is_line_terminator(0x85));
}

void test_positions()
{
    std::vector<Token> const tokens = lex(u"ab\ncd\r\n  ef g");
    CHECK_EQ(tokens.size(), 5u);
    CHECK_EQ(tokens[0].position.offset, 0u);
    CHECK_EQ(tokens[0].position.line, 1u);
    CHECK_EQ(tokens[0].position.column, 1u);
    CHECK_EQ(tokens[0].end_offset, 2u);
    CHECK_EQ(tokens[1].position.offset, 3u);
    CHECK_EQ(tokens[1].position.line, 2u);
    CHECK_EQ(tokens[1].position.column, 1u);
    CHECK_EQ(tokens[1].end_offset, 5u);
    CHECK_EQ(tokens[2].position.offset, 9u);
    CHECK_EQ(tokens[2].position.line, 3u); // CR LF was one line
    CHECK_EQ(tokens[2].position.column, 3u);
    CHECK_EQ(tokens[2].end_offset, 11u);
    CHECK_EQ(tokens[3].position.offset, 12u);
    CHECK_EQ(tokens[3].position.line, 4u);
    CHECK_EQ(tokens[3].position.column, 1u);
    CHECK_EQ(tokens[3].end_offset, 13u);
    CHECK_EQ(tokens[4].position.offset, 13u);
    CHECK_EQ(tokens[4].position.line, 4u);
    CHECK_EQ(tokens[4].position.column, 2u);
    CHECK_EQ(tokens[4].end_offset, 13u);
    // Columns count code units: a supplementary character is two.
    std::vector<Token> const wide = lex(u"\U0001F600 x");
    CHECK_EQ(wide[1].position.column, 4u);
    CHECK_EQ(wide[1].position.offset, 3u);
    // A line terminator inside a comment or a template moves the line.
    std::vector<Token> const through = lex(u"/* a\nb */ x `p\nq` y");
    CHECK_EQ(through[0].position.line, 2u);
    CHECK_EQ(through[0].position.column, 6u);
    CHECK_EQ(through[2].position.line, 3u);
    CHECK_EQ(through[2].position.column, 4u);
    // save() / restore() re-lex the same token.
    Lexer lexer(u"a b");
    lexer.next(true);
    Lexer::State const state = lexer.save();
    CHECK_EQ(ascii(lexer.next(true).value), "b");
    CHECK_EQ(lexer.next(true).type == End, true);
    lexer.restore(state);
    CHECK_EQ(ascii(lexer.next(true).value), "b");
    CHECK(lexer.source() == u"a b");
    // An Invalid token is positioned where it started.
    Token const bad = first(u"  'abc");
    CHECK_EQ(bad.position.column, 3u);
    CHECK_EQ(bad.position.offset, 2u);
    CHECK_EQ(bad.end_offset, 6u);
}

void test_invalid()
{
    struct Case {
        char16_t const* source;
        char const* message;
    };
    Case const cases[] = {
        { u"'abc", "Unterminated string literal" },
        { u"\"abc\n\"", "Unterminated string literal" },
        { u"'abc\\", "Unterminated string literal" },
        { u"'\\x4'", "Invalid hexadecimal escape sequence" },
        { u"'\\u12'", "Invalid Unicode escape sequence" },
        { u"'\\u{110000}'", "Invalid Unicode escape sequence" },
        { u"'\\u{}'", "Invalid Unicode escape sequence" },
        { u"/abc", "Invalid regular expression: missing /" },
        { u"1__0", "Only one underscore is allowed as numeric separator" },
        { u"1_", "Numeric separators are not allowed at the end of numeric literals" },
        { u"1_.5", "Numeric separators are not allowed at the end of numeric literals" },
        { u"0_1", "Numeric separator can not be used after leading 0" },
        { u"0x_FF", "Missing hexadecimal digits after 0x" },
        { u"0b2", "Missing binary digits after 0b" },
        { u"0o", "Missing octal digits after 0o" },
        { u"0x", "Missing hexadecimal digits after 0x" },
        { u"1e", "Missing exponent digits" },
        { u"1e+", "Missing exponent digits" },
        { u"10n", "BigInt literals are not supported" },
        { u"0x1Fn", "BigInt literals are not supported" },
        { u"0n", "BigInt literals are not supported" },
        { u"1.5n", "Identifier directly after number" },
        { u"017n", "Identifier directly after number" },
        { u"3in", "Identifier directly after number" },
        { u"1\\u0061", "Identifier directly after number" },
        { u"0b12", "Identifier directly after number" },
        { u"\\u0020x", "Invalid Unicode escape sequence" },
        { u"\\u0030a", "Invalid Unicode escape sequence" },
        { u"\\x41", "Invalid Unicode escape sequence" },
        { u"a\\u{}", "Invalid Unicode escape sequence" },
        { u"/* x", "Unterminated comment" },
        { u"`x", "Unterminated template literal" },
        { u"@", "Unexpected character '@'" },
        { u"#", "Unexpected character '#'" },
        { u"\\", "Invalid Unicode escape sequence" },
        { u"\xD800", "Unexpected character U+D800" },
        { u"\x7F", "Unexpected character U+007F" },
    };
    for (Case const& c : cases) {
        Token const token = first(c.source);
        std::string const label = "invalid: " + ascii(c.source);
        test::check_eq(type_name(token.type), std::string("invalid"), label.c_str(), "invalid", __FILE__, __LINE__);
        test::check_eq(token.message, std::string(c.message), label.c_str(), c.message, __FILE__, __LINE__);
    }
    CHECK(first(u"10n").message.find("BigInt") != std::string::npos);
    // The lexer stays well-defined past an error: every token consumed
    // something, and the state is at the end of what it read.
    Lexer lexer(u"@ b");
    CHECK_EQ(lexer.next(true).type == Bad, true);
    CHECK_EQ(ascii(lexer.next(true).value), "b");
    Lexer backslash(u"\\x41");
    CHECK_EQ(backslash.next(true).type == Bad, true);
    CHECK_EQ(ascii(backslash.next(true).value), "x41");
    CHECK_EQ(first(u"\\").end_offset, 1u);
    CHECK_EQ(number_of(u"0x0"), 0.0);
    CHECK_EQ(number_of(u"0b0"), 0.0);
    // A dot is neither IdentifierStart nor DecimalDigit, so `1.5.3` is two
    // numbers; rejecting the pair is the parser's job.
    CHECK_TOKENS(u"1.5.3", { { Num, u"1.5" }, { Num, u".3" } });
    CHECK_EQ(first(u"1.5.3").end_offset, 3u);
}

// The review's cases: each names the edge it attacks and the clause
// that decides it.
void test_edge_cases()
{
    // §12.7.2: a ReservedWord spelled with an escape is an IdentifierName
    // that is not the keyword; the flag lets the parser raise the error.
    Token token = first(u"\\u{69}f");
    CHECK_EQ(token.type == Id, true);
    CHECK_EQ(ascii(token.value), "if");
    CHECK(token.has_escape);
    token = first(u"i\\u0066");
    CHECK_EQ(token.type == Id, true);
    CHECK(token.has_escape);
    token = first(u"fals\\u0065");
    CHECK_EQ(token.type == Id, true);
    CHECK_EQ(ascii(token.value), "false");
    CHECK(token.has_escape);
    CHECK_EQ(first(u"false").type == Kw, true);
    CHECK(!first(u"false").has_escape);
    CHECK_TOKENS(u"\\u0069f (x)", { { Id, u"if" }, { Punct, u"(" }, { Id, u"x" }, { Punct, u")" } });

    // §12.9.3: "The SourceCharacter immediately following a NumericLiteral
    // must not be an IdentifierStart or DecimalDigit" — `3in` is one
    // error, not `3 in`, and a bare radix prefix is an error of its own.
    CHECK_EQ(first(u"3in").message, "Identifier directly after number");
    CHECK_TOKENS(u"3 in x", { { Num, u"3" }, { Kw, u"in" }, { Id, u"x" } });
    CHECK_EQ(first(u"0x").message, "Missing hexadecimal digits after 0x");
    CHECK_EQ(first(u"0x").end_offset, 2u);
    CHECK_EQ(first(u"0xg").message, "Missing hexadecimal digits after 0x");
    CHECK_EQ(first(u"1e5e5").message, "Identifier directly after number");
    CHECK_EQ(first(u"0b1e5").message, "Identifier directly after number");
    CHECK_EQ(first(u"1\xE9").message, "Identifier directly after number");

    // DecimalLiteral :: . DecimalDigits and DecimalIntegerLiteral . — so
    // `.5.` is the number .5 then a Dot, `1..toString` is `1.` Dot name,
    // `1.e3` carries the exponent on an empty fraction, and `.e3` is no
    // number at all. A hex literal takes no fraction.
    CHECK_TOKENS(u".5.", { { Num, u".5" }, { Punct, u"." } });
    CHECK_TOKENS(u"1..toString", { { Num, u"1." }, { Punct, u"." }, { Id, u"toString" } });
    CHECK_TOKENS(u"1.5.toString", { { Num, u"1.5" }, { Punct, u"." }, { Id, u"toString" } });
    CHECK_EQ(number_of(u"1.e3"), 1000.0);
    CHECK_EQ(number_of(u"0.e1"), 0.0);
    CHECK_TOKENS(u".e3", { { Punct, u"." }, { Id, u"e3" } });
    CHECK_TOKENS(u"0x1.5", { { Num, u"0x1" }, { Num, u".5" } });

    // §12.9.3 NumericLiteralSeparator: a single `_` between two digits of
    // one digit run — never beside the dot, the exponent, its sign, the
    // radix prefix, or the end of the literal.
    for (char16_t const* source : { u"1_.5", u"1e_5", u"1_e5", u"1e+_5", u"1e5_", u"0x_1", u"0x1_", u"0b_1", u"0o1_",
             u".5_", u"1._5", u"1._", u"0.5__5", u"1_000_" }) {
        std::string const label = "separator: " + ascii(source);
        test::check_eq(type_name(first(source).type), std::string("invalid"), label.c_str(), "invalid", __FILE__, __LINE__);
    }
    CHECK_EQ(first(u"1_.5").message, "Numeric separators are not allowed at the end of numeric literals");
    CHECK_EQ(first(u"1e_5").message, "Missing exponent digits");
    CHECK_EQ(first(u"1e+_5").message, "Missing exponent digits");
    CHECK_EQ(first(u"0x_1").message, "Missing hexadecimal digits after 0x");
    CHECK_TOKENS(u"._5", { { Punct, u"." }, { Id, u"_5" } }); // no digit follows the dot: not a number
    CHECK_EQ(number_of(u"0.0_1"), 0.01);
    CHECK_EQ(number_of(u"1e1_0"), 1e10);

    // Annex B.1.1: 017 is LegacyOctalIntegerLiteral, 08 and 018.5 are
    // NonOctalDecimalIntegerLiteral (a DecimalIntegerLiteral, so a
    // fraction and exponent follow); all three are flagged, none takes
    // a separator or the BigInt suffix, and an octal takes no fraction.
    token = first(u"017");
    CHECK_EQ(token.number, 15.0);
    CHECK(token.legacy_octal);
    token = first(u"08");
    CHECK_EQ(token.number, 8.0);
    CHECK(token.legacy_octal);
    token = first(u"018.5");
    CHECK_EQ(token.number, 18.5);
    CHECK(token.legacy_octal);
    CHECK_EQ(ascii(token.value), "018.5");
    CHECK_EQ(number_of(u"08e1"), 80.0);
    CHECK_TOKENS(u"00.5", { { Num, u"00" }, { Num, u".5" } });
    CHECK_EQ(first(u"07_7").type == Bad, true);
    CHECK_EQ(first(u"08_1").type == Bad, true);
    CHECK_EQ(first(u"09n").type == Bad, true);
    CHECK_EQ(first(u"00n").type == Bad, true);
    CHECK_EQ(number_of(u"0777777777777777777777777"), std::ldexp(1.0, 72)); // 2^72 − 1 rounds up

    // §12.9.4 UnicodeEscapeSequence :: u{ CodePoint }, at most 10FFFF: past
    // it is an error in a string or a name, an undefined cooked value in
    // a template, and no run of digits overflows the check.
    CHECK_EQ(first(u"'\\u{110000}'").message, "Invalid Unicode escape sequence");
    CHECK_EQ(first(u"'\\u{FFFFFFFFFFFFFFFFFFFF}'").message, "Invalid Unicode escape sequence");
    CHECK(first(u"'\\u{10FFFF}'").value == u"\U0010FFFF");
    CHECK_EQ(ascii(first(u"'\\u{0000000041}'").value), "A");
    CHECK_EQ(first(u"\\u{110000}").type == Bad, true);
    CHECK_EQ(first(u"a\\u{110000}").type == Bad, true);
    token = first(u"`\\u{110000}`");
    CHECK_EQ(token.type == Tmpl, true);
    CHECK(!token.cooked_valid);
    CHECK_EQ(ascii(token.raw), "\\u{110000}");
    CHECK(token.template_tail);
    // §12.9.6 NotEscapeSequence :: u [lookahead ∉ HexDigit] [lookahead ≠ {]:
    // `\u` right before `${` leaves the substitution intact.
    CHECK_TOKENS(u"`\\u${x}`", { { Tmpl, u"" }, { Id, u"x" }, { Punct, u"}" }, { Tmpl, u"" } });
    CHECK(!lex(u"`\\u${x}`")[0].cooked_valid);
    CHECK_EQ(ascii(lex(u"`\\u${x}`")[0].raw), "\\u");

    // §12.9.4 LineContinuation :: \ LineTerminatorSequence — CR LF is one
    // sequence, so the token after sits on the next line, one column past
    // the quote, with no newline_before (the terminator was inside).
    {
        Lexer lexer(u"'a\\\r\nb' x");
        Token const s = lexer.next(true);
        CHECK_EQ(ascii(s.value), "ab");
        CHECK(s.has_escape);
        CHECK_EQ(s.end_offset, 7u);
        Token const x = lexer.next(false);
        CHECK_EQ(x.position.line, 2u);
        CHECK_EQ(x.position.column, 4u);
        CHECK_EQ(x.position.offset, 8u);
        CHECK(!x.newline_before);
    }
    CHECK_EQ(ascii(first(u"'a\\\rb'").value), "ab");
    CHECK_EQ(ascii(first(u"'a\\ b'").value), "ab");
    CHECK_EQ(ascii(first(u"'a\\ b'").value), "ab");

    // §12.9.4: LS and PS are StringCharacters (ES2019); they end no string
    // and put no newline_before on the token after.
    CHECK_TOKENS(u"'a b' x", { { Str, u"a b" }, { Id, u"x" } });
    CHECK_TOKENS(u"\"a b\" x", { { Str, u"a b" }, { Id, u"x" } });
    CHECK_EQ(first(u"'a\rb'").message, "Unterminated string literal");
    CHECK_EQ(first(u"'a\r\nb'").end_offset, 2u); // stops before the terminator

    // §12.9.6.1: the TV and TRV of a LineTerminatorSequence are both LF,
    // for CR LF and a bare CR alike; a continuation keeps its backslash in
    // the raw text only.
    token = first(u"`a\r\nb\rc`");
    CHECK_EQ(ascii(token.value), "a\\u000Ab\\u000Ac");
    CHECK_EQ(ascii(token.raw), "a\\u000Ab\\u000Ac");
    CHECK_EQ(token.end_offset, 8u);
    token = first(u"`\\\r\n`");
    CHECK_EQ(ascii(token.value), "");
    CHECK_EQ(ascii(token.raw), "\\\\u000A");
    CHECK_EQ(token.end_offset, 5u);
    token = first(u"`\\\r`");
    CHECK_EQ(ascii(token.raw), "\\\\u000A");
    CHECK_EQ(ascii(first(u"`a b`").raw), "a\\u2028b"); // LS is its own TRV

    // The continuation after a } is the parser's call, however deep the
    // braces inside the substitution went.
    CHECK_TOKENS(u"`${ {a:{b:1}}.a }end`",
        { { Tmpl, u"" }, { Punct, u"{" }, { Id, u"a" }, { Punct, u":" }, { Punct, u"{" }, { Id, u"b" }, { Punct, u":" },
            { Num, u"1" }, { Punct, u"}" }, { Punct, u"}" }, { Punct, u"." }, { Id, u"a" }, { Punct, u"}" },
            { Tmpl, u"end" } });
    CHECK_TOKENS(u"`${(() => { return {}; })()}`",
        { { Tmpl, u"" }, { Punct, u"(" }, { Punct, u"(" }, { Punct, u")" }, { Punct, u"=>" }, { Punct, u"{" },
            { Kw, u"return" }, { Punct, u"{" }, { Punct, u"}" }, { Punct, u";" }, { Punct, u"}" }, { Punct, u")" },
            { Punct, u"(" }, { Punct, u")" }, { Punct, u"}" }, { Tmpl, u"" } });
    CHECK_TOKENS(u"`${`${ {} }`}`",
        { { Tmpl, u"" }, { Tmpl, u"" }, { Punct, u"{" }, { Punct, u"}" }, { Punct, u"}" }, { Tmpl, u"" }, { Punct, u"}" },
            { Tmpl, u"" } });

    // §12.8 OptionalChainingPunctuator :: ?. [lookahead ∉ DecimalDigit].
    CHECK_TOKENS(u"a?.5:b", { { Id, u"a" }, { Punct, u"?" }, { Num, u".5" }, { Punct, u":" }, { Id, u"b" } });
    CHECK_TOKENS(u"a?.5e1:b", { { Id, u"a" }, { Punct, u"?" }, { Num, u".5e1" }, { Punct, u":" }, { Id, u"b" } });
    CHECK_TOKENS(u"a?.[0]", { { Id, u"a" }, { Punct, u"?." }, { Punct, u"[" }, { Num, u"0" }, { Punct, u"]" } });
    CHECK_TOKENS(u"a?.(b)", { { Id, u"a" }, { Punct, u"?." }, { Punct, u"(" }, { Id, u"b" }, { Punct, u")" } });
    CHECK_TOKENS(u"a??.5", { { Id, u"a" }, { Punct, u"??" }, { Num, u".5" } });

    // §12.9.5: an escaped slash and a slash inside a class end nothing;
    // the body keeps the backslashes for Regex::compile.
    CHECK_TOKENS(u"/\\//", { { Re, u"\\/" } });
    CHECK_TOKENS(u"/[/]/", { { Re, u"[/]" } });
    CHECK_TOKENS(u"/[\\]/]/g;", { { Re, u"[\\]/]" }, { Punct, u";" } });
    CHECK_TOKENS(u"/a\\/b/.test(s)",
        { { Re, u"a\\/b" }, { Punct, u"." }, { Id, u"test" }, { Punct, u"(" }, { Id, u"s" }, { Punct, u")" } });
    CHECK_TOKENS(u"/[[]/", { { Re, u"[[]" } });
    CHECK_EQ(first(u"/[/").message, "Invalid regular expression: missing /");

    // B.1.1 SingleLineHTMLCloseComment needs a LineTerminatorSequence
    // (LS and PS included) before the -->; anywhere else it is -- then >.
    CHECK_TOKENS(u"a-->b", { { Id, u"a" }, { Punct, u"--" }, { Punct, u">" }, { Id, u"b" } });
    CHECK_TOKENS(u"x = y-->0", { { Id, u"x" }, { Punct, u"=" }, { Id, u"y" }, { Punct, u"--" }, { Punct, u">" }, { Num, u"0" } });
    CHECK_TOKENS(u"a\n-->b\nc", { { Id, u"a" }, { Id, u"c", true } });
    CHECK_TOKENS(u"a -->b\nc", { { Id, u"a" }, { Id, u"c", true } });
    CHECK_TOKENS(u"a\r\n\t-->b", { { Id, u"a" }, { End, u"", true } });

    // B.1.1's comments are trivia: inside a literal, <!-- and --> are text.
    CHECK_TOKENS(u"'<!--' + \"-->\"", { { Str, u"<!--" }, { Punct, u"+" }, { Str, u"-->" } });
    CHECK_TOKENS(u"`<!--${x}-->`", { { Tmpl, u"<!--" }, { Id, u"x" }, { Punct, u"}" }, { Tmpl, u"-->" } });
    CHECK_TOKENS(u"/<!--/", { { Re, u"<!--" } });
    CHECK_TOKENS(u"s = '<!--'\n--> gone\nt", { { Id, u"s" }, { Punct, u"=" }, { Str, u"<!--" }, { Id, u"t", true } });
    CHECK_TOKENS(u"a <!-- b\n'c'", { { Id, u"a" }, { Str, u"c", true } });

    // §12.4: a MultiLineComment counts as a LineTerminator only when it
    // contains one.
    CHECK_TOKENS(u"a/*x*/b", { { Id, u"a" }, { Id, u"b" } });
    CHECK_TOKENS(u"a/* x\ty */++b", { { Id, u"a" }, { Punct, u"++" }, { Id, u"b" } });
    CHECK_TOKENS(u"a/*\r\n*/b", { { Id, u"a" }, { Id, u"b", true } });
    CHECK_TOKENS(u"a/* */b", { { Id, u"a" }, { Id, u"b", true } });
    CHECK_TOKENS(u"return/**/x", { { Kw, u"return" }, { Id, u"x" } });
    CHECK_TOKENS(u"return/*\n*/x", { { Kw, u"return" }, { Id, u"x", true } });

    // Columns count UTF-16 code units, so a supplementary character in a
    // string, a name or between tokens moves the column by two.
    {
        std::vector<Token> const tokens = lex(u"'\U0001F600'y \U0001F600x");
        CHECK_EQ(tokens.size(), 4u);
        CHECK_EQ(tokens[1].position.column, 5u);
        CHECK_EQ(tokens[1].position.offset, 4u);
        CHECK_EQ(tokens[2].position.column, 7u);
        CHECK_EQ(tokens[2].position.offset, 6u);
        CHECK(tokens[2].value == u"\U0001F600x");
        CHECK_EQ(tokens[2].end_offset, 9u);
        CHECK_EQ(tokens[3].position.column, 10u);
    }

    // A block comment still open at the end of the input is one error,
    // positioned at its opening and spanning to the end, whatever the
    // last characters were.
    for (char16_t const* source : { u"/*", u"/* x", u"/* x *", u"/**", u"/*/", u"a /*\n" }) {
        Lexer lexer(source);
        Token t = lexer.next(true);
        if (t.type == Id)
            t = lexer.next(false);
        std::string const label = "comment: " + ascii(source);
        test::check_eq(t.message, std::string("Unterminated comment"), label.c_str(), "Unterminated comment", __FILE__, __LINE__);
        test::check_eq(static_cast<std::size_t>(t.end_offset), std::u16string_view(source).size(), label.c_str(), "length", __FILE__, __LINE__);
        test::check_eq(lexer.next(true).type == End, true, label.c_str(), "end after", __FILE__, __LINE__);
    }
    CHECK_EQ(first(u"a /*\n").position.offset, 0u);
    CHECK_EQ(lex(u"a /*\n")[1].position.offset, 2u);

    // A lone surrogate is no identifier character (§12.7 wants a code
    // point with ID_Start, and a surrogate is not one): outside a literal
    // it is an Unexpected character that consumes one unit; inside a
    // string, template or regular expression it is an ordinary unit.
    CHECK_EQ(first(u"\xD800").message, "Unexpected character U+D800");
    CHECK_EQ(first(u"\xDC00").message, "Unexpected character U+DC00");
    CHECK_EQ(first(u"\xD800").end_offset, 1u);
    CHECK_TOKENS(u"a\xD800", { { Id, u"a" }, { Bad, u"" } });
    CHECK_TOKENS(u"\xD800" u"b", { { Bad, u"" } });
    CHECK(first(u"'\xD800'").value == std::u16string(1, static_cast<char16_t>(0xD800)));
    CHECK(first(u"`\xDC00`").value == std::u16string(1, static_cast<char16_t>(0xDC00)));
    CHECK(first(u"/\xD800/").value == std::u16string(1, static_cast<char16_t>(0xD800)));
    CHECK_EQ(first(u"\\u{D800}").message, "Invalid Unicode escape sequence");
    CHECK(first(u"'\\u{D800}'").value == std::u16string(1, static_cast<char16_t>(0xD800))); // a string may hold one
    CHECK(first(u"'\\uD83D\\uDE00'").value == u"\U0001F600"); // two escapes make a pair
    CHECK_EQ(first(u"\xDC00\xD800").end_offset, 1u); // low then high is not a pair

    // The decimal path goes through StringToNumber (§7.1.4.1.1), which is
    // exactly rounded (§6.1.6.1) and portable; the hard cases stay right.
    CHECK_EQ(number_of(u"2.2250738585072011e-308"), 2.2250738585072011e-308);
    CHECK_EQ(number_of(u"4.9406564584124654e-324"), std::numeric_limits<double>::denorm_min());
    CHECK_EQ(number_of(u"2.4703282292062327e-324"), 0.0); // just under half denorm_min
    CHECK_EQ(number_of(u"2.4703282292062328e-324"), std::numeric_limits<double>::denorm_min());
    CHECK_EQ(number_of(u"1.7976931348623158e308"), std::numeric_limits<double>::max());
    CHECK(std::isinf(number_of(u"1.7976931348623159e308")));
    CHECK_EQ(number_of(u"9007199254740993.0"), 9007199254740992.0);
    CHECK_EQ(number_of(u"0.1e1"), 1.0);
    CHECK_EQ(number_of(u"123456789012345678901234567890"), 1.2345678901234568e29);
}

} // namespace

int main()
{
    test_punctuators();
    test_numbers();
    test_strings();
    test_templates();
    test_regex();
    test_comments_and_newlines();
    test_identifiers();
    test_positions();
    test_invalid();
    test_edge_cases();
    return sashfold::test::report("js_lexer");
}
