#include "JsTest.h"

#include "js/Interpreter.h"
#include "js/ScriptPreparer.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace sashfold;

// Scripts parsed off the page's thread (js/ScriptPreparer.h): the program
// taken is the one the page would have parsed, the bytes decide whether it
// is taken, a parse that failed leaves the page to report the error, the
// names a pool thread interned are the page's own atoms, and an
// interpreter may end while parses are still running.

namespace {

std::size_t ahead(js::Interpreter const& in)
{
    return in.account().programs_parsed_ahead;
}

// Waits, up to a generous ceiling, until a parse offered has begun: take()
// leaves a parse that has not begun to the page, by design.
void let_the_pool_start()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void test_offered_then_attached()
{
    js::Interpreter in;
    in.heap().set_stress(true);
    auto preparer = std::make_shared<js::ScriptPreparer>();
    // Offered before the heap is known, as a script that arrives before
    // the page's realm is made; a byte-order mark is not source.
    preparer->offer("https://example.test/a.js", "\xEF\xBB\xBFvar fromAhead = [1, 2, 3].map(function (x) { return x * 2; }).join(); function later() { return fromAhead + '!'; }");
    in.set_script_preparer(preparer);
    let_the_pool_start();
    js::Outcome const outcome = in.run_script(std::string_view("var fromAhead = [1, 2, 3].map(function (x) { return x * 2; }).join(); function later() { return fromAhead + '!'; }"), "https://example.test/a.js");
    CHECK(outcome.ok);
    CHECK(ahead(in) == 1);
    // The lazy body parsed at its first call, from the program's source.
    CHECK_JS_TRUE(in, "later() === '2,4,6!'");
    // The pool's atoms are the page's: a name read here finds the binding.
    CHECK_JS_TRUE(in, "'fromAhead' in globalThis && Object.keys({ fromAhead: 1 })[0] === 'fromAhead'");
    // An address offered and taken is not taken twice.
    js::Outcome const again = in.run_script(std::string_view("var second = 2;"), "https://example.test/a.js");
    CHECK(again.ok && ahead(in) == 1);
    CHECK_JS_TRUE(in, "second === 2");
}

void test_bytes_decide()
{
    js::Interpreter in;
    auto preparer = std::make_shared<js::ScriptPreparer>();
    in.set_script_preparer(preparer);
    // The page's own request got other bytes than the ones offered: the
    // page's bytes run, parsed here.
    preparer->offer("https://example.test/b.js", "var which = 'offered';");
    let_the_pool_start();
    js::Outcome const outcome = in.run_script(std::string_view("var which = 'fetched';"), "https://example.test/b.js");
    CHECK(outcome.ok);
    CHECK(ahead(in) == 0);
    CHECK_JS_TRUE(in, "which === 'fetched'");
    // An address never offered parses as always.
    js::Outcome const plain = in.run_script(std::string_view("var plain = 1;"), "https://example.test/c.js");
    CHECK(plain.ok && ahead(in) == 0);
}

void test_failed_parse_left_to_the_page()
{
    js::Interpreter in;
    auto preparer = std::make_shared<js::ScriptPreparer>();
    in.set_script_preparer(preparer);
    preparer->offer("https://example.test/bad.js", "var = ;");
    let_the_pool_start();
    js::Outcome const outcome = in.run_script(std::string_view("var = ;"), "https://example.test/bad.js");
    CHECK(!outcome.ok);
    CHECK(ahead(in) == 0);
    CHECK(in.describe(outcome.value).starts_with("SyntaxError"));
}

// Many scripts sharing names, parsed on the pool while the page's thread
// interns the same names and collects at every allocation: every program
// runs, and every name is one atom.
void test_names_shared_under_collection()
{
    js::Interpreter in;
    in.heap().set_stress(true);
    auto preparer = std::make_shared<js::ScriptPreparer>();
    in.set_script_preparer(preparer);
    constexpr int scripts = 24;
    auto source = [](int i) {
        std::string text;
        for (int j = 0; j < 40; ++j)
            text += "var shared" + std::to_string(j) + " = (globalThis.shared" + std::to_string(j) + " || 0) + 1; ";
        text += "var own" + std::to_string(i) + " = { key" + std::to_string(i % 5) + ": " + std::to_string(i) + " };";
        return text;
    };
    for (int i = 0; i < scripts; ++i)
        preparer->offer("https://example.test/s" + std::to_string(i) + ".js", source(i));
    // The page interns the same names meanwhile, collecting as it goes.
    for (int j = 0; j < 40; ++j)
        CHECK(sashfold::test::run_js(in, "var shared" + std::to_string(j) + " = 0;").ok);
    let_the_pool_start();
    for (int i = 0; i < scripts; ++i) {
        js::Outcome const outcome = in.run_script(std::string_view(source(i)), "https://example.test/s" + std::to_string(i) + ".js");
        CHECK(outcome.ok);
    }
    CHECK(ahead(in) > 0);
    CHECK_JS_TRUE(in, "shared0 === 24 && shared39 === 24 && own23.key3 === 23 && Object.keys(own7)[0] === 'key2'");
}

// The interpreter ends while parses are running: it waits for them and
// nothing touches its heap afterwards.
void test_ends_while_parsing()
{
    auto preparer = std::make_shared<js::ScriptPreparer>();
    std::string big;
    for (int i = 0; i < 20000; ++i)
        big += "function f" + std::to_string(i) + "(a, b) { return a + b * " + std::to_string(i) + "; }\n";
    {
        js::Interpreter in;
        in.set_script_preparer(preparer);
        for (int i = 0; i < 4; ++i)
            preparer->offer("https://example.test/big" + std::to_string(i) + ".js", big);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Offered after the end: nothing is parsed for a heap that is gone.
    preparer->offer("https://example.test/late.js", big);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(true);
}

} // namespace

int main()
{
    test_offered_then_attached();
    test_bytes_decide();
    test_failed_parse_left_to_the_page();
    test_names_shared_under_collection();
    test_ends_while_parsing();
    return sashfold::test::report("js_script_preparer");
}
