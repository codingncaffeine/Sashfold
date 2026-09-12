#include "JsTest.h"

#include "js/BigInteger.h"
#include "js/Interpreter.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

// BigInt: the integers themselves — every operation against small values
// the machine can check, the long division on many limbs, the rounding to
// a Number — then the language: literals, the operators, the comparisons
// and coercions the specification spells out, the BigInt function and its
// prototype, the errors, and both tiers of the engine.

using namespace sashfold;
using sashfold::js::BigInteger;

namespace {

js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

BigInteger big(std::string_view decimal)
{
    std::string text(decimal);
    bool const negative = !text.empty() && text.front() == '-';
    if (negative)
        text.erase(0, 1);
    BigInteger value = *BigInteger::parse_digits(text, 10);
    return negative ? value.negated() : value;
}

void test_integers()
{
    // Against the machine, over a grid of small values.
    std::int64_t const samples[] = { 0, 1, -1, 2, -2, 7, -7, 12345, -12345, 2147483647, -2147483648LL, 4294967295LL,
        -4294967296LL, 9007199254740993LL, -9007199254740993LL };
    for (std::int64_t const a : samples) {
        for (std::int64_t const b : samples) {
            BigInteger const x = BigInteger::from_int64(a);
            BigInteger const y = BigInteger::from_int64(b);
            CHECK(x + y == BigInteger::from_int64(a + b));
            CHECK(x - y == BigInteger::from_int64(a - b));
            if (std::abs(a) < (std::int64_t(1) << 31) && std::abs(b) < (std::int64_t(1) << 31))
                CHECK(x * y == BigInteger::from_int64(a * b));
            if (b != 0) {
                CHECK(*BigInteger::divide(x, y) == BigInteger::from_int64(a / b));
                CHECK(*BigInteger::remainder(x, y) == BigInteger::from_int64(a % b));
            }
            CHECK((x & y) == BigInteger::from_int64(a & b));
            CHECK((x | y) == BigInteger::from_int64(a | b));
            CHECK((x ^ y) == BigInteger::from_int64(a ^ b));
            CHECK(compare(x, y) == (a < b ? -1 : a > b ? 1 : 0));
        }
        BigInteger const x = BigInteger::from_int64(a);
        CHECK(x.bitwise_not() == BigInteger::from_int64(~a));
        CHECK(x.to_int64() == a);
        CHECK(x.to_double() == static_cast<double>(a));
        CHECK(*BigInteger::from_double(static_cast<double>(a)) == BigInteger::from_int64(static_cast<std::int64_t>(static_cast<double>(a))));
        CHECK(big(x.to_string()) == x);
        for (int shift : { 0, 1, 5, 31, 32, 33, 63 }) {
            std::int64_t const arithmetic = a >> shift; // floor, the language's >>
            CHECK(*BigInteger::shift_right(x, BigInteger::from_int64(shift)) == BigInteger::from_int64(arithmetic));
            if (std::abs(a) < 4 && shift < 61)
                CHECK(*BigInteger::shift_left(x, BigInteger::from_int64(shift)) == BigInteger::from_int64(a * (std::int64_t(1) << shift)));
        }
    }
    CHECK_EQ(BigInteger::from_int64(-255).to_string(16), "-ff");
    CHECK_EQ(BigInteger::from_int64(255).to_string(2), "11111111");
    CHECK_EQ(big("123456789012345678901234567890").to_string(), "123456789012345678901234567890");
    CHECK_EQ(big("123456789012345678901234567890").to_string(36), "byw97um9s91dlz68tsi"); // as Python spells it
    CHECK(BigInteger::parse_digits("byw97um9s91dlz68tsi", 36) == big("123456789012345678901234567890"));
    CHECK(!BigInteger::parse_digits("12a", 10).has_value());
    CHECK(!BigInteger::parse_digits("", 10).has_value());

    // Long division on many limbs: (a * b + r) / b == a and % == r.
    BigInteger const a = big("340282366920938463463374607431768211457"); // 2^128 + 1
    BigInteger const b = big("18446744073709551629"); // a prime past 2^64
    BigInteger const r = big("12345678901234567");
    BigInteger const n = a * b + r;
    CHECK(*BigInteger::divide(n, b) == a);
    CHECK(*BigInteger::remainder(n, b) == r);
    CHECK(*BigInteger::divide(n.negated(), b) == a.negated());
    CHECK(*BigInteger::remainder(n.negated(), b) == r.negated());
    CHECK(*BigInteger::divide(n, b.negated()) == a.negated());
    CHECK(*BigInteger::remainder(n, b.negated()) == r);
    BigInteger const two_to_200 = *BigInteger::shift_left(BigInteger::from_int64(1), BigInteger::from_int64(200));
    CHECK_EQ(two_to_200.to_string(), "1606938044258990275541962092341162602522202993782792835301376");
    CHECK(*BigInteger::divide(two_to_200, *BigInteger::shift_left(BigInteger::from_int64(1), BigInteger::from_int64(100)))
        == *BigInteger::shift_left(BigInteger::from_int64(1), BigInteger::from_int64(100)));
    CHECK(BigInteger::divide(a, BigInteger()) == std::nullopt);
    CHECK(*BigInteger::power(BigInteger::from_int64(2), BigInteger::from_int64(200)).value == two_to_200);
    CHECK(*BigInteger::power(BigInteger::from_int64(-3), BigInteger::from_int64(3)).value == BigInteger::from_int64(-27));
    CHECK(BigInteger::power(BigInteger::from_int64(2), BigInteger::from_int64(-1)).negative_exponent);
    CHECK(BigInteger::power(BigInteger::from_int64(2), big("100000000000")).too_large);
    CHECK(*BigInteger::power(BigInteger::from_int64(-1), big("100000000001")).value == BigInteger::from_int64(-1));

    // To a Number: ties to even at the 53rd bit, the infinities past 2^1024.
    CHECK(big("9007199254740993").to_double() == 9007199254740992.0);
    CHECK(big("9007199254740995").to_double() == 9007199254740996.0);
    CHECK(big("18446744073709551617").to_double() == 18446744073709551616.0);
    CHECK((*BigInteger::shift_left(BigInteger::from_int64(3), BigInteger::from_int64(100))).to_double() == 3.0 * 1267650600228229401496703205376.0);
    CHECK((*BigInteger::power(BigInteger::from_int64(2), BigInteger::from_int64(1024)).value).to_double() == std::numeric_limits<double>::infinity());
    CHECK((*BigInteger::power(BigInteger::from_int64(2), BigInteger::from_int64(1023)).value).to_double() == std::ldexp(1.0, 1023));
    CHECK(BigInteger::from_double(1e20)->to_string() == "100000000000000000000");
    CHECK(!BigInteger::from_double(1.5).has_value());
    CHECK(!BigInteger::from_double(std::numeric_limits<double>::infinity()).has_value());
    CHECK(big("5").compare_double(4.5) == 1 && big("5").compare_double(5.5) == -1 && big("5").compare_double(5.0) == 0);
    CHECK(big("-5").compare_double(-4.5) == -1 && big("-5").compare_double(-5.5) == 1);

    // Widths.
    CHECK(BigInteger::as_uint_n(8, BigInteger::from_int64(-1)) == BigInteger::from_int64(255));
    CHECK(BigInteger::as_int_n(8, BigInteger::from_int64(255)) == BigInteger::from_int64(-1));
    CHECK(BigInteger::as_int_n(8, BigInteger::from_int64(127)) == BigInteger::from_int64(127));
    CHECK(BigInteger::as_int_n(8, BigInteger::from_int64(128)) == BigInteger::from_int64(-128));
    CHECK(BigInteger::as_uint_n(64, BigInteger::from_int64(-1)) == BigInteger::from_uint64(18446744073709551615ULL));
    CHECK(BigInteger::as_int_n(64, *BigInteger::shift_left(BigInteger::from_int64(1), BigInteger::from_int64(63)))
        == BigInteger::from_int64(std::numeric_limits<std::int64_t>::min()));
    CHECK(BigInteger::as_uint_n(0, BigInteger::from_int64(5)).is_zero());
    CHECK(BigInteger::as_uint_n(200, BigInteger::from_int64(-1)) == two_to_200 - BigInteger::from_int64(1));

    // StringToBigInt.
    CHECK(*BigInteger::from_string(u"  0x1F  ") == BigInteger::from_int64(31));
    CHECK(*BigInteger::from_string(u"-12") == BigInteger::from_int64(-12));
    CHECK(*BigInteger::from_string(u"+12") == BigInteger::from_int64(12));
    CHECK(BigInteger::from_string(u"")->is_zero());
    CHECK(!BigInteger::from_string(u"1.5").has_value());
    CHECK(!BigInteger::from_string(u"-0x10").has_value());
    CHECK(!BigInteger::from_string(u"1n").has_value());
    CHECK(!BigInteger::from_string(u"1e3").has_value());
}

void test_language()
{
    js::Interpreter& in = fresh();
    // Literals.
    CHECK_JS_TRUE(in, "typeof 1n === 'bigint' && typeof Object(1n) === 'object'");
    CHECK_JS_TRUE(in, "0x10n === 16n && 0o17n === 15n && 0b101n === 5n && 1_000n === 1000n && 0n === -0n");
    CHECK_JS_STRING(in, "String(123456789012345678901234567890n)", "123456789012345678901234567890");
    CHECK_JS_STRING(in, "String(-0n)", "0");
    CHECK_JS_THROWS(in, "1.5n", "SyntaxError");
    CHECK_JS_THROWS(in, "1e3n", "SyntaxError");
    CHECK_JS_THROWS(in, "01n", "SyntaxError");
    // Arithmetic.
    CHECK_JS_TRUE(in, "2n ** 64n === 18446744073709551616n && (-7n) / 2n === -3n && (-7n) % 2n === -1n && 7n / -2n === -3n");
    CHECK_JS_TRUE(in, "10n ** 30n / 10n ** 29n === 10n && -(2n ** 100n) + 2n ** 100n === 0n && 3n * -4n === -12n");
    CHECK_JS_TRUE(in, "1n << 100n === 1267650600228229401496703205376n && (-8n) >> 1n === -4n && (-9n) >> 1n === -5n && -1n >> 200n === -1n && 1n << -1n === 0n");
    CHECK_JS_TRUE(in, "(5n & 3n) === 1n && (-5n & 3n) === 3n && (-5n | 3n) === -5n && (-5n ^ 3n) === -8n && ~5n === -6n && ~-1n === 0n");
    CHECK_JS_TRUE(in, "-(-3n) === 3n && -0n === 0n");
    // Comparisons and equality.
    CHECK_JS_TRUE(in, "1n < 2 && 2n > 1.5 && 1n == 1 && !(1n === 1) && 1n == '1' && 1n == true && 0n == '' && 1n < '2' && '1' < 2n");
    CHECK_JS_TRUE(in, "!(1n < 'x') && !(1n > 'x') && !(1n <= 'x') && 1n < Infinity && -1n > -Infinity && !(1n < NaN) && !(NaN > 1n) && !(1n == NaN)");
    CHECK_JS_TRUE(in, "9007199254740993n > 9007199254740992 && 9007199254740992n == 9007199254740992 && !(9007199254740993n == 9007199254740992)");
    CHECK_JS_TRUE(in, "Object.is(0n, -0n) && Object.is(1n, 1n) && [1n].includes(1n) && [1n].indexOf(1n) === 0 && new Set([1n, 1n]).size === 1 && new Map([[2n, 'x']]).get(2n) === 'x'");
    CHECK_JS_TRUE(in, "1n == Object(1n) && Object(1n) != 2n && (function () { switch (2n) { case 2n: return true; } return false; })()");
    // Conversions.
    CHECK_JS_TRUE(in, "Number(2n ** 53n + 1n) === 9007199254740992 && Number(-3n) === -3 && Number(Object(4n)) === 4 && new Number(5n).valueOf() === 5");
    CHECK_JS_TRUE(in, "BigInt(10) === 10n && BigInt('0x1f') === 31n && BigInt(' 12 ') === 12n && BigInt('') === 0n && BigInt(true) === 1n && BigInt(-0) === 0n && BigInt(1e21) === 1000000000000000000000n");
    CHECK_JS_TRUE(in, "BigInt.asIntN(8, 255n) === -1n && BigInt.asUintN(8, -1n) === 255n && BigInt.asIntN(64, 2n ** 63n) === -(2n ** 63n) && BigInt.asUintN(64, -1n) === 18446744073709551615n && BigInt.asIntN(0, 5n) === 0n");
    CHECK_JS_TRUE(in, "(255n).toString(16) === 'ff' && (-255n).toString(2) === '-11111111' && (255n).toString() === '255' && (7n).toLocaleString() === '7' && (7n).valueOf() === 7n");
    CHECK_JS_TRUE(in, "1n + 'x' === '1x' && 'x' + 1n === 'x1' && `${2n}` === '2' && !!0n === false && !!1n === true && (0n || 'a') === 'a'");
    CHECK_JS_TRUE(in, "Object(1n) instanceof BigInt && Object(1n).valueOf() === 1n && BigInt.prototype.toString.call(Object(3n)) === '3' && Object.prototype.toString.call(1n) === '[object BigInt]'");
    CHECK_JS_TRUE(in, "BigInt.prototype[Symbol.toStringTag] === 'BigInt' && BigInt.length === 1 && BigInt.name === 'BigInt' && BigInt.asIntN.length === 2");
    CHECK_JS_TRUE(in, "({ [1n]: 'a' })['1'] === 'a' && ({ 1n: 'b' })[1] === 'b' && ({ 0x10n: 'c' })[16] === 'c' && (class { 2n() { return 7; } }).prototype[2]() === 7");
    CHECK_JS_TRUE(in, "String(BigInt(Number.MAX_SAFE_INTEGER) + 2n) === '9007199254740993' && (2n ** 100n).toString().length === 31");
    CHECK_JS_TRUE(in, "parseInt(10n) === 10 && String(Number(10n ** 20n)) === '100000000000000000000' && isFinite(Number(1n))");
    // Update and compound assignment, in both tiers.
    CHECK_JS_TRUE(in, "(function () { let x = 1n; x++; if (x !== 2n) return false; if (x-- !== 2n) return false; if (x !== 1n) return false; let y = 5n; y += 2n; y *= 3n; y **= 2n; y >>= 1n; return y === 220n && ++x === 2n && --x === 1n; })()");
    CHECK_JS_TRUE(in, "(function* () { let a = 1n; a++; yield a * 2n + 1n; })().next().value === 5n");
    CHECK_JS_TRUE(in, "(function* () { let a = 2n; a **= 10n; a -= 24n; const b = -a; yield [a, b, ~a, a & 0xffn, typeof a]; })().next().value.join() === '1000,-1000,-1001,232,bigint'");
    CHECK_JS_TRUE(in, "(function* (x) { yield x++; yield x; })(9007199254740993n).next().value === 9007199254740993n");
    // Errors.
    CHECK_JS_THROWS(in, "1n + 1", "TypeError");
    CHECK_JS_THROWS(in, "1 * 1n", "TypeError");
    CHECK_JS_THROWS(in, "+1n", "TypeError");
    CHECK_JS_THROWS(in, "1n >>> 0n", "TypeError");
    CHECK_JS_THROWS(in, "1n / 0n", "RangeError");
    CHECK_JS_THROWS(in, "1n % 0n", "RangeError");
    CHECK_JS_THROWS(in, "2n ** -1n", "RangeError");
    CHECK_JS_THROWS(in, "BigInt(1.5)", "RangeError");
    CHECK_JS_THROWS(in, "BigInt(NaN)", "RangeError");
    CHECK_JS_THROWS(in, "BigInt('1.5')", "SyntaxError");
    CHECK_JS_THROWS(in, "BigInt('-0x1')", "SyntaxError");
    CHECK_JS_THROWS(in, "BigInt(undefined)", "TypeError");
    CHECK_JS_THROWS(in, "BigInt(null)", "TypeError");
    CHECK_JS_THROWS(in, "BigInt(Symbol())", "TypeError");
    CHECK_JS_THROWS(in, "new BigInt(1)", "TypeError");
    CHECK_JS_THROWS(in, "JSON.stringify(1n)", "TypeError");
    CHECK_JS_THROWS(in, "JSON.stringify({ a: 1n })", "TypeError");
    CHECK_JS_THROWS(in, "JSON.stringify(Object(1n))", "TypeError");
    CHECK_JS_THROWS(in, "Math.max(1n)", "TypeError");
    CHECK_JS_THROWS(in, "isNaN(1n)", "TypeError");
    CHECK_JS_THROWS(in, "Number.prototype.toString.call(1n)", "TypeError");
    CHECK_JS_THROWS(in, "BigInt.prototype.toString.call(1)", "TypeError");
    CHECK_JS_THROWS(in, "BigInt.prototype.valueOf.call('1')", "TypeError");
    CHECK_JS_THROWS(in, "(1n).toString(1)", "RangeError");
    CHECK_JS_THROWS(in, "BigInt.asIntN(-1, 1n)", "RangeError");
    CHECK_JS_THROWS(in, "BigInt.asIntN(8, 1)", "TypeError");
    CHECK_JS_THROWS(in, "1n << 2n ** 40n", "RangeError");
    CHECK_JS_THROWS(in, "new Date(1n)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { try { 1n + 1; } catch (e) { return e instanceof TypeError && /mix/.test(e.message); } })()");
}

// The two BigInt kinds of typed array and the DataView's four methods:
// elements in and out as BigInts, the low sixty-four bits kept, and the
// content types never mixing.
void test_typed_arrays()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "BigInt64Array.BYTES_PER_ELEMENT === 8 && BigUint64Array.BYTES_PER_ELEMENT === 8 && BigInt64Array.name === 'BigInt64Array' && Object.getPrototypeOf(BigUint64Array) === Object.getPrototypeOf(Int8Array)");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new BigUint64Array()) === '[object BigUint64Array]' && new BigInt64Array(3).length === 3 && new BigInt64Array(3)[0] === 0n && typeof new BigInt64Array(1)[0] === 'bigint'");
    CHECK_JS_TRUE(in, "(function () { const a = new BigInt64Array([1n, -1n, 2n ** 63n]); return a[0] === 1n && a[1] === -1n && a[2] === -(2n ** 63n) && a.length === 3; })()");
    CHECK_JS_TRUE(in, "(function () { const a = BigUint64Array.of(-1n, 2n ** 64n + 5n); return a[0] === 18446744073709551615n && a[1] === 5n; })()");
    CHECK_JS_TRUE(in, "(function () { const a = new BigInt64Array(2); a[0] = 7n; a[1] = '8'; a[5] = 9n; return a[0] === 7n && a[1] === 8n && a[5] === undefined && a.fill(3n)[1] === 3n; })()");
    CHECK_JS_TRUE(in, "(function () { const v = new DataView(new ArrayBuffer(16)); v.setBigInt64(0, -2n); v.setBigUint64(8, 2n ** 64n - 1n, true); return v.getBigInt64(0) === -2n && v.getBigUint64(0) === 2n ** 64n - 2n && v.getBigUint64(8, true) === 2n ** 64n - 1n && v.getBigInt64(8, true) === -1n && v.getUint8(0) === 255; })()");
    CHECK_JS_TRUE(in, "(function () { const a = new BigInt64Array([3n, -1n, 2n, -5n]); a.sort(); return a.join() === '-5,-1,2,3' && a.includes(2n) && a.indexOf(-1n) === 1 && !a.includes(2); })()");
    CHECK_JS_TRUE(in, "(function () { const a = new BigInt64Array([1n, 2n, 3n]); const m = a.map(x => x * 2n); const s = a.slice(1); const r = a.toReversed(); const w = a.with(0, 9n); return m.join() === '2,4,6' && m instanceof BigInt64Array && s.join() === '2,3' && r.join() === '3,2,1' && w.join() === '9,2,3' && a.reverse().join() === '3,2,1' && a.toSorted().join() === '1,2,3' && a.filter(x => x > 1n).join() === '3,2'; })()");
    CHECK_JS_TRUE(in, "(function () { const a = new BigUint64Array(2); a.set([1n, 2n]); const b = new BigInt64Array(2); b.set(a); return b.join() === '1,2' && Array.from(a).join() === '1,2' && [...a][1] === 2n && a.at(-1) === 2n; })()");
    CHECK_JS_TRUE(in, "(function () { const a = new BigInt64Array([5n]); return a.reduce((x, y) => x + y, 1n) === 6n && a.every(x => typeof x === 'bigint') && a.findLast(x => x === 5n) === 5n && JSON.stringify(Array.from(a, String)) === '[\"5\"]'; })()");
    CHECK_JS_TRUE(in, "(function () { const a = new BigInt64Array(1); Object.defineProperty(a, '0', { value: 4n }); return a[0] === 4n && Reflect.set(a, 0, 6n) && a[0] === 6n && Object.getOwnPropertyDescriptor(a, '0').value === 6n; })()");
    CHECK_JS_THROWS(in, "new BigInt64Array([1])", "TypeError");
    CHECK_JS_THROWS(in, "new BigInt64Array(1).fill(1)", "TypeError");
    CHECK_JS_THROWS(in, "(function () { const a = new BigInt64Array(1); a[0] = 1; })()", "TypeError");
    CHECK_JS_THROWS(in, "new BigInt64Array(new Int8Array(2))", "TypeError");
    CHECK_JS_THROWS(in, "new Int8Array(new BigInt64Array(2))", "TypeError");
    CHECK_JS_THROWS(in, "new BigInt64Array(2).set(new Int8Array(2))", "TypeError");
    CHECK_JS_THROWS(in, "Int8Array.from(new BigInt64Array(1))", "TypeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8)).setBigInt64(0, 1)", "TypeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8)).getBigInt64(1)", "RangeError");
    CHECK_JS_THROWS(in, "(function () { class Bad extends BigInt64Array { static get [Symbol.species]() { return Int8Array; } } new Bad(2).slice(); })()", "TypeError");
    CHECK_JS_THROWS(in, "new BigInt64Array([1n]).with(0, 1)", "TypeError");
}

} // namespace

int main()
{
    test_integers();
    test_language();
    test_typed_arrays();
    return sashfold::test::report("js_bigint");
}
