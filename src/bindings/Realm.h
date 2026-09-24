#pragma once

// The page's realm: one script engine per document, the window it runs
// in, the DOM it sees, and the event loop that drives it. The bindings are
// hand-written (WebIDL codegen comes later): each Web API object is a host
// object whose natives reach the tree behind it, and every node has at
// most one wrapper, cached on the node (ADR 0001).
//
// The host — the shell, --render, the test runners — makes a Realm over
// a document before parsing it, hands the realm to the parser as its
// ScriptRunner, calls document_parsed() when the parser is done, and then
// pumps run_pending() on the clock it gave. What a script changes is
// counted in mutation_count(): the host re-styles and re-lays out when it
// moves. Everything a script asks of the page that the engine cannot
// answer from the tree alone — a box's size, a control's live value,
// where to navigate — goes through HostHooks, each optional.

#include "css/ComputedStyle.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "js/Interpreter.h"
#include "net/Csp.h"
#include "net/Http.h"
#include "net/Url.h"
#include "platform/Audio.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold {
class Bitmap; // core/Bitmap.h
}

namespace sashfold::idb {
class Storage; // storage/IndexedDb.h
}

namespace sashfold::bindings {

class WorkerThreads; // Workers.h
class WorkerRuntime; // a worker's side of itself, private to the bindings

// A border box in page coordinates (CSS px), as last laid out.
struct LayoutBox {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

// The picture a <video> element shows now — the frame due at its playback
// position — for the painter. The bitmap stays the same object from frame
// to frame while the video's size holds and is written again in place:
// `shape` moves with a new bitmap (a new size, so lay out again), `frames`
// with every frame written (paint again).
struct VideoFrame {
    dom::Element const* element = nullptr;
    std::shared_ptr<Bitmap const> bitmap;
    std::uint64_t shape = 0;
    std::uint64_t frames = 0;
};

// A web storage area: the items in insertion order (key(n) counts on
// it), and a count that moves with every change made to them.
struct StorageArea {
    std::vector<std::pair<std::string, std::string>> items;
    std::uint64_t changes = 0;
};

// A document a frame is inside, for the framing rules: its URL without the
// fragment (about:srcdoc for an srcdoc document) and the URL of its origin.
struct FrameAncestor {
    std::string address;
    net::Url origin;
};

// An iframe's document as the host found it, once the framing rules let it
// through: its bytes and their type, the URL it has, the URL of its origin
// (an srcdoc document's is its parent's), whether the bytes are srcdoc text
// rather than a response to decode, its Content Security Policy, and the
// HTTP status it came with (200 for one never fetched): an object shows its
// fallback for an error status, where an iframe shows the error page.
struct FrameDocument {
    std::vector<std::uint8_t> bytes;
    std::string content_type;
    net::Url url;
    net::Url origin;
    bool srcdoc = false;
    std::optional<net::ContentSecurityPolicy> policy;
    int status = 200;
};

// Where an <img>'s picture stands, as the host that fetches it knows: it
// names none, it is on its way, it was had, or it could not be had or
// read (HTML §4.8.4's image request states, as `complete` asks for them).
enum class ImageState : std::uint8_t { None, Pending, Available, Broken };

// What the page's host provides to its scripts. Every hook is optional;
// a missing one answers with the least surprising nothing (no box, the
// attribute's value, no navigation).
struct HostHooks {
    // A classic script's source for <script src=…>, fetched on the page's
    // behalf and decoded to UTF-8; nullopt when it cannot be had. The
    // guard is the page's policy's say on the URL and on every redirect
    // hop: a host honours it before any request leaves.
    std::function<std::optional<std::string>(net::Url const&, net::RequestGuard const&)> fetch_script;
    // A request a script makes — fetch(), XMLHttpRequest, a module graph —
    // carried out on the page's behalf: the method, headers and body as
    // given, cookies only when the request allows them, the guard honoured
    // as above; the response with its status, headers, body and final
    // URL, or the error. Without it every such request is a network error.
    std::function<net::FetchResult(net::Url const&, net::ResourceRequest const&, net::RequestGuard const&)> fetch_resource;
    // The document's Content Security Policy, which the realm asks about
    // every inline script, handler attribute and string it would compile,
    // and whose guards it hands the fetches above; null for a page with
    // none. The realm adopts the policies its <meta> elements carry into
    // it as they appear.
    net::ContentSecurityPolicy* policy = nullptr;
    // The clock timers run on, in milliseconds. Wall time by default; the
    // replay and the tests give a virtual one so a timer fires when the
    // script says, deterministically.
    std::function<double()> now;
    // Whether a running script should be stopped now (the runners' deadline,
    // the shell's slow-script guard). Polled every few thousand steps.
    std::function<bool()> should_stop;
    // The ceiling on the page's script heap, in bytes (js::Heap::set_limit):
    // a page whose scripts hold more has them stopped, and its console says
    // so. 0 is none. The page's frames share its heap, and so its ceiling.
    std::size_t js_heap_limit = 0;
    // Told once, when the page's heap is found over that ceiling and its
    // scripts are stopped: the host's to tell the reader.
    std::function<void()> out_of_memory;
    // The page's own document gave itself another address without loading
    // anything — history.pushState, or replaceState when `push` is false:
    // the host's session history and its address field follow. A frame's
    // document does not say this; its entries are not the page's.
    std::function<void(net::Url const& url, bool push)> history_changed;
    // The border box of an element as laid out; nullopt for one that has no
    // box (display: none, or nothing laid out yet). The host lays the page
    // out first when the tree has changed since.
    std::function<std::optional<LayoutBox>(dom::Element const&)> layout_box;
    // The element's computed style, for getComputedStyle; null when unknown.
    std::function<css::ComputedStyle const*(dom::Element const&)> computed_style;
    // location.href = …, location.assign, a form the script submits.
    std::function<void(net::Url const&)> navigate;
    // window.scrollTo and friends, and where the document stands — the
    // document asking, since a frame's document scrolls inside its frame
    // and a frame's hooks are the page's.
    std::function<void(dom::Document const& document, int x, int y)> scroll_to;
    std::function<std::pair<int, int>(dom::Document const& document)> scroll_position;
    // document.cookie, read and written; without these the realm keeps its
    // own list, so a page that sets and reads a cookie sees what it set.
    std::function<std::string()> cookie_get;
    std::function<void(std::string_view)> cookie_set;
    // A form control's live value and checkedness (what the reader typed or
    // toggled), and setting them; without these the attributes answer.
    std::function<std::optional<std::string>(dom::Element const&)> control_value;
    std::function<void(dom::Element const&, std::string_view)> set_control_value;
    std::function<std::optional<bool>(dom::Element const&)> control_checked;
    std::function<void(dom::Element const&, bool)> set_control_checked;
    // Focus: element.focus(), element.blur() (null), document.activeElement.
    std::function<void(dom::Element const*)> focus;
    std::function<dom::Element const*()> focused;
    // An image's decoded size in CSS px, for naturalWidth and naturalHeight.
    std::function<std::optional<std::pair<int, int>>(dom::Element const&)> image_size;
    // Where an image element's picture stands, for `complete`: false while
    // it is Pending. Without the hook every image counts as complete.
    std::function<ImageState(dom::Element const&)> image_state;
    // Whether a picture's bytes decode: an object whose resource is a picture
    // that does not shows its fallback instead (HTML §4.8.7). Without it every
    // picture counts as one that decodes.
    std::function<bool(std::vector<std::uint8_t> const&)> image_decodes;
    // form.submit() and a submit event nobody cancelled: the host submits.
    std::function<void(dom::Element const& form, dom::Element const* submitter)> submit_form;
    // console.* output and every uncaught error, by level.
    std::function<void(std::string_view level, std::string_view message)> console;
    // The localStorage area for an origin, owned by the host and outliving
    // the realm: the page reads and writes it in place and counts each
    // change, so the host can write the area out when the count moves.
    // Without it a page's localStorage lives and dies with the document.
    std::function<StorageArea*(std::string const& origin)> local_storage;
    // The IndexedDB storage for an origin (storage/IndexedDb.h), owned by
    // the host and shared by every page and worker at that origin; it may
    // keep its databases in files under the profile. Without it the pages
    // of one agent share an in-memory storage per origin, which ends with
    // the page.
    std::function<std::shared_ptr<idb::Storage>(std::string const& origin)> indexed_db;
    // An iframe's document, for a realm of its own in this page's agent: what
    // its srcdoc or src names for the document at `base` under `policy` — or,
    // when `target` is given, what that URL names, the frame navigating there
    // on its own (its Location set, a reload) whatever its attributes say — as
    // the framing rules let it through for a frame inside `ancestors` (the
    // page first, the document at `base` last); nullopt when there is nothing
    // to show. Without it a frame's document has no realm, and
    // contentDocument is null.
    std::function<std::optional<FrameDocument>(dom::Element const& iframe, net::Url const& base,
        net::ContentSecurityPolicy* policy, std::vector<FrameAncestor> const& ancestors,
        std::optional<net::Url> const& target)> frame_document;

    // window.open's new window: the host shows `url` in a window of its own
    // that no script here reaches, as with noopener, and with no referrer
    // when `noreferrer`. Without it window.open answers null and says so on
    // the console.
    std::function<void(net::Url const& url, bool noreferrer)> open_window;
    // Whether the reader has acted on the page lately — a click or a key
    // within the last seconds (HTML §6.4.2, transient activation): a new
    // window opens only then. Without it every ask counts as a gesture.
    std::function<bool()> user_activation;
    // A page's element going full screen (true), or leaving it (false): the
    // host puts its chrome away and asks for the whole screen, or brings
    // them back. True when it did. Unset, no element goes full screen.
    std::function<bool(bool enter)> request_fullscreen;

    // Dedicated workers (HTML §10.2, Workers.h). With `worker_threads`, each
    // worker a document here starts runs on a thread of its own, and all it
    // asks of the network — its script, importScripts, fetch, XMLHttpRequest
    // — goes through `worker_fetch`, called ON that thread and for as long as
    // the WorkerThreads lives, whatever has become of the page: it must be
    // safe there and touch nothing of the page's. Without them a worker runs
    // on the page's own thread, a turn of its loop inside each run_pending(),
    // fetching through fetch_resource above and timed by `now`: what a test
    // or a headless run wants, since it keeps the run deterministic.
    WorkerThreads* worker_threads = nullptr;
    std::function<net::FetchResult(net::Url const&, net::ResourceRequest const&, net::RequestGuard const&)> worker_fetch;

    // Where a media element's sound goes. Unset, the machine's speakers
    // (platform::AudioDevice::open); a headless run and the tests give a
    // device that hears on the page's own clock, which plays nothing.
    std::function<std::unique_ptr<platform::AudioDevice>(platform::AudioFormat const&, std::string& error)> open_audio;

    // A line for each step of the event loop and the frames worth seeing — a
    // timer set or fired, a task run, a frame opened, closed or navigated, a
    // navigation asked, a message delivered — for a host that wants to know
    // why a page waits. Nothing by default.
    std::function<void(std::string_view)> trace;

    // Called before the viewport below is read, by a realm whose viewport may
    // have changed without anyone saying — a frame's, which is its container's
    // box in a layout its page owns. The realm's own; a host leaves it unset.
    std::function<void()> refresh_viewport;
    float viewport_width = 1024; // CSS px, for innerWidth and matchMedia
    float viewport_height = 768;
    // Device px per CSS px: devicePixelRatio, and the scale matchMedia's
    // context carries. Every box and position the hooks above answer with
    // is in CSS px; the host divides its device px by this before answering.
    float device_scale = 1;
    std::string user_agent; // navigator.userAgent
};

// How the host describes an event it fires into the page.
struct EventInit {
    bool bubbles = false;
    bool cancelable = false;
    bool composed = false;
};
struct MouseInit {
    int client_x = 0;
    int client_y = 0;
    int button = 0; // 0 left, 1 middle, 2 right
    int detail = 1; // the click count
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;
};
struct KeyInit {
    std::string key; // "a", "Enter", "ArrowLeft"
    std::string code; // "KeyA", "Enter", "ArrowLeft"
    int key_code = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;
    bool repeat = false;
};
struct InputInit {
    std::string data;
    std::string input_type = "insertText";
};

// The counts a host reports (--report, the dashboard).
struct ScriptStats {
    int scripts_run = 0; // scripts prepared and executed, classic and module alike
    int modules_run = 0; // of those, module scripts
    int scripts_failed = 0; // of those, ended by an uncaught exception
    int scripts_skipped = 0; // a type the engine does not run (JSON, a template, an import map)
    int scripts_refused = 0; // inline scripts and handlers the page's Content Security Policy refused
    int external_fetched = 0;
    int external_failed = 0;
    int timers_fired = 0;
    int events_dispatched = 0;
    int uncaught_errors = 0; // in any callback: scripts, timers, listeners
    // Every exception raised, whether the page caught it or not, and the
    // messages of the first few kinds. A page that tries something the
    // engine cannot do and catches the failure looks silent otherwise, so
    // this is what says it happened at all.
    std::uint64_t throws = 0;
    std::map<std::string, long> throw_messages;
    double script_ms = 0; // time inside the engine, all entries together
    // Custom elements (§4.13): the names the page defined, how many
    // elements became one of them, and how many constructors threw. A page
    // built out of components that defines many and upgrades none is a page
    // that will look empty, which is what the count is here to show.
    int custom_elements_defined = 0;
    int custom_elements_upgraded = 0;
    int custom_elements_failed = 0;
};

// Every <meta http-equiv=content-security-policy> in the document's head
// enforced (HTML §4.2.5.3): the realm does this before each check it
// makes, and a host that parsed without a realm does it once after.
void adopt_meta_policies(net::ContentSecurityPolicy& policy, dom::Document const& document);

// Which navigable container an element is: an iframe; the obsolete frame
// (HTML §16.3.2), which is an iframe without srcdoc and sandbox; an object or
// an embed, which has a window only for a document its data or src names
// (§4.8.6, §4.8.7); None for any other element, and for any of these names
// outside the HTML namespace.
enum class ContainerKind { None, IFrame, Frame, Object, Embed };
ContainerKind container_kind(dom::Element const& element);
inline bool is_navigable_container(dom::Element const& element) { return container_kind(element) != ContainerKind::None; }
// A container's srcdoc attribute, which only an iframe has; null for a frame
// with one, whose srcdoc names nothing.
dom::Attr const* container_srcdoc(dom::Element const& element);

// What a frame shows, as HTML reads its container's attributes: "srcdoc:"
// and the text, or "src:" and the URL resolved against `base`; empty for
// nothing — no src, an empty one, about:blank, or one that does not parse.
// The key a frame's document is opened and drawn under.
std::string frame_source(dom::Element const& container, net::Url const& base);

struct Agent;

class Realm final : public js::RootProvider, public html::ScriptRunner {
public:
    Realm(dom::Document& document, net::Url url, HostHooks hooks);
    ~Realm() override;
    Realm(Realm const&) = delete;
    Realm& operator=(Realm const&) = delete;

    js::Interpreter& interpreter();
    // SASHFOLD_THROW_TRACE=1: every exception raised in this realm's
    // interpreter, caught or not, on stderr with the functions that were
    // running. A page that tries what the engine cannot do and catches the
    // failure leaves no other trace, so this is what says which thing it
    // tried. SASHFOLD_EVAL_TRACE=1: every string the page turns into code.
    void trace_if_asked();
    dom::Document& document();
    // The document's URL as scripts see it: history.pushState moves it.
    net::Url const& url() const;
    // What the document's relative URLs are resolved against (HTML §2.4.3):
    // its own URL, or the href of its base element.
    net::Url const& base_url() const;
    // The URL of the document's origin: its own, or an srcdoc document's
    // parent's.
    net::Url const& origin_url() const;
    HostHooks& hooks();

    // --- Wrappers -----------------------------------------------------------
    // The script object for a node, made on first use and cached on it.
    js::Object* wrap(dom::Node&);
    js::Value wrap_or_null(dom::Node*);
    // The node behind a value; null when it is not a node wrapper made in
    // this realm's agent.
    dom::Node* node_of(js::Value const&) const;
    // The global object, which is the window.
    js::Object* window() const;

    // --- Scripts ------------------------------------------------------------
    // The parser's hook: prepares the script element (§4.12.1.1) and runs a
    // classic inline or external script now, or queues a deferred one.
    void run_script(dom::Element& script, html::TreeBuilder& builder) override;
    // The parser's other hook: an iframe it inserted gets its initial
    // about:blank document, and that document's load, before the next token.
    void frame_inserted(dom::Element& iframe) override;
    // Runs a <script> element inserted by a script (§4.12.1.1 step 1 of the
    // insertion steps): the same preparation, no parser.
    void run_inserted_script(dom::Element& script);
    // Runs global code in this realm; an uncaught exception goes to the
    // console and is counted. `name` is for messages.
    js::Outcome run(std::string_view utf8_source, std::string name);
    // The parser is done: readyState moves to interactive, the deferred
    // scripts run, DOMContentLoaded fires, then load; readyState is
    // complete.
    void document_parsed();
    std::string const& ready_state() const;

    // --- Events -------------------------------------------------------------
    using EventInit = bindings::EventInit;
    using MouseInit = bindings::MouseInit;
    using KeyInit = bindings::KeyInit;
    using InputInit = bindings::InputInit;
    // Fires an event of `type` at a node — or at the window when null —
    // through the capture, target and bubble phases. False when a listener
    // called preventDefault on a cancelable event: the host then skips
    // the default action.
    bool dispatch_event(dom::Node* target, std::string_view type, EventInit init = {});
    bool dispatch_mouse_event(dom::Node& target, std::string_view type, MouseInit const&);
    bool dispatch_key_event(dom::Node* target, std::string_view type, KeyInit const&);
    bool dispatch_input_event(dom::Node& target, std::string_view type, InputInit const& init = {});
    // The host navigates this window to `target` as the document would
    // navigate itself — a click on one of its links, one of its forms
    // submitted: a page hands it to its host, a frame schedules it on the
    // frame. False when the document's sandbox forbids it.
    bool navigate(net::Url const& target);
    // The name a link's target names this window by: its navigable's, or
    // nothing once it has none.
    std::optional<std::string> target_name() const;

    // --- The event loop -----------------------------------------------------
    // Runs every timer due by now on the hooks' clock, oldest first, and the
    // microtasks each leaves behind. A timer set while running waits for
    // the next call, so a host can paint between. True when anything ran.
    bool run_pending();
    // When the earliest pending timer is due, on the hooks' clock.
    std::optional<double> next_timer_due() const;
    bool has_pending_timers() const;
    // Runs the pending microtasks now (the checkpoint every entry from the
    // host performs on its way out).
    void perform_microtask_checkpoint();

    // Every change a script made to the tree, an attribute or a style: the
    // host re-styles and re-lays out when this moves.
    std::uint64_t mutation_count() const;
    // This document's count with those of its frames that have realms here,
    // at any depth: what a host that draws the frames watches.
    std::uint64_t tree_mutation_count() const;
    void note_mutation();

    // The pictures this document's video elements show now (not its
    // frames'): what a host draws them with, and watches to repaint.
    std::vector<VideoFrame> video_frames();
    // The host has the picture an <img> of this document names now, or knows
    // it cannot be had or read: the element's load or error event, fired in
    // a task of the event loop as HTML §4.8.4.3.4 queues it — never while the
    // script that set the source is still running. The host tells each
    // source it takes once.
    void image_settled(dom::Element const& image, bool available);
    // For a host on a virtual clock, which moves faster than pictures are
    // made: waits, up to `timeout_ms` of real time, until every video shows
    // the frame due at its position. How many do.
    std::size_t settle_video(double timeout_ms);
    // The reader left full screen (Esc): the page's element is let go and
    // told, as by exitFullscreen().
    void exit_fullscreen();

    ScriptStats const& stats() const;

    void trace_roots(js::Tracer&) override;

    struct Internals;
    Internals& internals() { return *m_internals; }

    // A frame's realm, in the agent of the page it is in: made by the realm of
    // the document its iframe is in, as that realm processes the iframe.
    Realm(Internals& parent, dom::Element& container, dom::Document& document, net::Url url, HostHooks hooks);
    // The realm of an iframe's document here, when it has one.
    Realm* frame_realm(dom::Element const& iframe);
    // The agent's stand-in for its ended realms: an empty document, no hooks.
    struct StandIn { };
    Realm(StandIn, Agent& agent, dom::Document& document);
    // A dedicated worker's realm (Workers.cpp): an agent of its own over a
    // document nothing shows, whose global object is a
    // DedicatedWorkerGlobalScope where a page's is its window. `url` is the
    // worker script's, which the scope's location shows and its relative URLs
    // are resolved against; `origin` is the origin it runs with, its owner's.
    struct WorkerScope {
        WorkerRuntime* runtime = nullptr;
        std::string name;
        net::Url origin;
    };
    Realm(WorkerScope scope, dom::Document& document, net::Url url, HostHooks hooks);

private:
    std::unique_ptr<Internals> m_internals;
};

}
