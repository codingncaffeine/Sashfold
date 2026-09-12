// gen-bindings: reads the WebIDL subset in idl/*.idl and writes the C++
// that registers those interfaces with the page's realm — the interfaces
// in an order parents come first, the tag-to-interface table, and the
// attributes reflected from content attributes, each as a call to the
// helpers the hand-written bindings use. Nothing here depends on the
// engine: it is a small parser and a printer, built as its own target.
//
//   gen_bindings <in.idl> --output <out.cpp>   writes the file
//   gen_bindings <in.idl> --check <out.cpp>    exits 2 when the file is not what it would write
//
// The subset (see the header of idl/html-elements.idl): comments;
// `[ExtendedAttributes] interface Name : Parent { members };`; a member is
// `[ExtendedAttributes] readonly? attribute Type name;` or a method
// `Type name(...);`, which is read and passed over. Anything else is an
// error that names its line.

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ExtendedAttribute {
    std::string name;
    std::vector<std::string> values; // Name=(a, b) or Name=a; empty for a bare name
};

struct Attribute {
    std::string type; // as written, the tokens joined by one space
    std::string name;
    bool readonly = false;
    std::vector<ExtendedAttribute> extended;
};

struct Interface {
    std::string name;
    std::string parent;
    std::vector<ExtendedAttribute> extended;
    std::vector<Attribute> attributes;
    int line = 0;
};

struct Token {
    enum class Kind { Ident, Punct, Integer, String, End } kind;
    std::string text;
    int line;
};

class Lexer {
public:
    explicit Lexer(std::string text)
        : m_text(std::move(text))
    {
    }

    Token next()
    {
        skip_space_and_comments();
        if (m_pos >= m_text.size())
            return Token { Token::Kind::End, "", m_line };
        char const c = m_text[m_pos];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t const start = m_pos;
            while (m_pos < m_text.size()
                && (std::isalnum(static_cast<unsigned char>(m_text[m_pos])) || m_text[m_pos] == '_'))
                ++m_pos;
            return Token { Token::Kind::Ident, m_text.substr(start, m_pos - start), m_line };
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && m_pos + 1 < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos + 1])))) {
            std::size_t const start = m_pos++;
            while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos])))
                ++m_pos;
            return Token { Token::Kind::Integer, m_text.substr(start, m_pos - start), m_line };
        }
        if (c == '"') {
            std::size_t const start = ++m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] != '"')
                ++m_pos;
            std::string const value = m_text.substr(start, m_pos - start);
            if (m_pos < m_text.size())
                ++m_pos;
            return Token { Token::Kind::String, value, m_line };
        }
        ++m_pos;
        return Token { Token::Kind::Punct, std::string(1, c), m_line };
    }

private:
    void skip_space_and_comments()
    {
        while (m_pos < m_text.size()) {
            char const c = m_text[m_pos];
            if (c == '\n') {
                ++m_line;
                ++m_pos;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                    ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
                m_pos += 2;
                while (m_pos + 1 < m_text.size() && !(m_text[m_pos] == '*' && m_text[m_pos + 1] == '/')) {
                    if (m_text[m_pos] == '\n')
                        ++m_line;
                    ++m_pos;
                }
                m_pos += 2;
            } else {
                break;
            }
        }
    }

    std::string m_text;
    std::size_t m_pos = 0;
    int m_line = 1;
};

[[noreturn]] void fail(int line, std::string const& message)
{
    std::cerr << "gen-bindings: line " << line << ": " << message << "\n";
    std::exit(1);
}

class Parser {
public:
    explicit Parser(std::string text)
        : m_lexer(std::move(text))
    {
        m_token = m_lexer.next();
    }

    std::vector<Interface> parse()
    {
        std::vector<Interface> interfaces;
        while (m_token.kind != Token::Kind::End) {
            std::vector<ExtendedAttribute> extended = extended_attributes();
            if (!is_ident("interface"))
                fail(m_token.line, "expected 'interface', got '" + m_token.text + "'");
            Interface interface;
            interface.line = m_token.line;
            interface.extended = std::move(extended);
            advance();
            interface.name = expect_ident("an interface name");
            if (is_punct(':')) {
                advance();
                interface.parent = expect_ident("a parent interface name");
            }
            expect_punct('{');
            while (!is_punct('}')) {
                if (m_token.kind == Token::Kind::End)
                    fail(m_token.line, "unterminated interface " + interface.name);
                member(interface);
            }
            expect_punct('}');
            expect_punct(';');
            interfaces.push_back(std::move(interface));
        }
        return interfaces;
    }

private:
    void advance() { m_token = m_lexer.next(); }
    bool is_ident(std::string_view text) const { return m_token.kind == Token::Kind::Ident && m_token.text == text; }
    bool is_punct(char c) const { return m_token.kind == Token::Kind::Punct && m_token.text.size() == 1 && m_token.text[0] == c; }

    std::string expect_ident(char const* what)
    {
        if (m_token.kind != Token::Kind::Ident)
            fail(m_token.line, std::string("expected ") + what + ", got '" + m_token.text + "'");
        std::string const text = m_token.text;
        advance();
        return text;
    }

    void expect_punct(char c)
    {
        if (!is_punct(c))
            fail(m_token.line, std::string("expected '") + c + "', got '" + m_token.text + "'");
        advance();
    }

    std::vector<ExtendedAttribute> extended_attributes()
    {
        std::vector<ExtendedAttribute> list;
        if (!is_punct('['))
            return list;
        advance();
        while (!is_punct(']')) {
            ExtendedAttribute attribute;
            attribute.name = expect_ident("an extended attribute name");
            if (is_punct('=')) {
                advance();
                if (is_punct('(')) {
                    advance();
                    while (!is_punct(')')) {
                        attribute.values.push_back(value_token());
                        if (is_punct(','))
                            advance();
                    }
                    advance();
                } else {
                    attribute.values.push_back(value_token());
                }
            }
            list.push_back(std::move(attribute));
            if (is_punct(','))
                advance();
            else if (!is_punct(']'))
                fail(m_token.line, "expected ',' or ']' in an extended attribute list");
        }
        advance();
        return list;
    }

    std::string value_token()
    {
        if (m_token.kind != Token::Kind::Ident && m_token.kind != Token::Kind::Integer && m_token.kind != Token::Kind::String)
            fail(m_token.line, "expected a value, got '" + m_token.text + "'");
        std::string const text = m_token.text;
        advance();
        return text;
    }

    // A type: identifiers, `unsigned`/`unrestricted` prefixes, a trailing `?`.
    std::string type()
    {
        std::string text = expect_ident("a type");
        while (m_token.kind == Token::Kind::Ident && (text == "unsigned" || text == "unrestricted" || text == "long")) {
            // `unsigned long`, `long long`, `unrestricted double`
            if (text == "long" && m_token.text != "long")
                break;
            text += " " + m_token.text;
            advance();
        }
        if (is_punct('?')) {
            text += "?";
            advance();
        }
        return text;
    }

    void member(Interface& interface)
    {
        int const line = m_token.line;
        std::vector<ExtendedAttribute> extended = extended_attributes();
        bool readonly = false;
        if (is_ident("readonly")) {
            readonly = true;
            advance();
        }
        if (is_ident("attribute")) {
            advance();
            Attribute attribute;
            attribute.readonly = readonly;
            attribute.extended = std::move(extended);
            attribute.type = type();
            attribute.name = expect_ident("an attribute name");
            expect_punct(';');
            interface.attributes.push_back(std::move(attribute));
            return;
        }
        if (readonly)
            fail(line, "'readonly' must be followed by 'attribute'");
        // A method: type, name, a parenthesized list, read past and left
        // to the hand-written bindings.
        (void)type();
        (void)expect_ident("a method name");
        expect_punct('(');
        int depth = 1;
        while (depth > 0) {
            if (m_token.kind == Token::Kind::End)
                fail(line, "unterminated argument list");
            if (is_punct('('))
                ++depth;
            else if (is_punct(')'))
                --depth;
            advance();
        }
        expect_punct(';');
    }

    Lexer m_lexer;
    Token m_token;
};

ExtendedAttribute const* find_extended(std::vector<ExtendedAttribute> const& list, std::string_view name)
{
    for (ExtendedAttribute const& attribute : list) {
        if (attribute.name == name)
            return &attribute;
    }
    return nullptr;
}

std::string lowercased(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string generate(std::vector<Interface> const& interfaces, std::string const& source_name)
{
    std::ostringstream out;
    out << "// Generated by tools/gen-bindings.cpp from " << source_name << " — do not edit by hand.\n"
        << "// Regenerate with the bindings-regen target; a test fails when this file is stale.\n\n"
        << "#include \"bindings/Internal.h\"\n\n"
        << "#include <string>\n\n"
        << "namespace sashfold::bindings::generated {\n\n";

    // The interfaces to define: every one not marked [Existing], parents
    // before children.
    std::map<std::string, Interface const*> by_name;
    for (Interface const& interface : interfaces) {
        if (by_name.contains(interface.name))
            fail(interface.line, "interface " + interface.name + " defined twice");
        by_name[interface.name] = &interface;
    }
    for (Interface const& interface : interfaces) {
        if (!interface.parent.empty() && !by_name.contains(interface.parent))
            fail(interface.line, "interface " + interface.name + " extends " + interface.parent + ", which is not declared");
    }
    std::vector<Interface const*> ordered;
    std::map<std::string, bool> placed;
    for (Interface const& interface : interfaces) {
        if (find_extended(interface.extended, "Existing"))
            placed[interface.name] = true;
    }
    // Repeatedly take every interface whose parent is placed.
    for (bool progress = true; progress;) {
        progress = false;
        for (Interface const& interface : interfaces) {
            if (placed[interface.name])
                continue;
            if (interface.parent.empty() || placed[interface.parent]) {
                ordered.push_back(&interface);
                placed[interface.name] = true;
                progress = true;
            }
        }
    }
    for (Interface const& interface : interfaces) {
        if (!placed[interface.name])
            fail(interface.line, "interface " + interface.name + " has a parent cycle");
    }

    out << "// The interfaces, each a constructor on the global whose prototype\n"
        << "// inherits its parent's, and the tag each is the interface of.\n"
        << "void install_html_element_interfaces(Realm::Internals& in)\n{\n"
        << "    js::Heap::NoCollect const guard(in.interpreter.heap());\n";
    for (Interface const* interface : ordered) {
        out << "    define_interface(in, \"" << interface->name << "\", in.prototype(\"" << interface->parent << "\"));\n";
        if (ExtendedAttribute const* tags = find_extended(interface->extended, "Tags")) {
            for (std::string const& tag : tags->values)
                out << "    in.tag_interfaces[\"" << tag << "\"] = \"" << interface->name << "\";\n";
        }
    }
    out << "}\n\n";

    out << "// The attributes reflected from content attributes (HTML §2.6.1),\n"
        << "// through the helpers the hand-written bindings use.\n"
        << "void install_reflected_attributes(Realm::Internals& in)\n{\n"
        << "    js::Heap::NoCollect const guard(in.interpreter.heap());\n";
    for (Interface const& interface : interfaces) {
        bool any = false;
        for (Attribute const& attribute : interface.attributes) {
            if (find_extended(attribute.extended, "Reflect"))
                any = true;
        }
        if (!any)
            continue;
        out << "    {\n        js::Object& proto = *in.prototype(\"" << interface.name << "\");\n";
        for (Attribute const& attribute : interface.attributes) {
            ExtendedAttribute const* reflect = find_extended(attribute.extended, "Reflect");
            if (!reflect)
                continue;
            std::string const content = reflect->values.empty() ? lowercased(attribute.name) : reflect->values.front();
            if (attribute.type == "boolean") {
                out << "        reflect_boolean(in, proto, \"" << attribute.name << "\", \"" << content << "\");\n";
            } else if (attribute.type == "long" || attribute.type == "unsigned long") {
                ExtendedAttribute const* fallback = find_extended(attribute.extended, "Default");
                std::string const value = fallback && !fallback->values.empty() ? fallback->values.front() : "0";
                out << "        reflect_long(in, proto, \"" << attribute.name << "\", \"" << content << "\", " << value << ");\n";
            } else if (attribute.type == "DOMString" || attribute.type == "USVString") {
                if (find_extended(attribute.extended, "URL"))
                    out << "        reflect_url(in, proto, \"" << attribute.name << "\", \"" << content << "\");\n";
                else
                    out << "        reflect_string(in, proto, \"" << attribute.name << "\", \"" << content << "\");\n";
            } else {
                fail(interface.line, "attribute " + interface.name + "." + attribute.name + " reflects a type the generator cannot: " + attribute.type);
            }
        }
        out << "    }\n";
    }
    out << "}\n\n}\n";
    return out.str();
}

std::optional<std::string> read_file(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4 || (std::string_view(argv[2]) != "--output" && std::string_view(argv[2]) != "--check")) {
        std::cerr << "usage: gen_bindings <in.idl> --output <out.cpp> | --check <out.cpp>\n";
        return 2;
    }
    std::string const input_path = argv[1];
    std::optional<std::string> const source = read_file(input_path);
    if (!source) {
        std::cerr << "gen-bindings: cannot read " << input_path << "\n";
        return 1;
    }
    Parser parser(*source);
    std::vector<Interface> const interfaces = parser.parse();
    std::string source_name = input_path;
    if (std::size_t const slash = source_name.find_last_of("/\\"); slash != std::string::npos)
        source_name = source_name.substr(slash + 1);
    std::string const text = generate(interfaces, "idl/" + source_name);
    std::string const output_path = argv[3];
    if (std::string_view(argv[2]) == "--check") {
        std::optional<std::string> existing = read_file(output_path);
        // A checkout on Windows may have given the file CRLF line endings;
        // the comparison is of the text, not of the line endings.
        if (existing)
            std::erase(*existing, '\r');
        if (!existing || *existing != text) {
            std::cerr << "gen-bindings: " << output_path << " is not what " << input_path
                      << " generates; run the bindings-regen target and commit the result\n";
            return 2;
        }
        std::cout << "gen-bindings: " << output_path << " is current (" << interfaces.size() << " interfaces)\n";
        return 0;
    }
    std::ofstream file(output_path, std::ios::binary);
    if (!file) {
        std::cerr << "gen-bindings: cannot write " << output_path << "\n";
        return 1;
    }
    file << text;
    std::cout << "gen-bindings: wrote " << output_path << " (" << interfaces.size() << " interfaces)\n";
    return 0;
}
