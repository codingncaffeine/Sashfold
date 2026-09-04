#include "js/Lexer.h"

#include "js/Strings.h" // string_to_number, append_code_point, utf16_from_utf8

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sashfold::js {

namespace {

constexpr char32_t eof_sentinel = 0xFFFFFFFF;

bool is_decimal_digit(char32_t c)
{
    return c >= U'0' && c <= U'9';
}

bool is_octal_digit(char32_t c)
{
    return c >= U'0' && c <= U'7';
}

bool is_binary_digit(char32_t c)
{
    return c == U'0' || c == U'1';
}

bool is_hex_digit(char32_t c)
{
    return is_decimal_digit(c) || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F');
}

int hex_value(char32_t c)
{
    if (is_decimal_digit(c))
        return static_cast<int>(c - U'0');
    if (c >= U'a' && c <= U'f')
        return static_cast<int>(c - U'a') + 10;
    return static_cast<int>(c - U'A') + 10;
}

bool is_ascii_letter(char32_t c)
{
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

bool is_high_surrogate(char32_t c)
{
    return c >= 0xD800 && c <= 0xDBFF;
}

bool is_low_surrogate(char32_t c)
{
    return c >= 0xDC00 && c <= 0xDFFF;
}

// The cursor over the source. Every consumption goes through advance(),
// so line and column stay right whatever grammar is being scanned, and a
// read past the end answers the sentinel rather than touching memory.
class Scanner {
public:
    Scanner(std::u16string_view text, Lexer::State& state)
        : m_source(text)
        , m_state(state)
    {
    }

    char32_t peek(std::size_t ahead = 0) const
    {
        std::size_t const index = m_state.offset + ahead;
        if (index >= m_source.size())
            return eof_sentinel;
        return m_source[index];
    }
    bool at_end() const { return m_state.offset >= m_source.size(); }
    std::size_t offset() const { return m_state.offset; }
    std::u16string_view source() const { return m_source; }

    // The code point here — a surrogate pair joined, a lone surrogate
    // itself — and how many units it spans.
    char32_t code_point(std::size_t* units) const
    {
        char32_t const first = peek();
        if (is_high_surrogate(first) && is_low_surrogate(peek(1))) {
            *units = 2;
            return 0x10000 + ((first - 0xD800) << 10) + (peek(1) - 0xDC00);
        }
        *units = 1;
        return first;
    }

    // Consumes one code unit — or a CR LF pair, which is a single
    // LineTerminatorSequence (§12.3) and so a single line.
    void advance()
    {
        if (at_end())
            return;
        char16_t const c = m_source[m_state.offset];
        ++m_state.offset;
        if (c == u'\r' && !at_end() && m_source[m_state.offset] == u'\n')
            ++m_state.offset;
        if (Lexer::is_line_terminator(c)) {
            ++m_state.line;
            m_state.column = 1;
        } else {
            ++m_state.column;
        }
    }
    void advance(std::size_t units)
    {
        for (std::size_t i = 0; i < units; ++i)
            advance();
    }
    bool eat(char16_t c)
    {
        if (peek() != c)
            return false;
        advance();
        return true;
    }
    SourcePosition position() const
    {
        SourcePosition p;
        p.offset = static_cast<std::uint32_t>(m_state.offset);
        p.line = m_state.line;
        p.column = m_state.column;
        return p;
    }

private:
    std::u16string_view m_source;
    Lexer::State& m_state;
};

Token invalid(Token token, std::string message)
{
    token.type = TokenType::Invalid;
    token.message = std::move(message);
    return token;
}

void skip_to_line_end(Scanner& s)
{
    while (!s.at_end() && !Lexer::is_line_terminator(s.peek()))
        s.advance();
}

struct Trivia {
    bool newline = false; // a line terminator was crossed, comments included
    bool unterminated_comment = false;
    SourcePosition comment_start;
};

// Whitespace and comments before a token (§12.2–§12.4, and Annex B.1.1's
// HTML-like comments, which script code — never a module — still allows).
Trivia skip_trivia(Scanner& s)
{
    Trivia trivia;
    // `-->` is a comment only at the start of a line (B.1.1
    // SingleLineHTMLCloseComment): after a line terminator, which may sit
    // inside a multi-line comment, with only whitespace and single-line
    // delimited comments between. Every browser also takes it at the start
    // of the input, and so do we.
    bool at_line_start = s.offset() == 0;
    while (!s.at_end()) {
        char32_t const c = s.peek();
        if (Lexer::is_whitespace(c)) {
            s.advance();
            continue;
        }
        if (Lexer::is_line_terminator(c)) {
            s.advance();
            trivia.newline = true;
            at_line_start = true;
            continue;
        }
        if (c == U'/' && s.peek(1) == U'/') {
            skip_to_line_end(s);
            continue;
        }
        if (c == U'/' && s.peek(1) == U'*') {
            SourcePosition const start = s.position();
            s.advance(2);
            bool closed = false;
            while (!s.at_end()) {
                if (s.peek() == U'*' && s.peek(1) == U'/') {
                    s.advance(2);
                    closed = true;
                    break;
                }
                // A multi-line comment holding a line terminator counts as
                // one for ASI (§12.4: "considered to be a LineTerminator").
                if (Lexer::is_line_terminator(s.peek())) {
                    trivia.newline = true;
                    at_line_start = true;
                }
                s.advance();
            }
            if (!closed) {
                trivia.unterminated_comment = true;
                trivia.comment_start = start;
                break;
            }
            continue;
        }
        if (c == U'<' && s.peek(1) == U'!' && s.peek(2) == U'-' && s.peek(3) == U'-') {
            skip_to_line_end(s); // SingleLineHTMLOpenComment, anywhere
            continue;
        }
        if (c == U'-' && s.peek(1) == U'-' && s.peek(2) == U'>' && at_line_start) {
            skip_to_line_end(s);
            continue;
        }
        if (c == U'#' && s.peek(1) == U'!' && s.offset() == 0) {
            skip_to_line_end(s); // §12.5 Hashbang Comments: only at the very start
            continue;
        }
        break;
    }
    return trivia;
}

// After the `u` of a \u escape: four hex digits, or {…} naming a code
// point up to 10FFFF (§12.9.4 UnicodeEscapeSequence). Never consumes the
// character that fails, so template scanning can carry on from it.
std::optional<char32_t> scan_unicode_escape(Scanner& s)
{
    if (s.eat(u'{')) {
        char32_t value = 0;
        int digits = 0;
        while (is_hex_digit(s.peek())) {
            value = value * 16 + static_cast<char32_t>(hex_value(s.peek()));
            if (value > 0x10FFFF)
                return std::nullopt;
            ++digits;
            s.advance();
        }
        if (digits == 0 || !s.eat(u'}'))
            return std::nullopt;
        return value;
    }
    char32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        if (!is_hex_digit(s.peek()))
            return std::nullopt;
        value = value * 16 + static_cast<char32_t>(hex_value(s.peek()));
        s.advance();
    }
    return value;
}

struct KeywordEntry {
    std::u16string_view text;
    Keyword keyword;
};

// The reserved words (§12.7.2) that are always keywords. The contextual
// ones — let, of, get, set, static, async, await, yield, and the strict
// mode reserved words — are Identifiers the parser interprets.
constexpr std::array<KeywordEntry, 36> keyword_table { {
    { u"break", Keyword::Break },
    { u"case", Keyword::Case },
    { u"catch", Keyword::Catch },
    { u"class", Keyword::Class },
    { u"const", Keyword::Const },
    { u"continue", Keyword::Continue },
    { u"debugger", Keyword::Debugger },
    { u"default", Keyword::Default },
    { u"delete", Keyword::Delete },
    { u"do", Keyword::Do },
    { u"else", Keyword::Else },
    { u"enum", Keyword::Enum },
    { u"export", Keyword::Export },
    { u"extends", Keyword::Extends },
    { u"false", Keyword::False },
    { u"finally", Keyword::Finally },
    { u"for", Keyword::For },
    { u"function", Keyword::Function },
    { u"if", Keyword::If },
    { u"import", Keyword::Import },
    { u"in", Keyword::In },
    { u"instanceof", Keyword::Instanceof },
    { u"new", Keyword::New },
    { u"null", Keyword::Null },
    { u"return", Keyword::Return },
    { u"super", Keyword::Super },
    { u"switch", Keyword::Switch },
    { u"this", Keyword::This },
    { u"throw", Keyword::Throw },
    { u"true", Keyword::True },
    { u"try", Keyword::Try },
    { u"typeof", Keyword::Typeof },
    { u"var", Keyword::Var },
    { u"void", Keyword::Void },
    { u"while", Keyword::While },
    { u"with", Keyword::With },
} };

// IdentifierName (§12.7): the escapes are decoded into the name and
// flagged, and a name spelled with an escape is never a keyword (§12.7.2:
// an escaped ReservedWord is a Syntax Error, which the parser raises).
Token scan_identifier(Scanner& s, Token token)
{
    std::u16string name;
    bool escaped = false;
    bool first = true;
    while (!s.at_end()) {
        if (s.peek() == U'\\') {
            if (s.peek(1) != U'u') {
                s.advance(); // the error still consumes something: a caller that goes on makes progress
                return invalid(std::move(token), "Invalid Unicode escape sequence");
            }
            s.advance(2);
            std::optional<char32_t> const code_point = scan_unicode_escape(s);
            // §12.7.1 early errors: the escaped code point must itself be
            // an identifier character in that position.
            if (!code_point || !(first ? Lexer::is_identifier_start(*code_point) : Lexer::is_identifier_part(*code_point)))
                return invalid(std::move(token), "Invalid Unicode escape sequence");
            append_code_point(name, *code_point);
            escaped = true;
        } else {
            std::size_t units = 0;
            char32_t const code_point = s.code_point(&units);
            if (!(first ? Lexer::is_identifier_start(code_point) : Lexer::is_identifier_part(code_point)))
                break;
            name.append(s.source().substr(s.offset(), units));
            s.advance(units);
        }
        first = false;
    }
    token.type = TokenType::Identifier;
    token.value = std::move(name);
    token.has_escape = escaped;
    if (!escaped) {
        if (std::optional<Keyword> const keyword = Lexer::keyword_for(token.value)) {
            token.type = TokenType::Keyword;
            token.keyword = *keyword;
        }
    }
    return token;
}

struct DigitRun {
    int count = 0;
    std::optional<std::string> error; // a misplaced numeric separator
};

// DecimalDigits[+Sep] and its hex, octal and binary siblings (§12.9.3):
// digits, with a single `_` allowed only between two of them. The digits
// go to `out` without the separators; the run stops at the first
// character that is neither.
DigitRun scan_digits(Scanner& s, bool (*is_digit)(char32_t), std::string& out)
{
    DigitRun run;
    while (true) {
        char32_t const c = s.peek();
        if (is_digit(c)) {
            out.push_back(static_cast<char>(c));
            ++run.count;
            s.advance();
            continue;
        }
        if (c == U'_' && run.count > 0) {
            s.advance();
            if (s.peek() == U'_') {
                run.error = "Only one underscore is allowed as numeric separator";
                return run;
            }
            if (!is_digit(s.peek())) {
                run.error = "Numeric separators are not allowed at the end of numeric literals";
                return run;
            }
            continue;
        }
        return run;
    }
}

// The Number value (§6.1.6.1: nearest, ties to even) of an integer whose
// radix is a power of two: the bits are streamed into a 53-bit mantissa
// and the rest fold into round and sticky bits, so a literal of any
// length — even one past 2^64 — rounds exactly once.
double power_of_two_value(std::string_view digits, int bits_per_digit)
{
    std::uint64_t mantissa = 0;
    int mantissa_bits = 0;
    bool round_bit = false;
    bool sticky = false;
    int dropped = 0;
    for (char const digit : digits) {
        int const v = hex_value(static_cast<char32_t>(static_cast<unsigned char>(digit)));
        for (int bit = bits_per_digit - 1; bit >= 0; --bit) {
            bool const one = ((v >> bit) & 1) != 0;
            if (mantissa_bits == 0 && !one)
                continue; // leading zeros
            if (mantissa_bits < 53) {
                mantissa = (mantissa << 1) | (one ? 1u : 0u);
                ++mantissa_bits;
            } else {
                if (dropped == 0)
                    round_bit = one;
                else
                    sticky = sticky || one;
                if (dropped < 100000) // past here the value is infinite anyway
                    ++dropped;
            }
        }
    }
    if (round_bit && (sticky || (mantissa & 1u) != 0))
        ++mantissa;
    return std::ldexp(static_cast<double>(mantissa), dropped);
}

// The Number value of a decimal spelling, separators already stripped.
// §12.9.3.1's NumericValue and §7.1.4.1.1's StringToNumber round the
// same digits the same way (§6.1.6.1: nearest, ties to even), so the
// spelling goes through string_to_number — which also carries the
// strtod fallback for a libc++ without std::from_chars(double), rather
// than a second copy of it here.
double decimal_value(std::string const& integer, std::string const& fraction, std::string const& exponent)
{
    std::string text = integer.empty() ? std::string("0") : integer;
    text += '.';
    text += fraction.empty() ? std::string("0") : fraction;
    if (!exponent.empty()) {
        text += 'e';
        text += exponent;
    }
    return string_to_number(utf16_from_utf8(text));
}

// NumericLiteral (§12.9.3, plus Annex B.1.1's legacy octal and 08/09
// forms). `s` sits on a digit, or on a `.` followed by one.
Token scan_number(Scanner& s, Token token)
{
    std::size_t const start = s.offset();
    std::string integer;
    std::string fraction;
    std::string exponent;
    bool bigint_allowed = true; // no fraction, exponent or legacy form seen
    bool decimal = true;
    double value = 0;

    char32_t const prefix = s.peek(1);
    bool const hex = prefix == U'x' || prefix == U'X';
    bool const octal = prefix == U'o' || prefix == U'O';
    bool const binary = prefix == U'b' || prefix == U'B';
    if (s.peek() == U'0' && (hex || octal || binary)) {
        s.advance(2);
        DigitRun const run = scan_digits(s, hex ? is_hex_digit : octal ? is_octal_digit : is_binary_digit, integer);
        if (run.error)
            return invalid(std::move(token), *run.error);
        if (run.count == 0)
            return invalid(std::move(token), hex ? "Missing hexadecimal digits after 0x" : octal ? "Missing octal digits after 0o" : "Missing binary digits after 0b");
        value = power_of_two_value(integer, hex ? 4 : octal ? 3 : 1);
        decimal = false;
    } else if (s.peek() == U'0' && is_decimal_digit(prefix)) {
        // A leading zero before a digit is Annex B: LegacyOctalIntegerLiteral
        // when every digit is octal, otherwise NonOctalDecimalIntegerLiteral
        // (08, 0791). Neither admits separators, and a `use strict` body
        // rejects both, hence the flag.
        token.legacy_octal = true;
        bigint_allowed = false;
        bool all_octal = true;
        while (is_decimal_digit(s.peek())) {
            if (!is_octal_digit(s.peek()))
                all_octal = false;
            integer.push_back(static_cast<char>(s.peek()));
            s.advance();
        }
        if (all_octal) {
            value = power_of_two_value(integer, 3);
            decimal = false; // 017.5 is the literal 017 followed by the literal .5
        }
    } else if (s.peek() == U'0') {
        integer.push_back('0');
        s.advance();
        if (s.peek() == U'_')
            return invalid(std::move(token), "Numeric separator can not be used after leading 0");
    } else if (is_decimal_digit(s.peek())) {
        DigitRun const run = scan_digits(s, is_decimal_digit, integer);
        if (run.error)
            return invalid(std::move(token), *run.error);
    }

    if (decimal) {
        if (s.peek() == U'.') {
            s.advance();
            bigint_allowed = false;
            DigitRun const run = scan_digits(s, is_decimal_digit, fraction);
            if (run.error)
                return invalid(std::move(token), *run.error);
        }
        if (s.peek() == U'e' || s.peek() == U'E') {
            s.advance();
            bigint_allowed = false;
            if (s.peek() == U'+' || s.peek() == U'-') {
                exponent.push_back(static_cast<char>(s.peek()));
                s.advance();
            }
            DigitRun const run = scan_digits(s, is_decimal_digit, exponent);
            if (run.error)
                return invalid(std::move(token), *run.error);
            if (run.count == 0)
                return invalid(std::move(token), "Missing exponent digits");
        }
        value = decimal_value(integer, fraction, exponent);
    }

    // A BigInt suffix is grammatical only on an integer spelling; the
    // engine has no BigInt (the parser names the gap).
    if (s.peek() == U'n' && bigint_allowed)
        return invalid(std::move(token), "BigInt literals are not supported");
    // §12.9.3: "The SourceCharacter immediately following a NumericLiteral
    // must not be an IdentifierStart or DecimalDigit."
    std::size_t units = 0;
    char32_t const following = s.code_point(&units);
    if (following == U'\\' || is_decimal_digit(following) || Lexer::is_identifier_start(following))
        return invalid(std::move(token), "Identifier directly after number");

    token.type = TokenType::Number;
    token.value = std::u16string(s.source().substr(start, s.offset() - start));
    token.number = value;
    return token;
}

// StringLiteral (§12.9.4, escapes per Annex B.1.2). A raw LF or CR ends
// the literal as an error; LS and PS are ordinary string characters since
// ES2019.
Token scan_string(Scanner& s, Token token)
{
    char32_t const quote = s.peek();
    s.advance();
    std::u16string value;
    while (true) {
        char32_t const c = s.peek();
        if (c == eof_sentinel || c == U'\n' || c == U'\r')
            return invalid(std::move(token), "Unterminated string literal");
        if (c == quote) {
            s.advance();
            break;
        }
        if (c != U'\\') {
            value.push_back(static_cast<char16_t>(c));
            s.advance();
            continue;
        }
        token.has_escape = true;
        s.advance();
        char32_t const e = s.peek();
        if (e == eof_sentinel)
            return invalid(std::move(token), "Unterminated string literal");
        if (Lexer::is_line_terminator(e)) {
            s.advance(); // LineContinuation: contributes nothing (a CR LF is one)
            continue;
        }
        s.advance();
        switch (e) {
        case U'n':
            value.push_back(u'\n');
            break;
        case U't':
            value.push_back(u'\t');
            break;
        case U'b':
            value.push_back(u'\b');
            break;
        case U'f':
            value.push_back(u'\f');
            break;
        case U'v':
            value.push_back(u'\v');
            break;
        case U'r':
            value.push_back(u'\r');
            break;
        case U'x': {
            if (!is_hex_digit(s.peek()) || !is_hex_digit(s.peek(1)))
                return invalid(std::move(token), "Invalid hexadecimal escape sequence");
            int const v = hex_value(s.peek()) * 16 + hex_value(s.peek(1));
            s.advance(2);
            value.push_back(static_cast<char16_t>(v));
            break;
        }
        case U'u': {
            std::optional<char32_t> const code_point = scan_unicode_escape(s);
            if (!code_point)
                return invalid(std::move(token), "Invalid Unicode escape sequence");
            append_code_point(value, *code_point);
            break;
        }
        case U'0':
        case U'1':
        case U'2':
        case U'3':
        case U'4':
        case U'5':
        case U'6':
        case U'7': {
            // \0 before anything but a digit is the NUL escape; every other
            // shape is B.1.2 LegacyOctalEscapeSequence: up to three octal
            // digits when the first is 0–3 (so the value fits a byte), two
            // when it is 4–7, and \0 before 8 or 9 is the legacy zero.
            int v = static_cast<int>(e - U'0');
            if (e != U'0' || is_decimal_digit(s.peek()))
                token.legacy_octal = true;
            int const more = e <= U'3' ? 2 : 1;
            for (int i = 0; i < more && is_octal_digit(s.peek()); ++i) {
                v = v * 8 + static_cast<int>(s.peek() - U'0');
                s.advance();
            }
            value.push_back(static_cast<char16_t>(v));
            break;
        }
        case U'8':
        case U'9':
            token.legacy_octal = true; // NonOctalDecimalEscapeSequence
            value.push_back(static_cast<char16_t>(e));
            break;
        default:
            // NonEscapeCharacter: the character itself. A surrogate pair's
            // high half lands here and its low half on the next turn.
            value.push_back(static_cast<char16_t>(e));
            break;
        }
    }
    token.type = TokenType::String;
    token.value = std::move(value);
    return token;
}

// Appends source[from, to) to a template's raw text with CR and CR LF
// folded to LF (§12.9.6.1: the TRV of a LineTerminatorSequence is LF).
void append_raw(std::u16string& raw, std::u16string_view text, std::size_t from, std::size_t to)
{
    for (std::size_t i = from; i < to; ++i) {
        char16_t const c = text[i];
        if (c == u'\r') {
            raw.push_back(u'\n');
            if (i + 1 < to && text[i + 1] == u'\n')
                ++i;
        } else {
            raw.push_back(c);
        }
    }
}

// One span of a template (§12.9.6): from just after the opening ` or the
// } of a substitution, to the closing ` or the next ${. A malformed
// escape (NotEscapeSequence) does not end the span: a tagged template
// accepts it with an undefined cooked value, so the span reports
// cooked_valid false and the parser decides.
Token scan_template_span(Scanner& s, Token token)
{
    std::u16string cooked;
    std::u16string raw;
    bool cooked_valid = true;
    while (true) {
        char32_t const c = s.peek();
        if (c == eof_sentinel)
            return invalid(std::move(token), "Unterminated template literal");
        if (c == U'`') {
            s.advance();
            token.template_tail = true;
            break;
        }
        if (c == U'$' && s.peek(1) == U'{') {
            s.advance(2);
            token.template_tail = false;
            break;
        }
        if (c == U'\r') {
            s.advance(); // CR or CR LF: both LF in the cooked and raw texts
            cooked.push_back(u'\n');
            raw.push_back(u'\n');
            continue;
        }
        if (c != U'\\') {
            cooked.push_back(static_cast<char16_t>(c));
            raw.push_back(static_cast<char16_t>(c));
            s.advance();
            continue;
        }
        std::size_t const escape_start = s.offset();
        s.advance();
        char32_t const e = s.peek();
        if (e == eof_sentinel)
            return invalid(std::move(token), "Unterminated template literal");
        s.advance();
        if (!Lexer::is_line_terminator(e)) {
            switch (e) {
            case U'n':
                cooked.push_back(u'\n');
                break;
            case U't':
                cooked.push_back(u'\t');
                break;
            case U'b':
                cooked.push_back(u'\b');
                break;
            case U'f':
                cooked.push_back(u'\f');
                break;
            case U'v':
                cooked.push_back(u'\v');
                break;
            case U'r':
                cooked.push_back(u'\r');
                break;
            case U'x':
                if (is_hex_digit(s.peek()) && is_hex_digit(s.peek(1))) {
                    cooked.push_back(static_cast<char16_t>(hex_value(s.peek()) * 16 + hex_value(s.peek(1))));
                    s.advance(2);
                } else {
                    cooked_valid = false;
                }
                break;
            case U'u': {
                std::optional<char32_t> const code_point = scan_unicode_escape(s);
                if (code_point)
                    append_code_point(cooked, *code_point);
                else
                    cooked_valid = false;
                break;
            }
            case U'0':
                if (is_decimal_digit(s.peek()))
                    cooked_valid = false; // no octal escapes in a template
                else
                    cooked.push_back(u'\0');
                break;
            case U'1':
            case U'2':
            case U'3':
            case U'4':
            case U'5':
            case U'6':
            case U'7':
            case U'8':
            case U'9':
                cooked_valid = false;
                break;
            default:
                cooked.push_back(static_cast<char16_t>(e));
                break;
            }
        }
        append_raw(raw, s.source(), escape_start, s.offset());
    }
    token.type = TokenType::Template;
    token.cooked_valid = cooked_valid;
    if (cooked_valid)
        token.value = std::move(cooked);
    token.raw = std::move(raw);
    return token;
}

// RegularExpressionLiteral (§12.9.5): the body runs to the first `/` that
// is neither escaped nor inside a class; a line terminator cannot appear
// even escaped. The flags are whatever IdentifierPartChars follow — their
// validity is the parser's early error.
Token scan_regex(Scanner& s, Token token)
{
    s.advance(); // the opening slash
    std::u16string body;
    bool in_class = false;
    while (true) {
        char32_t const c = s.peek();
        if (c == eof_sentinel || Lexer::is_line_terminator(c))
            return invalid(std::move(token), "Invalid regular expression: missing /");
        if (c == U'\\') {
            s.advance();
            char32_t const e = s.peek();
            if (e == eof_sentinel || Lexer::is_line_terminator(e))
                return invalid(std::move(token), "Invalid regular expression: missing /");
            body.push_back(u'\\');
            body.push_back(static_cast<char16_t>(e));
            s.advance();
            continue;
        }
        if (c == U'/' && !in_class) {
            s.advance();
            break;
        }
        if (c == U'[')
            in_class = true;
        else if (c == U']')
            in_class = false;
        body.push_back(static_cast<char16_t>(c));
        s.advance();
    }
    std::u16string flags;
    while (true) {
        std::size_t units = 0;
        char32_t const code_point = s.code_point(&units);
        if (!Lexer::is_identifier_part(code_point))
            break;
        flags.append(s.source().substr(s.offset(), units));
        s.advance(units);
    }
    token.type = TokenType::RegExp;
    token.value = std::move(body);
    token.raw = std::move(flags);
    return token;
}

// Punctuator (§12.8), longest match first. Anything else is an Invalid
// token that consumes one code point, so a caller that keeps going still
// makes progress.
Token scan_punctuator(Scanner& s, Token token)
{
    char32_t const c = s.peek();
    char32_t const c1 = s.peek(1);
    char32_t const c2 = s.peek(2);
    char32_t const c3 = s.peek(3);
    Punctuator p = Punctuator::Semicolon;
    std::size_t length = 1;
    bool matched = true;
    switch (c) {
    case U'{':
        p = Punctuator::LeftBrace;
        break;
    case U'}':
        p = Punctuator::RightBrace;
        break;
    case U'(':
        p = Punctuator::LeftParen;
        break;
    case U')':
        p = Punctuator::RightParen;
        break;
    case U'[':
        p = Punctuator::LeftBracket;
        break;
    case U']':
        p = Punctuator::RightBracket;
        break;
    case U';':
        p = Punctuator::Semicolon;
        break;
    case U',':
        p = Punctuator::Comma;
        break;
    case U':':
        p = Punctuator::Colon;
        break;
    case U'~':
        p = Punctuator::Tilde;
        break;
    case U'.':
        if (c1 == U'.' && c2 == U'.') {
            p = Punctuator::Ellipsis;
            length = 3;
        } else {
            p = Punctuator::Dot;
        }
        break;
    case U'<':
        if (c1 == U'<' && c2 == U'=') {
            p = Punctuator::LeftShiftAssign;
            length = 3;
        } else if (c1 == U'<') {
            p = Punctuator::LeftShift;
            length = 2;
        } else if (c1 == U'=') {
            p = Punctuator::LessEqual;
            length = 2;
        } else {
            p = Punctuator::Less;
        }
        break;
    case U'>':
        if (c1 == U'>' && c2 == U'>' && c3 == U'=') {
            p = Punctuator::UnsignedRightShiftAssign;
            length = 4;
        } else if (c1 == U'>' && c2 == U'>') {
            p = Punctuator::UnsignedRightShift;
            length = 3;
        } else if (c1 == U'>' && c2 == U'=') {
            p = Punctuator::RightShiftAssign;
            length = 3;
        } else if (c1 == U'>') {
            p = Punctuator::RightShift;
            length = 2;
        } else if (c1 == U'=') {
            p = Punctuator::GreaterEqual;
            length = 2;
        } else {
            p = Punctuator::Greater;
        }
        break;
    case U'=':
        if (c1 == U'=' && c2 == U'=') {
            p = Punctuator::StrictEqual;
            length = 3;
        } else if (c1 == U'=') {
            p = Punctuator::Equal;
            length = 2;
        } else if (c1 == U'>') {
            p = Punctuator::Arrow;
            length = 2;
        } else {
            p = Punctuator::Assign;
        }
        break;
    case U'!':
        if (c1 == U'=' && c2 == U'=') {
            p = Punctuator::StrictNotEqual;
            length = 3;
        } else if (c1 == U'=') {
            p = Punctuator::NotEqual;
            length = 2;
        } else {
            p = Punctuator::Exclamation;
        }
        break;
    case U'+':
        if (c1 == U'+') {
            p = Punctuator::PlusPlus;
            length = 2;
        } else if (c1 == U'=') {
            p = Punctuator::PlusAssign;
            length = 2;
        } else {
            p = Punctuator::Plus;
        }
        break;
    case U'-':
        if (c1 == U'-') {
            p = Punctuator::MinusMinus;
            length = 2;
        } else if (c1 == U'=') {
            p = Punctuator::MinusAssign;
            length = 2;
        } else {
            p = Punctuator::Minus;
        }
        break;
    case U'*':
        if (c1 == U'*' && c2 == U'=') {
            p = Punctuator::StarStarAssign;
            length = 3;
        } else if (c1 == U'*') {
            p = Punctuator::StarStar;
            length = 2;
        } else if (c1 == U'=') {
            p = Punctuator::StarAssign;
            length = 2;
        } else {
            p = Punctuator::Star;
        }
        break;
    case U'/':
        if (c1 == U'=') {
            p = Punctuator::SlashAssign;
            length = 2;
        } else {
            p = Punctuator::Slash;
        }
        break;
    case U'%':
        if (c1 == U'=') {
            p = Punctuator::PercentAssign;
            length = 2;
        } else {
            p = Punctuator::Percent;
        }
        break;
    case U'&':
        if (c1 == U'&' && c2 == U'=') {
            p = Punctuator::AndAssign;
            length = 3;
        } else if (c1 == U'&') {
            p = Punctuator::AmpersandAmpersand;
            length = 2;
        } else if (c1 == U'=') {
            p = Punctuator::AmpersandAssign;
            length = 2;
        } else {
            p = Punctuator::Ampersand;
        }
        break;
    case U'|':
        if (c1 == U'|' && c2 == U'=') {
            p = Punctuator::OrAssign;
            length = 3;
        } else if (c1 == U'|') {
            p = Punctuator::PipePipe;
            length = 2;
        } else if (c1 == U'=') {
            p = Punctuator::PipeAssign;
            length = 2;
        } else {
            p = Punctuator::Pipe;
        }
        break;
    case U'^':
        if (c1 == U'=') {
            p = Punctuator::CaretAssign;
            length = 2;
        } else {
            p = Punctuator::Caret;
        }
        break;
    case U'?':
        if (c1 == U'?' && c2 == U'=') {
            p = Punctuator::NullishAssign;
            length = 3;
        } else if (c1 == U'?') {
            p = Punctuator::QuestionQuestion;
            length = 2;
        } else if (c1 == U'.' && !is_decimal_digit(c2)) {
            // §12.8: `?.` is OptionalChainingPunctuator only when no
            // decimal digit follows, so that `a?.5:b` stays a conditional.
            p = Punctuator::QuestionDot;
            length = 2;
        } else {
            p = Punctuator::Question;
        }
        break;
    default:
        matched = false;
        break;
    }
    if (!matched) {
        std::size_t units = 0;
        char32_t const code_point = s.code_point(&units);
        s.advance(units);
        char buffer[40];
        if (code_point >= 0x21 && code_point <= 0x7E)
            std::snprintf(buffer, sizeof buffer, "Unexpected character '%c'", static_cast<char>(code_point));
        else
            std::snprintf(buffer, sizeof buffer, "Unexpected character U+%04X", static_cast<unsigned>(code_point));
        return invalid(std::move(token), buffer);
    }
    s.advance(length);
    token.type = TokenType::Punctuator;
    token.punctuator = p;
    return token;
}

} // namespace

Lexer::Lexer(std::u16string_view source)
    : m_source(source)
{
}

Token Lexer::next(bool regex_allowed)
{
    Scanner scanner(m_source, m_state);
    Trivia const trivia = skip_trivia(scanner);
    Token token;
    token.newline_before = trivia.newline;
    token.position = scanner.position();
    if (trivia.unterminated_comment) {
        token.position = trivia.comment_start;
        token = invalid(std::move(token), "Unterminated comment");
    } else {
        std::size_t units = 0;
        char32_t const c = scanner.code_point(&units);
        if (c == eof_sentinel)
            token.type = TokenType::EndOfInput;
        else if (c == U'\\' || is_identifier_start(c))
            token = scan_identifier(scanner, std::move(token));
        else if (is_decimal_digit(c) || (c == U'.' && is_decimal_digit(scanner.peek(1))))
            token = scan_number(scanner, std::move(token));
        else if (c == U'"' || c == U'\'')
            token = scan_string(scanner, std::move(token));
        else if (c == U'`') {
            scanner.advance();
            token = scan_template_span(scanner, std::move(token));
        } else if (c == U'/' && regex_allowed)
            token = scan_regex(scanner, std::move(token));
        else
            token = scan_punctuator(scanner, std::move(token));
    }
    token.end_offset = static_cast<std::uint32_t>(scanner.offset());
    return token;
}

Token Lexer::next_template_continuation()
{
    Scanner scanner(m_source, m_state);
    Token token;
    token.position = scanner.position();
    token = scan_template_span(scanner, std::move(token));
    token.end_offset = static_cast<std::uint32_t>(scanner.offset());
    return token;
}

std::optional<Keyword> Lexer::keyword_for(std::u16string_view name)
{
    for (KeywordEntry const& entry : keyword_table) {
        if (entry.text == name)
            return entry.keyword;
    }
    return std::nullopt;
}

bool Lexer::is_identifier_start(char32_t c)
{
    if (c < 0x80)
        return is_ascii_letter(c) || c == U'$' || c == U'_';
    // Over-approximation, on purpose: the engine carries no ID_Start /
    // ID_Continue table yet, so every non-ASCII scalar value that is not
    // whitespace or a line terminator is taken as an identifier character.
    // A page written to the real §12.7 grammar lexes identically; one that
    // is a SyntaxError under the tables (say a stray © outside a string)
    // fails later at a reference instead of at parse time. Surrogate code
    // points are never identifier characters and the sentinel is not one.
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        return false;
    return !is_whitespace(c) && !is_line_terminator(c);
}

bool Lexer::is_identifier_part(char32_t c)
{
    // ZWNJ and ZWJ (§12.7 IdentifierPartChar) are non-ASCII and not
    // whitespace, so the start rule already admits them.
    return is_identifier_start(c) || is_decimal_digit(c);
}

bool Lexer::is_line_terminator(char32_t c)
{
    return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

bool Lexer::is_whitespace(char32_t c)
{
    // §12.2: TAB VT FF SP NBSP ZWNBSP, plus the Zs category, whose members
    // outside those are U+1680, U+2000–U+200A, U+202F, U+205F and U+3000.
    return c == 0x09 || c == 0x0B || c == 0x0C || c == 0x20 || c == 0xA0 || c == 0xFEFF
        || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}

}
