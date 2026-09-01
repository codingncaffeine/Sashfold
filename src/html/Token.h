#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sashfold::html {

struct Attribute {
    std::string name; // UTF-8
    std::string value; // UTF-8
};

// One HTML token as defined by the tokenization stage. Strings are UTF-8.
struct Token {
    enum class Type {
        Doctype,
        StartTag,
        EndTag,
        Comment,
        Character,
        EndOfFile,
    };

    Type type = Type::EndOfFile;

    // Character
    char32_t code_point = 0;

    // StartTag / EndTag
    std::string tag_name;
    bool self_closing = false;
    std::vector<Attribute> attributes;

    // Comment
    std::string data;

    // Doctype — missing (never seen) and empty are distinct states.
    std::optional<std::string> doctype_name;
    std::optional<std::string> public_identifier;
    std::optional<std::string> system_identifier;
    bool force_quirks = false;

    bool is_character() const { return type == Type::Character; }
    bool is_start_tag() const { return type == Type::StartTag; }
    bool is_end_tag() const { return type == Type::EndTag; }
    bool is_eof() const { return type == Type::EndOfFile; }
};

}
