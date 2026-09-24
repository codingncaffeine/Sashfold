#include "bindings/Workers.h"

// Dedicated workers (HTML §10.2). Three parts:
//
//   the link      what a worker and the document that started it share, and
//                 all they share: the messages each has posted the other in
//                 their serialized form, the worker's console lines and
//                 uncaught errors on their way to the document, and the flags
//                 that end the worker. Behind a lock, since the two sides may
//                 be on two threads.
//   the runtime   the worker's side: its policy, a document nothing shows,
//                 a Realm over it whose global object is the worker's scope,
//                 and its event loop — a turn at a time on the document's
//                 thread, or for its whole life on a thread of its own.
//   the handle    the document's side: the Worker object kept alive, the
//                 runtime when it runs on this thread, and the delivery of
//                 what the worker says.
//
// Nothing of one side's heap is ever reached from the other: a worker on a
// thread of its own differs from one on the document's thread only in who
// calls its loop.

#include "bindings/Fetching.h"
#include "bindings/Internal.h"
#include "core/Ascii.h"
#include "js/Object.h"
#include "net/DataUrl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <pthread.h>

namespace sashfold::bindings {

namespace {

// A thread with a stack a script engine can run on. A thread the standard
// library starts has the platform's default — half a megabyte on macOS, one on
// Windows — and the engine measures its recursion against a budget of four: a
// worker's script a few hundred calls deep would run off the real stack before
// the engine said RangeError. POSIX threads, which every lane has, take a size.
class ScriptThread {
public:
    static constexpr std::size_t stack_bytes = 16u * 1024u * 1024u;

    explicit ScriptThread(std::function<void()> body)
        : m_body(std::make_unique<std::function<void()>>(std::move(body)))
    {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, stack_bytes);
        m_started = pthread_create(&m_thread, &attributes, &ScriptThread::run, m_body.get()) == 0;
        pthread_attr_destroy(&attributes);
        // Without a thread the body never runs: run it here, late but whole,
        // rather than leave a worker that never starts and never ends.
        if (!m_started)
            (*m_body)();
    }
    ScriptThread(ScriptThread&& other) noexcept
        : m_body(std::move(other.m_body))
        , m_thread(other.m_thread)
        , m_started(std::exchange(other.m_started, false))
    {
    }
    ScriptThread& operator=(ScriptThread&& other) noexcept
    {
        m_body = std::move(other.m_body);
        m_thread = other.m_thread;
        m_started = std::exchange(other.m_started, false);
        return *this;
    }
    void join()
    {
        if (m_started)
            pthread_join(m_thread, nullptr);
        m_started = false;
    }

private:
    static void* run(void* body)
    {
        (*static_cast<std::function<void()>*>(body))();
        return nullptr;
    }
    std::unique_ptr<std::function<void()>> m_body; // stays put while the thread runs it
    pthread_t m_thread {};
    bool m_started = false;
};

}

// --- The link --------------------------------------------------------------------------------

struct WorkerLink {
    std::mutex mutex;
    std::condition_variable for_worker; // a message has come, or the end

    // To the worker.
    std::deque<std::shared_ptr<SerializedMessage const>> to_worker;
    bool terminated = false; // by the document: terminate(), or its unloading
    // One of the worker's ports has been sent a message by another agent's.
    bool poked = false;
    // The same, as the worker's engine polls it while a script runs.
    std::atomic<bool> stop { false };

    // To the document.
    struct Word {
        enum class Kind : std::uint8_t {
            Message, // `message`
            Error, // an exception nothing in the worker dealt with: `text` says it, `detail` where
            Console, // a console line: `text`, at the level `detail`
            Failed, // the worker's script could not be had: `text` says why
            Closed, // the worker has ended; its last word
        };
        Kind kind = Kind::Message;
        std::shared_ptr<SerializedMessage const> message;
        std::string text;
        std::string detail;
    };
    std::deque<Word> to_owner;
    // Wakes the document's host; a worker on the document's thread needs none.
    std::function<void()> wake_owner;

    void say(Word word)
    {
        {
            std::lock_guard<std::mutex> const lock(mutex);
            to_owner.push_back(std::move(word));
        }
        if (wake_owner)
            wake_owner();
    }
    void console(std::string_view level, std::string_view message)
    {
        say(Word { Word::Kind::Console, nullptr, std::string(message), std::string(level) });
    }
    void poke()
    {
        {
            std::lock_guard<std::mutex> const lock(mutex);
            poked = true;
        }
        for_worker.notify_all();
    }
    // Ends the worker from outside: nothing more is delivered to it, and the
    // script it is running is stopped.
    void end()
    {
        {
            std::lock_guard<std::mutex> const lock(mutex);
            terminated = true;
        }
        stop.store(true, std::memory_order_relaxed);
        for_worker.notify_all();
    }
};

// --- The threads -----------------------------------------------------------------------------

struct WorkerThreads::Impl {
    mutable std::mutex mutex;
    std::function<void()> wake;
    struct Running {
        ScriptThread thread;
        std::shared_ptr<WorkerLink> link;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Running> running;

    // Joins the threads that have ended; the caller holds the lock.
    void reap()
    {
        for (auto it = running.begin(); it != running.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = running.erase(it);
            } else {
                ++it;
            }
        }
    }
};

WorkerThreads::WorkerThreads()
    : m_impl(std::make_unique<Impl>())
{
}

WorkerThreads::~WorkerThreads()
{
    std::vector<Impl::Running> running;
    {
        std::lock_guard<std::mutex> const lock(m_impl->mutex);
        running.swap(m_impl->running);
    }
    for (Impl::Running& worker : running)
        worker.link->end();
    for (Impl::Running& worker : running)
        worker.thread.join();
}

void WorkerThreads::set_wake(std::function<void()> wake)
{
    std::lock_guard<std::mutex> const lock(m_impl->mutex);
    m_impl->wake = std::move(wake);
}

std::size_t WorkerThreads::running() const
{
    std::lock_guard<std::mutex> const lock(m_impl->mutex);
    std::size_t count = 0;
    for (Impl::Running const& worker : m_impl->running)
        count += worker.done->load() ? 0 : 1;
    return count;
}

void WorkerThreads::start(std::shared_ptr<WorkerLink> link, std::function<void()> body)
{
    std::lock_guard<std::mutex> const lock(m_impl->mutex);
    m_impl->reap();
    auto done = std::make_shared<std::atomic<bool>>(false);
    ScriptThread thread([body = std::move(body), done] {
        body();
        done->store(true);
    });
    m_impl->running.push_back(Impl::Running { std::move(thread), std::move(link), std::move(done) });
}

void WorkerThreads::wake() const
{
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> const lock(m_impl->mutex);
        wake = m_impl->wake;
    }
    if (wake)
        wake();
}

// --- The runtime: the worker's side ----------------------------------------------------------

class WorkerRuntime {
public:
    // What the document hands over as it starts the worker: plain data and
    // hooks that are safe wherever the worker runs.
    struct Init {
        net::Url url;
        // The script's text when the document had it at hand — a blob: URL's
        // from its store, a data: URL's — and nothing to fetch.
        std::optional<std::string> source;
        // Why the worker cannot start, found by the document as it made it.
        std::optional<std::string> failure;
        std::string name;
        net::Url origin; // the origin the worker runs with: its owner's
        // A copy of the owner's policy as it stood, reporting through the
        // link: its worker-src judges the script's fetch, and a worker made
        // from a blob: or a data: URL runs under it.
        std::optional<net::ContentSecurityPolicy> owner_policy;
        // fetch_resource, now, should_stop, js_heap_limit and user_agent.
        HostHooks hooks;
    };

    WorkerRuntime(std::shared_ptr<WorkerLink> link, Init init)
        : m_link(std::move(link))
        , m_init(std::move(init))
    {
    }

    // A worker on the document's thread: one turn of its loop — its start
    // the first time — and whether anything ran.
    bool turn();
    // A worker on a thread of its own: its whole life.
    void live();

    bool started() const { return m_started; }
    bool over() const { return m_over; }
    // When its loop next has something to do, on its clock; none when only
    // a message can give it something.
    std::optional<double> next_due() const { return m_realm && !m_over ? m_realm->next_timer_due() : std::nullopt; }
    bool has_pending() const { return m_realm && !m_over && m_realm->has_pending_timers(); }

    // The scope's members.
    void post(std::shared_ptr<SerializedMessage const> message)
    {
        m_link->say(WorkerLink::Word { WorkerLink::Word::Kind::Message, std::move(message), {}, {} });
    }
    // close(): what is queued is dropped, and the script running now is the
    // last to run.
    void close() { m_over = true; }
    void report_error(std::string const& message, std::string_view where)
    {
        m_link->say(WorkerLink::Word { WorkerLink::Word::Kind::Error, nullptr, message, std::string(where) });
    }

private:
    void start();
    void fail(std::string why);
    void deliver(std::shared_ptr<SerializedMessage const> const& message);
    // After a turn: a script the host stopped, or a heap over its ceiling,
    // runs nothing again.
    void look_for_the_end();
    // The realm goes, on the thread it was made on, and the document is told.
    void finish();

    std::shared_ptr<WorkerLink> m_link;
    Init m_init;
    // In this order, so that the realm ends first.
    std::unique_ptr<net::ContentSecurityPolicy> m_policy;
    std::unique_ptr<dom::Document> m_document;
    std::unique_ptr<Realm> m_realm;
    bool m_started = false;
    bool m_over = false;
    bool m_finished = false;
};

void WorkerRuntime::fail(std::string why)
{
    m_over = true;
    m_link->say(WorkerLink::Word { WorkerLink::Word::Kind::Failed, nullptr, std::move(why), {} });
}

void WorkerRuntime::start()
{
    m_started = true;
    std::string const address = m_init.url.serialize();
    if (m_init.failure)
        return fail(*m_init.failure);

    std::string source;
    net::Url url = m_init.url;
    auto policy = std::make_unique<net::ContentSecurityPolicy>(url);
    auto const report = [link = m_link](std::string_view message) { link->console("error", message); };
    if (m_init.source) {
        source = std::move(*m_init.source);
        // A worker made from a blob: or a data: URL has its owner's policy.
        if (m_init.owner_policy)
            *policy = *m_init.owner_policy;
        policy->set_reporter(report);
    } else {
        if (!m_init.hooks.fetch_resource)
            return fail("The worker script at '" + address + "' could not be fetched: this host fetches nothing.");
        net::ResourceRequest request;
        request.destination = "script";
        net::RequestGuard const guard = m_init.owner_policy ? m_init.owner_policy->guard(net::ResourceKind::Worker) : net::RequestGuard {};
        net::FetchResult const result = m_init.hooks.fetch_resource(url, request, guard);
        if (!result.response)
            return fail("The worker script at '" + address + "' failed to load: " + result.error);
        net::FetchResponse const& response = *result.response;
        if (response.status < 200 || response.status > 299)
            return fail("The worker script at '" + address + "' failed to load: HTTP " + std::to_string(response.status));
        // A redirect may not leave the owner's origin (the fetch is same-origin).
        if (response.redirected && response.final_url.serialize_origin() != m_init.url.serialize_origin())
            return fail("The worker script at '" + address + "' was redirected to another origin.");
        // A worker's script is only ever a JavaScript MIME type (HTML §10.2.4).
        std::string const* const type = net::find_header(response.headers, "Content-Type");
        std::string const essence = type ? mime_essence(*type) : std::string();
        if (!is_javascript_type(essence))
            return fail("The worker script at '" + address + "' was not run: its MIME type ('" + essence + "') is not a JavaScript one.");
        source.assign(response.body.begin(), response.body.end());
        if (response.redirected)
            url = response.final_url;
        // The worker's own policy is what its response carries.
        policy = std::make_unique<net::ContentSecurityPolicy>(url);
        policy->set_reporter(report);
        for (net::Header const& header : response.headers) {
            if (ascii_ci_equals(header.name, "content-security-policy"))
                policy->add_header(header.value, false);
            else if (ascii_ci_equals(header.name, "content-security-policy-report-only"))
                policy->add_header(header.value, true);
        }
    }

    m_policy = std::move(policy);
    m_document = std::make_unique<dom::Document>();
    HostHooks hooks = m_init.hooks;
    hooks.policy = m_policy.get();
    hooks.console = [link = m_link](std::string_view level, std::string_view message) { link->console(level, message); };
    m_realm = std::make_unique<Realm>(Realm::WorkerScope { this, m_init.name, m_init.origin }, *m_document, url, std::move(hooks));
    // A worker on a thread of its own sleeps between turns: a message for
    // one of its ports, from another agent's, wakes it.
    if (m_link->wake_owner)
        m_realm->internals().agent.wake_loop = [link = m_link] { link->poke(); };
    m_realm->run(source, url.serialize());
    look_for_the_end();
}

void WorkerRuntime::deliver(std::shared_ptr<SerializedMessage const> const& message)
{
    Realm::Internals& in = m_realm->internals();
    Realm::Internals::HostEntry const host(in.agent);
    js::Interpreter::RealmScope const inside(in.interpreter, in.realm_record);
    js::Interpreter::Roots const roots(in.interpreter);
    js::Object* const scope = in.window_proxy();
    std::optional<Deserialized> const received = structured_deserialize(in, *message);
    if (!received) {
        deliver_message_error(in, scope, "", js::Value::null());
        return;
    }
    deliver_message(in, scope, received->value, "", js::Value::null(), received->transferred);
}

void WorkerRuntime::look_for_the_end()
{
    if (m_over || !m_realm)
        return;
    js::Interpreter& interpreter = m_realm->interpreter();
    if (interpreter.terminated() || interpreter.out_of_memory())
        m_over = true;
}

void WorkerRuntime::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    m_realm.reset();
    m_document.reset();
    m_policy.reset();
    m_link->say(WorkerLink::Word { WorkerLink::Word::Kind::Closed, nullptr, {}, {} });
}

bool WorkerRuntime::turn()
{
    if (m_finished)
        return false;
    bool ran = false;
    if (!m_started) {
        start();
        ran = true;
    }
    if (!m_over) {
        std::deque<std::shared_ptr<SerializedMessage const>> messages;
        {
            std::lock_guard<std::mutex> const lock(m_link->mutex);
            messages.swap(m_link->to_worker);
        }
        for (std::shared_ptr<SerializedMessage const> const& message : messages) {
            if (m_over)
                break;
            deliver(message);
            look_for_the_end();
            ran = true;
        }
    }
    if (!m_over) {
        ran = m_realm->run_pending() || ran;
        look_for_the_end();
    }
    if (m_over)
        finish();
    return ran;
}

void WorkerRuntime::live()
{
    start();
    while (!m_over) {
        std::deque<std::shared_ptr<SerializedMessage const>> messages;
        {
            std::unique_lock<std::mutex> lock(m_link->mutex);
            auto const wanted = [this] { return m_link->terminated || m_link->poked || !m_link->to_worker.empty(); };
            if (!wanted()) {
                std::optional<double> const due = m_realm->next_timer_due();
                if (!due) {
                    m_link->for_worker.wait(lock, wanted);
                } else {
                    // A timer that is always due — setInterval(f, 0) — is not
                    // let spin the thread: a turn a millisecond at the most.
                    double const wait_ms = std::max(1.0, *due - m_realm->internals().now());
                    m_link->for_worker.wait_for(lock, std::chrono::duration<double, std::milli>(wait_ms), wanted);
                }
            }
            if (m_link->terminated)
                break;
            m_link->poked = false;
            messages.swap(m_link->to_worker);
        }
        for (std::shared_ptr<SerializedMessage const> const& message : messages) {
            if (m_over)
                break;
            deliver(message);
            look_for_the_end();
        }
        if (!m_over) {
            m_realm->run_pending();
            look_for_the_end();
        }
    }
    finish();
}

void worker_report_error(Realm::Internals& in, std::string const& message, std::string_view where)
{
    if (in.worker != nullptr)
        in.worker->report_error(message, where);
}

// --- The handle: the document's side ---------------------------------------------------------

// A Worker (HTML §10.2.6.3), an EventTarget; its worker's handle, until the
// worker has ended or the document has terminated it.
class WorkerObject final : public EventTargetObject {
public:
    explicit WorkerObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    WorkerHandle* handle = nullptr;
};

struct WorkerHandle {
    WorkerHandle(Realm::Internals& the_owner, std::shared_ptr<WorkerLink> the_link, WorkerObject& the_object)
        : owner(the_owner)
        , link(std::move(the_link))
        , object(the_owner.interpreter.heap(), js::Value::object(&the_object))
    {
        the_object.handle = this;
        owner.agent.worker_handles.push_back(this);
    }
    ~WorkerHandle()
    {
        link->end();
        if (object.value().is_object())
            static_cast<WorkerObject*>(object.value().as_object())->handle = nullptr;
        std::erase(owner.agent.worker_handles, this);
    }
    WorkerHandle(WorkerHandle const&) = delete;
    WorkerHandle& operator=(WorkerHandle const&) = delete;

    // terminate(), and the document unloading: the worker is told to end, and
    // nothing it said or says is delivered any more.
    void terminate()
    {
        terminated = true;
        ended = true;
        link->end();
    }
    // The loop's turn: whether anything ran.
    bool pump();
    void hear(WorkerLink::Word const& word);

    Realm::Internals& owner; // the realm of the document that started it
    std::shared_ptr<WorkerLink> link;
    // The Worker, alive for as long as its worker may have something for it.
    js::Persistent object;
    // The worker itself, when it runs on this thread.
    std::unique_ptr<WorkerRuntime> runtime;
    bool terminated = false;
    bool ended = false; // nothing more will come of it: the next sweep lets it go
};

void WorkerHandleDeleter::operator()(WorkerHandle* handle) const
{
    delete handle;
}

void WorkerHandle::hear(WorkerLink::Word const& word)
{
    using Kind = WorkerLink::Word::Kind;
    js::Interpreter& interpreter = owner.interpreter;
    js::Interpreter::RealmScope const inside(interpreter, owner.realm_record);
    js::Interpreter::Roots const roots(interpreter);
    js::Object* const target = object.value().as_object();
    switch (word.kind) {
    case Kind::Message: {
        std::optional<Deserialized> const received = structured_deserialize(owner, *word.message);
        if (!received) {
            deliver_message_error(owner, target, "", js::Value::null());
            return;
        }
        deliver_message(owner, target, received->value, "", js::Value::null(), received->transferred);
        return;
    }
    case Kind::Error: {
        // The worker's exception as an ErrorEvent at its Worker; one nothing
        // cancels there is the document's console's to say.
        EventObject* const event = owner.new_event("ErrorEvent", "error", false, true);
        interpreter.root(js::Value::object(event));
        event->is_trusted = true;
        event->message = word.text;
        event->filename = word.detail;
        if (owner.dispatch(*event, target))
            owner.console("error", word.text + (word.detail.empty() ? std::string() : " (worker " + word.detail + ")"));
        return;
    }
    case Kind::Console:
        owner.console(word.detail, word.text);
        return;
    case Kind::Failed: {
        owner.console("error", word.text);
        EventObject* const event = owner.new_event("Event", "error", false, false);
        interpreter.root(js::Value::object(event));
        event->is_trusted = true;
        owner.dispatch(*event, target);
        return;
    }
    case Kind::Closed:
        ended = true;
        return;
    }
}

bool WorkerHandle::pump()
{
    bool ran = false;
    if (runtime && !terminated)
        ran = runtime->turn();
    std::deque<WorkerLink::Word> words;
    {
        std::lock_guard<std::mutex> const lock(link->mutex);
        words.swap(link->to_owner);
    }
    for (WorkerLink::Word const& word : words) {
        if (terminated)
            break;
        hear(word);
        ran = true;
    }
    return ran;
}

bool pump_workers(Agent& agent)
{
    bool ran = false;
    // A copy: what is delivered may start workers, and end them.
    std::vector<WorkerHandle*> const handles = agent.worker_handles;
    auto const alive = [&agent](WorkerHandle const* handle) {
        return std::find(agent.worker_handles.begin(), agent.worker_handles.end(), handle) != agent.worker_handles.end();
    };
    for (WorkerHandle* const handle : handles) {
        if (alive(handle))
            ran = handle->pump() || ran;
    }
    // The sweep: a worker that has ended, its last word delivered, is let go
    // of by the realm that held it — and its Worker with it, if no script
    // holds that.
    for (WorkerHandle* const handle : handles) {
        if (!alive(handle) || !handle->ended)
            continue;
        std::vector<WorkerHandlePtr>& held = handle->owner.workers;
        std::erase_if(held, [handle](WorkerHandlePtr const& entry) { return entry.get() == handle; });
    }
    return ran;
}

namespace {

// Whether a handle has something for the loop's next turn, now.
bool due_now(WorkerHandle const& handle)
{
    if (handle.ended)
        return true; // the sweep
    std::lock_guard<std::mutex> const lock(handle.link->mutex);
    if (!handle.link->to_owner.empty())
        return true;
    if (!handle.runtime || handle.terminated)
        return false;
    return !handle.runtime->started() || !handle.link->to_worker.empty();
}

}

std::optional<double> workers_next_due(Agent const& agent, double now)
{
    std::optional<double> due;
    for (WorkerHandle const* const handle : agent.worker_handles) {
        if (due_now(*handle))
            return now;
        if (!handle->runtime || handle->terminated)
            continue;
        std::optional<double> const own = handle->runtime->next_due();
        if (own && (!due || *own < *due))
            due = own;
    }
    return due;
}

bool workers_pending(Agent const& agent)
{
    for (WorkerHandle const* const handle : agent.worker_handles) {
        if (due_now(*handle))
            return true;
        if (handle->runtime && !handle->terminated && handle->runtime->has_pending())
            return true;
    }
    return false;
}

void terminate_workers(Realm::Internals& in)
{
    for (WorkerHandlePtr const& handle : in.workers)
        handle->terminate();
}

// --- Worker, for a window --------------------------------------------------------------------

namespace {

std::optional<WorkerObject*> this_worker(js::Interpreter& interp, js::Value const& this_value)
{
    if (this_value.is_object()) {
        if (auto* worker = dynamic_cast<WorkerObject*>(this_value.as_object()))
            return worker;
    }
    return interp.throw_type_error("Illegal invocation");
}

double steady_now_ms()
{
    using namespace std::chrono;
    return static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

}

void install_workers(Realm::Internals& in)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Object* const worker = define_interface(in, "Worker", in.prototype("EventTarget"),
        [](js::Interpreter& interp, Args args, js::Object*) -> Native {
            Realm::Internals& internals = internals_of(interp);
            if (args.empty())
                return interp.throw_type_error("Failed to construct 'Worker': 1 argument required, but only 0 present.");
            std::optional<std::string> const text = internals.to_utf8(args[0]);
            if (!text)
                return std::nullopt;
            // WorkerOptions: the type, classic or module, and the name.
            std::string name;
            bool module = false;
            js::Value const options = js::argument(args, 1);
            if (options.is_object()) {
                std::optional<js::Value> const type = interp.get(options, "type");
                if (!type)
                    return std::nullopt;
                if (!type->is_undefined()) {
                    std::optional<std::string> const type_text = internals.to_utf8(*type);
                    if (!type_text)
                        return std::nullopt;
                    if (*type_text != "classic" && *type_text != "module")
                        return interp.throw_type_error("Failed to construct 'Worker': The provided value '" + *type_text
                            + "' is not a valid enum value of type WorkerType.");
                    module = *type_text == "module";
                }
                std::optional<js::Value> const name_value = interp.get(options, "name");
                if (!name_value)
                    return std::nullopt;
                if (!name_value->is_undefined()) {
                    std::optional<std::string> name_text = internals.to_utf8(*name_value);
                    if (!name_text)
                        return std::nullopt;
                    name = std::move(*name_text);
                }
            } else if (!options.is_nullish()) {
                return interp.throw_type_error("Failed to construct 'Worker': The provided value is not of type 'WorkerOptions'.");
            }
            std::optional<net::Url> const url = net::parse_url(*text, &internals.base_url());
            if (!url)
                return internals.throw_dom_exception("SyntaxError", "Failed to construct 'Worker': '" + *text + "' is not a valid URL.");

            WorkerRuntime::Init init;
            init.url = *url;
            init.name = std::move(name);
            init.origin = url->scheme == "data" ? *url : internals.origin_url;
            std::string const address = url->serialize();
            if (url->scheme == "blob") {
                // What the URL names is in this agent's store, or nowhere.
                auto const entry = internals.agent.blob_urls.find(url->serialize(true));
                if (entry != internals.agent.blob_urls.end())
                    init.source = std::string(entry->second.bytes.begin(), entry->second.bytes.end());
                else
                    init.failure = "The worker script at '" + address + "' failed to load: the blob: URL names nothing.";
            } else if (url->scheme == "data") {
                std::optional<net::DataUrlPayload> const payload = net::parse_data_url(*url);
                if (payload)
                    init.source = std::string(payload->bytes.begin(), payload->bytes.end());
                else
                    init.failure = "The worker script at '" + address + "' failed to load: not a data: URL.";
            } else {
                // A worker's script comes from its owner's origin (HTML
                // §10.2.6.3); a local page may name a local script.
                std::string const own = internals.origin_url.serialize_origin();
                bool const local = url->scheme == "file" && internals.origin_url.scheme == "file";
                if (!local && (own == "null" || url->serialize_origin() != own))
                    return internals.throw_dom_exception("SecurityError",
                        "Failed to construct 'Worker': Script at '" + address + "' cannot be accessed from origin '" + own + "'.");
            }
            if (module && !init.failure)
                init.failure = "The worker at '" + address + "' was not started: module workers are not written yet.";

            auto link = std::make_shared<WorkerLink>();
            // The owner's policy as it stands, the <meta> ones adopted: its
            // worker-src judges the script here and now when there is nothing
            // to fetch, and on the way when there is.
            internals.adopt_meta_policies();
            if (internals.hooks.policy != nullptr) {
                if (init.source && !init.failure) {
                    net::RequestGuard const guard = internals.request_guard(net::ResourceKind::Worker);
                    if (guard.refusal) {
                        if (std::optional<std::string> const refused = guard.refusal(*url, false))
                            init.failure = *refused;
                    }
                }
                init.owner_policy = *internals.hooks.policy;
                init.owner_policy->set_reporter([link](std::string_view message) { link->console("error", message); });
            }

            WorkerThreads* const threads = internals.hooks.worker_fetch ? internals.hooks.worker_threads : nullptr;
            init.hooks.user_agent = internals.hooks.user_agent;
            init.hooks.js_heap_limit = internals.hooks.js_heap_limit;
            if (threads != nullptr) {
                init.hooks.fetch_resource = internals.hooks.worker_fetch;
                init.hooks.now = [] { return steady_now_ms(); };
                init.hooks.should_stop = [link] { return link->stop.load(std::memory_order_relaxed); };
                link->wake_owner = [threads] { threads->wake(); };
                // A port of this document's entangled with one of a worker's
                // is sent its messages from that worker's thread.
                if (!internals.agent.wake_loop)
                    internals.agent.wake_loop = [threads] { threads->wake(); };
            } else {
                init.hooks.fetch_resource = internals.hooks.fetch_resource;
                init.hooks.now = internals.hooks.now;
                init.hooks.should_stop = [link, inner = internals.hooks.should_stop] {
                    return link->stop.load(std::memory_order_relaxed) || (inner && inner());
                };
            }

            auto* const object = interp.heap().allocate<WorkerObject>(internals.prototype("Worker"));
            WorkerHandlePtr handle(new WorkerHandle(internals, link, *object));
            internals.trace("worker started: " + address);
            if (threads != nullptr) {
                threads->start(link, [link, init = std::move(init)]() mutable {
                    WorkerRuntime runtime(link, std::move(init));
                    runtime.live();
                });
            } else {
                handle->runtime = std::make_unique<WorkerRuntime>(link, std::move(init));
            }
            internals.workers.push_back(std::move(handle));
            return js::Value::object(object);
        },
        1);

    define_operation(interpreter, *worker, "postMessage", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<WorkerObject*> const found = this_worker(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<std::vector<js::Value>> const transfer = transfer_or_options(internals, js::argument(args, 1));
        if (!transfer)
            return std::nullopt;
        // Serialized here and now, whether or not anything will receive it,
        // in the form another agent may hold: a port it transfers becomes the
        // end of a channel.
        std::shared_ptr<SerializedMessage const> const message
            = structured_serialize_for_another_agent(internals, js::argument(args, 0), *transfer);
        if (!message)
            return std::nullopt;
        WorkerHandle* const handle = (*found)->handle;
        if (handle == nullptr || handle->terminated)
            return js::Value::undefined();
        {
            std::lock_guard<std::mutex> const lock(handle->link->mutex);
            handle->link->to_worker.push_back(message);
        }
        handle->link->for_worker.notify_all();
        return js::Value::undefined();
    });
    define_operation(interpreter, *worker, "terminate", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<WorkerObject*> const found = this_worker(interp, this_value);
        if (!found)
            return std::nullopt;
        if (WorkerHandle* const handle = (*found)->handle) {
            internals_of(interp).trace("worker terminated");
            handle->terminate();
        }
        return js::Value::undefined();
    });
    static constexpr std::string_view worker_events[] = { "message", "messageerror", "error" };
    define_event_handlers(in, *worker, worker_events);
}

// --- The worker's scope ----------------------------------------------------------------------

namespace {

// The names the window's installers give a global object that a worker's
// scope has too, as they are: the events, the timers, the encoders, fetch
// and XMLHttpRequest, messaging between ports, URLs. Every other one is
// taken out, and the scope's own members put in.
constexpr std::string_view shared_names[] = {
    "EventTarget", "Event", "CustomEvent", "ProgressEvent", "MessageEvent", "ErrorEvent",
    "origin", "isSecureContext",
    "setTimeout", "setInterval", "clearTimeout", "clearInterval", "queueMicrotask", "reportError",
    "atob", "btoa", "structuredClone", "performance", "crypto",
    "DOMException", "URL", "URLSearchParams",
    "TextEncoder", "TextDecoder", "Blob", "File",
    "Headers", "FormData", "Request", "Response", "fetch",
    "XMLHttpRequestEventTarget", "XMLHttpRequestUpload", "XMLHttpRequest",
    "AbortSignal", "AbortController", "MessagePort", "MessageChannel", "Origin",
};

// An accessor of one prototype given to another with its getter alone: what
// a worker's location and navigator show is what a URL and a window's
// navigator do, read-only.
void share_getter(js::Interpreter& interpreter, js::Object& from, js::Object& to, std::string_view name)
{
    std::optional<js::PropertyDescriptor> const found = from.get_own_property(interpreter.key(name));
    if (!found || !found->get || *found->get == nullptr)
        return;
    js::PropertyDescriptor descriptor;
    descriptor.get = *found->get;
    descriptor.set = static_cast<js::Object*>(nullptr);
    descriptor.enumerable = true;
    descriptor.configurable = true;
    (void)to.define_own_property(interpreter.key(name), descriptor);
}

}

void install_worker_scope(Realm::Internals& in, std::vector<js::PropertyKey> const& language_globals)
{
    js::Interpreter& interpreter = in.interpreter;
    js::Heap::NoCollect const guard(interpreter.heap());
    js::Object* const global = interpreter.global();

    // What a window's navigator and a URL answer with, kept before the
    // window is taken out.
    js::Object* const navigator_proto = in.prototype("Navigator");
    js::Object* const url_proto = in.prototype("URL");

    for (js::PropertyKey const& key : global->own_keys()) {
        if (!key.is_atom() || std::find(language_globals.begin(), language_globals.end(), key) != language_globals.end())
            continue;
        std::string const name = key.as_atom()->to_utf8();
        if (std::find(std::begin(shared_names), std::end(shared_names), name) == std::end(shared_names))
            global->remove_own(key);
    }

    // The scope's prototype chain (WebIDL §3.7.4): the scope,
    // DedicatedWorkerGlobalScope.prototype, WorkerGlobalScope.prototype,
    // EventTarget.prototype. A [Global] interface's members are the global
    // object's own.
    js::Object* const scope_proto = define_interface(in, "WorkerGlobalScope", in.prototype("EventTarget"));
    js::Object* const dedicated_proto = define_interface(in, "DedicatedWorkerGlobalScope", scope_proto);
    global->set_prototype(dedicated_proto);

    define_getter(in, *global, "self", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return js::Value::object(interp.global_this());
    });
    define_getter(in, *global, "name", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return internals.string(internals.worker_name);
    });

    // WorkerLocation (HTML §10.3.3): the script's URL, part by part.
    js::Object* const location_proto = define_interface(in, "WorkerLocation", nullptr);
    if (url_proto != nullptr) {
        for (std::string_view const part : { "href", "origin", "protocol", "host", "hostname", "port", "pathname", "search", "hash" })
            share_getter(interpreter, *url_proto, *location_proto, part);
    }
    define_operation(interpreter, *location_proto, "toString", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        auto* const location = this_value.is_object() ? dynamic_cast<UrlObject*>(this_value.as_object()) : nullptr;
        if (location == nullptr)
            return interp.throw_type_error("Illegal invocation");
        return internals_of(interp).string(location->url.serialize());
    });
    in.window_values["location"] = js::Value::object(interpreter.heap().allocate<UrlObject>(location_proto, in.url));
    define_getter(in, *global, "location", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).window_values["location"];
    });

    // WorkerNavigator (HTML §10.3.2).
    js::Object* const worker_navigator_proto = define_interface(in, "WorkerNavigator", nullptr);
    if (navigator_proto != nullptr) {
        for (std::string_view const member : { "userAgent", "appCodeName", "appName", "appVersion", "platform", "product", "language",
                 "languages", "onLine", "hardwareConcurrency", "deviceMemory" })
            share_getter(interpreter, *navigator_proto, *worker_navigator_proto, member);
    }
    in.window_values["navigator"] = js::Value::object(interpreter.heap().allocate<PlainPlatformObject>(worker_navigator_proto));
    define_getter(in, *global, "navigator", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return internals_of(interp).window_values["navigator"];
    });

    // postMessage(message, transfer) and (message, options): to the Worker
    // in the document that started this worker.
    define_operation(interpreter, *global, "postMessage", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        std::optional<std::vector<js::Value>> const transfer = transfer_or_options(internals, js::argument(args, 1));
        if (!transfer)
            return std::nullopt;
        std::shared_ptr<SerializedMessage const> const message
            = structured_serialize_for_another_agent(internals, js::argument(args, 0), *transfer);
        if (!message)
            return std::nullopt;
        if (internals.worker != nullptr)
            internals.worker->post(message);
        return js::Value::undefined();
    });
    define_operation(interpreter, *global, "close", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        if (internals.worker != nullptr)
            internals.worker->close();
        return js::Value::undefined();
    });

    // importScripts(...urls) (HTML §10.3.1): every URL parsed before any is
    // fetched; then each fetched, judged and run in turn, here and now. One
    // that cannot be had is a NetworkError; what a script throws is thrown
    // on to the caller.
    define_operation(interpreter, *global, "importScripts", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        std::vector<net::Url> urls;
        for (js::Value const& argument : args) {
            std::optional<std::string> const text = internals.to_utf8(argument);
            if (!text)
                return std::nullopt;
            std::optional<net::Url> url = net::parse_url(*text, &internals.base_url());
            if (!url)
                return internals.throw_dom_exception("SyntaxError",
                    "Failed to execute 'importScripts' on 'WorkerGlobalScope': The URL '" + *text + "' is invalid.");
            urls.push_back(std::move(*url));
        }
        for (net::Url const& url : urls) {
            std::string const address = url.serialize();
            auto const network_error = [&internals, &address](std::string const& why) {
                internals.console("error", "importScripts: '" + address + "' " + why);
                return internals.throw_dom_exception("NetworkError",
                    "Failed to execute 'importScripts' on 'WorkerGlobalScope': The script at '" + address + "' failed to load.");
            };
            std::string source;
            if (url.scheme == "data") {
                std::optional<net::DataUrlPayload> const payload = net::parse_data_url(url);
                if (!payload)
                    return network_error("is not a data: URL");
                source.assign(payload->bytes.begin(), payload->bytes.end());
            } else if (url.scheme == "blob") {
                // A blob: URL this worker made; one its document made is in
                // the document's store, which nothing here reaches yet.
                auto const entry = internals.agent.blob_urls.find(url.serialize(true));
                if (entry == internals.agent.blob_urls.end())
                    return network_error("names nothing in this worker's store");
                source.assign(entry->second.bytes.begin(), entry->second.bytes.end());
            } else {
                if (!internals.hooks.fetch_resource)
                    return network_error("could not be fetched: this host fetches nothing");
                net::ResourceRequest request;
                request.destination = "script";
                net::FetchResult const result = internals.hooks.fetch_resource(url, request, internals.request_guard(net::ResourceKind::Script));
                if (!result.response)
                    return network_error("failed to load: " + result.error);
                if (result.response->status < 200 || result.response->status > 299)
                    return network_error("failed to load: HTTP " + std::to_string(result.response->status));
                std::string const* const type = net::find_header(result.response->headers, "Content-Type");
                std::string const essence = type ? mime_essence(*type) : std::string();
                if (!is_javascript_type(essence))
                    return network_error("was not run: its MIME type ('" + essence + "') is not a JavaScript one");
                source.assign(result.response->body.begin(), result.response->body.end());
            }
            ++internals.stats.external_fetched;
            js::Outcome const outcome = interp.run_script(source, address);
            if (!outcome.ok) {
                if (interp.terminated())
                    return std::nullopt;
                return interp.throw_value(outcome.value);
            }
        }
        return js::Value::undefined();
    });

    static constexpr std::string_view scope_events[] = { "message", "messageerror", "error", "rejectionhandled",
        "unhandledrejection", "languagechange", "offline", "online" };
    define_event_handlers(in, *global, scope_events);
}

}
