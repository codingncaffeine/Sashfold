#include "JsTest.h"
#include "WebmBuilder.h"

#include "bindings/LayoutOracle.h"
#include "bindings/Realm.h"
#include "core/Base64.h"
#include "core/Bitmap.h"
#include "css/ComputedStyle.h"
#include "dom/Dom.h"
#include "html/Serializer.h"
#include "html/TreeBuilder.h"
#include "media/Vp9Accelerator.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The DOM bindings, the event model and the event loop, driven from the
// outside the way the shell drives them: a page parsed with its scripts
// running, a virtual clock for the timers, hooks the test answers. Every
// realm here runs under heap stress — a collection at every allocation —
// so a wrapper or callback that is not rooted fails the first time.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<bindings::Realm> realm;
    std::string console; // "level:message|"
    double clock = 1000;
    std::map<std::string, std::string> scripts; // URL → source, for <script src>
    std::vector<std::string> fetched;
    std::vector<net::Url> navigations;
    // Canned responses for fetch() and XMLHttpRequest by URL, and the
    // requests made: "METHOD url [body]" and every header sent.
    std::map<std::string, net::FetchResponse> responses;
    std::vector<std::string> requests;
    std::vector<std::string> request_headers;

    explicit Page(std::string_view html, std::string const& url = "https://example.test/dir/page.html",
        bindings::HostHooks hooks = {})
    {
        hooks.console = [this](std::string_view level, std::string_view message) {
            console += std::string(level) + ":" + std::string(message) + "|";
        };
        hooks.now = [this] { return clock; };
        // The page's guard is honoured the way the shell's loader honours
        // it: a refused URL is not served.
        hooks.fetch_script = [this](net::Url const& target, net::RequestGuard const& guard) -> std::optional<std::string> {
            if (guard.refusal && guard.refusal(target, false))
                return std::nullopt;
            fetched.push_back(target.serialize());
            auto const it = scripts.find(target.serialize());
            if (it == scripts.end())
                return std::nullopt;
            return it->second;
        };
        hooks.fetch_resource = [this](net::Url const& target, net::ResourceRequest const& request,
                                   net::RequestGuard const& guard) -> net::FetchResult {
            if (guard.refusal) {
                if (std::optional<std::string> refused = guard.refusal(target, false))
                    return { std::nullopt, std::move(*refused) };
            }
            std::string line = request.method + " " + target.serialize();
            if (!request.body.empty())
                line += " " + std::string(request.body.begin(), request.body.end());
            if (!request.credentials)
                line += " (no credentials)";
            requests.push_back(line);
            for (net::Header const& header : request.headers)
                request_headers.push_back(header.name + ": " + header.value);
            auto const it = responses.find(target.serialize());
            if (it == responses.end())
                return { std::nullopt, "no such resource" };
            net::FetchResponse response = it->second;
            if (response.final_url.scheme.empty())
                response.final_url = target;
            return { std::move(response), "" };
        };
        hooks.navigate = [this](net::Url const& target) { navigations.push_back(target); };
        hooks.user_agent = "Mozilla/5.0 TestAgent Sashfold/0.0";
        realm = std::make_unique<bindings::Realm>(*document, *net::parse_url(url), std::move(hooks));
        realm->interpreter().heap().set_stress(true);
        m_html = std::string(html);
    }

    void load()
    {
        html::parse_document_bytes_into(*document, m_html, realm.get());
        realm->document_parsed();
    }

    // A module script the loader can serve, with the type it is served as
    // and, when asked, the CORS header that lets another origin read it.
    void module(std::string const& url, std::string const& source, std::string const& type = "text/javascript", bool cors_open = false)
    {
        net::FetchResponse response;
        response.status = 200;
        response.status_text = "OK";
        response.headers.push_back(net::Header { "Content-Type", type });
        if (cors_open)
            response.headers.push_back(net::Header { "Access-Control-Allow-Origin", "*" });
        response.body.assign(source.begin(), source.end());
        responses[url] = response;
    }

    // Through the realm, as the shell runs script: the microtask checkpoint
    // on the way out, an uncaught error on the console and in the count.
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
    double number(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_number()) {
            test::fail((run.ok ? "not a number: " : "threw " + run.thrown + " evaluating: ") + std::string(source), __FILE__, __LINE__);
            return 0;
        }
        return run.value.as_number();
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
    // An expected throw is evaluated on the engine directly, so it is not
    // reported as a page error.
    std::string throws(std::string_view source) { return test::eval_throws(realm->interpreter(), source); }

private:
    std::string m_html;
};

// A page whose scripts must reach the realm's script hooks: the script
// sources by URL are given before load().
std::unique_ptr<Page> loaded(std::string_view html)
{
    auto page = std::make_unique<Page>(html);
    page->load();
    return page;
}

void test_inline_scripts_run_in_document_order()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><p id=a>x</p>
<script>document.getElementById('a').textContent = 'y'; var later = document.getElementById('b');</script>
<p id=b>z</p>)HTML");
    CHECK_EQ(page->string("document.getElementById('a').textContent"), "y");
    CHECK(page->boolean("later === null"));
    CHECK(page->boolean("document.getElementById('b') !== null"));
    CHECK_EQ(page->realm->stats().scripts_run, 1);
    CHECK_EQ(page->realm->stats().scripts_failed, 0);
    CHECK_EQ(page->console, "");
}

void test_wrapper_identity_and_expandos_survive_collection()
{
    auto page = loaded("<!DOCTYPE html><div id=d><span></span></div>");
    CHECK(page->boolean("document.body === document.body"));
    CHECK(page->boolean("document.getElementById('d') === document.body.firstChild"));
    CHECK(page->boolean("document.documentElement === document.body.parentNode"));
    // An expando on a connected element lives as long as the element is
    // in the tree, whatever the script still references (ADR 0001 §2).
    page->eval("document.body.firstChild.mark = 42; var junk = []; for (var i = 0; i < 40; i++) junk.push({ i: i, s: 'x' + i });");
    CHECK_EQ(page->number("document.getElementById('d').mark"), 42);
    // A detached subtree lives while a wrapper into it is reachable.
    page->eval("var t = document.createElement('div'); var s = document.createElement('span'); t.appendChild(s); s.tag = 7; for (var j = 0; j < 40; j++) junk.push([j]);");
    CHECK_EQ(page->number("t.firstChild.tag"), 7);
    CHECK(page->boolean("t.firstChild === s"));
    CHECK(page->boolean("!t.isConnected && document.body.isConnected"));
    CHECK(page->boolean("document.body.firstChild instanceof HTMLDivElement"));
    CHECK(page->boolean("document.body.firstChild instanceof HTMLElement && document.body.firstChild instanceof Element && document.body.firstChild instanceof Node && document.body.firstChild instanceof EventTarget"));
    CHECK(page->boolean("document instanceof Document && document instanceof HTMLDocument"));
    CHECK_EQ(page->string("Object.prototype.toString.call(document.body)"), "[object HTMLBodyElement]");
}

void test_tree_mutation_and_serialization()
{
    auto page = loaded("<!DOCTYPE html><body><ul id=list><li>one</li></ul></body>");
    std::uint64_t const before = page->realm->mutation_count();
    page->eval(R"JS(
        var list = document.getElementById('list');
        var li = document.createElement('li');
        li.textContent = 'two';
        list.appendChild(li);
        var first = document.createElement('li');
        first.innerHTML = '<b>zero</b> &amp; <i>more</i>';
        list.insertBefore(first, list.firstChild);
    )JS");
    CHECK(page->realm->mutation_count() > before);
    CHECK_EQ(page->string("list.innerHTML"), "<li><b>zero</b> &amp; <i>more</i></li><li>one</li><li>two</li>");
    CHECK_EQ(page->string("list.outerHTML"), "<ul id=\"list\"><li><b>zero</b> &amp; <i>more</i></li><li>one</li><li>two</li></ul>");
    CHECK_EQ(page->number("list.children.length"), 3);
    CHECK_EQ(page->number("list.childNodes.length"), 3);
    CHECK_EQ(page->string("list.firstElementChild.textContent"), "zero & more");
    CHECK_EQ(page->string("list.lastChild.previousSibling.nodeName"), "LI");
    CHECK_EQ(page->string("list.tagName + '/' + list.localName + '/' + list.nodeType"), "UL/ul/1");
    CHECK_EQ(page->string("list.firstChild.firstChild.nodeName + list.firstChild.childNodes[1].nodeType"), "B3");
    page->eval("var removed = list.removeChild(list.lastChild);");
    CHECK_EQ(page->number("list.children.length"), 2);
    CHECK(page->boolean("removed.parentNode === null && removed === li"));
    page->eval("list.replaceChild(removed, list.firstChild);");
    CHECK_EQ(page->string("list.textContent"), "twoone");
    page->eval("var clone = list.cloneNode(true); clone.firstChild.textContent = 'changed';");
    CHECK_EQ(page->string("list.firstChild.textContent"), "two");
    CHECK_EQ(page->string("clone.firstChild.textContent"), "changed");
    CHECK(page->boolean("list.cloneNode(false).childNodes.length === 0"));
    page->eval("list.textContent = 'flat';");
    CHECK_EQ(page->string("list.innerHTML"), "flat");
    CHECK_EQ(page->number("list.childNodes.length"), 1);
    page->eval("list.innerHTML = '';");
    CHECK_EQ(page->number("list.childNodes.length"), 0);
    page->eval("list.insertAdjacentHTML('beforeend', '<li>a</li>'); list.insertAdjacentHTML('afterbegin', '<li>b</li>'); list.insertAdjacentHTML('beforebegin', '<p>p</p>');");
    CHECK_EQ(page->string("list.textContent"), "ba");
    CHECK_EQ(page->string("list.previousSibling.textContent"), "p");
    page->eval("list.append('text', document.createElement('li')); list.prepend(document.createElement('li'));");
    CHECK_EQ(page->number("list.childNodes.length"), 5);
    CHECK_EQ(page->string("list.childNodes[3].nodeName"), "#text");
    page->eval("var frag = document.createDocumentFragment(); frag.appendChild(document.createElement('em')); frag.appendChild(document.createElement('strong')); list.appendChild(frag);");
    CHECK_EQ(page->number("frag.childNodes.length"), 0);
    CHECK_EQ(page->string("list.lastChild.nodeName"), "STRONG");
    CHECK_EQ(page->string("list.lastChild.previousSibling.nodeName"), "EM");
    // Hierarchy checks.
    CHECK(page->throws("list.appendChild(document.body)").starts_with("HierarchyRequestError"));
    CHECK(page->throws("list.removeChild(document.body)").starts_with("NotFoundError"));
    CHECK(page->throws("list.appendChild(document)").starts_with("HierarchyRequestError"));
    CHECK(page->boolean("document.body.contains(list) && !list.contains(document.body) && list.contains(list)"));
    CHECK_EQ(page->number("document.body.compareDocumentPosition(list)"), 20);
    CHECK_EQ(page->number("list.compareDocumentPosition(document.body)"), 10);
    CHECK_EQ(page->number("list.previousSibling.compareDocumentPosition(list)"), 4);
    // Nodes in two trees are disconnected, one preceding the other and that
    // one following it.
    CHECK(page->boolean("(() => { const a = document.createElement('a'), b = document.createElement('b');"
                        " const there = a.compareDocumentPosition(b), back = b.compareDocumentPosition(a);"
                        " return (there & 33) === 33 && (back & 33) === 33 && ((there & 6) === 2 ? (back & 6) === 4 : (there & 6) === 4 && (back & 6) === 2); })()"));
    // Text nodes.
    page->eval("var text = document.createTextNode('hello world'); document.body.appendChild(text);");
    CHECK_EQ(page->number("text.length"), 11);
    CHECK_EQ(page->string("text.substringData(6, 5)"), "world");
    page->eval("text.data = 'ab'; text.appendData('cd'); var rest = text.splitText(2);");
    CHECK_EQ(page->string("text.data + '|' + rest.data + '|' + text.wholeText"), "ab|cd|abcd");
    CHECK_EQ(page->string("document.body.lastChild.nodeValue"), "cd");
    CHECK(page->boolean("text instanceof Text && text instanceof CharacterData && text.nodeType === Node.TEXT_NODE"));
    page->eval("rest.remove(); text.before(document.createComment('c'));");
    CHECK_EQ(page->string("text.previousSibling.nodeName + text.previousSibling.data"), "#commentc");
    CHECK_EQ(page->console, "");
}

void test_cdata_sections_and_processing_instructions()
{
    auto page = loaded("<body><p id=p></p></body>");
    CHECK(page->boolean("document.createComment('x').nodeType === Node.COMMENT_NODE"));
    // A CDATA section is a Text node an XML document makes; an HTML document
    // refuses to.
    CHECK(page->throws("document.createCDATASection('x')").starts_with("NotSupportedError"));
    CHECK(page->throws("document.implementation.createHTMLDocument('').createCDATASection('x')").starts_with("NotSupportedError"));
    CHECK(page->throws("new Document().createCDATASection('a]]>b')").starts_with("InvalidCharacterError"));
    CHECK(page->boolean("(() => { const c = new Document().createCDATASection('12'); return c instanceof CDATASection && c instanceof Text"
                        " && c.nodeType === Node.CDATA_SECTION_NODE && c.nodeName === '#cdata-section' && c.data === '12' && c.length === 2; })()"));
    // The range tests' setup, word for word.
    CHECK(page->boolean("(() => { const p = document.getElementById('p'); const x = new Document();"
                        " p.appendChild(x.createCDATASection('1234')); p.appendChild(x.createCDATASection('5678')); p.append('9012');"
                        " return p.childNodes.length === 3 && p.firstChild.nodeType === 4 && p.firstChild.ownerDocument === document"
                        " && p.textContent === '123456789012'; })()"));
    // normalize() folds only exclusive Text nodes; wholeText takes CDATA too.
    CHECK(page->boolean("(() => { const d = document.createElement('div'); d.append('a'); d.appendChild(new Document().createCDATASection('b'));"
                        " d.append('c', 'd'); d.normalize(); return d.childNodes.length === 3 && d.lastChild.data === 'cd'"
                        " && d.firstChild.wholeText === 'abcd'; })()"));
    CHECK(page->boolean("(() => { const c = new Document().createCDATASection('z'); const k = c.cloneNode();"
                        " const d = document.createElement('div'); d.appendChild(c);"
                        " return k instanceof CDATASection && k.data === 'z' && d.cloneNode(true).firstChild.nodeType === 4; })()"));
    CHECK(page->boolean("(() => { const d = document.createElement('div'); const c = new Document().createCDATASection('abcd'); d.appendChild(c);"
                        " const r = c.splitText(2); return r instanceof CDATASection && d.childNodes.length === 2 && r.data === 'cd'; })()"));
    CHECK(page->boolean("!document.createTextNode('a').isEqualNode(new Document().createCDATASection('a'))"));
    // Documents made by script: new Document() and createDocument() are XML
    // documents, and createDocument() inserts its doctype and element.
    CHECK(page->boolean("(() => { const dt = document.implementation.createDocumentType('qorflesnorf', 'abcde', 'x');"
                        " const x = document.implementation.createDocument(null, null, dt);"
                        " return x.doctype === dt && dt.ownerDocument === x && x.documentElement === null && x.childNodes.length === 1"
                        " && x.contentType === 'application/xml' && new Document().contentType === 'application/xml'; })()"));
    CHECK(page->boolean("(() => { const y = document.implementation.createDocument('http://www.w3.org/2000/svg', 'svg', null);"
                        " return y.documentElement.localName === 'svg' && y.documentElement.namespaceURI === 'http://www.w3.org/2000/svg'"
                        " && y.contentType === 'image/svg+xml' && y.createCDATASection('a').nodeType === 4; })()"));
    CHECK(page->boolean("new DOMParser().parseFromString('<root></root>', 'application/xml').createCDATASection('a').nodeType === 4"));
    // Processing instructions.
    CHECK(page->boolean("(() => { const pi = document.createProcessingInstruction('xml-stylesheet', 'href=\"a.css\"');"
                        " return pi instanceof ProcessingInstruction && pi instanceof CharacterData && pi.nodeType === Node.PROCESSING_INSTRUCTION_NODE"
                        " && pi.nodeName === 'xml-stylesheet' && pi.target === 'xml-stylesheet' && pi.data === 'href=\"a.css\"'"
                        " && pi.nodeValue === pi.data && pi.textContent === pi.data && pi.ownerDocument === document; })()"));
    CHECK(page->throws("document.createProcessingInstruction('A', '?>')").starts_with("InvalidCharacterError"));
    CHECK(page->boolean("['\\u00B7A', '\\u00D7A', 'A\\u00D7', '\\\\A', '\\f', 0, '0'].every(t => {"
                        " try { document.createProcessingInstruction(t, 'x'); return false; } catch (e) { return e.name === 'InvalidCharacterError'; } })"));
    CHECK(page->boolean("['xml:fail', 'A\\u00B7A', 'a0'].every(t => document.createProcessingInstruction(t, 'x').target === t)"));
    CHECK(page->boolean("(() => { const pi = document.createProcessingInstruction('t', 'abc'); pi.appendData('d'); pi.deleteData(0, 1);"
                        " const d = document.createElement('div'); d.appendChild(pi);"
                        " return pi.data === 'bcd' && pi.length === 3 && d.textContent === '' && d.innerHTML === '<?t bcd>'"
                        " && pi.cloneNode().target === 't' && d.cloneNode(true).firstChild.data === 'bcd'"
                        " && pi.isEqualNode(pi.cloneNode()) && !pi.isEqualNode(document.createProcessingInstruction('u', 'bcd')); })()"));
}

void test_ranges()
{
    auto page = loaded("<body><div id=d><p id=a>Hello</p><p id=b>World</p></div></body>");
    CHECK(page->boolean("typeof document.createElement === 'function'"));
    // A new range is collapsed at the start of its document.
    CHECK(page->boolean("(() => { const r = document.createRange(); return r instanceof Range && r instanceof AbstractRange"
                        " && r.startContainer === document && r.startOffset === 0 && r.collapsed && r.commonAncestorContainer === document"
                        " && new Range().endContainer === document; })()"));
    // An end set before the start takes the start with it; a doctype or an
    // offset past the node's length is refused.
    CHECK(page->boolean("(() => { const r = document.createRange(); const a = document.getElementById('a').firstChild; r.setStart(a, 1); r.setEnd(a, 4);"
                        " const set = r.toString() === 'ell' && !r.collapsed; r.setEnd(a, 0); return set && r.startOffset === 0 && r.collapsed; })()"));
    CHECK(page->throws("document.createRange().setStart(document.getElementById('a').firstChild, 6)").starts_with("IndexSizeError"));
    CHECK(page->throws("document.createRange().setStart(document.implementation.createDocumentType('html', '', ''), 0)").starts_with("InvalidNodeTypeError"));
    // Comparing points with the range.
    CHECK(page->boolean("(() => { const d = document.getElementById('d'); const b = document.getElementById('b'); const r = document.createRange();"
                        " r.selectNodeContents(b); return r.comparePoint(d, 0) === -1 && r.comparePoint(b.firstChild, 2) === 0 && r.comparePoint(d, 2) === 1"
                        " && r.isPointInRange(b, 1) && !r.isPointInRange(d, 0) && r.intersectsNode(d) && !r.intersectsNode(document.getElementById('a'))"
                        " && r.compareBoundaryPoints(Range.START_TO_END, r) === 1; })()"));
    // A live range follows the tree: an insertion before its boundary moves
    // it, deleted data pulls its offsets back, a removed container collapses
    // it onto the parent.
    CHECK(page->boolean("(() => { const d = document.getElementById('d'); const r = document.createRange(); r.setStart(d, 1); r.setEnd(d, 2);"
                        " d.insertBefore(document.createElement('hr'), d.firstChild); const moved = r.startOffset === 2 && r.endOffset === 3;"
                        " const b = document.getElementById('b'); const inner = document.createRange(); inner.setStart(b.firstChild, 2); inner.setEnd(b.firstChild, 5);"
                        " b.firstChild.deleteData(0, 3); const pulled = inner.startOffset === 0 && inner.endOffset === 2;"
                        " b.remove(); return moved && pulled && inner.startContainer === d && inner.startOffset === 2 && inner.collapsed && r.endOffset === 2; })()"));
    // splitText carries the range to the new node, normalize() back.
    CHECK(page->boolean("(() => { const p = document.createElement('p'); p.textContent = 'abcdef'; const t = p.firstChild; const r = document.createRange();"
                        " r.setStart(t, 4); r.setEnd(p, 1); const rest = t.splitText(2);"
                        " const split = r.startContainer === rest && r.startOffset === 2 && r.endContainer === p && r.endOffset === 2;"
                        " p.normalize(); return split && r.startContainer === t && r.startOffset === 4 && r.endContainer === p && r.endOffset === 1"
                        " && p.childNodes.length === 1; })()"));
    // Contents cloned, extracted, inserted into and surrounded.
    CHECK(page->boolean("(() => { const p = document.createElement('p'); p.innerHTML = 'one <b>two</b> three'; const r = document.createRange();"
                        " r.setStart(p.firstChild, 2); r.setEnd(p.lastChild, 3); const clone = r.cloneContents();"
                        " const cloned = clone.childNodes.length === 3 && clone.firstChild.data === 'e ' && clone.childNodes[1].outerHTML === '<b>two</b>'"
                        " && clone.lastChild.data === ' th' && p.innerHTML === 'one <b>two</b> three';"
                        " const extracted = r.extractContents(); return cloned && extracted.textContent === 'e two th' && p.innerHTML === 'onree'"
                        " && r.collapsed && r.startContainer === p && r.startOffset === 1; })()"));
    CHECK(page->boolean("(() => { const p = document.createElement('p'); p.textContent = 'abcd'; const r = document.createRange(); r.setStart(p.firstChild, 2);"
                        " r.collapse(true); r.insertNode(document.createElement('i'));"
                        " const inserted = p.innerHTML === 'ab<i></i>cd' && r.startContainer === p.firstChild && r.startOffset === 2 && r.endContainer === p && r.endOffset === 2;"
                        " const s = document.createRange(); s.selectNodeContents(p.lastChild); s.surroundContents(document.createElement('u'));"
                        " return inserted && p.innerHTML === 'ab<i></i><u>cd</u>' && s.startContainer === p && s.startOffset === 3 && s.endOffset === 4; })()"));
    // A replacement refused leaves the tree, and so the range, as it was.
    CHECK(page->boolean("(() => { const p = document.createElement('p'); p.textContent = 'x'; const r = document.createRange(); r.selectNodeContents(p);"
                        " try { p.replaceChild(p, p.firstChild); } catch (e) { if (e.name !== 'HierarchyRequestError') return false; }"
                        " return p.childNodes.length === 1 && r.endOffset === 1; })()"));
    // A static range checks only the node types.
    CHECK(page->boolean("(() => { const a = document.getElementById('a'); const s = new StaticRange({ startContainer: a, startOffset: 7, endContainer: document, endOffset: 0 });"
                        " return s instanceof AbstractRange && !(s instanceof Range) && s.startContainer === a && s.startOffset === 7 && !s.collapsed; })()"));
    CHECK(page->throws("new StaticRange({ startContainer: document.implementation.createDocumentType('x', '', ''), startOffset: 0, endContainer: document, endOffset: 0 })")
              .starts_with("InvalidNodeTypeError"));
    // A contextual fragment's scripts run once inserted.
    CHECK(page->boolean("(() => { const r = document.createRange(); r.selectNodeContents(document.getElementById('d'));"
                        " const f = r.createContextualFragment('<span>x</span>y<script>window.ran = 1<\\/script>'); const parsed = f instanceof DocumentFragment"
                        " && f.firstChild.localName === 'span' && f.textContent === 'xywindow.ran = 1'; document.body.appendChild(f); return parsed && window.ran === 1; })()"));
    // A range keeps its boundary nodes' wrappers, expandos and all.
    page->eval("window.kept = document.createRange(); kept.selectNodeContents(document.createElement('q')); kept.startContainer.mark = 5;");
    CHECK(page->boolean("(() => { for (let i = 0; i < 50; i++) ({ i }); return kept.startContainer.mark === 5 && kept.startContainer.localName === 'q'; })()"));
    CHECK_EQ(page->console, "");
}

// A range into a frame's document outlives the frame: the document goes when
// the iframe is removed, and every range holding its nodes lets go of both
// boundaries, a static range reaching into the page too.
void test_a_range_outlives_its_frames_document()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<iframe id=f srcdoc="<p id=p>inside</p>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    page->eval("(() => { const inner = document.getElementById('f').contentDocument; window.r = inner.createRange();"
               " r.selectNodeContents(inner.getElementById('p').firstChild);"
               " window.s = new StaticRange({ startContainer: inner.body, startOffset: 0, endContainer: document.body, endOffset: 0 }); })()");
    CHECK(page->boolean("r.toString() === 'inside' && s.startContainer.localName === 'body' && s.endContainer === document.body"));
    page->eval("document.getElementById('f').remove();");
    CHECK(page->boolean("(() => { for (let i = 0; i < 50; i++) ({ i }); return r.startContainer === null && r.endContainer === null && r.collapsed"
                        " && r.toString() === '' && s.startContainer === null && s.endContainer === null; })()"));
    CHECK(page->boolean("(() => { r.setStart(document.body, 0); return r.startContainer === document.body && r.endContainer === document.body; })()"));
    CHECK_EQ(page->console, "");
    page.reset();
}

void test_selectors_and_collections()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><div class="a b" id=x data-role="menu"><p class=a>1</p><p>2</p><span class=b>3</span></div>)HTML");
    CHECK_EQ(page->number("document.querySelectorAll('.a').length"), 2);
    CHECK_EQ(page->string("document.querySelector('#x > p:nth-child(2)').textContent"), "2");
    CHECK_EQ(page->string("document.querySelector('[data-role=menu] span').textContent"), "3");
    CHECK(page->boolean("document.querySelector('.missing') === null"));
    CHECK_EQ(page->number("document.getElementsByClassName('a').length"), 2);
    CHECK_EQ(page->number("document.getElementsByClassName('a b').length"), 1);
    CHECK_EQ(page->number("document.getElementsByTagName('p').length"), 2);
    CHECK_EQ(page->number("document.getElementsByTagName('*').length"), 7);
    CHECK_EQ(page->string("document.getElementsByTagName('P').item(1).textContent"), "2");
    CHECK(page->boolean("document.getElementsByTagName('p').item(5) === null"));
    CHECK(page->boolean("document.querySelector('span').matches('.b') && !document.querySelector('span').matches('p')"));
    CHECK(page->boolean("document.querySelector('span').closest('#x') === document.getElementById('x')"));
    CHECK(page->boolean("document.querySelector('span').closest('.nothing') === null"));
    CHECK(page->boolean("Array.isArray(document.querySelectorAll('p')) && typeof document.querySelectorAll('p').forEach === 'function'"));
    CHECK_EQ(page->string("Array.prototype.map.call(document.querySelectorAll('p'), function (p) { return p.textContent; }).join(',')"), "1,2");
    CHECK(page->boolean("document.querySelectorAll('p') instanceof NodeList && document.getElementsByTagName('p') instanceof HTMLCollection"));
    std::string const thrown = page->throws("document.querySelector('p[')");
    CHECK(thrown.starts_with("SyntaxError"));
    CHECK(page->boolean("(function () { try { document.querySelector('p['); } catch (e) { return e instanceof DOMException && e instanceof Error && e.name === 'SyntaxError' && e.code === 12; } })()"));
    CHECK(page->boolean("document.getElementById('x').getElementsByTagName('p').namedItem('nope') === null"));
    CHECK_EQ(page->number("document.getElementById('x').querySelectorAll('.a').length"), 1);
    // document.all (HTML's "all exotic object"): a real HTMLCollection of
    // every element, but wearing [[IsHTMLDDA]] (Annex B.3.7) so it reads as
    // undefined everywhere except a direct reference to it — the guard a
    // page takes to be sure a value is genuinely absent, `x !== document.all`,
    // must not itself be fooled into treating undefined as "present".
    CHECK_EQ(page->number("document.all.length"), 7);
    CHECK(page->boolean("document.all instanceof HTMLCollection"));
    CHECK_EQ(page->string("typeof document.all"), "undefined");
    CHECK(page->boolean("document.all == null && document.all == undefined"));
    CHECK(page->boolean("document.all !== null && document.all !== undefined"));
    CHECK(page->boolean("!document.all"));
    CHECK(page->boolean("(function (f) { return !f && f !== document.all; })(undefined) === true"));
    CHECK(page->boolean("(function () { var all = document.all; return !all && all !== all; })() === false"));
    CHECK_EQ(page->console, "");
}

void test_attributes_classlist_style_dataset()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><div id=d class="one two" data-user-id="7" hidden title=T style="color: red; margin-top: 4px"></div>)HTML");
    page->eval("var d = document.getElementById('d');");
    CHECK_EQ(page->string("d.className"), "one two");
    CHECK_EQ(page->number("d.classList.length"), 2);
    CHECK(page->boolean("d.classList.contains('two') && !d.classList.contains('three')"));
    page->eval("d.classList.add('three', 'one'); d.classList.remove('two');");
    CHECK_EQ(page->string("d.getAttribute('class')"), "one three");
    CHECK(page->boolean("d.classList.toggle('four') === true && d.classList.toggle('four') === false && d.classList.toggle('five', true) === true && d.classList.toggle('five', true) === true"));
    CHECK_EQ(page->string("d.classList.value"), "one three five");
    CHECK_EQ(page->string("d.classList[0] + d.classList.item(2)"), "onefive");
    CHECK(page->boolean("d.classList.replace('one', 'uno') && d.className === 'uno three five'"));
    CHECK(page->throws("d.classList.add('')").starts_with("SyntaxError"));
    CHECK(page->throws("d.classList.add('a b')").starts_with("InvalidCharacterError"));
    // Attributes.
    CHECK(page->boolean("d.hasAttribute('hidden') && d.hidden === true"));
    page->eval("d.hidden = false; d.setAttribute('Title', 'new'); d.setAttribute('aria-label', 'L');");
    CHECK(page->boolean("!d.hasAttribute('hidden') && d.title === 'new' && d.getAttribute('title') === 'new'"));
    CHECK_EQ(page->number("d.attributes.length"), 6);
    CHECK_EQ(page->string("d.getAttributeNames().join()"), "id,class,data-user-id,title,style,aria-label");
    CHECK_EQ(page->string("d.attributes[0].name + '=' + d.attributes[0].value"), "id=d");
    CHECK_EQ(page->string("d.attributes.getNamedItem('title').value"), "new");
    CHECK(page->boolean("d.toggleAttribute('open') && d.hasAttribute('open') && !d.toggleAttribute('open')"));
    CHECK(page->boolean("d.getAttribute('nope') === null"));
    CHECK(page->throws("d.setAttribute('bad name', 'x')").starts_with("InvalidCharacterError"));
    // Style.
    CHECK_EQ(page->string("d.style.color"), "red");
    CHECK_EQ(page->string("d.style.marginTop"), "4px");
    CHECK_EQ(page->string("d.style.display"), "");
    page->eval("d.style.display = 'none'; d.style.setProperty('background-color', 'blue', 'important'); d.style.marginTop = '';");
    CHECK_EQ(page->string("d.getAttribute('style')"), "color: red; display: none; background-color: blue !important;");
    CHECK_EQ(page->string("d.style.cssText"), "color: red; display: none; background-color: blue !important;");
    CHECK_EQ(page->string("d.style.getPropertyValue('display') + d.style.getPropertyPriority('background-color')"), "noneimportant");
    CHECK_EQ(page->number("d.style.length"), 3);
    CHECK_EQ(page->string("d.style.item(1)"), "display");
    CHECK_EQ(page->string("d.style.removeProperty('color')"), "red");
    page->eval("d.style.cssText = 'width: 10px'; d.style.cssFloat = 'left';");
    CHECK_EQ(page->string("d.getAttribute('style')"), "width: 10px; float: left;");
    CHECK(page->boolean("d.style instanceof CSSStyleDeclaration"));
    // style, classList and relList are [PutForwards]: assigning to one
    // assigns to its cssText or value, and in strict code that must not
    // throw, since a read-only property would.
    page->eval(R"JS("use strict";
        d.style = 'height: 3px';
        d.classList = 'x y';
        var link = document.createElement('a'); link.relList = 'noopener';
        var g = document.createElementNS('http://www.w3.org/2000/svg', 'g'); g.style = 'opacity: 0.5';
    )JS");
    CHECK_EQ(page->string("d.getAttribute('style') + '|' + d.className + '|' + link.rel + '|' + g.getAttribute('style')"),
        "height: 3px;|x y|noopener|opacity: 0.5;");
    // Dataset.
    CHECK_EQ(page->string("d.dataset.userId"), "7");
    page->eval("d.dataset.fooBar = 'baz'; delete d.dataset.userId;");
    CHECK(page->boolean("d.getAttribute('data-foo-bar') === 'baz' && !d.hasAttribute('data-user-id') && d.dataset.userId === undefined"));
    CHECK_EQ(page->string("Object.keys(d.dataset).join()"), "fooBar");
    CHECK_EQ(page->console, "");
}

void test_css_supports()
{
    // Both CSS.supports forms go through the same <supports-condition>
    // evaluator @supports itself uses: they ask whether the style resolver
    // actually knows the property and accepts the value, not just whether
    // the text parses as some declaration.
    auto page = loaded(R"HTML(<!DOCTYPE html><div id=d></div>)HTML");
    CHECK(page->boolean("CSS.supports('display', 'block')"));
    CHECK(!page->boolean("CSS.supports('totally-not-a-real-property', '42')"));
    CHECK(!page->boolean("CSS.supports('display', 'not-a-real-keyword-xyz')"));
    CHECK(page->boolean("CSS.supports('(display: block)')"));
    CHECK(page->boolean("CSS.supports('display: block')")); // a bare declaration is wrapped in parens
    CHECK(!page->boolean("CSS.supports('totally-not-a-real-property: 42')"));
    CHECK(page->boolean("CSS.supports('not (totally-not-a-real-property: 42)')"));
    CHECK(page->boolean("CSS.supports('(display: block) and (color: red)')"));
    // appearance's initial value is none, so none is found supported only
    // against a baseline that is not none as well.
    CHECK(page->boolean("CSS.supports('appearance', 'none')"));
    CHECK(page->boolean("CSS.supports('-webkit-appearance', 'none')"));
    CHECK(page->boolean("CSS.supports('appearance', 'auto')"));
    CHECK(!page->boolean("CSS.supports('appearance', 'no-such-look')"));
    // The compat keywords are the specification's list; the older
    // push-button and its kind are not on it.
    CHECK(page->boolean("CSS.supports('appearance', 'menulist-button')"));
    CHECK(!page->boolean("CSS.supports('appearance', 'push-button')"));
    CHECK(!page->boolean("CSS.supports('-webkit-appearance', 'square-button')"));
    CHECK(!page->boolean("CSS.supports('(display: block) and (totally-not-a-real-property: 1)')"));
    CHECK(page->boolean("CSS.supports('(display: block) or (totally-not-a-real-property: 1)')"));
    CHECK(page->boolean("CSS.supports('selector(div > span)')"));
    CHECK(!page->boolean("CSS.supports('selector(:not-a-real-pseudo-xyz)')"));
    CHECK(page->boolean("CSS.supports('(--any-custom-prop: this is not even a real value !!)')"));
    // A pseudo-element inside :is()/:where()/:has() is invalid even when it
    // is one settle_pseudo_elements does not lift onto ComplexSelector
    // (::first-line, ::marker, ...): still a pseudo-element among the
    // compound's simples, not a bare compound selector.
    CHECK(!page->boolean("CSS.supports('selector(:is(::first-line))')"));
    // A value equal to the initial one is supported too.
    CHECK(page->boolean("CSS.supports('font-synthesis-weight', 'auto')"));
    CHECK(page->boolean("CSS.supports('font-weight', 'normal')"));
    CHECK(page->boolean("CSS.supports('font-synthesis', 'weight style small-caps position')"));
    CHECK(!page->boolean("CSS.supports('font-synthesis', 'none weight')"));
    // Laid out as CSS Display 3 has it, so claimed.
    CHECK(page->boolean("CSS.supports('display', 'contents')"));
    // var() makes any value of a known property valid until substitution.
    CHECK(page->boolean("CSS.supports('color: something-pointless var(--foo)')"));
    CHECK(page->boolean("CSS.supports('color', 'fn(var(--foo, 1px))')"));
    CHECK(!page->boolean("CSS.supports('color', 'var(foo)')"));
    CHECK(!page->boolean("CSS.supports('not-a-property', 'var(--foo)')"));
    // at-rule() names the rules the engine reads.
    CHECK(page->boolean("CSS.supports('at-rule(@media) and (color: red)')"));
    CHECK(!page->boolean("CSS.supports('not at-rule( @supports )')"));
    CHECK(!page->boolean("CSS.supports('at-rule(@counter-style)')"));
    CHECK_EQ(page->console, "");
}

void test_attribute_names_global_this_and_shadow_root()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><div id=d onfocusin=t data-x=1 item=i></div>)HTML");
    page->eval("var d = document.getElementById('d');");
    // WebIDL's named properties on NamedNodeMap: jQuery 1.x reads `attributes[name].expando`.
    CHECK_EQ(page->string("d.attributes['onfocusin'].value"), "t");
    CHECK_EQ(page->string("d.attributes['data-x'].name"), "data-x");
    CHECK(page->boolean("d.attributes['onfocusin'].expando === undefined"));
    CHECK(page->boolean("d.attributes.nope === undefined"));
    // A prototype name wins over an attribute of that name; nothing is enumerable but the indices.
    CHECK(page->boolean("typeof d.attributes.item === 'function' && d.attributes.length === 4"));
    CHECK_EQ(page->string("Object.keys(d.attributes).join()"), "0,1,2,3");
    // A bare call on the global, or one through a saved reference: undefined `this` is the window.
    page->eval("var hits = 0; addEventListener('ping', function () { hits++; }); dispatchEvent(new Event('ping')); var a = window.addEventListener; a('ping', function () { hits += 10; }); window.dispatchEvent(new Event('ping'));");
    CHECK_EQ(page->number("hits"), 12);
    CHECK(page->throws("EventTarget.prototype.addEventListener.call({}, 'x', function () {})").starts_with("TypeError"));
    // ShadowRoot is an interface; nothing makes one yet.
    CHECK(page->boolean("typeof ShadowRoot === 'function' && !(d instanceof ShadowRoot) && Object.getPrototypeOf(ShadowRoot.prototype) === DocumentFragment.prototype"));
    CHECK(page->throws("new ShadowRoot()").starts_with("TypeError"));
    CHECK(page->throws("d.attachShadow({ mode: 'open' })").starts_with("NotSupportedError"));
    CHECK_EQ(page->console, "");
}

void test_event_dispatch_order_and_flags()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><div id=outer><span id=inner>x</span></div>)HTML");
    page->eval(R"JS(
        var log = [];
        var outer = document.getElementById('outer'), inner = document.getElementById('inner');
        function l(name, capture) { return function (e) { log.push(name + ':' + e.eventPhase + ':' + (e.currentTarget === this)); }; }
        window.addEventListener('ping', l('wc', true), true);
        document.addEventListener('ping', l('dc'), { capture: true });
        outer.addEventListener('ping', l('oc'), true);
        inner.addEventListener('ping', l('ib'));
        inner.addEventListener('ping', l('ic'), true);
        outer.addEventListener('ping', l('ob'));
        document.addEventListener('ping', l('db'));
        window.addEventListener('ping', l('wb'));
        var result = inner.dispatchEvent(new Event('ping', { bubbles: true, cancelable: true }));
    )JS");
    CHECK_EQ(page->string("log.join(' ')"), "wc:1:true dc:1:true oc:1:true ic:2:true ib:2:true ob:3:true db:3:true wb:3:true");
    CHECK(page->boolean("result === true"));
    // A non-bubbling event stops at the target; the capture phase still runs.
    page->eval("log = []; inner.dispatchEvent(new Event('ping'));");
    CHECK_EQ(page->string("log.join(' ')"), "wc:1:true dc:1:true oc:1:true ic:2:true ib:2:true");
    // stopPropagation, stopImmediatePropagation, preventDefault, once, removal during dispatch.
    page->eval(R"JS(
        log = [];
        var target = document.createElement('p'); outer.appendChild(target);
        target.addEventListener('go', function (e) { log.push('a'); e.stopPropagation(); e.preventDefault(); });
        target.addEventListener('go', function (e) { log.push('b'); }, { once: true });
        var c = function () { log.push('c'); }; target.addEventListener('go', c);
        target.addEventListener('go', function () { target.removeEventListener('go', c); log.push('d'); });
        target.addEventListener('go', c); // a duplicate registration is ignored
        outer.addEventListener('go', function () { log.push('outer'); });
        var r1 = target.dispatchEvent(new Event('go', { bubbles: true, cancelable: true }));
        var r2 = target.dispatchEvent(new Event('go', { bubbles: true, cancelable: true }));
    )JS");
    CHECK_EQ(page->string("log.join(' ')"), "a b c d a d");
    CHECK(page->boolean("r1 === false && r2 === false"));
    page->eval(R"JS(
        log = [];
        target.addEventListener('stop', function (e) { log.push(1); e.stopImmediatePropagation(); });
        target.addEventListener('stop', function () { log.push(2); });
        target.dispatchEvent(new Event('stop'));
        var ev = new CustomEvent('data', { detail: { n: 5 } });
        var seen;
        target.addEventListener('data', function (e) { seen = e.detail.n + ':' + e.type + ':' + e.isTrusted + ':' + (e.target === target); });
        target.dispatchEvent(ev);
        var handled = { handleEvent: function (e) { log.push('obj:' + (this === handled)); } };
        target.addEventListener('obj', handled);
        target.dispatchEvent(new Event('obj'));
    )JS");
    CHECK_EQ(page->string("log.join(' ')"), "1 obj:true");
    CHECK_EQ(page->string("seen"), "5:data:false:true");
    CHECK(page->throws("target.dispatchEvent({})").starts_with("TypeError"));
    CHECK(page->boolean("(function () { var e = document.createEvent('Event'); try { target.dispatchEvent(e); return false; } catch (x) { return x.name === 'InvalidStateError'; } })()"));
    CHECK(page->boolean("(function () { var e = document.createEvent('Event'); e.initEvent('init', true, false); var got = false; target.addEventListener('init', function () { got = true; }); target.dispatchEvent(e); return got; })()"));
    // The on<type> handlers: property, content attribute, and `return false`.
    page->eval(R"JS(
        log = [];
        inner.onclick = function (e) { log.push('prop:' + (this === inner) + ':' + e.type); };
        outer.setAttribute('onclick', "log.push('attr:' + (this === outer) + ':' + event.type); return false;");
        var canceled = !inner.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, clientX: 3 }));
    )JS");
    CHECK_EQ(page->string("log.join(' ')"), "prop:true:click attr:true:click");
    CHECK(page->boolean("canceled === true"));
    CHECK(page->boolean("typeof inner.onclick === 'function' && typeof outer.onclick === 'function' && inner.onmouseover === null"));
    page->eval("outer.removeAttribute('onclick'); inner.onclick = null; log = []; inner.dispatchEvent(new Event('click', { bubbles: true }));");
    CHECK_EQ(page->string("log.join(' ')"), "");
    // From the host: a trusted click with coordinates, cancelable.
    page->eval("log = []; inner.addEventListener('click', function (e) { log.push(e.clientX + ',' + e.clientY + ',' + e.button + ',' + e.isTrusted + ',' + e.pageX + ',' + e.detail + ',' + (e instanceof MouseEvent) + ',' + (e instanceof UIEvent)); e.preventDefault(); });");
    dom::Element* inner = static_cast<dom::Element*>(page->realm->node_of(page->eval("inner").value));
    CHECK(inner != nullptr);
    bindings::Realm::MouseInit click;
    click.client_x = 10;
    click.client_y = 20;
    CHECK(!page->realm->dispatch_mouse_event(*inner, "click", click));
    CHECK_EQ(page->string("log.join(' ')"), "10,20,0,true,10,1,true,true");
    page->eval("log = []; document.addEventListener('keydown', function (e) { log.push(e.key + '/' + e.code + '/' + e.keyCode + '/' + e.ctrlKey + '/' + (e.target === document.body)); });");
    bindings::Realm::KeyInit key;
    key.key = "Enter";
    key.code = "Enter";
    key.key_code = 13;
    dom::Element* body = static_cast<dom::Element*>(page->realm->node_of(page->eval("document.body").value));
    CHECK(page->realm->dispatch_key_event(body, "keydown", key));
    CHECK_EQ(page->string("log.join(' ')"), "Enter/Enter/13/false/true");
    CHECK_EQ(page->realm->stats().uncaught_errors, 0);
    CHECK_EQ(page->console, "");
}

void test_timers_microtasks_and_the_clock()
{
    auto page = loaded("<!DOCTYPE html><body></body>");
    page->eval(R"JS(
        var log = [];
        var f = setTimeout(function (a, b) { log.push('f' + a + b); }, 100, 'A', 'B');
        var g = setTimeout(function () { log.push('g'); }, 50);
        var h = setTimeout(function () { log.push('h'); setTimeout(function () { log.push('nested'); }, 0); }, 100);
        var cleared = setTimeout(function () { log.push('never'); }, 60);
        clearTimeout(cleared);
        setTimeout('log.push("str")', 0);
        queueMicrotask(function () { log.push('micro1'); queueMicrotask(function () { log.push('micro2'); }); });
        log.push('sync');
    )JS");
    // The microtasks ran as the script finished, before any timer.
    CHECK_EQ(page->string("log.join(' ')"), "sync micro1 micro2");
    CHECK(page->boolean("f > 0 && g > f && h > g"));
    CHECK(page->realm->has_pending_timers());
    CHECK_EQ(*page->realm->next_timer_due(), 1000.0);
    page->clock = 1049;
    CHECK(page->realm->run_pending());
    CHECK_EQ(page->string("log.join(' ')"), "sync micro1 micro2 str");
    CHECK(!page->realm->run_pending());
    page->clock = 1100;
    CHECK(page->realm->run_pending());
    // g (50) before f and h (100, in creation order); the nested zero-delay
    // timer set while running waits for the next pump.
    CHECK_EQ(page->string("log.join(' ')"), "sync micro1 micro2 str g fAB h");
    CHECK(page->realm->run_pending());
    CHECK_EQ(page->string("log.join(' ')"), "sync micro1 micro2 str g fAB h nested");
    CHECK(!page->realm->has_pending_timers());
    // Intervals repeat until cleared; a microtask queued in a timer runs
    // right after it, before the next timer.
    page->eval(R"JS(
        log = [];
        var n = 0;
        var iv = setInterval(function () { n++; queueMicrotask(function () { log.push('m' + n); }); log.push('i' + n); if (n === 3) clearInterval(iv); }, 10);
        setTimeout(function () { log.push('t'); }, 25);
    )JS");
    page->clock = 1200;
    CHECK(page->realm->run_pending()); // the interval is due at 1110: one run, re-armed at 1210
    CHECK_EQ(page->string("log.join(' ')"), "i1 m1 t");
    page->clock = 1300;
    page->realm->run_pending();
    page->clock = 1400;
    page->realm->run_pending();
    CHECK_EQ(page->string("log.join(' ')"), "i1 m1 t i2 m2 i3 m3");
    CHECK(!page->realm->has_pending_timers());
    CHECK_EQ(page->realm->stats().timers_fired, 9);
    // requestAnimationFrame gets a timestamp; cancelAnimationFrame takes it back.
    page->eval("log = []; var raf = requestAnimationFrame(function (ts) { log.push('raf:' + (typeof ts === 'number') + ':' + (ts >= 0)); }); cancelAnimationFrame(requestAnimationFrame(function () { log.push('no'); }));");
    page->clock = 1500;
    page->realm->run_pending();
    CHECK_EQ(page->string("log.join(' ')"), "raf:true:true");
    // A throwing timer is reported and the loop goes on.
    page->eval("setTimeout(function () { throw new RangeError('late'); }, 1); setTimeout(function () { log.push('after'); }, 2);");
    page->clock = 1600;
    page->realm->run_pending();
    CHECK(page->console.find("error:Uncaught RangeError: late") != std::string::npos);
    CHECK_EQ(page->string("log.join(' ')"), "raf:true:true after");
    CHECK_EQ(page->realm->stats().uncaught_errors, 1);
    CHECK_EQ(page->number("performance.now()"), 600);
}

void test_document_ready_states_and_load_events()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><html><head><script>
        var log = [document.readyState];
        document.addEventListener('readystatechange', function () { log.push('rs:' + document.readyState); });
        document.addEventListener('DOMContentLoaded', function (e) { log.push('dcl:' + e.bubbles + ':' + (e.target === document)); });
        window.addEventListener('load', function (e) { log.push('load:' + (e.target === window) + ':' + document.readyState); });
        window.onload = function () { log.push('onload'); };
        document.addEventListener('load', function () { log.push('doc-load-should-not-fire'); });
    </script></head><body onunload="" onload="log.push('body-onload:' + (this === window))"><p>hi</p><script>log.push('inline:' + document.readyState);</script></body></html>)HTML");
    CHECK_EQ(page->string("log.join(' ')"), "loading inline:loading rs:interactive dcl:true:true rs:complete onload load:true:complete");
    CHECK_EQ(page->realm->ready_state(), "complete");
    CHECK_EQ(page->console, "");
}

void test_frame_load_events()
{
    // A page's load waits on its frames: an iframe with nothing to show
    // gets its about:blank document and fires load as the parser inserts it,
    // while the document is still loading; every other iframe the parse left
    // in the tree fires load after DOMContentLoaded, in tree order, without
    // bubbling, and before the window's — which fires once. One inside a
    // template is not in the tree.
    auto page = loaded(R"HTML(<!DOCTYPE html><html><head><script>
        var log = [];
        document.addEventListener('DOMContentLoaded', function () { log.push('dcl'); });
        window.addEventListener('load', function () { log.push('window'); });
        document.addEventListener('load', function () { log.push('bubbled'); });
    </script></head><body>
    <iframe id="first" onload="log.push('first:' + document.readyState + ':' + (event.target === this) + ':' + event.bubbles)"></iframe>
    <div><iframe id="second" srcdoc="<p>x"></iframe></div>
    <template><iframe onload="log.push('template')"></iframe></template>
    <script>document.getElementById('second').addEventListener('load', function () { log.push('second'); });</script>
    </body></html>)HTML");
    CHECK_EQ(page->string("log.join(' ')"), "first:loading:true:false dcl second window");
    CHECK_EQ(page->console, "");
}

void test_named_access_on_the_window()
{
    // An HTML element's id, and the name of an embed, form, img or object,
    // is a property of the window: one element as itself, several as a
    // collection in tree order — behind the window's own properties, those
    // of Object.prototype and every variable a script declares, and read
    // from the tree as it stands at each lookup.
    auto page = loaded(R"HTML(<!DOCTYPE html><html><body>
    <div id="lone"></div>
    <p id="twice"></p><span id="twice"></span>
    <form name="login"></form><img name="logo"><a name="anchor"></a><svg><g id="drawn"></g></svg>
    <div id="document"></div><div id="toString"></div><div id="declared"></div>
    <script>var declared = 5; var early = typeof later;</script>
    <div id="later"></div>
    </body></html>)HTML");
    CHECK_EQ(page->string("typeof lone + ':' + lone.tagName + ':' + (window.lone === document.getElementById('lone'))"),
        "object:DIV:true");
    CHECK_EQ(page->string("twice.length + ':' + twice[0].tagName + ':' + twice[1].tagName"), "2:P:SPAN");
    CHECK_EQ(page->string("login.tagName + ':' + logo.tagName + ':' + typeof anchor + ':' + typeof drawn"),
        "FORM:IMG:undefined:undefined");
    CHECK_EQ(page->string("(window.document === document) + ':' + typeof toString + ':' + declared"), "true:function:5");
    CHECK_EQ(page->string("early + ':' + ('lone' in window) + ':' + window.hasOwnProperty('lone')"), "undefined:true:false");
    // A name the tree stops holding stops answering, and one a script sets
    // on the window shadows the element.
    page->eval("document.getElementById('lone').remove(); window.twice = 'mine';");
    CHECK_EQ(page->string("typeof lone + ':' + twice + ':' + typeof later"), "undefined:mine:object");
    // An element drawn in SVG has an inline style of its own, as an HTML
    // one does: pages that draw their icons that way set it constantly.
    page->eval(R"JS(
        var drawing = document.querySelector('svg > g');
        drawing.style.pointerEvents = 'none';
        drawing.style.cssText = 'opacity: 0.5';
    )JS");
    CHECK_EQ(page->string("typeof drawing.style + ':' + drawing.getAttribute('style')"), "object:opacity: 0.5;");
    CHECK_EQ(page->string("drawing.style.opacity + ':' + drawing.style.length"), "0.5:1");
    CHECK_EQ(page->console, "");
}

void test_external_deferred_and_skipped_scripts()
{
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html><head>
<script src="a.js"></script>
<script defer src="../b.js"></script>
<script async src="c.js"></script>
<script type="module">log.push('module');</script>
<script type="text/template"><p>not code</p></script>
<script type="application/ld+json">{"not": "code"}</script>
<script nomodule>log.push('nomodule');</script>
<script src="missing.js" onerror="log.push('error:' + event.type + ':' + (event.target === this))"></script>
<script src="a.js"></script>
</head><body><script>log.push('body'); document.addEventListener('DOMContentLoaded', function () { log.push('dcl'); });</script></body>)HTML");
    page->scripts["https://example.test/dir/a.js"] = "var log = log || []; log.push('a');";
    page->scripts["https://example.test/b.js"] = "log.push('b:' + document.body.tagName);";
    page->scripts["https://example.test/dir/c.js"] = "log.push('c');";
    page->load();
    // a runs at once (twice: two elements), the data blocks are skipped,
    // nomodule runs, the failed fetch fires error, and the deferred and
    // async ones and the module run after the parse in order, before
    // DOMContentLoaded.
    CHECK_EQ(page->string("log.join(' ')"), "a nomodule error:error:true a body b:BODY c module dcl");
    CHECK_EQ(page->realm->stats().scripts_skipped, 2);
    CHECK_EQ(page->realm->stats().external_fetched, 4);
    CHECK_EQ(page->realm->stats().external_failed, 1);
    CHECK_EQ(page->realm->stats().scripts_run, 7);
    CHECK_EQ(page->realm->stats().modules_run, 1);
    CHECK_EQ(page->fetched.size(), 5u);
    CHECK_EQ(page->fetched[1], "https://example.test/b.js");
    CHECK(page->console.find("missing.js could not be loaded") != std::string::npos);
}

void test_module_scripts()
{
    // Module scripts on the page: an inline module and the external one it
    // imports, in one map with live bindings across them; the inline
    // module's base and import.meta.url are the document's, the external
    // one's its own; module code is strict, has no `this` and no
    // document.currentScript; a parser-inserted module waits for the parse
    // in document order with the deferred classic scripts, before
    // DOMContentLoaded, while an async one and a script-inserted one run at
    // once; and a second element naming the same module evaluates nothing
    // twice but still gets its load event.
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html><head>
<script>var log = ['start']; function count_seen() { return window.exported ? window.exported.count : 'none'; }</script>
<script type="module">
  import { count, bump } from './counter.js';
  import * as ns from "./counter.js";
  log.push('module:' + count + ':' + (this === undefined) + ':' + (document.currentScript === null) + ':' + import.meta.url);
  bump(); bump();
  log.push('live:' + count + ':' + ns.count);
  try { undeclared = 1; } catch (e) { log.push('strict:' + e.name); }
  window.exported = ns;
</script>
<script defer src="deferred.js"></script>
<script type="module" src="counter.js" onload="log.push('counter-load')"></script>
<script type="module" async>log.push('async:' + document.readyState);</script>
</head><body><script>log.push('body'); document.addEventListener('DOMContentLoaded', function () { log.push('dcl:' + count_seen()); });
var inserted = document.createElement('script'); inserted.type = 'module'; inserted.textContent = "log.push('inserted:' + document.readyState);"; document.head.appendChild(inserted);</script></body>)HTML");
    page->module("https://example.test/dir/counter.js", "log.push('counter:' + import.meta.url); export let count = 0; export function bump() { count += 1; }");
    page->scripts["https://example.test/dir/deferred.js"] = "log.push('deferred:' + count_seen());";
    page->load();
    CHECK_EQ(page->string("log.join(' ')"),
        "start async:loading body inserted:loading counter:https://example.test/dir/counter.js "
        "module:0:true:true:https://example.test/dir/page.html live:2:2 strict:ReferenceError deferred:2 counter-load dcl:2");
    CHECK_EQ(page->realm->stats().modules_run, 4);
    CHECK_EQ(page->realm->stats().scripts_run, 7);
    CHECK_EQ(page->realm->stats().scripts_failed, 0);
    // The module was fetched once, as a CORS GET with same-origin credentials.
    CHECK_EQ(page->requests.size(), 1u);
    CHECK_EQ(page->requests[0], "GET https://example.test/dir/counter.js");
    CHECK_EQ(page->console, "");

    // Every way a module graph fails: a missing module, a non-JavaScript
    // type, a bare specifier, a syntax error, and a cross-origin module
    // whose response does not allow this origin, each the element's error
    // event and a console line at the time the graph is fetched; a module
    // that throws when it runs is an uncaught error and its element still
    // gets load; a cross-origin module that opens itself with CORS runs.
    auto bad = std::make_unique<Page>(R"HTML(<!DOCTYPE html><body>
<script>var log = [];</script>
<script type="module" src="missing.js" onerror="log.push('missing:' + event.type)"></script>
<script type="module" src="plain.txt" onerror="log.push('plain:' + event.type)"></script>
<script type="module" onerror="log.push('bare:' + event.type)">import x from 'bare';</script>
<script type="module" onerror="log.push('syntax:' + event.type)">export var = 1;</script>
<script type="module" src="thrower.js" onerror="log.push('thrower-error')" onload="log.push('thrower-load')"></script>
<script type="module" src="https://other.test/cors.js" onerror="log.push('cors:' + event.type)" onload="log.push('cors-load')"></script>
<script type="module" src="https://other.test/open.js" onload="log.push('open-load')"></script>
</body>)HTML");
    bad->module("https://example.test/dir/plain.txt", "export default 1", "text/plain");
    bad->module("https://example.test/dir/thrower.js", "throw new RangeError('module boom');");
    bad->module("https://other.test/cors.js", "log.push('cors ran');");
    bad->module("https://other.test/open.js", "log.push('open ran');", "text/javascript", true);
    bad->load();
    CHECK_EQ(bad->string("log.join(' ')"), "missing:error plain:error bare:error syntax:error cors:error thrower-load open ran open-load");
    CHECK_EQ(bad->realm->stats().uncaught_errors, 1);
    CHECK_EQ(bad->realm->stats().scripts_failed, 1);
    CHECK_EQ(bad->realm->stats().external_failed, 3);
    CHECK(bad->console.find("module boom") != std::string::npos);
    CHECK(bad->console.find("not a JavaScript type") != std::string::npos);
    CHECK(bad->console.find("Failed to resolve module specifier 'bare'") != std::string::npos);

    // A top-level await in a page module runs through the checkpoint the
    // host takes on the way out, so the page sees its end before
    // DOMContentLoaded; and import() from a classic script resolves against
    // the document, on the same map.
    auto tla = std::make_unique<Page>(R"HTML(<!DOCTYPE html><body><script>var log = [];</script>
<script type="module">log.push('a'); await Promise.resolve(); log.push('b'); window.done = true;</script>
<script>document.addEventListener('DOMContentLoaded', function () { log.push('dcl:' + (window.done === true)); });
import('./m.js').then(function (ns) { log.push('dynamic:' + ns.v + ':' + (ns.v_url === import_url())); });
function import_url() { return 'https://example.test/dir/m.js'; }</script></body>)HTML");
    tla->module("https://example.test/dir/m.js", "export const v = 'v'; export const v_url = import.meta.url;");
    tla->load();
    CHECK_EQ(tla->string("log.join(' ')"), "dynamic:v:true a b dcl:true");
    CHECK_EQ(tla->console, "");
}

void test_noscript_is_raw_text_with_scripting_on()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><head><noscript><link rel=stylesheet href=x.css></noscript></head><body><noscript><p id=n>Enable scripts</p></noscript><p id=y>shown</p></body>)HTML");
    CHECK(page->boolean("document.getElementById('n') === null"));
    CHECK(page->boolean("document.getElementById('y') !== null"));
    CHECK_EQ(page->string("document.querySelector('body noscript').textContent"), "<p id=n>Enable scripts</p>");
    CHECK_EQ(page->string("document.querySelector('body noscript').innerHTML"), "<p id=n>Enable scripts</p>");
    // innerHTML in a scripting document parses noscript the same way.
    page->eval("document.body.innerHTML = '<noscript><b>raw</b></noscript>';");
    CHECK_EQ(page->number("document.getElementsByTagName('b').length"), 0);
    // With scripting off (no runner) noscript is content.
    auto plain = html::parse_document("<!DOCTYPE html><body><noscript><p id=n>x</p></noscript>");
    CHECK_EQ(html::serialize_children(*plain), "<!DOCTYPE html><html><head></head><body><noscript><p id=\"n\">x</p></noscript></body></html>");
}

void test_document_write_during_parsing()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><body><p id=a>a</p><script>document.write('<p id=w>written</p><scr' + 'ipt>document.write("<b>inner</b>")</scr' + 'ipt>');</script><p id=b>b</p></body>)HTML");
    CHECK_EQ(page->string("Array.prototype.map.call(document.body.children, function (e) { return e.tagName + (e.id ? '#' + e.id : ''); }).join(' ')"),
        "P#a SCRIPT P#w SCRIPT B P#b");
    // After the parse a write cannot land anywhere; it is reported, not lost silently.
    page->eval("document.write('<p>late</p>');");
    CHECK(page->console.find("document.write after the document was parsed") != std::string::npos);
    CHECK_EQ(page->number("document.getElementsByTagName('p').length"), 3);

    // An external script written during the parse is fetched and runs in
    // order, before the markup after it; writeln ends its text with a
    // newline; open() answers with the document and close() is nothing,
    // since a document is never replaced through them.
    auto external = std::make_unique<Page>(R"HTML(<!DOCTYPE html><body><script>var log = ['start']; document.write('<scr' + 'ipt src="lib.js"></scr' + 'ipt>'); document.writeln('<p id=w>x</p>'); log.push('after-write');</script><script>log.push('next');</script><p id=z>z</p></body>)HTML");
    external->scripts["https://example.test/dir/lib.js"] = "log.push('lib:' + document.getElementById('w') + ':' + document.getElementById('z'));";
    external->load();
    CHECK_EQ(external->string("log.join(' ')"), "start after-write lib:null:null next");
    CHECK_EQ(external->string("document.body.innerHTML.indexOf('<p id=\"w\">x</p>\\n') >= 0 ? 'newline' : document.body.innerHTML"), "newline");
    CHECK(external->boolean("document.open() === document && document.close() === undefined"));
    CHECK_EQ(external->number("document.getElementsByTagName('p').length"), 2);
}

void test_inserted_scripts_run_and_fragment_scripts_do_not()
{
    auto page = loaded("<!DOCTYPE html><body></body>");
    page->eval(R"JS(
        var s = document.createElement('script');
        s.textContent = 'window.ran = (window.ran || 0) + 1;';
        document.body.appendChild(s);
        document.body.appendChild(s); // already started: not again
        var detached = document.createElement('div');
        var s2 = document.createElement('script');
        s2.textContent = 'window.ran += 10;';
        detached.appendChild(s2); // not connected: waits
        document.body.appendChild(detached); // connected now: runs
        document.body.innerHTML += '<script>window.ran += 100;</scr' + 'ipt>'; // a fragment's scripts never run
        var wrapper = document.createElement('div');
        wrapper.innerHTML = '<script>window.ran += 1000;</scr' + 'ipt>';
        document.body.appendChild(wrapper); // still never: already started by the fragment parser
    )JS");
    CHECK_EQ(page->number("window.ran"), 11);
    CHECK_EQ(page->realm->stats().scripts_run, 2); // the appended one and the one connected later; a fragment's never
    CHECK_EQ(page->console, "");
}

void test_window_location_url_storage_navigator()
{
    auto page = loaded("<!DOCTYPE html><body></body>");
    CHECK_EQ(page->string("location.href"), "https://example.test/dir/page.html");
    CHECK_EQ(page->string("location.protocol + '|' + location.host + '|' + location.hostname + '|' + location.port + '|' + location.pathname + '|' + location.search + '|' + location.hash + '|' + location.origin"),
        "https:|example.test|example.test||/dir/page.html|||https://example.test");
    CHECK(page->boolean("window.location === document.location && location === window.location && String(location) === location.href"));
    CHECK(page->boolean("window === self && window === top && window === globalThis && window.window === window && document.defaultView === window"));
    page->eval("location.href = 'other.html?q=1#frag';");
    CHECK_EQ(page->navigations.size(), 1u);
    CHECK_EQ(page->navigations[0].serialize(), "https://example.test/dir/other.html?q=1#frag");
    page->eval("location.hash = 'top'; location.assign('/root');");
    CHECK_EQ(page->navigations.size(), 3u);
    CHECK_EQ(page->navigations[1].serialize(), "https://example.test/dir/page.html#top");
    CHECK_EQ(page->navigations[2].serialize(), "https://example.test/root");
    page->eval("history.pushState({ p: 1 }, '', '/pushed?x=2');");
    CHECK_EQ(page->string("location.pathname + location.search"), "/pushed?x=2");
    CHECK(page->boolean("history.state.p === 1 && history.length === 2"));
    CHECK(page->throws("history.pushState(null, '', 'https://other.test/')").starts_with("SecurityError"));
    // URL and URLSearchParams.
    page->eval("var u = new URL('../a/b?x=1&y=two%20words&x=3#h', 'https://h.test/p/q/');");
    CHECK_EQ(page->string("u.href"), "https://h.test/p/a/b?x=1&y=two%20words&x=3#h");
    CHECK_EQ(page->string("u.pathname + u.search + u.hash + u.origin + u.host"), "/p/a/b?x=1&y=two%20words&x=3#hhttps://h.testh.test");
    CHECK_EQ(page->string("u.searchParams.get('y') + '|' + u.searchParams.getAll('x').join(',') + '|' + u.searchParams.has('z')"), "two words|1,3|false");
    page->eval("u.searchParams.set('x', '9'); u.searchParams.append('z', 'a b'); u.searchParams.delete('y'); u.hash = 'new';");
    CHECK_EQ(page->string("u.href"), "https://h.test/p/a/b?x=9&z=a+b#new");
    CHECK_EQ(page->string("new URLSearchParams({ a: 1, b: 'x y' }).toString() + ' ' + new URLSearchParams('?k=v&k=w').getAll('k').length + ' ' + new URLSearchParams([['p', 'q']]).get('p')"), "a=1&b=x+y 2 q");
    CHECK(page->throws("new URL('nope')").starts_with("TypeError"));
    CHECK(page->boolean("URL.canParse('https://x.test') && !URL.canParse('nope') && u instanceof URL && JSON.stringify({ u: u }) === '{\"u\":\"https://h.test/p/a/b?x=9&z=a+b#new\"}'"));
    // Storage.
    page->eval("localStorage.setItem('k', 'v'); localStorage.other = 5; sessionStorage.setItem('s', '1');");
    CHECK_EQ(page->string("localStorage.getItem('k') + localStorage.k + localStorage.other + localStorage.length + localStorage.key(1) + (localStorage.getItem('nope') === null) + sessionStorage.length"), "vv52othertrue1");
    page->eval("localStorage.removeItem('k'); delete localStorage.other;");
    CHECK_EQ(page->number("localStorage.length"), 0);
    CHECK(page->boolean("localStorage instanceof Storage && Object.keys(sessionStorage).join() === 's'"));
    // Navigator, screen, sizes, media.
    CHECK(page->boolean("navigator.userAgent === 'Mozilla/5.0 TestAgent Sashfold/0.0' && navigator.language === 'en-US' && navigator.languages.length === 2 && navigator.onLine && navigator.cookieEnabled && !('serviceWorker' in navigator)"));
    CHECK(page->boolean("innerWidth === 1024 && innerHeight === 768 && screen.width === 1024 && devicePixelRatio === 1"));
    CHECK(page->boolean("matchMedia('(min-width: 500px)').matches && !matchMedia('(max-width: 500px)').matches && matchMedia('screen').media === 'screen'"));
    CHECK(page->boolean("typeof fetch === 'function' && typeof XMLHttpRequest === 'function' && typeof Promise === 'function'"));
    // Promise reactions and queueMicrotask share the checkpoint's queue.
    page->eval("var order = []; Promise.resolve().then(function () { order.push('reaction'); }); queueMicrotask(function () { order.push('microtask'); }); order.push('sync');");
    CHECK_EQ(page->string("order.join(' ')"), "sync reaction microtask");
    // An unhandled rejection is reported to the console once the checkpoint ends.
    page->eval("Promise.reject(new TypeError('nobody listens'));");
    CHECK(page->console.find("error:Uncaught (in promise) TypeError: nobody listens") != std::string::npos);
    CHECK_EQ(page->string("btoa('hello') + '|' + atob('aGVsbG8=') + '|' + atob(' aGk ')"), "aGVsbG8=|hello|hi");
    CHECK(page->throws("btoa('\\u0100')").starts_with("InvalidCharacterError"));
    CHECK(page->boolean("(function () { var o = structuredClone({ a: [1, { b: 2 }] }); return o.a[1].b === 2; })()"));
    CHECK(page->boolean("(function () { var e = new DOMException('m', 'NotFoundError'); return e.name === 'NotFoundError' && e.message === 'm' && e.code === 8 && e instanceof Error && DOMException.NOT_FOUND_ERR === 8; })()"));
    CHECK(page->boolean("typeof requestAnimationFrame === 'function' && typeof getComputedStyle === 'function' && typeof alert === 'function' && confirm('q') === false && prompt('p') === null"));
    CHECK(page->console.find("info:alert") == std::string::npos);
    // Cookies through the realm's own jar.
    page->eval("document.cookie = 'a=1; path=/'; document.cookie = 'b=2'; document.cookie = 'a=3';");
    CHECK_EQ(page->string("document.cookie"), "a=3; b=2");
    page->eval("document.cookie = 'b=; max-age=0';");
    CHECK_EQ(page->string("document.cookie"), "a=3");
    // An intersection observer measures at the end of a turn and delivers in
    // the task after it; a page with no layout has no boxes, so nothing it
    // watches is intersecting (test_intersection_observer has the geometry).
    page->eval("var io = new IntersectionObserver(function (entries, observer) { window.seen = entries.length + ':' + entries[0].isIntersecting + ':' + (entries[0].target === document.body) + ':' + (observer === io); }); io.observe(document.body); io.observe(document.documentElement);");
    page->clock += 10;
    page->realm->run_pending();
    page->realm->run_pending();
    CHECK_EQ(page->string("window.seen"), "2:false:true:true");
    CHECK(page->boolean("(function () { var m = new MutationObserver(function () {}); m.observe(document.body, { childList: true }); return m.takeRecords().length === 0; })()"));
    CHECK_EQ(page->realm->stats().uncaught_errors, 0);
}

void test_uncaught_errors_are_reported_and_counted()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><body>
<script>throw new TypeError('boom');</script>
<script>var log = ['after']; window.onerror = function (message, source, line, column, error) { log.push('onerror:' + message + ':' + (error instanceof SyntaxError)); return true; };</script>
<script>nope(</script>
<script>window.onerror = null; document.body.addEventListener('x', function () { throw new Error('in listener'); }); document.body.addEventListener('x', function () { log.push('second listener still runs'); }); document.body.dispatchEvent(new Event('x'));</script>
</body>)HTML");
    CHECK(page->console.find("error:Uncaught TypeError: boom") != std::string::npos);
    CHECK_EQ(page->string("log[0]"), "after");
    CHECK(page->string("log[1]").starts_with("onerror:Uncaught SyntaxError"));
    CHECK(page->string("log[1]").ends_with(":true"));
    CHECK_EQ(page->string("log[2]"), "second listener still runs");
    CHECK_EQ(page->realm->stats().scripts_run, 4);
    CHECK_EQ(page->realm->stats().scripts_failed, 2);
    CHECK_EQ(page->realm->stats().uncaught_errors, 3);
    // The one window.onerror swallowed is not on the console; the listener's is.
    CHECK(page->console.find("Uncaught SyntaxError") == std::string::npos);
    CHECK(page->console.find("Uncaught Error: in listener") != std::string::npos);
}

void test_layout_and_style_hooks()
{
    bindings::HostHooks hooks;
    hooks.layout_box = [](dom::Element const& element) -> std::optional<bindings::LayoutBox> {
        if (element.is_html("p"))
            return bindings::LayoutBox { 8, 40, 300, 20 };
        return std::nullopt;
    };
    css::ComputedStyle style;
    style.display = css::Display::Block;
    style.color = Color::rgb(255, 0, 0);
    style.font_size = 24;
    style.position = css::Position::Relative;
    style.opacity = 0.5f;
    hooks.computed_style = [&style](dom::Element const&) -> css::ComputedStyle const* { return &style; };
    hooks.scroll_position = [](dom::Document const&) { return std::pair<int, int> { 0, 30 }; };
    Page page("<!DOCTYPE html><body><p id=p>text</p><span id=s>hidden</span></body>", "https://example.test/", std::move(hooks));
    page.load();
    CHECK_EQ(page.string("(function () { var r = document.getElementById('p').getBoundingClientRect(); return [r.x, r.y, r.width, r.height, r.top, r.right, r.bottom, r.left].join(); })()"), "8,10,300,20,10,308,30,8");
    CHECK_EQ(page.string("(function () { var p = document.getElementById('p'); return [p.offsetWidth, p.offsetHeight, p.offsetTop, p.offsetLeft, p.clientWidth, p.getClientRects().length].join(); })()"), "300,20,40,8,300,1");
    CHECK_EQ(page.string("(function () { var s = document.getElementById('s'); var r = s.getBoundingClientRect(); return [s.offsetWidth, r.width, s.getClientRects().length, s.checkVisibility()].join(); })()"), "0,0,0,false");
    CHECK_EQ(page.string("(function () { var c = getComputedStyle(document.getElementById('p')); return [c.display, c.color, c.fontSize, c.position, c.opacity, c.getPropertyValue('font-size'), c.width, c.height].join('|'); })()"),
        "block|rgb(255, 0, 0)|24px|relative|0.5|24px|300px|20px");
    CHECK(page.throws("getComputedStyle(document.body).display = 'none'").starts_with("NoModificationAllowedError"));
    CHECK(page.boolean("scrollY === 30 && pageYOffset === 30 && scrollX === 0"));
    CHECK(page.boolean("(function () { var r = document.getElementById('p').getBoundingClientRect(); return JSON.stringify(r.toJSON()).indexOf('\"width\":300') > 0; })()"));
    CHECK_EQ(page.console, "");

    // A display of two device px per CSS px: the host answers in CSS px
    // already, the page sees the ratio, and matchMedia judges in CSS px.
    bindings::HostHooks scaled_hooks;
    scaled_hooks.device_scale = 2;
    scaled_hooks.viewport_width = 512;
    scaled_hooks.viewport_height = 384;
    scaled_hooks.layout_box = [](dom::Element const& element) -> std::optional<bindings::LayoutBox> {
        if (element.is_html("p"))
            return bindings::LayoutBox { 4, 20, 150, 10 };
        return std::nullopt;
    };
    Page scaled("<!DOCTYPE html><body><p id=p>text</p></body>", "https://example.test/", std::move(scaled_hooks));
    scaled.load();
    CHECK(scaled.boolean("devicePixelRatio === 2 && innerWidth === 512 && innerHeight === 384 && screen.width === 512"));
    CHECK(scaled.boolean("matchMedia('(max-width: 512px)').matches && !matchMedia('(min-width: 513px)').matches && matchMedia('(min-resolution: 2dppx)').matches"));
    CHECK_EQ(scaled.string("(function () { var r = document.getElementById('p').getBoundingClientRect(); return [r.x, r.y, r.width, r.height].join(); })()"), "4,20,150,10");
    CHECK_EQ(scaled.console, "");
}

// A page answered by a real style resolver and layout, as the renderer and
// the test runners answer it: the oracle is made once the page has its
// document, and the hooks reach it through the slot.
struct LaidOutPage {
    std::shared_ptr<std::unique_ptr<bindings::LayoutOracle>> slot = std::make_shared<std::unique_ptr<bindings::LayoutOracle>>();
    std::unique_ptr<Page> page;

    explicit LaidOutPage(std::string_view html)
    {
        bindings::HostHooks hooks;
        auto const oracle = slot;
        hooks.layout_box = [oracle](dom::Element const& element) { return (*oracle)->box(element); };
        hooks.computed_style = [oracle](dom::Element const& element) { return (*oracle)->style(element); };
        std::string const url = "https://example.test/";
        page = std::make_unique<Page>(html, url, std::move(hooks));
        *slot = std::make_unique<bindings::LayoutOracle>(*page->document, *net::parse_url(url), css::SheetFetcher {},
            css::MediaContext { 800, 600, 1 });
        (*slot)->set_realm(page->realm.get());
        page->load();
    }
};

void test_display_contents_geometry()
{
    // CSS Display 3 §2.5: the element has no box, so no rects, no offset
    // size and no offset parent; its child is laid out as its parent's,
    // 200 wide at the parent's corner (not 500 wide inside a border and
    // padding), and still inherits from it. An <img> computes to none
    // (Appendix B), a <button> does not. The DOM is as written.
    LaidOutPage laid(R"HTML(<!DOCTYPE html><body style="margin:0"><div id=outer style="width:200px">
<div id=c style="display:contents; border:10px solid red; padding:20px; width:500px; color:rgb(0, 128, 0)"><p id=p style="margin:0; height:30px">text</p></div>
<span id=s style="display:contents; border:5px solid red">inline text</span>
<img id=i style="display:contents" width=10 height=10><button id=b style="display:contents">label</button>
</div></body>)HTML");
    Page& page = *laid.page;
    CHECK(page.boolean("CSS.supports('display', 'contents') && CSS.supports('display: contents') && CSS.supports('(display: contents)')"));
    CHECK_EQ(page.string("getComputedStyle(document.getElementById('c')).display"), "contents");
    CHECK_EQ(page.string("getComputedStyle(document.getElementById('p')).color"), "rgb(0, 128, 0)");
    CHECK_EQ(page.string("(function () { var r = document.getElementById('c').getBoundingClientRect(); return [r.x, r.y, r.width, r.height].join(); })()"), "0,0,0,0");
    CHECK_EQ(page.string("(function () { var c = document.getElementById('c'); return [c.getClientRects().length, c.offsetWidth, c.offsetHeight, c.offsetParent === null, c.checkVisibility()].join(); })()"), "0,0,0,true,false");
    CHECK_EQ(page.string("(function () { var r = document.getElementById('p').getBoundingClientRect(); return [r.x, r.y, r.width, r.height].join(); })()"), "0,0,200,30");
    CHECK(page.boolean("document.getElementById('p').offsetParent === document.body"));
    CHECK_EQ(page.string("(function () { var s = document.getElementById('s'); return [s.getClientRects().length, s.offsetWidth, s.offsetParent === null].join(); })()"), "0,0,true");
    CHECK_EQ(page.string("getComputedStyle(document.getElementById('i')).display"), "none");
    CHECK_EQ(page.string("getComputedStyle(document.getElementById('b')).display"), "contents");
    CHECK(page.boolean("document.getElementById('p').parentNode === document.getElementById('c') && document.getElementById('c').parentNode.id === 'outer'"));
    CHECK_EQ(page.console, "");

    // On the root it computes to block (§2.8).
    LaidOutPage root(R"HTML(<!DOCTYPE html><html style="display:contents"><body></body></html>)HTML");
    CHECK_EQ(root.page->string("getComputedStyle(document.documentElement).display"), "block");
}

void test_form_controls_without_a_host()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><form id=f action="/go" method=POST><input id=t name=t value=init><input id=c type=checkbox name=c checked>
<select id=sel name=s><option value=a>A</option><option value=b selected>B</option><option>C</option></select>
<textarea id=ta name=ta>default text</textarea><button id=btn>Go</button></form>)HTML");
    // (Named access on the window — a bare `t` for the element with that
    // id — is not written; the ids are looked up.)
    page->eval("var f = document.getElementById('f'), t = document.getElementById('t'), c = document.getElementById('c'), sel = document.getElementById('sel'), ta = document.getElementById('ta'), btn = document.getElementById('btn');");
    CHECK_EQ(page->string("t.value + '|' + t.defaultValue + '|' + t.type + '|' + t.name"), "init|init|text|t");
    page->eval("t.value = 'typed';");
    CHECK_EQ(page->string("t.value + '|' + t.getAttribute('value')"), "typed|init");
    CHECK(page->boolean("c.checked === true && c.defaultChecked === true && c.value === 'on'"));
    page->eval("c.checked = false;");
    CHECK(page->boolean("c.checked === false && c.hasAttribute('checked')"));
    CHECK_EQ(page->string("sel.value + sel.selectedIndex + sel.options.length + sel.length + sel.options[2].value + sel.options[2].text + sel.type"), "b133CCselect-one");
    page->eval("sel.selectedIndex = 2;");
    CHECK_EQ(page->string("sel.value + '|' + sel.options[2].selected + '|' + sel.options[1].selected + '|' + sel.selectedOptions.length"), "C|true|false|1");
    page->eval("sel.value = 'a';");
    CHECK_EQ(page->number("sel.selectedIndex"), 0);
    CHECK_EQ(page->string("ta.value + '|' + ta.defaultValue + '|' + ta.textLength"), "default text|default text|12");
    page->eval("ta.value = 'new';");
    CHECK_EQ(page->string("ta.value + '|' + ta.textContent"), "new|default text");
    CHECK_EQ(page->string("f.method + '|' + f.action + '|' + f.elements.length + '|' + f.length + '|' + btn.type + '|' + (btn.form === f) + '|' + (t.form === f)"), "post|https://example.test/go|5|5|submit|true|true");
    CHECK(page->boolean("t.labels.length === 0 && t.validity.valid && t.checkValidity() && f.checkValidity()"));
    page->eval("var submitted = false; f.addEventListener('submit', function (e) { submitted = true; e.preventDefault(); }); f.requestSubmit();");
    CHECK(page->boolean("submitted"));
    page->eval("f.reset();");
    CHECK_EQ(page->string("t.value + '|' + c.checked + '|' + sel.value + '|' + ta.value"), "init|true|b|default text");
    CHECK(page->boolean("t.tabIndex === 0 && document.body.tabIndex === -1 && document.activeElement === document.body"));
    page->eval("var focus_log = []; t.addEventListener('focus', function () { focus_log.push('focus'); }); t.addEventListener('blur', function () { focus_log.push('blur'); }); t.focus();");
    CHECK(page->boolean("document.activeElement === t && focus_log.join() === 'focus'"));
    page->eval("t.blur();");
    CHECK(page->boolean("document.activeElement === document.body && focus_log.join() === 'focus,blur'"));
    CHECK_EQ(page->console, "");
}

void test_dom_parser_and_foreign_documents()
{
    auto page = loaded("<!DOCTYPE html><body></body>");
    page->eval("var parsed = new DOMParser().parseFromString('<p id=q>alpha <b>beta</b></p>', 'text/html');");
    CHECK_EQ(page->string("parsed.body.firstChild.textContent + '|' + parsed.readyState + '|' + parsed.URL"), "alpha beta|complete|about:blank");
    CHECK(page->boolean("parsed !== document && parsed.getElementById('q').ownerDocument === parsed && parsed.defaultView === null"));
    page->eval("var moved = document.body.appendChild(parsed.body.firstChild);");
    CHECK(page->boolean("moved.ownerDocument === document && moved.isConnected && parsed.body.childNodes.length === 0 && document.getElementById('q') === moved"));
    page->eval("var created = document.implementation.createHTMLDocument('T');");
    CHECK_EQ(page->string("created.title + '|' + created.body.tagName + '|' + created.documentElement.tagName"), "T|BODY|HTML");
    page->eval("var imported = document.importNode(created.body, true); var junk = []; for (var i = 0; i < 30; i++) junk.push('' + i);");
    CHECK(page->boolean("imported.ownerDocument === document && imported !== created.body && created.body.parentNode !== null"));
    CHECK_EQ(page->string("document.title"), "");
    page->eval("document.title = 'New Title';");
    CHECK_EQ(page->string("document.title + '|' + document.head.firstChild.tagName"), "New Title|TITLE");
    CHECK_EQ(page->string("document.compatMode + '|' + document.characterSet + '|' + document.contentType + '|' + document.doctype.name + '|' + document.documentURI"), "CSS1Compat|UTF-8|text/html|html|https://example.test/dir/page.html");
    CHECK_EQ(page->console, "");
}

void test_binary_data()
{
    auto page = loaded("<!DOCTYPE html><body></body>");
    // TextEncoder: UTF-8 out, a lone surrogate replaced, encodeInto's counts.
    CHECK_EQ(page->string("Array.from(new TextEncoder().encode('h\\u00e9\\ud83d\\ude00')).join()"), "104,195,169,240,159,152,128");
    CHECK_EQ(page->string("Array.from(new TextEncoder().encode('\\ud800x')).join()"), "239,191,189,120");
    CHECK(page->boolean("new TextEncoder().encoding === 'utf-8' && new TextEncoder().encode().length === 0 && new TextEncoder().encode(undefined).length === 0 && new TextEncoder().encode(12).length === 2 && new TextEncoder().encode('a') instanceof Uint8Array"));
    CHECK_EQ(page->string("(function () { var out = new Uint8Array(4); var r = new TextEncoder().encodeInto('a\\u00e9\\u20ac', out); return r.read + ':' + r.written + ':' + Array.from(out).join(); })()"), "2:3:97,195,169,0");
    // TextDecoder: labels, the BOM, streaming across chunks, fatal mode.
    CHECK_EQ(page->string("new TextDecoder().decode(new Uint8Array([104, 195, 169, 240, 159, 152, 128]))"), "h\xc3\xa9\xf0\x9f\x98\x80");
    CHECK_EQ(page->string("new TextDecoder().decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41]))"), "A");
    CHECK_EQ(page->number("new TextDecoder('utf-8', { ignoreBOM: true }).decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41])).length"), 2);
    CHECK_EQ(page->string("new TextDecoder().decode(new Uint8Array([0xC3, 0x28, 0xE2, 0x82]))"), "\xef\xbf\xbd(\xef\xbf\xbd");
    CHECK_EQ(page->string("(function () { var d = new TextDecoder(); return d.decode(new Uint8Array([0xE2, 0x82]), { stream: true }) + '|' + d.decode(new Uint8Array([0xAC]), { stream: true }) + '|' + d.decode(); })()"), "|\xe2\x82\xac|");
    CHECK_EQ(page->string("(function () { var d = new TextDecoder(); return d.decode(new Uint8Array([0xE2, 0x82]), { stream: true }) + '|' + d.decode(); })()"), "|\xef\xbf\xbd");
    CHECK(page->throws("new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xFF]))").starts_with("TypeError"));
    CHECK(page->throws("new TextDecoder('bogus')").starts_with("RangeError"));
    CHECK(page->throws("new TextDecoder().decode('abc')").starts_with("TypeError"));
    CHECK(page->throws("TextDecoder.prototype.decode.call({}, new Uint8Array(1))").starts_with("TypeError"));
    CHECK_EQ(page->string("new TextDecoder('utf-16le').decode(new Uint8Array([0x3D, 0xD8, 0x00, 0xDE, 0x41, 0x00]))"), "\xf0\x9f\x98\x80" "A");
    CHECK_EQ(page->string("new TextDecoder('latin1').decode(new Uint8Array([0x80, 0xE9])) + '|' + new TextDecoder('latin1').encoding + '|' + new TextDecoder('UTF8').encoding"), "\xe2\x82\xac\xc3\xa9|windows-1252|utf-8");
    CHECK(page->boolean("(function () { var d = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }); return d.fatal && d.ignoreBOM && d.encoding === 'utf-8' && new TextDecoder().decode() === '' && new TextDecoder().decode(new ArrayBuffer(0)) === '' && new TextDecoder().decode(new DataView(new Uint8Array([0x41]).buffer)) === 'A'; })()"));
    // Blob and File: parts of every kind, the type normalized, slices,
    // and the three readers through promises.
    CHECK(page->boolean("(function () { var b = new Blob(['ab', new Uint8Array([99]), new Blob(['d'])], { type: 'Text/Plain' }); return b.size === 4 && b.type === 'text/plain' && b.slice(1, 3).size === 2 && b.slice(-1, undefined, 'X').type === 'x' && new Blob().size === 0 && new Blob([], { type: 'a\\u00e9' }).type === '' && Object.prototype.toString.call(b) === '[object Blob]'; })()"));
    page->eval("var texts = []; var b = new Blob(['h\\u00e9', new Uint8Array([33])]); b.text().then(function (t) { texts.push(t); }); b.slice(0, 1).text().then(function (t) { texts.push(t); }); b.arrayBuffer().then(function (ab) { texts.push(ab.byteLength + ':' + new Uint8Array(ab).join()); }); b.bytes().then(function (u) { texts.push(u.constructor.name + u.length); });");
    CHECK_EQ(page->string("texts.join('|')"), "h\xc3\xa9!|h|4:104,195,169,33|Uint8Array4");
    CHECK(page->boolean("(function () { var f = new File(['x'], 'a.txt', { type: 'text/plain', lastModified: 5 }); return f instanceof Blob && f instanceof File && f.name === 'a.txt' && f.lastModified === 5 && f.size === 1 && f.type === 'text/plain' && Object.prototype.toString.call(f) === '[object File]' && typeof new File([], 'b').lastModified === 'number' && f.webkitRelativePath === ''; })()"));
    CHECK(page->throws("new File(['x'])").starts_with("TypeError"));
    CHECK(page->throws("new Blob(5)").starts_with("TypeError"));
    CHECK(page->throws("Blob.prototype.slice.call({})").starts_with("TypeError"));
    // crypto.getRandomValues fills integer views in place and refuses the rest.
    CHECK(page->boolean("(function () { var a = new Uint8Array(64); var r = crypto.getRandomValues(a); var sum = 0; for (var i = 0; i < a.length; i++) sum += a[i]; var b = new Int32Array(2); crypto.getRandomValues(b); var c = new Uint8Array(new ArrayBuffer(8), 4, 2); crypto.getRandomValues(c); return r === a && sum > 0 && (b[0] !== 0 || b[1] !== 0) && c.length === 2; })()"));
    CHECK(page->throws("crypto.getRandomValues(new Float32Array(1))").starts_with("TypeMismatchError"));
    CHECK(page->throws("crypto.getRandomValues(new DataView(new ArrayBuffer(1)))").starts_with("TypeMismatchError"));
    CHECK(page->throws("crypto.getRandomValues(new Uint8Array(65537))").starts_with("QuotaExceededError"));
    CHECK(page->throws("crypto.getRandomValues([1])").starts_with("TypeError"));
    CHECK_EQ(page->console, "");
}

bool sent_header(Page const& page, std::string_view header)
{
    return std::find(page.request_headers.begin(), page.request_headers.end(), header) != page.request_headers.end();
}

void test_fetch_and_xhr()
{
    auto page = loaded("<!DOCTYPE html><form id=f><input name=a value=1><input name=b type=checkbox checked><input name=c type=checkbox>"
                       "<textarea name=t>tt</textarea><input name=d disabled value=no><input type=submit name=go value=Go></form>");
    net::FetchResponse json;
    json.status = 200;
    json.status_text = "OK";
    json.headers.push_back({ "Content-Type", "application/json" });
    json.headers.push_back({ "X-Custom", "yes" });
    json.headers.push_back({ "Set-Cookie", "a=b" });
    std::string const json_body = "{\"a\":1}";
    json.body.assign(json_body.begin(), json_body.end());
    page->responses["https://example.test/api"] = json;
    page->responses["https://example.test/post"] = json;
    page->responses["https://other.test/plain"] = json; // no Access-Control-Allow-Origin
    net::FetchResponse allowed = json;
    allowed.headers.push_back({ "Access-Control-Allow-Origin", "*" });
    allowed.headers.push_back({ "Access-Control-Allow-Methods", "PUT" });
    allowed.headers.push_back({ "Access-Control-Allow-Headers", "x-custom" });
    page->responses["https://other.test/open"] = allowed;
    net::FetchResponse exposed = allowed;
    exposed.headers.push_back({ "Access-Control-Expose-Headers", "X-Custom" });
    page->responses["https://other.test/exposed"] = exposed;
    net::FetchResponse redirect;
    redirect.status = 302;
    redirect.status_text = "Found";
    redirect.headers.push_back({ "Location", "/api" });
    page->responses["https://example.test/r"] = redirect;

    // fetch: a same-origin JSON response, delivered on the next task; the
    // forbidden Set-Cookie never reaches the page.
    page->eval("var out = []; fetch('/api').then(function (r) { out.push(r.status + ':' + r.ok + ':' + r.type + ':' + r.url + ':' + r.headers.get('content-type') + ':' + r.headers.get('x-custom') + ':' + r.headers.get('set-cookie') + ':' + r.bodyUsed); return r.json(); }).then(function (j) { out.push(j.a); });");
    CHECK_EQ(page->number("out.length"), 0);
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join('|')"), "200:true:basic:https://example.test/api:application/json:yes:null:false|1");
    CHECK_EQ(page->requests.back(), "GET https://example.test/api");
    CHECK(sent_header(*page, "Accept: */*"));
    // A POST: the method, the body, its Content-Type and the Origin reach
    // the loader; a forbidden request header does not.
    page->requests.clear();
    page->request_headers.clear();
    page->eval("out = []; fetch('/post', { method: 'post', headers: { 'X-Token': 'abc', cookie: 'no' }, body: 'hello' }).then(function (r) { out.push(r.status); });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join()"), "200");
    CHECK_EQ(page->requests.back(), "POST https://example.test/post hello");
    CHECK(sent_header(*page, "x-token: abc"));
    CHECK(sent_header(*page, "content-type: text/plain;charset=UTF-8"));
    CHECK(sent_header(*page, "Origin: https://example.test"));
    CHECK(!sent_header(*page, "cookie: no"));
    // Cross-origin: no allow-origin is a network error; a star is a cors
    // response showing the safelisted headers only, unless exposed.
    page->eval("out = []; fetch('https://other.test/plain').then(function () { out.push('ok'); }, function (e) { out.push(e.name + ':' + e.message); });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join()"), "TypeError:Failed to fetch");
    page->eval("out = []; fetch('https://other.test/open').then(function (r) { out.push(r.type + ':' + r.headers.get('content-type') + ':' + r.headers.get('x-custom')); }); fetch('https://other.test/exposed').then(function (r) { out.push(r.headers.get('x-custom')); });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join('|')"), "cors:application/json:null|yes");
    page->eval("out = []; fetch('https://other.test/plain', { mode: 'no-cors' }).then(function (r) { out.push(r.type + ':' + r.status + ':' + r.ok + ':' + r.headers.get('content-type') + ':' + r.url); return r.text(); }).then(function (t) { out.push('[' + t + ']'); });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join('|')"), "opaque:0:false:null:|[]");
    page->requests.clear();
    page->eval("out = []; fetch('https://other.test/open', { mode: 'same-origin' }).catch(function (e) { out.push(e.name); }); fetch('https://other.test/open', { credentials: 'include' }).catch(function (e) { out.push(e.name); });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join()"), "TypeError,TypeError");
    CHECK_EQ(page->requests.size(), 1u); // the same-origin refusal made no request; the credentialed one did and failed the check
    // A preflight goes first for a PUT with a custom header.
    page->requests.clear();
    page->eval("out = []; fetch('https://other.test/open', { method: 'PUT', headers: { 'X-Custom': '1' }, body: 'b' }).then(function (r) { out.push(r.status); });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join()"), "200");
    CHECK_EQ(page->requests.size(), 2u);
    if (page->requests.size() == 2) {
        CHECK_EQ(page->requests[0], "OPTIONS https://other.test/open (no credentials)");
        CHECK_EQ(page->requests[1], "PUT https://other.test/open b (no credentials)");
    }
    CHECK(sent_header(*page, "Access-Control-Request-Method: PUT"));
    CHECK(sent_header(*page, "Access-Control-Request-Headers: x-custom"));
    // The redirect modes.
    page->eval("out = []; fetch('/r', { redirect: 'manual' }).then(function (r) { out.push(r.type + ':' + r.status); }); fetch('/r', { redirect: 'error' }).catch(function (e) { out.push(e.name); }); fetch('/r').then(function (r) { out.push(r.status); });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join('|')"), "opaqueredirect:0|TypeError|302");

    // Headers.
    CHECK(page->boolean("(function () { var h = new Headers({ 'Content-Type': 'text/plain', 'X-B': '1' }); h.append('x-b', '2'); h.append('Set-Cookie', 'a=1'); h.append('set-cookie', 'b=2'); return h.get('content-type') === 'text/plain' && h.get('X-B') === '1, 2' && h.has('x-b') && !h.has('nope') && h.get('nope') === null && h.getSetCookie().join('|') === 'a=1|b=2' && [...h.keys()].join() === 'content-type,set-cookie,set-cookie,x-b' && [...h].map(function (p) { return p.join('='); }).join('|') === 'content-type=text/plain|set-cookie=a=1|set-cookie=b=2|x-b=1, 2'; })()"));
    CHECK(page->boolean("(function () { var h = new Headers([['a', '1'], ['A', '2']]); h.set('a', '3'); h.delete('zzz'); var seen = []; h.forEach(function (v, k) { seen.push(k + '=' + v); }); return h.get('a') === '3' && seen.join() === 'a=3' && new Headers(h).get('a') === '3' && new Headers().has('a') === false && Object.prototype.toString.call(h) === '[object Headers]'; })()"));
    CHECK(page->throws("new Headers({ 'bad name': '1' })").starts_with("TypeError"));
    CHECK(page->throws("new Headers({ ok: 'bad\\nvalue' })").starts_with("TypeError"));
    CHECK(page->throws("new Headers([['only-one']])").starts_with("TypeError"));
    CHECK(page->boolean("(function () { var r = new Request('/x', { headers: { Cookie: 'a=b', Accept: 'text/plain', 'Proxy-A': '1' } }); return r.headers.get('cookie') === null && r.headers.get('accept') === 'text/plain' && r.headers.get('proxy-a') === null; })()"));
    page->eval("out = []; fetch('/api').then(function (r) { try { r.headers.set('a', 'b'); out.push('set'); } catch (e) { out.push(e.name); } });");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join()"), "TypeError");

    // Request and Response.
    CHECK(page->boolean("(function () { var r = new Request('/x?q=1#frag', { method: 'post', body: 'b' }); return r.url === 'https://example.test/x?q=1' && r.method === 'POST' && r.mode === 'cors' && r.credentials === 'same-origin' && r.cache === 'default' && r.redirect === 'follow' && r.referrer === 'about:client' && r.headers.get('content-type') === 'text/plain;charset=UTF-8' && r.signal instanceof AbortSignal && !r.signal.aborted && r.bodyUsed === false && r.body === null && r.destination === '' && r.keepalive === false; })()"));
    CHECK(page->boolean("(function () { var a = new Request('https://x.test/p', { method: 'PUT', body: 'z', mode: 'same-origin', credentials: 'include', redirect: 'manual' }); var b = new Request(a); var c = a.clone(); return b.url === a.url && b.method === 'PUT' && b.mode === 'same-origin' && b.credentials === 'include' && b.redirect === 'manual' && c.method === 'PUT' && new Request(a, { method: 'DELETE' }).method === 'DELETE'; })()"));
    CHECK(page->throws("new Request('/x', { method: 'get', body: 'x' })").starts_with("TypeError"));
    CHECK(page->throws("new Request('/x', { method: 'TRACE' })").starts_with("TypeError"));
    CHECK(page->throws("new Request('/x', { mode: 'navigate' })").starts_with("TypeError"));
    CHECK(page->throws("new Request('http://user:pw@x.test/')").starts_with("TypeError"));
    CHECK(page->throws("new Request('https://')").starts_with("TypeError"));
    CHECK(page->throws("new Request()").starts_with("TypeError"));
    page->eval("out = []; var res = new Response('hi', { status: 201, statusText: 'Created', headers: { a: 'b' } }); out.push(res.status + ':' + res.ok + ':' + res.statusText + ':' + res.headers.get('a') + ':' + res.type + ':' + res.url + ':' + res.redirected + ':' + res.headers.get('content-type')); res.text().then(function (t) { out.push(t + ':' + res.bodyUsed); return res.text(); }).catch(function (e) { out.push(e.name); });");
    CHECK_EQ(page->string("out.join('|')"), "201:true:Created:b:default::false:text/plain;charset=UTF-8|hi:true|TypeError");
    CHECK(page->throws("new Response('x', { status: 204 })").starts_with("TypeError"));
    CHECK(page->throws("new Response('x', { status: 199 })").starts_with("RangeError"));
    CHECK(page->boolean("Response.error().type === 'error' && Response.error().status === 0 && Response.redirect('/y', 301).status === 301 && Response.redirect('/y').headers.get('location') === 'https://example.test/y' && Response.redirect('/y').status === 302"));
    CHECK(page->throws("Response.redirect('/y', 200)").starts_with("RangeError"));
    page->eval("out = []; var rj = Response.json({ a: [1, 2] }, { status: 202 }); out.push(rj.status + ':' + rj.headers.get('content-type')); rj.json().then(function (j) { out.push(j.a.join()); }); new Response('a=1&b=2', { headers: { 'content-type': 'application/x-www-form-urlencoded' } }).formData().then(function (f) { out.push(f.get('a') + f.get('b')); }); new Response(new Uint8Array([104, 105])).arrayBuffer().then(function (b) { out.push(b.byteLength); }); new Response('blob', { headers: { 'content-type': 'Text/X' } }).blob().then(function (b) { out.push(b.size + b.type); }); new Response('by').bytes().then(function (u) { out.push(u.length + u.constructor.name); }); new Response('{').json().catch(function (e) { out.push(e.name); });");
    CHECK_EQ(page->string("out.join('|')"), "202:application/json|1,2|12|2|4text/x|2Uint8Array|SyntaxError");

    // FormData: by hand and from a form; every body kind through fetch.
    CHECK(page->boolean("(function () { var fd = new FormData(); fd.append('a', '1'); fd.append('a', '2'); fd.append('b', new Blob(['xy'], { type: 'text/plain' }), 'f.txt'); fd.set('c', 3); var f = fd.get('b'); return fd.get('a') === '1' && fd.getAll('a').join() === '1,2' && fd.has('c') && fd.get('c') === '3' && f instanceof File && f.name === 'f.txt' && f.size === 2 && [...fd.keys()].join() === 'a,a,b,c' && (fd.delete('a'), !fd.has('a')) && [...fd].length === 2; })()"));
    CHECK_EQ(page->string("[...new FormData(document.getElementById('f'))].map(function (p) { return p.join('='); }).join('|')"), "a=1|b=on|t=tt");
    CHECK(page->throws("new FormData(document.body)").starts_with("TypeError"));
    page->requests.clear();
    page->request_headers.clear();
    page->eval("var fd2 = new FormData(); fd2.append('k', 'v'); fd2.append('file', new Blob(['zz'], { type: 'text/plain' }), 'z.txt'); fetch('/post', { method: 'POST', body: fd2 }); fetch('/post', { method: 'POST', body: new URLSearchParams('q=a b&r=1') }); fetch('/post', { method: 'POST', body: new Blob(['bl'], { type: 'application/x-bl' }) }); fetch('/post', { method: 'POST', body: new Uint8Array([65, 66]) });");
    page->realm->run_pending();
    CHECK_EQ(page->requests.size(), 4u);
    if (page->requests.size() == 4) {
        CHECK(page->requests[0].find("Content-Disposition: form-data; name=\"k\"\r\n\r\nv\r\n") != std::string::npos);
        CHECK(page->requests[0].find("name=\"file\"; filename=\"z.txt\"\r\nContent-Type: text/plain\r\n\r\nzz\r\n") != std::string::npos);
        CHECK_EQ(page->requests[1], "POST https://example.test/post q=a+b&r=1");
        CHECK_EQ(page->requests[2], "POST https://example.test/post bl");
        CHECK_EQ(page->requests[3], "POST https://example.test/post AB");
    }
    CHECK(std::any_of(page->request_headers.begin(), page->request_headers.end(), [](std::string const& h) { return h.starts_with("content-type: multipart/form-data; boundary=----SashfoldFormBoundary"); }));
    if (!sent_header(*page, "content-type: application/x-www-form-urlencoded;charset=UTF-8")) {
        std::string all;
        for (std::string const& header : page->request_headers)
            all += header + " ; ";
        test::fail("urlencoded content-type not sent; headers were: " + all, __FILE__, __LINE__);
    }
    CHECK(sent_header(*page, "content-type: application/x-bl"));

    // AbortController: the event, the reason, a fetch rejected on the
    // next task, an already-aborted signal rejected with no request made.
    page->eval("out = []; var ac = new AbortController(); ac.signal.addEventListener('abort', function (e) { out.push('event:' + e.type + ':' + ac.signal.aborted); }); fetch('/api', { signal: ac.signal }).catch(function (e) { out.push(e.name + ':' + (e === ac.signal.reason)); }); ac.abort();");
    CHECK_EQ(page->string("out.join('|')"), "event:abort:true");
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join('|')"), "event:abort:true|AbortError:true");
    CHECK(page->boolean("(function () { var s = AbortSignal.abort('why'); var c2 = new AbortController(); c2.abort(); var ok = false; try { s.throwIfAborted(); } catch (e) { ok = e === 'why'; } return s.aborted && s.reason === 'why' && ok && c2.signal.reason.name === 'AbortError' && c2.signal.reason instanceof DOMException && new AbortController().signal.aborted === false; })()"));
    page->requests.clear();
    page->eval("out = []; fetch('/api', { signal: AbortSignal.abort() }).catch(function (e) { out.push(e.name); });");
    CHECK_EQ(page->string("out.join()"), "AbortError");
    CHECK(page->requests.empty());
    page->eval("out = []; var ts = AbortSignal.timeout(50); ts.onabort = function () { out.push(ts.reason.name); };");
    page->clock += 100;
    page->realm->run_pending();
    CHECK_EQ(page->string("out.join()"), "TimeoutError");

    // XMLHttpRequest: the states and events of a GET, the headers combined
    // and the forbidden one dropped, the response headers read back.
    page->requests.clear();
    page->request_headers.clear();
    page->eval("var log = []; var x = new XMLHttpRequest(); x.onreadystatechange = function () { log.push('rs' + x.readyState); }; x.addEventListener('loadstart', function () { log.push('start'); }); x.onprogress = function (e) { log.push('progress:' + e.loaded + '/' + e.total + ':' + e.lengthComputable); }; x.onload = function () { log.push('load:' + x.status + ':' + x.statusText + ':' + x.responseText + ':' + x.getResponseHeader('Content-Type') + ':' + x.getResponseHeader('nope') + ':' + x.responseURL); }; x.onloadend = function () { log.push('end'); }; x.open('GET', '/api'); x.setRequestHeader('X-A', '1'); x.setRequestHeader('x-a', '2'); x.setRequestHeader('Cookie', 'no'); log.push('sent:' + x.readyState); x.send();");
    CHECK_EQ(page->string("log.join('|')"), "rs1|sent:1|start");
    page->realm->run_pending();
    CHECK_EQ(page->string("log.join('|')"), "rs1|sent:1|start|rs2|rs3|progress:7/7:true|rs4|load:200:OK:{\"a\":1}:application/json:null:https://example.test/api|end");
    CHECK_EQ(page->requests.back(), "GET https://example.test/api");
    CHECK(sent_header(*page, "x-a: 1, 2"));
    CHECK(!sent_header(*page, "cookie: no"));
    CHECK(page->boolean("x.getAllResponseHeaders() === 'content-type: application/json\\r\\nx-custom: yes\\r\\n' && x.readyState === 4 && XMLHttpRequest.DONE === 4 && x.DONE === 4 && x.UNSENT === 0 && x.upload instanceof XMLHttpRequestUpload && x instanceof XMLHttpRequestEventTarget && x.responseXML === null"));
    CHECK(page->boolean("(function () { var s = new XMLHttpRequest(); s.open('GET', '/api', false); s.send(); return s.readyState === 4 && s.status === 200 && s.responseText === '{\"a\":1}'; })()"));
    page->eval("log = []; var j = new XMLHttpRequest(); j.open('GET', '/api'); j.responseType = 'json'; j.onload = function () { log.push(j.response.a + ':' + (j.response === j.response)); }; j.send(); var ab = new XMLHttpRequest(); ab.open('GET', '/api'); ab.responseType = 'arraybuffer'; ab.onload = function () { log.push(ab.response.byteLength + ':' + (ab.response instanceof ArrayBuffer)); }; ab.send(); var bl = new XMLHttpRequest(); bl.open('GET', '/api'); bl.responseType = 'blob'; bl.onload = function () { log.push(bl.response.size + ':' + bl.response.type); }; bl.send();");
    page->realm->run_pending();
    CHECK_EQ(page->string("log.join('|')"), "1:true|7:true|7:application/json");
    CHECK(page->throws("(function () { var t = new XMLHttpRequest(); t.open('GET', '/api'); t.responseType = 'json'; return t.responseText; })()").starts_with("InvalidStateError"));
    // A network error and an abort.
    page->eval("log = []; var bad = new XMLHttpRequest(); bad.onerror = function () { log.push('error:' + bad.status + ':' + bad.readyState); }; bad.onloadend = function () { log.push('end'); }; bad.onload = function () { log.push('load'); }; bad.open('GET', '/missing'); bad.send(); var ab2 = new XMLHttpRequest(); ab2.onabort = function () { log.push('abort:' + ab2.readyState); }; ab2.open('GET', '/api'); ab2.send(); ab2.abort(); log.push('after:' + ab2.readyState);");
    CHECK_EQ(page->string("log.join('|')"), "abort:4|after:0");
    page->realm->run_pending();
    CHECK_EQ(page->string("log.join('|')"), "abort:4|after:0|error:0:4|end");
    // Bodies and the method rules; the errors the states impose.
    page->requests.clear();
    page->request_headers.clear();
    page->eval("var p = new XMLHttpRequest(); p.open('POST', '/post'); p.send(new URLSearchParams('a=1&b=2')); var q = new XMLHttpRequest(); q.open('POST', '/post'); q.setRequestHeader('Content-Type', 'text/x'); q.send('body'); var hd = new XMLHttpRequest(); hd.open('HEAD', '/api'); hd.send('ignored'); var fdx = new XMLHttpRequest(); fdx.open('POST', '/post'); var fd3 = new FormData(); fd3.append('m', '1'); fdx.send(fd3);");
    page->realm->run_pending();
    CHECK_EQ(page->requests.size(), 4u);
    if (page->requests.size() == 4) {
        CHECK_EQ(page->requests[0], "POST https://example.test/post a=1&b=2");
        CHECK_EQ(page->requests[1], "POST https://example.test/post body");
        CHECK_EQ(page->requests[2], "HEAD https://example.test/api");
        CHECK(page->requests[3].find("name=\"m\"\r\n\r\n1\r\n") != std::string::npos);
    }
    CHECK(sent_header(*page, "content-type: application/x-www-form-urlencoded;charset=UTF-8"));
    CHECK(sent_header(*page, "content-type: text/x"));
    CHECK(page->throws("(function () { var u = new XMLHttpRequest(); u.setRequestHeader('a', 'b'); })()").starts_with("InvalidStateError"));
    CHECK(page->throws("(function () { var u = new XMLHttpRequest(); u.open('TRACE', '/x'); })()").starts_with("SecurityError"));
    CHECK(page->throws("(function () { var u = new XMLHttpRequest(); u.open('GET', 'https://'); })()").starts_with("SyntaxError"));
    CHECK(page->throws("(function () { var u = new XMLHttpRequest(); u.open('GET', '/x'); u.send(); u.send(); })()").starts_with("InvalidStateError"));
    CHECK(page->throws("(function () { var u = new XMLHttpRequest(); u.open('GET', '/x', false); u.responseType = 'json'; })()").starts_with("InvalidAccessError"));
    page->requests.clear();
    page->eval("var w = new XMLHttpRequest(); w.open('GET', 'https://other.test/open'); w.withCredentials = true; w.send();");
    page->realm->run_pending();
    CHECK_EQ(page->requests.back(), "GET https://other.test/open");

    // MessageChannel and window.postMessage: delivered as tasks, in order.
    page->eval("log = []; var ch = new MessageChannel(); ch.port1.onmessage = function (e) { log.push(e.data + ':' + (e instanceof MessageEvent) + ':' + e.origin + ':' + e.type); }; ch.port2.postMessage('hi'); ch.port2.postMessage({ n: 2 }); log.push('posted'); window.addEventListener('message', function (e) { log.push('win:' + e.data + ':' + e.origin + ':' + (e.source === window)); }); postMessage('x', '*'); window.postMessage('y', 'https://nope.test'); log.push('sync');");
    CHECK_EQ(page->string("log.join('|')"), "posted|sync");
    page->realm->run_pending();
    CHECK_EQ(page->string("log.join('|')"), "posted|sync|hi:true:https://example.test:message|[object Object]:true:https://example.test:message|win:x:https://example.test:true");
    page->eval("log = []; var ch2 = new MessageChannel(); ch2.port1.postMessage('early'); ch2.port2.addEventListener('message', function (e) { log.push(e.data); }); ch2.port2.start(); var ch3 = new MessageChannel(); ch3.port2.onmessage = function () { log.push('never'); }; ch3.port2.close(); ch3.port1.postMessage('lost');");
    page->realm->run_pending();
    CHECK_EQ(page->string("log.join('|')"), "early");
    CHECK(page->boolean("(function () { var pe = new ProgressEvent('progress', { lengthComputable: true, loaded: 5, total: 10 }); var me = new MessageEvent('message', { data: 7, origin: 'o' }); return pe.lengthComputable && pe.loaded === 5 && pe.total === 10 && me.data === 7 && me.origin === 'o' && me.source === null && me.ports.length === 0 && me.lastEventId === ''; })()"));
    CHECK(page->console.find("error:") == std::string::npos);
}

void test_content_security_policy()
{
    // The document's policy as the realm applies it: an inline script
    // without its nonce or hash is refused and the next one runs; a
    // handler attribute needs 'unsafe-hashes'; eval, Function and a
    // timer's string need 'unsafe-eval'; an external script's URL is
    // judged with its nonce; fetch() is connect-src's.
    std::vector<std::string> violations;
    std::string const page_url = "https://example.test/dir/page.html";
    net::ContentSecurityPolicy policy(*net::parse_url(page_url));
    policy.set_reporter([&violations](std::string_view message) { violations.emplace_back(message); });
    policy.add_header("script-src 'self' 'nonce-n1' '" + net::ContentSecurityPolicy::sha256_source("log.push('hashed');")
            + "'; connect-src 'self'",
        false);
    bindings::HostHooks hooks;
    hooks.policy = &policy;
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html><head>
<script>var log = ['refused'];</script>
<script nonce="n1">var log = ['nonced'];</script>
<script>log.push('hashed');</script>
<script nonce="n1">
  try { eval('1'); log.push('eval-ran'); } catch (e) { log.push('eval:' + e.name + ':' + (e.message.indexOf("'unsafe-eval'") >= 0)); }
  try { new Function('return 1'); log.push('function-ran'); } catch (e) { log.push('function:' + e.name); }
  log.push('timer:' + setTimeout('log.push("timer-ran")', 0));
</script>
<script src="ok.js" nonce="zzz"></script>
<script src="https://cdn.test/lib.js"></script>
<script src="https://cdn.test/nonced.js" nonce="n1"></script>
</head><body onload="log.push('handler')"><p id="p" onclick="log.push('click')">x</p>
<script nonce="n1">fetch('https://api.test/x').catch(function () {}); fetch('/api/ok').catch(function () {});</script>
</body>)HTML",
        page_url, hooks);
    page->scripts["https://example.test/dir/ok.js"] = "log.push('ok');";
    page->scripts["https://cdn.test/lib.js"] = "log.push('lib');";
    page->scripts["https://cdn.test/nonced.js"] = "log.push('cdn-nonced');";
    page->load();
    page->realm->run_pending();
    page->eval("document.getElementById('p').dispatchEvent(new Event('click'))");
    CHECK_EQ(page->string("log.join(' ')"), "nonced hashed eval:EvalError:true function:EvalError timer:0 ok cdn-nonced");
    CHECK_EQ(page->fetched.size(), 2u);
    CHECK_EQ(page->fetched[0], "https://example.test/dir/ok.js");
    CHECK_EQ(page->fetched[1], "https://cdn.test/nonced.js");
    CHECK_EQ(page->requests.size(), 1u);
    CHECK_EQ(page->requests[0], "GET https://example.test/api/ok");
    CHECK_EQ(page->realm->stats().scripts_refused, 3); // the inline script, the two handlers
    auto const reported = [&violations](std::string_view part) {
        for (std::string const& line : violations)
            if (line.find(part) != std::string::npos)
                return true;
        return false;
    };
    CHECK(reported("Refused to execute inline script"));
    CHECK(reported("Refused to load the script 'https://cdn.test/lib.js'"));
    CHECK(reported("Refused to evaluate a string as JavaScript"));
    CHECK(reported("Refused to execute inline event handler"));
    CHECK(reported("Refused to connect to 'https://api.test/x'"));
    CHECK(page->console.find("lib.js could not be loaded") != std::string::npos);

    // A <meta> policy in the head takes effect for what follows it, and
    // under 'strict-dynamic' a script a trusted script inserted loads
    // while a parser-inserted one needs its nonce. A report-only header
    // speaks and refuses nothing.
    violations.clear();
    net::ContentSecurityPolicy meta_policy(*net::parse_url(page_url));
    meta_policy.set_reporter([&violations](std::string_view message) { violations.emplace_back(message); });
    meta_policy.add_header("script-src 'none'", true);
    bindings::HostHooks meta_hooks;
    meta_hooks.policy = &meta_policy;
    auto second = std::make_unique<Page>(R"HTML(<!DOCTYPE html><head>
<script>var log = ['before-meta'];</script>
<meta http-equiv="Content-Security-Policy" content="script-src 'nonce-m' 'strict-dynamic'">
<script>log.push('after-meta');</script>
<script nonce="m">log.push('nonced'); var s = document.createElement('script'); s.src = 'https://cdn.test/dyn.js'; document.head.appendChild(s);</script>
<script src="https://cdn.test/parser.js" nonce="m"></script>
<script src="https://cdn.test/parser2.js"></script>
</head><body></body>)HTML",
        page_url, meta_hooks);
    second->scripts["https://cdn.test/dyn.js"] = "log.push('dyn');";
    second->scripts["https://cdn.test/parser.js"] = "log.push('parser');";
    second->scripts["https://cdn.test/parser2.js"] = "log.push('parser2');";
    second->load();
    CHECK_EQ(second->string("log.join(' ')"), "before-meta nonced dyn parser");
    CHECK_EQ(second->fetched.size(), 2u);
    CHECK_EQ(meta_policy.policies().size(), 2u);
    CHECK(reported("[Report Only] Refused to execute inline script"));
    CHECK(reported("Refused to load the script 'https://cdn.test/parser2.js'"));

    // A sandbox without allow-scripts: nothing runs.
    net::ContentSecurityPolicy boxed(*net::parse_url(page_url));
    boxed.add_header("sandbox allow-forms", false);
    bindings::HostHooks boxed_hooks;
    boxed_hooks.policy = &boxed;
    auto third = std::make_unique<Page>(R"HTML(<!DOCTYPE html><script>window.ran = 1;</script><body onload="window.loaded = 1"></body>)HTML",
        page_url, boxed_hooks);
    third->load();
    CHECK_EQ(third->string("typeof ran + ':' + typeof loaded"), "undefined:undefined");
    CHECK_EQ(third->realm->stats().scripts_refused, 2);
    CHECK_EQ(third->realm->stats().scripts_run, 0);
}

void test_local_storage_areas()
{
    // localStorage is the host's area for the origin, shared by every
    // document there and counting each change; sessionStorage stays the
    // document's; another origin is another area; an opaque origin gets
    // none of the host's and keeps its own.
    std::map<std::string, bindings::StorageArea> areas;
    bindings::HostHooks hooks;
    hooks.local_storage = [&areas](std::string const& origin) { return &areas[origin]; };
    auto first = std::make_unique<Page>(
        "<script>localStorage.setItem('a', '1'); localStorage.b = '2'; sessionStorage.setItem('s', 'x');</script>",
        "https://example.test/one.html", hooks);
    first->load();
    CHECK_EQ(areas.size(), 1u);
    CHECK_EQ(areas["https://example.test"].items.size(), 2u);
    CHECK_EQ(areas["https://example.test"].changes, std::uint64_t { 2 });
    auto second = std::make_unique<Page>(
        "<script>document.title = localStorage.length + ':' + localStorage.getItem('a') + ':' + localStorage.b + ':' + localStorage.key(1) + ':' + sessionStorage.length;"
        " localStorage.removeItem('a'); delete localStorage.b; localStorage.c = '3'; localStorage.clear(); localStorage.clear();</script>",
        "https://example.test/two.html", hooks);
    second->load();
    CHECK_EQ(second->string("document.title"), "2:1:2:b:0");
    CHECK_EQ(areas["https://example.test"].items.size(), 0u);
    CHECK_EQ(areas["https://example.test"].changes, std::uint64_t { 6 }); // an empty clear counts nothing
    auto other = std::make_unique<Page>("<script>localStorage.x = 'y';</script>", "https://other.test/", hooks);
    other->load();
    CHECK_EQ(areas.size(), 2u);
    CHECK_EQ(areas["https://other.test"].items.size(), 1u);
    auto opaque = std::make_unique<Page>("<script>localStorage.q = '1'; document.title = localStorage.length;</script>", "data:text/html,x", hooks);
    opaque->load();
    CHECK_EQ(areas.size(), 2u);
    CHECK_EQ(opaque->string("document.title"), "1");
}

// A frame's document with a realm of its own in the page's agent, under the
// same heap stress as every page here.
void test_frames_have_realms_of_their_own()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<iframe id=f srcdoc="<p id=inner>hi</p><script>var childValue = 7; window.timerId = setTimeout(function () { window.ranAs = this === window; }, 5); setTimeout('stringRan = true', 5);</script>"></iframe>
<script>var parentValue = 1;</script>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    CHECK_EQ(page->string("document.getElementById('f').contentDocument.getElementById('inner').textContent"), "hi");
    CHECK(page->boolean("document.getElementById('f').contentWindow !== window"));
    // The frame's script ran in the frame's global, not the page's.
    CHECK(page->boolean("document.getElementById('f').contentWindow.childValue === 7 && typeof childValue === 'undefined'"));
    CHECK(page->boolean("document.getElementById('f').contentWindow.parent === window && document.getElementById('f').contentWindow.top === window"));
    CHECK(page->boolean("document.getElementById('f').contentWindow.frameElement === document.getElementById('f')"));
    CHECK(page->boolean("window.parent === window && window.top === window && window.frameElement === null"));
    // The frame's timers run as the frame, and the page's clearTimeout of the
    // same id leaves them be.
    page->eval("clearTimeout(document.getElementById('f').contentWindow.timerId);");
    page->clock = 2000;
    page->realm->run_pending();
    CHECK(page->boolean("document.getElementById('f').contentWindow.ranAs === true"));
    CHECK(page->boolean("document.getElementById('f').contentWindow.stringRan === true && typeof stringRan === 'undefined'"));
    // A page script writing a frame element's style moves the frame's
    // mutation count, not the page's.
    dom::Node* const frame_node = page->realm->node_of(page->eval("document.getElementById('f')").value);
    bindings::Realm* const frame_realm = frame_node && frame_node->is_element()
        ? page->realm->frame_realm(*static_cast<dom::Element*>(frame_node))
        : nullptr;
    CHECK(frame_realm != nullptr);
    if (frame_realm) {
        std::uint64_t const page_before = page->realm->mutation_count();
        std::uint64_t const frame_before = frame_realm->mutation_count();
        page->eval("document.getElementById('f').contentDocument.getElementById('inner').style.color = 'red';");
        CHECK(frame_realm->mutation_count() > frame_before);
        CHECK_EQ(page->realm->mutation_count(), page_before);
    }
    // A node of the frame's document adopted into the page's.
    CHECK(page->boolean("document.body.appendChild(document.getElementById('f').contentDocument.getElementById('inner')).parentNode === document.body"));
    CHECK_EQ(page->string("document.getElementById('inner').textContent"), "hi");
    CHECK_EQ(page->console, "");
}

// A frame's window events are its own: its load and a message it posts to
// itself arrive at the frame's window, with that window as their target and
// source, whatever realm the host is running when it delivers them, and the
// page's own handlers see neither.
void test_a_frames_window_events_are_its_own()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var pageLoads = 0; var pageMessages = 0; onload = function () { pageLoads++; }; addEventListener('message', function () { pageMessages++; });</script>
<iframe id=f srcdoc="<script>onload = function (e) { window.loadSeenOnItself = e.currentTarget === window && this === window; }; addEventListener('message', function (e) { window.messageSeenOnItself = e.source === window && e.currentTarget === window && e.data === 'to itself'; }); postMessage('to itself', '*');</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    page->realm->run_pending();
    CHECK(page->boolean("document.getElementById('f').contentWindow.loadSeenOnItself === true"));
    CHECK(page->boolean("document.getElementById('f').contentWindow.messageSeenOnItself === true"));
    CHECK(page->boolean("pageLoads === 1 && pageMessages === 0"));
    CHECK_EQ(page->console, "");
}

// A frame's realm ends before the heap its wrappers live in, and lets every
// wrapper of its documents go of its node, not only those in the tree: an
// element and a fragment its script made and kept, never inserted, go with
// the frame's document, and the heap's teardown after the page's realm ends
// reads nothing of it.
void test_a_frames_detached_nodes_go_with_it()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<iframe id=f srcdoc="<script>window.kept = document.createElement('div'); kept.id = 'kept'; window.keptFragment = document.createDocumentFragment(); keptFragment.appendChild(document.createElement('span'));</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    CHECK(page->boolean("document.getElementById('f').contentWindow.kept.id === 'kept'"));
    CHECK(page->boolean("document.getElementById('f').contentWindow.keptFragment.firstChild.localName === 'span'"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// Messages between windows: the source and the origin are the sender's, the
// window whose script called postMessage, whichever window's method it called;
// "/" means the sender's origin, as do one argument and options without a
// target origin; a target origin the receiving document does not have
// delivers nothing, and one that does not parse throws.
void test_messages_between_windows()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var fromFrame = []; addEventListener('message', function (e) { fromFrame.push([e.data, e.origin, e.source === document.getElementById('f').contentWindow]); });</script>
<iframe id=f srcdoc="<script>var fromPage = []; addEventListener('message', function (e) { fromPage.push([e.data, e.origin, e.source === parent]); }); parent.postMessage('hello parent', '*');</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    page->eval("var frameWindow = document.getElementById('f').contentWindow;"
               " frameWindow.postMessage('with a slash', '/');"
               " frameWindow.postMessage('one argument');"
               " frameWindow.postMessage('by options', { targetOrigin: 'https://example.test' });"
               " frameWindow.postMessage('lost', 'https://other.test');"
               " var malformed = (function () { try { frameWindow.postMessage('x', 'not a url'); } catch (e) { return e.name === 'SyntaxError' && e.constructor.name === 'DOMException'; } return false; })();");
    page->realm->run_pending();
    CHECK_EQ(page->string("JSON.stringify(fromFrame)"), R"([["hello parent","https://example.test",true]])");
    CHECK_EQ(page->string("JSON.stringify(frameWindow.fromPage)"),
        R"([["with a slash","https://example.test",true],["one argument","https://example.test",true],["by options","https://example.test",true]])");
    CHECK(page->boolean("malformed"));
    CHECK_EQ(page->console, "");
}

// A frame's document follows its iframe. A changed srcdoc or src reopens the
// frame after the script that changed it, with a new window and one load
// event; a removed iframe's frame closes at once — contentWindow null, its
// timers and its own frames' timers never run — and its realm ends once the
// host's entry has returned, so a script still holding its window, a node of
// its document or a token list of one reads nothing rather than freed
// memory, while a node adopted into the page before the removal lives on
// there; a frame can remove its own iframe from inside its own load; and a
// script-inserted iframe opens after the script, with a load event.
void test_a_frames_document_follows_its_iframe()
{
    std::map<std::string, std::string> documents; // by URL, for a src
    documents["https://example.test/dir/second.html"] = "<script>var which = 'second';</script>";
    bindings::HostHooks hooks;
    hooks.frame_document = [&documents](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        bindings::FrameDocument answer;
        answer.content_type = "text/html";
        if (policy)
            answer.policy = *policy;
        if (dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc")) {
            answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
            answer.url = *net::parse_url("about:srcdoc");
            // An srcdoc document has its parent's origin, a nested one's too.
            answer.origin = ancestors.empty() ? base : ancestors.back().origin;
            answer.srcdoc = true;
            return answer;
        }
        dom::Attr const* const src = iframe.find_attribute("src");
        std::optional<net::Url> const url = src ? net::parse_url(src->value, &base) : std::nullopt;
        if (!url)
            return std::nullopt;
        auto const it = documents.find(url->serialize());
        if (it == documents.end())
            return std::nullopt;
        answer.bytes.assign(it->second.begin(), it->second.end());
        answer.url = *url;
        answer.origin = *url;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var loads = {}; document.addEventListener('load', function (e) { if (e.target.tagName === 'IFRAME') loads[e.target.id] = (loads[e.target.id] || 0) + 1; }, true);</script>
<iframe id=a srcdoc="<script>var which = 'first'; setTimeout(function () { parent.aTimerRan = true; }, 5);</script>"></iframe>
<iframe id=b srcdoc="<script>var which = 'b';</script>"></iframe>
<iframe id=c srcdoc="<p id=keep class=x>kept</p><script>var which = 'c'; setTimeout(function () { parent.cTimerRan = true; }, 1500);</script>"></iframe>
<iframe id=d srcdoc="<script>parent.dOpened = (parent.dOpened || 0) + 1; frameElement.remove();</script>"></iframe>
<iframe id=f srcdoc="<iframe id=inner srcdoc='<script>setTimeout(function () { top.innerTimerRan = true; }, 1500);</script>'></iframe>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    CHECK(page->boolean("loads.a === 1 && loads.b === 1 && loads.c === 1 && loads.f === 1"));
    // d removed its own iframe from inside its load: opened once, no load
    // event at an iframe no longer in the tree.
    CHECK(page->boolean("dOpened === 1 && loads.d === undefined && document.getElementById('d') === null"));

    // A changed srcdoc: the frame's WindowProxy stands for the old window until
    // the task, then for the new one; what a script kept of the old window's
    // own functions stays the old window's.
    page->eval("var a = document.getElementById('a'); var oldWindow = a.contentWindow; var oldSetTimeout = oldWindow.setTimeout;"
               " var oldPostMessage = oldWindow.postMessage; var oldDocumentGetter = Object.getOwnPropertyDescriptor(oldWindow, 'document').get;"
               " a.srcdoc = \"<script>var which = 'changed';</script>\";");
    CHECK(page->boolean("a.contentWindow === oldWindow && oldWindow.which === 'first' && loads.a === 1"));
    // A src in place of a srcdoc: two changes, one navigation.
    page->eval("var b = document.getElementById('b'); b.removeAttribute('srcdoc'); b.src = 'second.html';");
    page->clock = 2000;
    page->realm->run_pending();
    CHECK(page->boolean("a.contentWindow === oldWindow && oldWindow.which === 'changed' && loads.a === 2"));
    CHECK(page->boolean("b.contentWindow.which === 'second' && loads.b === 2"));
    // The old document's timer never ran; the old window's functions answer
    // from nothing.
    CHECK(page->boolean("typeof aTimerRan === 'undefined'"));
    CHECK(page->boolean("oldDocumentGetter.call(undefined).body === null && oldWindow.nosuch === undefined"));
    CHECK(page->boolean("oldPostMessage('x', '*') === undefined && typeof oldSetTimeout(function () { parent.neverRuns = true; }, 0) === 'number'"));

    // A removed iframe: the frame closes at once, and its realm ends when the
    // script that removed it has returned.
    page->eval("var c = document.getElementById('c'); var cWindow = c.contentWindow; var cDoc = c.contentDocument;"
               " var keep = cDoc.getElementById('keep'); var keepStyle = keep.style; var cList = cDoc.body.classList;"
               " document.body.appendChild(keep); c.remove();"
               " var closedAtOnce = c.contentWindow === null && c.contentDocument === null;");
    CHECK(page->boolean("closedAtOnce"));
    CHECK(page->boolean("keep.textContent === 'kept' && keep.parentNode === document.body && keep.ownerDocument === document && keep.className === 'x'"));
    // The adopted node's wrapper is the page's now: writing through it moves
    // the page's mutation count.
    std::uint64_t const before = page->realm->mutation_count();
    page->eval("keepStyle.color = 'red';");
    CHECK(page->realm->mutation_count() > before);
    CHECK_EQ(page->string("keep.getAttribute('style')"), "color: red;");
    CHECK(page->boolean("cWindow.which === 'c' && cWindow.document.body === null && cWindow.keep === undefined"));
    // The TypeError is the frame realm's own, so it is told by name.
    CHECK(page->boolean("cList.length === 0 && (function () { try { cDoc.getElementById('keep'); } catch (e) { return e.name === 'TypeError'; } return false; })()"));
    // Its timer, and a nested frame's, due between the pumps, never run.
    page->eval("document.getElementById('f').remove();");
    page->clock = 3000;
    page->realm->run_pending();
    CHECK(page->boolean("typeof cTimerRan === 'undefined' && typeof innerTimerRan === 'undefined' && typeof neverRuns === 'undefined'"));

    // A script-inserted iframe has its initial about:blank document at once,
    // with no load, and opens what its srcdoc names after the script, once,
    // with a load, behind the same WindowProxy.
    page->eval("var e = document.createElement('iframe'); e.id = 'e';"
               " e.srcdoc = \"<script>parent.eOpened = (parent.eOpened || 0) + 1;</script>\"; document.body.appendChild(e);"
               " var eAtOnce = e.contentWindow;"
               " var blankAtOnce = eAtOnce !== null && e.contentDocument.URL === 'about:blank' && loads.e === undefined && typeof eOpened === 'undefined';");
    page->realm->run_pending();
    CHECK(page->boolean("blankAtOnce && eOpened === 1 && loads.e === 1 && e.contentWindow === eAtOnce"));
    // Removed from inside, by its own script, at the page's ask.
    page->eval("e.contentWindow.eval('frameElement.remove()');");
    CHECK(page->boolean("document.getElementById('e') === null && eOpened === 1"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// An iframe with nothing to show — no src, an empty one, about:blank — has
// the initial about:blank document (HTML §7.5.2): empty, of its parent's
// origin, with a window of its own the moment the iframe is in the tree, so
// a script that makes an iframe for a fresh realm has one before the next
// line runs; its load fires once, after the script. Pointing it somewhere
// later navigates it as any frame; removing it closes it.
void test_an_iframe_has_the_initial_about_blank_document()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt; // nothing the host can show: about:blank is the realm's own
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = ancestors.empty() ? base : ancestors.back().origin;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var loads = {}; document.addEventListener('load', function (e) { if (e.target.tagName === 'IFRAME') loads[e.target.id] = (loads[e.target.id] || 0) + 1; }, true);</script>
<iframe id=parsed></iframe><iframe id=blank src="about:blank"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    // The parse's own: a window each, empty, the page's origin, one load each.
    CHECK(page->boolean("loads.parsed === 1 && loads.blank === 1"));
    CHECK(page->boolean("parsed.contentWindow !== null && parsed.contentWindow.parent === window && parsed.contentDocument.URL === 'about:blank'"));
    CHECK(page->boolean("parsed.contentDocument.body !== null && parsed.contentDocument.body.childNodes.length === 0 && parsed.contentWindow.origin === 'https://example.test'"));
    CHECK(page->boolean("blank.contentWindow !== null && blank.contentWindow !== parsed.contentWindow"));
    // A script-made iframe: a window the moment it is in the tree, and a
    // realm of its own — its Array is not the page's.
    page->eval("var made = document.createElement('iframe'); made.id = 'made'; var beforeInsert = made.contentWindow;"
               " document.body.appendChild(made); var atOnce = made.contentWindow;"
               " var ownRealm = atOnce !== null && atOnce.Array !== Array && new atOnce.Array() instanceof atOnce.Array;"
               " atOnce.document.body.innerHTML = '<p>written</p>';");
    CHECK(page->boolean("beforeInsert === null && atOnce !== null && ownRealm"));
    // Its load fired inside the insertion: the document's capturing listener
    // saw it, and one added afterwards never will.
    CHECK(page->boolean("made.contentDocument.body.firstChild.textContent === 'written' && loads.made === 1"));
    page->eval("var lateLoad = false; made.addEventListener('load', function () { lateLoad = true; });");
    page->realm->run_pending();
    CHECK(page->boolean("loads.made === 1 && !lateLoad && made.contentWindow === atOnce"));
    // A document a script made has no browsing context, so an iframe put into
    // it gets no navigable and no window (HTML §4.8.5: only an iframe
    // connected to a document with a browsing context creates one).
    CHECK(page->boolean("(function () { var pd = new DOMParser().parseFromString('<body></body>', 'text/html'); var o = pd.createElement('iframe');"
                        " pd.body.appendChild(o); return o.contentWindow === null && o.contentDocument === null; })()"));
    // Pointed at an srcdoc afterwards: navigated, one more load, and the same
    // window, since the srcdoc document has the page's origin as the initial
    // about:blank document it replaces does.
    page->eval("var blankArray = atOnce.Array; made.srcdoc = '<script>var which = \"srcdoc\";</script>';");
    page->realm->run_pending();
    CHECK(page->boolean("made.contentWindow === atOnce && made.contentWindow.which === 'srcdoc' && atOnce.Array === blankArray && loads.made === 2"));
    // Removed: closed at once.
    page->eval("made.remove();");
    CHECK(page->boolean("made.contentWindow === null && atOnce.closed"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// A window is reached through its WindowProxy (HTML §7.2.3): `window`,
// `globalThis`, `this`, a frame's contentWindow, frames by index, parent and
// top are one object, which follows its frame to the next document and is
// closed with it. A window of another origin shows a script its frames and
// its CrossOriginProperties alone, through functions made for the realm that
// asks, and throws a SecurityError for the rest; its Location does the same.
// The window's own members check `this`.
void test_a_window_of_another_origin_shows_little()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        bindings::FrameDocument answer;
        answer.content_type = "text/html";
        dom::Attr const* const src = iframe.find_attribute("src");
        if (dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc")) {
            answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
            answer.url = *net::parse_url("about:srcdoc");
            answer.origin = ancestors.empty() ? base : ancestors.back().origin;
            answer.srcdoc = true;
            if (policy)
                answer.policy = *policy;
        } else if (src && src->value == "https://other.test/frame.html") {
            std::string const html = "<iframe></iframe><iframe name=donotleakme></iframe><script>var secret = 2; window.then = 'x';"
                                     " try { parent.document; parent.postMessage('read', '*'); } catch (e) { parent.postMessage(e.name, '*'); }</script>";
            answer.bytes.assign(html.begin(), html.end());
            answer.url = *net::parse_url(src->value);
            answer.origin = answer.url;
        } else {
            return std::nullopt;
        }
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var messages = []; addEventListener('message', function (e) { messages.push(e.data + ':' + (e.source === frames[1]) + ':' + e.origin); });
function threw(f, name) { try { f(); } catch (e) { return e.name === name; } return false; }</script>
<iframe id=same srcdoc="<iframe name=inner></iframe><script>window.frames = 'override'; var secret = 1;</script>"></iframe>
<iframe id=other src="https://other.test/frame.html"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    page->realm->run_pending();
    // One object for the window, however it is reached.
    CHECK(page->boolean("window === self && window === globalThis"));
    CHECK(page->boolean("this === window"));
    CHECK(page->boolean("(function () { return this; })() === window"));
    CHECK(page->boolean("document.defaultView === window"));
    // Window as WebIDL makes a [Global] interface: its members the window's
    // own, EventTarget's inherited, the chain through Window.prototype.
    CHECK(page->boolean("window instanceof Window && window.constructor === Window && !window.hasOwnProperty('constructor')"));
    CHECK(page->boolean("Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf(window))) === EventTarget.prototype"));
    CHECK(page->boolean("Object.getOwnPropertyDescriptor(window, 'addEventListener') === undefined && window.addEventListener === EventTarget.prototype.addEventListener"));
    CHECK(page->boolean("var d = Object.getOwnPropertyDescriptor(window, 'document'); d.enumerable && !d.configurable && d.set === undefined"));
    CHECK(page->boolean("var t = Object.getOwnPropertyDescriptor(window, 'setTimeout'); t.writable && t.enumerable && t.configurable"));
    CHECK(page->boolean("var h = Object.getOwnPropertyDescriptor(window, 'history'); typeof h.get === 'function' && h.set === undefined && h.enumerable"));
    CHECK(page->boolean("screenX = 5; Object.getOwnPropertyDescriptor(window, 'screenX').value === 5 && typeof Object.getOwnPropertyDescriptor(window, 'innerWidth').set === 'function'"));
    CHECK(page->boolean("window.length === 2 && frames[0] === document.getElementById('same').contentWindow && window[1] === document.getElementById('other').contentWindow"));
    page->eval("var s = document.getElementById('same').contentWindow; var w = document.getElementById('other').contentWindow;");
    CHECK(page->boolean("s.parent === window && s.top === window && s.self === s && s.window === s"));
    // The same origin: all of it, the frame's own replacements included.
    CHECK(page->boolean("s.frames === 'override' && s.secret === 1 && s.length === 1 && s.inner === s[0] && s.document.defaultView === s && Object.getPrototypeOf(s) !== null"));
    // Another origin: its frames and its CrossOriginProperties, nothing more.
    CHECK(page->boolean("w.parent === window && w.top === window && w.self === w && w.frames === w && w.window === w && w.closed === false"));
    CHECK(page->boolean("w.length === 2 && typeof w[0] === 'object' && w.donotleakme === w[1] && w.then === undefined"));
    CHECK(page->boolean("threw(function () { return w['']; }, 'SecurityError')"));
    CHECK(page->boolean("document.getElementById('other').contentDocument === null"));
    CHECK(page->boolean("threw(function () { return w.secret; }, 'SecurityError') && threw(function () { return w.document; }, 'SecurityError')"));
    CHECK(page->boolean("threw(function () { w.secret = 1; }, 'SecurityError') && threw(function () { delete w.parent; }, 'SecurityError')"));
    CHECK(page->boolean("threw(function () { Object.defineProperty(w, 'x', { value: 1 }); }, 'SecurityError') && threw(function () { return 'secret' in w; }, 'SecurityError')"));
    CHECK(page->boolean("threw(function () { return Object.getOwnPropertyDescriptor(w, '2'); }, 'SecurityError')"));
    CHECK(page->boolean("Object.getPrototypeOf(w) === null && Object.isExtensible(w) && Reflect.preventExtensions(w) === false"));
    CHECK(page->boolean("Reflect.setPrototypeOf(w, null) === true && Reflect.setPrototypeOf(w, {}) === false"));
    CHECK_EQ(page->string("Object.getOwnPropertyNames(w).join()"), "0,1,window,self,location,close,closed,focus,blur,frames,length,top,opener,parent,postMessage,then");
    CHECK_EQ(page->string("Reflect.ownKeys(w).length + ':' + Object.keys(w).join()"), "19:0,1");
    // The functions another origin is shown are the asking realm's, the same
    // each time, and act on the window they were made for.
    CHECK(page->boolean("typeof w.close === 'function' && w.close === w.close && w.close !== close && Object.getPrototypeOf(w.close) === Function.prototype"));
    CHECK(page->boolean("var parentGetter = Object.getOwnPropertyDescriptor(w, 'parent').get; parentGetter.call(undefined) === window && parentGetter.name === 'get parent'"));
    CHECK(page->boolean("({}).toString.call(w) === '[object Object]'"));
    // Its Location: the href setter and replace.
    CHECK(page->boolean("w.location === w.location && typeof w.location.replace === 'function' && Object.getOwnPropertyDescriptor(w.location, 'href').get === undefined"));
    CHECK(page->boolean("threw(function () { return w.location.href; }, 'SecurityError') && threw(function () { return w.location.pathname; }, 'SecurityError')"));
    CHECK(page->boolean("Object.getPrototypeOf(w.location) === null && Object.getOwnPropertyNames(w.location).join() === 'href,replace,then'"));
    CHECK(page->boolean("s.location.href === 'about:srcdoc' && Object.getPrototypeOf(s.location) === s.Location.prototype"));
    // What the functions another origin is shown throw before they reach the
    // window is the asking realm's: a `this` that is no location, a URL that
    // is no string.
    CHECK(page->boolean("var hrefSetter = Object.getOwnPropertyDescriptor(w.location, 'href').set;"
                        " (function () { try { hrefSetter.call({}, 'x'); } catch (e) { return e instanceof TypeError; } return false; })()"));
    CHECK(page->boolean("(function () { try { w.location.href = Symbol(); } catch (e) { return e instanceof TypeError; } return false; })()"
                        " && (function () { try { w.location = Symbol(); } catch (e) { return e instanceof TypeError; } return false; })()"));
    // window.location's setter forwards to the Location's href setter, as a
    // [[Set]] from the setter's own realm ([PutForwards=href], WebIDL §3.7.6):
    // a URL that is no string throws from that href setter, the frame's own
    // for a frame of the same origin, the asking realm's for another origin.
    CHECK(page->boolean("var locationSetter = Object.getOwnPropertyDescriptor(window, 'location').set;"
                        " (function () { try { locationSetter.call(s, Symbol()); } catch (e) { return e instanceof s.TypeError && !(e instanceof TypeError); } return false; })()"));
    CHECK(page->boolean("(function () { try { locationSetter.call(w, Symbol()); } catch (e) { return e instanceof TypeError; } return false; })()"));
    // Only the setter forwards; the getter takes no argument, so it converts
    // none and a symbol passed to it is ignored.
    CHECK(page->boolean("Object.getOwnPropertyDescriptor(window, 'location').get.call(window, Symbol()) === location"));
    // The window's bars, window.external and the media event handlers.
    CHECK(page->boolean("locationbar.visible === true && toolbar instanceof BarProp && typeof external.AddSearchProvider === 'function' && 'oncanplay' in window && onended === null"));
    // The window's own members check `this`: another origin's window only for
    // what that origin shows, and anything else is no window at all.
    CHECK(page->boolean("var documentGetter = Object.getOwnPropertyDescriptor(window, 'document').get; threw(function () { documentGetter.call(w); }, 'SecurityError') && documentGetter.call(s) === s.document"));
    CHECK(page->boolean("Object.getOwnPropertyDescriptor(window, 'closed').get.call(w) === false && threw(function () { documentGetter.call({}); }, 'TypeError')"));
    CHECK(page->boolean("var hrefGetter = Object.getOwnPropertyDescriptor(location, 'href').get; threw(function () { hrefGetter.call(w.location); }, 'SecurityError') && hrefGetter.call(s.location) === 'about:srcdoc'"));
    // What the frame of another origin met reaching into the page; the
    // message's source is that frame's WindowProxy.
    CHECK_EQ(page->string("messages.join()"), "SecurityError:true:https://other.test");
    // Events and timers see the WindowProxy.
    page->eval("addEventListener('custom', function (e) { window.eventSaw = e.target === window && e.currentTarget === window && this === window; }); dispatchEvent(new Event('custom'));"
               " setTimeout(function () { window.timerSaw = this === window; }, 0);");
    page->clock += 10;
    page->realm->run_pending();
    CHECK(page->boolean("eventSaw === true && timerSaw === true"));
    // A frame going on to another document keeps its WindowProxy; removed, its
    // window is closed and has no parent or top.
    page->eval("var held = s; var oldArray = s.Array; document.getElementById('same').srcdoc = '<script>var second = 2;</script>';");
    page->realm->run_pending();
    CHECK(page->boolean("held === document.getElementById('same').contentWindow && held.second === 2 && held.secret === undefined && held.Array !== oldArray && held.length === 0"));
    page->eval("document.getElementById('same').remove();");
    CHECK(page->boolean("held.closed === true && held.parent === null && held.top === null && window.length === 1 && frames[0] === w"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// document.domain (HTML §7.1.3): a document may set its domain to its own
// host, or to a suffix of it that is not a public suffix, and from then on it
// is same origin-domain only with documents that set the same: a frame of the
// page's own origin that sets it is another origin to the page until the page
// sets it too. An srcdoc or about:blank document has the page's origin itself,
// so what either sets is both's. A document with no window may not set it.
void test_document_domain_relaxes_the_same_origin_rule()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        bindings::FrameDocument answer;
        answer.content_type = "text/html";
        dom::Attr const* const src = iframe.find_attribute("src");
        if (dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc")) {
            answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
            answer.url = *net::parse_url("about:srcdoc");
            answer.origin = ancestors.empty() ? base : ancestors.back().origin;
            answer.srcdoc = true;
            if (policy)
                answer.policy = *policy;
        } else if (src && src->value == "https://www.example.test/frame.html") {
            std::string const html = "<script>var secret = 1; document.domain = document.domain;</script>";
            answer.bytes.assign(html.begin(), html.end());
            answer.url = *net::parse_url(src->value);
            answer.origin = answer.url;
        } else {
            return std::nullopt;
        }
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>function threw(f, name) { try { f(); } catch (e) { return e.name === name; } return false; }</script>
<iframe id=f src="https://www.example.test/frame.html"></iframe>
<iframe id=g srcdoc="<script>var shared = 3;</script>"></iframe>)HTML",
        "https://www.example.test/page.html", std::move(hooks));
    page->load();
    page->eval("var f = document.getElementById('f').contentWindow; var g = document.getElementById('g').contentWindow;"
               " var blank = document.createElement('iframe'); document.body.appendChild(blank); blank.contentDocument.body.textContent = 'blank';");
    CHECK(page->boolean("document.domain === 'www.example.test'"));
    // The frame at a URL of the page's origin set its domain: another origin
    // to the page now.
    CHECK(page->boolean("threw(function () { return f.secret; }, 'SecurityError') && document.getElementById('f').contentDocument === null"));
    CHECK(page->boolean("threw(function () { document.domain = 'other.test'; }, 'SecurityError') && threw(function () { document.domain = 'test'; }, 'SecurityError')"));
    CHECK(page->boolean("threw(function () { document.domain = 'sub.www.example.test'; }, 'SecurityError')"));
    CHECK(page->boolean("threw(function () { document.implementation.createHTMLDocument('').domain = 'www.example.test'; }, 'SecurityError')"));
    page->eval("document.domain = 'www.example.test';");
    CHECK(page->boolean("f.secret === 1 && document.getElementById('f').contentDocument !== null"));
    // The srcdoc and about:blank frames were given the page's domain with it.
    CHECK(page->boolean("g.shared === 3 && g.document.domain === 'www.example.test' && blank.contentDocument.body.textContent === 'blank'"));
    page->eval("document.domain = 'example.test';");
    CHECK(page->boolean("document.domain === 'example.test' && threw(function () { return f.secret; }, 'SecurityError')"));
    CHECK(page->boolean("g.shared === 3 && g.document.domain === 'example.test' && blank.contentWindow.document.domain === 'example.test'"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// The Origin interface (HTML §7.1.1): an origin as an object. `new Origin()`
// is a fresh opaque origin; Origin.from reads one out of a URL string, a URL,
// an Origin, a hyperlink element (<a>, <area>, the SVG <a> by its href or
// xlink:href), a message a window posted and a window of the caller's origin,
// and throws a TypeError for anything else. Opaque origins compare by
// identity, which a document keeps for its own origin and a message carries
// from its sender; isSameSite compares the scheme and the registrable domain.
// The SVG <a>'s href is an SVGAnimatedString reflecting the attribute.
void test_the_origin_interface()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        // The iframe with the id "opaque" shows a document of an opaque
        // origin, as a data: URL's document is.
        dom::Attr const* const id = iframe.find_attribute("id");
        bool const opaque = id && id->value == "opaque";
        answer.url = *net::parse_url(opaque ? "data:text/html,frame" : "about:srcdoc");
        answer.origin = opaque ? answer.url : base;
        answer.srcdoc = !opaque;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var messages = []; addEventListener('message', function (e) { messages.push({ data: e.data, origin: Origin.from(e) }); });</script>
<svg><a id=parsedLink xlink:href="https://parsed.example/"></a></svg>
<iframe id=same srcdoc="<script>parent.postMessage('same', '*');</script>"></iframe>
<iframe id=opaque srcdoc="<script>var received = []; addEventListener('message', function (e) { received.push({ data: e.data, origin: Origin.from(e) }); }); postMessage('first', '*'); postMessage('second', '*');</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    // No script of the page reaches a window of another origin until a window
    // proxy guards it, so the page is handed the opaque frame's window here,
    // as a message's source would hand it, and posts it a message.
    dom::Node* const opaque_node = page->realm->node_of(page->eval("document.getElementById('opaque')").value);
    bindings::Realm* const opaque_realm = opaque_node && opaque_node->is_element()
        ? page->realm->frame_realm(*static_cast<dom::Element*>(opaque_node))
        : nullptr;
    CHECK(opaque_realm != nullptr);
    if (!opaque_realm)
        return;
    js::Interpreter& interpreter = page->realm->interpreter();
    page->realm->window()->put(interpreter.key("opaqueWindow"), js::Value::object(opaque_realm->window()), js::default_attributes);
    page->eval("opaqueWindow.postMessage('from the page', '*');");
    page->realm->run_pending();
    // A boolean from a script run in the opaque frame's own realm.
    auto const in_opaque_frame = [&opaque_realm](std::string_view source) {
        js::Outcome const outcome = opaque_realm->run(source, "<test>");
        return outcome.ok && outcome.value.is_boolean() && outcome.value.as_boolean();
    };

    // The constructor: a new opaque origin each time, same origin and same
    // site with itself alone.
    CHECK(page->boolean("typeof Origin === 'function' && new Origin().opaque === true && Object.prototype.toString.call(new Origin()) === '[object Origin]'"));
    CHECK(page->boolean("(function () { var a = new Origin(), b = new Origin(); return a.isSameOrigin(a) && a.isSameSite(a) && !a.isSameOrigin(b) && !a.isSameSite(b); })()"));
    // Strings, parsed as URLs: tuple origins compare by scheme, host and port;
    // sites by the scheme and the registrable domain.
    CHECK(page->boolean("(function () { var a = Origin.from('https://a.example'); return !a.opaque"
                        " && a.isSameOrigin(Origin.from('https://a.example:443/path?q#f'))"
                        " && !a.isSameOrigin(Origin.from('https://a.example:8443'))"
                        " && !a.isSameOrigin(Origin.from('https://b.a.example'))"
                        " && a.isSameSite(Origin.from('https://b.a.example')) && Origin.from('https://b.a.example').isSameSite(Origin.from('https://c.a.example:99'))"
                        " && !a.isSameSite(Origin.from('https://b.example')) && !a.isSameSite(Origin.from('https://a.b.example'))"
                        " && !Origin.from('http://a.example').isSameOrigin(a) && !Origin.from('http://a.example').isSameSite(a); })()"));
    CHECK(page->boolean("Origin.from('https://\xC3\xBCmlauted.example').isSameOrigin(Origin.from('https://xn--mlauted-m2a.example'))"
                        " && Origin.from('https://user:pass@site.example').isSameOrigin(Origin.from('https://site.example'))"
                        " && Origin.from('blob:https://example.com/some-guid').isSameOrigin(Origin.from('https://example.com'))"
                        " && Origin.from('https://127.0.0.1/').isSameSite(Origin.from('https://127.0.0.1:8080/'))"
                        " && !Origin.from('https://127.0.0.1/').isSameSite(Origin.from('https://127.0.0.2/'))"
                        " && Origin.from('https://[::1]/').isSameOrigin(Origin.from('https://[0::1]:443'))"));
    // An opaque origin from a string is a new one each time, kept by an
    // Origin made from it.
    CHECK(page->boolean("(function () { var d = Origin.from('data:text/plain,x'); return d.opaque && d.isSameOrigin(d) && d.isSameSite(d)"
                        " && !d.isSameOrigin(Origin.from('data:text/plain,x')) && !d.isSameSite(Origin.from('data:text/plain,x'))"
                        " && Origin.from(d).isSameOrigin(d) && Origin.from(d) !== d"
                        " && Origin.from('weird-protocol:whatever').opaque && Origin.from('blob:weird-protocol:whatever').opaque"
                        " && Origin.from('file:///path/to/a/file.txt').opaque && Origin.from('about:blank').opaque; })()"));
    // A URL object's origin.
    CHECK(page->boolean("Origin.from(new URL('https://site.example:123/p')).isSameOrigin(Origin.from('https://site.example:123')) && Origin.from(new URL('about:blank')).opaque"));
    // Everything else throws this realm's TypeError: values that are not
    // platform objects, strings that do not parse, a String object, a
    // Location, a constructed MessageEvent, elements with no URL.
    CHECK(page->boolean("[null, undefined, 1, 1.1, true, {}, Object, Origin, Origin.from, '', 'not-valid', new String('https://a.example'), location,"
                        " new MessageEvent('message', { origin: 'https://example.test' }), document.createElement('a'), document.createElement('area'),"
                        " document.createElement('div'), document.createElementNS('http://www.w3.org/2000/svg', 'a'),"
                        " document.createElementNS('http://www.w3.org/1998/Math/MathML', 'a')].every(function (value) {"
                        " try { Origin.from(value); } catch (e) { return e instanceof TypeError; } return false; })"));
    // An element that is not a hyperlink element gives no origin, whatever
    // its href attribute holds: a <div>, a <link>, the SVG <use>, the MathML
    // <a>.
    CHECK(page->boolean("(function () { var div = document.createElement('div'), link = document.createElement('link');"
                        " var use = document.createElementNS('http://www.w3.org/2000/svg', 'use'), math = document.createElementNS('http://www.w3.org/1998/Math/MathML', 'a');"
                        " return [div, link, use, math].every(function (element) { element.setAttribute('href', 'https://site.example/');"
                        " try { Origin.from(element); } catch (e) { return e instanceof TypeError; } return false; }); })()"));
    CHECK(page->boolean("(function () { try { new Origin().isSameOrigin({}); } catch (e) { return e instanceof TypeError; } return false; })()"));
    CHECK(page->boolean("(function () { try { Object.getOwnPropertyDescriptor(Origin.prototype, 'opaque').get.call({}); } catch (e) { return e instanceof TypeError; } return false; })()"));
    // <a> and <area>: the origin of the href resolved against the document,
    // an opaque one a new one each time.
    CHECK(page->boolean("(function () { var a = document.createElement('a'); a.href = 'https://site.example/x'; var relative = document.createElement('a'); relative.href = 'other.html';"
                        " var area = document.createElement('area'); area.href = 'data:,x';"
                        " return Origin.from(a).isSameOrigin(Origin.from('https://site.example')) && Origin.from(relative).isSameOrigin(Origin.from('https://example.test'))"
                        " && Origin.from(area).opaque && !Origin.from(area).isSameOrigin(Origin.from(area)); })()"));
    // The SVG <a>: href an SVGAnimatedString over the href attribute, read
    // from xlink:href when there is no href; the origin of what it reads.
    CHECK(page->boolean("(function () { var a = document.createElementNS('http://www.w3.org/2000/svg', 'a');"
                        " if (!(a instanceof SVGAElement) || !(a instanceof SVGElement) || !(a.href instanceof SVGAnimatedString) || a.href.baseVal !== '') return false;"
                        " a.href.baseVal = 'https://site.example/y';"
                        " return a.getAttribute('href') === 'https://site.example/y' && a.href.baseVal === 'https://site.example/y' && a.href.animVal === 'https://site.example/y'"
                        " && Origin.from(a).isSameOrigin(Origin.from('https://site.example')); })()"));
    CHECK(page->boolean("(function () { var a = document.createElementNS('http://www.w3.org/2000/svg', 'a');"
                        " a.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', 'https://other.example/');"
                        " if (a.href.baseVal !== 'https://other.example/' || !Origin.from(a).isSameOrigin(Origin.from('https://other.example'))) return false;"
                        " a.setAttribute('href', 'data:,z'); return a.href.baseVal === 'data:,z' && Origin.from(a).opaque; })()"));
    CHECK(page->boolean("(function () { var parsed = document.getElementById('parsedLink');"
                        " return parsed.href.baseVal === 'https://parsed.example/' && Origin.from(parsed).isSameOrigin(Origin.from('https://parsed.example')); })()"));
    // An attribute in no namespace named xlink:href is not the XLink href.
    CHECK(page->boolean("(function () { var a = document.createElementNS('http://www.w3.org/2000/svg', 'a'); a.setAttribute('xlink:href', 'https://nonamespace.example/');"
                        " if (a.getAttribute('xlink:href') !== 'https://nonamespace.example/' || a.href.baseVal !== '') return false;"
                        " try { Origin.from(a); } catch (e) { return e instanceof TypeError; } return false; })()"));
    // baseVal writes the attribute it reflects: xlink:href while the element
    // has no href, and href once it has one.
    CHECK(page->boolean("(function () { var holder = document.createElement('div'); holder.innerHTML = '<svg><a xlink:href=\"https://parsed.example/\"></a></svg>';"
                        " var a = holder.firstChild.firstChild; a.href.baseVal = 'https://written.example/';"
                        " if (a.attributes.length !== 1 || a.getAttributeNS('http://www.w3.org/1999/xlink', 'href') !== 'https://written.example/' || a.hasAttribute('href')) return false;"
                        " a.setAttribute('href', 'https://both.example/'); a.href.baseVal = 'https://href.example/';"
                        " return a.attributes.length === 2 && a.getAttribute('href') === 'https://href.example/'"
                        " && a.getAttributeNS('http://www.w3.org/1999/xlink', 'href') === 'https://written.example/' && a.href.animVal === 'https://href.example/'; })()"));
    // href is one SVGAnimatedString for the element's life, kept through
    // collections while nothing else holds it.
    CHECK(page->boolean("(function () { var a = document.createElementNS('http://www.w3.org/2000/svg', 'a'); a.href.expando = 'kept';"
                        " for (var i = 0; i < 64; i++) [i, {}]; var first = a.href; a.setAttribute('href', 'https://same.example/');"
                        " return a.href === first && first.expando === 'kept' && first.baseVal === 'https://same.example/'; })()"));
    // A window of this origin: its document's origin; an Origin made by
    // another realm's from is that realm's, and compares all the same.
    CHECK(page->boolean("!Origin.from(window).opaque && Origin.from(window).isSameOrigin(Origin.from('https://example.test'))"
                        " && Origin.from(document.getElementById('same').contentWindow).isSameOrigin(Origin.from(window))"));
    CHECK(page->boolean("(function () { var other = document.getElementById('same').contentWindow; var made = other.Origin.from('https://a.example');"
                        " return made instanceof other.Origin && !(made instanceof Origin) && made.isSameOrigin(Origin.from('https://a.example')); })()"));
    // A window of another origin gives this caller no origin.
    CHECK(page->boolean("(function () { try { Origin.from(opaqueWindow); } catch (e) { return e instanceof TypeError; } return false; })()"));
    // A document of an opaque origin keeps one identity for it, in its own
    // Origin.from and in every message it posts.
    CHECK(in_opaque_frame("(function () { var a = Origin.from(globalThis), b = Origin.from(window); return a.opaque && a.isSameOrigin(b)"
                          " && !a.isSameOrigin(Origin.from('data:text/html,frame')); })()"));
    // Messages: the origin of the document whose window posted them, not the
    // receiver's.
    CHECK(page->boolean("messages.length === 1 && messages[0].data === 'same' && messages[0].origin.isSameOrigin(Origin.from(window))"));
    CHECK(in_opaque_frame("received.length === 3 && received[0].data === 'first' && received[0].origin.opaque"
                          " && received[0].origin.isSameOrigin(received[1].origin) && received[1].origin.isSameOrigin(Origin.from(window))"));
    CHECK(in_opaque_frame("received[2].data === 'from the page' && !received[2].origin.opaque && received[2].origin.isSameOrigin(Origin.from('https://example.test'))"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// Attributes in namespaces (DOM §4.9): setAttributeNS validates and extracts
// the namespace, prefix and local name and keeps all three; the NS methods
// find an attribute by its namespace and local name, the others the first by
// its qualified name.
void test_attributes_in_namespaces()
{
    auto page = loaded("<!DOCTYPE html><p></p>");
    CHECK(page->boolean("(function () { var e = document.createElement('p'); e.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', 'v'); var attr = e.attributes[0];"
                        " return e.attributes.length === 1 && attr.namespaceURI === 'http://www.w3.org/1999/xlink' && attr.prefix === 'xlink' && attr.localName === 'href'"
                        " && attr.name === 'xlink:href' && e.getAttributeNames()[0] === 'xlink:href'; })()"));
    // By namespace and local name: a second prefix sets the same attribute
    // and keeps the first prefix.
    CHECK(page->boolean("(function () { var e = document.createElement('p'); e.setAttributeNS('ns', 'a:x', '1'); e.setAttributeNS('ns', 'b:x', '2'); e.setAttributeNS(null, 'x', '3');"
                        " if (e.attributes.length !== 2 || e.attributes[0].prefix !== 'a' || e.getAttributeNS('ns', 'x') !== '2' || e.getAttributeNS('', 'x') !== '3') return false;"
                        " if (e.getAttributeNS('ns', 'a:x') !== null || !e.hasAttributeNS('ns', 'x') || e.hasAttributeNS('other', 'x')) return false;"
                        " e.removeAttributeNS('ns', 'x'); e.setAttributeNS('ns', 'c:x', '4'); e.removeAttributeNS('ns', 'x'); return e.attributes.length === 1 && e.getAttributeNS(null, 'x') === '3'; })()"));
    // By qualified name: the first attribute that has it, in any namespace.
    CHECK(page->boolean("(function () { var e = document.createElement('p'); e.setAttributeNS('ns', 'a:x', '1'); e.setAttributeNS('other', 'x', '2');"
                        " if (e.getAttribute('a:x') !== '1' || e.getAttribute('x') !== '2' || !e.hasAttribute('a:x')) return false;"
                        " e.setAttribute('a:x', 'changed'); if (e.attributes.length !== 2 || e.getAttributeNS('ns', 'x') !== 'changed') return false;"
                        " e.removeAttribute('a:x'); return e.attributes.length === 1 && e.getAttributeNS('other', 'x') === '2'; })()"));
    // The exceptions of validate and extract.
    CHECK(page->boolean("(function () { var e = document.createElement('p'); function thrown(namespace, name) { try { e.setAttributeNS(namespace, name, 'v'); } catch (x) { return x.name; } return 'none'; }"
                        " return thrown(null, 'a:b') === 'NamespaceError' && thrown('ns', 'xml:b') === 'NamespaceError' && thrown('ns', 'xmlns') === 'NamespaceError'"
                        " && thrown('http://www.w3.org/2000/xmlns/', 'a:b') === 'NamespaceError' && thrown('ns', 'b:') === 'InvalidCharacterError'"
                        " && thrown('ns', ':b') === 'InvalidCharacterError' && thrown('http://www.w3.org/XML/1998/namespace', 'a:b') === 'none'"
                        " && thrown('http://www.w3.org/2000/xmlns/', 'xmlns:a') === 'none' && e.attributes.length === 2; })()"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// A host for the frame tests below: an srcdoc's text, of its parent's origin;
// a src, or a URL the frame navigates to, from `documents` by URL without the
// fragment, of that URL's origin; nothing for the rest.
bindings::HostHooks hooks_serving(std::map<std::string, std::string> const& documents)
{
    bindings::HostHooks hooks;
    hooks.frame_document = [&documents](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors,
                               std::optional<net::Url> const& target) -> std::optional<bindings::FrameDocument> {
        bindings::FrameDocument answer;
        answer.content_type = "text/html";
        if (policy)
            answer.policy = *policy;
        dom::Attr const* const srcdoc = target ? nullptr : iframe.find_attribute("srcdoc");
        if (srcdoc) {
            answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
            answer.url = *net::parse_url("about:srcdoc");
            answer.origin = ancestors.empty() ? base : ancestors.back().origin;
            answer.srcdoc = true;
            return answer;
        }
        dom::Attr const* const src = iframe.find_attribute("src");
        std::optional<net::Url> const url = target ? target : src ? net::parse_url(src->value, &base) : std::nullopt;
        if (!url)
            return std::nullopt;
        auto const it = documents.find(url->serialize(true));
        if (it == documents.end())
            return std::nullopt;
        answer.bytes.assign(it->second.begin(), it->second.end());
        answer.url = *url;
        answer.origin = *url;
        return answer;
    };
    return hooks;
}

// The realm of the frame of the page's iframe with this id, whatever its origin.
bindings::Realm* frame_realm_of(Page& page, std::string const& id)
{
    dom::Node* const node = page.realm->node_of(page.eval("document.getElementById('" + id + "')").value);
    return node && node->is_element() ? page.realm->frame_realm(static_cast<dom::Element&>(*node)) : nullptr;
}

// A string a realm answers, run there as the host runs script.
std::string string_in(bindings::Realm* realm, std::string_view source)
{
    if (!realm) {
        test::fail("no realm to evaluate: " + std::string(source), __FILE__, __LINE__);
        return "";
    }
    js::Outcome const outcome = realm->run(source, "<test>");
    if (!outcome.ok || !outcome.value.is_string()) {
        test::fail("not a string: " + std::string(source), __FILE__, __LINE__);
        return "";
    }
    return outcome.value.as_string()->to_utf8();
}

// A frame's navigable keeps one target name (HTML §7.3.1): its iframe's name
// attribute when the navigable is made, then whatever window.name sets, across
// every document the frame goes on to; the attribute changed later does not
// touch it. window.name is that name, and a window with no navigable — its
// frame removed, or gone on to another document, or its realm ended — has none
// to read or to set. The parent names its frames by it.
// window.open: _self, _parent and _top name a window here, and a frame's
// name is found in this window's frames, theirs, and the windows above;
// that window navigates and its proxy comes back, or null with noopener.
// A new window is the host's: none here means null and a console line; a
// host that opens windows is asked only on the reader's gesture, and a
// sandboxed document without popups asks nothing.
void test_window_open()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/dir/a.html"] = "<script>var which = 'a';</script>";
    documents["https://example.test/dir/b.html"] = "<script>var which = 'b';</script>";
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<iframe id=f name=first src="a.html"></iframe>
<iframe id=n srcdoc="<iframe name=inner src='a.html'></iframe>"></iframe>
<iframe id=s sandbox="allow-scripts" srcdoc="<p>boxed</p>"></iframe>)HTML",
        "https://example.test/dir/page.html", hooks_serving(documents));
    page->load();
    // _self and _top navigate this window through its host, and the window
    // itself comes back — null when noopener is asked for.
    CHECK(page->boolean("window.open('b.html', '_self') === window"));
    CHECK(page->navigations.size() == 1 && page->navigations.back().serialize() == "https://example.test/dir/b.html");
    CHECK(page->boolean("window.open('b.html', '_top', 'noopener') === null"));
    CHECK_EQ(page->navigations.size(), std::size_t(2));
    // A frame by name, among this window's frames and their frames: the
    // frame navigates, in a task, and its proxy comes back at once.
    page->eval("var f = document.getElementById('f'); var fw = f.contentWindow; var opened = window.open('b.html', 'first');");
    CHECK(page->boolean("opened === fw"));
    page->realm->run_pending();
    CHECK(page->boolean("opened === fw && fw.which === 'b'"));
    page->eval("var innerOpened = window.open('b.html', 'inner');");
    page->realm->run_pending();
    CHECK(page->boolean("innerOpened === document.getElementById('n').contentWindow.frames[0] && innerOpened.which === 'b'"));
    // From a frame, _parent is the page, and a name is found up the tree.
    CHECK_EQ(string_in(frame_realm_of(*page, "n"), "String(window.open('b.html', '_parent') === parent)"), std::string("true"));
    CHECK_EQ(page->navigations.size(), std::size_t(3));
    CHECK_EQ(string_in(frame_realm_of(*page, "n"), "String(window.open('a.html', 'first') === parent.frames[0])"), std::string("true"));
    // A name found nowhere, _blank and no target at all ask the host for a
    // new window: none here, so null and a console line each.
    CHECK(page->boolean("window.open('b.html', 'nowhere') === null && window.open('b.html', '_blank') === null && window.open() === null"));
    CHECK(page->console.find("info:window.open blocked: https://example.test/dir/b.html|") != std::string::npos);
    CHECK(page->console.find("info:window.open blocked: about:blank|") != std::string::npos);
    // A sandboxed document without popups asks nothing.
    CHECK_EQ(string_in(frame_realm_of(*page, "s"), "String(window.open('b.html') === null)"), std::string("true"));
    CHECK(page->console.find("allows no popups") != std::string::npos);
    // A URL that does not parse is a SyntaxError.
    CHECK(page->boolean("(function () { try { window.open('https://exa mple/'); return false; } catch (e) { return e.name === 'SyntaxError'; } })()"));

    // A host that opens windows: asked only on the reader's gesture, with
    // the URL resolved and whether the referrer is to be kept back.
    bool active = false;
    std::vector<std::string> opened;
    bindings::HostHooks hooks;
    hooks.open_window = [&opened](net::Url const& url, bool noreferrer) {
        opened.push_back(url.serialize() + (noreferrer ? " noreferrer" : ""));
    };
    hooks.user_activation = [&active] { return active; };
    Page popup("<!DOCTYPE html><p>x</p>", "https://example.test/dir/page.html", std::move(hooks));
    popup.load();
    CHECK(popup.boolean("window.open('b.html') === null"));
    CHECK(opened.empty());
    CHECK(popup.console.find("not from a gesture") != std::string::npos);
    active = true;
    CHECK(popup.boolean("window.open('b.html', '_blank', 'noreferrer') === null"));
    CHECK(popup.boolean("window.open('c.html', 'nowhere', 'noopener=1,width=100') === null"));
    CHECK(popup.boolean("window.open() === null"));
    CHECK_EQ(opened.size(), std::size_t(3));
    if (opened.size() == 3) {
        CHECK_EQ(opened[0], std::string("https://example.test/dir/b.html noreferrer"));
        CHECK_EQ(opened[1], std::string("https://example.test/dir/c.html"));
        CHECK_EQ(opened[2], std::string("about:blank"));
    }
}

void test_a_navigable_keeps_its_target_name()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/dir/a.html"] = "<script>var which = 'a';</script>";
    documents["https://example.test/dir/b.html"] = "<script>var which = 'b';</script>";
    documents["https://example.test/dir/probe.html"] = "<script>var which = 'probe'; var oldName = parent.oldNameGetter.call(undefined); var ownName = window.name;</script>";
    documents["https://other.test/c.html"] = "<script>var which = 'c';</script>";
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<iframe id=f name=first src="a.html"></iframe><iframe id=g src="a.html"></iframe>
<iframe id=x name=cross src="https://other.test/c.html"></iframe>
<iframe id=n srcdoc="<iframe name=inner></iframe>"></iframe>)HTML",
        "https://example.test/dir/page.html", hooks_serving(documents));
    page->load();
    page->eval("var f = document.getElementById('f'), g = document.getElementById('g'), x = document.getElementById('x'), n = document.getElementById('n');"
               " var fw = f.contentWindow, gw = g.contentWindow, xw = x.contentWindow;");
    CHECK(page->boolean("fw.name === 'first' && gw.name === '' && window.first === fw"));

    // A name a script gives stays with the frame as it navigates.
    page->eval("fw.name = 'renamed'; gw.name = 'given'; f.src = 'b.html'; g.src = 'b.html';");
    page->realm->run_pending();
    CHECK(page->boolean("fw.which === 'b' && fw.name === 'renamed'"));
    CHECK(page->boolean("gw.which === 'b' && gw.name === 'given'"));
    CHECK(page->boolean("window.renamed === fw && window.given === gw && window.first === undefined"));

    // The attribute changed afterwards is not read again; the window the frame
    // left, still running while the next document parses, has no name.
    page->eval("f.setAttribute('name', 'attr'); var oldNameGetter = Object.getOwnPropertyDescriptor(fw, 'name').get; f.src = 'probe.html';");
    page->realm->run_pending();
    CHECK(page->boolean("fw.which === 'probe' && fw.name === 'renamed' && window.attr === undefined"));
    CHECK(page->boolean("fw.oldName === '' && fw.ownName === 'renamed'"));

    // Removed, the frame's window neither reads nor keeps a name; nor once its
    // realm has ended.
    CHECK(page->boolean("(function () { var held = gw; g.remove(); var a = held.name; held.name = 'again'; return a === '' && held.name === '' && g.getAttribute('name') === null; })()"));
    CHECK(page->boolean("gw.name = 'leak'; gw.name === ''"));

    // A frame of another origin is named by the name it gives itself too, as
    // browsers name it, and not by an empty one.
    string_in(frame_realm_of(*page, "x"), "window.name = 'secret'; ''");
    CHECK(page->boolean("window.secret === xw && window.cross === undefined"));

    // A frame removed takes its frames' navigables with it: their windows are
    // closed at once, with no parent, top or name, as the frame's own is —
    // read in the same script, before any of their realms has ended.
    CHECK_EQ(page->string("(function () { var nw = n.contentWindow, iw = nw.inner; n.remove();"
                          " return [nw.closed, iw.parent === null, iw.top === null, iw.closed, iw.name].join(':'); })()"),
        "true:true:true:true:");
    CHECK_EQ(page->console, "");
    page.reset();
}

// A frame navigates. In a frame's realm the Location's setters, assign,
// replace and reload, and the window's location setter, navigate that frame,
// not the page: the URL is parsed against the base of the document whose
// script asked, and one that does not parse throws a SyntaxError DOMException
// of the Location's realm. A change of the fragment alone updates the URL at
// once and fires hashchange when the fragment differs, keeping the document;
// anything else reopens the frame after the script, with one load event at
// the iframe, whose src stays as it was. Once the frame has navigated
// elsewhere, setting src or srcdoc to the value it already has takes it back
// to what the attribute names; and of two changes in one script the later is
// the one navigated to. Script that navigates from inside the frame is run in
// the frame's own realm, where the entry and the incumbent realm are one.
void test_a_frame_navigates()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/sub/first.html"] = "<script>var which = 'first';</script>";
    documents["https://example.test/sub/second.html"] = "<script>var which = 'second';</script>";
    documents["https://example.test/sub/third.html"] = "<script>var which = 'third, against the frame';</script>";
    documents["https://example.test/dir/third.html"] = "<script>var which = 'third'; var hashes = [];"
                                                       " addEventListener('hashchange', function () { hashes.push(location.hash); });</script>";
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var loads = {}; document.addEventListener('load', function (e) { if (e.target.tagName === 'IFRAME') loads[e.target.id] = (loads[e.target.id] || 0) + 1; }, true);</script>
<iframe id=f src="/sub/first.html"></iframe>
<iframe id=g srcdoc="<script>var which = 'g';</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", hooks_serving(documents));
    page->load();
    page->eval("var f = document.getElementById('f'); var g = document.getElementById('g');");
    CHECK(page->boolean("loads.f === 1 && f.contentWindow.which === 'first' && loads.g === 1"));

    // From inside the frame, against its own document's URL: the same window
    // until the script is done, then a new one behind the same WindowProxy.
    page->eval("var first = f.contentWindow, firstArray = first.Array;");
    CHECK_EQ(string_in(frame_realm_of(*page, "f"), "location.href = 'second.html'; String(which)"), "first");
    CHECK(page->boolean("f.contentWindow === first && first.Array === firstArray && loads.f === 1"));
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow === first && first.Array !== firstArray && f.contentWindow.which === 'second' && loads.f === 2"));
    CHECK(page->boolean("f.contentDocument.URL === 'https://example.test/sub/second.html' && f.getAttribute('src') === '/sub/first.html'"));

    // From the page, against the page's URL.
    page->eval("var second = f.contentWindow, secondArray = second.Array; second.location.assign('third.html');");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow === second && second.Array !== secondArray && f.contentWindow.which === 'third' && loads.f === 3"));

    // A URL that does not parse throws, as the Location's realm's DOMException;
    // the page's own Location throws the same.
    CHECK(page->boolean("!URL.canParse('https://exa mple.test/') && (function () { try { f.contentWindow.location = 'https://exa mple.test/'; }"
                        " catch (e) { return e instanceof f.contentWindow.DOMException && e.name === 'SyntaxError' && e.code === 12; } return false; })()"));
    CHECK(page->boolean("(function () { try { location.href = 'https://exa mple.test/'; } catch (e) { return e instanceof DOMException && e.name === 'SyntaxError'; } return false; })()"));

    // The fragment alone: the URL at once, hashchange after, the same document.
    page->eval("var third = f.contentWindow, thirdArray = third.Array; third.location.hash = 'part'; var hashAtOnce = third.location.href; third.location.href = 'third.html#other';");
    CHECK_EQ(page->string("hashAtOnce"), "https://example.test/dir/third.html#part");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow === third && third.Array === thirdArray && loads.f === 3"));
    // One hashchange for each, both run after the script: each listener reads
    // the URL as it stands by then.
    CHECK_EQ(page->string("JSON.stringify(third.hashes)"), R"(["#other","#other"])");
    // The fragment the URL already has fires nothing, set as the hash or in
    // the whole URL.
    page->eval("third.location.hash = 'other'; third.location.hash = '#other'; third.location.href = 'third.html#other';");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow === third && third.Array === thirdArray && loads.f === 3"));
    CHECK_EQ(page->string("JSON.stringify(third.hashes)"), R"(["#other","#other"])");

    // A reload reopens the document at its URL.
    page->eval("third.location.reload();");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow === third && third.Array !== thirdArray && f.contentWindow.which === 'third' && loads.f === 4"));
    CHECK_EQ(page->string("f.contentWindow.location.href"), "https://example.test/dir/third.html#other");

    // replace from the page; the window's location setter from inside.
    page->eval("f.contentWindow.location.replace('/sub/first.html');");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow.which === 'first' && loads.f === 5"));
    string_in(frame_realm_of(*page, "f"), "window.location = 'second.html'; ''");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow.which === 'second' && loads.f === 6"));

    // Of two navigations asked in one script only the later happens: a
    // Location's and then a changed src; two changed srcdocs.
    page->eval("f.contentWindow.location.href = '/sub/second.html'; f.src = '/sub/third.html';");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow.which === 'third, against the frame' && loads.f === 7"));
    page->eval("g.srcdoc = \"<script>var which = 'g2';</script>\"; g.srcdoc = \"<script>var which = 'g3';</script>\";");
    page->realm->run_pending();
    CHECK(page->boolean("g.contentWindow.which === 'g3' && loads.g === 2"));

    // A frame its Location took elsewhere no longer shows what its attributes
    // name: setting src or srcdoc to the value it has brings that back.
    page->eval("f.contentWindow.location.href = '/sub/second.html'; g.contentWindow.location.href = '/sub/second.html';");
    page->realm->run_pending();
    CHECK(page->boolean("f.contentWindow.which === 'second' && loads.f === 8 && g.contentWindow.which === 'second' && loads.g === 3"));
    page->eval("f.setAttribute('src', f.getAttribute('src')); g.srcdoc = g.getAttribute('srcdoc');");
    page->realm->run_pending();
    CHECK_EQ(page->string("String(f.contentWindow.which)"), "third, against the frame");
    CHECK_EQ(page->string("String(g.contentWindow.which)"), "g3");
    CHECK(page->boolean("loads.f === 9 && loads.g === 4"));
    // A reload of an srcdoc document opens it anew, from the srcdoc.
    page->eval("var gShown = g.contentWindow, gArray = gShown.Array; gShown.location.reload();");
    page->realm->run_pending();
    CHECK(page->boolean("g.contentWindow === gShown && gShown.Array !== gArray && g.contentWindow.which === 'g3' && loads.g === 5"));
    // The same holds for a document made by a javascript: URL its Location
    // navigated to.
    page->eval("g.contentWindow.location.href = 'javascript:\"<p id=made>made</p>\"';");
    page->realm->run_pending();
    CHECK(page->boolean("g.contentDocument.getElementById('made') !== null && loads.g === 6"));
    page->eval("g.srcdoc = g.getAttribute('srcdoc');");
    page->realm->run_pending();
    CHECK(page->boolean("g.contentWindow.which === 'g3' && loads.g === 7"));
    CHECK(page->navigations.empty());
    CHECK_EQ(page->console, "");
    page.reset();

    // A page's own hash: its host navigates, but not to the fragment the URL
    // already has.
    auto hashed = std::make_unique<Page>("<!DOCTYPE html>", "https://example.test/dir/page.html#here");
    hashed->load();
    hashed->eval("location.hash = 'here'; location.hash = '#here';");
    CHECK(hashed->navigations.empty());
    hashed->eval("location.hash = 'there';");
    CHECK_EQ(hashed->navigations.size(), 1u);
    CHECK_EQ(hashed->console, "");
    hashed.reset();
}

// Every iframe has a window from its insertion on (HTML §4.8.5): the initial
// about:blank document, of its parent's origin, which the document its src
// or srcdoc names replaces behind the same WindowProxy, with that document's
// load alone. A frame whose document cannot be shown keeps its window: an
// empty error document of a new opaque origin for one the host has no answer
// for, and an empty document of its own origin for one that is not markup.
void test_every_iframe_has_a_window()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/sub/doc.html"] = "<script>var which = 'doc';</script>";
    bindings::HostHooks hooks = hooks_serving(documents);
    auto const serving = hooks.frame_document;
    hooks.frame_document = [serving](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors,
                               std::optional<net::Url> const& target) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const src = iframe.find_attribute("src");
        if (!target && src && src->value == "/sub/pic.png") {
            bindings::FrameDocument picture;
            picture.content_type = "image/png";
            picture.bytes = { 0x89, 'P', 'N', 'G' };
            picture.url = *net::parse_url("https://example.test/sub/pic.png");
            picture.origin = picture.url;
            return picture;
        }
        return serving(iframe, base, policy, ancestors, target);
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var loads = {}; document.addEventListener('load', function (e) { if (e.target.tagName === 'IFRAME') loads[e.target.id] = (loads[e.target.id] || 0) + 1; }, true);</script>
<iframe id=doc src="/sub/doc.html"></iframe>
<script>var doc = document.getElementById('doc'); var docAtParse = doc.contentWindow;
var docBlank = docAtParse !== null && doc.contentDocument.URL === 'about:blank' && loads.doc === undefined;</script>
<iframe id=missing src="/sub/missing.html"></iframe>
<iframe id=pic src="/sub/pic.png"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    page->eval("var missing = document.getElementById('missing'); var pic = document.getElementById('pic');"
               " function reach(w) { try { return typeof w.document; } catch (e) { return e.name; } }");
    // During the parse, the initial about:blank; then the document named.
    CHECK(page->boolean("docBlank"));
    CHECK(page->boolean("doc.contentWindow === docAtParse && docAtParse.which === 'doc' && loads.doc === 1"));
    // No answer: a window of another origin, and the load.
    CHECK(page->boolean("missing.contentWindow !== null && missing.contentDocument === null && loads.missing === 1"));
    CHECK_EQ(page->string("reach(missing.contentWindow)"), "SecurityError");
    // Not markup: an empty document of its own origin.
    CHECK(page->boolean("pic.contentDocument !== null && pic.contentDocument.URL === 'https://example.test/sub/pic.png' && pic.contentDocument.contentType === 'image/png'"
                        " && pic.contentDocument.body !== null && pic.contentDocument.body.childNodes.length === 0 && loads.pic === 1"));

    // Script-inserted: the window at once, the document after the script.
    page->eval("var late = document.createElement('iframe'); late.id = 'late'; late.src = '/sub/doc.html'; document.body.appendChild(late);"
               " var lateAtOnce = late.contentWindow; var lateBlank = lateAtOnce !== null && late.contentDocument.URL === 'about:blank' && loads.late === undefined;"
               " var gone = document.createElement('iframe'); gone.id = 'gone'; gone.src = '/sub/gone.html'; document.body.appendChild(gone);"
               " var goneAtOnce = gone.contentWindow;");
    CHECK(page->boolean("lateBlank && goneAtOnce !== null"));
    page->realm->run_pending();
    CHECK(page->boolean("late.contentWindow === lateAtOnce && lateAtOnce.which === 'doc' && loads.late === 1"));
    CHECK(page->boolean("gone.contentWindow !== null && gone.contentWindow === goneAtOnce && gone.contentDocument === null && loads.gone === 1"));
    // A src emptied before the frame navigates names about:blank, which the
    // frame navigates to, with a load, though it shows an about:blank already.
    page->eval("var emptied = document.createElement('iframe'); emptied.id = 'emptied'; emptied.src = '/sub/doc.html'; document.body.appendChild(emptied);"
               " var emptiedAtOnce = emptied.contentWindow; emptied.src = '';");
    page->realm->run_pending();
    CHECK(page->boolean("emptiedAtOnce !== null && emptied.contentWindow === emptiedAtOnce && emptied.contentDocument.URL === 'about:blank' && loads.emptied === 1"));

    // A navigation by Location to what the host cannot answer keeps the
    // window; setting the src it has brings its document back.
    page->eval("docAtParse.location.href = '/sub/nowhere.html';");
    page->realm->run_pending();
    CHECK(page->boolean("doc.contentWindow === docAtParse && doc.contentDocument === null && loads.doc === 2"));
    CHECK_EQ(page->string("reach(docAtParse)"), "SecurityError");
    page->eval("doc.setAttribute('src', doc.getAttribute('src'));");
    page->realm->run_pending();
    CHECK(page->boolean("doc.contentWindow === docAtParse && docAtParse.which === 'doc' && loads.doc === 3"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// A frame element is a navigable container as an iframe is (HTML §16.3.2):
// its window and initial about:blank document at insertion, then the document
// its src names; a window name taken from its name attribute at creation and
// not changed by the attribute after; no srcdoc. The parser gives one inside
// a frameset its initial document as it inserts it, so a frame that names
// nothing has loaded before DOMContentLoaded. Every check here answers false
// rather than throwing on an engine without frames.
void test_a_frame_has_a_window()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/sub/doc.html"] = "<script>var which = 'doc';</script>";
    auto page = std::make_unique<Page>("<!DOCTYPE html>", "https://example.test/dir/page.html", hooks_serving(documents));
    page->load();
    page->eval("var fr = document.createElement('frame'); fr.name = 'one'; fr.src = '/sub/doc.html'; document.body.appendChild(fr);"
               " var atOnce = fr.contentWindow; var blankAtOnce = atOnce != null && (fr.contentDocument || {}).URL === 'about:blank';");
    CHECK(page->boolean("blankAtOnce"));
    page->realm->run_pending();
    CHECK(page->boolean("atOnce != null && fr.contentWindow === atOnce && atOnce.which === 'doc' && atOnce.name === 'one'"
                        " && window.length === 1 && window[0] === atOnce && window.one === atOnce"));
    CHECK(page->boolean("typeof HTMLFrameElement === 'function' && fr instanceof HTMLFrameElement"
                        " && typeof HTMLFrameSetElement === 'function' && document.createElement('frameset') instanceof HTMLFrameSetElement"));
    // HTML's tabIndex getter answers 0 for a frame and an object as for an
    // iframe.
    CHECK(page->boolean("fr.tabIndex === 0 && document.createElement('object').tabIndex === 0"));
    // The name attribute names the navigable only as it is created.
    page->eval("fr.setAttribute('name', 'two');");
    CHECK_EQ(page->string("String(atOnce && atOnce.name)"), "one");
    // srcdoc is an iframe's alone: a frame with one shows about:blank.
    page->eval("var sd = document.createElement('frame'); sd.setAttribute('srcdoc', '<p id=x>'); document.body.appendChild(sd);");
    page->realm->run_pending();
    CHECK(page->boolean("sd.contentDocument != null && sd.contentDocument.URL === 'about:blank' && sd.contentDocument.getElementById('x') === null"));
    // So is sandbox: a frame with one runs its document's scripts, in its
    // parent's origin.
    page->eval("var sb = document.createElement('frame'); sb.setAttribute('sandbox', ''); sb.src = '/sub/doc.html'; document.body.appendChild(sb);");
    page->realm->run_pending();
    CHECK(page->boolean("sb.contentDocument != null && sb.contentWindow.which === 'doc'"));
    // A frame taken out of the tree takes its window with it.
    page->eval("var sbWindow = sb.contentWindow; sb.remove();");
    CHECK(page->boolean("sbWindow != null && sbWindow.closed === true && sb.contentWindow === null"));

    auto parsed = std::make_unique<Page>(
        R"HTML(<!DOCTYPE html><script>var log = [];</script><frameset><frame id=p src="/sub/doc.html" onload="log.push('p')"></frameset>)HTML",
        "https://example.test/dir/page.html", hooks_serving(documents));
    parsed->load();
    CHECK(parsed->boolean("(function () { var w = (document.getElementById('p') || {}).contentWindow;"
                          " return w != null && w.which === 'doc' && log.join() === 'p' && window.length === 1; })()"));
    // The order of loads: q, which names nothing, as the parser inserts it;
    // p, whose document is fetched, as the parse ends.
    auto ordered = std::make_unique<Page>(
        R"HTML(<!DOCTYPE html><script>var log = []; document.addEventListener('DOMContentLoaded', function () { log.push('dcl'); });</script>)HTML"
        R"HTML(<frameset><frame id=p src="/sub/doc.html" onload="log.push('p')"><frame id=q onload="log.push('q')"></frameset>)HTML",
        "https://example.test/dir/page.html", hooks_serving(documents));
    ordered->load();
    CHECK_EQ(ordered->string("log.join()"), "q,dcl,p");
    // A src written on a frame already in the tree navigates it to the
    // document the new URL names; the WindowProxy it had stays its own.
    documents["https://example.test/sub/other.html"] = "<script>var which = 'other';</script>";
    page->eval("fr.src = '/sub/other.html';");
    page->realm->run_pending();
    CHECK(page->boolean("atOnce != null && fr.contentWindow === atOnce && atOnce.which === 'other'"
                        " && (fr.contentDocument || {}).URL === 'https://example.test/sub/other.html'"));
    CHECK_EQ(page->console + parsed->console + ordered->console, "");
    ordered.reset();
    parsed.reset();
    page.reset();
}

// An object or an embed has no window at insertion. A task after it decides
// what the element represents (HTML §4.8.6 and §4.8.7): a window for a
// document the engine shows (markup, XML and SVG, text, JSON), named by the
// name attribute at that moment, with its load; an image for a picture, with
// a load; and otherwise the object's fallback or the embed's nothing. A
// document that cannot be had fires error at an object and load at an embed.
// The parse decides for the elements it inserted as it ends, in tree order,
// so that an object's fallback is settled before what is inside it: nothing
// inside an object that shows a document, a media element, or a subtree that
// is not rendered gets a window. Every check here answers false rather than
// throwing on an engine without them.
void test_objects_and_embeds_have_windows()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/sub/doc.html"] = "<script>var which = 'doc';</script>";
    documents["https://example.test/sub/two.html"] = "<script>var which = 'two';</script>";
    documents["https://example.test/sub/pic.svg"] = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    bindings::HostHooks hooks = hooks_serving(documents);
    auto const serving = hooks.frame_document;
    hooks.frame_document = [serving](dom::Element const& element, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const& ancestors,
                               std::optional<net::Url> const& target) -> std::optional<bindings::FrameDocument> {
        std::optional<bindings::FrameDocument> answer;
        if (target && target->serialize_path() == "/sub/pic.png") {
            answer = bindings::FrameDocument {};
            answer->content_type = "image/png";
            answer->bytes = { 0x89, 'P', 'N', 'G' };
            answer->url = *target;
            answer->origin = *target;
            return answer;
        }
        answer = serving(element, base, policy, ancestors, target);
        if (answer && target && target->serialize_path().ends_with(".svg"))
            answer->content_type = "image/svg+xml";
        return answer;
    };
    auto page = std::make_unique<Page>("<!DOCTYPE html>", "https://example.test/dir/page.html", hooks);
    page->load();
    page->eval("var ev = {}; function track(el, id) {"
               " el.onload = function () { ev[id + 'l'] = (ev[id + 'l'] || 0) + 1; };"
               " el.onerror = function () { ev[id + 'e'] = (ev[id + 'e'] || 0) + 1; }; }"
               " var o = document.createElement('object'); o.name = 'obj'; o.data = '/sub/doc.html'; track(o, 'o'); document.body.appendChild(o);"
               " var oAtOnce = o.contentWindow;"
               " var i = document.createElement('object'); i.data = '/sub/pic.png'; track(i, 'i'); document.body.appendChild(i);"
               " var f = document.createElement('object'); f.data = '/sub/missing.html'; track(f, 'f'); document.body.appendChild(f);"
               " var e = document.createElement('embed'); e.name = 'emb'; e.src = '/sub/doc.html'; track(e, 'e'); document.body.appendChild(e);"
               " var n = document.createElement('embed'); n.src = '/sub/missing.html'; track(n, 'n'); document.body.appendChild(n);"
               " var s = document.createElement('embed'); s.src = '/sub/pic.svg'; document.body.appendChild(s);");
    page->realm->run_pending();
    // A document: a window after the script, named as the element was then.
    CHECK(page->boolean("oAtOnce === null && o.contentWindow != null && o.contentWindow.which === 'doc' && o.contentWindow.name === 'obj' && ev.ol === 1 && !ev.oe"));
    CHECK(page->boolean("o.contentWindow != null && window.obj === o.contentWindow && o.contentWindow.frameElement === o"));
    // A picture: no window, a load.
    CHECK(page->boolean("i.contentWindow === null && ev.il === 1 && !ev.ie"));
    // Nothing to be had: an object's error, an embed's load.
    CHECK(page->boolean("f.contentWindow === null && ev.fe === 1 && !ev.fl"));
    CHECK(page->boolean("!!window.emb && window.emb.frameElement === e && window.emb.which === 'doc' && ev.el === 1"));
    CHECK(page->boolean("ev.nl === 1 && !ev.ne"));
    // SVG is a document too, which getSVGDocument answers for; a markup one
    // it does not.
    CHECK(page->boolean("window.length === 3 && typeof s.getSVGDocument === 'function' && s.getSVGDocument() !== null"
                        " && s.getSVGDocument().contentType === 'image/svg+xml' && o.getSVGDocument() === null"));
    // An iframe the page makes and inserts into the object's document has its
    // window there, though its getters are the page's.
    page->eval("var deep = document.createElement('iframe'); if (o.contentDocument) o.contentDocument.body.appendChild(deep);");
    CHECK(page->boolean("deep.contentWindow != null && deep.contentWindow.parent === o.contentWindow"));

    // Inside an object with nothing to show, an embed gets its window; inside
    // one that shows a document, none, and its name is the element's.
    auto nested = std::make_unique<Page>(R"HTML(<!DOCTYPE html><object><embed name=inner src="/sub/doc.html"></object>)HTML"
                                         R"HTML(<object data="/sub/doc.html"><embed name=hidden src="/sub/doc.html"></object>)HTML",
        "https://example.test/dir/page.html", hooks);
    nested->load();
    CHECK(nested->boolean("window.length === 2 && window.inner != null && window.inner.which === 'doc'"
                          " && window.hidden != null && window.hidden.tagName === 'EMBED'"));
    // Inside a media element: none.
    auto media = std::make_unique<Page>(R"HTML(<!DOCTYPE html><video><object data="/sub/doc.html"></object></video><object data="/sub/doc.html"></object>)HTML",
        "https://example.test/dir/page.html", hooks);
    media->load();
    CHECK(media->boolean("window.length === 1"));
    // Not rendered, under an ancestor with display: none: none, and no event;
    // a box of no size is rendered.
    css::ComputedStyle shown;
    shown.display = css::Display::Block;
    css::ComputedStyle gone = shown;
    gone.display = css::Display::None;
    bindings::HostHooks styled = hooks;
    styled.computed_style = [&shown, &gone](dom::Element const& element) -> css::ComputedStyle const* {
        dom::Attr const* const id = element.find_attribute("id");
        return id && id->value == "gone" ? &gone : &shown;
    };
    auto hidden = std::make_unique<Page>(R"HTML(<!DOCTYPE html><script>var ev = [];</script>)HTML"
                                         R"HTML(<div id=gone><object data="/sub/doc.html" onload="ev.push('h')" onerror="ev.push('he')"></object></div>)HTML"
                                         R"HTML(<embed id=z width=0 height=0 src="/sub/doc.html" onload="ev.push('z')">)HTML",
        "https://example.test/dir/page.html", std::move(styled));
    hidden->load();
    CHECK(hidden->boolean("window.length === 1 && window[0] != null && window[0].frameElement === document.getElementById('z') && ev.join() === 'z'"));
    // A parsed object whose data a script changes before the parse ends is
    // decided once, at the parse end, by what its data names then: one load,
    // the second document, and nothing left for the update the script queued.
    auto changed = std::make_unique<Page>(R"HTML(<!DOCTYPE html><script>var n = 0, e = 0;</script>)HTML"
                                          R"HTML(<object id=x data="/sub/doc.html" onload="n++" onerror="e++"></object>)HTML"
                                          R"HTML(<script>x.setAttribute('data', '/sub/two.html');</script>)HTML",
        "https://example.test/dir/page.html", hooks);
    changed->load();
    changed->realm->run_pending();
    CHECK(changed->boolean("n === 1 && e === 0 && x.contentWindow != null && x.contentWindow.which === 'two'"));
    // A blob: URL that URL.createObjectURL made names its Blob's document until
    // it is revoked (File API §8): each URL a new one, of the page's origin,
    // shown by an object and an embed alike; a revoked one names nothing, an
    // object's error; and only a Blob makes one.
    page->eval("var made = URL.createObjectURL(new Blob(['<script>var which = \"blob\";<\\/script>'], { type: 'text/html' }));"
               " var gone = URL.createObjectURL(new Blob(['<script>var which = \"gone\";<\\/script>'], { type: 'text/html' }));"
               " URL.revokeObjectURL(gone);"
               " var bo = document.createElement('object'); bo.data = made; track(bo, 'bo'); document.body.appendChild(bo);"
               " var go = document.createElement('object'); go.data = gone; track(go, 'go'); document.body.appendChild(go);"
               " var be = document.createElement('embed'); be.src = made; track(be, 'be'); document.body.appendChild(be);");
    page->realm->run_pending();
    CHECK(page->boolean("made !== gone && made.startsWith('blob:https://example.test/') && made.length === 'blob:https://example.test/'.length + 36"));
    CHECK(page->boolean("bo.contentWindow != null && bo.contentWindow.which === 'blob' && ev.bol === 1 && !ev.boe"));
    CHECK(page->boolean("ev.bel === 1 && window.length === 5 && window[4] != null && window[4].which === 'blob'"));
    CHECK(page->boolean("go.contentWindow === null && ev.goe === 1 && !ev.gol"));
    // A blob: URL goes when the document that made it unloads (File API §8,
    // HTML's unloading document cleanup steps): one a frame's document made
    // names nothing once the frame is removed.
    page->eval(R"JS(var maker = document.createElement('object');
        maker.data = URL.createObjectURL(new Blob(['<script>parent.fromFrame = URL.createObjectURL(new Blob(["<p>orphan</p>"], { type: "text/html" }));<\/script>'], { type: 'text/html' }));
        document.body.appendChild(maker);)JS");
    page->realm->run_pending();
    CHECK(page->boolean("typeof fromFrame === 'string' && fromFrame.startsWith('blob:https://example.test/')"));
    page->eval("maker.remove();");
    page->realm->run_pending();
    page->eval("var oo = document.createElement('object'); oo.data = fromFrame; track(oo, 'oo'); document.body.appendChild(oo);");
    page->realm->run_pending();
    CHECK(page->boolean("oo.contentWindow === null && ev.ooe === 1 && !ev.ool"));
    // A blob: URL's document nests no deeper than a fetched one: a document
    // that shows its own blob: URL again is refused once two of the documents
    // it would be inside have that URL, and one that makes a new URL each time
    // stops ten windows below the page. Each document stops itself at twenty.
    page->eval(R"JS(var deepest = { self: 0, fresh: 0 };
        var nester = function (kind) {
            return '<!DOCTYPE html><body><script>var d = 0, w = window; while (w !== w.parent && d < 30) { d++; w = w.parent; }'
                + ' w.deepest.' + kind + ' = Math.max(w.deepest.' + kind + ', d);'
                + ' if (d < 20) { var n = document.createElement("object");'
                + (kind === 'self' ? ' n.data = location.href;' : ' n.data = URL.createObjectURL(new Blob([w.nester("fresh")], { type: "text/html" }));')
                + ' document.body.appendChild(n); }</' + 'script></body>';
        };
        var ns = document.createElement('object'); ns.data = URL.createObjectURL(new Blob([nester('self')], { type: 'text/html' })); document.body.appendChild(ns);
        var nf = document.createElement('object'); nf.data = URL.createObjectURL(new Blob([nester('fresh')], { type: 'text/html' })); document.body.appendChild(nf);)JS");
    page->realm->run_pending();
    CHECK(page->boolean("deepest.self === 2"));
    CHECK(page->boolean("deepest.fresh === 10"));
    CHECK(page->boolean("(function () { try { URL.createObjectURL({}); return false; } catch (e) { return e instanceof TypeError; } })()"));
    // An object's useMap reflects its usemap attribute, which HTML's obsolete
    // features still give HTMLObjectElement.
    CHECK(page->boolean("o.useMap === '' && (o.setAttribute('usemap', '#m'), o.useMap === '#m')"
                        " && (o.useMap = '#n', o.getAttribute('usemap') === '#n')"));
    CHECK_EQ(page->console + nested->console + media->console + hidden->console + changed->console, "");
    changed.reset();
    hidden.reset();
    media.reset();
    nested.reset();
    page.reset();
}

// javascript: URLs in frames (HTML §7.4.2.2, "navigate to a javascript: URL").
// An iframe whose src is one gets the initial about:blank document at once;
// the script runs in that document's realm, after the script that inserted
// the iframe, or before the page's load for one the parser inserted. A string
// result replaces the frame's document with one parsed from it, at the old
// document's URL, with a load event; anything else leaves the document, with
// the load event of the initial insertion only. A frame navigated to one by its
// Location, or by a changed src, is treated the same, even when a script
// changes a parser-inserted iframe's src to one before the page is parsed.
// Only script of the origin of the frame's document runs one there: not the
// page's in a frame sandboxed into an opaque origin, nor in a frame of
// another origin.
void test_javascript_urls_in_frames()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/sub/first.html"] = "<script>var which = 'first';</script>";
    documents["https://other.test/doc.html"] = "<script>var which = 'other';</script>";
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var loads = {}; document.addEventListener('load', function (e) { if (e.target.tagName === 'IFRAME') loads[e.target.id] = (loads[e.target.id] || 0) + 1; }, true);</script>
<iframe id=p src="javascript:'<p id=parsed>parsed</p>'"></iframe>
<iframe id=n src="javascript:parent.parsedRan = (parent.parsedRan || 0) + 1"></iframe>
<iframe id=late src="/sub/first.html"></iframe>
<script>document.getElementById('late').src = "javascript:'<b id=js>js</b>'";</script>
<iframe id=boxed sandbox=allow-scripts src="javascript:'<p id=replaced>replaced</p>'"></iframe>
<iframe id=other src="https://other.test/doc.html"></iframe>
<script>var pageLoadSaw = null; onload = function () { pageLoadSaw = [loads.p, loads.n].join(); };</script>)HTML",
        "https://example.test/dir/page.html", hooks_serving(documents));
    page->load();
    page->eval("var p = document.getElementById('p'); var n = document.getElementById('n');");
    CHECK(page->boolean("pageLoadSaw === '1,1' && parsedRan === 1"));
    CHECK(page->boolean("(function (late) { return late.contentDocument !== null && late.contentDocument.getElementById('js') !== null && loads.late === 1; })(document.getElementById('late'))"));
    // The page's origin is not the opaque one of the sandboxed frame's
    // document, nor the other frame's: neither runs the page's URL.
    CHECK_EQ(string_in(frame_realm_of(*page, "boxed"), "document.getElementById('replaced') === null ? 'kept' : 'replaced'"), "kept");
    page->eval("document.getElementById('other').src = \"javascript:'<p id=replaced>replaced</p>'\";");
    page->realm->run_pending();
    CHECK_EQ(string_in(frame_realm_of(*page, "other"), "String(window.which) + (document.getElementById('replaced') === null ? ' kept' : ' replaced')"), "other kept");
    CHECK(page->boolean("p.contentDocument.getElementById('parsed').textContent === 'parsed' && p.contentDocument.URL === 'about:blank'"));
    CHECK(page->boolean("n.contentDocument.URL === 'about:blank' && n.contentDocument.body.childNodes.length === 0"));

    // Script-inserted: about:blank at once, the script after, in the frame.
    page->eval(R"JS(var s = document.createElement('iframe'); s.id = 's'; s.src = 'javascript:"<b id=made>" + (window !== parent) + "</b>"';
document.body.appendChild(s); var sBlank = s.contentWindow;
var atOnce = sBlank !== null && s.contentDocument.URL === 'about:blank' && loads.s === undefined;
var u = document.createElement('iframe'); u.id = 'u'; u.src = 'javascript:parent.insertedRan = true; 1'; document.body.appendChild(u);)JS");
    CHECK(page->boolean("atOnce"));
    page->realm->run_pending();
    CHECK(page->boolean("s.contentWindow === sBlank && s.contentDocument.getElementById('made').textContent === 'true' && s.contentDocument.URL === 'about:blank' && loads.s === 1"));
    CHECK(page->boolean("insertedRan === true && loads.u === 1 && u.contentDocument.body.childNodes.length === 0"));

    // Navigated to one: a string replaces the document; a number does not,
    // and fires no load; nor does a changed src to one.
    page->eval(R"JS(var sMade = s.contentWindow, madeArray = sMade.Array; sMade.location.href = 'javascript:"<i id=again>again</i>"'; var sameAfterScript = s.contentWindow === sMade && sMade.Array === madeArray;)JS");
    CHECK(page->boolean("sameAfterScript"));
    page->realm->run_pending();
    CHECK(page->boolean("s.contentWindow === sMade && sMade.Array !== madeArray && s.contentDocument.getElementById('again') !== null && loads.s === 2"));
    page->eval(R"JS(var sAgain = s.contentWindow, againArray = sAgain.Array; sAgain.location.href = 'javascript:parent.laterRan = true; 7'; u.src = 'javascript:parent.srcRan = (parent.srcRan || 0) + 1; 2';)JS");
    page->realm->run_pending();
    CHECK(page->boolean("laterRan === true && s.contentWindow === sAgain && sAgain.Array === againArray && s.contentDocument.getElementById('again') !== null && loads.s === 2"));
    CHECK(page->boolean("srcRan === 1 && loads.u === 1"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// A frame's first navigation away from its initial about:blank document, to
// a document same origin-domain with it, keeps the window (HTML §7.5.1,
// create and initialize a Document object): its expandos, its listeners and
// its intrinsics go on under the new document. The about:blank document stays
// alive, with no window; its timers are cleared as it unloads. A later
// navigation, or one to another origin or domain, makes a new window.
void test_a_frame_reuses_the_initial_about_blank_window()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/sub/same.html"] = "<script>var sawPersisted = window.persisted; addEventListener('load', function () { window.didLoadFrame = true; });</script>";
    documents["https://www.example.test/sub/same.html"] = "<script>var sawPersisted = window.persisted;</script>";
    documents["https://other.test/doc.html"] = "<script>var which = 'other';</script>";
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<script>var loads = {}; document.addEventListener('load', function (e) { if (e.target.tagName === 'IFRAME') loads[e.target.id] = (loads[e.target.id] || 0) + 1; }, true);</script>)HTML",
        "https://example.test/dir/page.html", hooks_serving(documents));
    page->load();

    // A src named at the insertion: the navigation the initial about:blank
    // document stood in for keeps its window.
    page->eval("var a = document.createElement('iframe'); a.id = 'a'; a.src = '/sub/same.html'; document.body.appendChild(a);"
               " var aWindow = a.contentWindow, aBlankDoc = a.contentDocument, aArray = aWindow.Array;"
               " aWindow.persisted = 'kept'; aWindow.pinged = 0; aWindow.addEventListener('ping', function () { aWindow.pinged++; });"
               " aWindow.setTimeout(function () { aWindow.blankTimerRan = true; }, 5);");
    CHECK_EQ(string_in(frame_realm_of(*page, "a"), "history.replaceState('s', ''); location.hash = 'x'; addEventListener('hashchange', function () { window.sawHashchange = true; });"
        " var inner = document.createElement('iframe'); document.body.appendChild(inner); window.innerWindow = inner.contentWindow; String(innerWindow !== null && history.state === 's')"),
        "true");
    // Past the timer's due time: only the navigation's unload keeps it from
    // running.
    page->clock = 2000;
    page->realm->run_pending();
    CHECK(page->boolean("a.contentWindow === aWindow && aWindow.persisted === 'kept' && aWindow.sawPersisted === 'kept' && aWindow.Array === aArray && aWindow.didLoadFrame === true && loads.a === 1"));
    CHECK(page->boolean("aWindow.dispatchEvent(new aWindow.Event('ping')) && aWindow.pinged === 1"));
    CHECK(page->boolean("(function () { try { return a.contentDocument !== aBlankDoc && aBlankDoc.defaultView === null && a.contentDocument.defaultView === aWindow"
                        " && aBlankDoc.createElement('p').ownerDocument === aBlankDoc; } catch (e) { return false; } })()"));
    CHECK(page->boolean("aWindow.blankTimerRan === undefined"));
    CHECK(page->boolean("aWindow.history.state === null"));
    CHECK(page->boolean("aWindow.sawHashchange === undefined"));
    CHECK(page->boolean("aWindow.innerWindow.closed === true"));

    // A src set after the insertion's load: still the initial about:blank
    // document, still its window. An iframe put into that document now has
    // no browsing context to be a frame of.
    page->eval("var b = document.createElement('iframe'); b.id = 'b'; document.body.appendChild(b);"
               " var bWindow = b.contentWindow, bBlankDoc = b.contentDocument; bWindow.persisted = 'b'; b.src = '/sub/same.html';");
    page->realm->run_pending();
    CHECK(page->boolean("b.contentWindow === bWindow && bWindow.persisted === 'b' && loads.b === 2"));
    CHECK_EQ(string_in(frame_realm_of(*page, "b"), "(function () { try { var d = parent.bBlankDoc, i = d.createElement('iframe'); d.body.appendChild(i);"
                                                   " return String(i.contentWindow === null); } catch (e) { return e.name; } })()"),
        "true");

    // A javascript: URL's string result is a navigation too; the document it
    // makes is not the initial about:blank one, so the next navigation makes
    // a new window.
    page->eval("var c = document.createElement('iframe'); c.id = 'c'; document.body.appendChild(c);"
               " var cWindow = c.contentWindow; cWindow.persisted = 'c'; cWindow.location.href = 'javascript:\"<p id=made>made</p>\"';");
    page->realm->run_pending();
    CHECK(page->boolean("c.contentDocument.getElementById('made') !== null && cWindow.persisted === 'c' && loads.c === 2"));
    page->eval("cWindow.second = 2; cWindow.location.href = '/sub/same.html';");
    page->realm->run_pending();
    CHECK(page->boolean("c.contentWindow === cWindow && cWindow.second === undefined && cWindow.didLoadFrame === true"));

    // Another origin: a new window.
    page->eval("var d = document.createElement('iframe'); d.id = 'd'; d.src = 'https://other.test/doc.html'; document.body.appendChild(d); d.contentWindow.persisted = 'd';");
    page->realm->run_pending();
    CHECK_EQ(string_in(frame_realm_of(*page, "d"), "String(window.persisted)"), "undefined");

    // The same origin, but the page set document.domain, which the initial
    // about:blank document shares and the fetched one does not: a new window.
    // An srcdoc document has the page's origin itself, domain and all, so it
    // keeps the window, and goes on sharing the page's domain.
    auto domained = std::make_unique<Page>("<!DOCTYPE html>", "https://www.example.test/dir/page.html", hooks_serving(documents));
    domained->load();
    domained->clock = 2000;
    domained->eval("var e = document.createElement('iframe'); e.id = 'e'; document.body.appendChild(e); e.contentWindow.persisted = 'e';"
                   " document.domain = document.domain; e.src = '/sub/same.html';");
    domained->realm->run_pending();
    CHECK_EQ(string_in(frame_realm_of(*domained, "e"), "String(window.persisted)"), "undefined");
    domained->eval("var e2 = document.createElement('iframe'); e2.id = 'e2'; document.body.appendChild(e2); e2.contentWindow.persisted = 'e2'; e2.srcdoc = '<p id=x>x</p>';");
    domained->realm->run_pending();
    CHECK(domained->boolean("e2.contentWindow.persisted === 'e2' && e2.contentDocument.getElementById('x') !== null"));
    domained->eval("document.domain = 'example.test';");
    CHECK(domained->boolean("e2.contentDocument !== null && e2.contentDocument.domain === 'example.test'"));
    CHECK_EQ(page->console, "");
    CHECK_EQ(domained->console, "");
    domained.reset();
    page.reset();
}

// The iframe's sandbox attribute (HTML §7.6.2), its tokens ASCII
// case-insensitive, read when the frame navigates rather than when the
// attribute changes. Without allow-scripts no script runs in the frame, a
// javascript: URL's included, nor in a frame inside it whatever that frame's
// own sandbox says; without allow-same-origin its document's origin is opaque
// — window.origin "null", localStorage and document.cookie refused with a
// SecurityError, frameElement and the page's contentDocument null, its
// requests made as the opaque origin's, without credentials — while the page
// and the frame still reach each other's window, as postMessage needs;
// without allow-top-navigation it may not navigate the top, though it may
// reload it; and sandboxed at all it may navigate itself and its own frames
// but not a sibling.
void test_the_sandbox_attribute()
{
    std::map<std::string, std::string> documents;
    documents["https://example.test/sub/first.html"] = "<script>var which = 'first';</script>";
    documents["https://example.test/sub/fetch-opaque.html"] = "<script>var x = new XMLHttpRequest(); x.open('GET', 'opaque.txt'); x.send();</script>";
    documents["https://example.test/sub/fetch-same.html"] = "<script>var x = new XMLHttpRequest(); x.open('GET', 'same.txt'); x.send();</script>";
    std::string const probe = "<body><script>var r = [origin]; try { localStorage.length; r.push('storage'); } catch (e) { r.push(e.name); }"
                              " try { document.cookie; r.push('cookie'); } catch (e) { r.push(e.name); } r.push(parent === window ? 'self' : 'parent', frameElement ? 'element' : 'none');"
                              " document.body.setAttribute('data-probe', r.join(' ')); parent.postMessage(origin, '*');</script>";
    std::string const markup = R"HTML(<!DOCTYPE html>
<script>var messages = []; addEventListener('message', function (e) { messages.push(e.data + ' ' + e.origin + ' ' + (e.source === document.getElementById(e.origin === 'null' ? 'opaque' : 'same').contentWindow)); });</script>
<iframe id=none sandbox srcdoc="<script>parent.noneRan = true;</script>"></iframe>
<iframe id=noscripts sandbox="allow-same-origin" srcdoc="<script>parent.noScriptsRan = true;</script>"></iframe>
<iframe id=jsurl sandbox="allow-same-origin" src="javascript:parent.jsUrlRan = true; 1"></iframe>
<iframe id=opaque sandbox="allow-scripts" srcdoc="PROBE"></iframe>
<iframe id=same sandbox="allow-scripts allow-same-origin" srcdoc="PROBE"></iframe>
<iframe id=upper sandbox=" ALLOW-SCRIPTS	Allow-Same-Origin " srcdoc="<script>parent.upperRan = true;</script>"></iframe>
<iframe id=long sandbox="allow-ſcripts allow-same-origin" srcdoc="<script>parent.longRan = true;</script>"></iframe>
<iframe id=top1 sandbox="allow-scripts allow-same-origin" srcdoc="<script>try { top.location.href = 'elsewhere.html'; parent.topResult = 'navigated'; } catch (e) { parent.topResult = e.name; }</script>"></iframe>
<iframe id=top2 sandbox="allow-scripts allow-same-origin allow-top-navigation" srcdoc="<script>top.location.href = 'allowed.html'; parent.topAllowed = true;</script>"></iframe>
<iframe id=reload sandbox="allow-scripts allow-same-origin" srcdoc="<script>try { top.location.reload(); parent.reloadResult = 'reloaded'; } catch (e) { parent.reloadResult = e.name; }</script>"></iframe>
<iframe id=sib srcdoc="<script>var which = 'sibling';</script>"></iframe>
<iframe id=nav sandbox="allow-scripts allow-same-origin" srcdoc="<script>function tryToNavigate(target) { try { target.location.href = '/sub/first.html'; return 'navigated'; } catch (e) { return e.name; } }</script><iframe id=kid srcdoc='<script>var which = 1</script>'></iframe>"></iframe>
<iframe id=outer sandbox="allow-same-origin" srcdoc="<iframe srcdoc='<script>document.title = 1</script>'></iframe><iframe sandbox='allow-scripts allow-same-origin' srcdoc='<script>document.title = 1</script>'></iframe>"></iframe>
<iframe id=open srcdoc="<iframe srcdoc='<script>document.title = 1</script>'></iframe>"></iframe>
<iframe id=fetchOpaque sandbox="allow-scripts" src="/sub/fetch-opaque.html"></iframe>
<iframe id=fetchSame sandbox="allow-scripts allow-same-origin" src="/sub/fetch-same.html"></iframe>
<iframe id=fetchSrcdoc srcdoc="<script>var x = new XMLHttpRequest(); x.open('GET', 'https://example.test/sub/srcdoc.txt'); x.send();</script>"></iframe>)HTML";
    std::string html;
    for (std::size_t at = 0; at < markup.size();) {
        std::size_t const found = markup.find("PROBE", at);
        html += markup.substr(at, found == std::string::npos ? std::string::npos : found - at);
        if (found == std::string::npos)
            break;
        html += probe;
        at = found + 5;
    }
    auto page = std::make_unique<Page>(html, "https://example.test/dir/page.html", hooks_serving(documents));
    for (std::string const name : { "opaque", "same", "srcdoc", "named" }) {
        net::FetchResponse response;
        response.status = 200;
        response.status_text = "OK";
        response.body.assign(name.begin(), name.end());
        // The opaque origin's request is a CORS one, which this lets through.
        if (name == "opaque")
            response.headers.push_back(net::Header { "Access-Control-Allow-Origin", "*" });
        // Allowed to the page's origin alone, which an opaque origin is not.
        if (name == "named")
            response.headers.push_back(net::Header { "Access-Control-Allow-Origin", "https://example.test" });
        page->responses["https://example.test/sub/" + name + ".txt"] = response;
    }
    page->load();
    page->realm->run_pending();
    // No script at all without allow-scripts.
    bindings::Realm* const none = frame_realm_of(*page, "none");
    CHECK(none != nullptr && none->stats().scripts_run == 0 && none->stats().scripts_refused == 1);
    CHECK(page->boolean("typeof noScriptsRan === 'undefined' && typeof jsUrlRan === 'undefined'"));
    // An opaque origin without allow-same-origin, the page's with it; either
    // way the frame reaches its parent, and the page the frame's window, but
    // only a document of its own origin.
    CHECK_EQ(string_in(frame_realm_of(*page, "opaque"), "document.body.getAttribute('data-probe')"), "null SecurityError SecurityError parent none");
    CHECK_EQ(string_in(frame_realm_of(*page, "same"), "document.body.getAttribute('data-probe')"), "https://example.test storage cookie parent element");
    CHECK(page->boolean("document.getElementById('opaque').contentWindow !== null && document.getElementById('opaque').contentDocument === null"));
    CHECK(page->boolean("document.getElementById('same').contentWindow !== null && document.getElementById('same').contentDocument !== null"));
    CHECK_EQ(page->string("messages.join()"), "null null true,https://example.test https://example.test true");
    // No script inside a frame without allow-scripts, whatever its own sandbox
    // attribute says; a frame inside an unsandboxed one runs its script.
    CHECK_EQ(page->string("JSON.stringify(Array.from(document.getElementById('outer').contentDocument.querySelectorAll('iframe'), function (i) { return i.contentDocument.title; })"
                          ".concat(document.getElementById('open').contentDocument.querySelector('iframe').contentDocument.title))"),
        R"(["","","1"])");
    // Requests as the opaque origin: no credentials, and Origin: null; with
    // the page's origin, or an srcdoc document's inherited one, credentials.
    auto const requested = [&page](std::string const& line) { return std::find(page->requests.begin(), page->requests.end(), line) != page->requests.end(); };
    CHECK(requested("GET https://example.test/sub/opaque.txt (no credentials)"));
    CHECK(requested("GET https://example.test/sub/same.txt"));
    CHECK(requested("GET https://example.test/sub/srcdoc.txt"));
    page->request_headers.clear();
    string_in(frame_realm_of(*page, "fetchOpaque"), "var y = new XMLHttpRequest(); y.open('GET', 'opaque.txt'); y.send(); ''");
    page->realm->run_pending();
    CHECK(std::find(page->request_headers.begin(), page->request_headers.end(), "Origin: null") != page->request_headers.end());
    // A response allowed to the page's origin alone is refused to the opaque one.
    string_in(frame_realm_of(*page, "fetchOpaque"), "var named = 'waiting'; var z = new XMLHttpRequest(); z.onload = function () { named = 'loaded'; };"
                                                    " z.onerror = function () { named = 'refused'; }; z.open('GET', 'named.txt'); z.send(); ''");
    page->realm->run_pending();
    CHECK_EQ(string_in(frame_realm_of(*page, "fetchOpaque"), "named"), "refused");
    CHECK_EQ(page->console, "warn:XMLHttpRequest https://example.test/sub/named.txt: the CORS check on https://example.test/sub/named.txt failed|");
    page->console.clear();
    // Tokens compare ASCII case-insensitively, and only so.
    CHECK(page->boolean("upperRan === true && typeof longRan === 'undefined'"));
    // The top, only with allow-top-navigation; a reload of it asks no sandbox.
    CHECK(page->boolean("topResult === 'SecurityError' && topAllowed === true"));
    CHECK_EQ(page->string("String(reloadResult)"), "reloaded");
    std::string navigated;
    for (net::Url const& url : page->navigations)
        navigated += url.serialize() + " ";
    CHECK_EQ(navigated, "https://example.test/dir/allowed.html https://example.test/dir/page.html ");
    // Its own frames, not a sibling.
    page->eval("var nav = document.getElementById('nav'); var sib = document.getElementById('sib'); var kid = nav.contentDocument.getElementById('kid');"
               " var toSibling = nav.contentWindow.tryToNavigate(sib.contentWindow); var toChild = nav.contentWindow.tryToNavigate(kid.contentWindow);");
    CHECK(page->boolean("toSibling === 'SecurityError' && toChild === 'navigated' && kid.contentWindow.which === 1"));
    page->realm->run_pending();
    CHECK(page->boolean("sib.contentWindow.which === 'sibling' && kid.contentWindow.which === 'first'"));
    // Itself.
    page->eval("var toItself = nav.contentWindow.tryToNavigate(nav.contentWindow);");
    CHECK(page->boolean("toItself === 'navigated'"));
    page->realm->run_pending();
    CHECK(page->boolean("nav.contentWindow.which === 'first'"));
    // A changed attribute waits for the next navigation.
    page->eval("var same = document.getElementById('same'); var sameWindow = same.contentWindow; same.sandbox = 'allow-scripts';");
    page->realm->run_pending();
    CHECK(page->boolean("same.contentWindow === sameWindow && sameWindow.origin === 'https://example.test'"));
    page->eval("same.srcdoc = same.getAttribute('srcdoc') + ' ';");
    page->realm->run_pending();
    CHECK(page->boolean("same.contentWindow === sameWindow && same.contentDocument === null"));
    CHECK_EQ(string_in(frame_realm_of(*page, "same"), "document.body.getAttribute('data-probe')"), "null SecurityError SecurityError parent none");
    // A frame sandboxed into an opaque origin reaches its parent and the top
    // as windows of another origin, whatever origin its URL has.
    CHECK_EQ(string_in(frame_realm_of(*page, "opaque"), "(function () { try { return typeof parent.document; } catch (e) { return e.name; } })()"), "SecurityError");
    CHECK_EQ(string_in(frame_realm_of(*page, "opaque"), "top === parent ? 'top' : 'self'"), "top");
    // Nor may the page run a javascript: URL in it through its Location.
    page->eval("document.getElementById('opaque').contentWindow.location.href = \"javascript:'<p id=injected>injected</p>'\";");
    page->realm->run_pending();
    CHECK_EQ(string_in(frame_realm_of(*page, "opaque"), "document.getElementById('injected') === null ? 'kept' : 'injected'"), "kept");
    // A sandboxed frame may not navigate a parent that is not the top.
    page->eval(R"JS(var nested = sib.contentDocument.createElement('iframe'); nested.setAttribute('sandbox', 'allow-scripts allow-same-origin');
nested.srcdoc = "<script>try { parent.location.href = '/sub/first.html'; parent.ancestorResult = 'navigated'; } catch (e) { parent.ancestorResult = e.name; }</script>";
sib.contentDocument.body.appendChild(nested);)JS");
    page->realm->run_pending();
    CHECK(page->boolean("sib.contentWindow.ancestorResult === 'SecurityError' && sib.contentWindow.which === 'sibling'"));
    CHECK_EQ(page->console, "");
    page.reset();

    // A sandboxed document may not set document.domain, even with its page's
    // origin; the same document unsandboxed may.
    auto domains = std::make_unique<Page>("<!DOCTYPE html><iframe id=boxed sandbox='allow-scripts allow-same-origin' src=/sub/first.html></iframe>"
                                          "<iframe id=open src=/sub/first.html></iframe>",
        "https://example.test/dir/page.html", hooks_serving(documents));
    domains->load();
    domains->realm->run_pending();
    std::string const set_domain = "(function () { try { document.domain = document.domain; return 'set ' + document.domain; } catch (e) { return e.name + ' ' + document.domain; } })()";
    CHECK_EQ(string_in(frame_realm_of(*domains, "boxed"), set_domain), "SecurityError example.test");
    CHECK_EQ(string_in(frame_realm_of(*domains, "open"), set_domain), "set example.test");
    CHECK_EQ(domains->console, "");
    domains.reset();

    // Under a file: page, whose URLs all serialize their origin as "null", a
    // sandbox's opaque origin is still of no URL.
    std::map<std::string, std::string> files;
    files["file:///sub/fetch-opaque.html"] = documents["https://example.test/sub/fetch-opaque.html"];
    auto local = std::make_unique<Page>("<!DOCTYPE html><iframe sandbox=allow-scripts src=/sub/fetch-opaque.html></iframe>", "file:///dir/page.html", hooks_serving(files));
    net::FetchResponse open_response;
    open_response.status = 200;
    open_response.status_text = "OK";
    open_response.headers.push_back(net::Header { "Access-Control-Allow-Origin", "*" });
    local->responses["file:///sub/opaque.txt"] = open_response;
    local->load();
    local->realm->run_pending();
    CHECK(std::find(local->requests.begin(), local->requests.end(), "GET file:///sub/opaque.txt (no credentials)") != local->requests.end());
    CHECK_EQ(local->console, "");
    local.reset();
}

// structuredClone (HTML §2.7): a value serialized into a record of its own
// and made again — the language's values, the wrappers of its primitives,
// dates, regular expressions, buffers and every view over them, the keyed
// collections in order, errors by their native type, arrays with holes and
// extra properties, objects by their own enumerable keys, blobs and files —
// with identity and cycles kept inside one clone; what cannot be cloned is a
// DataCloneError at the call, and a transferred buffer is detached.
void test_structured_clone_values()
{
    auto page = loaded("<!DOCTYPE html><p>clone</p>");
    CHECK(page->boolean("(function () { var c = structuredClone([-0, NaN, 12n, undefined, '\\uD800']);"
                        " return Object.is(c[0], -0) && Number.isNaN(c[1]) && c[2] === 12n && c.length === 5 && 3 in c && c[4] === '\\uD800'; })()"));
    CHECK(page->boolean("(function () { var c = structuredClone([new Boolean(false), new Number(-0), new String('x'), Object(5n)]);"
                        " return c[0] instanceof Boolean && c[0].valueOf() === false && Object.is(c[1].valueOf(), -0)"
                        " && c[2] instanceof String && c[2].valueOf() === 'x' && typeof c[3] === 'object' && c[3].valueOf() === 5n; })()"));
    CHECK(page->boolean("(function () { var r = /a+/gi; r.lastIndex = 3; var c = structuredClone({ when: new Date(86400000), re: r });"
                        " return c.when instanceof Date && c.when.getTime() === 86400000 && c.re instanceof RegExp && c.re !== r"
                        " && c.re.source === 'a+' && c.re.flags === 'gi' && c.re.lastIndex === 0; })()"));
    CHECK(page->boolean("(function () { var buffer = new ArrayBuffer(8, { maxByteLength: 16 }); var bytes = new Uint8Array(buffer); bytes[1] = 7;"
                        " var c = structuredClone({ bytes: bytes, view: new DataView(buffer, 2), floats: new Float64Array([1.5]) });"
                        " return c.bytes instanceof Uint8Array && c.bytes[1] === 7 && c.bytes.buffer === c.view.buffer && c.bytes.buffer !== buffer"
                        " && c.bytes.buffer.maxByteLength === 16 && c.view instanceof DataView && c.view.byteOffset === 2 && c.floats[0] === 1.5; })()"));
    CHECK(page->boolean("(function () { var c = structuredClone(new Map([[{ k: 1 }, 'a'], ['b', new Set([3, 2, 1])]]));"
                        " return c instanceof Map && JSON.stringify(Array.from(c.keys())) === '[{\"k\":1},\"b\"]'"
                        " && c.get('b') instanceof Set && Array.from(c.get('b')).join() === '3,2,1'; })()"));
    CHECK(page->boolean("(function () { var e = new RangeError('bad', { cause: 'why' }); e.extra = 1; var c = structuredClone(e);"
                        " return c instanceof RangeError && c.message === 'bad' && c.cause === 'why' && !('extra' in c)"
                        " && !Object.prototype.hasOwnProperty.call(structuredClone(new TypeError()), 'message'); })()"));
    CHECK(page->boolean("(function () { var shared = {}; var a = [1, , 3]; a.extra = 'x'; var o = { z: 1, a: a, s1: shared, s2: shared }; o.self = o;"
                        " var c = structuredClone(o);"
                        " return Object.keys(c).join() === 'z,a,s1,s2,self' && Array.isArray(c.a) && !(1 in c.a) && c.a.length === 3"
                        " && c.a.extra === 'x' && c.s1 === c.s2 && c.s1 !== shared && c.self === c; })()"));
    CHECK(page->boolean("(function () { var f = structuredClone(new File(['abc'], 'n.txt', { type: 'text/plain', lastModified: 42 }));"
                        " return f instanceof File && f.name === 'n.txt' && f.size === 3 && f.type === 'text/plain' && f.lastModified === 42; })()"));
    CHECK_EQ(page->string("(function () { function refused(v, t) { try { structuredClone(v, t ? { transfer: t } : undefined); }"
                          " catch (e) { return e instanceof DOMException && e.name === 'DataCloneError' && e.code === 25; } return false; }"
                          " var detached = new ArrayBuffer(1); structuredClone(detached, { transfer: [detached] }); var buffer = new ArrayBuffer(1);"
                          " return [refused(function () {}), refused(Symbol('s')), refused(new WeakMap()), refused(new Proxy({}, {})), refused(new Response()),"
                          " refused(detached), refused(buffer, [buffer, buffer]), refused(1, [new Blob()])].join(); })()"),
        "true,true,true,true,true,true,true,true");
    CHECK(page->boolean("(function () { var t = new Uint8Array([1, 2]).buffer; var moved = structuredClone({ t: t }, { transfer: [t] });"
                        " return t.byteLength === 0 && moved.t.byteLength === 2 && new Uint8Array(moved.t)[1] === 2; })()"));
    // A getter's throw is the clone's.
    CHECK(page->boolean("(function () { var thrown = new Error('mine'); try { structuredClone({ get x() { throw thrown; } }); } catch (e) { return e === thrown; } return false; })()"));
    CHECK_EQ(page->console, "");
}

// A clone is made in the realm the specification names: structuredClone's in
// its this's realm; window.postMessage's and a port's in the receiving
// window's, at delivery, after the sender's call has serialized it — so a
// value that cannot be cloned throws at the call, a transferred buffer is
// detached at once, and a transferred port arrives in the event's ports,
// entangled with the port that stayed; history's state is a clone taken
// before the URL is judged.
void test_structured_clone_across_realms()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<iframe id=f srcdoc="<script>var got = []; addEventListener('message', function (e) { got.push(e); });</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    page->eval("var frameWindow = document.getElementById('f').contentWindow;");
    CHECK(page->boolean("(function () { var made = frameWindow.structuredClone.call(window, new frameWindow.Array(1));"
                        " return made instanceof Array && !(made instanceof frameWindow.Array) && made.length === 1 && !(0 in made); })()"));
    CHECK(page->boolean("frameWindow.structuredClone(new Date(0)) instanceof frameWindow.Date"));
    page->eval("var sent = { list: [1] }; frameWindow.postMessage(sent, '*');"
               " var channel = new MessageChannel(); var buffer = new ArrayBuffer(4);"
               " frameWindow.postMessage({ buffer: buffer }, '*', [channel.port2, buffer]);"
               " var detachedAtOnce = buffer.byteLength === 0;"
               " var back = null; channel.port1.onmessage = function (m) { back = m.data; };");
    CHECK(page->boolean("(function () { try { frameWindow.postMessage(function () {}, '*'); } catch (e) { return e.name === 'DataCloneError'; } return false; })()"));
    CHECK(page->boolean("detachedAtOnce"));
    page->realm->run_pending();
    CHECK(page->boolean("frameWindow.got.length === 2 && frameWindow.got[0].data !== sent && frameWindow.got[0].data.list instanceof frameWindow.Array"
                        " && frameWindow.got[0].data.list[0] === 1"));
    CHECK(page->boolean("(function () { var e = frameWindow.got[1]; return e.ports.length === 1 && e.ports[0] instanceof frameWindow.MessagePort"
                        " && e.data.buffer instanceof frameWindow.ArrayBuffer && e.data.buffer.byteLength === 4; })()"));
    page->eval("frameWindow.got[1].ports[0].postMessage({ word: 'back' });");
    page->realm->run_pending();
    CHECK(page->boolean("back !== null && back.word === 'back' && back instanceof Object"));
    page->eval("var payload = { n: 1 }; var received = null; var second = new MessageChannel();"
               " second.port2.onmessage = function (e) { received = e.data; }; second.port1.postMessage(payload);");
    page->realm->run_pending();
    CHECK(page->boolean("received !== null && received !== payload && received.n === 1"));
    CHECK(page->boolean("(function () { var state = { deep: [1] }; history.pushState(state, ''); return history.state !== state && history.state.deep[0] === 1; })()"));
    CHECK(page->boolean("(function () { var before = history.state; try { history.pushState(function () {}, ''); } catch (e) {"
                        " return e.name === 'DataCloneError' && history.state === before; } return false; })()"));
    CHECK(page->boolean("(function () { var before = history.state; try { history.pushState({ x: 1 }, '', 'https://other.test/'); } catch (e) {"
                        " return e.name === 'SecurityError' && history.state === before; } return false; })()"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// A message keeps what it transfers. The transfer list stays alive while the
// rest of window.postMessage's options are converted, so a getter that drops
// the list's last reference and allocates frees nothing the call still reads;
// a transferred port that nothing else reaches lives on while its message
// waits in a task or in the queue of a port not yet started, and still
// carries a reply; the messages in a port's queue, one already in a task
// among them, go with the port when it is transferred, in their order; and a
// port may not transfer itself.
void test_a_message_keeps_what_it_transfers()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy* policy,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        if (policy)
            answer.policy = *policy;
        return answer;
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<iframe id=f srcdoc="<script>var got = []; addEventListener('message', function (e) { got.push(e.data); if (e.ports.length) e.ports[0].postMessage('reply to ' + e.data); });</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    page->eval("var frameWindow = document.getElementById('f').contentWindow;"
               " function junk() { var made = []; for (var i = 0; i < 50; ++i) made.push({ i: i }); return made.length; }");
    // The list's only reference is the generator's, or the array the getter
    // empties.
    page->eval("frameWindow.postMessage('generated', { transfer: (function* () { yield new ArrayBuffer(64); })(),"
               " get targetOrigin() { junk(); return '*'; } });"
               " var emptied = [new ArrayBuffer(64)];"
               " frameWindow.postMessage('emptied', { transfer: emptied, get targetOrigin() { emptied.length = 0; junk(); return '*'; } });");
    page->realm->run_pending();
    CHECK_EQ(page->string("frameWindow.got.join()"), "generated,emptied");
    // A port reached only through the window message carrying it.
    page->eval("(function () { var c = new MessageChannel(); c.port1.onmessage = function (e) { window.inFlight = e.data; };"
               " frameWindow.postMessage('in flight', '*', [c.port2]); })();");
    page->eval("junk();");
    page->realm->run_pending();
    page->realm->run_pending();
    CHECK(page->boolean("window.inFlight === 'reply to in flight'"));
    // A port reached only through the queue of a port that has not started.
    page->eval("(function () { var carrier = new MessageChannel(); window.carried = carrier.port2;"
               " var inner = new MessageChannel(); inner.port1.onmessage = function (e) { window.queued = e.data; };"
               " carrier.port1.postMessage('queued', [inner.port2]); })();");
    page->eval("junk();");
    page->eval("carried.onmessage = function (e) { e.ports[0].postMessage('reply to ' + e.data); };");
    page->realm->run_pending();
    page->realm->run_pending();
    CHECK(page->boolean("window.queued === 'reply to queued'"));
    // One message already in a task for the port, one sent once it has moved.
    page->eval("var order = []; var moving = new MessageChannel(); moving.port2.onmessage = function (e) { order.push('old ' + e.data); };"
               " moving.port1.postMessage('sent before the move');"
               " var moved = structuredClone(moving.port2, { transfer: [moving.port2] });"
               " moved.onmessage = function (e) { order.push('new ' + e.data); };"
               " moving.port1.postMessage('sent after the move');");
    page->realm->run_pending();
    page->realm->run_pending();
    CHECK_EQ(page->string("order.join()"), "new sent before the move,new sent after the move");
    // A port in its own transfer list: a DataCloneError, and the port still works.
    page->eval("var selfish = new MessageChannel(); var stillWorks = null; selfish.port2.onmessage = function (e) { stillWorks = e.data; };"
               " var refusedItself = (function () { try { selfish.port1.postMessage(0, [selfish.port1]); } catch (e) {"
               " return e instanceof DOMException && e.name === 'DataCloneError'; } return false; })();"
               " selfish.port1.postMessage('still');");
    page->realm->run_pending();
    CHECK(page->boolean("refusedItself"));
    CHECK(page->boolean("stillWorks === 'still'"));
    CHECK_EQ(page->console, "");
    page.reset();
}

// A platform object that is not serializable is a DataCloneError, whatever
// the bindings build it as, while an ordinary object made from an
// interface's prototype is cloned as the ordinary object it is. A delivered
// message's ports are a frozen array, the same one at every read. And
// window.postMessage picks its overload as WebIDL does, by the number of
// arguments: with three, the second is the target origin as a string and the
// third the transfer sequence; with two, an object, null or undefined is the
// options dictionary.
void test_platform_objects_and_message_arrivals()
{
    auto page = loaded("<!DOCTYPE html><p>platform</p>");
    CHECK_EQ(page->string("(function () { function clone(v) { try { structuredClone(v); return 'cloned'; } catch (e) { return e.name; } }"
                          " return [location, history, navigator, screen, document.implementation, new DOMParser(), new TextEncoder(),"
                          " document.createAttribute('a'), new AbortController(), new MessageChannel()].map(clone).join(); })()"),
        "DataCloneError,DataCloneError,DataCloneError,DataCloneError,DataCloneError,DataCloneError,DataCloneError,DataCloneError,DataCloneError,DataCloneError");
    CHECK(page->boolean("(function () { var c = structuredClone(Object.create(Location.prototype)); return Object.getPrototypeOf(c) === Object.prototype; })()"));
    CHECK_EQ(page->string("(function () { function outcome(f) { try { f(); return 'ok'; } catch (e) { return e.name; } }"
                          " return [outcome(function () { postMessage(1, { targetOrigin: '*' }, 5); }),"
                          " outcome(function () { postMessage(1, { targetOrigin: '*' }, []); }),"
                          " outcome(function () { postMessage(1, { targetOrigin: '*' }, undefined); }),"
                          " outcome(function () { postMessage(1, null); }), outcome(function () { postMessage(1, undefined); })].join(); })()"),
        "TypeError,SyntaxError,SyntaxError,ok,ok");
    page->realm->run_pending();
    page->eval("var arrivals = []; addEventListener('message', function (e) { arrivals.push(e); });"
               " postMessage('bare', '*'); var channel = new MessageChannel(); postMessage('with a port', '*', [channel.port1]);");
    page->realm->run_pending();
    CHECK(page->boolean("arrivals.length === 2 && arrivals[0].ports.length === 0 && arrivals[1].ports.length === 1"
                        " && arrivals.every(function (e) { return Array.isArray(e.ports) && Object.isFrozen(e.ports) && e.ports === e.ports; })"));
    CHECK_EQ(page->console, "");
}

// A frame's scripts measure the frame's own document, laid out at the size
// its container has in the page's layout: its viewport, its boxes and its
// computed styles are its own, not the page's and not nothing.
void test_a_frame_measures_itself()
{
    bindings::HostHooks hooks;
    hooks.viewport_width = 1280;
    hooks.viewport_height = 900;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy*,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        return answer;
    };
    // The page's layout, as its host would answer: the iframe's border box.
    hooks.layout_box = [](dom::Element const& element) -> std::optional<bindings::LayoutBox> {
        if (element.is_html("iframe"))
            return bindings::LayoutBox { 10, 20, 400, 580 };
        return std::nullopt;
    };
    Page page(R"HTML(<!DOCTYPE html>
<iframe id=f srcdoc="<!DOCTYPE html><style>body{margin:0} #w{padding:24px;height:66px;border:2px solid;font-size:20px}</style><div id=w><span id=s>x</span></div>
<script>var w = document.getElementById('w'), cs = getComputedStyle(w), root = document.documentElement;
window.facts = [innerWidth, innerHeight, root.clientWidth, root.clientHeight, w.offsetWidth, w.offsetHeight, w.clientWidth, w.clientHeight,
  w.clientTop, w.clientLeft, cs.width, cs.height, cs.paddingLeft, cs.fontSize, w.getBoundingClientRect().width, matchMedia('(max-width: 500px)').matches].join();</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page.load();
    CHECK_EQ(page.string("document.getElementById('f').contentWindow.facts"),
        std::string("400,580,400,580,400,118,396,114,2,2,348px,66px,24px,20px,400,true"));
    // The page's own window is still the page's.
    CHECK_EQ(page.string("[innerWidth, innerHeight, matchMedia('(max-width: 500px)').matches].join()"), std::string("1280,900,false"));
    // A change to the frame's document is measured afresh.
    CHECK_EQ(page.string("var inner = document.getElementById('f').contentWindow; inner.document.getElementById('w').style.height = '100px';"
                         " String(inner.document.getElementById('w').offsetHeight)"),
        std::string("152"));
}

// appearance computes to the keyword as written, the built-in sheet's `auto`
// on a button and the initial `none` elsewhere, under both spellings; and a
// style object finds the attribute of every property it supports on its
// prototype, never as its own.
void test_the_computed_appearance()
{
    bindings::HostHooks hooks;
    hooks.frame_document = [](dom::Element const& iframe, net::Url const& base, net::ContentSecurityPolicy*,
                               std::vector<bindings::FrameAncestor> const&, std::optional<net::Url> const&) -> std::optional<bindings::FrameDocument> {
        dom::Attr const* const srcdoc = iframe.find_attribute("srcdoc");
        if (!srcdoc)
            return std::nullopt;
        bindings::FrameDocument answer;
        answer.bytes.assign(srcdoc->value.begin(), srcdoc->value.end());
        answer.content_type = "text/html";
        answer.url = *net::parse_url("about:srcdoc");
        answer.origin = base;
        answer.srcdoc = true;
        return answer;
    };
    Page page(R"HTML(<!DOCTYPE html>
<iframe id=f srcdoc="<!DOCTYPE html><div id=d></div><button id=b>Go</button><button id=c style='appearance: Textfield'>x</button><button id=n style='-webkit-appearance: none'>y</button>
<script>function cs(id) { return getComputedStyle(document.getElementById(id)); } var s = cs('b');
window.facts = [s.appearance, s.webkitAppearance, s.getPropertyValue('-webkit-appearance'), cs('d').appearance, cs('c').appearance, cs('n').appearance].join();
window.names = ['color' in s, 'webkitAppearance' in s, 'WebkitAppearance' in s, 'Color' in s, 'fooBar' in s, s.hasOwnProperty('setProperty'), typeof s.setProperty].join();
var made = 0; for (var i = 0; i < 5000; ++i) if (('madeUp' + i) in s) ++made;
window.after = [made, 'color' in s, 'webkitAppearance' in s, 'madeUp1' in s].join();
var P = CSSStyleDeclaration.prototype, t = document.getElementById('d').style;
window.owner = [s.hasOwnProperty('color'), Object.getOwnPropertyDescriptor(s, 'color') === undefined, t.hasOwnProperty('backgroundColor'), P.hasOwnProperty('backgroundColor'), typeof Object.getOwnPropertyDescriptor(P, 'color').get, t.color = 'red', document.getElementById('d').getAttribute('style'), delete P.color, 'color' in t, 'color' in s].join();</script>"></iframe>)HTML",
        "https://example.test/dir/page.html", std::move(hooks));
    page.load();
    CHECK_EQ(page.string("document.getElementById('f').contentWindow.facts"), std::string("auto,auto,auto,none,textfield,none"));
    CHECK_EQ(page.string("document.getElementById('f').contentWindow.names"), std::string("true,true,true,false,false,false,function"));
    // More made-up names than the store of answers keeps: it starts again,
    // and the answers stay right.
    CHECK_EQ(page.string("document.getElementById('f').contentWindow.after"), std::string("0,true,true,false"));
    // The attributes are the prototype's accessors, as a browser has them,
    // never a style object's own; writing one sets the declaration, and a
    // deleted one is gone from every style object.
    CHECK_EQ(page.string("document.getElementById('f').contentWindow.owner"), std::string("false,true,false,true,function,red,color: red;,true,false,false"));
}

// A script runs once, and only in a document that has a window: a copy of one
// that has started has started too, and one put into a document made by
// `new Document()`, createHTMLDocument or a clone never runs — a page that
// clones itself into such a document must not run itself again inside it.
void test_scripts_that_must_not_run_again()
{
    Page page(R"HTML(<!DOCTYPE html><body><script id=first>window.runs = (window.runs || 0) + 1;</script><script>
      var made = new Document();
      made.appendChild(document.documentElement.cloneNode(true));
      var other = document.implementation.createHTMLDocument();
      var inside = document.createElement('script');
      inside.textContent = 'window.runs += 100';
      other.body.appendChild(inside);
      var copy = document.cloneNode(true);
      var again = document.getElementById('first').cloneNode(true);
      document.body.appendChild(again);
      var imported = document.importNode(document.getElementById('first'), true);
      document.body.appendChild(imported);
      var fresh = document.createElement('script');
      fresh.textContent = 'window.runs += 10';
      document.body.appendChild(fresh);
      // One that started where it could not run stays started.
      document.body.appendChild(inside);
    </script>)HTML");
    page.load();
    CHECK_EQ(page.number("runs"), 11.0);
    CHECK_EQ(page.string("String(made.querySelectorAll('script').length) + copy.querySelectorAll('script').length"), std::string("22"));
    CHECK_EQ(page.console, std::string(""));
}

// The Audio constructor (HTML §4.8.11) makes a preload="auto" element off
// the tree, its src content attribute set only when a caller gave one.
void test_the_audio_constructor()
{
    Page page("<!DOCTYPE html><body></body>");
    page.load();
    page.eval("var a = new Audio(); var b = new Audio('clip.mp3');");
    CHECK_EQ(page.string("a.tagName"), std::string("AUDIO"));
    CHECK_EQ(page.string("a.getAttribute('preload')"), std::string("auto"));
    CHECK(page.boolean("!a.hasAttribute('src')"));
    CHECK(page.boolean("a instanceof HTMLAudioElement && a instanceof HTMLMediaElement"));
    CHECK(page.boolean("Object.getPrototypeOf(a) === Audio.prototype"));
    CHECK_EQ(page.string("b.getAttribute('src')"), std::string("clip.mp3"));
    CHECK(page.boolean("typeof a.play === 'function' && typeof a.pause === 'function'"));
    CHECK(page.boolean("(function () { try { Audio(); return false; } catch (e) { return e instanceof TypeError; } })()"));
    CHECK_EQ(page.console, std::string(""));
}

// What the platform says is a promise is one, settled the way the engine can
// honestly settle it — never `undefined`, which a page calls .then() on.
void test_interfaces_that_promise()
{
    Page page("<!DOCTYPE html><body><p>x</p></body>");
    page.load();
    page.eval(R"JS(
        var out = [];
        function hex(buffer) { return Array.prototype.map.call(new Uint8Array(buffer), function (b) { return ('0' + b.toString(16)).slice(-2); }).join(''); }
        document.hasStorageAccess().then(function (v) { out.push('has ' + v); });
        document.requestStorageAccess().then(function (v) { out.push('request ' + v); });
        document.fonts.ready.then(function (f) { out.push('fonts ' + typeof f.check); });
        document.fonts.load('12px serif').then(function (faces) { out.push('load ' + faces.length); });
        document.createElement('video').play().catch(function (e) { out.push('play ' + e.name); });
        new Image().decode().catch(function (e) { out.push('decode ' + e.name); });
        document.exitFullscreen().catch(function (e) { out.push('exit ' + e.name); }); // nothing is full screen
        document.body.requestFullscreen().catch(function (e) { out.push('full ' + e.name); });
        out.push('when ' + (customElements.whenDefined('x-y') instanceof Promise));
        crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc')).then(function (b) { out.push(hex(b)); });
        crypto.subtle.digest({ name: 'sha-512' }, new Uint8Array(0)).then(function (b) { out.push('512 ' + b.byteLength + ' ' + (b instanceof ArrayBuffer)); });
        crypto.subtle.digest('SHA-384', new Uint8Array([1, 2, 3]).buffer).then(function (b) { out.push('384 ' + b.byteLength); });
        crypto.subtle.digest('SHA-1', new Uint8Array(0)).catch(function (e) { out.push('sha1 ' + e.name); });
        crypto.subtle.digest('SHA-256', 'not bytes').catch(function (e) { out.push('bytes ' + e.name); });
        crypto.subtle.sign().catch(function (e) { out.push('sign ' + e.name); });
    )JS");
    CHECK_EQ(page.string("out.join(' | ')"),
        std::string("when true | has true | request undefined | fonts function | load 0 | play NotSupportedError | decode EncodingError | exit TypeError"
                    " | full TypeError | ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad | 512 64 true | 384 48"
                    " | sha1 NotSupportedError | bytes TypeError | sign NotSupportedError"));
    CHECK_EQ(page.console, std::string(""));
}

} // namespace

// MutationObserver: what changed in a part of the tree, in one batch at the
// end of the turn. The component libraries the web is built with watch
// their own children with one, so an observer that takes registrations and
// says nothing leaves those components holding nothing.
void test_mutation_observer()
{
    auto page = loaded("<!DOCTYPE html><body><div id=box><span id=old>a</span></div><div id=other></div></body>");
    auto const pump = [&page] {
        for (int i = 0; i < 100 && page->realm->run_pending(); ++i) { }
    };
    page->eval(R"JS(
        var seen = [];
        var take = function () { var s = seen.join(' '); seen = []; return s; };
        var box = document.getElementById('box');
        var sameObserver = false;
        var mo = new MutationObserver(function (records, who) {
            sameObserver = who === mo;
            records.forEach(function (r) {
                seen.push(r.type + '|' + (r.attributeName || '-') + '|+' + r.addedNodes.length + '-' + r.removedNodes.length
                    + '|' + r.oldValue + '|' + (r.target === box ? 'box' : r.target.nodeName));
            });
        });
        mo.observe(box, { childList: true, attributes: true, characterData: true, subtree: true, characterDataOldValue: true });
        box.appendChild(document.createElement('b'));
        box.setAttribute('data-x', '1');
        document.getElementById('old').firstChild.data = 'z';
        // Nothing is delivered while the script that changed the tree is
        // still running: the batch comes at the checkpoint after it.
        var duringScript = seen.length;
    )JS");
    CHECK_EQ(page->string("duringScript + ''"), "0");
    pump();
    CHECK_EQ(page->string("take()"),
        "childList|-|+1-0|null|box attributes|data-x|+0-0|null|box characterData|-|+0-0|a|#text");
    CHECK(page->boolean("sameObserver"));

    // A record says where in the children the change was.
    page->eval("box.insertBefore(document.createElement('i'), box.firstChild);");
    pump();
    CHECK_EQ(page->string("take()"), "childList|-|+1-0|null|box");
    // Taking a node out of the tree is one record, and putting it in
    // another parent is two: it left one and joined the other.
    page->eval("var moved = box.firstChild; document.getElementById('other').appendChild(moved);");
    pump();
    CHECK_EQ(page->string("take()"), "childList|-|+0-1|null|box");

    // What it was not asked to watch, it does not report.
    page->eval(R"JS(
        var quiet = [];
        var narrow = new MutationObserver(function (r) { r.forEach(function (x) { quiet.push(x.type); }); });
        narrow.observe(box, { attributes: true, attributeFilter: ['data-y'] });
        box.setAttribute('data-x', '2');
        box.setAttribute('data-y', '3');
        box.appendChild(document.createElement('u'));
    )JS");
    pump();
    CHECK_EQ(page->string("quiet.join(',')"), "attributes");
    CHECK_EQ(page->string("take()"), "attributes|data-x|+0-0|null|box attributes|data-y|+0-0|null|box childList|-|+1-0|null|box");

    // Asked for, an attribute record carries the value before the change:
    // null for one just added, the old text for a change and for a removal.
    page->eval(R"JS(
        var olds = [];
        var withOld = new MutationObserver(function (r) { r.forEach(function (x) { olds.push(x.attributeName + '=' + x.oldValue); }); });
        withOld.observe(box, { attributes: true, attributeOldValue: true, attributeFilter: ['data-w'] });
        box.setAttribute('data-w', 'one');
        box.setAttribute('data-w', 'two');
        box.removeAttribute('data-w');
    )JS");
    pump();
    CHECK_EQ(page->string("olds.join(',')"), "data-w=null,data-w=one,data-w=two");
    page->eval("withOld.disconnect(); take();");

    // One that did not ask for the subtree hears only of its own node.
    page->eval(R"JS(
        var shallow = [];
        var near = new MutationObserver(function (r) { r.forEach(function (x) { shallow.push(x.type + ':' + x.target.nodeName); }); });
        near.observe(box, { childList: true });
        box.firstChild.appendChild(document.createElement('tt'));
        box.appendChild(document.createElement('kbd'));
    )JS");
    pump();
    CHECK_EQ(page->string("shallow.join(',')"), "childList:DIV");
    page->eval("near.disconnect(); take();");

    // Naming only a filter asks for attributes as well: the option counts
    // as spoken for, which is what keeps this from throwing.
    page->eval(R"JS(
        var filtered = [];
        var threw = '';
        var only = new MutationObserver(function (r) { r.forEach(function (x) { filtered.push(x.attributeName); }); });
        try { only.observe(box, { attributeFilter: ['data-z'] }); } catch (e) { threw = e.name; }
        box.setAttribute('data-q', '1');
        box.setAttribute('data-z', '2');
    )JS");
    pump();
    CHECK_EQ(page->string("threw + '/' + filtered.join(',')"), "/data-z");
    page->eval("only.disconnect(); take();");

    // takeRecords hands over what is waiting and leaves nothing behind;
    // disconnect ends it.
    page->eval("box.appendChild(document.createElement('em')); var taken = mo.takeRecords();");
    CHECK_EQ(page->string("taken.length + ' ' + taken[0].type + ' ' + mo.takeRecords().length"), "1 childList 0");
    pump();
    CHECK_EQ(page->string("take()"), "");
    page->eval("mo.disconnect(); box.appendChild(document.createElement('s'));");
    pump();
    CHECK_EQ(page->string("take()"), "");

    // The options must ask for something, and the target must be a node.
    CHECK_EQ(page->string("var r = ''; try { new MutationObserver(function () {}).observe(box, {}); } catch (e) { r = e.name; } r"), "TypeError");
    CHECK_EQ(page->string("var r = ''; try { new MutationObserver(function () {}).observe(7, { childList: true }); } catch (e) { r = e.name; } r"), "TypeError");
    CHECK_EQ(page->string("var r = ''; try { new MutationObserver(1); } catch (e) { r = e.name; } r"), "TypeError");
}

// IntersectionObserver with geometry the test decides: each element's box by
// its id, an 800 by 600 viewport, and a scroll position the test moves. The
// entries come only when a target crosses a threshold or goes in or out of
// view, and a page checking for the interface finds all of it.
void test_intersection_observer()
{
    std::map<std::string, bindings::LayoutBox> boxes;
    boxes["top"] = { 0, 100, 200, 100 }; // wholly in view
    boxes["half"] = { 0, 550, 200, 100 }; // half in view: 550 to 650 against 0 to 600
    boxes["below"] = { 0, 2000, 200, 100 }; // out of view until the page scrolls
    std::pair<int, int> scroll { 0, 0 };
    bindings::HostHooks hooks;
    hooks.layout_box = [&boxes](dom::Element const& element) -> std::optional<bindings::LayoutBox> {
        dom::Attr const* const id = element.find_attribute("id");
        if (id == nullptr)
            return std::nullopt;
        auto const it = boxes.find(id->value);
        if (it == boxes.end())
            return std::nullopt;
        return it->second;
    };
    hooks.scroll_position = [&scroll](dom::Document const&) { return scroll; };
    hooks.viewport_width = 800;
    hooks.viewport_height = 600;
    auto page = std::make_unique<Page>(
        "<!DOCTYPE html><body><div id=top></div><div id=half></div><div id=below></div><div id=none></div></body>",
        "https://example.test/dir/page.html", std::move(hooks));
    page->load();
    auto const pump = [&page] {
        for (int i = 0; i < 10 && page->realm->run_pending(); ++i) { }
    };
    page->eval(R"JS(
        var log = [];
        var take = function () { var s = log.join(' '); log = []; return s; };
        var io = new IntersectionObserver(function (entries, observer) {
            entries.forEach(function (e) { log.push(e.target.id + ':' + e.isIntersecting + ':' + e.intersectionRatio); });
        }, { threshold: [0, 0.5, 1] });
        ['top', 'half', 'below', 'none'].forEach(function (id) { io.observe(document.getElementById(id)); });
    )JS");
    pump();
    CHECK_EQ(page->string("take()"), "top:true:1 half:true:0.5 below:false:0 none:false:0");
    // Nothing has moved, so there is nothing more to say.
    page->clock += 1000;
    pump();
    CHECK_EQ(page->string("take()"), "");
    // Scrolling takes two out of view and brings the third in.
    scroll = { 0, 1500 };
    page->clock += 50;
    pump();
    CHECK_EQ(page->string("take()"), "top:false:0 half:false:0 below:true:1");
    // The entry's rectangles are in the viewport's coordinates.
    page->eval(R"JS(
        var rects = '';
        var measuring = new IntersectionObserver(function (entries) {
            var e = entries[0];
            rects = [e.boundingClientRect.y, e.intersectionRect.height, e.rootBounds.height, typeof e.time].join(',');
        });
        measuring.observe(document.getElementById('below'));
    )JS");
    pump();
    CHECK_EQ(page->string("rects"), "500,100,600,number");
    // rootMargin grows the root: a margin of 1500px at the bottom brings the
    // first one back into what the second observer calls view.
    page->eval(R"JS(
        var near = '';
        var ahead = new IntersectionObserver(function (entries) { near = entries.map(function (e) { return e.target.id + ':' + e.isIntersecting; }).join(' '); },
            { rootMargin: '1500px 0px 0px 0px' });
        ahead.observe(document.getElementById('top'));
    )JS");
    pump();
    CHECK_EQ(page->string("near"), "top:true");
    // takeRecords hands over what is waiting; unobserve and disconnect end it.
    page->eval("io.unobserve(document.getElementById('below')); ahead.disconnect(); measuring.disconnect();");
    scroll = { 0, 0 };
    page->clock += 50;
    pump();
    CHECK_EQ(page->string("take()"), "top:true:1 half:true:0.5");
    // Crossing a threshold while staying in view is news too; and a box that
    // moves with no change to the tree (a picture arriving above it) is seen
    // on the quarter-second refresh.
    boxes["half"] = { 0, 575, 200, 100 };
    page->clock += 300;
    pump();
    CHECK_EQ(page->string("take()"), "half:true:0.25");

    // The interface as a page checks for it before deciding to bring its own.
    CHECK(page->boolean("'intersectionRatio' in IntersectionObserverEntry.prototype && 'isIntersecting' in IntersectionObserverEntry.prototype"));
    CHECK_EQ(page->string("new IntersectionObserver(function () {}, { rootMargin: '10px 5%' }).rootMargin"), "10px 5% 10px 5%");
    CHECK_EQ(page->string("new IntersectionObserver(function () {}).rootMargin"), "0px 0px 0px 0px");
    CHECK_EQ(page->string("new IntersectionObserver(function () {}, { threshold: [1, 0, 0.5] }).thresholds.join()"), "0,0.5,1");
    CHECK(page->boolean("new IntersectionObserver(function () {}).root === null"));
    CHECK(page->throws("new IntersectionObserver(function () {}, { threshold: 2 })").starts_with("RangeError"));
    CHECK(page->throws("new IntersectionObserver(function () {}, { rootMargin: '10' })").starts_with("SyntaxError"));
    CHECK(page->throws("new IntersectionObserver(function () {}, { root: {} })").starts_with("TypeError"));
    CHECK(page->throws("new IntersectionObserver(function () {}).observe({})").starts_with("TypeError"));
    CHECK_EQ(page->realm->stats().uncaught_errors, 0);
}

// Custom elements: a page names a class of its own and the tree becomes it.
// Almost every page built out of components rests on this, so what is
// checked here is the whole of what one needs — the element already in the
// markup becoming an instance, the callbacks as it arrives, changes and
// leaves, and the ways a page can get it wrong.
void test_custom_elements()
{
    auto page = loaded(R"HTML(<!DOCTYPE html><body><my-card title=first>light</my-card><div id=host></div></body>)HTML");
    auto const pump = [&page] {
        for (int i = 0; i < 100 && page->realm->run_pending(); ++i) { }
    };
    page->eval(R"JS(
        var log = [];
        var take = function () { var s = log.join(' '); log = []; return s; };
        var host = document.getElementById('host');
        class MyCard extends HTMLElement {
            static get observedAttributes() { return ['title']; }
            constructor() { super(); this.built = true; log.push('made'); }
            connectedCallback() { log.push('in:' + this.getAttribute('title')); this.textContent = 'CARD'; }
            disconnectedCallback() { log.push('out'); }
            attributeChangedCallback(name, was, now) { log.push('attr:' + name + ':' + was + ':' + now); }
        }
    )JS");
    // Nothing happens to it until the name is defined.
    CHECK_EQ(page->string("document.querySelector('my-card').textContent + ' ' + take()"), "light ");
    CHECK(page->boolean("document.querySelector('my-card').built === undefined"));

    // Defining it upgrades what is already there: the constructor, then the
    // attributes it watches, then the document it is already in.
    page->eval("customElements.define('my-card', MyCard);");
    CHECK_EQ(page->string("take()"), "made attr:title:null:first in:first");
    CHECK(page->boolean("document.querySelector('my-card') instanceof MyCard"));
    CHECK(page->boolean("document.querySelector('my-card').built === true"));
    CHECK_EQ(page->string("document.querySelector('my-card').textContent"), "CARD");
    CHECK_EQ(page->string("document.querySelector('my-card').constructor.name"), "MyCard");

    // An element the document makes is already one of them, and is in no
    // tree until something puts it in one.
    page->eval("var made = document.createElement('my-card');");
    CHECK_EQ(page->string("(made instanceof MyCard) + ' ' + made.localName + ' ' + made.isConnected + ' ' + take()"), "true my-card false made");
    page->eval("made.setAttribute('title', 'second'); host.appendChild(made);");
    CHECK_EQ(page->string("take()"), "attr:title:null:second in:second");
    CHECK_EQ(page->string("made.textContent"), "CARD");
    // A change says what the value was, and so does a removal: a component
    // that compares the two ignores a change that reports none, which is how
    // Polymer's disable-upgrade kept a whole application asleep.
    page->eval("made.setAttribute('title', 'third'); made.removeAttribute('title');");
    CHECK_EQ(page->string("take()"), "attr:title:second:third attr:title:third:null");
    // An attribute it does not watch says nothing; leaving the tree does.
    page->eval("made.setAttribute('hidden', ''); made.remove();");
    CHECK_EQ(page->string("take() + ' ' + made.isConnected"), "out false");
    // Made by the class itself, it is an element like any other.
    page->eval("var byHand = new MyCard(); host.appendChild(byHand);");
    CHECK_EQ(page->string("byHand.localName + ' ' + (byHand.parentNode === host) + ' ' + take()"), "my-card true made in:null");

    // A definition that arrives after the element does upgrades it where it
    // stands, callbacks and all.
    page->eval(R"JS(
        host.appendChild(document.createElement('my-late'));
        class Late extends HTMLElement { connectedCallback() { log.push('late in'); } }
        customElements.define('my-late', Late);
    )JS");
    CHECK_EQ(page->string("take()"), "late in");
    CHECK(page->boolean("host.querySelector('my-late') instanceof Late"));

    // A template's contents cloned into the document are upgraded as the
    // clone is made, before anything inserts them: a framework stamps a
    // template and sets data on what it stamped, and must be setting it on
    // the class. The template's own contents, in a document with no
    // registry, stay as they are.
    page->eval(R"JS(
        var template = document.createElement('template');
        template.innerHTML = '<div><my-card title=stamped></my-card></div>';
        var stamped = document.importNode(template.content, true);
        var inStamp = stamped.firstChild.firstChild;
    )JS");
    CHECK_EQ(page->string("(inStamp instanceof MyCard) + ' ' + inStamp.built + ' ' + inStamp.isConnected + ' ' + take()"),
        "true true false made attr:title:null:stamped");
    CHECK(page->boolean("!(template.content.firstChild.firstChild instanceof MyCard)"));
    page->eval("host.appendChild(stamped);");
    CHECK_EQ(page->string("take()"), "in:stamped");
    // cloneNode makes an upgraded copy too, connected or not.
    page->eval("var copy = inStamp.cloneNode(false);");
    CHECK_EQ(page->string("(copy instanceof MyCard) + ' ' + copy.isConnected + ' ' + take()"), "true false made attr:title:null:stamped");

    // The registry answers for what it holds.
    CHECK(page->boolean("customElements.get('my-card') === MyCard"));
    CHECK(page->boolean("customElements.get('my-nothing') === undefined"));
    CHECK_EQ(page->string("customElements.getName(MyCard)"), "my-card");
    page->eval("customElements.whenDefined('my-card').then(function (c) { log.push('when:' + (c === MyCard)); });");
    pump();
    CHECK_EQ(page->string("take()"), "when:true");

    // The ways a page can get it wrong.
    CHECK_EQ(page->string("var r = ''; try { customElements.define('nodash', MyCard); } catch (e) { r = e.name; } r"), "SyntaxError");
    CHECK_EQ(page->string("var r = ''; try { customElements.define('my-card', class extends HTMLElement {}); } catch (e) { r = e.name; } r"), "NotSupportedError");
    CHECK_EQ(page->string("var r = ''; try { customElements.define('my-again', MyCard); } catch (e) { r = e.name; } r"), "NotSupportedError");
    CHECK_EQ(page->string("var r = ''; try { customElements.define('my-thing', {}); } catch (e) { r = e.name; } r"), "TypeError");
    CHECK_EQ(page->string("var r = ''; try { new HTMLElement(); } catch (e) { r = e.name; } r"), "TypeError");

    // A constructor that throws leaves the element as it was and the page
    // carries on: it is reported, not fatal, and never tried again.
    page->eval(R"JS(
        host.appendChild(document.createElement('my-bad'));
        class Bad extends HTMLElement { constructor() { super(); throw new Error('no'); } }
        customElements.define('my-bad', Bad);
    )JS");
    CHECK(page->console.find("no") != std::string::npos);
    // Its super() had already run, so it wears the class's prototype, as it
    // does in a browser; what it never gets is the callbacks, nor a second
    // attempt at its constructor.
    CHECK(page->boolean("host.querySelector('my-bad') instanceof Bad"));
    CHECK_EQ(page->string("host.querySelector('my-bad').localName + ' ' + take()"), "my-bad ");
    page->eval("host.querySelector('my-bad').remove();");
    CHECK_EQ(page->string("take()"), "");
}

// A MediaSource's sound, decoded as it plays: a real Opus stream (a second
// of a 440 Hz sine, made by ffmpeg in CELT alone) appended by the page,
// played into a device that lets the test listen to every sample it was
// given. The device hears at half the page clock's pace, so that the
// element's time — which is what has been heard, as a browser's is its
// sound card's — cannot be mistaken for the page clock's.
void test_media_source_plays_its_sound()
{
    std::filesystem::path const fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / "tiny-opus-celt.webm";
    std::ifstream in(fixture, std::ios::binary);
    std::vector<std::uint8_t> const opus_stream { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    CHECK(opus_stream.size() > 1000);

    std::vector<float> heard;
    int devices = 0;
    double* clock = nullptr;
    bindings::HostHooks hooks;
    hooks.open_audio = [&heard, &devices, &clock](platform::AudioFormat const& format, std::string&) {
        devices++;
        return platform::open_virtual_audio(format, [&clock] { return *clock / 2; },
            [&heard](std::span<float const> samples) { heard.insert(heard.end(), samples.begin(), samples.end()); });
    };
    auto page = std::make_unique<Page>("<!DOCTYPE html><body><audio id=a></audio></body>", "https://example.test/dir/page.html", std::move(hooks));
    clock = &page->clock;
    page->load();
    auto const pump = [&page] {
        for (int i = 0; i < 100 && page->realm->run_pending(); ++i) { }
    };
    page->eval("var OI = '" + base64_encode(opus_stream) + "';");
    page->eval(R"JS(
        var log = [];
        var a = document.getElementById('a'), ms = new MediaSource();
        ['playing', 'ended', 'seeked'].forEach(function (t) { a.addEventListener(t, function () { log.push(t); }); });
        ms.addEventListener('sourceopen', function () {
            var sb = ms.addSourceBuffer('audio/webm; codecs="opus"');
            sb.addEventListener('updateend', function () { ms.endOfStream(); log.push('appended'); }, { once: true });
            sb.appendBuffer(Uint8Array.from(atob(OI), function (c) { return c.charCodeAt(0); }));
        }, { once: true });
        a.src = URL.createObjectURL(ms);
    )JS");
    pump();
    CHECK_EQ(page->string("log.join(' ') + ' ' + a.readyState"), "appended 4");
    page->eval("a.play();");
    pump();
    CHECK_EQ(page->string("log.join(' ')"), "appended playing");
    page->eval("log = [];");
    page->clock += 500;
    pump();
    // A quarter of a second heard in half a second of the page's: the time
    // is where the sound is, and the device was given the whole second at
    // once, as far ahead as it takes.
    CHECK_EQ(devices, 1);
    CHECK_EQ(page->number("Math.round(a.currentTime * 1000)"), 250);
    CHECK(heard.size() >= 48000u * 2u * 9u / 10u);
    // What it was given is the tone: 440 cycles a second cross zero 880
    // times, and at the level it was made (ffmpeg's sine is an eighth of
    // full scale, a mean square of 1/128), not a whisper of noise.
    std::size_t crossings = 0;
    double energy = 0;
    std::size_t const from = 48000 / 4;
    std::size_t const to = 48000 * 3 / 4;
    for (std::size_t i = from; i < to && 2 * i + 2 < heard.size(); ++i) {
        float const now = heard[2 * i];
        float const next = heard[2 * i + 2];
        if ((now < 0) != (next < 0))
            crossings++;
        energy += static_cast<double>(now) * static_cast<double>(now);
    }
    CHECK(crossings >= 420 && crossings <= 460);
    double const mean_square = energy / static_cast<double>(to - from);
    CHECK(mean_square > 0.006 && mean_square < 0.0095);
    // It plays out to the end of the stream and says so.
    page->clock += 2000;
    pump();
    pump();
    CHECK_EQ(page->string("log.join(' ') + ' ' + a.ended + ' ' + (a.currentTime === a.duration)"), "ended true true");
    // A seek starts the sound again where it lands, a little before so the
    // decoder settles, and throws the settling away.
    std::size_t const before = heard.size();
    page->eval("log = []; a.currentTime = 0.5; a.play();");
    pump();
    page->clock += 200;
    pump();
    // The whole stream went out the first time. From the seek, what lies
    // after half a second by the container's own times does, the pre-roll
    // thrown away: decoding starts in the frame 80 ms back, which this file
    // stamps 0.401 s (ffmpeg rounds the codec delay into every block after
    // the first), so 20 frames of 960 samples and 99 ms of the 21st are not
    // played, two channels each.
    CHECK_EQ(heard.size() - before, before - 2u * (20u * 960u + 4752u));
    CHECK_EQ(page->string("log.join(' ') + ' ' + (Math.round(a.currentTime * 10) / 10)"), "seeked playing 0.6");
    CHECK_EQ(page->realm->stats().uncaught_errors, 0);
}

// A MediaSource's video, shown as it plays: the element's picture is the
// frame due at its position, one bitmap written again as the frames come
// (on the machine's video hardware; without it there is no picture, and
// the test says so). The page's clock here is virtual, faster than
// pictures are made, so each step waits for them as a --render does.
void test_media_source_shows_its_video()
{
    std::filesystem::path const fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media" / "vp9-144p.webm";
    std::ifstream in(fixture, std::ios::binary);
    std::vector<std::uint8_t> const video_stream { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    CHECK(video_stream.size() > 1000);
    std::string why;
    bool const hardware = media::Vp9Accelerator::open(why) != nullptr;

    auto page = std::make_unique<Page>("<!DOCTYPE html><body><video id=v></video></body>", "https://example.test/dir/page.html");
    page->load();
    auto const pump = [&page] {
        for (int i = 0; i < 100 && page->realm->run_pending(); ++i) { }
    };
    page->eval("var VI = '" + base64_encode(video_stream) + "';");
    page->eval(R"JS(
        var log = [];
        var v = document.getElementById('v'), ms = new MediaSource();
        ['resize', 'playing', 'seeked'].forEach(function (t) { v.addEventListener(t, function () { log.push(t); }); });
        ms.addEventListener('sourceopen', function () {
            var sb = ms.addSourceBuffer('video/webm; codecs="vp9"');
            sb.addEventListener('updateend', function () { ms.endOfStream(); log.push('appended'); }, { once: true });
            sb.appendBuffer(Uint8Array.from(atob(VI), function (c) { return c.charCodeAt(0); }));
        }, { once: true });
        v.src = URL.createObjectURL(ms);
    )JS");
    pump();
    CHECK_EQ(page->string("log.join(' ') + ' ' + v.readyState + ' ' + v.videoWidth + 'x' + v.videoHeight"), "appended resize 4 256x144");
    page->eval("v.play();");
    pump();
    page->realm->settle_video(3000);
    std::vector<bindings::VideoFrame> const first = page->realm->video_frames();
    if (!hardware) {
        std::cerr << "test_bindings: no VP9 hardware here (" << why << "); the video's pictures are not tested\n";
        CHECK(first.empty());
        return;
    }
    CHECK_EQ(first.size(), 1u);
    if (first.size() != 1)
        return;
    dom::Attr const* const id = first[0].element ? first[0].element->find_attribute("id") : nullptr;
    CHECK(first[0].element && first[0].element->is_html("video") && id && id->value == "v");
    CHECK(first[0].bitmap && first[0].bitmap->width() == 256 && first[0].bitmap->height() == 144);
    // testsrc2's top left corner is its dark counter panel, not the white a
    // bitmap starts as.
    CHECK(first[0].bitmap && first[0].bitmap->pixels()[0] < 128);
    CHECK(first[0].frames >= 1u);

    // Half a second on, newer frames have been written into the same bitmap.
    page->clock += 500;
    pump();
    page->realm->settle_video(3000);
    std::vector<bindings::VideoFrame> const later = page->realm->video_frames();
    CHECK_EQ(later.size(), 1u);
    if (later.size() == 1) {
        CHECK(later[0].bitmap == first[0].bitmap);
        CHECK_EQ(later[0].shape, first[0].shape);
        CHECK(later[0].frames > first[0].frames);
    }
    // Paused and sought back, the picture is the one at the new position.
    page->eval("log = []; v.pause(); v.currentTime = 0.1;");
    pump();
    page->clock += 20;
    pump();
    page->realm->settle_video(3000);
    std::vector<bindings::VideoFrame> const sought = page->realm->video_frames();
    CHECK(sought.size() == 1 && later.size() == 1 && sought[0].frames > later[0].frames);
    CHECK_EQ(page->string("log.join(' ') + ' ' + v.currentTime"), "seeked 0.1");
    // A new load forgets the picture.
    page->eval("v.removeAttribute('src'); v.load();");
    pump();
    CHECK(page->realm->video_frames().empty());
    CHECK_EQ(page->realm->stats().uncaught_errors, 0);
}

// A sound file played by the element itself: fetched, read, and moved
// through by the clock. No machine running this has a sound server it may
// open, so what is measured here is the element's own account of playing —
// the events, the position, the ranges — which is the same account it gives
// when the speakers are real.
void test_a_sound_file_plays()
{
    // Half a second of quiet, at eight thousand frames a second: a RIFF
    // header, the format, and the samples.
    auto const put16 = [](std::string& out, unsigned value) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    };
    auto const put32 = [&put16](std::string& out, unsigned value) {
        put16(out, value & 0xFFFF);
        put16(out, value >> 16);
    };
    unsigned const rate = 8000;
    unsigned const frames = 4000; // half a second
    std::string wav = "RIFF";
    put32(wav, 36 + frames * 2);
    wav += "WAVEfmt ";
    put32(wav, 16);
    put16(wav, 1); // whole numbers
    put16(wav, 1); // one channel
    put32(wav, rate);
    put32(wav, rate * 2);
    put16(wav, 2);
    put16(wav, 16);
    wav += "data";
    put32(wav, frames * 2);
    for (unsigned i = 0; i < frames; ++i)
        put16(wav, 0);

    auto page = loaded("<!DOCTYPE html><body><audio id=a></audio></body>");
    auto const pump = [&page] {
        for (int i = 0; i < 100 && page->realm->run_pending(); ++i) { }
    };
    net::FetchResponse response;
    response.status = 200;
    response.status_text = "OK";
    response.headers.push_back(net::Header { "Content-Type", "audio/wav" });
    response.body.assign(wav.begin(), wav.end());
    page->responses["https://example.test/dir/tone.wav"] = response;
    net::FetchResponse missing;
    missing.status = 404;
    missing.status_text = "Not Found";
    page->responses["https://example.test/dir/gone.wav"] = missing;
    net::FetchResponse nonsense = response;
    nonsense.body = { 'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v' };
    page->responses["https://example.test/dir/nonsense.wav"] = nonsense;

    page->eval(R"JS(
        var log = [];
        var take = function () { var s = log.join(' '); log = []; return s; };
        var ranges = function (r) { var s = ''; for (var i = 0; i < r.length; i++) s += '[' + r.start(i) + ',' + r.end(i) + ')'; return s; };
        var a = document.getElementById('a');
        ['loadstart', 'loadedmetadata', 'durationchange', 'loadeddata', 'canplay', 'canplaythrough', 'play', 'playing',
            'pause', 'ended', 'seeking', 'seeked', 'emptied'].forEach(function (t) { a.addEventListener(t, function () { log.push(t); }); });
        a.addEventListener('error', function () { log.push('error ' + a.error.code + ' ' + a.networkState); });
        var updates = 0;
        a.addEventListener('timeupdate', function () { updates++; });
        a.src = 'tone.wav';
    )JS");
    CHECK_EQ(page->string("a.networkState + ' ' + a.readyState"), "2 0"); // loading, nothing known yet
    pump();
    CHECK_EQ(page->string("take()"), "loadstart loadedmetadata durationchange loadeddata canplay canplaythrough");
    CHECK_EQ(page->string("a.duration + ' ' + a.readyState + ' ' + a.networkState + ' ' + a.paused"), "0.5 4 1 true");
    CHECK_EQ(page->string("ranges(a.buffered) + ' ' + ranges(a.seekable) + ' ' + ranges(a.played)"), "[0,0.5) [0,0.5) ");
    CHECK_EQ(page->string("[a.canPlayType('audio/wav'), a.canPlayType('audio/wav; codecs=1'), a.canPlayType('audio/wav; codecs=\"mp3\"')].join('|')"),
        "maybe|probably|");

    // Playing moves the position with the clock, a tenth of a second at a time.
    page->eval("a.play().then(function () { log.push('resolved'); });");
    pump();
    CHECK_EQ(page->string("take()"), "resolved play playing");
    page->clock += 100;
    pump();
    CHECK(std::abs(page->number("a.currentTime") - 0.1) < 0.001);
    page->clock += 100;
    pump();
    CHECK(std::abs(page->number("a.currentTime") - 0.2) < 0.001);
    CHECK_EQ(page->string("a.paused + ' ' + a.ended + ' ' + take()"), "false false ");

    // A seek lands where it was asked and says so.
    page->eval("a.currentTime = 0.4;");
    CHECK(page->boolean("a.seeking"));
    pump();
    CHECK_EQ(page->string("a.currentTime + ' ' + a.seeking + ' ' + take()"), "0.4 false seeking seeked");

    // It runs out at the end, pauses itself, and says what it played.
    page->clock += 200;
    pump();
    CHECK_EQ(page->string("a.currentTime + ' ' + a.ended + ' ' + a.paused + ' ' + take()"), "0.5 true true pause ended");
    CHECK_EQ(page->string("ranges(a.played)"), "[0,0.2)[0.4,0.5)");
    CHECK(page->number("updates") >= 2);

    // Playing again from the end starts over.
    page->eval("a.play();");
    pump();
    CHECK_EQ(page->string("a.currentTime + ' ' + a.ended + ' ' + a.paused"), "0 false false");

    // What cannot be fetched, and what is fetched but is of no kind we read.
    page->eval("a.src = 'gone.wav'; take();");
    pump();
    CHECK_EQ(page->string("take() + ' ' + a.readyState + ' ' + a.duration"), "emptied loadstart error 4 3 0 NaN");
    page->eval("a.src = 'nonsense.wav'; take();");
    pump();
    CHECK_EQ(page->string("take()"), "emptied loadstart error 4 3");
    // And a fresh element of its own, so that nothing above is what makes it work.
    page->eval(R"JS(
        var b = document.createElement('audio');
        b.addEventListener('canplaythrough', function () { log.push('b ready ' + b.duration); });
        b.src = 'tone.wav';
    )JS");
    pump();
    CHECK_EQ(page->string("take()"), "b ready 0.5");
}

// The IDL attributes reflected from content attributes (HTML §2.6.1). Every
// kind of reflection is here with the cases that tell it from a plain string:
// the state an enumerated attribute is in, the rules for reading a number out
// of an attribute, the numbers a setter refuses, and the ones it replaces
// with the attribute's default.
void test_reflected_attributes()
{
    auto page = loaded("<!DOCTYPE html><body><div id=d></div></body>");

    // An enumerated attribute answers with the keyword of the state it is in,
    // in the keyword's own case. A value that is no keyword, and an absent
    // attribute, are the invalid and missing value defaults — for dir, states
    // with no keyword at all, which read as the empty string.
    page->eval("var d = document.getElementById('d');");
    CHECK_EQ(page->string("d.dir"), "");
    CHECK_EQ(page->string("d.setAttribute('dir', 'RTL'), d.dir"), "rtl");
    CHECK_EQ(page->string("d.setAttribute('dir', 'sideways'), d.dir"), "");
    // The setter writes the value as given, keyword or not.
    CHECK_EQ(page->string("d.dir = 'LTR', d.getAttribute('dir')"), "LTR");
    CHECK(page->boolean("d.dir === 'ltr'"));
    // Every element has one, including an element with no interface of its own.
    CHECK(page->boolean("var u = document.createElement('nosuchelement');"
                        "u.setAttribute('dir', 'auto'), u.dir === 'auto' && u.inputMode === '' && u.enterKeyHint === ''"));
    // An enumerated attribute whose states include one with no keyword reads
    // as null there, and null written removes the attribute.
    CHECK(page->boolean("var img = document.createElement('img'); img.crossOrigin === null"));
    CHECK(page->boolean("img.setAttribute('crossorigin', ''), img.crossOrigin === 'anonymous'"));
    CHECK(page->boolean("img.setAttribute('crossorigin', 'USE-CREDENTIALS'), img.crossOrigin === 'use-credentials'"));
    CHECK(page->boolean("img.crossOrigin = null, !img.hasAttribute('crossorigin') && img.crossOrigin === null"));
    CHECK(page->boolean("img.decoding === 'auto' && (img.setAttribute('decoding', 'sync'), img.decoding === 'sync')"));

    // The rules for parsing integers: leading whitespace and a sign are
    // allowed, trailing text is ignored, and anything else — or a value out of
    // the IDL type's range — leaves the default.
    CHECK(page->boolean("var li = document.createElement('li'); li.value === 0"));
    CHECK(page->boolean("li.setAttribute('value', ' \\t\\n+12px'), li.value === 12"));
    CHECK(page->boolean("li.setAttribute('value', '-7'), li.value === -7"));
    CHECK(page->boolean("li.setAttribute('value', '\\u00a07'), li.value === 0")); // not ASCII whitespace
    CHECK(page->boolean("li.setAttribute('value', '2147483648'), li.value === 0"));
    CHECK(page->boolean("li.setAttribute('value', '2147483647'), li.value === 2147483647"));
    // The setter takes WebIDL's long of the value: out of range it wraps.
    CHECK_EQ(page->string("li.value = 2147483648, li.getAttribute('value')"), "-2147483648");
    CHECK_EQ(page->string("li.value = 1.9, li.getAttribute('value')"), "1");

    // An unsigned long parses as a non-negative integer, has the attribute's
    // own default, and on setting takes the default for anything out of range.
    CHECK(page->boolean("var c = document.createElement('canvas'); c.width === 300 && c.height === 150"));
    CHECK(page->boolean("c.setAttribute('width', '-1'), c.width === 300"));
    CHECK(page->boolean("c.setAttribute('width', '640'), c.width === 640"));
    CHECK_EQ(page->string("c.width = 4294967295, c.getAttribute('width')"), "300");
    CHECK_EQ(page->string("c.width = '-0', c.getAttribute('width')"), "0");

    // Limited to non-negative numbers: a negative set is an IndexSizeError,
    // and the default with no attribute is -1.
    CHECK(page->boolean("var t = document.createElement('textarea'); t.maxLength === -1"));
    CHECK(page->boolean("t.setAttribute('maxlength', '5'), t.maxLength === 5"));
    CHECK_EQ(page->string("try { t.maxLength = -1; 'no throw'; } catch (e) { e.name; }"), "IndexSizeError");
    CHECK(page->boolean("t.getAttribute('maxlength') === '5'"));
    // Limited to greater than zero: a zero set throws for an input's size,
    // and takes the default for a textarea's rows and cols.
    CHECK(page->boolean("var i = document.createElement('input'); i.size === 20"));
    CHECK(page->boolean("i.setAttribute('size', '0'), i.size === 20"));
    CHECK_EQ(page->string("try { i.size = 0; 'no throw'; } catch (e) { e.name; }"), "IndexSizeError");
    CHECK(page->boolean("t.rows === 2 && t.cols === 20"));
    CHECK_EQ(page->string("t.cols = 0, t.getAttribute('cols')"), "20");
    CHECK_EQ(page->string("t.rows = 8, t.getAttribute('rows')"), "8");

    // Clamped on reading, not on writing: a cell spans at least one column
    // and at most a thousand, and may span no rows at all.
    CHECK(page->boolean("var td = document.createElement('td'); td.colSpan === 1 && td.rowSpan === 1"));
    CHECK(page->boolean("td.setAttribute('colspan', '0'), td.colSpan === 1"));
    CHECK(page->boolean("td.setAttribute('colspan', '4000'), td.colSpan === 1000"));
    CHECK(page->boolean("td.setAttribute('rowspan', '0'), td.rowSpan === 0"));
    CHECK(page->boolean("td.setAttribute('rowspan', 'nine'), td.rowSpan === 1"));

    // The rules for parsing floating-point number values, limited to numbers
    // greater than zero: a value that is not is the default, and setting one
    // leaves the attribute alone.
    CHECK(page->boolean("var p = document.createElement('progress'); p.max === 1"));
    CHECK(page->boolean("p.setAttribute('max', '2.5e1'), p.max === 25"));
    CHECK(page->boolean("p.setAttribute('max', '.5'), p.max === 0.5"));
    CHECK(page->boolean("p.setAttribute('max', '-3'), p.max === 1"));
    CHECK(page->boolean("p.setAttribute('max', '1.8e308'), p.max === 1"));
    CHECK_EQ(page->string("p.max = 4, p.max = -1, p.getAttribute('max')"), "4");
    CHECK_EQ(page->string("try { p.max = Infinity; 'no throw'; } catch (e) { e.name; }"), "TypeError");

    // [LegacyNullToEmptyString]: null written to one of these writes the
    // empty string, where any other attribute would write "null".
    CHECK_EQ(page->string("document.body.bgColor = null, document.body.getAttribute('bgcolor')"), "");
    CHECK_EQ(page->string("d.title = null, d.getAttribute('title')"), "null");

    // A form's action, and a submit button's, read as the document's own URL
    // when the attribute is absent or empty; any other URL is resolved.
    CHECK_EQ(page->string("var f = document.createElement('form'); f.action"), "https://example.test/dir/page.html");
    CHECK_EQ(page->string("i.formAction"), "https://example.test/dir/page.html");
    CHECK_EQ(page->string("i.formAction = 'there', i.formAction"), "https://example.test/dir/there");
    CHECK_EQ(page->string("i.getAttribute('formaction')"), "there");

    // The document reflects two of its elements' attributes: dir is the root
    // element's, and the colours are the body's.
    CHECK_EQ(page->string("document.dir"), "");
    CHECK_EQ(page->string("document.documentElement.setAttribute('dir', 'RTL'), document.dir"), "rtl");
    CHECK_EQ(page->string("document.dir = 'ltr', document.documentElement.getAttribute('dir')"), "ltr");
    CHECK_EQ(page->string("document.bgColor = 'red', document.body.getAttribute('bgcolor')"), "red");
    CHECK_EQ(page->string("document.body.setAttribute('text', 'blue'), document.fgColor"), "blue");

    CHECK_EQ(page->console, "");
    page.reset();
}

// Media Source Extensions over the media element: a page feeds two
// SourceBuffers a WebM stream written here, and the element's buffered
// ranges, ready state, events, play promise and clock follow — the clock
// being the test's own, so every position is exact.
void test_media_source_and_the_media_element()
{
    using namespace sashfold::test::webm;
    auto const base64 = [](Bytes const& bytes) { return base64_encode(bytes); };
    auto const cluster = [](int track, std::initializer_list<int> times) {
        Bytes body = element(0xE7, uint_body(0));
        for (int const time : times)
            sashfold::test::webm::append(body, element(0xA3, block(track, std::abs(time), time >= 0 ? 0x80 : 0x00, sashfold::test::webm::text("frame"))));
        return element(0x1F43B675, body);
    };
    auto page = loaded("<!DOCTYPE html><body><video id=v></video></body>");
    auto const pump = [&page] {
        for (int i = 0; i < 100 && page->realm->run_pending(); ++i) { }
    };
    // Video: key frames at 0 and 1 s, a frame every half second to 2 s.
    // Audio: a frame every half second to 2.5 s, which is the Info's duration.
    page->eval("var VI = '" + base64(init_segment(true, false)) + "', AI = '" + base64(init_segment(false, true)) + "', VC = '"
        + base64(cluster(1, { 0, -500, 1000, -1500 })) + "', AC = '" + base64(cluster(2, { 0, 500, 1000, 1500, 2000 })) + "';");
    page->eval(R"JS(
        var log = [];
        var take = function () { var s = log.join(' '); log = []; return s; };
        var bytes = function (b) { return Uint8Array.from(atob(b), function (c) { return c.charCodeAt(0); }); };
        var ranges = function (r) { var s = ''; for (var i = 0; i < r.length; i++) s += '[' + r.start(i) + ',' + r.end(i) + ')'; return s; };
        var watch = function (video, name) {
            ['loadstart', 'durationchange', 'loadedmetadata', 'resize', 'loadeddata', 'canplay', 'canplaythrough', 'play', 'playing', 'waiting',
                'pause', 'ended', 'seeking', 'seeked', 'emptied'].forEach(function (t) { video.addEventListener(t, function () { log.push(name + t); }); });
            video.addEventListener('error', function () { log.push(name + 'error ' + video.error.code + ' ' + video.networkState); });
        };
        var append = function (b, d) { return new Promise(function (r) { b.addEventListener('updateend', r, { once: true }); b.appendBuffer(bytes(d)); }); };
        var v = document.getElementById('v'), ms = new MediaSource(), vb, ab;
        watch(v, '');
        ['sourceopen', 'sourceended', 'sourceclose'].forEach(function (t) { ms.addEventListener(t, function () { log.push(t); }); });
        ms.addEventListener('sourceopen', function () {
            vb = ms.addSourceBuffer('video/webm; codecs="vp9"');
            ab = ms.addSourceBuffer('audio/webm; codecs="opus"');
            append(vb, VI).then(function () { return append(ab, AI); }).then(function () { return append(vb, VC); })
                .then(function () { return append(ab, AC); }).then(function () { log.push('appended'); });
        }, { once: true });
    )JS");
    CHECK_EQ(page->string("ms.readyState + ' ' + ms.duration + ' ' + v.readyState + ' ' + v.networkState + ' ' + v.duration + ' ' + v.paused"), "closed NaN 0 0 NaN true");
    page->eval("v.src = URL.createObjectURL(ms);");
    CHECK_EQ(page->string("ms.readyState"), "open");
    pump();
    CHECK_EQ(page->string("take()"), "loadstart sourceopen durationchange resize loadedmetadata appended loadeddata canplay");
    CHECK_EQ(page->string("ranges(vb.buffered) + ' ' + ranges(ab.buffered) + ' ' + ranges(v.buffered) + ' ' + ranges(v.seekable)"), "[0,2) [0,2.5) [0,2) [0,2.5)");
    CHECK_EQ(page->string("v.readyState + ' ' + v.duration + ' ' + v.videoWidth + 'x' + v.videoHeight + ' ' + ms.sourceBuffers.length + ' ' + ms.activeSourceBuffers.length"), "3 2.5 320x180 2 2");
    CHECK(page->boolean("ms.sourceBuffers[0] === vb && ms.activeSourceBuffers[1] === ab && ms.sourceBuffers[2] === undefined"));
    // A buffer at work refuses a second append.
    CHECK_EQ(page->string("var r = ''; vb.appendBuffer(bytes(VC)); try { vb.appendBuffer(bytes(VC)); } catch (e) { r = e.name + ' ' + vb.updating; } r"), "InvalidStateError true");
    pump();
    CHECK_EQ(page->string("ranges(vb.buffered) + ' ' + vb.updating + take()"), "[0,2) false");

    // Playing: the promise, then the events; the position is the clock's.
    page->eval("v.play().then(function () { log.push('resolved'); });");
    pump();
    CHECK_EQ(page->string("take()"), "resolved play playing");
    page->clock += 1000;
    pump();
    CHECK_EQ(page->number("v.currentTime"), 1.0);
    // It runs out of pictures at 2 s and waits there.
    page->clock += 2000;
    pump();
    CHECK_EQ(page->string("v.currentTime + ' ' + v.readyState + ' ' + v.paused + ' ' + take()"), "2 2 false waiting");
    // The end of the stream: the shorter track counts as reaching the
    // longer's end, and playback carries on to it.
    page->eval("ms.endOfStream();");
    pump();
    CHECK_EQ(page->string("ms.readyState + ' ' + ranges(v.buffered) + ' ' + v.readyState + ' ' + take()"), "ended [0,2.5) 4 sourceended canplay playing canplaythrough");
    page->clock += 1000;
    pump();
    CHECK_EQ(page->string("v.currentTime + ' ' + v.ended + ' ' + v.paused + ' ' + ranges(v.played) + ' ' + take()"), "2.5 true true [0,2.5) pause ended");
    // Seeking back.
    page->eval("v.currentTime = 1;");
    CHECK(page->boolean("v.seeking"));
    pump();
    CHECK_EQ(page->string("v.currentTime + ' ' + v.seeking + ' ' + v.ended + ' ' + take()"), "1 false false seeking seeked");
    // Removing the first second reopens the source; the pictures from the
    // key frame at 1 s stay.
    page->eval("vb.remove(0, 1);");
    pump();
    CHECK_EQ(page->string("ms.readyState + ' ' + ranges(vb.buffered) + ' ' + ranges(v.buffered) + ' ' + take()"), "open [1,2) [1,2) sourceopen");
    // A removed buffer answers nothing more.
    CHECK_EQ(page->string("ms.removeSourceBuffer(ab); var r = ms.sourceBuffers.length + ' '; try { ab.buffered; } catch (e) { r += e.name; } r"), "1 InvalidStateError");
    // Loading something else lets the source go.
    // (Removing src alone reloads nothing.)
    page->eval("v.removeAttribute('src');");
    pump();
    CHECK_EQ(page->string("ms.readyState + take()"), "open");
    page->eval("v.load();");
    pump();
    CHECK_EQ(page->string("ms.readyState + ' ' + ms.sourceBuffers.length + ' ' + v.readyState + ' ' + v.duration + ' ' + take()"), "closed 0 0 NaN sourceclose emptied");

    // What the engine says it plays: VP9 at 8 bits and Opus in WebM, and a
    // stream it could not is refused by its parameters.
    CHECK_EQ(page->string(R"JS([ 'video/webm; codecs="vp9"', 'video/webm; codecs="vp09.00.10.08"', 'audio/webm; codecs="opus"', 'video/webm; codecs="vp9, opus"',
        'video/webm', 'video/webm; codecs="vp09.02.51.10.01.09.16.09.00"', 'video/webm; codecs="vp8"', 'audio/webm; codecs="vp9"',
        'video/mp4; codecs="avc1.42E01E"', 'video/webm; codecs="vp9"; width=3840; height=2160; framerate=60; bitrate=26523399; eotf=bt709',
        'video/webm; codecs="vp9"; width=99999', 'video/webm; codecs="vp9"; framerate=9999', 'audio/webm; codecs="opus"; channels=99',
        'video/webm; codecs="vp9"; eotf=catavision', 'video/webm; codecs="vp9"; decode-to-texture=nope', 'video/webm; codecs="vp9"; cryptoblockformat=subsample',
        'video/webm; codecs=""' ].map(function (t) { return MediaSource.isTypeSupported(t) ? 'y' : 'n'; }).join(''))JS"),
        "yyyyynnnnynnnnnnn");
    CHECK_EQ(page->string("[v.canPlayType('video/webm; codecs=\"vp9\"'), v.canPlayType('audio/webm'), v.canPlayType('video/mp4')].join('|')"), "probably|maybe|");
    CHECK_EQ(page->string("var r = ''; try { new MediaSource().addSourceBuffer('video/webm'); } catch (e) { r = e.name; } r"), "InvalidStateError");

    // A stream that is not WebM: the buffer's error, the source ended, the
    // element's load failed.
    page->eval(R"JS(
        var v2 = document.createElement('video'), ms2 = new MediaSource();
        watch(v2, 'v2 ');
        ms2.addEventListener('sourceopen', function () {
            var b = ms2.addSourceBuffer('video/webm');
            b.addEventListener('error', function () { log.push('buffer error'); });
            b.appendBuffer(new Uint8Array([0, 0, 0, 1]));
        });
        ms2.addEventListener('sourceended', function () { log.push('ms2 ' + ms2.readyState); });
        v2.src = URL.createObjectURL(ms2);
    )JS");
    pump();
    CHECK_EQ(page->string("take()"), "v2 loadstart buffer error ms2 ended v2 error 4 3");
    // A source that is no MediaSource, and a URL revoked before it is used.
    page->eval(R"JS(
        var v3 = document.createElement('video');
        watch(v3, 'v3 ');
        v3.src = 'movie.mp4';
        v3.play().catch(function (e) { log.push('play ' + e.name); });
        var v4 = document.createElement('video'), gone = URL.createObjectURL(new MediaSource());
        watch(v4, 'v4 ');
        URL.revokeObjectURL(gone);
        v4.src = gone;
    )JS");
    pump();
    CHECK_EQ(page->string("take()"), // Both fetches are begun before either can fail: a source that is no
        // MediaSource is now really asked for.
        "play NotSupportedError v3 loadstart v4 loadstart v3 error 4 3 v4 error 4 3");
}

int main()
{
    // The media pipeline is behind a switch until it decodes; the realms made
    // here are made with it on.
#ifdef _WIN32
    _putenv_s("SASHFOLD_MEDIA", "1");
#else
    setenv("SASHFOLD_MEDIA", "1", 1);
    // No test may open the machine's speakers: the element plays against
    // the clock, which is what it does where there is no sound server.
    setenv("PULSE_SERVER", "unix:/nonexistent/sashfold-tests", 1);
#endif
    test_inline_scripts_run_in_document_order();
    test_wrapper_identity_and_expandos_survive_collection();
    test_tree_mutation_and_serialization();
    test_cdata_sections_and_processing_instructions();
    test_ranges();
    test_a_range_outlives_its_frames_document();
    test_selectors_and_collections();
    test_attributes_classlist_style_dataset();
    test_css_supports();
    test_attribute_names_global_this_and_shadow_root();
    test_event_dispatch_order_and_flags();
    test_timers_microtasks_and_the_clock();
    test_document_ready_states_and_load_events();
    test_frame_load_events();
    test_named_access_on_the_window();
    test_external_deferred_and_skipped_scripts();
    test_module_scripts();
    test_noscript_is_raw_text_with_scripting_on();
    test_document_write_during_parsing();
    test_inserted_scripts_run_and_fragment_scripts_do_not();
    test_window_location_url_storage_navigator();
    test_uncaught_errors_are_reported_and_counted();
    test_layout_and_style_hooks();
    test_display_contents_geometry();
    test_form_controls_without_a_host();
    test_dom_parser_and_foreign_documents();
    test_binary_data();
    test_fetch_and_xhr();
    test_content_security_policy();
    test_local_storage_areas();
    test_frames_have_realms_of_their_own();
    test_a_frames_window_events_are_its_own();
    test_a_frames_detached_nodes_go_with_it();
    test_messages_between_windows();
    test_a_frames_document_follows_its_iframe();
    test_an_iframe_has_the_initial_about_blank_document();
    test_a_window_of_another_origin_shows_little();
    test_document_domain_relaxes_the_same_origin_rule();
    test_the_origin_interface();
    test_attributes_in_namespaces();
    test_a_frame_navigates();
    test_a_navigable_keeps_its_target_name();
    test_window_open();
    test_every_iframe_has_a_window();
    test_a_frame_has_a_window();
    test_objects_and_embeds_have_windows();
    test_javascript_urls_in_frames();
    test_a_frame_reuses_the_initial_about_blank_window();
    test_the_sandbox_attribute();
    test_structured_clone_values();
    test_structured_clone_across_realms();
    test_a_message_keeps_what_it_transfers();
    test_platform_objects_and_message_arrivals();
    test_a_frame_measures_itself();
    test_the_computed_appearance();
    test_the_audio_constructor();
    test_interfaces_that_promise();
    test_scripts_that_must_not_run_again();
    test_mutation_observer();
    test_intersection_observer();
    test_custom_elements();
    test_reflected_attributes();
    test_media_source_and_the_media_element();
    test_media_source_plays_its_sound();
    test_media_source_shows_its_video();
    test_a_sound_file_plays();
    return test::report("test_bindings");
}
