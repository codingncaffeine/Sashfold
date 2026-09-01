// Runs the html5lib tree-construction fixtures (.dat) with the same
// ratcheting-baseline contract as the tokenizer runner. Tests marked
// #script-on are skipped (scripting is off by design at this milestone).
//
// usage: html5lib_tree <fixtures-dir> <baseline-file>

#include "core/Unicode.h"
#include "html/TreeBuilder.h"
#include "html/TreeDump.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;
using namespace sashfold::html;

namespace {

struct DatTest {
    std::string data;
    std::optional<std::string> fragment_context; // e.g. "div", "svg path"
    bool script_on = false;
    std::string expected;
};

std::vector<DatTest> parse_dat(std::string const& content)
{
    std::vector<std::string> lines;
    {
        std::istringstream stream(content);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }

    std::vector<DatTest> tests;
    std::size_t i = 0;
    while (i < lines.size()) {
        if (lines[i] != "#data") {
            ++i;
            continue;
        }
        ++i;
        DatTest test;

        std::vector<std::string> data_lines;
        while (i < lines.size() && lines[i] != "#errors")
            data_lines.push_back(lines[i++]);
        for (std::size_t j = 0; j < data_lines.size(); ++j) {
            if (j)
                test.data += "\n";
            test.data += data_lines[j];
        }

        // Directive sections until #document.
        while (i < lines.size() && lines[i] != "#document") {
            if (lines[i] == "#document-fragment" && i + 1 < lines.size()) {
                test.fragment_context = lines[i + 1];
                i += 2;
                continue;
            }
            if (lines[i] == "#script-on")
                test.script_on = true;
            ++i;
        }
        if (i < lines.size())
            ++i; // consume #document

        std::vector<std::string> expected_lines;
        while (i < lines.size()) {
            // A blank line ends the tree only as the test separator (next line
            // is #data, or EOF); inside a multi-line text node it is content.
            if (lines[i].empty() && (i + 1 >= lines.size() || lines[i + 1] == "#data"))
                break;
            expected_lines.push_back(lines[i++]);
        }
        for (std::size_t j = 0; j < expected_lines.size(); ++j)
            test.expected += expected_lines[j] + "\n";

        tests.push_back(std::move(test));
    }
    return tests;
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

std::string run_test(DatTest const& test)
{
    std::u32string input = decode_utf8(test.data);
    if (test.fragment_context) {
        std::string_view context = *test.fragment_context;
        std::string_view context_namespace = dom::ns::html;
        if (context.starts_with("svg ")) {
            context_namespace = dom::ns::svg;
            context.remove_prefix(4);
        } else if (context.starts_with("math ")) {
            context_namespace = dom::ns::mathml;
            context.remove_prefix(5);
        }
        FragmentParseResult result = parse_fragment(std::move(input), context_namespace, context);
        return result.root ? dump_children(*result.root) : std::string {};
    }
    auto document = parse_document(std::move(input));
    return dump_document(*document);
}

}

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: html5lib_tree <fixtures-dir> <baseline-file>\n";
        return 2;
    }
    std::filesystem::path const fixtures_dir = argv[1];

    long baseline = 0;
    {
        std::optional<std::string> baseline_text = read_file(argv[2]);
        if (!baseline_text) {
            std::cerr << "cannot read baseline " << argv[2] << "\n";
            return 2;
        }
        std::istringstream stream(*baseline_text);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line[0] != '#') {
                baseline = std::stol(line);
                break;
            }
        }
    }

    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(fixtures_dir)) {
        if (entry.path().extension() == ".dat")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    long total_runs = 0;
    long total_pass = 0;
    long skipped_script_on = 0;
    int printed_failures = 0;
    int max_printed_failures = 8;
    if (char const* env = std::getenv("SASHFOLD_PRINT_FAILURES"))
        max_printed_failures = std::atoi(env);

    for (auto const& path : files) {
        std::optional<std::string> content = read_file(path);
        if (!content) {
            std::cerr << "cannot read " << path << "\n";
            return 2;
        }
        long file_runs = 0;
        long file_pass = 0;
        for (DatTest const& test : parse_dat(*content)) {
            if (test.script_on) {
                ++skipped_script_on;
                continue;
            }
            ++file_runs;
            std::string const actual = run_test(test);
            if (actual == test.expected) {
                ++file_pass;
            } else if (printed_failures < max_printed_failures) {
                ++printed_failures;
                std::cerr << "FAIL [" << path.filename().string() << "]\n#data\n"
                          << test.data << "\n#expected\n"
                          << test.expected << "#actual\n"
                          << actual << "\n";
            }
        }
        total_runs += file_runs;
        total_pass += file_pass;
        std::cout << path.filename().string() << ": " << file_pass << "/" << file_runs << "\n";
    }

    std::cout << "TOTAL: " << total_pass << "/" << total_runs
              << " (baseline " << baseline << ", " << skipped_script_on << " script-on skipped)\n";
    if (total_pass < baseline) {
        std::cerr << "REGRESSION: score fell below the committed baseline\n";
        return 1;
    }
    if (total_pass > baseline)
        std::cout << "RATCHET: raise the baseline to " << total_pass << "\n";
    return 0;
}
