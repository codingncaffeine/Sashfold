// js_probe runs a script on the engine under heap stress and shows what
// the engine made of it: the completion or the thrown value after the job
// queue has drained, the syntax tree (--dump-ast), the scope tree with every
// binding and every reference as the parser resolved them (--dump-scopes)
// and the bytecode of every function body the bytecode tier compiled
// (--dump-bytecode). A build tool for the script engine's own work, never
// shipped.
//
//   js_probe "<source>" [--module] [--dump-ast] [--dump-scopes] [--dump-bytecode] [--no-stress] [--vm-profile]
//
// The source is one argument; a file arrives as "$(cat page.js)". With
// --no-stress the heap collects as it does in a page, which is how a
// script is timed here against another engine; --vm-profile prints where the
// running went (the run loop, the natives) and the instructions run most. With
// --module it is parsed under the Module goal and evaluated as a module,
// its imports read as files named by their specifiers relative to the
// working directory. The dumps show a function whose body lazy parsing let
// go of as `lazy` (SASHFOLD_LAZY=0 parses every body at once). Exit
// status: 0 when the script completed (a module: its evaluation promise
// fulfilled), 1 when it threw (a module: the promise rejected), 2 for a
// syntax error or a usage error.
#include "js/Bytecode.h"
#include "js/Evaluator.h"
#include "js/Interpreter.h"
#include "js/Module.h"
#include "js/Object.h"
#include "js/Parser.h"
#include "js/Strings.h"
#include "platform/Memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sashfold;

namespace {

int usage()
{
    std::fputs("usage: js_probe \"<source>\" [--module] [--dump-ast] [--dump-scopes] [--dump-bytecode]\n", stderr);
    return 2;
}

}

// The run loop's profile: the interpreter's own account of it, and the
// instructions executed most, by opcode, when the build counts them.
void print_profile(js::Interpreter& in)
{
    std::fputs(in.profile_text(15).c_str(), stdout);
    std::uint64_t const hits = in.cache_hits();
    std::uint64_t const misses = in.cache_misses();
    std::printf("  inline caches: ic_hits %llu, ic_misses %llu (%.1f%% hit)\n", static_cast<unsigned long long>(hits),
        static_cast<unsigned long long>(misses), hits + misses ? 100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses) : 0.0);
    js::Interpreter::Account const& account = in.account();
    std::uint64_t total = 0;
    for (std::uint64_t count : account.executed)
        total += count;
    if (account.executed.empty())
        std::printf("  (instruction counts: a build made with -DSASHFOLD_VM_COUNTS=ON)\n");
    std::vector<std::pair<std::uint64_t, std::size_t>> ranked;
    for (std::size_t op = 0; op < account.executed.size(); ++op) {
        if (account.executed[op] != 0)
            ranked.emplace_back(account.executed[op], op);
    }
    std::sort(ranked.rbegin(), ranked.rend());
    for (std::size_t i = 0; i < ranked.size() && i < 25; ++i) {
        std::printf("  %-26s %12llu  %5.1f%%\n", js::opcode_name(static_cast<js::Opcode>(ranked[i].second)),
            static_cast<unsigned long long>(ranked[i].first), total ? 100.0 * static_cast<double>(ranked[i].first) / static_cast<double>(total) : 0.0);
    }
    in.set_vm_profiling(false); // said here; not again when the interpreter ends
}

int main(int argc, char** argv)
{
    bool want_ast = false;
    bool want_scopes = false;
    bool want_bytecode = false;
    bool want_module = false;
    bool stress = true;
    bool want_profile = false;
    char const* source = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-ast") == 0) {
            want_ast = true;
        } else if (std::strcmp(argv[i], "--dump-scopes") == 0) {
            want_scopes = true;
        } else if (std::strcmp(argv[i], "--dump-bytecode") == 0) {
            want_bytecode = true;
        } else if (std::strcmp(argv[i], "--module") == 0) {
            want_module = true;
        } else if (std::strcmp(argv[i], "--no-stress") == 0) {
            stress = false;
        } else if (std::strcmp(argv[i], "--vm-profile") == 0) {
            want_profile = true;
        } else if (source == nullptr) {
            source = argv[i];
        } else {
            return usage();
        }
    }
    if (source == nullptr)
        return usage();

    // The room a page's script has in the window, so a depth measured here
    // is the depth a page gets.
    js::Interpreter in;
    in.set_stack_budget(platform::js_stack_budget_for(platform::widen_main_thread_stack(platform::script_stack_bytes)));
    in.heap().set_stress(stress);
    if (want_profile)
        in.set_vm_profiling(true);
    if (want_ast || want_scopes || want_module) {
        js::ParseOptions options;
        options.module = want_module;
        options.record_references = want_scopes;
        options.lazy_functions = js::lazy_natives();
        js::Parser parser(in.heap(), js::utf16_from_utf8(source), options);
        std::unique_ptr<js::Program> const program = parser.parse_program("<probe>");
        if (!program) {
            std::optional<js::ParseError> const& error = parser.error();
            std::printf("syntax error: %s\n", error ? error->message.c_str() : "(no message)");
            return 2;
        }
        if (want_ast) {
            std::fputs(js::dump_ast(*program).c_str(), stdout);
            std::fputc('\n', stdout);
        }
        if (want_scopes)
            std::fputs(js::dump_scopes(*program).c_str(), stdout);
    }
    if (want_module) {
        in.set_module_hooks(
            [](std::string_view, std::string_view specifier, std::string&) -> std::optional<std::string> {
                return std::string(specifier);
            },
            [](std::string_view key, std::string& error) -> std::optional<std::u16string> {
                std::ifstream file { std::string(key), std::ios::binary };
                if (!file) {
                    error = "cannot read '" + std::string(key) + "'";
                    return std::nullopt;
                }
                std::string const text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                return js::utf16_from_utf8(text);
            });
        js::ModuleRecord* record = in.parse_module(js::utf16_from_utf8(source), "<probe>");
        if (record == nullptr) {
            std::printf("syntax error: %s\n", in.describe(in.take_exception()).c_str());
            return 2;
        }
        if (!in.load_module(*record) || !in.link_module(*record)) {
            std::printf("threw %s\n", in.describe(in.take_exception()).c_str());
            return 1;
        }
        std::optional<js::Value> const promise = in.evaluate_module(*record);
        in.run_jobs([&in](js::Value const& thrown) {
            std::printf("a job threw %s\n", in.describe(thrown).c_str());
        });
        if (!promise) {
            std::printf("threw %s\n", in.describe(in.take_exception()).c_str());
            return 1;
        }
        auto const* state = static_cast<js::PromiseObject const*>(promise->as_object());
        bool const ok = state->state() == js::PromiseObject::State::Fulfilled;
        std::printf("%s %s\n", ok ? "ok" : "threw", ok ? "undefined" : in.describe(state->result()).c_str());
        if (want_bytecode) {
            for (auto const& [node, code] : in.impl().code_blocks) {
                std::printf("--- %s ---\n", node->name ? node->name->to_utf8().c_str() : "(anonymous)");
                std::fputs(js::disassemble(*code).c_str(), stdout);
            }
        }
        return ok ? 0 : 1;
    }
    js::Outcome const outcome = in.run_script(std::string_view(source), "<probe>");
    in.run_jobs([&in](js::Value const& thrown) {
        std::printf("a job threw %s\n", in.describe(thrown).c_str());
    });
    std::printf("%s %s\n", outcome.ok ? "ok" : "threw", in.describe(outcome.value).c_str());
    if (want_profile)
        print_profile(in);
    if (want_bytecode) {
        for (auto const& [node, code] : in.impl().code_blocks) {
            std::printf("--- %s ---\n", node->name ? node->name->to_utf8().c_str() : "(anonymous)");
            std::fputs(js::disassemble(*code).c_str(), stdout);
        }
    }
    return outcome.ok ? 0 : 1;
}
