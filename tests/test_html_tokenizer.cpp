#include "Test.h"

#include "core/Unicode.h"
#include "html/Tokenizer.h"

#include <string>
#include <vector>

using namespace sashfold;
using namespace sashfold::html;

namespace {

// Runs the tokenizer and folds Character tokens into strings for compact
// assertions: C:text, S:name, E:name, D:name, M:comment, F (EOF).
std::vector<std::string> run(std::string_view input)
{
    Tokenizer tokenizer(input);
    std::vector<std::string> out;
    std::string characters;
    auto flush_characters = [&] {
        if (!characters.empty()) {
            out.push_back("C:" + characters);
            characters.clear();
        }
    };
    while (auto token = tokenizer.next_token()) {
        switch (token->type) {
        case Token::Type::Character:
            append_utf8(characters, token->code_point);
            break;
        case Token::Type::StartTag: {
            flush_characters();
            std::string entry = "S:" + token->tag_name;
            for (Attribute const& attribute : token->attributes)
                entry += " " + attribute.name + "=" + attribute.value;
            if (token->self_closing)
                entry += " /";
            out.push_back(std::move(entry));
            break;
        }
        case Token::Type::EndTag:
            flush_characters();
            out.push_back("E:" + token->tag_name);
            break;
        case Token::Type::Comment:
            flush_characters();
            out.push_back("M:" + token->data);
            break;
        case Token::Type::Doctype: {
            flush_characters();
            std::string entry = "D:" + token->doctype_name.value_or("<none>");
            if (token->force_quirks)
                entry += " quirks";
            out.push_back(std::move(entry));
            break;
        }
        case Token::Type::EndOfFile:
            flush_characters();
            out.push_back("F");
            break;
        }
    }
    return out;
}

std::vector<std::string> run_in(Tokenizer::State state, std::string last_start_tag, std::string_view input)
{
    Tokenizer tokenizer(InputStream::from_utf8(input), state, std::move(last_start_tag));
    std::vector<std::string> out;
    std::string characters;
    while (auto token = tokenizer.next_token()) {
        if (token->type == Token::Type::Character) {
            append_utf8(characters, token->code_point);
            continue;
        }
        if (!characters.empty()) {
            out.push_back("C:" + characters);
            characters.clear();
        }
        if (token->type == Token::Type::EndTag)
            out.push_back("E:" + token->tag_name);
        else if (token->type == Token::Type::StartTag)
            out.push_back("S:" + token->tag_name);
        else if (token->type == Token::Type::EndOfFile)
            out.push_back("F");
    }
    return out;
}

bool equals(std::vector<std::string> const& actual, std::vector<std::string> const& expected)
{
    if (actual == expected)
        return true;
    std::string joined_actual;
    for (auto const& entry : actual)
        joined_actual += "[" + entry + "]";
    std::string joined_expected;
    for (auto const& entry : expected)
        joined_expected += "[" + entry + "]";
    std::cerr << "  actual:   " << joined_actual << "\n  expected: " << joined_expected << "\n";
    return false;
}

}

int main()
{
    // Plain tags, attribute quoting styles, case folding.
    CHECK(equals(run("<div ID=x class=\"y z\" data-a='q'>t</DIV>"),
        { "S:div id=x class=y z data-a=q", "C:t", "E:div", "F" }));

    // Self-closing and duplicate attributes (duplicate is dropped).
    CHECK(equals(run("<br/><img a=1 A=2>"), { "S:br /", "S:img a=1", "F" }));

    // Named character references: full, legacy-without-semicolon, attribute quirk.
    CHECK(equals(run("&notin;"), { "C:\xE2\x88\x89", "F" }));
    CHECK(equals(run("&not;in"), { "C:\xC2\xACin", "F" }));
    CHECK(equals(run("&nota"), { "C:\xC2\xAC" "a", "F" }));
    CHECK(equals(run("<a b=\"&nota\">"), { "S:a b=&nota", "F" })); // historical: no replacement
    CHECK(equals(run("<a b=\"&not;a\">"), { "S:a b=\xC2\xAC" "a", "F" }));
    CHECK(equals(run("&noident"), { "C:&noident", "F" }));

    // Numeric character references, including the C1 remap and error fallbacks.
    CHECK(equals(run("&#65;&#x41;"), { "C:AA", "F" }));
    CHECK(equals(run("&#0;"), { "C:\xEF\xBF\xBD", "F" }));
    CHECK(equals(run("&#x80;"), { "C:\xE2\x82\xAC", "F" })); // -> U+20AC
    CHECK(equals(run("&#xD800;"), { "C:\xEF\xBF\xBD", "F" }));
    CHECK(equals(run("&#x110000;"), { "C:\xEF\xBF\xBD", "F" }));
    CHECK(equals(run("&#;"), { "C:&#;", "F" }));

    // Comment edge cases.
    CHECK(equals(run("<!-->"), { "M:", "F" }));
    CHECK(equals(run("<!--a--!>"), { "M:a", "F" }));
    CHECK(equals(run("<!--x--><!---->"), { "M:x", "M:", "F" }));
    CHECK(equals(run("<?pi?>"), { "M:?pi?", "F" }));

    // DOCTYPE shapes.
    CHECK(equals(run("<!DOCTYPE html>"), { "D:html", "F" }));
    CHECK(equals(run("<!doctypehtml>"), { "D:html", "F" }));
    CHECK(equals(run("<!DOCTYPE>"), { "D:<none> quirks", "F" }));

    // RCDATA: the appropriate end tag ends it, other end tags are text.
    CHECK(equals(run_in(Tokenizer::State::RCDATA, "title", "x</title>y"), { "C:x", "E:title", "C:y", "F" }));
    CHECK(equals(run_in(Tokenizer::State::RCDATA, "title", "a</b>c"), { "C:a</b>c", "F" }));

    // Script data comment-like escaping.
    CHECK(equals(run_in(Tokenizer::State::ScriptData, "script", "a<!--b--></script>c"),
        { "C:a<!--b-->", "E:script", "C:c", "F" }));

    // EOF inside a tag: the tag is abandoned, only EOF comes out.
    CHECK(equals(run("<div"), { "F" }));

    // Newline normalization.
    CHECK(equals(run("a\r\nb\rc"), { "C:a\nb\nc", "F" }));

    return sashfold::test::report("html_tokenizer");
}
