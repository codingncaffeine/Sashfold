// js_probe runs a script on the engine under heap stress and shows what
// the engine made of it: the completion or the thrown value after the job
// queue has drained, the syntax tree (--dump-ast) and the bytecode of every
// function body the bytecode tier compiled (--dump-bytecode). A build tool
// for the script engine's own work, never shipped.
//
//   js_probe "<source>" [--module] [--dump-ast] [--dump-bytecode]
//
// The source is one argument; a file arrives as "$(cat page.js)". With
// --module it is parsed under the Module goal and evaluated as a module,
// its imports read as files named by their specifiers relative to the
// working directory. Exit status: 0 when the script completed (a module:
// its evaluation promise fulfilled), 1 when it threw (a module: the
// promise rejected), 2 for a syntax error or a usage error.
#include "js/Bytecode.h"
#include "js/Evaluator.h"
#include "js/Interpreter.h"
#include "js/Module.h"
#include "js/Object.h"
#include "js/Parser.h"
#include "js/Strings.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace sashfold;

namespace {

int usage()
{
    std::fputs("usage: js_probe \"<source>\" [--module] [--dump-ast] [--dump-bytecode]\n", stderr);
    return 2;
}

}

int main(int argc, char** argv)
{
    bool want_ast = false;
    bool want_bytecode = false;
    bool want_module = false;
    char const* source = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-ast") == 0) {
            want_ast = true;
        } else if (std::strcmp(argv[i], "--dump-bytecode") == 0) {
            want_bytecode = true;
        } else if (std::strcmp(argv[i], "--module") == 0) {
            want_module = true;
        } else if (source == nullptr) {
            source = argv[i];
        } else {
            return usage();
        }
    }
    if (source == nullptr)
        return usage();

    js::Interpreter in;
    in.heap().set_stress(true);
    if (want_ast || want_module) {
        js::ParseOptions options;
        options.module = want_module;
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
    if (want_bytecode) {
        for (auto const& [node, code] : in.impl().code_blocks) {
            std::printf("--- %s ---\n", node->name ? node->name->to_utf8().c_str() : "(anonymous)");
            std::fputs(js::disassemble(*code).c_str(), stdout);
        }
    }
    return outcome.ok ? 0 : 1;
}
