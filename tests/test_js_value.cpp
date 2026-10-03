#include "JsTest.h"

#include "js/BigInteger.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Object.h"
#include "js/Value.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

// The value in eight bytes (js/Value.h): JavaScriptCore's encoding, bit
// for bit — the constants, the int32 and double forms, every NaN made the
// one canonical NaN, a cell as its own address with its kind in the cell
// — and the int32 paths of the machine's operators, each case checked
// against the answer node gives for the same source.

using namespace sashfold;
using js::Value;

namespace {

std::uint64_t bits_of(double d)
{
    return std::bit_cast<std::uint64_t>(d);
}

void test_constants()
{
    CHECK_EQ(Value::NumberTag, 0xfffe000000000000ull);
    CHECK_EQ(Value::DoubleEncodeOffset, 0x0002000000000000ull);
    CHECK_EQ(Value::OtherTag, 0x2ull);
    CHECK_EQ(Value::BoolTag, 0x4ull);
    CHECK_EQ(Value::UndefinedTag, 0x8ull);
    CHECK_EQ(Value::NotCellMask, 0xfffe000000000002ull);
    CHECK_EQ(Value::CanonicalNaN, 0x7ff8000000000000ull);

    CHECK_EQ(Value::empty().bits(), 0x0ull);
    CHECK_EQ(Value::null().bits(), 0x2ull);
    CHECK_EQ(Value::boolean(false).bits(), 0x6ull);
    CHECK_EQ(Value::boolean(true).bits(), 0x7ull);
    CHECK_EQ(Value::undefined().bits(), 0xaull);
    CHECK_EQ(Value().bits(), 0xaull);

    CHECK(Value().is_undefined());
    CHECK(Value().type() == Value::Type::Undefined);
    CHECK(Value::undefined().is_nullish());
    CHECK(!Value::undefined().is_null());
    CHECK(Value::null().is_null());
    CHECK(Value::null().is_nullish());
    CHECK(Value::null().type() == Value::Type::Null);
    CHECK(!Value::null().is_undefined());
    CHECK(Value::boolean(true).is_boolean());
    CHECK(Value::boolean(false).is_boolean());
    CHECK(Value::boolean(true).as_boolean());
    CHECK(!Value::boolean(false).as_boolean());
    CHECK(Value::boolean(false).type() == Value::Type::Boolean);
    CHECK(!Value::boolean(false).is_nullish());
    CHECK(Value::empty().is_empty());
    CHECK(Value::empty().type() == Value::Type::Empty);
    CHECK(!Value::empty().is_nullish());
    for (Value const v : { Value::undefined(), Value::null(), Value::boolean(false), Value::boolean(true), Value::empty() }) {
        CHECK(!v.is_cell());
        CHECK(v.as_cell() == nullptr);
        CHECK(!v.is_number());
        CHECK(!v.is_string());
        CHECK(!v.is_object());
    }
}

void test_int32()
{
    for (std::int32_t const i : { 0, 1, -1, INT32_MAX, INT32_MIN }) {
        Value const v = Value::int32(i);
        CHECK_EQ(v.bits(), 0xfffe000000000000ull | static_cast<std::uint32_t>(i));
        CHECK(v.is_int32());
        CHECK(v.is_number());
        CHECK(!v.is_double());
        CHECK(!v.is_cell());
        CHECK(v.type() == Value::Type::Number);
        CHECK_EQ(v.as_int32(), i);
        CHECK_EQ(v.as_number(), static_cast<double>(i));
        // The double that is this integer is stored as the same int32.
        CHECK_EQ(Value::number(static_cast<double>(i)).bits(), v.bits());
    }
}

void test_doubles()
{
    double const values[] = {
        -0.0,
        2147483648.0, // 2^31, one past INT32_MAX
        -2147483649.0, // -2^31 - 1
        0.5,
        2147483647.5,
        -2147483648.5,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::max(), // the largest normal
        -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::min(), // the smallest normal
        -std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min(), // a subnormal
        4.9406564584124654e-320,
    };
    for (double const d : values) {
        Value const v = Value::number(d);
        CHECK_EQ(v.bits(), bits_of(d) + 0x0002000000000000ull);
        CHECK(!v.is_int32());
        CHECK(v.is_double());
        CHECK(v.is_number());
        CHECK(!v.is_cell());
        CHECK(v.type() == Value::Type::Number);
        CHECK_EQ(bits_of(v.as_number()), bits_of(d));
    }
    CHECK(std::signbit(Value::number(-0.0).as_number()));
    CHECK(!Value::number(0.0).is_double());
}

void test_nan_payloads()
{
    // Whatever bits a NaN arrives with, it is stored as the canonical one:
    // an impure payload plus 2^49 could otherwise read as an int32, a cell
    // or nothing at all. The bits are checked first, and nothing below
    // reads through a value that might be a stray pointer.
    std::uint64_t const canonical = 0x7ff8000000000000ull + 0x0002000000000000ull;
    std::uint64_t const payloads[] = {
        0x7ff8000000000000ull, // the canonical quiet NaN
        0x7ff0000000000001ull, // signalling
        0x7ff4000000000000ull, // signalling, another payload
        0xfff8000000000000ull, // negative
        0xfffc000000000001ull, // + 2^49 would be the int32 1
        0xfffe000000001000ull, // + 2^49 would be a cell at 0x1000
        0xffffffffffffffffull, // + 2^49 would be no kind of value
    };
    for (std::uint64_t const payload : payloads) {
        double const nan = std::bit_cast<double>(payload);
        Value const v = Value::number(nan);
        CHECK_EQ(v.bits(), canonical);
        CHECK(v.is_number());
        CHECK(!v.is_int32());
        CHECK(!v.is_cell());
        CHECK(std::isnan(v.as_number()));
        CHECK_EQ(bits_of(v.as_number()), 0x7ff8000000000000ull);
    }

    // And read from a Float64Array, or a DataView, over a hand-filled
    // buffer: what the script sees is a number and a NaN, never the int32
    // its bits would otherwise have decoded to.
    js::Interpreter in;
    in.heap().set_stress(true);
    char const* const sources[] = {
        "var b = new ArrayBuffer(8); var u = new Uint32Array(b); u[0] = 1; u[1] = 0xfffc0000; var f = new Float64Array(b); "
        "[typeof f[0], f[0] === f[0], f[0] === 1].join()",
        "var b = new ArrayBuffer(8); var u = new Uint32Array(b); u[0] = 0xffffffff; u[1] = 0xffffffff; var f = new Float64Array(b); "
        "[typeof f[0], f[0] === f[0], f[0] === 1].join()",
        "var b = new ArrayBuffer(8); var u = new Uint32Array(b); u[0] = 1; u[1] = 0x7ff00000; var f = new Float64Array(b); "
        "[typeof f[0], f[0] === f[0], f[0] === 1].join()",
        "var dv = new DataView(new ArrayBuffer(8)); dv.setUint32(0, 0xfffc0000); dv.setUint32(4, 1); var v = dv.getFloat64(0); "
        "[typeof v, v === v, v === 1].join()",
    };
    for (char const* source : sources)
        CHECK_JS_STRING(in, source, "number,false,false");
    // The value itself, as it leaves the typed array.
    test::JsRun const run = test::run_js(in,
        "var b = new ArrayBuffer(8); var u = new Uint32Array(b); u[0] = 1; u[1] = 0xfffc0000; new Float64Array(b)[0]");
    CHECK(run.ok);
    CHECK_EQ(run.value.bits(), canonical);
}

void test_cells()
{
    js::Heap heap;
    js::Heap::NoCollect const no_collect(heap);
    js::JsString* string = heap.string(std::u16string_view(u"text"));
    js::Symbol* symbol = heap.symbol(string);
    js::BigInt* bigint = heap.bigint(js::BigInteger::from_int64(7));
    js::Object* object = heap.allocate<js::Object>(nullptr);
    js::ArrayObject* array = heap.allocate<js::ArrayObject>(nullptr);

    CHECK(string->kind() == js::CellKind::String);
    CHECK(symbol->kind() == js::CellKind::Symbol);
    CHECK(bigint->kind() == js::CellKind::BigInt);
    CHECK(object->kind() == js::CellKind::Object);
    CHECK(array->kind() == js::CellKind::Object);
    struct Plain : js::Cell { };
    Plain const plain;
    CHECK(plain.kind() == js::CellKind::Other);

    struct Case {
        Value value;
        js::Cell* cell;
        Value::Type type;
    };
    Case const cases[] = {
        { Value::string(string), string, Value::Type::String },
        { Value::symbol(symbol), symbol, Value::Type::Symbol },
        { Value::bigint(bigint), bigint, Value::Type::BigInt },
        { Value::object(object), object, Value::Type::Object },
        { Value::object(array), array, Value::Type::Object },
    };
    for (Case const& c : cases) {
        Value const v = c.value;
        // The cell's own address, unchanged: 8-aligned, top 16 bits clear.
        CHECK_EQ(v.bits(), static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(c.cell)));
        CHECK_EQ(v.bits() >> 48, 0ull);
        CHECK_EQ(v.bits() & 7u, 0ull);
        CHECK(v.is_cell());
        CHECK(v.as_cell() == c.cell);
        CHECK(!v.is_number());
        CHECK(!v.is_nullish());
        CHECK(!v.is_boolean());
        CHECK(!v.is_empty());
        CHECK(v.type() == c.type);
        CHECK_EQ(v.is_string(), c.type == Value::Type::String);
        CHECK_EQ(v.is_symbol(), c.type == Value::Type::Symbol);
        CHECK_EQ(v.is_bigint(), c.type == Value::Type::BigInt);
        CHECK_EQ(v.is_object(), c.type == Value::Type::Object);
    }
    CHECK(Value::string(string).as_string() == string);
    CHECK(Value::symbol(symbol).as_symbol() == symbol);
    CHECK(Value::bigint(bigint).as_bigint() == bigint);
    CHECK(Value::object(object).as_object() == object);

    // The same from script.
    js::Interpreter in;
    in.heap().set_stress(true);
    CHECK(test::run_js(in, "'s'").value.type() == Value::Type::String);
    CHECK(test::run_js(in, "({})").value.type() == Value::Type::Object);
    CHECK(test::run_js(in, "[]").value.type() == Value::Type::Object);
    CHECK(test::run_js(in, "(function () {})").value.type() == Value::Type::Object);
    CHECK(test::run_js(in, "Symbol()").value.type() == Value::Type::Symbol);
    CHECK(test::run_js(in, "10n").value.type() == Value::Type::BigInt);
    CHECK_JS_STRING(in, "[typeof 's', typeof {}, typeof Symbol(), typeof 1n, typeof 1, typeof null].join()",
        "string,object,symbol,bigint,number,object");
}

void test_identity()
{
    // operator== is the bits: +0 and −0 differ, NaN meets NaN, 1 and 1.0
    // are one value.
    CHECK(!(Value::number(0.0) == Value::number(-0.0)));
    CHECK(Value::number(0.0) == Value::int32(0));
    CHECK(Value::number(std::nan("")) == Value::number(std::bit_cast<double>(0xfff8000000000001ull)));
    CHECK(Value::number(1) == Value::number(1.0));
    CHECK(Value::number(1.0) == Value::int32(1));
    CHECK(!(Value::number(1.0) == Value::number(1.5)));
    CHECK(!(Value::int32(0) == Value::empty()));
    CHECK(!(Value::boolean(false) == Value::int32(0)));
}

// A number from script, checked against node's answer bit for bit (so −0
// is not +0) and in its one encoding (an integer in range an int32, the
// rest a double).
void check_number(js::Interpreter& in, std::string_view source, double expected, int line)
{
    test::JsRun const run = test::run_js(in, source);
    bool const ok = run.ok && run.value.is_number()
        && (std::isnan(expected) ? std::isnan(run.value.as_number()) : bits_of(run.value.as_number()) == bits_of(expected))
        && run.value == Value::number(expected);
    std::string const what = std::string(source) + " is " + std::to_string(expected) + (std::signbit(expected) && expected == 0 ? " (-0)" : "")
        + (run.ok ? "" : ", threw " + run.thrown);
    test::check(ok, what.c_str(), __FILE__, line);
}

#define CHECK_NUMBER(in, source, expected) check_number(in, source, expected, __LINE__)

void test_int32_operators()
{
    js::Interpreter in;
    in.heap().set_stress(true);
    double const inf = std::numeric_limits<double>::infinity();
    // The edges of int32 arithmetic, each against node 26.
    CHECK_NUMBER(in, "2147483647+1", 2147483648.0);
    CHECK_NUMBER(in, "-2147483648-1", -2147483649.0);
    CHECK_NUMBER(in, "65536*65536", 4294967296.0);
    CHECK_NUMBER(in, "0*-1", -0.0);
    CHECK_NUMBER(in, "-1%1", -0.0);
    CHECK_NUMBER(in, "-2147483648%-1", -0.0);
    CHECK_NUMBER(in, "-2147483648/-1", 2147483648.0);
    CHECK_NUMBER(in, "(-0)+0", 0.0);
    CHECK_NUMBER(in, "1/(0*-1)", -inf);
    CHECK_NUMBER(in, "(2**31)|0", -2147483648.0);
    CHECK_NUMBER(in, "-1>>>0", 4294967295.0);
    CHECK_NUMBER(in, "var x = 2147483647; x++; x", 2147483648.0);
    CHECK_NUMBER(in, "var x = 2147483647; var y = x++; y", 2147483647.0);
    CHECK_NUMBER(in, "var x = 2147483647; ++x", 2147483648.0);
    CHECK_NUMBER(in, "var x = -2147483648; x--; x", -2147483649.0);
    CHECK_NUMBER(in, "var x = -2147483648; --x", -2147483649.0);
    CHECK_JS_STRING(in, "var m = new Map(); m.set(-0, 'z'); m.get(0)", "z");
    CHECK_JS_TRUE(in, "var m = new Map(); m.set(-0, 'z'); Object.is([...m.keys()][0], 0)");
    CHECK_NUMBER(in, "var s = new Set([0, -0, 1, 1.0, NaN, 0/0]); s.size", 3);
    // A lookup by −0 finds the +0 key: one hash for the two.
    CHECK_JS_STRING(in, "var m = new Map(); m.set(0, 'z'); m.get(-0)", "z");
    CHECK_JS_TRUE(in, "new Set([0]).has(-0)");
    CHECK_JS_STRING(in, "var m = new Map([[1, 'one']]); m.get(1.0) + m.has(0.5 + 0.5)", "onetrue");

    // Around them.
    CHECK_NUMBER(in, "7 % -3", 1);
    CHECK_NUMBER(in, "-7 % 3", -1);
    CHECK_NUMBER(in, "5 % 0", std::nan(""));
    CHECK_NUMBER(in, "0 % -5", 0.0);
    CHECK_NUMBER(in, "-5 % 5", -0.0);
    CHECK_NUMBER(in, "1 << 31", -2147483648.0);
    CHECK_NUMBER(in, "1 << 32", 1);
    CHECK_NUMBER(in, "-8 >> 1", -4);
    CHECK_NUMBER(in, "-8 >>> 28", 15);
    CHECK_NUMBER(in, "-5 * 0", -0.0);
    CHECK_NUMBER(in, "0 * 5", 0.0);
    CHECK_NUMBER(in, "46341 * 46341", 2147488281.0);
    CHECK_NUMBER(in, "-65536 * 32768", -2147483648.0);
    CHECK_NUMBER(in, "~2147483647", -2147483648.0);
    CHECK_NUMBER(in, "6 / 3", 2);
    CHECK_NUMBER(in, "7 / 2", 3.5);
    CHECK_NUMBER(in, "0.5 + 0.5", 1);
    CHECK_JS_TRUE(in, "-2147483648 < 2147483647");
    CHECK_JS_FALSE(in, "2147483647 <= -2147483648");
    CHECK_JS_TRUE(in, "3 === 3.0");
    CHECK_JS_TRUE(in, "-0 === 0");
    CHECK_JS_FALSE(in, "Object.is(-0, 0)");
    // Through parameters, so nothing but the operator sees the operands.
    CHECK_NUMBER(in, "(function (a, b) { return a + b; })(2147483647, 1)", 2147483648.0);
    CHECK_NUMBER(in, "(function (a, b) { return a - b; })(-2147483648, 1)", -2147483649.0);
    CHECK_NUMBER(in, "(function (a, b) { return a * b; })(0, -1)", -0.0);
    CHECK_NUMBER(in, "(function (a, b) { return a % b; })(-2147483648, -1)", -0.0);
    CHECK_NUMBER(in, "(function (a, b) { return a * b; })(-65536, 32768)", -2147483648.0);
    CHECK_NUMBER(in, "(function (a) { return -a; })(0)", -0.0);
}

void test_array_reads()
{
    // GetMember at an int32 index: the dense element when there is one;
    // a hole, an index past the storage, a negative index, an accessor
    // and every other base go the whole way (answers from node 26).
    js::Interpreter in;
    in.heap().set_stress(true);
    // (A script whose completion is the hole would read undefined anyway:
    // the comparisons are what see it.)
    CHECK_JS_STRING(in, "var a = [1, , 3]; [a[1] === undefined, typeof a[1], 1 in a].join()", "true,undefined,false");
    CHECK_JS_STRING(in, "var a = [1, , 3]; Array.prototype[1] = 'p'; var r = a[1]; delete Array.prototype[1]; r", "p");
    CHECK(test::run_js(in, "var a = [1, 2, 3]; a[3]").value.is_undefined());
    CHECK(test::run_js(in, "var a = [1, 2, 3]; a[-1]").value.is_undefined());
    CHECK_JS_STRING(in, "var a = [1, 2, 3]; a[-1] = 'n'; a[-1]", "n");
    CHECK_JS_STRING(in, "var a = [1, 2, 3]; Object.defineProperty(a, 1, { get() { return 'g'; } }); a[1]", "g");
    CHECK_JS_STRING(in, "'abc'[1]", "b");
    CHECK_NUMBER(in, "var t = new Int8Array([5, 6]); t[1]", 6);
    CHECK_NUMBER(in, "(function () { return arguments[1]; })(7, 8)", 8);
    CHECK(test::run_js(in, "var a = [10, 20]; a[1.5]").value.is_undefined());
    CHECK(test::run_js(in, "var a = []; a[2147483647]").value.is_undefined());
    CHECK_NUMBER(in, "var a = [1, 2, 3]; var s = 0; for (var i = 0; i < a.length; i++) s += a[i]; s", 6);
    CHECK_NUMBER(in, "var a = [1, , 3]; var n = 0; for (var i = 0; i < 3; i++) if (a[i] === undefined) n++; n", 1);
    CHECK(test::run_js(in, "var a = [1, 2]; a.length = 1; a[1]").value.is_undefined());
}

}

int main()
{
    test_constants();
    test_int32();
    test_doubles();
    test_nan_payloads();
    test_cells();
    test_identity();
    test_int32_operators();
    test_array_reads();
    return test::report("js_value");
}
