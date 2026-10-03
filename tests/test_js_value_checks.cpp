// The check on a cell's address where a cell value is made (js/Value.h,
// Value::cell): an address that is null, not 8-aligned, or has any of its
// top 16 bits set would read back as empty or as a number, so making a
// value of one ends the process. The check runs in debug and sanitizer
// builds; this test turns it on itself, so that every build runs it.
//
// It is its own program, linked to nothing of the engine's: the engine's
// files are compiled with the check as their build has it, and an inline
// function must be the same everywhere it is linked.

#define SASHFOLD_JS_VALUE_CHECKS 1

#include "Test.h"

#include "js/Value.h"

#include <cstdint>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

using namespace sashfold;
using js::Value;

namespace {

Value make(std::uint64_t address)
{
    return Value::object(reinterpret_cast<js::Object*>(static_cast<std::uintptr_t>(address)));
}

void test_accepted()
{
    // The lowest and the highest addresses a cell may have, and one between.
    for (std::uint64_t const address : { 0x8ull, 0x7f0000001000ull, 0x0000fffffffffff8ull }) {
        Value const v = make(address);
        CHECK_EQ(v.bits(), address);
        CHECK(v.is_cell());
        CHECK(!v.is_number());
    }
}

// A cell's kind is read with Value.h alone: this file includes nothing
// else of the engine's, so it would not build were the read defined
// elsewhere (it once was, in Heap.h).
struct KindOnly : js::Cell {
    explicit KindOnly(js::CellKind kind)
        : Cell(kind)
    {
    }
};

void test_kind_read_here()
{
    KindOnly string(js::CellKind::String);
    KindOnly object(js::CellKind::Object);
    js::Cell* const as_string = &string;
    js::Cell* const as_object = &object;
    Value const s = Value::string(reinterpret_cast<js::JsString*>(as_string));
    Value const o = Value::object(reinterpret_cast<js::Object*>(as_object));
    CHECK(s.is_string() && !s.is_object());
    CHECK(o.is_object() && !o.is_string());
    CHECK(o.type() == Value::Type::Object);
}

#ifndef _WIN32
// Makes a value of `address` in a child and reports how the child ended:
// true when it was ended by SIGABRT, as the check ends it.
bool aborts(std::uint64_t address)
{
    std::fflush(nullptr);
    pid_t const child = fork();
    if (child == 0) {
        // No core file for an abort this test asks for.
        rlimit const none { 0, 0 };
        setrlimit(RLIMIT_CORE, &none);
#ifdef __linux__
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
        Value const v = make(address);
        _exit(v.bits() == address ? 0 : 1);
    }
    int status = 0;
    if (!CHECK(waitpid(child, &status, 0) == child))
        return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

void test_refused()
{
    CHECK(aborts(0x0)); // null: would read as empty
    CHECK(aborts(0x1004)); // 4-aligned
    CHECK(aborts(0x1001)); // odd
    CHECK(aborts(0x0001000000001000ull)); // bit 48: would read as a double
    CHECK(aborts(0x8000000000001000ull)); // bit 63
    CHECK(aborts(0xfffe000000001000ull)); // would read as an int32
    // And a good address in a child ends normally, so the refusals above
    // are the check's and not the child's way of ending.
    CHECK(!aborts(0x7f0000001000ull));
}
#endif

}

int main()
{
    test_accepted();
    test_kind_read_here();
#ifndef _WIN32
    test_refused();
#endif
    return test::report("js_value_checks");
}
