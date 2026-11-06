#pragma once

// PDF's lexical conventions (ISO 32000-2 §7.2) and its object syntax
// (§7.3), shared by the file's objects and by content streams: a Lexer
// turns bytes into tokens, a Parser turns tokens into objects. Both are
// forgiving the way readers are — a stray delimiter is skipped, an
// unterminated string or array ends at the end of the data — because the
// files people open are written by every kind of program.

#include "pdf/Object.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace sashfold::pdf {

struct Token {
    enum class Kind { Eof, Integer, Real, String, HexString, Name, ArrayOpen, ArrayClose, DictOpen, DictClose, Keyword };
    Kind kind = Kind::Eof;
    std::int64_t integer = 0;
    double real = 0;
    std::string text; // string bytes, name text, keyword
    std::size_t start = 0; // where the token began
};

bool is_pdf_whitespace(std::uint8_t c);
bool is_pdf_delimiter(std::uint8_t c);

class Lexer {
public:
    explicit Lexer(std::span<std::uint8_t const> data, std::size_t position = 0)
        : m_data(data)
        , m_position(position)
    {
    }

    Token next();
    Token peek();
    std::size_t position() const { return m_position; }
    void seek(std::size_t position) { m_position = position; }
    std::span<std::uint8_t const> data() const { return m_data; }
    void skip_whitespace(); // and comments

private:
    std::span<std::uint8_t const> m_data;
    std::size_t m_position = 0;
};

class Parser {
public:
    // Strings met inside an indirect object are passed through this
    // (decryption) with the object's number; none for content streams.
    using StringFilter = std::function<std::string(std::string)>;

    explicit Parser(Lexer& lexer, bool allow_refs = true)
        : m_lexer(lexer)
        , m_allow_refs(allow_refs)
    {
    }
    void set_string_filter(StringFilter filter) { m_filter = std::move(filter); }

    // One object; `first` is the token already read, when there is one.
    // A keyword (an operator in a content stream) comes back as a null with
    // `keyword` set to its text.
    Object parse(std::string* keyword = nullptr);
    Object parse_from(Token first, std::string* keyword = nullptr);
    Dict parse_dict_body(); // after "<<", through ">>"

private:
    Object parse_from_depth(Token first, std::string* keyword, int depth);
    Dict parse_dict(int depth);

    Lexer& m_lexer;
    bool m_allow_refs = true;
    StringFilter m_filter;
};

}
