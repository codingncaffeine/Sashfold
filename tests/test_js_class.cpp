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

void test_definitions()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { class A { constructor(x) { this.x = x; } get() { return this.x; } } return typeof A + new A(3).get(); })()", "function3");
    // The constructor, its prototype and the instance's chain.
    CHECK_JS_TRUE(in, "(function () { class A {} var a = new A(); return Object.getPrototypeOf(a) === A.prototype && A.prototype.constructor === A && Object.getPrototypeOf(A) === Function.prototype && Object.getPrototypeOf(A.prototype) === Object.prototype; })()");
    CHECK_JS_TRUE(in, "(function () { class A {} var d = Object.getOwnPropertyDescriptor(A, 'prototype'); return !d.writable && !d.enumerable && !d.configurable; })()");
    CHECK_JS_TRUE(in, "(function () { class A { m() {} static s() {} get g() { return 1; } } var m = Object.getOwnPropertyDescriptor(A.prototype, 'm'); var s = Object.getOwnPropertyDescriptor(A, 's'); var g = Object.getOwnPropertyDescriptor(A.prototype, 'g'); return m.writable && !m.enumerable && m.configurable && s.writable && !s.enumerable && s.configurable && !g.enumerable && g.configurable && typeof g.get === 'function'; })()");
    CHECK_JS_STRING(in, "(function () { class A { m() { return 'm'; } } return Object.keys(A.prototype).length + A.prototype.m.name + A.name + (new A()).m(); })()", "0mAm");
    // Methods are not constructors and have no prototype.
    CHECK_JS_THROWS(in, "(function () { class A { m() {} } new (new A().m)(); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { class A { m() {} } return !('prototype' in A.prototype.m); })()");
    // A class constructor is only for new.
    CHECK_JS_THROWS(in, "(function () { class A {} A(); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { class A {} A.call({}); })()", "TypeError");
    // Class code is strict, and the class name is bound inside, immutably.
    CHECK_JS_THROWS(in, "(function () { class A { m() { undeclared = 1; } } new A().m(); })()", "ReferenceError");
    CHECK_JS_THROWS(in, "(function () { class A { m() { A = 1; } } new A().m(); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { class A { m() { return A; } } var B = A; A = null; return new B().m() === B; })()");
    // Expressions, named and anonymous, and NamedEvaluation.
    CHECK_JS_STRING(in, "(function () { var C = class {}; var D = class Named {}; var o = { E: class {} }; return C.name + ',' + D.name + ',' + o.E.name; })()", "C,Named,E");
    CHECK_JS_STRING(in, "(class {}).name", "");
    CHECK_JS_TRUE(in, "(function () { var C = class Inner { m() { return Inner; } }; return typeof Inner === 'undefined' && new C().m() === C; })()");
    // The declaration is let-like: hoisted into a dead zone, block scoped.
    CHECK_JS_THROWS(in, "(function () { new A(); class A {} })()", "ReferenceError");
    CHECK_JS_TRUE(in, "(function () { { class A {} } return typeof A === 'undefined'; })()");
    CHECK_JS_THROWS(in, "class A {} class A {}", "SyntaxError");
    // Accessors, computed and symbol keys, static members, `static` as a name.
    CHECK_JS_STRING(in, "(function () { var k = 'dyn'; var s = Symbol('s'); class A { get [k]() { return 'D'; } set [k](v) { this.v = v; } [s]() { return 'S'; } static [k + '2']() { return 'T'; } static static() { return 'st'; } } var a = new A(); a.dyn = 5; return a.dyn + a.v + a[s]() + A.dyn2() + A.static(); })()", "D5STst");
    CHECK_JS_STRING(in, "(function () { class A { static count = 0; static inc() { return ++A.count; } } A.inc(); return A.inc() + ':' + A.count; })()", "2:2");
    CHECK_JS_TRUE(in, "(function () { var s = Symbol('tag'); class A { [s]() {} } return A.prototype[s].name === '[tag]'; })()");
    CHECK_JS_STRING(in, "(function () { class A { get x() { return 1; } } class B extends A { set x(v) {} } var d = Object.getOwnPropertyDescriptor(B.prototype, 'x'); return typeof d.get + typeof d.set; })()", "undefinedfunction");
    // Semicolons between elements, a keyword as a method name, and
    // Function.prototype.toString gives the class's own text.
    CHECK_JS_NUMBER(in, "(function () { class A { ; m() { return 1; } ; if() { return 2; } } return new A().m() + new A().if(); })()", 3);
    CHECK_JS_STRING(in, "(function () { class A { m() {} } return A.toString(); })()", "class A { m() {} }");
    CHECK_JS_NUMBER(in, "(function () { class A { constructor(a, b = 1, ...r) {} } return A.length; })()", 1);
    CHECK_JS_NUMBER(in, "(function () { class A {} class B extends A {} return A.length + B.length; })()", 0);
    CHECK_JS_STRING(in, "(function () { class A {} return Object.prototype.toString.call(new A()) + typeof A.prototype; })()", "[object Object]object");
}

void test_inheritance()
{
    js::Interpreter& in = fresh();
    // Two classes, declared inside a function so each script has its own.
    auto shapes = [](std::string const& body) {
        return "(function () { class Shape { constructor(n) { this.n = n; } area() { return 0; } describe() { return this.n + ':' + this.area(); } static create(n) { return new this(n); } } class Square extends Shape { constructor(s) { super('square'); this.s = s; } area() { return this.s * this.s; } describe() { return super.describe() + '!'; } } " + body + " })()";
    };
    CHECK_JS_STRING(in, shapes("return new Square(3).describe();"), "square:9!");
    CHECK_JS_TRUE(in, shapes("var s = new Square(2); return s instanceof Square && s instanceof Shape && Object.getPrototypeOf(Square) === Shape && Object.getPrototypeOf(Square.prototype) === Shape.prototype && s.constructor === Square;"));
    // `this` in a static method is the class new was applied to.
    CHECK_JS_TRUE(in, shapes("var s = Square.create(4); return s instanceof Square && s.s === 4 && s.n === 'square' && Shape.create(1).constructor === Shape;"));
    // The default derived constructor forwards every argument.
    CHECK_JS_STRING(in, "(function () { class A { constructor(...a) { this.a = a.join(); } } class B extends A {} return new B(1, 2, 3).a; })()", "1,2,3");
    // `this` before super() is a ReferenceError; so is a missing super().
    CHECK_JS_THROWS(in, "(function () { class A {} class B extends A { constructor() { this.x = 1; super(); } } new B(); })()", "ReferenceError");
    CHECK_JS_THROWS(in, "(function () { class A {} class B extends A { constructor() {} } new B(); })()", "ReferenceError");
    CHECK_JS_THROWS(in, "(function () { class A {} class B extends A { constructor() { super(); super(); } } new B(); })()", "ReferenceError");
    // A derived constructor may return an object of its own, and nothing else.
    CHECK_JS_NUMBER(in, "(function () { class A {} class B extends A { constructor() { return { v: 7 }; } } return new B().v; })()", 7);
    CHECK_JS_THROWS(in, "(function () { class A {} class B extends A { constructor() { super(); return 1; } } new B(); })()", "TypeError");
    CHECK_JS_TRUE(in, "(function () { class A {} class B extends A { constructor() { super(); return undefined; } } return new B() instanceof B; })()");
    // super() in an arrow inside the constructor; this afterwards.
    CHECK_JS_NUMBER(in, "(function () { class A { constructor() { this.v = 5; } } class B extends A { constructor() { var f = () => super(); f(); this.w = this.v + 1; } } return new B().w; })()", 6);
    // super.x reads through the prototype with this as the receiver, and
    // writes onto this.
    CHECK_JS_STRING(in, "(function () { class A { get who() { return 'A:' + this.tag; } } class B extends A { constructor() { super(); this.tag = 'b'; } get who() { return super.who + '/B'; } } return new B().who; })()", "A:b/B");
    CHECK_JS_TRUE(in, "(function () { class A {} class B extends A { m() { super.x = 1; return this.hasOwnProperty('x') && !('x' in A.prototype); } } return new B().m(); })()");
    CHECK_JS_STRING(in, "(function () { class A { static s() { return 'S'; } } class B extends A { static s() { return super.s() + 'B'; } } return B.s(); })()", "SB");
    CHECK_JS_STRING(in, "(function () { var o = { m() { return super.toString === Object.prototype.toString ? 'proto' : 'no'; } }; return o.m(); })()", "proto");
    CHECK_JS_STRING(in, "(function () { var base = { hi() { return 'base'; } }; var o = { hi() { return super.hi() + '+o'; } }; Object.setPrototypeOf(o, base); return o.hi(); })()", "base+o");
    CHECK_JS_NUMBER(in, "(function () { class A { m() { return 1; } } class B extends A { m() { return super['m']() + 1; } } return new B().m(); })()", 2);
    CHECK_JS_TRUE(in, "(function () { class A { m() { return this; } } class B extends A { m() { return super.m(); } } var b = new B(); return b.m() === b; })()");
    CHECK_JS_THROWS(in, "(function () { class A {} class B extends A { m() { delete super.x; } } new B().m(); })()", "ReferenceError");
    // extends null, non-constructors, and bad prototype properties.
    CHECK_JS_TRUE(in, "(function () { class N extends null {} return Object.getPrototypeOf(N.prototype) === null && Object.getPrototypeOf(N) === Function.prototype; })()");
    CHECK_JS_THROWS(in, "(function () { class N extends null {} new N(); })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { class A extends 1 {} })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { function F() {} F.prototype = 1; class A extends F {} })()", "TypeError");
    CHECK_JS_THROWS(in, "(function () { class A extends (() => 1) {} })()", "TypeError");
    // Extending a plain function and a built-in.
    CHECK_JS_STRING(in, "(function () { function F(a) { this.a = a; } F.prototype.f = function () { return 'f' + this.a; }; class C extends F { constructor() { super('x'); } } return new C().f(); })()", "fx");
    CHECK_JS_TRUE(in, "(function () { class E extends Error { constructor(m) { super(m); this.name = 'E'; } } var e = new E('boom'); return e instanceof E && e instanceof Error && e.message === 'boom' && String(e) === 'E: boom'; })()");
    CHECK_JS_TRUE(in, "(function () { class L extends Array {} var l = new L(); l.push(1, 2); return l.length === 2 && Array.isArray(l) && l instanceof L; })()");
    // new.target: the constructor new was applied to, undefined in a call.
    CHECK_JS_TRUE(in, "(function () { class A { constructor() { this.t = new.target; } } class B extends A {} return new A().t === A && new B().t === B; })()");
    CHECK_JS_TRUE(in, "(function () { function F() { return new.target; } return F() === undefined && new F() === F; })()");
    CHECK_JS_TRUE(in, "(function () { function F() { return (() => new.target)(); } return new F() === F; })()");
    CHECK_JS_THROWS(in, "new.target", "SyntaxError");
    // The heritage is evaluated in the class's scope, once, in strict code.
    CHECK_JS_NUMBER(in, "(function () { var n = 0; function P() { n++; return Object; } class A extends P() {} return n; })()", 1);
    // Inherited static methods and instanceof through Symbol.hasInstance-less chains.
    CHECK_JS_STRING(in, "(function () { class A { static who() { return 'A'; } } class B extends A {} return B.who(); })()", "A");
}

void test_fields()
{
    js::Interpreter& in = fresh();
    CHECK_JS_STRING(in, "(function () { class A { x = 1; y; z = this.x + 1; static s = 'S'; } var a = new A(); return a.x + ',' + a.y + ',' + a.z + ',' + A.s + ',' + ('s' in a) + ',' + Object.keys(a).join(); })()", "1,undefined,2,S,false,x,y,z");
    // Instance fields are defined before the constructor body runs, after
    // super() in a derived class, and per instance.
    CHECK_JS_STRING(in, "(function () { var log = []; class A { constructor() { log.push('A:' + this.f); } f = log.push('field'); } new A(); return log.join(); })()", "field,A:1");
    CHECK_JS_STRING(in, "(function () { var log = []; class A { constructor() { log.push('A'); } } class B extends A { g = log.push('g'); constructor() { log.push('B0'); super(); log.push('B1:' + this.g); } } new B(); return log.join(); })()", "B0,A,g,B1:3");
    CHECK_JS_TRUE(in, "(function () { class A { o = {}; } return new A().o !== new A().o; })()");
    // Fields are own data properties, enumerable, and shadow prototype accessors.
    CHECK_JS_TRUE(in, "(function () { class A { get x() { return 'proto'; } } class B extends A { x = 'own'; } var b = new B(); var d = Object.getOwnPropertyDescriptor(b, 'x'); return d.value === 'own' && d.writable && d.enumerable && d.configurable; })()");
    // Computed keys are evaluated once, at definition; initializers see
    // `this` and the class through super.
    CHECK_JS_STRING(in, "(function () { var n = 0; class A { [`k${n++}`] = n; } return Object.keys(new A()).join() + new A().k0 + n; })()", "k011");
    CHECK_JS_STRING(in, "(function () { class A { hello() { return 'hi'; } } class B extends A { greeting = super.hello() + '!'; arrow = () => this.greeting; } var b = new B(); return b.arrow(); })()", "hi!");
    // An anonymous function initializer is named after the field.
    CHECK_JS_STRING(in, "(function () { class A { f = function () {}; g = () => 1; static h = class {}; } var a = new A(); return a.f.name + ',' + a.g.name + ',' + A.h.name; })()", "f,g,h");
    // Static fields and blocks run in order with the class as this; a
    // static block sees the class name.
    CHECK_JS_STRING(in, "(function () { var log = []; class A { static a = log.push('a'); static { log.push('block:' + this.a + ':' + (this === A)); } static b = log.push('b'); } return log.join(); })()", "a,block:1:true,b");
    CHECK_JS_NUMBER(in, "(function () { class A { static x; static { A.x = 5; } } return A.x; })()", 5);
    // A throwing initializer aborts construction.
    CHECK_JS_THROWS(in, "(function () { class A { f = (function () { throw new TypeError('init'); })(); } new A(); })()", "TypeError");
    // arguments and return are early errors in initializers and static blocks.
    CHECK_JS_THROWS(in, "(function () { class A { f = arguments; } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { static { arguments; } } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { static { return; } } })()", "SyntaxError");
    CHECK_JS_NUMBER(in, "(function () { class A { f = function () { return arguments.length; }; } return new A().f(1, 2); })()", 2);
    // Field names: a keyword, a string, a number, `static` and `constructor`.
    CHECK_JS_NUMBER(in, "(function () { class A { if = 1; 'two' = 2; 3 = 3; static = 4; } var a = new A(); return a.if + a.two + a[3] + a.static; })()", 10);
    CHECK_JS_THROWS(in, "(function () { class A { constructor = 1; } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { static prototype = 1; } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { static prototype() {} } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { get constructor() {} } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { constructor() {} constructor() {} } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { #x = 1; } })()", "SyntaxError");
    // A static method named constructor is just a static method.
    CHECK_JS_STRING(in, "(function () { class A { static constructor() { return 'sc'; } } return A.constructor() + (A.prototype.constructor === A); })()", "sctrue");
}

void test_super_and_eval()
{
    js::Interpreter& in = fresh();
    CHECK_JS_THROWS(in, "super.x", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { super.x; })", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { m() { super(); } } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { constructor() { super(); } } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A extends Object { constructor() { function f() { super(); } } } })()", "SyntaxError");
    CHECK_JS_THROWS(in, "(function () { class A { m() { super?.x; } } })()", "SyntaxError");
    // Direct eval inherits what super may do.
    CHECK_JS_STRING(in, "(function () { class A { m() { return 'A'; } } class B extends A { m() { return eval('super.m()') + 'B'; } } return new B().m(); })()", "AB");
    CHECK_JS_TRUE(in, "(function () { class A {} class B extends A { constructor() { eval('super()'); } } return new B() instanceof B; })()");
    CHECK_JS_THROWS(in, "(function () { function f() { eval('super.x'); } f(); })()", "SyntaxError");
    // A method's home object survives the method being moved.
    CHECK_JS_STRING(in, "(function () { class A { m() { return 'A'; } } class B extends A { m() { return super.m() + 'B'; } } var m = B.prototype.m; return m.call({}); })()", "AB");
    // typeof and update through super, and super in an object literal getter.
    CHECK_JS_STRING(in, "(function () { class A { get x() { return 'ax'; } } class B extends A { m() { return typeof super.x; } } return new B().m(); })()", "string");
    CHECK_JS_NUMBER(in, "(function () { var base = { n: 1 }; var o = { m() { super.n += 1; return this.n; } }; Object.setPrototypeOf(o, base); return o.m() + base.n; })()", 3);
}

} // namespace

int main()
{
    test_definitions();
    test_inheritance();
    test_fields();
    test_super_and_eval();
    return sashfold::test::report("js_class");
}
