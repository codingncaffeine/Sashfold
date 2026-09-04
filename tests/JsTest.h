#pragma once

// Helpers for tests that run script: an interpreter in heap stress mode
// (a collection at every allocation, so an unrooted value fails the first
// time), and one-line evaluators that report a result or the thrown value.

#include "Test.h"

#include "js/Interpreter.h"
#include "js/Strings.h"

#include <string>
#include <string_view>

namespace sashfold::test {

struct JsRun {
    bool ok = false;
    js::Value value;
    std::string thrown; // "TypeError: message" when !ok
};

inline JsRun run_js(js::Interpreter& interpreter, std::string_view source)
{
    js::Outcome outcome = interpreter.run_script(source, "<test>");
    JsRun run;
    run.ok = outcome.ok;
    run.value = outcome.value;
    if (!outcome.ok)
        run.thrown = interpreter.describe(outcome.value);
    return run;
}

// Tests make their realm on the stack and turn stress on:
//     js::Interpreter interpreter;
//     interpreter.heap().set_stress(true);

inline double eval_number(js::Interpreter& interpreter, std::string_view source)
{
    JsRun run = run_js(interpreter, source);
    if (!run.ok) {
        fail("threw " + run.thrown + " evaluating: " + std::string(source), __FILE__, __LINE__);
        return 0;
    }
    if (!run.value.is_number()) {
        fail("not a number: " + std::string(source), __FILE__, __LINE__);
        return 0;
    }
    return run.value.as_number();
}

inline std::string eval_string(js::Interpreter& interpreter, std::string_view source)
{
    JsRun run = run_js(interpreter, source);
    if (!run.ok) {
        fail("threw " + run.thrown + " evaluating: " + std::string(source), __FILE__, __LINE__);
        return "";
    }
    if (!run.value.is_string()) {
        fail("not a string: " + std::string(source), __FILE__, __LINE__);
        return "";
    }
    return run.value.as_string()->to_utf8();
}

inline bool eval_bool(js::Interpreter& interpreter, std::string_view source)
{
    JsRun run = run_js(interpreter, source);
    if (!run.ok) {
        fail("threw " + run.thrown + " evaluating: " + std::string(source), __FILE__, __LINE__);
        return false;
    }
    if (!run.value.is_boolean()) {
        fail("not a boolean: " + std::string(source), __FILE__, __LINE__);
        return false;
    }
    return run.value.as_boolean();
}

// The thrown value's description ("RangeError: …"), or "" when it ran.
inline std::string eval_throws(js::Interpreter& interpreter, std::string_view source)
{
    JsRun run = run_js(interpreter, source);
    return run.ok ? std::string() : run.thrown;
}

}

// CHECK_JS_EQ(interp, "1 + 2", 3.0)   CHECK_JS_EQ(interp, "'a' + 1", "a1")
#define CHECK_JS_NUMBER(interpreter, source, expected) \
    CHECK_EQ(::sashfold::test::eval_number(interpreter, source), static_cast<double>(expected))
#define CHECK_JS_STRING(interpreter, source, expected) \
    CHECK_EQ(::sashfold::test::eval_string(interpreter, source), std::string(expected))
#define CHECK_JS_TRUE(interpreter, source) CHECK(::sashfold::test::eval_bool(interpreter, source))
#define CHECK_JS_FALSE(interpreter, source) CHECK(!::sashfold::test::eval_bool(interpreter, source))
// The error's name must start the description: CHECK_JS_THROWS(i, "null.x", "TypeError")
#define CHECK_JS_THROWS(interpreter, source, error_name) \
    CHECK(::sashfold::test::eval_throws(interpreter, source).starts_with(error_name))
