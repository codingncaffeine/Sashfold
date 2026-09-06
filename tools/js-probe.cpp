// js_probe runs a script on the engine under heap stress and shows what
// the engine made of it: the completion or the thrown value after the job
// queue has drained, the syntax tree (--dump-ast) and the bytecode of every
// function body the bytecode tier compiled (--dump-bytecode). A build tool
// for the script engine's own work, never shipped.
//
//   js_probe "<source>" [--dump-ast] [--dump-bytecode]
//
// The source is one argument; a file arrives as "$(cat page.js)". Exit
// status: 0 when the script completed, 1 when it threw, 2 for a syntax
// error or a usage error.
#include "js/Bytecode.h"
#include "js/Evaluator.h"
#include "js/Interpreter.h"
#include "js/Parser.h"
#include "js/Strings.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

using namespace sashfold;

namespace {

int usage()
{
    std::fputs("usage: js_probe \"<source>\" [--dump-ast] [--dump-bytecode]\n", stderr);
    return 2;
}

}

int main(int argc, char** argv)
{
    bool want_ast = false;
    bool want_bytecode = false;
    char const* source = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-ast") == 0) {
            want_ast = true;
        } else if (std::strcmp(argv[i], "--dump-bytecode") == 0) {
            want_bytecode = true;
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
    if (want_ast) {
        js::Parser parser(in.heap(), js::utf16_from_utf8(source));
        std::unique_ptr<js::Program> const program = parser.parse_program("<probe>");
        if (!program) {
            std::optional<js::ParseError> const& error = parser.error();
            std::printf("syntax error: %s\n", error ? error->message.c_str() : "(no message)");
            return 2;
        }
        std::fputs(js::dump_ast(*program).c_str(), stdout);
        std::fputc('\n', stdout);
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
