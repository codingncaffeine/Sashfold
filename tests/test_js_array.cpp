#include "JsTest.h"

#include "js/Interpreter.h"

#include <string>

using namespace sashfold;

namespace {

js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

void test_constructor_and_statics()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "Array.length === 1 && Array.prototype.length === 0 && Array.isArray(Array.prototype) && Array.prototype.constructor === Array");
    CHECK_JS_TRUE(in, "Array(3).length === 3 && new Array(3).length === 3 && !(0 in Array(3))");
    CHECK_JS_TRUE(in, "Array('3').length === 1 && Array('3')[0] === '3' && Array(1, 2).length === 2");
    CHECK_JS_THROWS(in, "new Array(-1)", "RangeError");
    CHECK_JS_THROWS(in, "new Array(1.5)", "RangeError");
    CHECK_JS_THROWS(in, "new Array(4294967296)", "RangeError");
    CHECK_JS_TRUE(in, "Array.isArray([]) && !Array.isArray({ length: 0 })");
    CHECK_JS_TRUE(in, "Array.of(1, 2, 3).length === 3 && Array.of(7)[0] === 7 && Array.of().length === 0");
    CHECK_JS_TRUE(in, "Array.from({ length: 2, 0: 'a', 1: 'b' }).join('') === 'ab' && Array.from('abc').length === 3");
    CHECK_JS_TRUE(in, "Array.from([1, 2, 3], function (x, i) { return x * 2 + i + this.k; }, { k: 10 }).join() === '12,15,18'");
    CHECK_JS_THROWS(in, "Array.from([1], 5)", "TypeError");
    CHECK_JS_THROWS(in, "Array.from(null)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { function F() {} F.prototype = Object.create(Array.prototype); var a = Reflect.construct(Array, [2], F); return a.length === 2 && Object.getPrototypeOf(a) === F.prototype; })()");
    CHECK_JS_TRUE(in, "Array[Symbol.species] === Array && Object.getOwnPropertyDescriptor(Array, Symbol.species).get.name === 'get [Symbol.species]'");
}

void test_mutators()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { var a = [1]; return a.push(2, 3) === 3 && a.join() === '1,2,3' && a.pop() === 3 && a.length === 2 && [].pop() === undefined; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; return a.shift() === 1 && a.join() === '2,3' && a.unshift(0, 0.5) === 4 && a.join() === '0,0.5,2,3' && [].shift() === undefined; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3, 4, 5]; var r = a.splice(1, 2, 'a', 'b', 'c'); return r.join() === '2,3' && a.join() === '1,a,b,c,4,5' && a.splice(-1).join() === '5' && a.splice(1).length === 4 && a.length === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; return a.splice().length === 0 && a.length === 3 && a.splice(1, undefined).length === 0 && a.length === 3; })()");
    CHECK_JS_TRUE(in, "[3, 1, 2].sort().join() === '1,2,3' && [10, 9, 1].sort().join() === '1,10,9' && [3, 1, 2].sort(function (a, b) { return b - a; }).join() === '3,2,1'");
    CHECK_JS_TRUE(in, "(function () { var a = [1, undefined, , 0]; a.sort(); return a.length === 4 && a[0] === 0 && a[1] === 1 && a[2] === undefined && !(3 in a); })()");
    CHECK_JS_TRUE(in, "(function () { var a = [{ k: 1, v: 'a' }, { k: 0, v: 'b' }, { k: 1, v: 'c' }, { k: 0, v: 'd' }]; a.sort(function (x, y) { return x.k - y.k; }); return a.map(function (e) { return e.v; }).join('') === 'bdac'; })()");
    CHECK_JS_THROWS(in, "[1, 2].sort(1)", "TypeError");
    CHECK_JS_THROWS(in, "[2, 1].sort(function () { throw new RangeError('cmp'); })", "RangeError");
    CHECK_JS_TRUE(in, "[1, 2, 3].reverse().join() === '3,2,1' && (function () { var a = [1, , 3]; a.reverse(); return a[0] === 3 && !(1 in a) && a[2] === 1; })()");
    CHECK_JS_TRUE(in, "[1, 2, 3].fill(0).join() === '0,0,0' && [1, 2, 3, 4].fill(9, 1, 3).join() === '1,9,9,4' && [1, 2, 3].fill(7, -1).join() === '1,2,7'");
    CHECK_JS_TRUE(in, "[1, 2, 3, 4, 5].copyWithin(0, 3).join() === '4,5,3,4,5' && [1, 2, 3, 4, 5].copyWithin(1, 3, 4).join() === '1,4,3,4,5' && [1, 2, 3, 4, 5].copyWithin(-2).join() === '1,2,3,1,2'");
    CHECK_JS_TRUE(in, "(function () { var o = { length: 2, 0: 'a', 1: 'b' }; Array.prototype.push.call(o, 'c'); return o.length === 3 && o[2] === 'c'; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { length: 0 }; Array.prototype.pop.call(o); return o.length === 0; })()");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var f = Object.freeze([1]); f.push(2); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var a = { length: 2 ** 53 - 1 }; Array.prototype.push.call(a, 1); })()", "TypeError");
}

void test_accessors()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "[1, 2, 3].at(-1) === 3 && [1, 2, 3].at(0) === 1 && [1].at(5) === undefined && [1].at(-5) === undefined");
    CHECK_JS_TRUE(in, "[1, 2].concat([3, [4]], 5).length === 5 && [1].concat([2, 3])[2] === 3 && [].concat(1, [2], [[3]])[2][0] === 3");
    CHECK_JS_TRUE(in, "(function () { var o = { length: 1, 0: 'x' }; o[Symbol.isConcatSpreadable] = true; return [].concat(o)[0] === 'x' && [].concat(o).length === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2]; a[Symbol.isConcatSpreadable] = false; return [].concat(a)[0] === a; })()");
    CHECK_JS_TRUE(in, "(function () { var r = [1, , 3].concat([4]); return r.length === 4 && !(1 in r); })()");
    CHECK_JS_TRUE(in, "[1, 2, 3].includes(2) && ![1, 2, 3].includes(4) && [NaN].includes(NaN) && [1, 2, 3].includes(3, -1) && ![1, 2, 3].includes(1, 1) && [, ].includes(undefined)");
    CHECK_JS_TRUE(in, "[1, 2, 3, 2].indexOf(2) === 1 && [1, 2, 3, 2].indexOf(2, 2) === 3 && [1, 2].indexOf(5) === -1 && [NaN].indexOf(NaN) === -1 && [1, 2, 3].indexOf(3, -1) === 2 && [, ].indexOf(undefined) === -1");
    CHECK_JS_TRUE(in, "[1, 2, 3, 2].lastIndexOf(2) === 3 && [1, 2, 3, 2].lastIndexOf(2, 2) === 1 && [1, 2, 3].lastIndexOf(3, -2) === -1 && [1].lastIndexOf(1, -Infinity) === -1 && [1, 2].lastIndexOf(2, 5) === 1");
    CHECK_JS_TRUE(in, "[1, 2, 3].join() === '1,2,3' && [1, 2, 3].join('-') === '1-2-3' && [null, undefined, 1].join() === ',,1' && [].join() === '' && [1, [2, 3]].join(';') === '1;2,3'");
    CHECK_JS_TRUE(in, "[1, 2, 3].slice(1).join() === '2,3' && [1, 2, 3].slice(-2).join() === '2,3' && [1, 2, 3].slice(0, -1).join() === '1,2' && [1, 2, 3].slice(5).length === 0 && [1, 2, 3].slice(2, 1).length === 0");
    CHECK_JS_TRUE(in, "String([1, 2]) === '1,2' && [1, [2, [3]]].toString() === '1,2,3' && Array.prototype.toString.call({ join: function () { return 'j'; } }) === 'j' && Array.prototype.toString.call({}) === '[object Object]'");
    CHECK_JS_TRUE(in, "[1, 2].toLocaleString() === '1,2' && [{ toLocaleString() { return 'L'; } }, null].toLocaleString() === 'L,'");
    CHECK_JS_TRUE(in, "[1, [2, [3, [4]]]].flat().length === 3 && [1, [2, [3, [4]]]].flat(Infinity).join() === '1,2,3,4' && [1, [2]].flat(0)[1][0] === 2 && [1, , 3].flat().length === 2");
    CHECK_JS_TRUE(in, "[1, 2].flatMap(function (x) { return [x, x * 2]; }).join() === '1,2,2,4' && [1, 2].flatMap(function (x) { return [[x]]; })[0][0] === 1");
    CHECK_JS_TRUE(in, "[1, 2, 3].toReversed().join() === '3,2,1' && [3, 1, 2].toSorted().join() === '1,2,3' && [1, 2, 3].toSpliced(1, 1, 'x').join() === '1,x,3' && [1, 2, 3].with(1, 9).join() === '1,9,3' && [1, 2, 3].with(-1, 0).join() === '1,2,0'");
    CHECK_JS_THROWS(in, "[1].with(1, 0)", "RangeError");
    CHECK_JS_TRUE(in, "(function () { var a = [1, , 3]; var t = a.toReversed(); return t.length === 3 && 1 in t && t[1] === undefined; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; var s = a.toSorted(); s[0] = 9; return a[0] === 1; })()");
}

void test_iteration_methods()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "[1, 2, 3].map(function (x) { return x * 2; }).join() === '2,4,6' && [1, 2, 3].map(function (x, i, a) { return this.k + i + a.length; }, { k: 1 }).join() === '4,5,6'");
    CHECK_JS_TRUE(in, "(function () { var m = [1, , 3].map(function (x) { return x; }); return m.length === 3 && !(1 in m); })()");
    CHECK_JS_TRUE(in, "[1, 2, 3, 4].filter(function (x) { return x % 2 === 0; }).join() === '2,4' && [1, , 3].filter(function () { return true; }).length === 2");
    CHECK_JS_TRUE(in, "(function () { var s = 0; [1, 2, 3].forEach(function (x) { s += x; }); return s === 6 && [1].forEach(function () {}) === undefined; })()");
    CHECK_JS_TRUE(in, "[1, 2, 3].every(function (x) { return x > 0; }) && ![1, 2, 3].every(function (x) { return x > 1; }) && [].every(function () { return false; })");
    CHECK_JS_TRUE(in, "[1, 2, 3].some(function (x) { return x > 2; }) && ![1, 2, 3].some(function (x) { return x > 3; }) && ![].some(function () { return true; })");
    CHECK_JS_TRUE(in, "[1, 2, 3].find(function (x) { return x > 1; }) === 2 && [1, 2, 3].find(function (x) { return x > 5; }) === undefined && [1, 2, 3].findIndex(function (x) { return x === 3; }) === 2 && [1].findIndex(function () { return false; }) === -1");
    CHECK_JS_TRUE(in, "[1, 2, 3, 4].findLast(function (x) { return x % 2; }) === 3 && [1, 2, 3, 4].findLastIndex(function (x) { return x % 2; }) === 2 && [2].findLast(function (x) { return x > 5; }) === undefined");
    CHECK_JS_TRUE(in, "(function () { var visited = 0; [, , 1].find(function () { visited++; return false; }); return visited === 3; })()");
    CHECK_JS_TRUE(in, "[1, 2, 3].reduce(function (a, b) { return a + b; }) === 6 && [1, 2, 3].reduce(function (a, b) { return a + b; }, 10) === 16 && [[1], [2]].reduce(function (a, b) { return a.concat(b); }, []).join() === '1,2'");
    CHECK_JS_TRUE(in, "[1, 2, 3].reduceRight(function (a, b) { return a + '' + b; }) === '321' && [].reduceRight(function () {}, 'init') === 'init' && [, 5, , 6].reduce(function (a, b) { return a + b; }) === 11");
    CHECK_JS_THROWS(in, "[].reduce(function () {})", "TypeError");
    CHECK_JS_THROWS(in, "[, ,].reduce(function () {})", "TypeError");
    CHECK_JS_THROWS(in, "[1].map(1)", "TypeError");
    CHECK_JS_THROWS(in, "[1].forEach()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; var seen = []; a.forEach(function (x) { seen.push(x); if (x === 1) a.push(4); }); return seen.join() === '1,2,3'; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; var seen = []; a.forEach(function (x) { seen.push(x); if (x === 1) a.pop(); }); return seen.join() === '1,2'; })()");
    CHECK_JS_TRUE(in, "Array.prototype.map.call('ab', function (c) { return c + c; }).join('') === 'aabb' && Array.prototype.filter.call({ length: 2, 0: 'x', 1: 'y' }, function (v) { return v === 'y'; })[0] === 'y'");
    CHECK_JS_TRUE(in, "(function () { function My() {} My.prototype = Object.create(Array.prototype); My[Symbol.species] = Array; var a = [1, 2]; a.constructor = My; return Array.isArray(a.map(function (x) { return x; })) && Object.getPrototypeOf(a.map(function (x) { return x; })) === Array.prototype; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2]; a.constructor = { [Symbol.species]: function (n) { this.made = n; } }; var m = a.map(function (x) { return x; }); return m.made === 2 && m[1] === 2; })()");
    CHECK_JS_THROWS(in, "(function () { var a = [1]; a.constructor = { [Symbol.species]: 1 }; a.map(function () {}); })()", "TypeError");
}

void test_length_and_holes()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { var a = []; a[2] = 1; return a.length === 3 && Object.keys(a).join() === '2' && a.indexOf(undefined) === -1; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; a.length = 0; return a.length === 0 && a[0] === undefined; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; Object.defineProperty(a, 'length', { writable: false }); a.length = 5; return a.length === 3 && !Object.getOwnPropertyDescriptor(a, 'length').writable; })()");
    CHECK_JS_THROWS(in, "(function () { 'use strict'; var a = [1]; Object.defineProperty(a, 'length', { writable: false }); a.push(2); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var a = [1]; Object.defineProperty(a, 5, { value: 'x', configurable: false }); a.length = 0; return a.length === 6; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; a.length = 1; return Object.keys(a).length === 1; })()");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor([], 'length').writable && !Object.getOwnPropertyDescriptor([], 'length').enumerable && !Object.getOwnPropertyDescriptor([], 'length').configurable");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyNames([1, 2]).join() === '0,1,length'");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; delete a[1]; return a.length === 3 && Object.keys(a).join() === '0,2' && a.join() === '1,,3'; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; a[4294967294] = 'max'; return a.length === 4294967295; })()");
    CHECK_JS_TRUE(in, "(function () { var a = []; a[4294967295] = 'not an index'; return a.length === 0 && a[4294967295] === 'not an index'; })()");
    CHECK_JS_TRUE(in, "(function () { var a = [1, 2, 3]; a.length = 2 ** 32 - 1; return a.length === 4294967295 && a[2] === 3; })()");
    CHECK_JS_TRUE(in, "[1, 2, 3].length === 3 && typeof [].length === 'number' && [].concat([]).length === 0");
}

} // namespace

int main()
{
    test_constructor_and_statics();
    test_mutators();
    test_accessors();
    test_iteration_methods();
    test_length_and_holes();
    return sashfold::test::report("js_array");
}
