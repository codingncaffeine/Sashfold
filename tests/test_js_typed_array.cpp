#include "JsTest.h"

#include "js/Interpreter.h"
#include "js/Object.h"

#include <optional>
#include <string>

using namespace sashfold;

namespace {

// Every realm here runs under heap stress: a collection at every
// allocation, so an unrooted value fails the first time.
js::Interpreter& fresh()
{
    static js::Interpreter* interpreter = nullptr;
    delete interpreter;
    interpreter = new js::Interpreter();
    interpreter->heap().set_stress(true);
    return *interpreter;
}

void test_array_buffer()
{
    js::Interpreter& in = fresh();
    CHECK_JS_NUMBER(in, "new ArrayBuffer(8).byteLength", 8);
    CHECK_JS_THROWS(in, "ArrayBuffer(8)", "TypeError");
    CHECK_JS_THROWS(in, "new ArrayBuffer(-1)", "RangeError");
    CHECK_JS_THROWS(in, "new ArrayBuffer(2 ** 53)", "RangeError");
    CHECK_JS_THROWS(in, "new ArrayBuffer(2 ** 40)", "RangeError");
    CHECK_JS_TRUE(in, "new ArrayBuffer().byteLength === 0 && new ArrayBuffer(3.7).byteLength === 3 && new ArrayBuffer('4').byteLength === 4");
    // slice copies a range into a fresh buffer, negative ends included.
    CHECK_JS_TRUE(in, "(function () { var b = new Uint8Array([1, 2, 3, 4, 5]).buffer; var s = b.slice(1, -1); return s.byteLength === 3 && new Uint8Array(s).join() === '2,3,4' && s !== b; })()");
    CHECK_JS_TRUE(in, "(function () { var b = new Uint8Array([1, 2, 3]).buffer; return b.slice(5).byteLength === 0 && b.slice(-1).byteLength === 1 && new Uint8Array(b.slice(-1))[0] === 3; })()");
    CHECK_JS_TRUE(in, "ArrayBuffer.isView(new Uint8Array(1)) && ArrayBuffer.isView(new DataView(new ArrayBuffer(1))) && !ArrayBuffer.isView(new ArrayBuffer(1)) && !ArrayBuffer.isView([]) && !ArrayBuffer.isView()");
    // Resizable buffers grow and shrink within their maximum.
    CHECK_JS_TRUE(in, "(function () { var r = new ArrayBuffer(4, { maxByteLength: 16 }); var before = r.resizable && r.maxByteLength === 16 && r.byteLength === 4; r.resize(12); var grown = r.byteLength === 12; r.resize(2); return before && grown && r.byteLength === 2 && r.maxByteLength === 16; })()");
    CHECK_JS_THROWS(in, "new ArrayBuffer(4, { maxByteLength: 16 }).resize(20)", "RangeError");
    CHECK_JS_THROWS(in, "new ArrayBuffer(8, { maxByteLength: 4 })", "RangeError");
    CHECK_JS_THROWS(in, "new ArrayBuffer(4).resize(2)", "TypeError");
    CHECK_JS_TRUE(in, "!new ArrayBuffer(4).resizable && new ArrayBuffer(4).maxByteLength === 4 && !new ArrayBuffer(4).detached");
    // A grown buffer's new bytes are zero.
    CHECK_JS_TRUE(in, "(function () { var r = new ArrayBuffer(1, { maxByteLength: 4 }); new Uint8Array(r)[0] = 9; r.resize(3); return new Uint8Array(r).join() === '9,0,0'; })()");
    // transfer moves the bytes and detaches the source; the resizability
    // survives transfer and not transferToFixedLength.
    CHECK_JS_TRUE(in, "(function () { var b = new Uint8Array([1, 2, 3]).buffer; var t = b.transfer(); return b.detached && b.byteLength === 0 && b.maxByteLength === 0 && !t.detached && new Uint8Array(t).join() === '1,2,3'; })()");
    CHECK_JS_TRUE(in, "(function () { var b = new Uint8Array([1, 2, 3]).buffer; var t = b.transfer(5); var five = t.byteLength === 5 && new Uint8Array(t).join() === '1,2,3,0,0'; var u = t.transfer(2); return five && t.detached && t.byteLength === 0 && new Uint8Array(u).join() === '1,2'; })()");
    CHECK_JS_THROWS(in, "(function () { var b = new ArrayBuffer(1); b.transfer(); return b.transfer(); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var r = new ArrayBuffer(2, { maxByteLength: 8 }); var t = r.transfer(4); var f = t.transferToFixedLength(); return t.resizable && t.detached && !f.resizable && f.byteLength === 4 && f.maxByteLength === 4; })()");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new ArrayBuffer(1)) === '[object ArrayBuffer]' && ArrayBuffer[Symbol.species] === ArrayBuffer && ArrayBuffer.length === 1 && ArrayBuffer.prototype.slice.length === 2");
    CHECK_JS_THROWS(in, "ArrayBuffer.prototype.byteLength", "TypeError");
    CHECK_JS_THROWS(in, "ArrayBuffer.prototype.slice.call({}, 0)", "TypeError");
    // Species: a subclass slices into itself.
    CHECK_JS_TRUE(in, "(function () { class B extends ArrayBuffer {} var b = new B(4); return b instanceof B && b.slice(0) instanceof B && b.slice(1).byteLength === 3; })()");
    CHECK_JS_THROWS(in, "(function () { var b = new ArrayBuffer(4); b.constructor = { [Symbol.species]: function () { return b; } }; return b.slice(0); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var b = new ArrayBuffer(4); b.constructor = { [Symbol.species]: function () { return new ArrayBuffer(1); } }; return b.slice(0); })()", "TypeError");
}

void test_constructors()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "new Uint8Array(3).length === 3 && new Uint8Array().length === 0 && new Uint8Array(3).join() === '0,0,0'");
    CHECK_JS_STRING(in, "new Int16Array([1, 2, 3]).join()", "1,2,3");
    CHECK_JS_STRING(in, "new Float64Array(new Int8Array([-1, 5])).join()", "-1,5");
    CHECK_JS_STRING(in, "new Uint8Array(new Uint8Array([7, 8])).join()", "7,8");
    CHECK_JS_STRING(in, "new Uint8Array({ length: 2, 0: 7, 1: 300 }).join()", "7,44");
    CHECK_JS_STRING(in, "new Uint8Array(new Set([1, 2])).join()", "1,2");
    // A string is a primitive, so it is a length (NaN → 0), not an iterable.
    CHECK_JS_TRUE(in, "new Uint8Array('ab').length === 0 && new Uint8Array(new String('ab')).join() === '0,0'");
    CHECK_JS_TRUE(in, "new Uint8Array(null).length === 0 && new Uint8Array(undefined).length === 0 && new Uint8Array(1.5).length === 1 && new Uint8Array('2').length === 2");
    // Over a buffer: offset and length, and the alignment rules.
    CHECK_JS_TRUE(in, "(function () { var b = new ArrayBuffer(8); var a = new Uint16Array(b, 2, 2); return a.length === 2 && a.byteOffset === 2 && a.byteLength === 4 && a.buffer === b; })()");
    CHECK_JS_TRUE(in, "(function () { var b = new ArrayBuffer(8); return new Uint16Array(b, 2).length === 3 && new Uint16Array(b, 8).length === 0 && new Uint16Array(b).length === 4; })()");
    CHECK_JS_THROWS(in, "new Uint16Array(new ArrayBuffer(8), 1)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint16Array(new ArrayBuffer(8), 10)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint16Array(new ArrayBuffer(8), 2, 4)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint16Array(new ArrayBuffer(7))", "RangeError");
    CHECK_JS_THROWS(in, "new Uint8Array(new ArrayBuffer(4), -1)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint8Array(-1)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint8Array(2 ** 32)", "RangeError");
    CHECK_JS_THROWS(in, "Uint8Array(1)", "TypeError");
    CHECK_JS_THROWS(in, "new (Object.getPrototypeOf(Int8Array))()", "TypeError");
    CHECK_JS_THROWS(in, "Object.getPrototypeOf(Int8Array)()", "TypeError");
    // The shape: %TypedArray% above every kind, BYTES_PER_ELEMENT on both
    // sides, length 3, and the tag from a getter.
    CHECK_JS_TRUE(in, "(function () { var TA = Object.getPrototypeOf(Int8Array); return TA === Object.getPrototypeOf(Float64Array) && Object.getPrototypeOf(Int8Array.prototype) === TA.prototype && TA.name === 'TypedArray' && TA.length === 0 && TA[Symbol.species] === TA && TA.prototype.constructor === TA; })()");
    CHECK_JS_TRUE(in, "Int8Array.BYTES_PER_ELEMENT === 1 && Float64Array.prototype.BYTES_PER_ELEMENT === 8 && Float16Array.BYTES_PER_ELEMENT === 2 && Uint8ClampedArray.BYTES_PER_ELEMENT === 1 && Int8Array.length === 3 && Int8Array.name === 'Int8Array'");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Uint8Array, 'BYTES_PER_ELEMENT'); var p = Object.getOwnPropertyDescriptor(Uint8Array, 'prototype'); return d.value === 1 && !d.writable && !d.enumerable && !d.configurable && !p.writable && !p.configurable; })()");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new Float32Array(1)) === '[object Float32Array]' && new Uint8Array(1)[Symbol.toStringTag] === 'Uint8Array' && Uint8Array.prototype[Symbol.toStringTag] === undefined && Object.getPrototypeOf(Uint8Array.prototype)[Symbol.toStringTag] === undefined");
    CHECK_JS_TRUE(in, "(function () { class T extends Uint8Array {} var t = new T(2); return t instanceof T && t instanceof Uint8Array && t.length === 2 && Object.getPrototypeOf(t) === T.prototype && new Uint8Array([1]).constructor === Uint8Array; })()");
    // An object argument is asked for @@iterator first and read as an
    // array-like when that is nullish.
    CHECK_JS_TRUE(in, "(function () { var log = []; var a = new Uint8Array({ get [Symbol.iterator]() { log.push('iter'); return undefined; }, get length() { log.push('length'); return 1; }, get 0() { log.push('0'); return 7; } }); return log.join() === 'iter,length,0' && a.length === 1 && a[0] === 7; })()");
}

void test_conversions()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "new Int8Array([127, 128, -129, 255, 256, 1.9, -1.9, NaN, Infinity]).join()", "127,-128,127,-1,0,1,-1,0,0");
    CHECK_JS_STRING(in, "new Uint8Array([255, 256, -1, 1.5, 2 ** 32 + 3, -Infinity]).join()", "255,0,255,1,3,0");
    CHECK_JS_STRING(in, "new Uint8ClampedArray([300, -5, 1.5, 2.5, 0.5, 254.5, 255.5, NaN, 1.4999, Infinity]).join()", "255,0,2,2,0,254,255,0,1,255");
    CHECK_JS_STRING(in, "new Int16Array([32767, 32768, -32769, 65535]).join()", "32767,-32768,32767,-1");
    CHECK_JS_STRING(in, "new Uint16Array([65535, 65536, -1]).join()", "65535,0,65535");
    CHECK_JS_STRING(in, "new Int32Array([2147483647, 2147483648, -2147483649, 4294967295]).join()", "2147483647,-2147483648,2147483647,-1");
    CHECK_JS_STRING(in, "new Uint32Array([4294967295, 4294967296, -1, 2 ** 53]).join()", "4294967295,0,4294967295,0");
    // Float32 rounds to single precision; past the largest single it is
    // infinity from the midpoint on.
    CHECK_JS_TRUE(in, "(function () { var f = new Float32Array([0.1])[0]; return f !== 0.1 && Math.abs(f - 0.1) < 1e-8; })()");
    CHECK_JS_TRUE(in, "new Float32Array([1e40])[0] === Infinity && new Float32Array([-1e40])[0] === -Infinity && new Float32Array([3.4028235677973366e38])[0] === Infinity && new Float32Array([3.4028234663852886e38])[0] === 3.4028234663852886e38");
    CHECK_JS_TRUE(in, "Object.is(new Float32Array([-0])[0], -0) && isNaN(new Float32Array([NaN])[0]) && new Float32Array([1.5])[0] === 1.5");
    CHECK_JS_TRUE(in, "new Float64Array([0.1])[0] === 0.1 && new Float64Array([Infinity])[0] === Infinity && Object.is(new Float64Array([-0])[0], -0) && isNaN(new Float64Array([NaN])[0])");
    // Float16: ties to even, the overflow threshold, the subnormal range.
    CHECK_JS_TRUE(in, "new Float16Array([2049])[0] === 2048 && new Float16Array([2051])[0] === 2052 && new Float16Array([65520])[0] === Infinity && new Float16Array([65504])[0] === 65504 && new Float16Array([65519])[0] === 65504");
    CHECK_JS_TRUE(in, "new Float16Array([2.9802322387695312e-8])[0] === 0 && new Float16Array([5.960464477539063e-8])[0] === 5.960464477539063e-8 && new Float16Array([0.00006103515625])[0] === 0.00006103515625 && new Float16Array([0.00006097555160522461])[0] === 0.00006097555160522461");
    CHECK_JS_TRUE(in, "new Float16Array([1.1])[0] === 1.099609375 && Object.is(new Float16Array([-0])[0], -0) && isNaN(new Float16Array([NaN])[0]) && new Float16Array([-Infinity])[0] === -Infinity && new Float16Array([-1.5])[0] === -1.5");
    // Writes through an index convert the same way, objects through valueOf.
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array(3); a[0] = 257; a[1] = '5'; a[2] = { valueOf: function () { return 9; } }; return a.join() === '1,5,9'; })()");
}

void test_exotic_keys()
{
    js::Interpreter& in = fresh();
    // An index past the end is absent and a write there is dropped; a
    // canonical numeric string that is no index is the same.
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([10, 20]); a[2] = 5; return a[2] === undefined && a.length === 2 && !('2' in a) && Object.keys(a).join() === '0,1'; })()");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([10, 20]); a['-0'] = 7; a['1.5'] = 1; a.Infinity = 1; a.NaN = 3; a['-1'] = 4; return a['-0'] === undefined && !('-0' in a) && a[0] === 10 && a['1.5'] === undefined && !a.hasOwnProperty('1.5') && a.Infinity === undefined && a.NaN === undefined && a['-1'] === undefined && Object.keys(a).length === 2; })()");
    // A name that is not canonical is an ordinary property.
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array(2); a['01'] = 4; a.foo = 'x'; a[' 1'] = 5; return a['01'] === 4 && a.foo === 'x' && a[' 1'] === 5 && Reflect.ownKeys(a).join() === '0,1,01,foo, 1'; })()");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(new Uint8Array([10]), '0'); return d.value === 10 && d.writable && d.enumerable && d.configurable && Object.getOwnPropertyDescriptor(new Uint8Array(1), '5') === undefined; })()");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array(1); return delete a[0] === false && delete a[5] === true && delete a['-0'] === true && a.length === 1; })()");
    CHECK_JS_THROWS(in, "'use strict'; delete new Uint8Array(1)[0]", "TypeError");
    // Defining an element: a value lands (converted), anything that would
    // change the shape is refused.
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array(1); Object.defineProperty(a, '0', { value: 3 }); var one = a[0] === 3; Object.defineProperty(a, '0', { value: { valueOf: function () { return 42; } } }); return one && a[0] === 42 && Object.defineProperty(a, '0', { value: 7, writable: true, enumerable: true, configurable: true }) === a && a[0] === 7; })()");
    CHECK_JS_THROWS(in, "Object.defineProperty(new Uint8Array(1), '0', { configurable: false })", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty(new Uint8Array(1), '0', { enumerable: false })", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty(new Uint8Array(1), '0', { writable: false })", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty(new Uint8Array(1), '0', { get: function () {} })", "TypeError");
    CHECK_JS_THROWS(in, "Object.defineProperty(new Uint8Array(1), '5', { value: 1 })", "TypeError");
    CHECK_JS_TRUE(in, "Reflect.defineProperty(new Uint8Array(1), '5', { value: 1 }) === false && Reflect.defineProperty(new Uint8Array(1), '0', { value: 1 }) === true");
    // An invalid index refuses before the value is looked at.
    CHECK_JS_TRUE(in, "(function () { var called = false; var r = Reflect.defineProperty(new Uint8Array(1), '5', { value: { valueOf: function () { called = true; return 1; } } }); return r === false && !called; })()");
    CHECK_JS_THROWS(in, "Object.freeze(new Uint8Array(1))", "TypeError");
    CHECK_JS_THROWS(in, "Object.seal(new Uint8Array(1))", "TypeError");
    CHECK_JS_TRUE(in, "Object.isFrozen(Object.freeze(new Uint8Array(0))) && !Object.isFrozen(new Uint8Array(1)) && Object.isExtensible(new Uint8Array(1))");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array(1); Object.preventExtensions(a); a.foo = 1; a[0] = 5; return a.foo === undefined && a[0] === 5; })()");
    // Through other receivers: a valid index lands on the receiver, an
    // invalid one is silently fine, and a typed-array receiver converts.
    CHECK_JS_TRUE(in, "(function () { var o = {}; var a = new Uint8Array(2); return Reflect.set(a, '0', 5, o) === true && o[0] === 5 && Reflect.set(a, '5', 5, o) === true && !('5' in o) && a[0] === 0; })()");
    CHECK_JS_TRUE(in, "(function () { var t = new Uint8Array(1); return Reflect.set({}, '0', '7', t) === true && t[0] === 7 && Reflect.set({}, '3', 1, t) === false && t.length === 1; })()");
    CHECK_JS_TRUE(in, "(function () { var proto = new Uint8Array([9]); var o = Object.create(proto); o[0] = 4; return o[0] === 4 && proto[0] === 9 && o.hasOwnProperty('0'); })()");
    // A typed array up the chain decides a numeric key itself: an invalid
    // index creates nothing anywhere and converts nothing, whatever the
    // prototype above it says; a valid one with the typed array as
    // receiver writes the element.
    CHECK_JS_TRUE(in, "(function () { var t = new Uint8Array([0]); var calls = 0; Object.defineProperty(Uint8Array.prototype, '1', { set: function () { throw new Error('unreachable'); }, configurable: true }); var r = Object.create(t); r[1] = { valueOf: function () { calls++; return 2; } }; r['1.5'] = 1; var arr = Object.setPrototypeOf([], t); arr[1] = 5; delete Uint8Array.prototype[1]; return !t.hasOwnProperty(1) && !r.hasOwnProperty(1) && !r.hasOwnProperty('1.5') && calls === 0 && !arr.hasOwnProperty(1) && arr.length === 0; })()");
    CHECK_JS_TRUE(in, "(function () { var t = new Int32Array(2); var o = Object.create(t); var calls = 0; var ok = Reflect.set(o, 5, { valueOf: function () { calls++; return 1; } }, t); return ok && calls === 1 && t.length === 2 && Reflect.set(o, 0, 7, t) && t[0] === 7 && !o.hasOwnProperty('0'); })()");
    // The language around it.
    CHECK_JS_TRUE(in, "JSON.stringify(new Uint8Array([1, 2])) === '{\"0\":1,\"1\":2}' && !Array.isArray(new Uint8Array(1)) && Array.prototype.slice.call(new Uint8Array([1, 2, 3]), 1).join() === '2,3'");
    CHECK_JS_TRUE(in, "(function () { var ks = []; for (var k in new Uint8Array(2)) ks.push(k); return ks.join() === '0,1' && [...new Uint8Array([1, 2])].join() === '1,2' && Array.from(new Uint8Array([3])).join() === '3' && Object.entries(new Uint8Array([5])).join('|') === '0,5'; })()");
    CHECK_JS_TRUE(in, "(function () { var [x, y] = new Int8Array([-1, 2]); return x === -1 && y === 2 && Math.max(...new Uint8Array([1, 9, 3])) === 9; })()");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([1]); return Object.getOwnPropertyNames(a).join() === '0' && a.propertyIsEnumerable('0') && a.propertyIsEnumerable(0) && !a.propertyIsEnumerable('length'); })()");
}

void test_methods()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { var a = new Int8Array([1, 2, 3]); return a.at(-1) === 3 && a.at(0) === 1 && a.at(3) === undefined && a.at(-4) === undefined && a.at('1') === 2; })()");
    CHECK_JS_STRING(in, "new Uint8Array([1, 2, 3, 4, 5]).copyWithin(0, 3).join()", "4,5,3,4,5");
    CHECK_JS_STRING(in, "new Uint8Array([1, 2, 3, 4, 5]).copyWithin(1, 0, 3).join()", "1,1,2,3,5");
    CHECK_JS_STRING(in, "new Uint8Array([1, 2, 3, 4, 5]).copyWithin(-2, -3, -1).join()", "1,2,3,3,4");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([5, 6]); return [...a.entries()].join('|') === '0,5|1,6' && [...a.keys()].join() === '0,1' && [...a.values()].join() === '5,6' && Uint8Array.prototype[Symbol.iterator] === Uint8Array.prototype.values && a[Symbol.iterator]().next().value === 5; })()");
    CHECK_JS_TRUE(in, "new Uint8Array([2, 4]).every(function (v) { return v % 2 === 0; }) && !new Uint8Array([2, 3]).every(function (v) { return v % 2 === 0; }) && new Uint8Array([1, 2]).some(function (v) { return v === 2; }) && !new Uint8Array([1, 2]).some(function (v) { return v === 3; })");
    CHECK_JS_TRUE(in, "new Uint8Array(4).fill(7, 1, 3).join() === '0,7,7,0' && new Uint8Array(3).fill('2').join() === '2,2,2' && new Uint8Array(3).fill(1, -1).join() === '0,0,1' && new Uint8Array(2).fill(300).join() === '44,44'");
    CHECK_JS_TRUE(in, "(function () { var f = new Uint8Array([1, 2, 3, 4]).filter(function (v) { return v > 2; }); return f.join() === '3,4' && f instanceof Uint8Array && f.length === 2; })()");
    CHECK_JS_TRUE(in, "(function () { var a = new Int8Array([1, -2, 3, -4]); function neg(v) { return v < 0; } function big(v) { return v > 10; } return a.find(neg) === -2 && a.findIndex(neg) === 1 && a.findLast(neg) === -4 && a.findLastIndex(neg) === 3 && a.find(big) === undefined && a.findIndex(big) === -1 && a.findLast(big) === undefined && a.findLastIndex(big) === -1; })()");
    CHECK_JS_TRUE(in, "(function () { var s = 0; var seen = []; new Uint8Array([1, 2, 3]).forEach(function (v, i, arr) { s += v * 10 + i + arr.length; seen.push(this.t); }, { t: 'x' }); return s === 72 && seen.join() === 'x,x,x'; })()");
    CHECK_JS_THROWS(in, "new Uint8Array(1).forEach(1)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var a = new Float32Array([1, NaN, 3, 1]); return a.includes(NaN) && a.indexOf(NaN) === -1 && a.lastIndexOf(NaN) === -1 && a.indexOf(1) === 0 && a.lastIndexOf(1) === 3 && a.indexOf(1, 1) === 3 && a.lastIndexOf(1, 2) === 0 && a.indexOf(3, -2) === 2 && !a.includes(2) && a.includes(3, -1) === false && a.lastIndexOf(1, -1) === 3 && a.includes(1, 4) === false && a.indexOf(1, Infinity) === -1 && a.lastIndexOf(1, -Infinity) === -1; })()");
    CHECK_JS_TRUE(in, "new Uint8Array([1, 2]).lastIndexOf(2, undefined) === -1 && new Uint8Array([1, 2]).lastIndexOf(1, undefined) === 0 && new Uint8Array([1, 2]).lastIndexOf(2) === 1 && new Uint8Array(0).indexOf(0) === -1 && new Uint8Array(0).includes(undefined) === false");
    CHECK_JS_TRUE(in, "new Uint8Array([1, 2, 3]).join('-') === '1-2-3' && new Uint8Array([1, 2]).join() === '1,2' && new Uint8Array(0).join() === '' && new Float64Array([0.5, -0]).join() === '0.5,0' && new Uint8Array([1, 2]).join('') === '12'");
    CHECK_JS_TRUE(in, "(function () { var m = new Uint8Array([1, 2]).map(function (v) { return v * 200; }); return m.join() === '200,144' && m instanceof Uint8Array; })()");
    CHECK_JS_TRUE(in, "(function () { function add(a, b) { return a + b; } var a = new Uint8Array([1, 2, 3]); return a.reduce(add) === 6 && a.reduce(add, 10) === 16 && a.reduceRight(function (x, y) { return x + '' + y; }, '') === '321' && a.reduceRight(add) === 6 && new Uint8Array(0).reduce(add, 'i') === 'i'; })()");
    CHECK_JS_THROWS(in, "new Uint8Array(0).reduce(function () {})", "TypeError");
    CHECK_JS_THROWS(in, "new Uint8Array(0).reduceRight(function () {})", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var r = new Uint8Array([1, 2, 3]); return r.reverse() === r && r.join() === '3,2,1' && new Uint8Array([1, 2]).reverse().join() === '2,1' && new Uint8Array(0).reverse().length === 0; })()");
    // set: from an array-like and from another view, overlap and
    // conversion between kinds over one buffer.
    CHECK_JS_TRUE(in, "(function () { var t = new Uint8Array(5); t.set([1, 2], 1); t.set(new Int8Array([-1]), 3); t.set({ length: 1, 0: '9' }); return t.join() === '9,1,2,255,0'; })()");
    CHECK_JS_THROWS(in, "new Uint8Array(5).set([1, 2, 3], 3)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint8Array(5).set([1], -1)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint8Array(5).set([1], Infinity)", "RangeError");
    CHECK_JS_TRUE(in, "(function () { var b = new Uint8Array([1, 2, 3, 4]); b.set(b.subarray(0, 3), 1); return b.join() === '1,1,2,3'; })()");
    CHECK_JS_TRUE(in, "(function () { var buf = new ArrayBuffer(8); var u8 = new Uint8Array(buf); var u16 = new Uint16Array(buf); u8.set([1, 2, 3, 4, 5, 6, 7, 8]); u16.set(new Uint8Array(buf, 0, 4)); return u16.join() === '1,2,3,4'; })()");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([1, 2, 3, 4]); var s = a.slice(1, 3); var c = a.slice(); return s.join() === '2,3' && s.buffer !== a.buffer && a.slice(-2).join() === '3,4' && a.slice(5).length === 0 && c.buffer !== a.buffer && c.join() === '1,2,3,4'; })()");
    // sort: numeric by default, −0 before +0, NaN last, stable with a
    // comparator; the comparator is checked before the receiver.
    CHECK_JS_TRUE(in, "(function () { var s = new Float64Array([3, NaN, -0, 0, -1, Infinity, -Infinity]).sort(); return s.join() === '-Infinity,-1,0,0,3,Infinity,NaN' && Object.is(s[2], -0) && Object.is(s[3], 0); })()");
    CHECK_JS_TRUE(in, "new Uint8Array([1, 10, 2]).sort().join() === '1,2,10' && new Uint8Array([1, 10, 2]).sort(function (a, b) { return b - a; }).join() === '10,2,1' && new Uint8Array([3, 1, 2]).sort(function () { return 0; }).join() === '3,1,2'");
    CHECK_JS_THROWS(in, "new Uint8Array(1).sort(1)", "TypeError");
    CHECK_JS_THROWS(in, "Uint8Array.prototype.sort.call({}, 1)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([1, 2, 3, 4]); var s = a.subarray(1, 3); s[0] = 9; return s.length === 2 && s.buffer === a.buffer && s.byteOffset === 1 && s.join() === '9,3' && a[1] === 9 && a.subarray(-1).join() === '4' && a.subarray(2, 1).length === 0 && a.subarray().length === 4; })()");
    CHECK_JS_TRUE(in, "new Uint8Array([1, 2]).toLocaleString() === '1,2' && new Uint8Array(0).toLocaleString() === ''");
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([3, 1, 2]); return a.toReversed().join() === '2,1,3' && a.toSorted().join() === '1,2,3' && a.toSorted(function (x, y) { return y - x; }).join() === '3,2,1' && a.with(0, 9).join() === '9,1,2' && a.with(-1, 7).join() === '3,1,7' && a.join() === '3,1,2'; })()");
    CHECK_JS_THROWS(in, "new Uint8Array([3, 1, 2]).with(3, 1)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint8Array([3, 1, 2]).with(-4, 1)", "RangeError");
    CHECK_JS_THROWS(in, "new Uint8Array(1).toSorted(1)", "TypeError");
    CHECK_JS_TRUE(in, "new Uint8Array([1, 2]).toString() === '1,2' && Uint8Array.prototype.toString === Array.prototype.toString && Object.getPrototypeOf(Uint8Array.prototype).hasOwnProperty('toString')");
    CHECK_JS_THROWS(in, "Uint8Array.prototype.length", "TypeError");
    CHECK_JS_THROWS(in, "Uint8Array.prototype.join.call([], ',')", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var TA = Object.getPrototypeOf(Uint8Array); var d = Object.getOwnPropertyDescriptor(TA.prototype, 'length'); return d.get.name === 'get length' && d.set === undefined && !d.enumerable && d.configurable && TA.prototype.set.length === 1 && TA.prototype.slice.length === 2 && TA.prototype.subarray.length === 2 && TA.prototype.with.length === 2 && TA.from.length === 1 && TA.of.length === 0; })()");
    // from and of build through `this`, which must be a constructor
    // making a typed array of at least the length asked for.
    CHECK_JS_TRUE(in, "Uint8Array.from([1, 2, 3], function (v) { return v * 2; }).join() === '2,4,6' && Uint8Array.of(1, 2).join() === '1,2' && Int8Array.from(new Set([1])).join() === '1' && Uint8Array.from({ length: 2, 0: 5 }).join() === '5,0' && Uint8Array.from('12').join() === '1,2'");
    CHECK_JS_TRUE(in, "(function () { var seen = []; var r = Uint8Array.from([1, 2], function (v, i) { seen.push(this.p + i); return v + 10; }, { p: 'q' }); return r.join() === '11,12' && seen.join() === 'q0,q1'; })()");
    CHECK_JS_THROWS(in, "Uint8Array.from.call(Object, [1])", "TypeError");
    CHECK_JS_THROWS(in, "Uint8Array.of.call(function () {}, 1)", "TypeError");
    CHECK_JS_THROWS(in, "Uint8Array.from.call({}, [1])", "TypeError");
    CHECK_JS_THROWS(in, "Uint8Array.from([1], 1)", "TypeError");
}

void test_species()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { class T extends Uint8Array {} var t = new T([1, 2, 3]); return t.map(function (v) { return v; }) instanceof T && t.filter(function () { return true; }) instanceof T && t.slice() instanceof T && t.subarray() instanceof T && t.toSorted() instanceof Uint8Array && !(t.toSorted() instanceof T) && t.toReversed().constructor === Uint8Array && t.with(0, 1).constructor === Uint8Array && T.from([1]) instanceof T && T.of(1) instanceof T; })()");
    CHECK_JS_TRUE(in, "(function () { var c = new Uint8Array([300 - 256]); c.constructor = { [Symbol.species]: Int8Array }; var s = c.slice(); return s instanceof Int8Array && s[0] === 44 && c.map(function (v) { return 200; })[0] === -56; })()");
    CHECK_JS_THROWS(in, "(function () { var d = new Uint8Array(2); d.constructor = { [Symbol.species]: function () { return new Uint8Array(1); } }; return d.slice(); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var d = new Uint8Array(2); d.constructor = { [Symbol.species]: function () { return {}; } }; return d.map(function (v) { return v; }); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var d = new Uint8Array(2); d.constructor = { [Symbol.species]: 1 }; return d.slice(); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var d = new Uint8Array([1, 2]); d.constructor = undefined; return d.slice().constructor === Uint8Array && d.subarray(1).join() === '2'; })()");
    // A species view over the same buffer: slice copies with memmove
    // semantics, subarray shares.
    CHECK_JS_TRUE(in, "(function () { var a = new Uint8Array([1, 2, 3, 4]); a.constructor = { [Symbol.species]: function (n) { return new Uint8Array(a.buffer, 0, n); } }; var s = a.slice(1, 3); return s.join() === '2,3' && a.join() === '2,3,3,4'; })()");
    // The copy runs forward one byte at a time, so a result view that
    // starts inside the range smears, as the specification's loop does.
    CHECK_JS_TRUE(in, "(function () { var ta = new Uint8Array([10, 20, 30, 40, 50, 60]); ta.constructor = { [Symbol.species]: function () { return new Uint8Array(ta.buffer, 2); } }; return ta.slice(1, 4).join() === '20,20,20,60'; })()");
    CHECK_JS_TRUE(in, "(function () { var ta = new Uint16Array([10, 20, 30, 40, 50, 60]); ta.constructor = { [Symbol.species]: function () { return new Uint16Array(ta.buffer, 4); } }; return ta.slice(1, 4).join() === '20,20,20,60'; })()");
}

void test_resizable_views()
{
    js::Interpreter& in = fresh();
    // A view with no length over a resizable buffer tracks its end; one
    // with a length goes out of bounds when the buffer shrinks under it
    // and comes back when it grows.
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(2, { maxByteLength: 8 }); var tr = new Uint8Array(rb); var fixed = new Uint8Array(rb, 0, 2); var off = new Uint8Array(rb, 2); var r = []; r.push(tr.length === 2 && off.length === 0); rb.resize(6); r.push(tr.length === 6 && fixed.length === 2 && off.length === 4 && off.byteOffset === 2); rb.resize(1); r.push(tr.length === 1 && fixed.length === 0 && fixed[0] === undefined && fixed.byteLength === 0 && fixed.byteOffset === 0 && off.length === 0 && off.byteOffset === 0 && off.byteLength === 0); rb.resize(4); r.push(fixed.length === 2 && off.length === 2 && off.byteOffset === 2); return r.join() === 'true,true,true,true'; })()");
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var tr = new Uint8Array(rb); var sub = tr.subarray(1); var fixed = tr.subarray(1, 3); rb.resize(8); return sub.length === 7 && fixed.length === 2 && tr.length === 8 && Object.keys(sub).length === 7; })()");
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var tr = new Uint16Array(rb); rb.resize(7); return tr.length === 3 && tr.byteLength === 6; })()");
    CHECK_JS_THROWS(in, "new Uint16Array(new ArrayBuffer(4, { maxByteLength: 8 }), 6)", "RangeError");
    CHECK_JS_THROWS(in, "(function () { var rb = new ArrayBuffer(2, { maxByteLength: 8 }); var fixed = new Uint8Array(rb, 0, 2); rb.resize(1); return fixed.fill(1); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var rb = new ArrayBuffer(2, { maxByteLength: 8 }); var it = new Uint8Array(rb, 0, 2).values(); rb.resize(1); return it.next(); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(2, { maxByteLength: 8 }); var fixed = new Uint8Array(rb, 0, 2); rb.resize(1); return fixed.subarray === Uint8Array.prototype.subarray && Object.keys(fixed).length === 0 && !(0 in fixed) && JSON.stringify(fixed) === '{}'; })()");
    // DataView tracks too, and answers its length with a TypeError once
    // out of bounds.
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(8, { maxByteLength: 8 }); var dvt = new DataView(rb); var a = dvt.byteLength === 8; rb.resize(3); return a && dvt.byteLength === 3; })()");
    CHECK_JS_THROWS(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var dvf = new DataView(rb, 1, 2); rb.resize(2); return dvf.byteLength; })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var dvf = new DataView(rb, 1, 2); rb.resize(2); return dvf.getInt8(0); })()", "TypeError");
    // A length read at the start of a method stands even when a callback
    // shrinks the buffer: the reads past the new end are undefined.
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var tr = new Uint8Array(rb); tr.set([1, 2, 3, 4]); var seen = []; tr.forEach(function (v, i) { seen.push(v); if (i === 1) rb.resize(2); }); return seen.join() === '1,2,,' && tr.length === 2; })()");
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var tr = new Uint8Array(rb); tr.set([1, 2, 3, 4]); var r = tr.map(function (v, i) { if (i === 0) rb.resize(2); return v + 1; }); return r.length === 4 && r.join() === '2,3,0,0' && tr.length === 2; })()");
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var tr = new Uint8Array(rb); tr.set([4, 3, 2, 1]); tr.sort(function (a, b) { rb.resize(2); return a - b; }); return tr.length === 2 && tr.join() === '1,2'; })()");
    CHECK_JS_TRUE(in, "(function () { var rb = new ArrayBuffer(4, { maxByteLength: 8 }); var tr = new Uint8Array(rb); var r = tr.includes(undefined, { valueOf: function () { rb.resize(0); return 0; } }); return r === true && tr.indexOf(undefined) === -1; })()");
}

void test_detach()
{
    js::Interpreter& in = fresh();
    test::run_js(in, "var b = new ArrayBuffer(4); var a = new Uint8Array(b); a[0] = 7; var it = a.values(); var dv = new DataView(b); var half = a.subarray(2);");
    // DetachArrayBuffer from the outside, as a host would.
    std::optional<js::Value> const buffer = in.get(*in.global(), in.key("b"));
    CHECK(buffer && buffer->is_object() && buffer->as_object()->class_id() == js::Object::Class::ArrayBuffer);
    if (buffer && buffer->is_object() && buffer->as_object()->class_id() == js::Object::Class::ArrayBuffer)
        static_cast<js::ArrayBufferObject*>(buffer->as_object())->detach();
    CHECK_JS_TRUE(in, "b.detached && b.byteLength === 0 && b.maxByteLength === 0 && !b.resizable && a.length === 0 && a.byteLength === 0 && a.byteOffset === 0 && half.byteOffset === 0 && a[0] === undefined && !('0' in a) && Object.keys(a).length === 0 && a.buffer === b");
    CHECK_JS_TRUE(in, "(function () { a[0] = 5; return a[0] === undefined && Reflect.defineProperty(a, '0', { value: 1 }) === false && delete a[0] === true && Reflect.set(a, '0', 1) === true; })()");
    CHECK_JS_THROWS(in, "it.next()", "TypeError");
    CHECK_JS_THROWS(in, "a.fill(1)", "TypeError");
    CHECK_JS_THROWS(in, "a.at(0)", "TypeError");
    CHECK_JS_THROWS(in, "a.join()", "TypeError");
    CHECK_JS_THROWS(in, "a.set([1])", "TypeError");
    CHECK_JS_THROWS(in, "a.values()", "TypeError");
    CHECK_JS_THROWS(in, "a.slice()", "TypeError");
    CHECK_JS_THROWS(in, "a.subarray(0)", "TypeError");
    CHECK_JS_THROWS(in, "new Uint8Array(b)", "TypeError");
    CHECK_JS_THROWS(in, "new Uint8Array(a)", "TypeError");
    CHECK_JS_THROWS(in, "new DataView(b)", "TypeError");
    CHECK_JS_THROWS(in, "dv.getInt8(0)", "TypeError");
    CHECK_JS_THROWS(in, "dv.byteLength", "TypeError");
    CHECK_JS_THROWS(in, "dv.byteOffset", "TypeError");
    CHECK_JS_THROWS(in, "b.slice(0)", "TypeError");
    CHECK_JS_THROWS(in, "b.transfer()", "TypeError");
    CHECK_JS_THROWS(in, "new Uint8Array(2).set(a)", "TypeError");
    CHECK_JS_TRUE(in, "dv.buffer === b && a.buffer === b && Object.prototype.toString.call(a) === '[object Uint8Array]' && JSON.stringify(a) === '{}'");
    // A DataView made before the detach keeps its identity only.
    CHECK_JS_TRUE(in, "ArrayBuffer.isView(dv) && ArrayBuffer.isView(a)");
}

void test_data_view()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "(function () { var b = new ArrayBuffer(8); var dv = new DataView(b); dv.setInt16(0, -2); return dv.getInt16(0) === -2 && dv.getUint16(0) === 65534 && dv.getUint8(0) === 255 && dv.getUint8(1) === 254 && dv.getInt16(0, true) === -257 && dv.getUint16(0, true) === 65279; })()");
    CHECK_JS_TRUE(in, "(function () { var dv = new DataView(new ArrayBuffer(8)); dv.setUint32(0, 0x01020304, true); return dv.getUint8(0) === 4 && dv.getUint8(3) === 1 && dv.getUint32(0) === 0x04030201 && dv.getInt32(0, true) === 0x01020304 && dv.getInt32(0) === 0x04030201; })()");
    CHECK_JS_TRUE(in, "(function () { var dv = new DataView(new ArrayBuffer(8)); dv.setFloat64(0, Math.PI); var big = dv.getFloat64(0) === Math.PI && dv.getFloat64(0, true) !== Math.PI; dv.setFloat64(0, Math.PI, true); return big && dv.getFloat64(0, true) === Math.PI; })()");
    CHECK_JS_TRUE(in, "(function () { var dv = new DataView(new ArrayBuffer(8)); dv.setFloat32(4, 1.5, true); dv.setFloat32(0, -0); return dv.getFloat32(4, true) === 1.5 && Object.is(dv.getFloat32(0), -0) && dv.getUint8(0) === 128; })()");
    CHECK_JS_TRUE(in, "(function () { var dv = new DataView(new ArrayBuffer(4)); dv.setFloat16(0, 1.1); dv.setFloat16(2, 65520, true); return dv.getFloat16(0) === 1.099609375 && dv.getFloat16(2, true) === Infinity && dv.getUint8(0) === 0x3c && dv.getUint8(1) === 0x66; })()");
    CHECK_JS_TRUE(in, "(function () { var dv = new DataView(new ArrayBuffer(2)); dv.setUint8(0, 257); dv.setInt8(1, -1); return dv.getUint8(0) === 1 && dv.getUint8(1) === 255 && dv.getInt8(1) === -1 && dv.setUint8(0, 1) === undefined; })()");
    CHECK_JS_TRUE(in, "(function () { var dv = new DataView(new ArrayBuffer(2)); dv.setUint16(0, 1); return dv.getUint8(1) === 1 && dv.getUint8(0) === 0; })()");
    CHECK_JS_TRUE(in, "(function () { var dv = new DataView(new ArrayBuffer(8)); dv.setInt32('1', { valueOf: function () { return 300; } }); return dv.getInt32(1) === 300 && dv.getInt8(0) === 0 && dv.getInt8(1) === 0 && dv.getInt8(4) === 44; })()");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(4)).setInt32(1, 0)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8)).getInt32(5)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8)).getInt8(8)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8)).getInt8(-1)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8)).setInt8(8, 1)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8)).setFloat64(1, 1)", "RangeError");
    CHECK_JS_TRUE(in, "new DataView(new ArrayBuffer(8)).getInt8(7) === 0 && new DataView(new ArrayBuffer(8)).getFloat64(0) === 0");
    CHECK_JS_TRUE(in, "(function () { var b = new ArrayBuffer(8); var dv = new DataView(b, 2); return dv.byteLength === 6 && dv.byteOffset === 2 && dv.buffer === b && new DataView(b, 2, 3).byteLength === 3 && new DataView(b, 8).byteLength === 0 && new DataView(b, 8, 0).byteLength === 0; })()");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8), 9)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8), 2, 7)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView(new ArrayBuffer(8), -1)", "RangeError");
    CHECK_JS_THROWS(in, "new DataView({})", "TypeError");
    CHECK_JS_THROWS(in, "new DataView(new Uint8Array(1))", "TypeError");
    CHECK_JS_THROWS(in, "DataView(new ArrayBuffer(1))", "TypeError");
    CHECK_JS_THROWS(in, "DataView.prototype.getInt8.call({}, 0)", "TypeError");
    CHECK_JS_THROWS(in, "DataView.prototype.buffer", "TypeError");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new DataView(new ArrayBuffer(1))) === '[object DataView]' && DataView.length === 1 && DataView.prototype.getInt8.length === 1 && DataView.prototype.setInt8.length === 2 && DataView.prototype.getFloat32.length === 1 && DataView.prototype.setFloat32.length === 2 && DataView.prototype.getInt8.name === 'getInt8'");
    CHECK_JS_TRUE(in, "(function () { class D extends DataView {} var d = new D(new ArrayBuffer(2)); return d instanceof D && d.byteLength === 2 && Object.getPrototypeOf(d) === D.prototype; })()");
    // A view offset inside a view: the bytes are the buffer's.
    CHECK_JS_TRUE(in, "(function () { var b = new ArrayBuffer(4); new DataView(b, 1).setUint8(0, 9); return new Uint8Array(b).join() === '0,9,0,0' && new Uint8Array(new Uint16Array([1]).buffer).join() === '1,0'; })()");
}

void test_collection()
{
    js::Interpreter& in = fresh();
    // Under stress every allocation collects: the buffer must be reached
    // through its views, and the views through everything that holds them.
    CHECK_JS_NUMBER(in, "(function () { var keep = []; for (var i = 0; i < 50; i++) keep.push(new Uint8Array([i])); return keep.map(function (t) { return t[0]; }).reduce(function (a, b) { return a + b; }); })()", 1225);
    CHECK_JS_TRUE(in, "(function () { var b = new ArrayBuffer(4); var views = [new Uint8Array(b), new Int8Array(b, 1), new DataView(b, 2)]; b = null; views[0][3] = 255; return views[1][2] === -1 && views[2].getUint8(1) === 255 && views[0].buffer === views[2].buffer; })()");
    CHECK_JS_STRING(in, "(function () { var a = new Uint16Array(3); for (var i = 0; i < 3; i++) a[i] = i * 1000; return Array.from(a.entries(), function (e) { return e.join(':'); }).join('|'); })()", "0:0|1:1000|2:2000");
    CHECK_JS_TRUE(in, "(function () { var t = new Float64Array(64); for (var i = 0; i < 64; i++) t[i] = i / 4; var s = t.subarray(32).map(function (v) { return v * 2; }); return s.length === 32 && s[0] === 16 && s[31] === 31.5 && t.slice(60).join() === '15,15.25,15.5,15.75'; })()");
}

} // namespace

int main()
{
    test_array_buffer();
    test_constructors();
    test_conversions();
    test_exotic_keys();
    test_methods();
    test_species();
    test_resizable_views();
    test_detach();
    test_data_view();
    test_collection();
    return test::report("test_js_typed_array");
}
