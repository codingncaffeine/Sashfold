// The script engine scored against test262, the ECMAScript conformance
// suite: every test under the directories in tests/test262/directories.txt
// is run the way the suite's INTERPRETING.md says — the harness's assert.js
// and sta.js first, then the test's own includes, then the test, once in
// sloppy code and once in strict unless it says otherwise — and a baseline
// file lists every passing test: a listed test that fails is a regression,
// a new pass is a ratchet to bless with --update.
//
// Two honesty rules the suite does not impose but this engine does. A
// negative test expecting a SyntaxError passes only when the error is a
// real one: an error whose message says a feature is "not supported" is the
// engine declining, and a test that expects a class-syntax early error must
// not score because the engine cannot parse classes at all. And every test
// in a listed directory counts, including the ones that need modules, async
// functions or features not written yet; what the engine cannot do, it does
// not score.

#include "js/Interpreter.h"
#include "js/Object.h"
#include "js/Strings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace sashfold;

namespace {

std::optional<std::string> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string trimmed(std::string_view text)
{
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
        ++start;
    std::size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
        --end;
    return std::string(text.substr(start, end - start));
}

std::vector<std::string> read_lines(std::filesystem::path const& path)
{
    std::vector<std::string> lines;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::string const clean = trimmed(line);
        if (!clean.empty() && clean[0] != '#')
            lines.push_back(clean);
    }
    return lines;
}

// A YAML flow list "[a, b]" or a single bare value, split on commas.
std::vector<std::string> list_items(std::string_view text)
{
    std::string body = trimmed(text);
    if (!body.empty() && body.front() == '[' && body.back() == ']')
        body = body.substr(1, body.size() - 2);
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= body.size()) {
        std::size_t const comma = body.find(',', start);
        std::string item = trimmed(body.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty()) {
            if ((item.front() == '"' && item.back() == '"') || (item.front() == '\'' && item.back() == '\''))
                item = item.substr(1, item.size() - 2);
            items.push_back(item);
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return items;
}

struct Metadata {
    std::vector<std::string> includes;
    std::vector<std::string> features;
    bool only_strict = false;
    bool no_strict = false;
    bool raw = false;
    bool module = false;
    bool async = false;
    bool can_block_is_false = false;
    bool can_block_is_true = false;
    std::string negative_phase; // "parse", "resolution", "runtime", or ""
    std::string negative_type; // "SyntaxError", "TypeError", …
};

// The front matter between /*--- and ---*/: the YAML subset the suite
// uses (flat keys, flow and block lists, one nested mapping for negative,
// and block scalars for the prose keys, which are skipped).
Metadata parse_metadata(std::string const& source)
{
    Metadata meta;
    std::size_t const open = source.find("/*---");
    if (open == std::string::npos)
        return meta;
    std::size_t const close = source.find("---*/", open);
    if (close == std::string::npos)
        return meta;
    std::istringstream block(source.substr(open + 5, close - open - 5));
    std::string line;
    std::string key;
    auto const apply_list = [&](std::string const& into, std::vector<std::string> const& items) {
        if (into == "includes")
            meta.includes.insert(meta.includes.end(), items.begin(), items.end());
        else if (into == "features")
            meta.features.insert(meta.features.end(), items.begin(), items.end());
        else if (into == "flags") {
            for (std::string const& flag : items) {
                if (flag == "onlyStrict")
                    meta.only_strict = true;
                else if (flag == "noStrict")
                    meta.no_strict = true;
                else if (flag == "raw")
                    meta.raw = true;
                else if (flag == "module")
                    meta.module = true;
                else if (flag == "async")
                    meta.async = true;
                else if (flag == "CanBlockIsFalse")
                    meta.can_block_is_false = true;
                else if (flag == "CanBlockIsTrue")
                    meta.can_block_is_true = true;
            }
        }
    };
    while (std::getline(block, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (trimmed(line).empty())
            continue;
        bool const indented = line[0] == ' ' || line[0] == '\t';
        if (!indented) {
            std::size_t const colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            key = trimmed(line.substr(0, colon));
            std::string const rest = trimmed(line.substr(colon + 1));
            if (rest.empty() || rest == "|" || rest == ">" || rest == "|-" || rest == ">-")
                continue; // a block follows
            apply_list(key, list_items(rest));
            continue;
        }
        std::string const item = trimmed(line);
        if (key == "negative") {
            std::size_t const colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            std::string const sub = trimmed(item.substr(0, colon));
            std::string const value = trimmed(item.substr(colon + 1));
            if (sub == "phase")
                meta.negative_phase = value;
            else if (sub == "type")
                meta.negative_type = value;
        } else if (item.size() > 1 && item[0] == '-' && (key == "includes" || key == "features" || key == "flags")) {
            apply_list(key, list_items(item.substr(1)));
        }
    }
    return meta;
}

std::string error_name(std::string const& described)
{
    std::size_t const colon = described.find(':');
    return colon == std::string::npos ? described : described.substr(0, colon);
}

enum class Mode { Sloppy, Strict, Raw };

struct RunResult {
    bool pass = false;
    std::string reason;
};

// One test in one mode: a fresh realm, the harness, the test.
RunResult run_one(std::filesystem::path const& root, std::string const& source, Metadata const& meta, Mode mode,
    int timeout_ms)
{
    js::Interpreter interpreter;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    interpreter.set_interrupt([deadline] { return std::chrono::steady_clock::now() > deadline; });

    // The host hooks INTERPRETING.md asks for: print, and the $262 object.
    auto printed = std::make_shared<std::string>();
    {
        js::Interpreter::Roots roots(interpreter);
        js::Object* global = interpreter.global();
        js::NativeFunction* print = interpreter.new_native("print", 1,
            [printed](js::Interpreter& in, js::Value const&, std::span<js::Value const> arguments) -> std::optional<js::Value> {
                std::optional<js::JsString*> text = in.to_string(arguments.empty() ? js::Value::undefined() : arguments[0]);
                if (!text)
                    return std::nullopt;
                *printed += (*text)->to_utf8();
                *printed += '\n';
                return js::Value::undefined();
            });
        interpreter.root(js::Value::object(print));
        global->put(interpreter.key("print"), js::Value::object(print), js::builtin_attributes);
        js::Object* host = interpreter.new_object();
        interpreter.root(js::Value::object(host));
        host->put(interpreter.key("global"), js::Value::object(global), js::builtin_attributes);
        js::NativeFunction* gc = interpreter.new_native("gc", 0,
            [](js::Interpreter& in, js::Value const&, std::span<js::Value const>) -> std::optional<js::Value> {
                in.heap().collect();
                return js::Value::undefined();
            });
        host->put(interpreter.key("gc"), js::Value::object(gc), js::builtin_attributes);
        js::NativeFunction* eval_script = interpreter.new_native("evalScript", 1,
            [](js::Interpreter& in, js::Value const&, std::span<js::Value const> arguments) -> std::optional<js::Value> {
                std::optional<js::JsString*> text = in.to_string(arguments.empty() ? js::Value::undefined() : arguments[0]);
                if (!text)
                    return std::nullopt;
                js::Outcome outcome = in.run_script((*text)->view(), "evalScript");
                if (!outcome.ok)
                    return in.throw_value(outcome.value);
                return outcome.value;
            });
        host->put(interpreter.key("evalScript"), js::Value::object(eval_script), js::builtin_attributes);
        global->put(interpreter.key("$262"), js::Value::object(host), js::builtin_attributes);
    }

    if (mode != Mode::Raw) {
        std::vector<std::string> harness { "assert.js", "sta.js" };
        if (meta.async)
            harness.push_back("doneprintHandle.js");
        harness.insert(harness.end(), meta.includes.begin(), meta.includes.end());
        for (std::string const& name : harness) {
            std::optional<std::string> const text = read_file(root / "harness" / name);
            if (!text)
                return { false, "harness file missing: " + name };
            js::Outcome const outcome = interpreter.run_script(*text, "harness/" + name);
            if (!outcome.ok)
                return { false, "harness " + name + " failed: " + interpreter.describe(outcome.value) };
        }
    }

    std::string program = mode == Mode::Strict ? "\"use strict\";\n" + source : source;
    js::Outcome const outcome = interpreter.run_script(program, "test");
    if (interpreter.terminated())
        return { false, "timeout" };

    if (!meta.negative_type.empty()) {
        if (outcome.ok)
            return { false, "expected " + meta.negative_type + " (" + meta.negative_phase + "), ran without error" };
        std::string const described = interpreter.describe(outcome.value);
        if (error_name(described) != meta.negative_type)
            return { false, "expected " + meta.negative_type + ", got " + described };
        if (described.find("not supported") != std::string::npos)
            return { false, "the expected error came from an unsupported feature: " + described };
        return { true, "" };
    }
    if (!outcome.ok)
        return { false, interpreter.describe(outcome.value) };
    if (meta.async) {
        if (printed->find("Test262:AsyncTestComplete") == std::string::npos)
            return { false, "async test did not complete: " + trimmed(*printed) };
        if (printed->find("Test262:AsyncTestFailure") != std::string::npos)
            return { false, trimmed(*printed) };
    }
    return { true, "" };
}

struct Test {
    std::string rel;
    std::size_t directory = 0;
};

struct DirectoryScore {
    std::string path;
    long passed = 0;
    long total = 0;
};

std::string json_escaped(std::string const& text)
{
    std::string out;
    for (char const c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

std::string html_escaped(std::string const& text)
{
    std::string out;
    for (char const c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

std::string today()
{
    std::time_t const now = std::time(nullptr);
    char buffer[16] = {};
    if (std::strftime(buffer, sizeof buffer, "%Y-%m-%d", std::gmtime(&now)) == 0)
        return "";
    return buffer;
}

std::string percent_of(long passed, long total)
{
    if (total == 0)
        return "0.0%";
    char buffer[16] = {};
    std::snprintf(buffer, sizeof buffer, "%.1f%%", 100.0 * static_cast<double>(passed) / static_cast<double>(total));
    return buffer;
}

void write_json(std::filesystem::path const& path, std::vector<DirectoryScore> const& scores, long passed, long total,
    std::string const& revision)
{
    std::ofstream out(path, std::ios::binary);
    out << "{\n  \"revision\": \"" << json_escaped(revision) << "\",\n  \"date\": \"" << today() << "\",\n  \"passed\": "
        << passed << ",\n  \"total\": " << total << ",\n  \"directories\": [\n";
    for (std::size_t i = 0; i < scores.size(); ++i) {
        out << "    { \"path\": \"" << json_escaped(scores[i].path) << "\", \"passed\": " << scores[i].passed
            << ", \"total\": " << scores[i].total << " }" << (i + 1 < scores.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

// The public page, in the landing page's manner, no scripts.
void write_html(std::filesystem::path const& path, std::vector<DirectoryScore> const& scores, long passed, long total,
    std::string const& revision)
{
    std::ofstream out(path, std::ios::binary);
    out << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>Sashfold — test262</title>\n"
           "<meta name=\"description\" content=\"Sashfold's pass rate on test262, the ECMAScript conformance suite, by directory.\">\n"
           "<link rel=\"icon\" type=\"image/png\" href=\"icon-160.png\">\n"
           "<link rel=\"canonical\" href=\"https://sashfold.com/test262.html\">\n"
           "<style>\n"
           "  :root { --bg: #f7f9fc; --panel: #ffffff; --ink: #16233c; --muted: #4e5d78; --navy: #16305e;\n"
           "          --blue: #1e63c4; --sky: #37b6e8; --accent: #f49c3c; --line: #d8e1ee; --code-bg: #eef3fa; }\n"
           "  @media (prefers-color-scheme: dark) {\n"
           "    :root { --bg: #0d1524; --panel: #131e33; --ink: #e6edf7; --muted: #9fb0c9; --navy: #a8c4ef;\n"
           "            --blue: #5b9bea; --sky: #4cc3ef; --accent: #f5aa55; --line: #24334f; --code-bg: #1a2740; }\n"
           "  }\n"
           "  * { box-sizing: border-box; }\n"
           "  body { margin: 0; background: var(--bg); color: var(--ink);\n"
           "         font: 17px/1.65 system-ui, \"Segoe UI\", Roboto, Helvetica, Arial, sans-serif; }\n"
           "  main { max-width: 880px; margin: 0 auto; padding: 0 20px 64px; }\n"
           "  a { color: var(--blue); }\n"
           "  a:hover { color: var(--sky); }\n"
           "  h1, h2 { color: var(--navy); line-height: 1.2; }\n"
           "  h1 { margin-top: 48px; }\n"
           "  code { font-family: ui-monospace, \"Cascadia Code\", Consolas, monospace; background: var(--code-bg);\n"
           "         padding: 1px 6px; border-radius: 4px; font-size: 0.92em; }\n"
           "  .stat { display: inline-block; background: var(--panel); border: 1px solid var(--line);\n"
           "          border-radius: 10px; padding: 14px 20px; margin: 8px 0 20px; }\n"
           "  .stat .big { font-size: 1.8rem; font-weight: 700; color: var(--blue); display: block; }\n"
           "  .stat .what { color: var(--muted); font-size: 0.92rem; }\n"
           "  table { border-collapse: collapse; width: 100%; margin: 20px 0; }\n"
           "  th, td { text-align: left; padding: 8px 10px; border-bottom: 1px solid var(--line); }\n"
           "  th { color: var(--muted); font-weight: 600; font-size: 0.92rem; }\n"
           "  td.num { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }\n"
           "  .bar { background: var(--code-bg); border-radius: 4px; height: 10px; width: 100%; min-width: 120px; }\n"
           "  .bar span { display: block; height: 10px; border-radius: 4px; background: var(--blue); }\n"
           "  p.muted { color: var(--muted); font-size: 0.95rem; }\n"
           "</style>\n</head>\n<body>\n<main>\n"
           "<p><a href=\"./\">← sashfold.com</a></p>\n"
           "<h1>test262</h1>\n"
           "<p>Sashfold's pass rate on <a href=\"https://github.com/tc39/test262\">test262</a>, the ECMAScript "
           "conformance suite, run the way the suite's own instructions say: the harness first, then the test, once "
           "as sloppy code and once as strict unless the test says otherwise. Every test under each listed directory "
           "counts, including the ones that need modules, async functions or features not written yet. A test that "
           "expects a syntax error scores only when the engine raised a real one, never when it merely declined a "
           "feature it does not have.</p>\n";
    out << "<div class=\"stat\"><span class=\"big\">" << passed << " / " << total << "</span><span class=\"what\">test262 tests passing — "
        << percent_of(passed, total) << "</span></div>\n";
    out << "<table>\n<thead><tr><th>Directory</th><th></th><th>Passing</th><th>Rate</th></tr></thead>\n<tbody>\n";
    for (DirectoryScore const& score : scores) {
        int const width = score.total == 0 ? 0 : static_cast<int>(100 * score.passed / score.total);
        out << "<tr><td><code>" << html_escaped(score.path) << "</code></td><td><div class=\"bar\"><span style=\"width: "
            << width << "%\"></span></div></td><td class=\"num\">" << score.passed << " / " << score.total
            << "</td><td class=\"num\">" << percent_of(score.passed, score.total) << "</td></tr>\n";
    }
    out << "</tbody>\n</table>\n";
    out << "<p class=\"muted\">Scored " << today() << " against test262 revision <code>" << html_escaped(revision.substr(0, 12))
        << "</code>. The number is enforced in CI: a baseline names every passing test, and a test that stops "
           "passing fails the build. What the engine cannot do, it does not score.</p>\n"
           "</main>\n</body>\n</html>\n";
}

void usage(char const* program)
{
    std::cerr << "usage: " << program
              << " <test262-checkout> <directories-file> <baseline-file> [--update] [--only <text>]\n"
                 "       [--json <file>] [--html <file>] [--revision <file>] [--print <n>] [--jobs <n>] [--timeout <ms>]\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    std::filesystem::path const root = argv[1];
    std::filesystem::path const directories_file = argv[2];
    std::filesystem::path const baseline_file = argv[3];
    bool update = false;
    std::string only;
    std::string json_path;
    std::string html_path;
    std::string revision_file;
    int max_printed = 20;
    int jobs = 4;
    int timeout_ms = 10000;
    if (char const* env = std::getenv("SASHFOLD_PRINT_FAILURES"))
        max_printed = std::atoi(env);
    for (int i = 4; i < argc; ++i) {
        std::string const arg = argv[i];
        auto const value = [&](std::string& into) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                std::exit(2);
            }
            into = argv[++i];
        };
        std::string text;
        if (arg == "--update")
            update = true;
        else if (arg == "--only")
            value(only);
        else if (arg == "--json")
            value(json_path);
        else if (arg == "--html")
            value(html_path);
        else if (arg == "--revision")
            value(revision_file);
        else if (arg == "--print") {
            value(text);
            max_printed = std::atoi(text.c_str());
        } else if (arg == "--jobs") {
            value(text);
            jobs = std::max(1, std::atoi(text.c_str()));
        } else if (arg == "--timeout") {
            value(text);
            timeout_ms = std::max(100, std::atoi(text.c_str()));
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    std::vector<std::string> directories;
    std::vector<std::string> excluded;
    for (std::string const& line : read_lines(directories_file)) {
        if (line[0] == '-')
            excluded.push_back(trimmed(line.substr(1)));
        else
            directories.push_back(line);
    }
    if (directories.empty()) {
        std::cerr << "no directories listed in " << directories_file << "\n";
        return 2;
    }
    std::string revision;
    if (!revision_file.empty()) {
        std::vector<std::string> const lines = read_lines(revision_file);
        if (!lines.empty())
            revision = lines.front();
    }

    auto const started = std::chrono::steady_clock::now();
    std::vector<Test> tests;
    for (std::size_t d = 0; d < directories.size(); ++d) {
        std::filesystem::path const directory = root / directories[d];
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) {
            std::cerr << "not a directory: " << directory.string() << " (run tools/test262-fetch.sh)\n";
            return 2;
        }
        for (auto const& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".js")
                continue;
            std::string const rel = entry.path().lexically_relative(root).generic_string();
            if (rel.ends_with("_FIXTURE.js"))
                continue;
            if (std::any_of(excluded.begin(), excluded.end(), [&](std::string const& prefix) { return rel.starts_with(prefix); }))
                continue;
            if (!only.empty() && rel.find(only) == std::string::npos)
                continue;
            tests.push_back(Test { rel, d });
        }
    }
    std::sort(tests.begin(), tests.end(), [](Test const& a, Test const& b) { return a.rel < b.rel; });
    if (tests.empty()) {
        std::cerr << "no tests found\n";
        return 2;
    }
    auto const discovered = std::chrono::steady_clock::now();
    std::cout << tests.size() << " tests under " << directories.size() << " directories ("
              << static_cast<long>(std::chrono::duration<double>(discovered - started).count()) << " s to find)\n";

    // Every test in a fresh realm; the realms are independent, so the
    // tests run on `jobs` threads. Results land by index and are read
    // back in order.
    std::vector<RunResult> results(tests.size());
    std::atomic<std::size_t> next { 0 };
    std::atomic<std::size_t> done { 0 };
    auto const worker = [&] {
        for (;;) {
            std::size_t const i = next.fetch_add(1);
            if (i >= tests.size())
                return;
            std::optional<std::string> const source = read_file(root / tests[i].rel);
            if (!source) {
                results[i] = { false, "unreadable" };
                continue;
            }
            Metadata const meta = parse_metadata(*source);
            RunResult result;
            if (meta.module) {
                result = { false, "module code is not supported" };
            } else {
                std::vector<Mode> modes;
                if (meta.raw)
                    modes.push_back(Mode::Raw);
                else if (meta.only_strict)
                    modes.push_back(Mode::Strict);
                else if (meta.no_strict)
                    modes.push_back(Mode::Sloppy);
                else {
                    modes.push_back(Mode::Sloppy);
                    modes.push_back(Mode::Strict);
                }
                result = { true, "" };
                for (Mode const mode : modes) {
                    RunResult const one = run_one(root, *source, meta, mode, timeout_ms);
                    if (!one.pass) {
                        result = one;
                        if (mode == Mode::Strict)
                            result.reason = "(strict) " + result.reason;
                        break;
                    }
                }
            }
            results[i] = std::move(result);
            std::size_t const finished = done.fetch_add(1) + 1;
            if (finished % 5000 == 0)
                std::cerr << "  " << finished << " / " << tests.size() << "\n";
        }
    };
    std::vector<std::thread> threads;
    for (int t = 1; t < jobs; ++t)
        threads.emplace_back(worker);
    worker();
    for (std::thread& thread : threads)
        thread.join();
    auto const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - discovered).count();

    std::vector<DirectoryScore> scores;
    for (std::string const& directory : directories)
        scores.push_back(DirectoryScore { directory, 0, 0 });
    std::set<std::string> passing;
    std::vector<std::pair<std::string, std::string>> failures;
    for (std::size_t i = 0; i < tests.size(); ++i) {
        ++scores[tests[i].directory].total;
        if (results[i].pass) {
            ++scores[tests[i].directory].passed;
            passing.insert(tests[i].rel);
        } else {
            failures.emplace_back(tests[i].rel, results[i].reason);
        }
    }

    long passed = 0;
    long total = 0;
    for (DirectoryScore const& score : scores) {
        std::printf("  %-44s %6ld / %6ld  %6s\n", score.path.c_str(), score.passed, score.total,
            percent_of(score.passed, score.total).c_str());
        passed += score.passed;
        total += score.total;
    }
    std::printf("  %-44s %6ld / %6ld  %6s\n", "TOTAL", passed, total, percent_of(passed, total).c_str());
    std::printf("%zu tests in %.1f s on %d thread(s)\n", tests.size(), elapsed, jobs);

    int printed = 0;
    for (auto const& [rel, reason] : failures) {
        if (printed++ >= max_printed)
            break;
        std::cout << "FAIL " << rel << ": " << reason << "\n";
    }
    if (printed < static_cast<int>(failures.size()))
        std::cout << "(" << failures.size() - static_cast<std::size_t>(printed)
                  << " more failures; SASHFOLD_PRINT_FAILURES=n or --print n shows them)\n";

    if (!json_path.empty())
        write_json(json_path, scores, passed, total, revision);
    if (!html_path.empty())
        write_html(html_path, scores, passed, total, revision);

    std::set<std::string> const baseline = [&] {
        std::vector<std::string> const lines = read_lines(baseline_file);
        return std::set<std::string>(lines.begin(), lines.end());
    }();
    std::set<std::string> ran;
    for (Test const& test : tests)
        ran.insert(test.rel);
    std::vector<std::string> regressions;
    std::vector<std::string> new_passes;
    for (std::string const& rel : baseline) {
        if (ran.contains(rel) && !passing.contains(rel))
            regressions.push_back(rel);
    }
    for (std::string const& rel : passing) {
        if (!baseline.contains(rel))
            new_passes.push_back(rel);
    }
    long gone = 0;
    for (std::string const& rel : baseline) {
        if (!ran.contains(rel))
            ++gone;
    }

    if (update) {
        if (!only.empty()) {
            std::cerr << "--update needs the whole run, not --only\n";
            return 2;
        }
        std::ofstream out(baseline_file, std::ios::binary);
        out << "# test262 tests passing (" << passed << " of " << total << ") at test262 revision "
            << (revision.empty() ? std::string("(unrecorded)") : revision) << ".\n"
            << "# One test per line; a listed test that fails is a regression.\n"
            << "# Re-bless with: test262_runner <checkout> <directories> <this file> --update\n";
        for (std::string const& rel : passing)
            out << rel << "\n";
        std::cout << "blessed " << passing.size() << " passing tests into " << baseline_file.string() << "\n";
        return 0;
    }

    if (!only.empty()) {
        std::cout << "(partial run: the baseline is not enforced)\n";
        return 0;
    }
    if (!regressions.empty()) {
        std::cerr << "REGRESSION: " << regressions.size() << " test(s) in the baseline no longer pass:\n";
        for (std::string const& rel : regressions)
            std::cerr << "  " << rel << "\n";
        return 1;
    }
    if (gone > 0)
        std::cout << gone << " baseline test(s) are no longer in the checkout (a different revision?)\n";
    if (!new_passes.empty())
        std::cout << "RATCHET: " << new_passes.size() << " new pass(es) — bless them with --update\n";
    std::cout << "baseline held: " << (baseline.size() - static_cast<std::size_t>(gone)) << " tests still pass\n";
    return 0;
}
