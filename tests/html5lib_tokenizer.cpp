// Runs the html5lib tokenizer conformance fixtures and holds the score to a
// committed baseline. The baseline may only ratchet upward: a run below it
// fails, a run above it says so, and the number in the README comes from here.
//
// usage: html5lib_tokenizer <fixtures-dir> <baseline-file>

#include "core/Json.h"
#include "core/Unicode.h"
#include "html/InputStream.h"
#include "html/Tokenizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;
using namespace sashfold::html;

namespace {

struct Item {
    enum class Kind { Doctype, StartTag, EndTag, Comment, Characters };
    Kind kind = Kind::Characters;
    std::string name; // tag name
    std::map<std::string, std::string> attributes;
    bool self_closing = false;
    std::optional<std::string> doctype_name;
    std::optional<std::string> public_identifier;
    std::optional<std::string> system_identifier;
    bool correctness = true; // == !force_quirks
    std::string text; // characters / comment data (WTF-8)

    bool operator==(Item const&) const = default;
};

std::string describe(Item const& item)
{
    switch (item.kind) {
    case Item::Kind::Doctype: {
        std::string out = "DOCTYPE(" + item.doctype_name.value_or("~");
        out += "," + item.public_identifier.value_or("~");
        out += "," + item.system_identifier.value_or("~");
        out += item.correctness ? ",ok)" : ",quirks)";
        return out;
    }
    case Item::Kind::StartTag: {
        std::string out = "Start(" + item.name;
        for (auto const& [key, value] : item.attributes)
            out += " " + key + "=" + value;
        if (item.self_closing)
            out += " /";
        return out + ")";
    }
    case Item::Kind::EndTag:
        return "End(" + item.name + ")";
    case Item::Kind::Comment:
        return "Comment(" + item.text + ")";
    case Item::Kind::Characters:
        return "Chars(" + item.text + ")";
    }
    return "?";
}

std::string describe(std::vector<Item> const& items)
{
    std::string out;
    for (Item const& item : items)
        out += describe(item) + " ";
    return out;
}

// Second unescape pass for doubleEscaped fixtures: literal \uXXXX sequences
// become code points; a valid surrogate pair combines, a lone one stays.
std::u32string double_unescape(std::u32string const& in)
{
    auto hex4_at = [&](std::size_t at) -> std::optional<char32_t> {
        if (at + 6 > in.size() || in[at] != U'\\' || in[at + 1] != U'u')
            return std::nullopt;
        char32_t value = 0;
        for (std::size_t i = at + 2; i < at + 6; ++i) {
            char32_t const c = in[i];
            bool const is_hex = (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F');
            if (!is_hex)
                return std::nullopt;
            char32_t digit = 0;
            if (c <= U'9')
                digit = c - U'0';
            else if (c <= U'F')
                digit = c - U'A' + 10;
            else
                digit = c - U'a' + 10;
            value = value * 16 + digit;
        }
        return value;
    };

    std::u32string out;
    std::size_t i = 0;
    while (i < in.size()) {
        std::optional<char32_t> unit = hex4_at(i);
        if (!unit) {
            out.push_back(in[i]);
            ++i;
            continue;
        }
        i += 6;
        char32_t code_point = *unit;
        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            std::optional<char32_t> low = hex4_at(i);
            if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
                code_point = 0x10000 + ((code_point - 0xD800) << 10) + (*low - 0xDC00);
                i += 6;
            }
        }
        out.push_back(code_point);
    }
    return out;
}

std::string convert_expected_string(std::string const& utf8, bool double_escaped)
{
    if (!double_escaped)
        return utf8;
    return to_utf8(double_unescape(decode_utf8(utf8, true)));
}

std::optional<std::vector<Item>> build_expected(JsonValue const& output, bool double_escaped)
{
    std::vector<Item> items;
    for (JsonValue const& entry : output.as_array()) {
        if (!entry.is_array() || entry.as_array().empty() || !entry.as_array()[0].is_string())
            return std::nullopt;
        auto const& row = entry.as_array();
        std::string const& kind = row[0].as_string();

        if (kind == "Character") {
            std::string const text = convert_expected_string(row[1].as_string(), double_escaped);
            if (!items.empty() && items.back().kind == Item::Kind::Characters)
                items.back().text += text;
            else {
                Item item;
                item.kind = Item::Kind::Characters;
                item.text = text;
                items.push_back(std::move(item));
            }
        } else if (kind == "Comment") {
            Item item;
            item.kind = Item::Kind::Comment;
            item.text = convert_expected_string(row[1].as_string(), double_escaped);
            items.push_back(std::move(item));
        } else if (kind == "StartTag") {
            Item item;
            item.kind = Item::Kind::StartTag;
            item.name = convert_expected_string(row[1].as_string(), double_escaped);
            for (auto const& [attr_name, attr_value] : row[2].as_object())
                item.attributes[convert_expected_string(attr_name, double_escaped)]
                    = convert_expected_string(attr_value.as_string(), double_escaped);
            if (row.size() > 3 && row[3].is_bool())
                item.self_closing = row[3].as_bool();
            items.push_back(std::move(item));
        } else if (kind == "EndTag") {
            Item item;
            item.kind = Item::Kind::EndTag;
            item.name = convert_expected_string(row[1].as_string(), double_escaped);
            items.push_back(std::move(item));
        } else if (kind == "DOCTYPE") {
            Item item;
            item.kind = Item::Kind::Doctype;
            if (!row[1].is_null())
                item.doctype_name = convert_expected_string(row[1].as_string(), double_escaped);
            if (!row[2].is_null())
                item.public_identifier = convert_expected_string(row[2].as_string(), double_escaped);
            if (!row[3].is_null())
                item.system_identifier = convert_expected_string(row[3].as_string(), double_escaped);
            item.correctness = row[4].as_bool();
            items.push_back(std::move(item));
        } else {
            return std::nullopt;
        }
    }
    return items;
}

std::vector<Item> run_tokenizer(std::u32string const& input, Tokenizer::State state, std::string last_start_tag)
{
    Tokenizer tokenizer(InputStream(input), state, std::move(last_start_tag));
    std::vector<Item> items;
    while (auto token = tokenizer.next_token()) {
        switch (token->type) {
        case Token::Type::Character:
            if (items.empty() || items.back().kind != Item::Kind::Characters) {
                Item item;
                item.kind = Item::Kind::Characters;
                items.push_back(std::move(item));
            }
            append_utf8(items.back().text, token->code_point);
            break;
        case Token::Type::StartTag: {
            Item item;
            item.kind = Item::Kind::StartTag;
            item.name = token->tag_name;
            for (Attribute const& attribute : token->attributes)
                item.attributes[attribute.name] = attribute.value;
            item.self_closing = token->self_closing;
            items.push_back(std::move(item));
            break;
        }
        case Token::Type::EndTag: {
            Item item;
            item.kind = Item::Kind::EndTag;
            item.name = token->tag_name;
            items.push_back(std::move(item));
            break;
        }
        case Token::Type::Comment: {
            Item item;
            item.kind = Item::Kind::Comment;
            item.text = token->data;
            items.push_back(std::move(item));
            break;
        }
        case Token::Type::Doctype: {
            Item item;
            item.kind = Item::Kind::Doctype;
            item.doctype_name = token->doctype_name;
            item.public_identifier = token->public_identifier;
            item.system_identifier = token->system_identifier;
            item.correctness = !token->force_quirks;
            items.push_back(std::move(item));
            break;
        }
        case Token::Type::EndOfFile:
            break;
        }
    }
    return items;
}

std::optional<std::string> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    return std::move(stream).str();
}

}

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: html5lib_tokenizer <fixtures-dir> <baseline-file>\n";
        return 2;
    }
    std::filesystem::path const fixtures_dir = argv[1];
    std::filesystem::path const baseline_path = argv[2];

    std::optional<std::string> baseline_text = read_file(baseline_path);
    if (!baseline_text) {
        std::cerr << "cannot read baseline " << baseline_path << "\n";
        return 2;
    }
    long baseline = 0;
    {
        std::istringstream stream(*baseline_text);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line[0] != '#') {
                baseline = std::stol(line);
                break;
            }
        }
    }

    long total_runs = 0;
    long total_pass = 0;
    int printed_failures = 0;
    int constexpr max_printed_failures = 12;

    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(fixtures_dir)) {
        if (entry.path().extension() == ".test" && entry.path().filename() != "xmlViolation.test")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (auto const& path : files) {
        std::optional<std::string> text = read_file(path);
        if (!text) {
            std::cerr << "cannot read " << path << "\n";
            return 2;
        }
        std::optional<JsonValue> document = JsonValue::parse(*text);
        if (!document || !document->is_object()) {
            std::cerr << "cannot parse " << path << "\n";
            return 2;
        }
        JsonValue const* tests = document->get("tests");
        if (!tests || !tests->is_array())
            continue;

        long file_runs = 0;
        long file_pass = 0;

        for (JsonValue const& test : tests->as_array()) {
            JsonValue const* input_value = test.get("input");
            JsonValue const* output_value = test.get("output");
            if (!input_value || !output_value || !output_value->is_array())
                continue;

            bool const double_escaped = test.get("doubleEscaped") && test.get("doubleEscaped")->as_bool();

            std::u32string input = decode_utf8(input_value->as_string(), true);
            if (double_escaped)
                input = double_unescape(input);

            std::string last_start_tag;
            if (JsonValue const* last = test.get("lastStartTag"); last && last->is_string())
                last_start_tag = last->as_string();

            std::vector<Tokenizer::State> states;
            if (JsonValue const* initial = test.get("initialStates"); initial && initial->is_array()) {
                for (JsonValue const& name : initial->as_array()) {
                    if (auto state = Tokenizer::state_from_test_name(name.as_string()))
                        states.push_back(*state);
                }
            }
            if (states.empty())
                states.push_back(Tokenizer::State::Data);

            std::optional<std::vector<Item>> expected = build_expected(*output_value, double_escaped);
            if (!expected)
                continue;

            for (Tokenizer::State state : states) {
                ++file_runs;
                std::vector<Item> const actual = run_tokenizer(input, state, last_start_tag);
                if (actual == *expected) {
                    ++file_pass;
                } else if (printed_failures < max_printed_failures) {
                    ++printed_failures;
                    JsonValue const* description = test.get("description");
                    std::cerr << "FAIL [" << path.filename().string() << "] "
                              << (description && description->is_string() ? description->as_string() : "?") << "\n"
                              << "  expected: " << describe(*expected) << "\n"
                              << "  actual:   " << describe(actual) << "\n";
                }
            }
        }

        total_runs += file_runs;
        total_pass += file_pass;
        std::cout << path.filename().string() << ": " << file_pass << "/" << file_runs << "\n";
    }

    std::cout << "TOTAL: " << total_pass << "/" << total_runs
              << " (baseline " << baseline << ")\n";
    if (total_pass < baseline) {
        std::cerr << "REGRESSION: score fell below the committed baseline\n";
        return 1;
    }
    if (total_pass > baseline)
        std::cout << "RATCHET: raise the baseline to " << total_pass << "\n";
    return 0;
}
