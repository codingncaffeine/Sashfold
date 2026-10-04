#include "JsTest.h"

#include "bindings/Realm.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "js/Heap.h"
#include "js/Object.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// What a document's realm makes when it is made, and what it makes only
// when a page asks. A realm is born with every interface's members as
// descriptions: a few hundred function objects (the interfaces' own
// constructors) where it had four and a half thousand, and the Streams
// script not run at all. A member's function is made at its first use, in
// the realm of the object it is defined on whichever realm asked, behind
// the same checks as before — its receiver, its argument count, the
// window's own — said by the description's flags. The Streams interfaces
// are data properties of the window from the start, and the script behind
// them runs at the first look at one, with the built-ins as the realm was
// born with them however a page has replaced them since. And nothing a page
// can list differs from a realm that makes everything at once, which each
// part below checks against one (js::set_lazy_natives(false)).
//
// The ceilings here are the gate on what a realm costs: they are counts of
// cells and functions, the same on every machine, and a change that makes a
// realm build something up front again turns them red.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<bindings::Realm> realm;
    double clock = 1000;

    explicit Page(std::string_view html)
    {
        bindings::HostHooks hooks;
        hooks.now = [this] { return clock; };
        realm = std::make_unique<bindings::Realm>(*document, *net::parse_url("https://example.test/page.html"), std::move(hooks));
        html::parse_document_bytes_into(*document, std::string(html), realm.get());
        realm->document_parsed();
        while (realm->run_pending()) {
        }
    }

    js::Heap::LazyCensus const& census() { return realm->interpreter().heap().lazy_census(); }

    test::JsRun eval(std::string_view source)
    {
        js::Outcome const outcome = realm->run(source, "<test>");
        test::JsRun run;
        run.ok = outcome.ok;
        run.value = outcome.value;
        if (!outcome.ok)
            run.thrown = realm->interpreter().describe(outcome.value);
        return run;
    }
    std::string string(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_string()) {
            test::fail((run.ok ? "not a string: " : "threw " + run.thrown + " evaluating: ") + std::string(source), __FILE__, __LINE__);
            return "";
        }
        return run.value.as_string()->to_utf8();
    }
    bool boolean(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_boolean()) {
            test::fail((run.ok ? "not a boolean: " : "threw " + run.thrown + " evaluating: ") + std::string(source), __FILE__, __LINE__);
            return false;
        }
        return run.value.as_boolean();
    }
    double number(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_number()) {
            test::fail((run.ok ? "not a number: " : "threw " + run.thrown + " evaluating: ") + std::string(source), __FILE__, __LINE__);
            return 0;
        }
        return run.value.as_number();
    }
    // The message of what a script throws, without the page's error report.
    std::string throws(std::string_view source) { return test::eval_throws(realm->interpreter(), source); }
};

constexpr std::string_view blank = "<!DOCTYPE html><body><p id=p>text</p></body>";

// Everything a page can list of the realm's own objects.
constexpr std::string_view shape_script = R"JS(
(function () {
    function names(o) { return Object.getOwnPropertyNames(o).join(); }
    var objects = [window, Window.prototype, EventTarget.prototype, Node.prototype, Element.prototype, HTMLElement.prototype,
        HTMLInputElement.prototype, HTMLAnchorElement.prototype, HTMLBodyElement.prototype, Document.prototype, document,
        Event.prototype, navigator, Navigator.prototype, location, history, CSSStyleDeclaration.prototype, Range.prototype,
        CanvasRenderingContext2D.prototype, DOMMatrix, DOMMatrix.prototype, URL.prototype, Headers.prototype, Response.prototype,
        ReadableStream.prototype, AbortSignal, Node, document.body, setTimeout, Node.prototype.appendChild,
        Object.getOwnPropertyDescriptor(Node.prototype, 'nodeType').get, Object.getOwnPropertyDescriptor(window, 'onload').set];
    var out = [];
    for (var i = 0; i < objects.length; i++)
        out.push(names(objects[i]));
    var forIn = [];
    for (var k in document.body)
        forIn.push(k);
    out.push(forIn.join());
    return out.join('|');
})()
)JS";

// What a script sees of a member's property and function, as text.
constexpr std::string_view describe_script = R"JS(
function describe(holder, key) {
    var d = Object.getOwnPropertyDescriptor(holder, key);
    if (!d)
        return 'none';
    function fn(f) { return typeof f === 'function' ? f.name + '/' + f.length : String(f); }
    return ('value' in d ? 'value ' + fn(d.value) + ' w' + d.writable : 'get ' + fn(d.get) + ' set ' + fn(d.set)) + ' e' + d.enumerable + ' c' + d.configurable;
}
)JS";

// A page's scripts replace the built-ins the Streams script works with, and
// only then touch a stream.
constexpr std::string_view tamper_script = R"JS(
var thenCalls = 0, realThen = Promise.prototype.then, log = [];
Promise.prototype.then = function () { thenCalls++; return realThen.apply(this, arguments); };
Object.defineProperty = function () { throw new Error('a page defineProperty'); };
Object.getOwnPropertyNames = function () { throw new Error('a page getOwnPropertyNames'); };
Reflect.apply = function () { throw new Error('a page Reflect.apply'); };
var realIterator = Array.prototype[Symbol.iterator];
Array.prototype[Symbol.iterator] = function () { throw new Error('a page array iterator'); };
Function.prototype.bind = function () { throw new Error('a page bind'); };
var loadedAs = typeof ReadableStream;
// (The script has loaded under all of that. Reading a stream iterates
// arrays as any script does, so that one is put back before the reads.)
Array.prototype[Symbol.iterator] = realIterator;
var stream = new ReadableStream({ start: function (c) { c.enqueue('a'); c.enqueue('b'); c.close(); } });
var reader = stream.getReader();
var before = thenCalls;
// (Two reads, each waited for with the real `then`: nothing of the page's
// own here goes through the replaced one, so whatever does is the script's.)
realThen.call(reader.read(), function (r) { log.push(r.value + ':' + r.done); });
realThen.call(reader.read(), function (r) { log.push(r.value + ':' + r.done); });
)JS";

std::string shape_of(Page& page)
{
    return page.string(shape_script);
}

}

int main()
{
    js::set_lazy_natives(true);

    // --- What a realm is born with.
    {
        Page page(blank);
        js::Heap& heap = page.realm->interpreter().heap();
        js::Heap::LazyCensus const& census = page.census();
        // The members are descriptions, the path every installer takes.
        CHECK(census.natives_described >= 4000);
        // What is still made at once is the interfaces' own objects (their
        // constructors) and a few natives their definers ask for. Today:
        // 378; four and a half thousand before.
        CHECK(census.natives_made_at_once <= 420);
        // The parse of this page asked for a handful.
        CHECK(census.natives_made_later <= 120);
        // The Streams script has not run: its fifteen interfaces are names.
        CHECK_EQ(census.values_described, std::uint64_t { 15 });
        CHECK_EQ(census.values_made, std::uint64_t { 0 });
        CHECK(census.script_functions <= 2);
        // The whole realm, in cells: 3,300 today; 9,985 before.
        heap.collect();
        CHECK(heap.cell_count() <= 3700);

        // A member is made when it is first used, and is then one function.
        std::uint64_t const made = census.natives_made_later;
        CHECK(page.boolean("document.getElementById('p').hasAttribute('id')"));
        CHECK(census.natives_made_later > made);
        std::uint64_t const after_use = census.natives_made_later;
        CHECK(page.boolean("document.getElementById('p').hasAttribute('id') && Element.prototype.hasAttribute === Element.prototype.hasAttribute"));
        CHECK_EQ(census.natives_made_later, after_use);
        // An attribute read makes its accessor's pair and nothing else.
        std::uint64_t const before_read = census.natives_made_later;
        CHECK(page.number("document.getElementById('p').childElementCount") == 0);
        CHECK(census.natives_made_later - before_read <= 2);
    }

    // --- The members behave as they did: names, lengths, attributes, checks.
    {
        Page page(blank);
        page.eval(describe_script);
        CHECK_EQ(page.string("describe(Node.prototype, 'appendChild')"), "value appendChild/1 wtrue etrue ctrue");
        CHECK_EQ(page.string("describe(Node.prototype, 'nodeType')"), "get get nodeType/0 set undefined etrue ctrue");
        CHECK_EQ(page.string("describe(Node.prototype, 'textContent')"), "get get textContent/0 set set textContent/1 etrue ctrue");
        CHECK_EQ(page.string("describe(HTMLElement.prototype, 'onclick')"), "get get onclick/0 set set onclick/1 etrue ctrue");
        CHECK_EQ(page.string("describe(HTMLAnchorElement.prototype, 'download')"), "get get download/0 set set download/1 etrue ctrue");
        CHECK_EQ(page.string("describe(window, 'setTimeout')"), "value setTimeout/1 wtrue etrue ctrue");
        CHECK_EQ(page.string("describe(window, 'document')"), "get get document/0 set undefined etrue cfalse");
        CHECK_EQ(page.string("describe(window, 'onload')"), "get get onload/0 set set onload/1 etrue ctrue");
        CHECK_EQ(page.string("describe(window, 'Node')"), "value Node/0 wtrue efalse ctrue");
        CHECK(page.string("Object.getOwnPropertyNames(Node).join()").starts_with("length,name,prototype,ELEMENT_NODE,"));
        // The receiver, then the argument count, each said as the browsers say it.
        CHECK(page.throws("Node.prototype.appendChild.call({}, document.body)").starts_with("TypeError: Failed to execute 'appendChild' on 'Node': Illegal invocation"));
        CHECK(page.throws("Node.prototype.appendChild.call({})").starts_with("TypeError: Failed to execute 'appendChild' on 'Node': Illegal invocation"));
        CHECK(page.throws("document.body.appendChild()").starts_with("TypeError: Failed to execute 'appendChild' on 'Node': 1 argument required, but only 0 present."));
        CHECK(page.throws("Object.getOwnPropertyDescriptor(Node.prototype, 'nodeType').get.call({})").starts_with("TypeError: Illegal invocation"));
        CHECK(page.throws("Object.getOwnPropertyDescriptor(Node.prototype, 'textContent').set.call({}, 'x')").starts_with("TypeError: Illegal invocation"));
        CHECK(page.throws("EventTarget.prototype.addEventListener.call({}, 'x', null)").starts_with("TypeError: Illegal invocation"));
        CHECK(page.throws("document.createElement()").starts_with("TypeError: Failed to execute 'createElement' on 'Document': 1 argument required, but only 0 present."));
        CHECK(page.throws("Node()").starts_with("TypeError: Failed to construct 'Node': Please use the 'new' operator"));
        CHECK(page.throws("new Node()").starts_with("TypeError: Illegal constructor"));
        // A [LegacyLenientThis] handler answers undefined for the wrong
        // receiver; any other handler refuses it.
        CHECK(page.boolean("Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'onmouseenter').get.call({}) === undefined"));
        CHECK(page.throws("Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'onclick').get.call({})").starts_with("TypeError: Illegal invocation"));
        // One accessor pair serves every handler: each property keeps its own.
        CHECK(page.boolean("var a = function () {}, b = function () {}; document.body.onclick = a; document.body.onkeydown = b;"
                           " document.body.onclick === a && document.body.onkeydown === b && document.body.onkeyup === null"));
        CHECK(page.boolean("window.onresize = a; window.onresize === a && window.onscroll === null && document.body.onresize === a"));
        // And the handler a property holds is the one its event finds.
        CHECK(page.boolean("var fired = 0; document.body.onclick = function () { fired++; }; document.body.dispatchEvent(new Event('click'));"
                           " document.body.dispatchEvent(new Event('keydown')); fired === 1"));
        // One accessor pair serves every reflected attribute of a kind.
        CHECK(page.boolean("var link = document.createElement('a'); link.download = 'd'; link.hreflang = 'en'; link.rel = 'r';"
                           " link.getAttribute('download') === 'd' && link.hreflang === 'en' && link.getAttribute('rel') === 'r' && link.target === ''"));
        CHECK(page.boolean("var input = document.createElement('input'); input.disabled = true; input.maxLength = 7; input.type = 'RANGE';"
                           " input.hasAttribute('disabled') && input.required === false && input.maxLength === 7 && input.type === 'range' && input.getAttribute('type') === 'RANGE'"));
        CHECK(page.throws("document.createElement('input').maxLength = -1").starts_with("IndexSizeError"));
        // A window's own members: replaceable ones give way to an assignment.
        CHECK(page.boolean("var kept = window.innerWidth; window.innerWidth = 'mine'; window.innerWidth === 'mine' && typeof kept === 'number'"));
        CHECK(page.boolean("window.navigator === window.navigator && window.self === window && typeof window.setTimeout(function () {}, 0) === 'number'"));
        CHECK(page.throws("window.setTimeout.call({}, function () {}, 0)").starts_with("TypeError: Illegal invocation"));
    }

    // --- A function made at first touch is its object's realm's.
    {
        Page page("<!DOCTYPE html><body><iframe id=f></iframe></body>");
        CHECK(page.boolean("var frame = document.getElementById('f').contentWindow; frame !== window && frame.Node !== Node"));
        // Asked for by the page, never by the frame.
        CHECK(page.boolean("var append = frame.Node.prototype.appendChild; append instanceof frame.Function && !(append instanceof Function)"));
        CHECK(page.boolean("Object.getPrototypeOf(append) === frame.Function.prototype && append !== Node.prototype.appendChild"));
        CHECK(page.boolean("var getter = Object.getOwnPropertyDescriptor(frame.Node.prototype, 'nodeType').get;"
                           " getter instanceof frame.Function && !(getter instanceof Function) && getter.call(document) === 9"));
        // What it throws is its own realm's error.
        CHECK(page.boolean("(function () { try { append.call({}, document.body); } catch (e) { return e instanceof frame.TypeError && !(e instanceof TypeError); } return false; })()"));
        // The frame's document is the frame's, made of the frame's interfaces.
        CHECK(page.boolean("frame.document instanceof frame.Document && !(frame.document instanceof Document)"
                           " && frame.document.createElement('p') instanceof frame.HTMLParagraphElement"));
    }

    // --- The Streams script at first look, with the built-ins as the realm was born with them.
    {
        Page page(blank);
        js::Heap::LazyCensus const& census = page.census();
        page.eval(describe_script);
        CHECK_EQ(census.values_made, std::uint64_t { 0 });
        // Listed without being looked at.
        std::uint64_t const functions_before = census.script_functions;
        CHECK(page.boolean("Object.getOwnPropertyNames(window).indexOf('WritableStream') > 0 && Object.keys(window).indexOf('TransformStream') < 0"));
        CHECK_EQ(census.values_made, std::uint64_t { 0 });
        // A data property like any interface object: the first look runs the script.
        CHECK_EQ(page.string("describe(window, 'WritableStream')"), "value WritableStream/0 wtrue efalse ctrue");
        // (Each of the fifteen names is given its interface as the script
        // defines it, which counts as its making.)
        CHECK_EQ(census.values_made, std::uint64_t { 15 });
        CHECK(census.script_functions - functions_before > 200);
        // One run defines them all: no other name runs it again.
        std::uint64_t const functions_loaded = census.script_functions;
        CHECK(page.boolean("typeof ReadableStream === 'function' && typeof TextDecoderStream === 'function' && new ByteLengthQueuingStrategy({ highWaterMark: 4 }).highWaterMark === 4"));
        CHECK(census.script_functions - functions_loaded < 20);
        CHECK(page.boolean("ReadableStream === window.ReadableStream && Object.getPrototypeOf(new ReadableStream()) === ReadableStream.prototype"));
    }
    {
        // The engine's own first need of a stream loads it too.
        Page page(blank);
        js::Heap::LazyCensus const& census = page.census();
        CHECK_EQ(census.values_made, std::uint64_t { 0 });
        std::uint64_t const functions_before = census.script_functions;
        CHECK(page.boolean("new Blob(['abc']).stream() instanceof ReadableStream"));
        CHECK(census.script_functions - functions_before > 200);
    }
    std::string tampered_lazy;
    {
        Page page(blank);
        CHECK_EQ(page.census().values_made, std::uint64_t { 0 });
        test::JsRun const ran = page.eval(tamper_script);
        CHECK(ran.ok);
        while (page.realm->run_pending()) {
        }
        tampered_lazy = page.string("loadedAs + ' ' + log.join() + ' ' + (thenCalls - before)");
        // The script loaded, and worked, under a page that had replaced
        // what it is built on; none of its own promise steps went through
        // the page's `then`.
        CHECK_EQ(tampered_lazy, std::string("function a:false,b:false 0"));
    }

    // --- Nothing a page can list differs from a realm that makes everything at once.
    std::string lazy_shape;
    {
        Page page(blank);
        // Asked for out of order first, so that an order that followed the
        // asking would show.
        CHECK(page.boolean("typeof document.body.remove === 'function' && typeof Node.prototype.contains === 'function'"
                           " && typeof window.onpopstate === 'object' && typeof document.title === 'string' && typeof TransformStream === 'function'"));
        lazy_shape = shape_of(page);
    }
    js::set_lazy_natives(false);
    {
        Page page(blank);
        js::Heap::LazyCensus const& census = page.census();
        // The eager mode: everything made as it is defined, the script run.
        CHECK_EQ(census.natives_described, std::uint64_t { 0 });
        CHECK(census.natives_made_at_once >= 4000);
        CHECK(census.script_functions > 200);
        std::string const eager_shape = shape_of(page);
        CHECK(!lazy_shape.empty());
        CHECK_EQ(lazy_shape, eager_shape);
        page.eval(describe_script);
        CHECK_EQ(page.string("describe(Node.prototype, 'textContent')"), "get get textContent/0 set set textContent/1 etrue ctrue");
        CHECK_EQ(page.string("describe(window, 'WritableStream')"), "value WritableStream/0 wtrue efalse ctrue");
    }
    {
        // The same page that replaced the built-ins, in the eager mode.
        Page page(blank);
        test::JsRun const ran = page.eval(tamper_script);
        CHECK(ran.ok);
        while (page.realm->run_pending()) {
        }
        CHECK_EQ(page.string("loadedAs + ' ' + log.join() + ' ' + (thenCalls - before)"), tampered_lazy);
    }
    js::set_lazy_natives(true);

    return test::report("realm lazy");
}
