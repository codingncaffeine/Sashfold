#include "JsTest.h"

#include "bindings/Realm.h"
#include "bindings/Workers.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Dedicated workers, driven the way a host drives a page: a page with its
// scripts, a virtual clock, a loader the test answers for. Without a
// WorkerThreads a worker runs on the page's thread, a turn inside each
// run_pending(), so everything here but the last tests is deterministic; the
// last ones give the workers threads of their own and wait on the wall clock,
// inside bands.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<net::ContentSecurityPolicy> policy;
    std::unique_ptr<bindings::Realm> realm;
    std::string console; // "level:message|"
    double clock = 1000;
    std::map<std::string, net::FetchResponse> responses;
    std::vector<std::string> requests;

    explicit Page(std::string const& url = "https://example.test/dir/page.html", std::string const& policy_header = {},
        bindings::WorkerThreads* threads = nullptr)
    {
        bindings::HostHooks hooks;
        if (!policy_header.empty()) {
            policy = std::make_unique<net::ContentSecurityPolicy>(*net::parse_url(url));
            policy->set_reporter([this](std::string_view message) { console += "policy:" + std::string(message) + "|"; });
            policy->add_header(policy_header, false);
            hooks.policy = policy.get();
        }
        hooks.console = [this](std::string_view level, std::string_view message) {
            console += std::string(level) + ":" + std::string(message) + "|";
        };
        hooks.now = [this] { return clock; };
        hooks.fetch_resource = [this](net::Url const& target, net::ResourceRequest const& request,
                                   net::RequestGuard const& guard) -> net::FetchResult {
            if (guard.refusal) {
                if (std::optional<std::string> refused = guard.refusal(target, false))
                    return { std::nullopt, std::move(*refused) };
            }
            requests.push_back(request.method + " " + target.serialize());
            auto const it = responses.find(target.serialize());
            if (it == responses.end())
                return { std::nullopt, "no such resource" };
            net::FetchResponse response = it->second;
            if (response.final_url.scheme.empty())
                response.final_url = target;
            return { std::move(response), "" };
        };
        if (threads != nullptr) {
            // The worker's own way to the network: a copy of the answers as
            // they stand when the page is made, touched by nothing else.
            hooks.worker_threads = threads;
        }
        hooks.user_agent = "Mozilla/5.0 TestAgent Sashfold/0.0";
        m_hooks = std::move(hooks);
        m_url = url;
    }

    // The realm is made once the answers are in, so that a threaded worker's
    // fetch hook can carry a copy of them.
    void open()
    {
        if (m_hooks.worker_threads != nullptr) {
            auto const answers = std::make_shared<std::map<std::string, net::FetchResponse> const>(responses);
            m_hooks.worker_fetch = [answers](net::Url const& target, net::ResourceRequest const&,
                                       net::RequestGuard const& guard) -> net::FetchResult {
                if (guard.refusal) {
                    if (std::optional<std::string> refused = guard.refusal(target, false))
                        return { std::nullopt, std::move(*refused) };
                }
                auto const it = answers->find(target.serialize());
                if (it == answers->end())
                    return { std::nullopt, "no such resource" };
                net::FetchResponse response = it->second;
                response.final_url = target;
                return { std::move(response), "" };
            };
        }
        realm = std::make_unique<bindings::Realm>(*document, *net::parse_url(m_url), m_hooks);
        realm->interpreter().heap().set_stress(true);
        html::parse_document_bytes_into(*document, std::string("<!doctype html><body></body>"), realm.get());
        realm->document_parsed();
    }

    void script(std::string const& url, std::string const& source, std::string const& type = "text/javascript", int status = 200)
    {
        net::FetchResponse response;
        response.status = status;
        response.status_text = status == 200 ? "OK" : "Not Found";
        response.headers.push_back(net::Header { "Content-Type", type });
        response.body.assign(source.begin(), source.end());
        responses[url] = response;
    }

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
    std::string throws(std::string_view source) { return test::eval_throws(realm->interpreter(), source); }

    // The loop, until nothing is due at the clock's time: a worker on this
    // thread has its turns inside it.
    void settle()
    {
        for (int i = 0; i < 100; ++i) {
            std::optional<double> const due = realm->next_timer_due();
            if (!due || *due > clock)
                return;
            realm->run_pending();
        }
        test::fail("the loop never settled", __FILE__, __LINE__);
    }
    void advance(double ms)
    {
        clock += ms;
        settle();
    }

private:
    bindings::HostHooks m_hooks;
    std::string m_url;
};

// A page whose `log` collects what its worker says.
constexpr std::string_view listen = R"js(
    var log = [];
    function listen(worker) {
        worker.onmessage = function (e) { log.push(typeof e.data === 'string' ? e.data : JSON.stringify(e.data)); };
        worker.onerror = function (e) { log.push('error:' + (e.message || e.type)); };
        return worker;
    }
    function blob(source) { return URL.createObjectURL(new Blob([source], { type: 'text/javascript' })); }
)js";

void test_a_worker_answers_its_page()
{
    Page page;
    page.open();
    page.eval(listen);
    page.eval(R"js(
        var w = listen(new Worker(blob("onmessage = function (e) { postMessage('got ' + e.data + ' origin[' + e.origin + '] source ' + e.source); };")));
        w.postMessage('ping');
    )js");
    // Nothing ran inside the constructor or postMessage: the worker starts
    // at the loop's next turn, and its answer arrives in a turn of its own.
    CHECK_EQ(page.string("log.join()"), std::string(""));
    CHECK(page.realm->has_pending_timers());
    page.settle();
    CHECK_EQ(page.string("log.join()"), std::string("got ping origin[] source null"));
    // An idle worker asks nothing of the loop.
    CHECK(!page.realm->has_pending_timers());
    CHECK(!page.realm->next_timer_due().has_value());
    // The event the page got.
    page.eval("var seen; w.addEventListener('message', function (e) { seen = e; }); w.postMessage('again');");
    page.settle();
    CHECK_EQ(page.string("seen.constructor.name + ' ' + seen.type + ' [' + seen.origin + '] ' + seen.source + ' ' + seen.ports.length + ' ' + (seen.target === w)"),
        std::string("MessageEvent message [] null 0 true"));
    CHECK_EQ(page.console, std::string(""));
}

void test_the_scope_is_not_a_window()
{
    Page page;
    page.script("https://example.test/dir/scope.js?v=2", R"js(
        var facts = {
            window: typeof window, document: typeof document, parent: typeof parent, localStorage: typeof localStorage,
            alert: typeof alert, Node: typeof Node, HTMLElement: typeof HTMLElement, Worker: typeof Worker,
            self: self === globalThis, thisIs: this === self,
            isScope: self instanceof DedicatedWorkerGlobalScope && self instanceof WorkerGlobalScope && self instanceof EventTarget,
            tag: Object.prototype.toString.call(self),
            name: self.name,
            href: location.href, origin: location.origin, pathname: location.pathname, search: location.search,
            locationIs: location instanceof WorkerLocation, locationText: String(location),
            agent: navigator.userAgent, navigatorIs: navigator instanceof WorkerNavigator, onLine: navigator.onLine,
            cookieEnabled: typeof navigator.cookieEnabled,
            globalOrigin: origin,
            shared: [typeof setTimeout, typeof fetch, typeof XMLHttpRequest, typeof URL, typeof Blob, typeof TextEncoder,
                typeof MessageChannel, typeof AbortController, typeof structuredClone, typeof atob, typeof queueMicrotask,
                typeof importScripts, typeof close, typeof postMessage].join(),
            handlers: [onmessage, onerror, onmessageerror].join(),
        };
        postMessage(facts);
    )js");
    page.open();
    page.eval("var w = new Worker('scope.js?v=2', { name: 'helper' }); var facts; w.onmessage = function (e) { facts = e.data; };");
    page.settle();
    // Asked for once, as a script, by the address the page gave.
    CHECK_EQ(page.requests.size(), std::size_t(1));
    CHECK_EQ(page.requests.back(), std::string("GET https://example.test/dir/scope.js?v=2"));
    CHECK_EQ(page.string("[facts.window, facts.document, facts.parent, facts.localStorage, facts.alert, facts.Node, facts.HTMLElement, facts.Worker].join()"),
        std::string("undefined,undefined,undefined,undefined,undefined,undefined,undefined,undefined"));
    CHECK_EQ(page.string("[facts.self, facts.thisIs, facts.isScope, facts.tag, facts.name].join()"),
        std::string("true,true,true,[object DedicatedWorkerGlobalScope],helper"));
    CHECK_EQ(page.string("[facts.href, facts.origin, facts.pathname, facts.search, facts.locationIs, facts.locationText].join()"),
        std::string("https://example.test/dir/scope.js?v=2,https://example.test,/dir/scope.js,?v=2,true,https://example.test/dir/scope.js?v=2"));
    CHECK_EQ(page.string("[facts.agent, facts.navigatorIs, facts.onLine, facts.cookieEnabled, facts.globalOrigin].join()"),
        std::string("Mozilla/5.0 TestAgent Sashfold/0.0,true,true,undefined,https://example.test"));
    CHECK_EQ(page.string("facts.shared"),
        std::string("function,function,function,function,function,function,function,function,function,function,function,function,function,function"));
    CHECK_EQ(page.string("facts.handlers"), std::string(",,"));
    CHECK_EQ(page.console, std::string(""));
}

void test_what_a_message_carries()
{
    Page page;
    page.open();
    page.eval(listen);
    page.eval(R"js(
        var w = listen(new Worker(blob(`
            onmessage = function (e) {
                var d = e.data;
                if (d.buffer) {
                    var view = new Uint8Array(d.buffer);
                    view[0] = view[0] + 1;
                    postMessage({ back: d.buffer, length: d.buffer.byteLength, first: view[0] }, [d.buffer]);
                    return;
                }
                postMessage({ map: d.map instanceof Map && d.map.get('k'), when: d.when instanceof Date && d.when.getTime(),
                    same: d.twice[0] === d.twice[1], nested: d.nested.list[2].deep, big: String(d.big) });
            };
        `)));
        var shared = { x: 1 };
        w.postMessage({ map: new Map([['k', 'v']]), when: new Date(5), twice: [shared, shared], nested: { list: [1, 2, { deep: 'yes' }] }, big: 12345678901234567890n });
    )js");
    page.settle();
    CHECK_EQ(page.string("log.join()"), std::string(R"({"map":"v","when":5,"same":true,"nested":"yes","big":"12345678901234567890"})"));
    // A buffer transferred is gone from the side that sent it, both ways.
    page.eval(R"js(
        log = [];
        var buffer = new Uint8Array([41, 2, 3]).buffer;
        var back;
        w.onmessage = function (e) { back = e.data; };
        w.postMessage({ buffer: buffer }, [buffer]);
    )js");
    CHECK_EQ(page.string("String(buffer.byteLength)"), std::string("0"));
    page.settle();
    CHECK_EQ(page.string("[back.length, back.first, back.back.byteLength, new Uint8Array(back.back)[0]].join()"), std::string("3,42,3,42"));
    // What cannot be cloned throws where it is posted.
    CHECK(page.throws("w.postMessage(function () {})").starts_with("DataCloneError"));
    CHECK(page.throws("w.postMessage(1, [{}])").starts_with("DataCloneError"));
    CHECK(page.throws("Worker.prototype.postMessage.call({}, 1)").starts_with("TypeError"));
    CHECK_EQ(page.console, std::string(""));
}

// A MessagePort crosses between a page and its worker: the two ports end up
// in two heaps, entangled through a channel that holds nothing of either.
void test_ports_cross_between_the_two()
{
    Page page;
    page.open();
    page.eval(listen);
    // The page's port goes to the worker, with what was already queued on it.
    page.eval(R"js(
        var w = listen(new Worker(blob(`
            onmessage = function (e) {
                var port = e.ports[0];
                postMessage('worker got ' + e.data + ' with ' + e.ports.length + ' port, a ' + port.constructor.name);
                port.onmessage = function (m) { port.postMessage('echo ' + m.data); if (m.data === 'close') port.close(); };
            };
        `)));
        var channel = new MessageChannel();
        var heard = [];
        channel.port1.onmessage = function (e) { heard.push(e.data); };
        channel.port2.postMessage('to the page, before the transfer');
        channel.port1.postMessage('queued before the transfer');
        w.postMessage('hello', [channel.port2]);
        channel.port1.postMessage('after the transfer');
    )js");
    for (int i = 0; i < 4; ++i)
        page.settle();
    CHECK_EQ(page.string("log.join()"), std::string("worker got hello with 1 port, a MessagePort"));
    CHECK_EQ(page.string("heard.join(' | ')"),
        std::string("to the page, before the transfer | echo queued before the transfer | echo after the transfer"));
    // An idle pair asks nothing of the loop.
    CHECK(!page.realm->has_pending_timers());
    // The port that left is detached here: it posts nowhere, and cannot leave again.
    page.eval("channel.port2.postMessage('from a detached port');");
    CHECK(page.throws("w.postMessage(1, [channel.port2])").starts_with("DataCloneError"));
    // Closing one end ends the conversation: what is sent after goes nowhere.
    page.eval("heard = []; channel.port1.postMessage('close'); ");
    for (int i = 0; i < 4; ++i)
        page.settle();
    page.eval("channel.port1.postMessage('into the void');");
    for (int i = 0; i < 4; ++i)
        page.settle();
    CHECK_EQ(page.string("heard.join(' | ')"), std::string("echo close"));

    // The other way: a worker makes the channel and sends the page an end.
    page.eval(R"js(
        heard = [];
        var w2 = new Worker(blob(`
            var channel = new MessageChannel();
            channel.port1.onmessage = function (e) { channel.port1.postMessage(e.data * 2); };
            postMessage('here is a port', [channel.port2]);
        `));
        var fromWorker;
        w2.onmessage = function (e) {
            fromWorker = e.ports[0];
            fromWorker.onmessage = function (m) { heard.push('doubled ' + m.data); };
            fromWorker.postMessage(21);
        };
    )js");
    for (int i = 0; i < 6; ++i)
        page.settle();
    CHECK_EQ(page.string("heard.join()"), std::string("doubled 42"));

    // An end that moves on: the page hands one worker's port to another, and
    // the two workers talk with the page out of it.
    page.eval(R"js(
        heard = [];
        var speaker = new Worker(blob(`
            var channel = new MessageChannel();
            channel.port1.onmessage = function (e) { postMessage('speaker heard ' + e.data); };
            channel.port1.postMessage('sent before anyone held the other end');
            postMessage('port', [channel.port2]);
        `));
        var listener = new Worker(blob(`
            onmessage = function (e) {
                var port = e.ports[0];
                port.onmessage = function (m) { postMessage('listener heard ' + m.data); port.postMessage('thanks'); };
            };
        `));
        speaker.onmessage = function (e) {
            if (e.ports.length) listener.postMessage('take it', [e.ports[0]]);
            else heard.push(e.data);
        };
        listener.onmessage = function (e) { heard.push(e.data); };
    )js");
    for (int i = 0; i < 10; ++i)
        page.settle();
    CHECK_EQ(page.string("heard.join(' | ')"),
        std::string("listener heard sent before anyone held the other end | speaker heard thanks"));
    CHECK_EQ(page.console, std::string(""));
    // A port of a worker that has ended is an end that closes: nothing dangles.
    page.eval("speaker.terminate(); listener.terminate(); w.terminate(); w2.terminate(); fromWorker.postMessage(1);");
    for (int i = 0; i < 4; ++i)
        page.settle();
    CHECK(!page.realm->has_pending_timers());
}

void test_import_scripts()
{
    Page page;
    page.script("https://example.test/lib/one.js", "var order = ['one']; var base = location.href;");
    page.script("https://example.test/lib/two.js", "order.push('two');");
    page.script("https://other.test/three.js", "order.push('three');");
    page.script("https://example.test/lib/page.html", "order.push('html');", "text/html");
    page.script("https://example.test/lib/throws.js", "order.push('before'); throw new RangeError('from the import');");
    page.script("https://example.test/lib/main.js", R"js(
        importScripts('one.js', './two.js', 'https://other.test/three.js');
        var results = [order.join('+'), base];
        function attempt(what) {
            try { importScripts(what); return 'ran'; } catch (e) { return e.name + (e instanceof DOMException ? '(DOM)' : '') + ':' + e.message.slice(0, 12); }
        }
        results.push(attempt('missing.js'), attempt('page.html'), attempt('throws.js'), attempt('http://[bad'), order.join('+'));
        importScripts();
        importScripts('data:text/javascript,order.push("data")');
        results.push(order[order.length - 1]);
        postMessage(results);
    )js");
    page.open();
    page.eval(listen);
    page.eval("var results; var w = new Worker('/lib/main.js'); w.onmessage = function (e) { results = e.data; };");
    page.settle();
    CHECK_EQ(page.string("results[0]"), std::string("one+two+three"));
    // A script imported runs in the worker's scope, whose address is the worker's.
    CHECK_EQ(page.string("results[1]"), std::string("https://example.test/lib/main.js"));
    CHECK_EQ(page.string("results[2]"), std::string("NetworkError(DOM):Failed to ex"));
    CHECK_EQ(page.string("results[3]"), std::string("NetworkError(DOM):Failed to ex"));
    CHECK_EQ(page.string("results[4]"), std::string("RangeError:from the imp"));
    CHECK_EQ(page.string("results[5]"), std::string("SyntaxError(DOM):Failed to ex"));
    CHECK_EQ(page.string("results[6]"), std::string("one+two+three+before"));
    CHECK_EQ(page.string("results[7]"), std::string("data"));
    // Why each failed is on the page's console, from the worker.
    CHECK(page.console.find("error:importScripts: 'https://example.test/lib/missing.js' failed to load: no such resource|") != std::string::npos);
    CHECK(page.console.find("its MIME type ('text/html') is not a JavaScript one|") != std::string::npos);
}

void test_a_script_that_cannot_be_had()
{
    Page page;
    page.script("https://example.test/dir/page-as-worker.js", "postMessage('ran');", "text/html");
    page.script("https://example.test/dir/gone.js", "postMessage('ran');", "text/javascript", 404);
    page.open();
    page.eval(listen);
    page.eval(R"js(
        var events = [];
        function watch(url, options) {
            var w = new Worker(url, options);
            w.onmessage = function (e) { events.push(url + ' said ' + e.data); };
            w.onerror = function (e) { events.push(url + ' ' + e.constructor.name + ' ' + e.type + ' ' + e.cancelable); };
            return w;
        }
        var a = watch('page-as-worker.js'), b = watch('gone.js'), c = watch('nowhere.js'), d = watch('blob:https://example.test/nothing');
        var e = watch('data:text/javascript,postMessage("module")', { type: 'module' });
    )js");
    page.settle();
    CHECK_EQ(page.string("events.join('; ')"),
        std::string("page-as-worker.js Event error false; gone.js Event error false; nowhere.js Event error false; "
                    "blob:https://example.test/nothing Event error false; data:text/javascript,postMessage(\"module\") Event error false"));
    CHECK(page.console.find("its MIME type ('text/html') is not a JavaScript one") != std::string::npos);
    CHECK(page.console.find("failed to load: HTTP 404") != std::string::npos);
    CHECK(page.console.find("failed to load: no such resource") != std::string::npos);
    CHECK(page.console.find("the blob: URL names nothing") != std::string::npos);
    CHECK(page.console.find("module workers are not written yet") != std::string::npos);
    // Each has ended: nothing is left for the loop.
    CHECK(!page.realm->has_pending_timers());
    // What is wrong with the call itself throws where it is made.
    CHECK(page.throws("new Worker()").starts_with("TypeError"));
    CHECK(page.throws("new Worker('http://[bad')").starts_with("SyntaxError"));
    CHECK(page.throws("new Worker('https://other.test/w.js')").starts_with("SecurityError"));
    CHECK(page.throws("new Worker('w.js', { type: 'shared' })").starts_with("TypeError"));
    CHECK(page.throws("new Worker('w.js', 7)").starts_with("TypeError"));
    CHECK(page.throws("Worker('w.js')").starts_with("TypeError"));
}

void test_errors_reach_the_page()
{
    Page page;
    page.open();
    page.eval(listen);
    // Nothing deals with it in the worker: the Worker is told, and with
    // nothing cancelling it there the page's console says it.
    page.eval(R"js(
        var told = [];
        var w = new Worker(blob("onmessage = function (e) { if (e.data === 'throw') throw new TypeError('in the worker'); postMessage('fine'); };"));
        w.onerror = function (e) { told.push(e.constructor.name + ':' + e.message + ':' + e.cancelable + ':' + (e.target === w)); };
        w.postMessage('throw');
    )js");
    page.settle();
    CHECK_EQ(page.string("told.join()"), std::string("ErrorEvent:Uncaught TypeError: in the worker:true:true"));
    CHECK(page.console.find("error:Uncaught TypeError: in the worker") != std::string::npos);
    // The worker goes on after an uncaught error.
    page.eval("w.onmessage = function (e) { told.push(e.data); }; w.postMessage('go');");
    page.settle();
    CHECK_EQ(page.string("told[1]"), std::string("fine"));
    // The Worker's listener cancels it: the console is not told.
    page.console.clear();
    page.eval("w.onerror = null; w.addEventListener('error', function (e) { e.preventDefault(); told.push('cancelled'); }); w.postMessage('throw');");
    page.settle();
    CHECK_EQ(page.string("told[2]"), std::string("cancelled"));
    CHECK_EQ(page.console, std::string(""));
    // The worker's own onerror takes the five parts, and true from it is the end of it.
    page.eval(R"js(
        var w2 = new Worker(blob(`
            onerror = function (message, file, line, column, error) {
                postMessage([typeof message, message, typeof file, line, column, error instanceof RangeError, arguments.length].join());
                return true;
            };
            addEventListener('error', function (e) { postMessage('listener ' + e.constructor.name + ' ' + e.error.message); });
            setTimeout(function () { throw new RangeError('handled here'); }, 0);
        `));
        var heard = [];
        w2.onmessage = function (e) { heard.push(e.data); };
        w2.onerror = function () { heard.push('the page was told'); };
    )js");
    page.settle();
    CHECK_EQ(page.string("heard.join(' | ')"),
        std::string("string,Uncaught RangeError: handled here,string,0,0,true,5 | listener ErrorEvent handled here"));
    CHECK_EQ(page.console, std::string(""));
    // A script that fails as it first runs is an error like any other.
    page.eval("var w3 = new Worker(blob('nothing.here();')); w3.onerror = function (e) { heard.push(e.message); e.preventDefault(); };");
    page.settle();
    CHECK(page.string("heard[2]").starts_with("Uncaught ReferenceError"));
}

void test_a_windows_error_event()
{
    Page page;
    page.open();
    page.eval(R"js(
        var seen = [];
        window.addEventListener('error', function (e) {
            seen.push(e.constructor.name + ':' + e.message + ':' + (e.error instanceof SyntaxError) + ':' + e.cancelable);
        });
        setTimeout(function () { throw new SyntaxError('from a timer'); }, 0);
    )js");
    page.settle();
    CHECK_EQ(page.string("seen.join()"), std::string("ErrorEvent:Uncaught SyntaxError: from a timer:true:true"));
    CHECK(page.console.find("error:Uncaught SyntaxError: from a timer") != std::string::npos);
    // onerror's five parts; true from it keeps the console quiet, and the listeners still hear.
    page.console.clear();
    page.eval(R"js(
        seen = [];
        window.onerror = function (message, file, line, column, error) { seen.push('handler ' + arguments.length + ' ' + error.message); return true; };
        setTimeout(function () { throw new Error('second'); }, 0);
    )js");
    page.settle();
    CHECK_EQ(page.string("seen.join(' | ')"), std::string("handler 5 second | ErrorEvent:Uncaught Error: second:false:true"));
    CHECK_EQ(page.console, std::string(""));
    // One thrown while an error is being reported is said, not reported again.
    page.eval("window.onerror = function () { throw new Error('inside onerror'); }; setTimeout(function () { throw new Error('third'); }, 0);");
    page.settle();
    CHECK(page.console.find("Uncaught Error: inside onerror") != std::string::npos);
    CHECK(page.console.find("Uncaught Error: third") != std::string::npos);
    // The constructor.
    CHECK_EQ(page.string("var made = new ErrorEvent('error', { message: 'm', filename: 'f', lineno: 3, colno: 4, error: 5 });"
                         "[made.message, made.filename, made.lineno, made.colno, made.error, new ErrorEvent('x').error].join()"),
        std::string("m,f,3,4,5,"));
}

void test_timers_run_on_the_pages_clock()
{
    Page page;
    page.open();
    page.eval(listen);
    page.eval(R"js(
        var w = listen(new Worker(blob(`
            var ticks = 0;
            setTimeout(function () { postMessage('late'); }, 50);
            var id = setInterval(function () { postMessage('tick ' + (++ticks)); if (ticks === 3) clearInterval(id); }, 20);
            Promise.resolve().then(function () { postMessage('microtask'); });
            postMessage('started at ' + Math.round(performance.now()));
        `)));
    )js");
    page.settle();
    CHECK_EQ(page.string("log.join()"), std::string("started at 0,microtask"));
    // The worker's timers are what the page's loop waits for.
    CHECK(page.realm->has_pending_timers());
    CHECK_EQ(*page.realm->next_timer_due(), 1020.0);
    page.advance(20);
    CHECK_EQ(page.string("log.join()"), std::string("started at 0,microtask,tick 1"));
    page.advance(30);
    CHECK_EQ(page.string("log.join()"), std::string("started at 0,microtask,tick 1,tick 2,late"));
    // An interval is armed again from when it ran: the second ran at 1050.
    CHECK_EQ(*page.realm->next_timer_due(), 1070.0);
    page.advance(20);
    CHECK_EQ(page.string("log.join()"), std::string("started at 0,microtask,tick 1,tick 2,late,tick 3"));
    CHECK(!page.realm->has_pending_timers());
}

void test_terminate_and_close()
{
    Page page;
    page.open();
    page.eval(listen);
    page.eval(R"js(
        var w = listen(new Worker(blob("setInterval(function () { postMessage('tick'); }, 10); onmessage = function (e) { postMessage('heard ' + e.data); };")));
    )js");
    page.settle();
    page.advance(10);
    CHECK_EQ(page.string("log.join()"), std::string("tick"));
    // terminate(): what the worker had said and not yet delivered is dropped
    // with it, its timers never fire again, and a message to it goes nowhere.
    page.eval("w.postMessage('one'); w.terminate(); w.postMessage('two'); w.terminate();");
    page.advance(50);
    CHECK_EQ(page.string("log.join()"), std::string("tick"));
    CHECK(!page.realm->has_pending_timers());

    // close(): the script running is the last; what was queued is dropped.
    page.eval(R"js(
        log = [];
        var w2 = listen(new Worker(blob(`
            setTimeout(function () { postMessage('never'); }, 5);
            onmessage = function (e) { postMessage('closing on ' + e.data); close(); postMessage('still this turn'); };
        `)));
        w2.postMessage('first'); w2.postMessage('second');
    )js");
    page.advance(50);
    CHECK_EQ(page.string("log.join()"), std::string("closing on first,still this turn"));
    CHECK(!page.realm->has_pending_timers());
    page.eval("w2.postMessage('after'); w2.terminate();");
    page.advance(50);
    CHECK_EQ(page.string("log.join()"), std::string("closing on first,still this turn"));

    // A worker that terminates itself from its page's handler mid-delivery.
    page.eval(R"js(
        log = [];
        var w3 = new Worker(blob("postMessage('a'); postMessage('b'); postMessage('c');"));
        w3.onmessage = function (e) { log.push(e.data); if (e.data === 'a') w3.terminate(); };
    )js");
    page.advance(50);
    CHECK_EQ(page.string("log.join()"), std::string("a"));
    CHECK_EQ(page.console, std::string(""));
}

void test_the_console_and_the_policy()
{
    // The worker's console is the page's.
    {
        Page page;
        page.open();
        page.eval("new Worker(URL.createObjectURL(new Blob([\"console.log('from', 'the worker'); console.warn('careful');\"])));");
        page.settle();
        CHECK_EQ(page.console, std::string("log:from the worker|warn:careful|"));
    }
    // worker-src judges the script: a blob: worker where only 'self' may be one.
    {
        Page page("https://example.test/dir/page.html", "worker-src 'self'");
        page.script("https://example.test/dir/own.js", "postMessage('own ran');");
        page.open();
        page.eval(listen);
        page.eval("var a = listen(new Worker(blob(\"postMessage('blob ran');\"))); var b = listen(new Worker('own.js'));");
        page.settle();
        CHECK_EQ(page.string("log.join()"), std::string("error:error,own ran"));
        CHECK(page.console.find("worker-src 'self'") != std::string::npos);
    }
    // Without worker-src, script-src is what is asked; and a blob: worker runs
    // under its owner's policy: its own fetches are judged by connect-src.
    {
        Page page("https://example.test/dir/page.html", "script-src 'self' blob: 'unsafe-inline'; connect-src 'none'");
        page.script("https://example.test/dir/data.txt", "secret", "text/plain");
        page.open();
        page.eval(listen);
        page.eval(R"js(
            var w = listen(new Worker(blob("fetch('https://example.test/dir/data.txt').then(function (r) { return r.text(); }).then(postMessage, function (e) { postMessage('refused: ' + e.name); });")));
        )js");
        page.settle();
        page.settle();
        CHECK_EQ(page.string("log.join()"), std::string("refused: TypeError"));
        CHECK(page.console.find("connect-src 'none'") != std::string::npos);
    }
}

void test_a_worker_fetches()
{
    Page page;
    page.script("https://example.test/dir/data.json", R"({"answer": 42})", "application/json");
    page.script("https://example.test/dir/fetcher.js", R"js(
        fetch('data.json').then(function (r) { return r.json(); }).then(function (d) {
            var xhr = new XMLHttpRequest();
            xhr.open('GET', 'data.json', false);
            xhr.send();
            postMessage(d.answer + ' ' + xhr.status + ' ' + JSON.parse(xhr.responseText).answer);
        });
    )js");
    page.open();
    page.eval(listen);
    page.eval("var w = listen(new Worker('fetcher.js'));");
    for (int i = 0; i < 5; ++i)
        page.settle();
    CHECK_EQ(page.string("log.join()"), std::string("42 200 42"));
    // Relative URLs were resolved against the worker's script.
    CHECK_EQ(page.requests.back(), std::string("GET https://example.test/dir/data.json"));
}

void test_a_page_that_ends_takes_its_workers()
{
    auto page = std::make_unique<Page>();
    page->open();
    page->eval(listen);
    page->eval("var w = listen(new Worker(blob(\"setInterval(function () { postMessage('tick'); }, 10);\")));");
    page->settle();
    page->advance(10);
    CHECK_EQ(page->string("log.join()"), std::string("tick"));
    // Nothing to check but that it ends cleanly (the sanitizers watch).
    page.reset();
    CHECK(true);
}

// --- Threads of their own --------------------------------------------------------------------

double elapsed_ms(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

void test_a_worker_on_a_thread_of_its_own()
{
    std::atomic<int> wakes { 0 };
    bindings::WorkerThreads threads;
    threads.set_wake([&wakes] { ++wakes; });
    {
        Page page("https://example.test/dir/page.html", {}, &threads);
        page.script("https://example.test/lib/sum.js", "function sum(n) { var total = 0; for (var i = 1; i <= n; ++i) total += i; return total; }");
        page.script("https://example.test/dir/threaded.js", R"js(
            importScripts('/lib/sum.js');
            onmessage = function (e) { postMessage('sum ' + sum(e.data)); };
            setTimeout(function () { postMessage('timer'); }, 30);
            console.log('started');
        )js");
        page.open();
        page.eval(listen);
        auto const began = std::chrono::steady_clock::now();
        page.eval("var w = listen(new Worker('threaded.js')); w.postMessage(200000);");
        // The page's loop has nothing to run for the worker: it only hears.
        while (page.string("String(log.length)") != "2" && elapsed_ms(began) < 10000) {
            page.realm->run_pending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        double const took = elapsed_ms(began);
        std::string const heard = page.string("log.slice().sort().join()");
        CHECK_EQ(heard, std::string("sum 20000100000,timer"));
        std::printf("    a threaded worker answered in %.0f ms (band: under 10000)\n", took);
        CHECK(took < 10000);
        CHECK(page.console.find("log:started|") != std::string::npos);
        // Every word the worker had for its page woke the host.
        CHECK(wakes.load() >= 3);
        CHECK_EQ(threads.running(), std::size_t(1));
        // The page's thread was never inside the worker: its fetch hook saw nothing.
        CHECK_EQ(page.requests.size(), std::size_t(0));

        // The thread has a stack a script can recurse on, and running out of
        // the engine's budget is a RangeError there as anywhere, not a crash.
        page.eval("var deep = new Worker(blob(\"function d(n) { return n ? 1 + d(n - 1) : 0; } var out = d(300); try { (function f() { f(); })(); } catch (e) { out += ' ' + e.name; } postMessage(out);\")); deep.onmessage = function (e) { log.push('deep ' + e.data); deep.terminate(); };");
        auto const recursing = std::chrono::steady_clock::now();
        while (page.string("String(log.length)") != "3" && elapsed_ms(recursing) < 10000) {
            page.realm->run_pending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_EQ(page.string("log[2]"), std::string("deep 300 RangeError"));
        page.eval("log.pop();");
        auto const recursed = std::chrono::steady_clock::now();
        while (threads.running() > 1 && elapsed_ms(recursed) < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // A port crosses to the thread and back: the page's loop is woken for
        // what comes through it.
        page.eval(R"js(
            var crossing = new Worker(blob("onmessage = function (e) { var p = e.ports[0]; p.onmessage = function (m) { p.postMessage(m.data + 1); }; };"));
            var pair = new MessageChannel();
            pair.port1.onmessage = function (e) { log.push('through the port ' + e.data); };
            crossing.postMessage('port', [pair.port2]);
            pair.port1.postMessage(41);
        )js");
        auto const crossing = std::chrono::steady_clock::now();
        int const wakes_before = wakes.load();
        while (page.string("String(log.length)") != "3" && elapsed_ms(crossing) < 10000) {
            page.realm->run_pending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_EQ(page.string("log[2]"), std::string("through the port 42"));
        CHECK(wakes.load() > wakes_before);
        page.eval("crossing.terminate();");
        auto const crossed = std::chrono::steady_clock::now();
        while (threads.running() > 1 && elapsed_ms(crossed) < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // A worker deep in a loop is stopped by terminate(), and its thread ends.
        page.eval("var spinner = new Worker(blob('while (true) {}')); spinner.onerror = function () { log.push('spinner error'); };");
        auto const spinning = std::chrono::steady_clock::now();
        while (threads.running() < 2 && elapsed_ms(spinning) < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK_EQ(threads.running(), std::size_t(2));
        page.eval("spinner.terminate();");
        auto const stopping = std::chrono::steady_clock::now();
        while (threads.running() > 1 && elapsed_ms(stopping) < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::printf("    a spinning worker stopped %.0f ms after terminate() (band: under 5000)\n", elapsed_ms(stopping));
        CHECK_EQ(threads.running(), std::size_t(1));
        // The page ends with a worker still alive: its thread is told, not waited for here.
    }
    auto const ending = std::chrono::steady_clock::now();
    while (threads.running() > 0 && elapsed_ms(ending) < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_EQ(threads.running(), std::size_t(0));
}

// A database the page holds open, asked for at a higher version by a worker
// on a thread of its own: the version change is handed to the page from the
// worker's thread, and wakes the host although the page's side of IndexedDB
// was made before the page had any worker to be woken for.
void test_a_database_on_a_workers_thread_wakes_its_page()
{
    std::atomic<int> wakes { 0 };
    bindings::WorkerThreads threads;
    threads.set_wake([&wakes] { ++wakes; });
    {
        Page page("https://example.test/dir/page.html", {}, &threads);
        page.open();
        page.eval(listen);
        page.eval(R"js(
            var held;
            var opening = indexedDB.open('shared', 1);
            opening.onsuccess = function () {
                held = opening.result;
                held.onversionchange = function (e) { log.push('versionchange ' + e.oldVersion + '->' + e.newVersion); held.close(); };
                log.push('page opened');
            };
        )js");
        auto const opened = std::chrono::steady_clock::now();
        while (page.string("String(log.length)") != "1" && elapsed_ms(opened) < 5000) {
            page.realm->run_pending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_EQ(page.string("log.join()"), std::string("page opened"));

        // The worker says nothing until it has the database, and it cannot
        // have it before the page closes its connection: the page's loop is
        // not run here, so only the version change can wake the host.
        int const before = wakes.load();
        page.eval(R"js(
            var upgrader = listen(new Worker(blob("var r = indexedDB.open('shared', 2); r.onsuccess = function () { postMessage('worker opened ' + r.result.version); };")));
        )js");
        auto const asked = std::chrono::steady_clock::now();
        while (wakes.load() == before && elapsed_ms(asked) < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::printf("    the page was woken %.0f ms after its worker asked for version 2 (band: under 5000)\n", elapsed_ms(asked));
        CHECK(wakes.load() > before);
        CHECK_EQ(page.string("log.join()"), std::string("page opened"));

        auto const answered = std::chrono::steady_clock::now();
        while (page.string("String(log.length)") != "3" && elapsed_ms(answered) < 5000) {
            page.realm->run_pending();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_EQ(page.string("log.join()"), std::string("page opened,versionchange 1->2,worker opened 2"));
        page.eval("upgrader.terminate();");
        auto const ending = std::chrono::steady_clock::now();
        while (threads.running() > 0 && elapsed_ms(ending) < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK_EQ(threads.running(), std::size_t(0));
    }
}

void test_the_threads_end_with_their_host()
{
    auto const began = std::chrono::steady_clock::now();
    {
        bindings::WorkerThreads threads;
        Page page("https://example.test/dir/page.html", {}, &threads);
        page.open();
        page.eval("var w = new Worker(URL.createObjectURL(new Blob(['while (true) {}'])));");
        while (threads.running() < 1 && elapsed_ms(began) < 5000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // The page first, as a host ends: then the threads, which stop the
        // script still spinning and wait for it.
        page.realm.reset();
    }
    double const took = elapsed_ms(began);
    std::printf("    a host with a spinning worker ended in %.0f ms (band: under 5000)\n", took);
    CHECK(took < 5000);
}

} // namespace

int main()
{
    test_a_worker_answers_its_page();
    test_the_scope_is_not_a_window();
    test_what_a_message_carries();
    test_ports_cross_between_the_two();
    test_import_scripts();
    test_a_script_that_cannot_be_had();
    test_errors_reach_the_page();
    test_a_windows_error_event();
    test_timers_run_on_the_pages_clock();
    test_terminate_and_close();
    test_the_console_and_the_policy();
    test_a_worker_fetches();
    test_a_page_that_ends_takes_its_workers();
    test_a_worker_on_a_thread_of_its_own();
    test_a_database_on_a_workers_thread_wakes_its_page();
    test_the_threads_end_with_their_host();
    return test::report("test_workers");
}
