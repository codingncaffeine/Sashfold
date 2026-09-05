#include "JsTest.h"

#include "bindings/Realm.h"
#include "css/ComputedStyle.h"
#include "dom/Dom.h"
#include "html/Serializer.h"
#include "html/TreeBuilder.h"

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

    explicit Page(std::string_view html, std::string const& url = "https://example.test/dir/page.html",
        bindings::HostHooks hooks = {})
    {
        hooks.console = [this](std::string_view level, std::string_view message) {
            console += std::string(level) + ":" + std::string(message) + "|";
        };
        hooks.now = [this] { return clock; };
        hooks.fetch_script = [this](net::Url const& target) -> std::optional<std::string> {
            fetched.push_back(target.serialize());
            auto const it = scripts.find(target.serialize());
            if (it == scripts.end())
                return std::nullopt;
            return it->second;
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
    // Dataset.
    CHECK_EQ(page->string("d.dataset.userId"), "7");
    page->eval("d.dataset.fooBar = 'baz'; delete d.dataset.userId;");
    CHECK(page->boolean("d.getAttribute('data-foo-bar') === 'baz' && !d.hasAttribute('data-user-id') && d.dataset.userId === undefined"));
    CHECK_EQ(page->string("Object.keys(d.dataset).join()"), "fooBar");
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
    // a runs at once (twice: two elements), the module and the data blocks
    // are skipped, nomodule runs, the failed fetch fires error, and the
    // deferred and async ones run after the parse in order, before
    // DOMContentLoaded.
    CHECK_EQ(page->string("log.join(' ')"), "a nomodule error:error:true a body b:BODY c dcl");
    CHECK_EQ(page->realm->stats().scripts_skipped, 3);
    CHECK_EQ(page->realm->stats().external_fetched, 4);
    CHECK_EQ(page->realm->stats().external_failed, 1);
    CHECK_EQ(page->realm->stats().scripts_run, 6);
    CHECK_EQ(page->fetched.size(), 5u);
    CHECK_EQ(page->fetched[1], "https://example.test/b.js");
    CHECK(page->console.find("module scripts are not run yet") != std::string::npos);
    CHECK(page->console.find("missing.js could not be loaded") != std::string::npos);
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
    CHECK(page->boolean("typeof fetch === 'undefined' && typeof XMLHttpRequest === 'undefined' && typeof Promise === 'undefined'"));
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
    // Observers deliver once, on the next turn.
    page->eval("var io = new IntersectionObserver(function (entries, observer) { window.seen = entries.length + ':' + entries[0].isIntersecting + ':' + (entries[0].target === document.body) + ':' + (observer === io); }); io.observe(document.body); io.observe(document.documentElement);");
    page->clock += 10;
    page->realm->run_pending();
    CHECK_EQ(page->string("window.seen"), "2:true:true:true");
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
    hooks.scroll_position = [] { return std::pair<int, int> { 0, 30 }; };
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

} // namespace

int main()
{
    test_inline_scripts_run_in_document_order();
    test_wrapper_identity_and_expandos_survive_collection();
    test_tree_mutation_and_serialization();
    test_selectors_and_collections();
    test_attributes_classlist_style_dataset();
    test_event_dispatch_order_and_flags();
    test_timers_microtasks_and_the_clock();
    test_document_ready_states_and_load_events();
    test_external_deferred_and_skipped_scripts();
    test_noscript_is_raw_text_with_scripting_on();
    test_document_write_during_parsing();
    test_inserted_scripts_run_and_fragment_scripts_do_not();
    test_window_location_url_storage_navigator();
    test_uncaught_errors_are_reported_and_counted();
    test_layout_and_style_hooks();
    test_form_controls_without_a_host();
    test_dom_parser_and_foreign_documents();
    return test::report("test_bindings");
}
