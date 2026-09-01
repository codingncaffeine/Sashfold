#pragma once

// CSS tokens (css-syntax-3 §4). One struct for every token type; the payload
// fields that don't apply to a given type stay defaulted.

#include <string>

namespace sashfold::css {

struct Token {
    enum class Type {
        Ident,
        Function, // value = the function name, "(" already consumed
        AtKeyword,
        Hash,
        String,
        BadString,
        Url,
        BadUrl,
        Delim,
        Number,
        Percentage,
        Dimension,
        UnicodeRange, // only under "unicode ranges allowed" (§4.3.1)
        Whitespace,
        CDO,
        CDC,
        Colon,
        Semicolon,
        Comma,
        OpenSquare,
        CloseSquare,
        OpenParen,
        CloseParen,
        OpenBrace,
        CloseBrace,
        EndOfFile,
    };

    enum class NumericType {
        Integer,
        Number,
    };

    enum class HashType {
        Unrestricted,
        Id,
    };

    Type type = Type::EndOfFile;

    std::string value; // ident/function/at-keyword/hash/string/url text (UTF-8)
    char32_t delim = 0; // Delim only

    double numeric_value = 0; // Number/Percentage/Dimension
    NumericType numeric_type = NumericType::Integer;
    bool has_sign = false; // the number was written with an explicit +/- (an+b needs this)
    std::string unit; // Dimension only

    HashType hash_type = HashType::Unrestricted; // Hash only

    char32_t range_start = 0; // UnicodeRange only
    char32_t range_end = 0;

    bool is(Type t) const { return type == t; }
};

}
