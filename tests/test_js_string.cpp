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
    CHECK_JS_TRUE(in, "String() === '' && String(1) === '1' && String(null) === 'null' && String([1, 2]) === '1,2' && String(Symbol('s')) === 'Symbol(s)'");
    CHECK_JS_THROWS(in, "new String(Symbol())", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var s = new String('ab'); return typeof s === 'object' && s.length === 2 && s[0] === 'a' && s.valueOf() === 'ab' && s + '' === 'ab' && Object.keys(s).join() === '0,1'; })()");
    CHECK_JS_TRUE(in, "String.length === 1 && String.prototype.length === 0 && String.prototype.constructor === String && Object.prototype.toString.call(String.prototype) === '[object String]'");
    CHECK_JS_TRUE(in, "String.fromCharCode(72, 105) === 'Hi' && String.fromCharCode(0x10041) === 'A' && String.fromCharCode() === ''");
    CHECK_JS_TRUE(in, "String.fromCodePoint(0x1F600).length === 2 && String.fromCodePoint(65, 66) === 'AB'");
    CHECK_JS_THROWS(in, "String.fromCodePoint(0x110000)", "RangeError");
    CHECK_JS_THROWS(in, "String.fromCodePoint(1.5)", "RangeError");
    CHECK_JS_TRUE(in, "String.raw({ raw: ['a', 'b', 'c'] }, 1, 2) === 'a1b2c' && String.raw({ raw: 'xy' }, '-') === 'x-y' && String.raw({ raw: [] }) === ''");
    // Tagged templates: the tag gets the site's frozen strings, with the
    // raw ones on `raw`, and the substitutions; the same object every
    // time the site runs; a member tag keeps its object as this.
    CHECK_JS_STRING(in, "(function () { function tag(s, ...v) { return s.join('|') + '#' + s.raw.join('|') + '#' + v.join(','); } return tag`a${1}b\\n${2}c`; })()", "a|b\n|c#a|b\\n|c#1,2");
    CHECK_JS_TRUE(in, "(function () { function id(s) { return s; } function site() { return id`x${1}y`; } var a = site(), b = site(); return a === b && a !== id`x${1}y` && Object.isFrozen(a) && Object.isFrozen(a.raw) && a.length === 2 && a.raw.length === 2; })()");
    CHECK_JS_STRING(in, "String.raw`a\\n${1 + 1}b`", "a\\n2b");
    CHECK_JS_STRING(in, "(function () { var o = { n: 'o', tag(s, v) { return this.n + s[0] + v; } }; return o.tag`t${9}` + o['tag']`u${8}`; })()", "ot9ou8");
    // An invalid escape leaves the cooked string undefined and keeps the raw text.
    CHECK_JS_TRUE(in, "(function () { function tag(s) { return s[0] === undefined && s.raw[0] === '\\\\unicode' && s[1] === '!'; } return tag`\\unicode${0}!`; })()");
    CHECK_JS_THROWS(in, "`\\unicode`", "SyntaxError");
    CHECK_JS_THROWS(in, "(1)`x`", "TypeError");
    CHECK_JS_THROWS(in, "a?.b`x`", "SyntaxError");
    CHECK_JS_NUMBER(in, "(function () { function tag() { return arguments.length; } return tag`` + tag`${1}${2}`; })()", 4);
    CHECK_JS_NUMBER(in, "(function () { function f(s) { return s.raw[0]; } return f`\\u{41}${0}`.length * 10 + f`${0}`.length; })()", 60);
    CHECK_JS_THROWS(in, "String.prototype.toString.call(1)", "TypeError");
    CHECK_JS_TRUE(in, "String.prototype.valueOf.call(new String('v')) === 'v' && 'x'.toString() === 'x'");
}

void test_access_and_search()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "'abc'.charAt(1) === 'b' && 'abc'.charAt(5) === '' && 'abc'.charAt(-1) === '' && 'abc'.charAt() === 'a'");
    CHECK_JS_TRUE(in, "'abc'.charCodeAt(1) === 98 && Number.isNaN('abc'.charCodeAt(9)) && 'abc'.at(-1) === 'c' && 'abc'.at(9) === undefined");
    CHECK_JS_TRUE(in, "'\\ud83d\\ude00'.codePointAt(0) === 0x1F600 && '\\ud83d\\ude00'.codePointAt(1) === 0xDE00 && 'a'.codePointAt(1) === undefined");
    CHECK_JS_TRUE(in, "'abc'.indexOf('c') === 2 && 'abc'.indexOf('') === 0 && 'abc'.indexOf('', 9) === 3 && 'abcabc'.indexOf('b', 2) === 4 && 'abc'.indexOf('z') === -1");
    CHECK_JS_TRUE(in, "'abcabc'.lastIndexOf('b') === 4 && 'abcabc'.lastIndexOf('b', 3) === 1 && 'abc'.lastIndexOf('') === 3 && 'abc'.lastIndexOf('a', -5) === 0 && 'aaa'.lastIndexOf('a', NaN) === 2");
    CHECK_JS_TRUE(in, "'abc'.includes('bc') && !'abc'.includes('bc', 2) && 'abc'.includes('') && 'abc'.startsWith('ab') && 'abc'.startsWith('bc', 1) && 'abc'.endsWith('bc') && 'abc'.endsWith('ab', 2) && !'abc'.endsWith('abcd')");
    CHECK_JS_THROWS(in, "'abc'.includes(/b/)", "TypeError");
    CHECK_JS_THROWS(in, "'abc'.startsWith(/a/)", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var re = /a/; re[Symbol.match] = false; return 'a/a/'.includes(re); })()");
    CHECK_JS_TRUE(in, "'abc'.slice(1) === 'bc' && 'abc'.slice(-2) === 'bc' && 'abc'.slice(1, -1) === 'b' && 'abc'.slice(2, 1) === '' && 'abc'.substring(2, 0) === 'ab' && 'abc'.substring(-1, 1) === 'a' && 'abc'.substr(1, 1) === 'b' && 'abc'.substr(-2) === 'bc' && 'abc'.substr(1, -1) === ''");
    CHECK_JS_TRUE(in, "'a,b,,c'.split(',').length === 4 && 'abc'.split('').join('-') === 'a-b-c' && 'abc'.split().length === 1 && 'abc'.split(undefined)[0] === 'abc' && 'a,b'.split(',', 1).length === 1 && ''.split('').length === 0 && ''.split('x').length === 1 && 'abc'.split('', 2).join() === 'a,b' && 'ab'.split('', 0).length === 0");
    CHECK_JS_TRUE(in, "'a b  c'.split(' ').length === 4 && 'abc'.split('abc').length === 2 && 'abc'.split('abc')[0] === '' && 'aXbXc'.split('X').join() === 'a,b,c'");
    CHECK_JS_TRUE(in, "'abc'.concat('d', 1, null) === 'abcd1null' && 'ab'.repeat(3) === 'ababab' && 'ab'.repeat(0) === '' && ''.repeat(5) === ''");
    CHECK_JS_THROWS(in, "'a'.repeat(-1)", "RangeError");
    CHECK_JS_THROWS(in, "'a'.repeat(Infinity)", "RangeError");
    CHECK_JS_TRUE(in, "'5'.padStart(3, '0') === '005' && '5'.padEnd(3) === '5  ' && 'abc'.padStart(6, '12') === '121abc' && 'abc'.padStart(2) === 'abc' && 'abc'.padStart(5, '') === 'abc'");
    CHECK_JS_TRUE(in, "'  a \\t\\n'.trim() === 'a' && '  a  '.trimStart() === 'a  ' && '  a  '.trimEnd() === '  a' && String.prototype.trimLeft === String.prototype.trimStart && String.prototype.trimRight === String.prototype.trimEnd && '\\u00a0\\ufeffx\\u2028'.trim() === 'x'");
    CHECK_JS_TRUE(in, "'aBc'.toUpperCase() === 'ABC' && 'aBc'.toLowerCase() === 'abc' && 'ß'.toUpperCase() === 'ß' && '\\u00e9'.toUpperCase() === '\\u00c9' && 'x'.toLocaleUpperCase() === 'X' && 'X'.toLocaleLowerCase() === 'x'");
    CHECK_JS_TRUE(in, "'a'.localeCompare('b') === -1 && 'b'.localeCompare('a') === 1 && 'a'.localeCompare('a') === 0");
    CHECK_JS_TRUE(in, "'\\u0041\\u030a'.normalize() === '\\u00c5' && '\\u00c5'.normalize('NFD') === '\\u0041\\u030a' && 'abc'.normalize('NFKC') === 'abc' && '\\uac00'.normalize('NFD').length === 2 && '\\u1100\\u1161'.normalize('NFC') === '\\uac00'");
    CHECK_JS_THROWS(in, "'a'.normalize('NFX')", "RangeError");
    CHECK_JS_TRUE(in, "'ab'.isWellFormed() && !'\\ud800'.isWellFormed() && '\\ud800x'.toWellFormed() === '\\ufffdx' && 'ok'.toWellFormed() === 'ok'");
    CHECK_JS_TRUE(in, "'x'.anchor('a\"b') === '<a name=\"a&quot;b\">x</a>' && 'x'.bold() === '<b>x</b>' && 'x'.link('u') === '<a href=\"u\">x</a>' && 'x'.fontsize(3) === '<font size=\"3\">x</font>' && 'x'.sub() === '<sub>x</sub>'");
    CHECK_JS_THROWS(in, "String.prototype.trim.call(null)", "TypeError");
    CHECK_JS_TRUE(in, "String.prototype.indexOf.call(123, '2') === 1 && String.prototype.slice.call(true, 1) === 'rue'");
}

void test_replace_and_match()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "'aXbXc'.replace('X', '-') === 'a-bXc' && 'aXbXc'.replaceAll('X', '-') === 'a-b-c' && 'abc'.replace('z', 'y') === 'abc' && 'abc'.replace('', '-') === '-abc' && 'abc'.replaceAll('', '-') === '-a-b-c-'");
    CHECK_JS_TRUE(in, "'abc'.replace('b', '[$&]') === 'a[b]c' && 'abc'.replace('b', '$`|$\\'') === 'aa|cc' && 'abc'.replace('b', '$$') === 'a$c' && 'abc'.replace('b', '$1') === 'a$1c' && 'abc'.replace('b', '$<x>') === 'a$<x>c'");
    CHECK_JS_TRUE(in, "'abc'.replace('b', function (m, p, s) { return '[' + m + p + s + ']'; }) === 'a[b1abc]c'");
    CHECK_JS_TRUE(in, "'a1b2'.replace(/\\d/, 'N') === 'aNb2' && 'a1b2'.replace(/\\d/g, 'N') === 'aNbN' && 'a1b22'.replace(/(\\d)(\\d)?/g, '$2$1') === 'a1b22'.replace(/(\\d)(\\d)?/g, function (m, a, b) { return (b === undefined ? '' : b) + a; })");
    CHECK_JS_TRUE(in, "'2026-09-04'.replace(/(?<y>\\d+)-(?<m>\\d+)-(?<d>\\d+)/, '$<d>/$<m>/$<y>') === '04/09/2026' && 'x'.replace(/(?<n>x)/, function () { return typeof arguments[arguments.length - 1]; }) === 'object'");
    CHECK_JS_TRUE(in, "'aaa'.replace(/a/g, function () { return '$&'; }) === '$&$&$&' && 'abc'.replace(/(b)/, '$1$1') === 'abbc' && 'abc'.replace(/b/, '$0') === 'a$0c' && 'abc'.replace(/(b)/, '$01') === 'abc' && 'abc'.replace(/(b)/, '$10') === 'ab0c'");
    CHECK_JS_TRUE(in, "'xAx'.replaceAll(/a/gi, 'b') === 'xbx'");
    CHECK_JS_THROWS(in, "'x'.replaceAll(/a/, 'b')", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var m = 'abc'.match(/b/); return m[0] === 'b' && m.index === 1 && m.input === 'abc' && m.length === 1 && m.groups === undefined; })()");
    CHECK_JS_TRUE(in, "'a1b2c3'.match(/\\d/g).join() === '1,2,3' && 'abc'.match(/z/g) === null && 'abc'.match(/z/) === null && 'abc'.match('b').index === 1 && 'a.c'.match('.').index === 0 && 'abc'.match().index === 0");
    CHECK_JS_TRUE(in, "'aaa'.match(/a*?/g).length === 4 && 'abc'.match(/(?:)/g).length === 4");
    CHECK_JS_TRUE(in, "'abc'.search(/c/) === 2 && 'abc'.search('z') === -1 && 'abc'.search() === 0 && (function () { var r = /b/g; r.lastIndex = 2; var s = 'abc'.search(r); return s === 1 && r.lastIndex === 2; })()");
    CHECK_JS_TRUE(in, "'a1b22c'.split(/\\d+/).join() === 'a,b,c' && 'a1b'.split(/(\\d)/).join() === 'a,1,b' && 'abc'.split(/(?:)/).join() === 'a,b,c' && ''.split(/x/).length === 1 && ''.split(/(?:)/).length === 0 && 'a,b,c'.split(/,/, 2).join() === 'a,b'");
    CHECK_JS_TRUE(in, "(function () { var o = { [Symbol.replace](s, r) { return s + '|' + r; } }; return 'str'.replace(o, 'rep') === 'str|rep'; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { [Symbol.split](s, l) { return [s, l]; } }; return 'str'.split(o, 3)[1] === 3; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { [Symbol.search]() { return 42; } }; return 'str'.search(o) === 42; })()");
    CHECK_JS_TRUE(in, "(function () { var o = { [Symbol.match]() { return 'matched'; } }; return 'str'.match(o) === 'matched'; })()");
    CHECK_JS_THROWS(in, "String.prototype.replace.call(null, 'a', 'b')", "TypeError");
}

void test_regexp()
{
    js::Interpreter& in = fresh();
    CHECK_JS_TRUE(in, "/a/.source === 'a' && /a/gi.flags === 'gi' && /a/y.sticky && /a/s.dotAll && /a/u.unicode && /a/m.multiline && /a/i.ignoreCase && !/a/.global && /a/g.global && /a/.hasIndices === false");
    CHECK_JS_TRUE(in, "new RegExp('a', 'g').global && new RegExp('a').flags === '' && RegExp('a').source === 'a' && new RegExp().source === '(?:)' && new RegExp('/').source === '\\\\/' && new RegExp('\\n').source === '\\\\n'");
    CHECK_JS_TRUE(in, "(function () { var r = /x/g; return RegExp(r) === r && new RegExp(r) !== r && new RegExp(r).flags === 'g' && new RegExp(r, 'i').flags === 'i' && RegExp(r, 'i') !== r; })()");
    CHECK_JS_TRUE(in, "String(/a\\/b/) === '/a\\\\/b/' && /a/gimsuy.flags === 'gimsuy' && RegExp.prototype.toString.call({ source: 'S', flags: 'F' }) === '/S/F'");
    CHECK_JS_THROWS(in, "new RegExp('(')", "SyntaxError");
    CHECK_JS_THROWS(in, "new RegExp('a', 'gg')", "SyntaxError");
    CHECK_JS_THROWS(in, "new RegExp('a', 'x')", "SyntaxError");
    CHECK_JS_THROWS(in, "/(/", "SyntaxError");
    CHECK_JS_TRUE(in, "/a/.test('cat') && !/a/.test('dog') && /^a$/.test('a') && /a/i.test('A') && /a.c/s.test('a\\nc') && !/a.c/.test('a\\nc')");
    CHECK_JS_TRUE(in, "(function () { var r = /a/g; var s = 'aXa'; return r.exec(s).index === 0 && r.lastIndex === 1 && r.exec(s).index === 2 && r.lastIndex === 3 && r.exec(s) === null && r.lastIndex === 0; })()");
    CHECK_JS_TRUE(in, "(function () { var r = /a/y; r.lastIndex = 1; return r.exec('ba') !== null && r.lastIndex === 2 && r.exec('ba') === null && r.lastIndex === 0; })()");
    CHECK_JS_TRUE(in, "(function () { var r = /a/; r.lastIndex = 5; return r.exec('a') !== null && r.lastIndex === 5; })()");
    CHECK_JS_TRUE(in, "(function () { var m = /(\\d+)-(\\d+)?/.exec('12-'); return m[0] === '12-' && m[1] === '12' && m[2] === undefined && m.length === 3 && m.index === 0; })()");
    CHECK_JS_TRUE(in, "(function () { var m = /(?<year>\\d{4})/.exec('in 2026'); return m.groups.year === '2026' && Object.getPrototypeOf(m.groups) === null && m[1] === '2026'; })()");
    CHECK_JS_TRUE(in, "/\\bfoo\\b/.test('a foo b') && /[a-c]+/.exec('xxabcx')[0] === 'abc' && /(a)|(b)/.exec('b')[1] === undefined && /a{2,3}/.exec('aaaa')[0] === 'aaa'");
    CHECK_JS_TRUE(in, "(function () { var d = Object.getOwnPropertyDescriptor(/a/, 'lastIndex'); return d.writable && !d.enumerable && !d.configurable && d.value === 0; })()");
    CHECK_JS_TRUE(in, "Object.getOwnPropertyDescriptor(RegExp.prototype, 'global').get.call(RegExp.prototype) === undefined && RegExp.prototype.source === '(?:)' && RegExp.prototype.flags === ''");
    CHECK_JS_THROWS(in, "Object.getOwnPropertyDescriptor(RegExp.prototype, 'global').get.call({})", "TypeError");
    CHECK_JS_THROWS(in, "RegExp.prototype.exec.call({}, 'a')", "TypeError");
    CHECK_JS_TRUE(in, "(function () { var r = /a/; r.exec = function () { return { 0: 'custom', index: 0, length: 1 }; }; return r.test('zzz') && 'zzz'.replace(r, 'X') === 'X'; })()");
    CHECK_JS_TRUE(in, "(function () { var r = /a/; r.compile('b', 'g'); return r.source === 'b' && r.global && r.test('b'); })()");
    CHECK_JS_TRUE(in, "RegExp[Symbol.species] === RegExp && RegExp.length === 2 && RegExp.prototype[Symbol.replace].length === 2 && RegExp.prototype[Symbol.split].name === '[Symbol.split]'");
    CHECK_JS_TRUE(in, "/(?=a)/.exec('ba').index === 1 && /a(?!b)/.exec('abac').index === 2 && /\\u{1F600}/u.test('\\ud83d\\ude00') && /^.$/u.test('\\ud83d\\ude00') && !/^.$/.test('\\ud83d\\ude00')");
    CHECK_JS_TRUE(in, "'x'.replace(/x/, '$`') === '' && 'ab'.replace(/(?<n>a)/, '[$<n>]') === '[a]b' && 'ab'.replace(/(?<n>a)/, '[$<z>]') === '[]b'");
    CHECK_JS_TRUE(in, "(function () { var count = 0; 'aaa'.replace(/a/g, function () { count++; return ''; }); return count === 3; })()");
    CHECK_JS_TRUE(in, "Object.prototype.toString.call(/a/) === '[object RegExp]' && typeof /a/ === 'object' && /a/ instanceof RegExp && (/a/).constructor === RegExp");
    CHECK_JS_TRUE(in, "(function () { var a = /x/; var b = /x/; return a !== b; })()");
    CHECK_JS_TRUE(in, "(function () { var r = /a/gi; var copy = new RegExp(r); copy.lastIndex = 3; return r.lastIndex === 0 && copy.flags === 'gi'; })()");
}

} // namespace

int main()
{
    test_constructor_and_statics();
    test_access_and_search();
    test_replace_and_match();
    test_regexp();
    return sashfold::test::report("js_string");
}
