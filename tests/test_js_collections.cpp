#include "JsTest.h"

#include "js/Interpreter.h"

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

void test_map()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var m = new Map(); m.set('a', 1).set('b', 2); return m.size + ':' + m.get('a') + m.get('b') + m.get('c') + ':' + m.has('a') + m.has('z'); })()", "2:12undefined:truefalse");
    CHECK_JS_STRING(in, "(function () { var m = new Map([['x', 1], ['y', 2]]); m.delete('x'); return m.size + ':' + m.has('x') + ':' + m.delete('nope'); })()", "1:false:false");
    // Keys compare by SameValueZero: NaN finds NaN, −0 is stored as +0,
    // objects by identity, strings by contents.
    CHECK_JS_TRUE(in, "(function () { var m = new Map(); m.set(NaN, 'n'); m.set(-0, 'z'); var k = {}; m.set(k, 'o'); m.set('s' + 'x', 1); return m.get(NaN) === 'n' && m.get(0) === 'z' && Object.is(m.keys().next().value, NaN) && Object.is([...m.keys()][1], 0) && m.get(k) === 'o' && m.get({}) === undefined && m.get('sx') === 1; })()");
    // Insertion order, replacement keeps the place, and iteration sees
    // entries added along the way and skips deleted ones.
    CHECK_JS_STRING(in, "(function () { var m = new Map([[1, 'a'], [2, 'b'], [3, 'c']]); m.set(2, 'B'); m.delete(1); m.set(1, 'A'); return [...m.keys()].join() + '|' + [...m.values()].join() + '|' + [...m].map(function (e) { return e.join('='); }).join(); })()", "2,3,1|B,c,A|2=B,3=c,1=A");
    CHECK_JS_STRING(in, "(function () { var m = new Map([[1, 1], [2, 2]]); var out = []; for (var [k, v] of m) { out.push(k); if (k === 1) { m.delete(2); m.set(3, 3); } } return out.join(); })()", "1,3");
    CHECK_JS_STRING(in, "(function () { var m = new Map([[1, 1], [2, 2], [3, 3]]); var it = m.keys(); it.next(); m.clear(); m.set(4, 4); var out = []; for (var k of it) out.push(k); return out.join() + ':' + m.size; })()", "4:1");
    // forEach with the value, the key and the map; thisArg; live entries.
    CHECK_JS_STRING(in, "(function () { var m = new Map([['a', 1], ['b', 2]]); var out = []; m.forEach(function (v, k, map) { out.push(k + v + (map === m) + this.t); if (k === 'a') map.set('c', 3); }, { t: '!' }); return out.join(); })()", "a1true!,b2true!,c3true!");
    CHECK_JS_THROWS(in, "new Map().forEach(1)", "TypeError");
    // The constructor: requires new, takes any iterable of entry objects
    // through the `set` it finds, and closes the iterator on a bad entry.
    CHECK_JS_THROWS(in, "Map()", "TypeError");
    CHECK_JS_THROWS(in, "new Map([1])", "TypeError");
    CHECK_JS_THROWS(in, "new Map(5)", "TypeError");
    CHECK_JS_STRING(in, "(function () { var log = []; var it = {}; it[Symbol.iterator] = function () { return { next: function () { return { value: 1, done: false }; }, return: function () { log.push('closed'); return {}; } }; }; try { new Map(it); } catch (e) { return e.constructor.name + log.join(); } })()", "TypeErrorclosed");
    CHECK_JS_STRING(in, "(function () { var seen = []; class M extends Map { set(k, v) { seen.push(k); return super.set(k, v * 10); } } var m = new M([['q', 1]]); return seen.join() + m.get('q'); })()", "q10");
    CHECK_JS_TRUE(in, "(function () { var m = new Map(new Map([[1, 2]])); return m.get(1) === 2 && new Map(null).size === 0 && new Map(undefined).size === 0; })()");
    // Prototype shape and tags.
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new Map()) === '[object Map]' && Map.prototype[Symbol.iterator] === Map.prototype.entries && Map.length === 0 && Map.name === 'Map' && Map[Symbol.species] === Map");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(Map.prototype, 'size'); return typeof d.get === 'function' && d.set === undefined && !d.enumerable && d.configurable; })()");
    CHECK_JS_THROWS(in, "Map.prototype.get.call({}, 1)", "TypeError");
    CHECK_JS_THROWS(in, "Map.prototype.size", "TypeError");
    CHECK_JS_THROWS(in, "new Map().keys().next.call(new Set().values())", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var it = new Map().entries(); return Object.prototype.toString.call(it) === '[object Map Iterator]' && it[Symbol.iterator]() === it && Object.getPrototypeOf(Object.getPrototypeOf(it)) === Object.getPrototypeOf(Object.getPrototypeOf([].values())); })()");
    CHECK_JS_TRUE(in, "(function () { var it = new Map([[1, 1]]).keys(); it.next(); var end = it.next(); return end.done && end.value === undefined && it.next().done; })()");
    // getOrInsert and getOrInsertComputed: the value in place, or the one
    // given or computed, stored first; a callback that adds the key
    // itself has its own result overwritten.
    CHECK_JS_STRING(in, "(function () { var m = new Map([['a', 1]]); var out = [m.getOrInsert('a', 9), m.getOrInsert('b', 2), m.get('b'), m.getOrInsertComputed('c', function (k) { return k + '!'; }), m.getOrInsertComputed('c', function () { return 'no'; })]; return out.join() + ':' + m.size; })()", "1,2,2,c!,c!:3");
    CHECK_JS_NUMBER(in, "(function () { var m = new Map(); m.getOrInsertComputed('k', function () { m.set('k', 1); return 2; }); return m.get('k'); })()", 2);
    CHECK_JS_THROWS(in, "new Map().getOrInsertComputed('k', 1)", "TypeError");
    CHECK_JS_THROWS(in, "new WeakMap().getOrInsert(1, 1)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var k = {}; var w = new WeakMap(); return w.getOrInsert(k, 5) === 5 && w.get(k) === 5 && w.getOrInsertComputed(k, function () { return 6; }) === 5; })()");
    // Map.groupBy.
    CHECK_JS_STRING(in, "(function () { var g = Map.groupBy([1, 2, 3, 4, 5], function (n, i) { return n % 2 ? 'odd' : 'even'; }); return [...g.keys()].join() + ':' + g.get('odd').join() + ':' + g.get('even').join(); })()", "odd,even:1,3,5:2,4");
    CHECK_JS_STRING(in, "(function () { var g = Object.groupBy('aab', function (c) { return c; }); return Object.getPrototypeOf(g) === null ? Object.keys(g).join() + g.a.length + g.b.length : 'proto'; })()", "a,b21");
    // Many entries with churn: the table compacts while an iterator is mid-walk and the iterator keeps its place.
    CHECK_JS_STRING(in, "(function () { var m = new Map(); for (var i = 0; i < 100; i++) m.set(i, i); var it = m.keys(); var first = it.next().value; for (var j = 0; j < 90; j++) m.delete(j + 1); m.set(200, 200); var rest = []; for (var k of it) rest.push(k); return first + ':' + rest.length + ':' + rest[0] + ':' + rest[rest.length - 1] + ':' + m.size; })()", "0:10:91:200:11");
    CHECK_JS_NUMBER(in, "(function () { var m = new Map(); for (var i = 0; i < 50; i++) m.set('k' + i, i); for (var i = 0; i < 50; i++) m.delete('k' + i); for (var i = 0; i < 5; i++) m.set(i, i); var n = 0; m.forEach(function () { n++; }); return n + m.size; })()", 10);
}

void test_set()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var s = new Set([1, 2, 2, 3]); s.add(4).add(1); return s.size + ':' + [...s].join() + ':' + s.has(2) + s.has(9) + ':' + s.delete(2) + s.delete(2) + ':' + [...s.keys()].join(); })()", "4:1,2,3,4:truefalse:truefalse:1,3,4");
    CHECK_JS_TRUE(in, "(function () { var s = new Set(); s.add(-0); s.add(NaN); s.add(NaN); return s.size === 2 && Object.is([...s][0], 0) && s.has(0) && s.has(NaN); })()");
    CHECK_JS_STRING(in, "(function () { var s = new Set('hello'); return [...s].join('') + ':' + [...s.entries()].map(function (e) { return e[0] + e[1]; }).join(); })()", "helo:hh,ee,ll,oo");
    CHECK_JS_STRING(in, "(function () { var s = new Set([1, 2]); var out = []; s.forEach(function (v, k, set) { out.push(v + '=' + k + (set === s)); }); return out.join(); })()", "1=1true,2=2true");
    CHECK_JS_TRUE(in, "Set.prototype.keys === Set.prototype.values && Set.prototype[Symbol.iterator] === Set.prototype.values && Object.prototype.toString.call(new Set()) === '[object Set]' && Set[Symbol.species] === Set");
    CHECK_JS_THROWS(in, "Set()", "TypeError");
    CHECK_JS_THROWS(in, "new Set(1)", "TypeError");
    CHECK_JS_THROWS(in, "Set.prototype.add.call(new Map(), 1)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var it = new Set([1]).values(); return Object.prototype.toString.call(it) === '[object Set Iterator]' && it.next().value === 1 && it.next().done; })()");
    // A subclass's add is what the constructor feeds.
    CHECK_JS_STRING(in, "(function () { var log = []; class S extends Set { add(v) { log.push(v); return super.add(v); } } var s = new S([1, 2]); return log.join() + ':' + s.size + ':' + (s instanceof S); })()", "1,2:2:true");
    // Deleting during iteration, and adding: the walk sees the new ones.
    CHECK_JS_STRING(in, "(function () { var s = new Set([1, 2, 3]); var out = []; for (var v of s) { out.push(v); if (v === 1) { s.delete(2); s.add(5); } } return out.join(); })()", "1,3,5");
    // The ES2025 methods over sets and set-likes.
    std::string const sets = "var a = new Set([1, 2, 3]); var b = new Set([3, 4]); var like = { size: 2, has: function (v) { return v === 2 || v === 9; }, keys: function () { return [2, 9][Symbol.iterator](); } }; ";
    CHECK_JS_STRING(in, sets + "[...a.union(b)].join() + '|' + [...a.union(like)].join()", "1,2,3,4|1,2,3,9");
    CHECK_JS_STRING(in, sets + "[...a.intersection(b)].join() + '|' + [...a.intersection(like)].join() + '|' + [...b.intersection(a)].join()", "3|2|3");
    CHECK_JS_STRING(in, sets + "[...a.difference(b)].join() + '|' + [...a.difference(like)].join() + '|' + [...b.difference(a)].join()", "1,2|1,3|4");
    CHECK_JS_STRING(in, sets + "[...a.symmetricDifference(b)].join() + '|' + [...a.symmetricDifference(like)].join()", "1,2,4|1,3,9");
    CHECK_JS_STRING(in, sets + "[a.isSubsetOf(b), new Set([3]).isSubsetOf(b), a.isSupersetOf(new Set([1])), a.isSupersetOf(b), a.isDisjointFrom(b), a.isDisjointFrom(new Set([7])), a.isDisjointFrom(like)].join()", "false,true,true,false,false,true,false");
    CHECK_JS_TRUE(in, sets + "a.union(b) !== a && a.union(b) instanceof Set && a.size === 3");
    CHECK_JS_THROWS(in, "new Set().union(1)", "TypeError");
    CHECK_JS_THROWS(in, "new Set().union({ size: NaN, has: function () {}, keys: function () {} })", "TypeError");
    CHECK_JS_THROWS(in, "new Set().union({ size: -1, has: function () {}, keys: function () {} })", "RangeError");
    CHECK_JS_THROWS(in, "new Set().union({ size: 1, has: 1, keys: function () {} })", "TypeError");
    CHECK_JS_THROWS(in, "new Set().union({ size: 1, has: function () {}, keys: null })", "TypeError");
    // Stopping early closes the other's keys iterator; a size that
    // settles the answer means the keys are never asked for.
    CHECK_JS_STRING(in, "(function () { var log = []; var like = { size: 2, has: function () { return true; }, keys: function () { return { next: function () { return { value: 7, done: false }; }, return: function () { log.push('closed'); return {}; } }; } }; return new Set([1, 2]).isSupersetOf(like) + log.join(); })()", "falseclosed");
    CHECK_JS_STRING(in, "(function () { var log = []; var like = { size: 5, has: function () { return true; }, keys: function () { log.push('keys'); return { next: function () { return { value: 7, done: false }; } }; } }; return new Set([1, 2]).isSupersetOf(like) + log.join(); })()", "false");
    CHECK_JS_STRING(in, "(function () { var log = []; var like = { size: 1, has: function () { return true; }, keys: function () { return { next: function () { return { value: 1, done: false }; }, return: function () { log.push('closed'); return {}; } }; } }; return new Set([1, 2]).isDisjointFrom(like) + log.join(); })()", "falseclosed");
}

void test_weak_collections()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { var k = {}, s = Symbol('s'); var m = new WeakMap([[k, 1]]); m.set(s, 2); return m.get(k) + m.get(s) + ':' + m.has(k) + m.has({}) + ':' + m.delete(k) + m.has(k) + ':' + m.get({}); })()", "3:truefalse:truefalse:undefined");
    CHECK_JS_THROWS(in, "new WeakMap().set(1, 1)", "TypeError");
    CHECK_JS_THROWS(in, "new WeakMap().set('k', 1)", "TypeError");
    CHECK_JS_THROWS(in, "new WeakMap().set(Symbol.for('registered'), 1)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var m = new WeakMap(); m.set(Symbol.iterator, 1); return m.get(Symbol.iterator) === 1 && !m.has(1) && !m.delete(1); })()");
    CHECK_JS_THROWS(in, "WeakMap()", "TypeError");
    CHECK_JS_THROWS(in, "new WeakMap([[1, 1]])", "TypeError");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new WeakMap()) === '[object WeakMap]' && !('size' in WeakMap.prototype) && !('forEach' in WeakMap.prototype) && !(Symbol.iterator in WeakMap.prototype)");
    CHECK_JS_STRING(in, "(function () { var o = {}; var s = new WeakSet([o]); return s.has(o) + ':' + s.has({}) + ':' + s.add(o).has(o) + ':' + s.delete(o) + s.has(o); })()", "true:false:true:truefalse");
    CHECK_JS_THROWS(in, "new WeakSet().add(1)", "TypeError");
    CHECK_JS_THROWS(in, "new WeakSet([1])", "TypeError");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(new WeakSet()) === '[object WeakSet]' && WeakSet.length === 0");
    CHECK_JS_THROWS(in, "WeakMap.prototype.get.call(new Map(), {})", "TypeError");
}

} // namespace

int main()
{
    test_map();
    test_set();
    test_weak_collections();
    return sashfold::test::report("js_collections");
}
