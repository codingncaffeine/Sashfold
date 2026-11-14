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
#include "dom/Forms.h"
#include "html/TreeBuilder.h"
#include "js/Interpreter.h"
#include "net/Csp.h"
#include "net/Http.h"
#include "net/Url.h"
#include "net/WebSocket.h"
#include "platform/Audio.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

namespace sashfold::media {
class VideoPipeline; // media/VideoPipeline.h
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
    // The used padding of an element's own box (percentages resolved against
    // its containing block); 0 for a box of runs.
    float padding_top = 0;
    float padding_right = 0;
    float padding_bottom = 0;
    float padding_left = 0;
};

// A box that scrolls (CSSOM View §6): how far its content is moved now and
// how far it can be on each axis, in CSS px.
struct ElementScroll {
    float x = 0;
    float y = 0;
    float range_x = 0;
    float range_y = 0;
    // Whether the box scrolls; one that does not reports where its content
    // reaches past it (its scrollWidth and scrollHeight), at offset 0.
    bool scrolls = true;
};

// The picture a <video> element shows now — the frame due at its playback
// position — for the painter. The bitmap stays the same object from frame
// to frame while the video's size holds and is written again in place:
// `shape` moves with a new bitmap (a new size, so lay out again), `frames`
// with every frame written (paint again).
//
// Where the host shows video itself (HostHooks::video_layers) `layer` is
// where the pictures come from, by their own clock: the bitmap then says
// only how large the picture is, the page's painting leaves its place
// open, and a new frame is nothing for the page to paint.
struct VideoFrame {
    dom::Element const* element = nullptr;
    std::shared_ptr<Bitmap const> bitmap;
    std::uint64_t shape = 0;
    std::uint64_t frames = 0;
    std::shared_ptr<media::VideoPipeline> layer = nullptr;
};

// A web storage area: the items in insertion order (key(n) counts on
// it), and a count that moves with every change made to them. An area a
// host gives is shared by every page of its origin, and those pages may be
// on threads of their own: whoever reads or changes an area holds
// storage_areas_mutex() while it does.
struct StorageArea {
    std::vector<std::pair<std::string, std::string>> items;
    std::uint64_t changes = 0;
};
std::mutex& storage_areas_mutex();

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

// An <img>'s decoded picture as the host holds it, with where its bytes
// came from: the URL its redirects ended at, and the response's
// Access-Control-Allow-Origin and Access-Control-Allow-Credentials values.
// A canvas that draws it judges from these whether the picture is
// CORS-same-origin; with `from` unknown it counts as another origin's.
struct HostPicture {
    std::shared_ptr<Bitmap const> bitmap;
    std::optional<net::Url> from;
    std::string allow_origin;
    bool allow_credentials = false;
};

// The steady clock as a script may read it, in milliseconds: coarsened to
// 100 µs, the resolution HR-Time's "coarsen time" asks of a context that
// is not cross-origin isolated (none is, here). A finer clock in a process
// that writes machine code makes cache-timing attacks practical
// (js-JIT-DESIGN.md §7). What `now` is when a host gives none.
double coarse_clock_ms();

// A notification a page asks to be shown (bindings/Notifications.cpp): what
// of it the desktop is told.
struct NotificationRequest {
    std::string title;
    std::string body;
    std::string tag; // one with the same tag, of the same site, is replaced
    // The picture the page gave it (NotificationOptions.icon), resolved
    // against the document's base; empty for none. The host fetches it as
    // the document's own picture would be and shows the notice once it has
    // come — or has failed: a notice is never held back for its picture.
    std::string icon;
    bool require_interaction = false;
    bool silent = false;
};

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
    // A WebSocket the page opens, through the host's loader (its lists, its
    // cookies, its user agent) and under the guard given; null with
    // `refused` saying why when it may not go. Without it none opens.
    std::function<std::shared_ptr<net::WebSocket>(net::WebSocket::Request, net::WebSocket::Events, net::RequestGuard const&, std::string& refused)>
        open_websocket;
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
    // The time origin of a document's realm, on the `now` clock: when its
    // navigation began (HTML §7.4.2.3, navigation params' time origin),
    // so performance.now() counts the document's own fetch as every engine
    // does. Unset, the realm's making is the origin (a frame, a test).
    std::optional<double> time_origin;
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
    // The boxes of part of a Text node's data, UTF-16 offsets from and to,
    // as laid out (find_text_boxes): none when it is not drawn.
    std::function<std::vector<LayoutBox>(dom::Node const&, std::uint32_t, std::uint32_t)> text_boxes;
    // The element's computed style, for getComputedStyle; null when unknown.
    std::function<css::ComputedStyle const*(dom::Element const&)> computed_style;
    // The element uppermost at a point of the viewport, in CSS px from its
    // top left (elementFromPoint): what the pointer there would land on, an
    // inline <svg>'s shape included; null for none.
    std::function<dom::Element const*(float client_x, float client_y)> element_from_point;
    // Runs the callback with the page's own fonts (its @font-face rules) in
    // the process's font manager, and nothing else laying out meanwhile: what
    // a canvas measures and draws text through. Without it the text is
    // shaped with whatever fonts the process holds at the moment.
    std::function<void(std::function<void()> const&)> with_fonts;
    // location.href = …, location.assign, a form the script submits.
    std::function<void(net::Url const&)> navigate;
    // The page moved to a fragment of its own document (location.hash, a
    // link to #something): the realm has the new URL already and fires
    // hashchange; the host records the history entry, shows the address and
    // scrolls to the fragment — it loads nothing.
    std::function<void(net::Url const&)> navigated_to_fragment;
    // window.scrollTo and friends, and where the document stands — the
    // document asking, since a frame's document scrolls inside its frame
    // and a frame's hooks are the page's.
    std::function<void(dom::Document const& document, int x, int y)> scroll_to;
    std::function<std::pair<int, int>(dom::Document const& document)> scroll_position;
    // Element scrolling (scrollTop and the like): where an element's box stands
    // and how far its content reaches, nothing for an element with no box; and
    // a move of a box that scrolls, which the host holds within its reach.
    // CSS px.
    std::function<std::optional<ElementScroll>(dom::Element const& element)> element_scroll;
    std::function<void(dom::Element const& element, float x, float y)> scroll_element;
    // document.cookie, read and written; without these the realm keeps its
    // own list, so a page that sets and reads a cookie sees what it set.
    std::function<std::string()> cookie_get;
    std::function<void(std::string_view)> cookie_set;
    // A form control's live value and checkedness (what the reader typed or
    // toggled), and setting them; without these the attributes answer.
    std::function<std::optional<std::string>(dom::Element const&)> control_value;
    std::function<void(dom::Element const&, std::string_view)> set_control_value;
    // A text control's selection as the host keeps it (the user's caret and
    // what they selected, dom::forms::TextSelection), and setting it; without
    // these the realm keeps its own.
    // A file input's chosen files as the host keeps them, with a number that
    // moves each time they change (what keeps input.files the same object
    // until then), and setting them (input.value = "" lets them go).
    // choose_files opens the desktop's chooser for the input — a script's
    // click() in a user activation, showPicker() — its answer coming later
    // as input and change.
    std::function<std::optional<std::pair<std::vector<dom::forms::ChosenFile>, std::uint64_t>>(dom::Element const&)> control_files;
    std::function<void(dom::Element const&, std::vector<dom::forms::ChosenFile>)> set_control_files;
    std::function<void(dom::Element&)> choose_files;
    std::function<std::optional<dom::forms::TextSelection>(dom::Element const&)> control_selection;
    std::function<void(dom::Element const&, dom::forms::TextSelection const&)> set_control_selection;
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
    // An <img>'s decoded picture as the host holds it, for a canvas that
    // draws it; a null bitmap while it has none. Without it the realm
    // fetches and decodes the picture itself when a canvas asks for it.
    std::function<HostPicture(dom::Element const&)> image_picture;
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
    // The sessionStorage area for an origin, kept by the host for the tab
    // (the top-level browsing context): it lasts through the tab's
    // navigations and reloads, and goes with the tab. Without it a page's
    // sessionStorage lives and dies with the document.
    std::function<StorageArea*(std::string const& origin)> session_storage;
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

    // Notifications. What the reader has said of this page's site:
    // "granted", "denied", or "default" — not asked yet, and may be.
    // Unset, "denied": a host that shows none asks no one.
    std::function<std::string()> notification_permission;
    // The page asks to be allowed: the host asks the reader, when and how
    // it sees fit, and gives the answer to
    // Realm::notification_permission_answered — "default" when the reader
    // turned the question away.
    std::function<void()> ask_notification_permission;
    // Shows a notification and names it (0: it cannot be shown). What
    // becomes of it comes back through Realm::notification_event.
    std::function<std::uint64_t(NotificationRequest const&)> show_notification;
    std::function<void(std::uint64_t id)> close_notification;

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

    // A fetch on a thread of the host's loader, for a media element reading
    // a file in parts while it plays: `done` is called ON that thread with
    // what came back, and may ask for the next part from there. The agent's
    // remote tasks (and its wake loop) carry the bytes to the page's thread.
    // Unset, the element fetches its file whole on its own thread (the
    // script runner: deterministic).
    std::function<bool(net::Url const&, net::ResourceRequest const&, net::RequestGuard const&,
        std::function<void(net::FetchResult const&)> done)>
        fetch_in_background;

    // Where a media element's sound goes. Unset, the machine's speakers
    // (platform::AudioDevice::open); a headless run and the tests give a
    // device that hears on the page's own clock, which plays nothing.
    std::function<std::unique_ptr<platform::AudioDevice>(platform::AudioFormat const&, std::string& error)> open_audio;

    // What makes a media element's video into pictures. Unset, the machine's
    // own video hardware (media::VideoPipeline::open); a test gives a
    // pipeline over a decoder of its own, so that what becomes of the
    // pictures is tested on a machine that has no such hardware too.
    std::function<std::shared_ptr<media::VideoPipeline>(std::string const& codec_id, std::span<std::uint8_t const> codec_private, std::string& error)> open_video;

    // The host shows a video's pictures itself, from the pipeline that
    // makes them and at the moment each comes due (VideoFrame::layer), as a
    // browser's compositor does: playback is then as smooth as the display
    // however long the page's scripts keep its thread. Unset, the element
    // takes each picture at its own tick and the page paints it — what a
    // headless run wants, where the page's clock is the only clock.
    bool video_layers = false;

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
    int buttons = 0; // the buttons held while the event fires (UI Events §5.3.3)
    int movement_x = 0; // since the last move (Pointer Lock §4); the realm fills these in
    int movement_y = 0;
    int detail = 1; // the click count
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;
    bool pointer = false; // a PointerEvent (pointerId 1, "mouse") rather than a MouseEvent
    dom::Element* related_target = nullptr; // mouseover/out, enter/leave: the other element
};
// What a release gave: the click, where it fired (the nearest common
// ancestor of where the button went down and came up), and whether its
// default action — following a link, pressing a control — may proceed.
struct PointerRelease {
    dom::Element* click_target = nullptr;
    bool proceed = true;
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
    // How another thread wakes this page's loop when it hands the agent a
    // task (a media file's part, a worker's word): set by a host whose
    // pages run on threads of their own.
    void set_wake_loop(std::function<void()> wake);
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
    bool shadow_root_refused(dom::Element& host) override;
    void custom_element_inserted(dom::Element& element) override;
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
    // The reader moved the focus — a press on a control, Tab — and the
    // host has already recorded it: the events a script's focus() fires,
    // blur and focusout at the element that had it, focus and focusin at
    // the one that has it now. Either may be null, or another realm's (a
    // frame's), which is then not this realm's to tell.
    void focus_moved(dom::Element const* previous, dom::Element const* now);
    // The reader's answer to HostHooks::ask_notification_permission:
    // "granted", "denied" or "default". Every requestPermission() waiting
    // on it is settled with it.
    void notification_permission_answered(std::string_view state);
    // This realm's own waiting asks settled (the page's answer, told to a
    // frame of its origin that asked through it).
    void answer_notification_asks(std::string_view state);
    // What became of a notification HostHooks::show_notification named:
    // "show" (the desktop has it), "click", "close", or "error" (it could
    // not be shown). The event of that name fires at the Notification.
    // False when it was a `click` the page canceled: the reader's press is
    // then the page's alone, and the host brings nothing forward (§2.7
    // "activate"); true otherwise.
    bool notification_event(std::uint64_t id, std::string_view type);
    // The host's window (or what it shows the page in) has another size, in
    // CSS px: innerWidth and the media queries read it from now, and a task
    // fires resize at the window and change at each MediaQueryList whose
    // answer moved (HTML §8.1.7.3 "run the resize steps", "evaluate media
    // queries and report changes").
    void viewport_changed(float width, float height);
    // A box the reader or a script moved — null for the page itself: it hears
    // scroll in a task, once however often it moved before the task ran
    // (CSSOM View §13.1); the page's goes to the document and bubbles to the
    // window.
    void scrolled(dom::Element const* element);
    // A form the reader submits (a submit button pressed, Enter in a field):
    // its constraints checked interactively unless it or the submitter says
    // novalidate, then a SubmitEvent the page may cancel. False when the
    // host must not submit (HTML §4.10.21.3).
    bool submit_requested(dom::Element& form, dom::Element* submitter);
    bool dispatch_mouse_event(dom::Node& target, std::string_view type, MouseInit const&);
    // The pointer over the page (UI Events §5.3, Pointer Events §5.2): the
    // host says which element is under it now, or none, and where; the
    // realm fires what moved — pointerout and pointerleave up the chain the
    // pointer left, pointerover and pointerenter down the chain it entered,
    // the mouse counterparts of each after them, then pointermove and
    // mousemove — and keeps the element for next time.
    // `moved` false: the pointer stood still and the page changed under it
    // — the boundary events fire, the move events do not.
    void pointer_moved(dom::Element* target, MouseInit const&, bool moved = true);
    // A button pressed over an element: pointerdown, then mousedown unless
    // the pointerdown was canceled. False when the mousedown was canceled:
    // the host then skips what a press does on its own (focus, selection).
    bool pointer_pressed(dom::Element& target, MouseInit const&);
    // A press that became a drag (HTML §6.11.5, Pointer Events §4.2.6):
    // pointercancel where it went down, and no mouseup or click to come.
    void pointer_cancelled(MouseInit const&);
    // The button released over an element, or over nothing: pointerup and
    // mouseup there (at the pressed element when over nothing), then the
    // click at the nearest common ancestor of the pressed and the released
    // element — auxclick for a button other than the first; dblclick after
    // a second click within half a second and a few pixels.
    PointerRelease pointer_released(dom::Element* target, MouseInit const&);
    bool dispatch_key_event(dom::Node* target, std::string_view type, KeyInit const&);
    bool dispatch_input_event(dom::Node& target, std::string_view type, InputInit const& init = {});
    // The reader changed a text control's selection (the window's caret
    // keys, a drag, a double click): selectionchange at the control, and
    // select when text is selected now (HTML §4.10.18.6, "when the user
    // changes the element's selection").
    void text_control_selection_changed(dom::Element& control, bool selected);
    // The document's selection (Selection.cpp): its anchor and focus, none
    // when it has no range; set by the window between two points (each in
    // this document, an offset past a node's length clamped); taken away.
    // Every change queues a selectionchange; the version moves with each.
    struct SelectionPoints {
        dom::Node* anchor = nullptr;
        std::uint32_t anchor_offset = 0;
        dom::Node* focus = nullptr;
        std::uint32_t focus_offset = 0;
    };
    std::optional<SelectionPoints> selection() const;
    void set_selection(dom::Node& anchor, std::uint32_t anchor_offset, dom::Node& focus, std::uint32_t focus_offset);
    void clear_selection();
    std::uint64_t selection_version() const;
    // A reader's edit of rich text (Editing.cpp), named by its inputType
    // ("insertText", "deleteContentBackward", "insertParagraph", ...):
    // beforeinput at the editing host the selection is in, which may cancel
    // it, the change, then input. False when the selection is in no editing
    // host, and the window should do what the key does elsewhere.
    bool edit(std::string_view input_type, std::string const& data = {});

    // The clipboard events (Clipboard API §5): `copy`, `cut` or `paste`
    // fired at the focused editable element, else at the element the
    // selection starts in, else at the body, with a clipboardData the page
    // reads (paste) or fills (copy, cut). Cancelled: the page took the
    // action as its own, and `items` is what it set (a copy or cut whose
    // listener set nothing leaves the clipboard as it was).
    struct ClipboardItems {
        std::string text;
        std::string html;
        std::vector<std::uint8_t> png = {}; // a picture: clipboardData's file "image.png"
    };
    struct ClipboardOutcome {
        bool cancelled = false;
        std::optional<ClipboardItems> items;
    };
    ClipboardOutcome clipboard_event(std::string_view type, ClipboardItems const* pasted = nullptr);
    // The document's selection as markup for the clipboard's text/html
    // (empty when nothing is selected).
    std::string selection_markup();
    // A paste into the editing host the selection is in: the markup when
    // there is some and the host takes it, else the text, as
    // insertFromPaste with the data on beforeinput's dataTransfer.
    bool paste_into_editing_host(ClipboardItems const& items);

    // An input method's composition in the editing host the selection is in
    // (UI Events §5.7, Input Events): compose() starts one (compositionstart)
    // or updates it — compositionupdate, then beforeinput and input of
    // insertCompositionText with the composing text put in the DOM where
    // the selection was, as Chrome does. end_composition() ends it with the
    // text committed (nullopt: what is composed stays) and compositionend.
    // False when there is no editing host, or no composition to end.
    bool compose(std::string const& text);
    bool end_composition(std::optional<std::string> const& committed);
    bool composing() const;
    // Where the composing text is, to be underlined: its text node and the
    // code units it spans there.
    struct CompositionSpan {
        dom::Node* node = nullptr;
        std::uint32_t start = 0;
        std::uint32_t end = 0;
    };
    std::optional<CompositionSpan> composition_span() const;

    // Drag and drop (HTML §6.11.5), the page's side of it. drag_start()
    // fires dragstart at the source with the store holding `data` (the
    // page may change it, or cancel: false, and there is no drag);
    // `kind` is "selection", "link" or "element". drag_event() fires drag,
    // dragenter, dragover, dragleave, drop or dragend at a node with the
    // store in the mode that event gives it and dropEffect as the standard
    // sets it first; true when not cancelled. The drag's operation as the
    // page left it (dropEffect after dragenter/dragover), its data, and an
    // end that lets the store go.
    bool drag_start(dom::Element& source, MouseInit const&, std::vector<std::pair<std::string, std::string>> data, std::string kind);
    bool drag_event(dom::Node& target, std::string_view type, MouseInit const&, std::string const& operation = "none");
    // A drag from another program over the page (HTML §6.11.5 with no source
    // here: no dragstart, drag or dragend): a store with the formats it
    // offers, their data empty until the drop, and the files it carries —
    // seen as "Files" in types while it moves, as Files in drop.
    void external_drag_start(std::vector<std::string> const& formats, std::vector<dom::forms::ChosenFile> files);
    // The drag's data, read from the program at the drop, put in the store
    // before the drop is fired.
    void external_drag_data(std::vector<std::pair<std::string, std::string>> data);
    bool dragging() const;
    std::string drag_drop_effect() const;
    std::string drag_effect_allowed() const;
    std::vector<std::pair<std::string, std::string>> drag_data() const;
    dom::Element* drag_source() const;
    std::string drag_kind() const;
    void drag_finished();
    // A drop into the editing host the selection is in (put at the drop
    // point first): insertFromDrop with the data on beforeinput's
    // dataTransfer, the dragged selection deleted first (deleteByDrag) when
    // it moves within the document. False when there was no editing host.
    bool drop_into_editing_host(std::vector<std::pair<std::string, std::string>> const& data, bool move_from_selection,
        std::optional<SelectionPoints> const& dragged);
    // The editing host around a node: its outermost contenteditable element,
    // the body in designMode; null when the node is not editable.
    dom::Element* editing_host(dom::Node& node);
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
    // The host has the resource an element of this document names now — an
    // <img>'s picture, a <link rel=stylesheet>'s sheet — or knows it cannot
    // be had or read: the element's load or error event, fired in a task of
    // the event loop as HTML §4.8.4.3.4 and §4.6.7 queue it — never while
    // the script that set the source is still running. The host tells each
    // source it takes once.
    void resource_settled(dom::Element const& element, bool available);
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
