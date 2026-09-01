#include "Test.h"

#include "css/Tokenizer.h"

#include <string>
#include <vector>

using namespace sashfold;
using css::Token;

namespace {

char const* type_name(Token::Type type)
{
    switch (type) {
    case Token::Type::Ident: return "ident";
    case Token::Type::Function: return "function";
    case Token::Type::AtKeyword: return "at-keyword";
    case Token::Type::Hash: return "hash";
    case Token::Type::String: return "string";
    case Token::Type::BadString: return "bad-string";
    case Token::Type::Url: return "url";
    case Token::Type::BadUrl: return "bad-url";
    case Token::Type::Delim: return "delim";
    case Token::Type::Number: return "number";
    case Token::Type::Percentage: return "percentage";
    case Token::Type::Dimension: return "dimension";
    case Token::Type::UnicodeRange: return "unicode-range";
    case Token::Type::Whitespace: return "ws";
    case Token::Type::CDO: return "cdo";
    case Token::Type::CDC: return "cdc";
    case Token::Type::Colon: return "colon";
    case Token::Type::Semicolon: return "semicolon";
    case Token::Type::Comma: return "comma";
    case Token::Type::OpenSquare: return "[";
    case Token::Type::CloseSquare: return "]";
    case Token::Type::OpenParen: return "(";
    case Token::Type::CloseParen: return ")";
    case Token::Type::OpenBrace: return "{";
    case Token::Type::CloseBrace: return "}";
    case Token::Type::EndOfFile: return "eof";
    }
    return "?";
}

// Compact spelling of a token stream, e.g. "ident:color colon ws dimension:12:px".
std::string spell(std::string_view input, bool unicode_ranges_allowed = false)
{
    std::string out;
    for (Token const& token : css::Tokenizer::tokenize(input, unicode_ranges_allowed)) {
        if (!out.empty())
            out += ' ';
        out += type_name(token.type);
        switch (token.type) {
        case Token::Type::Ident:
        case Token::Type::Function:
        case Token::Type::AtKeyword:
        case Token::Type::String:
        case Token::Type::Url:
            out += ':' + token.value;
            break;
        case Token::Type::Hash:
            out += token.hash_type == Token::HashType::Id ? ":id:" : ":any:";
            out += token.value;
            break;
        case Token::Type::Delim:
            out += ':';
            out += static_cast<char>(token.delim < 0x80 ? token.delim : '?');
            break;
        case Token::Type::Number:
        case Token::Type::Percentage:
        case Token::Type::Dimension: {
            std::string number = std::to_string(token.numeric_value);
            number.erase(number.find_last_not_of('0') + 1);
            if (!number.empty() && number.back() == '.')
                number.pop_back();
            out += ':' + number;
            if (token.numeric_type == Token::NumericType::Integer)
                out += ":int";
            if (token.type == Token::Type::Dimension)
                out += ':' + token.unit;
            break;
        }
        case Token::Type::UnicodeRange: {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, ":%X-%X",
                static_cast<unsigned>(token.range_start), static_cast<unsigned>(token.range_end));
            out += buffer;
            break;
        }
        default:
            break;
        }
    }
    return out;
}

} // namespace

int main()
{
    // --- The shape of ordinary CSS -------------------------------------------
    CHECK_EQ(spell("color: red;"), "ident:color colon ws ident:red semicolon");
    CHECK_EQ(spell("margin:12px 50% .5em"),
        "ident:margin colon dimension:12:int:px ws percentage:50:int ws dimension:0.5:em");
    CHECK_EQ(spell(".cls > #id { }"), "delim:. ident:cls ws delim:> ws hash:id:id ws { ws }");
    CHECK_EQ(spell("a[href^=\"x\"]"), "ident:a [ ident:href delim:^ delim:= string:x ]");
    CHECK_EQ(spell("@media (min-width: 600px)"),
        "at-keyword:media ws ( ident:min-width colon ws dimension:600:int:px )");
    CHECK_EQ(spell("calc(1 + 2)"), "function:calc number:1:int ws delim:+ ws number:2:int )");
    CHECK_EQ(spell("--x: var(--y)"), "ident:--x colon ws function:var ident:--y )");

    // --- Comments ------------------------------------------------------------
    CHECK_EQ(spell("a/* comment */b"), "ident:a ident:b");
    CHECK_EQ(spell("a/* unterminated"), "ident:a");
    CHECK_EQ(spell("a/b"), "ident:a delim:/ ident:b");
    CHECK_EQ(spell("/**//**/x"), "ident:x");

    // --- Numbers -------------------------------------------------------------
    CHECK_EQ(spell("12 -3 +4 1.5 -.5 +.25 3e2 1E-1 2e+3"),
        "number:12:int ws number:-3:int ws number:4:int ws number:1.5 ws number:-0.5 "
        "ws number:0.25 ws number:300 ws number:0.1 ws number:2000");
    CHECK_EQ(spell("5e"), "dimension:5:int:e"); // no digits after e: unit, not exponent
    CHECK_EQ(spell("5e-"), "dimension:5:int:e-"); // the dangling minus joins the unit
    CHECK_EQ(spell("12px7"), "dimension:12:int:px7");
    CHECK_EQ(spell("+ 1"), "delim:+ ws number:1:int");
    CHECK_EQ(spell("- 1"), "delim:- ws number:1:int");
    CHECK_EQ(spell(". 1"), "delim:. ws number:1:int");

    // --- Idents and escapes --------------------------------------------------
    CHECK_EQ(spell("\\41 B"), "ident:AB"); // \41 + trailing space consumed = "A", then B
    CHECK_EQ(spell("-a -\\-x --b -"), "ident:-a ws ident:--x ws ident:--b ws delim:-");
    CHECK_EQ(spell("\\6li"), "ident:\x06li"); // hex escape takes the 6, stops at l
    CHECK_EQ(spell("\\0 x"), "ident:\xEF\xBF\xBDx"); // NUL escape -> U+FFFD
    CHECK_EQ(spell("caf\xC3\xA9"), "ident:caf\xC3\xA9"); // non-ASCII ident code point
    CHECK_EQ(spell("\\"), "ident:\xEF\xBF\xBD"); // EOF is not a newline: valid escape -> U+FFFD ident
    CHECK_EQ(spell("\\\nx"), "delim:\\ ws ident:x"); // a newline does break the escape

    // --- Hash ----------------------------------------------------------------
    CHECK_EQ(spell("#foo #0ab #-x #\\41"), "hash:id:foo ws hash:any:0ab ws hash:id:-x ws hash:id:A");
    CHECK_EQ(spell("# #."), "delim:# ws delim:# delim:.");

    // --- Strings -------------------------------------------------------------
    CHECK_EQ(spell("'it\\'s'"), "string:it's");
    CHECK_EQ(spell("\"a\\\nb\""), "string:ab"); // escaped newline continues the string
    CHECK_EQ(spell("\"unterminated"), "string:unterminated"); // EOF: still a string
    CHECK_EQ(spell("\"bad\nx\""), "bad-string ws ident:x string:"); // raw newline: bad-string
    CHECK_EQ(spell("\"\\41\""), "string:A");

    // --- url() ---------------------------------------------------------------
    CHECK_EQ(spell("url(foo.png)"), "url:foo.png");
    CHECK_EQ(spell("url(  spaced.png  )"), "url:spaced.png");
    CHECK_EQ(spell("url()"), "url:");
    CHECK_EQ(spell("url( \"quoted\" )"), "function:url ws string:quoted ws )");
    CHECK_EQ(spell("url('q')"), "function:url string:q )");
    CHECK_EQ(spell("URL(x)"), "url:x"); // ASCII case-insensitive
    CHECK_EQ(spell("url(a b)"), "bad-url"); // space inside: bad url
    CHECK_EQ(spell("url(a\"b)"), "bad-url");
    CHECK_EQ(spell("url(a(b)"), "bad-url");
    CHECK_EQ(spell("url(\\))"), "url:)"); // escaped paren belongs to the url
    CHECK_EQ(spell("url(unterminated"), "url:unterminated"); // EOF: still a url

    // --- CDO / CDC -----------------------------------------------------------
    CHECK_EQ(spell("<!-- x -->"), "cdo ws ident:x ws cdc");
    CHECK_EQ(spell("<"), "delim:<");
    CHECK_EQ(spell("-->"), "cdc");
    CHECK_EQ(spell("--> x"), "cdc ws ident:x");
    CHECK_EQ(spell("-- >"), "ident:-- ws delim:>"); // -- alone is a custom-ish ident

    // --- At-keywords ---------------------------------------------------------
    CHECK_EQ(spell("@media @-x @2"), "at-keyword:media ws at-keyword:-x ws delim:@ number:2:int");

    // --- Unicode ranges: gated by the flag -----------------------------------
    CHECK_EQ(spell("u+26", true), "unicode-range:26-26");
    CHECK_EQ(spell("U+0-7F", true), "unicode-range:0-7F");
    CHECK_EQ(spell("u+0??", true), "unicode-range:0-FF"); // 0?? spans 000-0FF
    CHECK_EQ(spell("u+???", true), "unicode-range:0-FFF");
    CHECK_EQ(spell("U+ABCDEF12", true), "unicode-range:ABCDEF-ABCDEF number:12:int");
    CHECK_EQ(spell("u+26", false), "ident:u number:26:int"); // +26 reads as a signed number
    CHECK_EQ(spell("u-nice", true), "ident:u-nice"); // no +: plain ident either way

    // --- Preprocessing -------------------------------------------------------
    CHECK_EQ(spell("a\r\nb\rc\fd"), "ident:a ws ident:b ws ident:c ws ident:d");
    CHECK_EQ(spell(std::string_view("a\0b", 3)), "ident:a\xEF\xBF\xBD" "b"); // NUL -> U+FFFD, itself an ident code point

    return sashfold::test::report("css-tokenizer");
}
