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
#include "net/Url.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sashfold::bindings {

// A border box in page coordinates (CSS px), as last laid out.
struct LayoutBox {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

// What the page's host provides to its scripts. Every hook is optional;
// a missing one answers with the least surprising nothing (no box, the
// attribute's value, no navigation).
struct HostHooks {
    // A classic script's source for <script src=…>, fetched on the page's
    // behalf and decoded to UTF-8; nullopt when it cannot be had.
    std::function<std::optional<std::string>(net::Url const&)> fetch_script;
    // The clock timers run on, in milliseconds. Wall time by default; the
    // replay and the tests give a virtual one so a timer fires when the
    // script says, deterministically.
    std::function<double()> now;
    // Whether a running script should be stopped now (the runners' deadline,
    // the shell's slow-script guard). Polled every few thousand steps.
    std::function<bool()> should_stop;
    // The border box of an element as laid out; nullopt for one that has no
    // box (display: none, or nothing laid out yet). The host lays the page
    // out first when the tree has changed since.
    std::function<std::optional<LayoutBox>(dom::Element const&)> layout_box;
    // The element's computed style, for getComputedStyle; null when unknown.
    std::function<css::ComputedStyle const*(dom::Element const&)> computed_style;
    // location.href = …, location.assign, a form the script submits.
    std::function<void(net::Url const&)> navigate;
    // window.scrollTo and friends, and where the page stands.
    std::function<void(int x, int y)> scroll_to;
    std::function<std::pair<int, int>()> scroll_position;
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
    // form.submit() and a submit event nobody cancelled: the host submits.
    std::function<void(dom::Element const& form, dom::Element const* submitter)> submit_form;
    // console.* output and every uncaught error, by level.
    std::function<void(std::string_view level, std::string_view message)> console;

    float viewport_width = 1024; // CSS px, for innerWidth and matchMedia
    float viewport_height = 768;
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
    int scripts_run = 0; // classic scripts prepared and executed
    int scripts_failed = 0; // of those, ended by an uncaught exception
    int scripts_skipped = 0; // a type the engine does not run (module, JSON, a template)
    int external_fetched = 0;
    int external_failed = 0;
    int timers_fired = 0;
    int events_dispatched = 0;
    int uncaught_errors = 0; // in any callback: scripts, timers, listeners
    double script_ms = 0; // time inside the engine, all entries together
};

class Realm final : public js::RootProvider, public html::ScriptRunner {
public:
    Realm(dom::Document& document, net::Url url, HostHooks hooks);
    ~Realm() override;
    Realm(Realm const&) = delete;
    Realm& operator=(Realm const&) = delete;

    js::Interpreter& interpreter();
    dom::Document& document();
    // The document's URL as scripts see it: history.pushState moves it.
    net::Url const& url() const;
    HostHooks& hooks();

    // --- Wrappers -----------------------------------------------------------
    // The script object for a node, made on first use and cached on it.
    js::Object* wrap(dom::Node&);
    js::Value wrap_or_null(dom::Node*);
    // The node behind a value; null when it is not a node wrapper of this
    // realm.
    dom::Node* node_of(js::Value const&) const;
    // The global object, which is the window.
    js::Object* window() const;

    // --- Scripts ------------------------------------------------------------
    // The parser's hook: prepares the script element (§4.12.1.1) and runs a
    // classic inline or external script now, or queues a deferred one.
    void run_script(dom::Element& script, html::TreeBuilder& builder) override;
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
    void note_mutation();

    ScriptStats const& stats() const;

    void trace_roots(js::Tracer&) override;

    struct Internals;
    Internals& internals() { return *m_internals; }

private:
    std::unique_ptr<Internals> m_internals;
};

}
