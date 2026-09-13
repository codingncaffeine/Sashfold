#pragma once

// Private to the bindings: the host object classes and the realm's
// internals the installer files share. Nothing outside src/bindings
// includes this.

#include "bindings/Realm.h"
#include "js/Object.h"
#include "js/Runtime.h"
#include "js/Strings.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::bindings {

using Args = std::span<js::Value const>;
using Native = std::optional<js::Value>;

// One registration made by addEventListener, or the slot an on<type>
// handler occupies.
struct Listener {
    js::Value callback; // a function, or an object whose handleEvent is called
    std::uint64_t id = 0; // unique in the realm: a dispatch finds it again after the list changed
    bool capture = false;
    bool once = false;
    bool passive = false;
};

struct ListenerEntry {
    std::string type;
    Listener listener;
};

// An event handler (§8.1.8.1): the function an on<type> property holds,
// or the one compiled from an on<type> content attribute, remembered with
// the attribute text it came from so a changed attribute recompiles.
struct EventHandler {
    js::Value function; // undefined = null handler
    std::string source; // the attribute text, when from_attribute
    bool from_attribute = false;
};

using HandlerMap = std::unordered_map<std::string, EventHandler>;

// The base of every host object that receives events: its listeners and
// handlers live here, traced by the collector with the object.
class EventTargetObject : public js::Object {
public:
    explicit EventTargetObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::vector<ListenerEntry> listeners;
    HandlerMap handlers;
    void trace(js::Tracer&) override;
    std::size_t size_in_bytes() const override
    {
        return Object::size_in_bytes() + listeners.size() * sizeof(ListenerEntry);
    }
};

// A node's one wrapper (ADR 0001 §1). It names its realm through the realm's
// record, which outlives the realm: a frame's realm that has ended hands the
// record to the agent's stand-in, so a wrapper made by an ended realm still
// answers, from nothing.
class NodeWrapper final : public EventTargetObject {
public:
    NodeWrapper(js::Object* prototype, js::RealmRecord& record, dom::Node& node);
    ~NodeWrapper() override;
    dom::Node& node() const { return *m_node; }
    Realm& realm() const { return *static_cast<Realm*>(m_record->host_defined); }
    // A node adopted into another realm's document is that realm's from then on.
    void rehome(js::RealmRecord& record) { m_record = &record; }
    // A frame's realm ending before the heap its wrappers live in lets each go
    // of its node; no native accepts a detached wrapper again.
    void detach() { m_node = nullptr; }
    bool detached() const { return m_node == nullptr; }
    void trace(js::Tracer&) override;

private:
    js::RealmRecord* m_record;
    dom::Node* m_node;
};

// An Event (DOM §2.2) and its subclasses: one C++ class, the interface
// told apart by the prototype and the fields it fills.
class EventObject final : public js::Object {
public:
    explicit EventObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    enum class Phase : std::uint8_t { None = 0, Capturing = 1, AtTarget = 2, Bubbling = 3 };

    std::string type;
    js::Value target; // an EventTarget, or undefined before dispatch (read as null)
    js::Value current_target;
    js::Value related_target; // MouseEvent, FocusEvent
    js::Value detail_value; // CustomEvent.detail
    Phase phase = Phase::None;
    bool bubbles = false;
    bool cancelable = false;
    bool composed = false;
    bool default_prevented = false;
    bool stop_propagation = false;
    bool stop_immediate = false;
    bool is_trusted = false;
    bool dispatching = false;
    bool initialized = false;
    bool in_passive_listener = false;
    double time_stamp = 0;
    // UIEvent and below.
    int detail = 0;
    int client_x = 0;
    int client_y = 0;
    int screen_x = 0;
    int screen_y = 0;
    int button = 0;
    int buttons = 0;
    bool ctrl_key = false;
    bool shift_key = false;
    bool alt_key = false;
    bool meta_key = false;
    // KeyboardEvent.
    std::string key;
    std::string code;
    int key_code = 0;
    bool repeat = false;
    // InputEvent.
    std::string data;
    std::string input_type;
    // WheelEvent.
    double delta_x = 0;
    double delta_y = 0;
    // ProgressEvent.
    bool length_computable = false;
    double loaded = 0;
    double total = 0;
    // MessageEvent: the data is detail_value; the origin, the source, the ports.
    std::string origin;
    js::Value source_value;
    js::Value ports;

    void trace(js::Tracer& tracer) override;
};

// A host object over one element, reached through the element's wrapper: the
// wrapper is traced with it, and once the wrapper is detached — its
// document gone with a frame — there is no element to read, and the realm
// answering is the wrapper's, the one its node's document has now.
class ElementBackedObject : public js::Object {
public:
    ElementBackedObject(js::Object* prototype, NodeWrapper* the_wrapper)
        : Object(prototype, Class::Host)
        , wrapper(the_wrapper)
    {
    }
    NodeWrapper* wrapper; // null for a computed style of no element
    dom::Element* element() const
    {
        return wrapper && !wrapper->detached() ? static_cast<dom::Element*>(&wrapper->node()) : nullptr;
    }
    void trace(js::Tracer&) override;
};

// A DOMTokenList over one attribute of an element (classList, relList).
class TokenListObject final : public ElementBackedObject {
public:
    TokenListObject(js::Object* prototype, NodeWrapper& the_wrapper, std::string the_attribute)
        : ElementBackedObject(prototype, &the_wrapper)
        , attribute(std::move(the_attribute))
    {
    }
    std::string attribute;
    Realm::Internals& internals() const;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const&) const override;
};

// A CSSStyleDeclaration: an element's style attribute read and written
// property by property, or — read-only — its computed style.
class StyleDeclarationObject final : public ElementBackedObject {
public:
    StyleDeclarationObject(js::Object* prototype, js::RealmRecord& the_record, NodeWrapper* the_wrapper, bool is_computed)
        : ElementBackedObject(prototype, the_wrapper)
        , record(&the_record)
        , computed(is_computed)
    {
    }
    js::RealmRecord* record; // the realm that made it, for a declaration of no element
    bool computed;
    Realm::Internals& internals() const;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;
    void trace(js::Tracer&) override;
};

// element.dataset: the data-* attributes as properties.
class DatasetObject final : public ElementBackedObject {
public:
    DatasetObject(js::Object* prototype, NodeWrapper& the_wrapper)
        : ElementBackedObject(prototype, &the_wrapper)
    {
    }
    Realm::Internals& internals() const;
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const&) const override;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;
    bool delete_property(js::PropertyKey const&) override;
    std::vector<js::PropertyKey> own_keys() const override;
};

// localStorage and sessionStorage: a map of strings, reachable as
// properties too (storage.key = "v"). The area is the host's when it
// gave one (localStorage, per origin, outliving the document), else the
// object's own.
class StorageObject final : public js::Object {
public:
    explicit StorageObject(js::Object* prototype, StorageArea* backing = nullptr)
        : Object(prototype, Class::Host)
        , m_backing(backing)
    {
    }
    StorageArea& area() { return m_backing ? *m_backing : m_own; }
    StorageArea const& area() const { return m_backing ? *m_backing : m_own; }
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const&) const override;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;
    bool delete_property(js::PropertyKey const&) override;
    std::vector<js::PropertyKey> own_keys() const override;
    std::string const* find(std::string_view key) const;
    void put_item(std::string key, std::string value);
    bool remove_item(std::string_view key);
    void clear_items();

private:
    StorageArea* m_backing = nullptr;
    StorageArea m_own;
};

// A URL object (`new URL(…)`) and its searchParams.
class UrlObject final : public js::Object {
public:
    UrlObject(js::Object* prototype, net::Url the_url)
        : Object(prototype, Class::Host)
        , url(std::move(the_url))
    {
    }
    net::Url url;
    js::Object* search_params = nullptr;
    void trace(js::Tracer& tracer) override;
};

class SearchParamsObject final : public js::Object {
public:
    explicit SearchParamsObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    UrlObject* owner = nullptr; // the URL whose query this list is, when it is one
    void trace(js::Tracer& tracer) override;
};

// A Blob or a File (Binary.cpp): bytes with a type, and a File's name and
// modification time.
class BlobObject final : public js::Object {
public:
    explicit BlobObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::vector<std::uint8_t> bytes;
    std::string type;
    bool is_file = false;
    std::string name;
    double last_modified = 0;
    std::size_t size_in_bytes() const override { return sizeof(*this) + bytes.capacity(); }
};

// An AbortSignal (Tasks.cpp): whether it has fired and why.
class AbortSignalObject final : public EventTargetObject {
public:
    explicit AbortSignalObject(js::Object* prototype)
        : EventTargetObject(prototype)
    {
    }
    bool aborted = false;
    js::Value reason; // undefined until aborted
    void trace(js::Tracer& tracer) override
    {
        EventTargetObject::trace(tracer);
        tracer.visit(reason);
    }
};

struct Timer {
    int id = 0;
    double due = 0; // on the hooks' clock
    std::uint64_t sequence = 0; // ties broken by creation order
    double interval = -1; // repeats every so many ms; below zero = once
    bool animation_frame = false; // requestAnimationFrame: the callback takes a timestamp
    std::unique_ptr<js::Persistent> callback; // a function, or a string of source
    std::vector<std::unique_ptr<js::Persistent>> arguments;
    Realm::Internals* owner = nullptr; // the realm that set it
};

// A task queued for the event loop's next turn — a response to deliver, a
// message to post — with the realm that queued it.
struct Task {
    std::uint64_t sequence = 0;
    Realm::Internals* owner = nullptr;
    std::function<void()> run;
};

// An origin as the cross-origin rules compare them (HTML §7.1.1, "same
// origin-domain"): its serialization, "null" for an opaque one; its scheme;
// and the domain document.domain gave its document, if it did.
struct OriginSnapshot {
    std::string serialized;
    std::string scheme;
    std::optional<std::string> domain;
};

struct ChildFrame;

// One member a window or a location shows a script of another origin (HTML
// §7.2.3.2, CrossOriginProperties): a method, or an accessor with a getter,
// a setter or both.
struct CrossOriginProperty {
    std::string_view name;
    bool method = false;
    bool needs_get = false;
    bool needs_set = false;
    int length = 0; // a method's
};

// An entry of [[CrossOriginPropertyDescriptorMap]] (HTML §7.2.3.1): the
// property one realm was shown for one member of one window or location, so
// that the same realm asking again is shown the same functions.
struct CrossOriginEntry {
    js::RealmRecord* current = nullptr;
    js::RealmRecord* object_realm = nullptr;
    std::string_view name;
    js::PropertyDescriptor descriptor;
};

// What WindowProxy and Location share: every internal method asks first
// whether the script running has the origin of the window behind the object
// (HTML §7.2.3), forwarding to what the object stands for when it does, and
// showing CrossOriginProperties alone when it does not. Both derive from the
// engine's proxy so that the interpreter routes their throwing internal
// methods as it routes a Proxy's; the target stands in as the handler, and
// no trap ever runs.
class CrossOriginObject : public js::ProxyObject {
public:
    CrossOriginObject(js::Object& target, js::RealmRecord& window_realm);
    // The realm of the window behind the object, the one the rules judge.
    js::RealmRecord& record() const { return *m_record; }

    using js::ProxyObject::get_own_property;
    using js::ProxyObject::define_own_property;
    using js::ProxyObject::has_property;
    using js::ProxyObject::delete_property;
    using js::ProxyObject::own_keys;
    // OrdinaryHasProperty (§10.1.7.1) over the internal methods, which throw.
    std::optional<bool> has_property(js::Interpreter&, js::PropertyKey const&) override;
    void trace(js::Tracer&) override;

protected:
    virtual std::span<CrossOriginProperty const> cross_origin_properties() const = 0;
    // A member's property as the window or location was made with it.
    virtual std::optional<js::PropertyDescriptor> original_member(js::Interpreter&, std::string_view name) const = 0;
    // CrossOriginGetOwnPropertyHelper, CrossOriginGet, CrossOriginSet,
    // CrossOriginOwnPropertyKeys and CrossOriginPropertyFallback (HTML
    // §7.2.3.4 to §7.2.3.7).
    std::optional<js::PropertyDescriptor> cross_origin_property(js::Interpreter&, js::PropertyKey const&);
    std::optional<js::Value> cross_origin_get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver);
    std::optional<bool> cross_origin_set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver);
    std::vector<js::PropertyKey> cross_origin_keys(js::Interpreter&) const;
    static std::optional<std::optional<js::PropertyDescriptor>> cross_origin_fallback(js::Interpreter&, js::PropertyKey const&);

    js::RealmRecord* m_record;
    std::vector<CrossOriginEntry> m_cross_origin;
};

// The WindowProxy exotic object (HTML §7.2.3.3): what every script holds for
// a window — `window`, `globalThis`, an iframe's contentWindow, a message's
// source. It stands for the window of its frame's current document, the same
// proxy before and after the frame goes on to another, and shows a script of
// another origin that window's frames, by index and by name, and its
// CrossOriginProperties, nothing more.
class WindowProxyObject final : public CrossOriginObject {
public:
    explicit WindowProxyObject(js::RealmRecord& window_realm);
    js::Object& window() const { return *target(); }
    Realm::Internals& internals() const;
    // Its frame has gone on to another document: that document's window.
    void stand_for(js::RealmRecord& window_realm);

    using CrossOriginObject::get_own_property;
    using CrossOriginObject::define_own_property;
    using CrossOriginObject::has_property;
    using CrossOriginObject::delete_property;
    using CrossOriginObject::own_keys;
    std::optional<js::Object*> get_prototype_of(js::Interpreter&) override;
    std::optional<bool> set_prototype_of(js::Interpreter&, js::Object* prototype) override;
    std::optional<bool> is_extensible(js::Interpreter&) override;
    std::optional<bool> prevent_extensions(js::Interpreter&) override;
    std::optional<std::optional<js::PropertyDescriptor>> get_own_property(js::Interpreter&, js::PropertyKey const&) override;
    std::optional<bool> define_own_property(js::Interpreter&, js::PropertyKey const&, js::PropertyDescriptor const&) override;
    std::optional<bool> delete_property(js::Interpreter&, js::PropertyKey const&) override;
    std::optional<std::vector<js::PropertyKey>> own_keys(js::Interpreter&) override;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;

protected:
    std::span<CrossOriginProperty const> cross_origin_properties() const override;
    std::optional<js::PropertyDescriptor> original_member(js::Interpreter&, std::string_view name) const override;

private:
    // A frame of the window's document by its index: its WindowProxy.
    std::optional<js::PropertyDescriptor> child_window(js::PropertyKey const&) const;
};

// A Location object (HTML §7.10): its members are its own and unforgeable,
// kept on the object it forwards to, and to a script of another origin it is
// exotic the way a WindowProxy is, showing its href setter and replace.
class LocationObject final : public CrossOriginObject {
public:
    LocationObject(js::Object& members, js::RealmRecord& window_realm);

    using CrossOriginObject::get_own_property;
    using CrossOriginObject::define_own_property;
    using CrossOriginObject::has_property;
    using CrossOriginObject::delete_property;
    using CrossOriginObject::own_keys;
    std::optional<js::Object*> get_prototype_of(js::Interpreter&) override;
    std::optional<bool> set_prototype_of(js::Interpreter&, js::Object* prototype) override;
    std::optional<bool> is_extensible(js::Interpreter&) override;
    std::optional<bool> prevent_extensions(js::Interpreter&) override;
    std::optional<std::optional<js::PropertyDescriptor>> get_own_property(js::Interpreter&, js::PropertyKey const&) override;
    std::optional<bool> define_own_property(js::Interpreter&, js::PropertyKey const&, js::PropertyDescriptor const&) override;
    std::optional<bool> delete_property(js::Interpreter&, js::PropertyKey const&) override;
    std::optional<std::vector<js::PropertyKey>> own_keys(js::Interpreter&) override;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;
    void trace(js::Tracer&) override;

protected:
    std::span<CrossOriginProperty const> cross_origin_properties() const override;
    std::optional<js::PropertyDescriptor> original_member(js::Interpreter&, std::string_view name) const override;

private:
    bool is_default_property(js::PropertyKey const&) const;
    std::vector<js::PropertyKey> m_default_properties; // [[DefaultProperties]]
};

WindowProxyObject* as_window_proxy(js::Value const&);
LocationObject* as_location(js::Value const&);
// IsPlatformObjectSameOrigin (HTML §7.2.3.2): whether the current realm's
// origin is same origin-domain with the origin of a window's realm.
bool is_platform_object_same_origin(js::Interpreter&, js::RealmRecord const& window_realm);
// The SecurityError a script of another origin meets.
std::optional<js::Value> throw_security_error(js::Interpreter&);
// The frames of a window's document that have windows here, in tree order
// (the document-tree child navigables).
std::vector<ChildFrame const*> child_navigables(Realm::Internals const&);
// A Location member behind the object it is called on, as that object's
// window runs it, with the security check unless another origin may call it.
js::NativeFunction::Callback location_member(js::NativeFunction::Callback, bool shown_to_other_origins);
// Puts every member the window's interfaces gave its global object behind
// WebIDL's checks of `this` and HTML's security check, and gives the realm
// its WindowProxy; `language_globals` are the names that were there before.
void install_window_proxy(Realm::Internals&, std::vector<js::PropertyKey> const& language_globals);

// An iframe's document with a realm of its own in its page's agent: the
// policy, the document, and the Realm last, so that the Realm ends first;
// and what the frame was opened from, as the painter keys it.
struct ChildFrame {
    dom::Element* container = nullptr;
    std::string source;
    std::unique_ptr<net::ContentSecurityPolicy> policy;
    std::unique_ptr<dom::Document> document;
    std::unique_ptr<Realm> realm;
};

// The agent a page's documents run in (HTML §8.1.2, the similar-origin
// window agent): the interpreter, whose heap and job queue — the microtask
// queue — its realms share, the event loop's tasks and timers, and how deep
// the host's entries into script go. A page's realm makes one of its own.
struct Agent {
    js::Interpreter interpreter;
    std::vector<Timer> timers;
    // Run before the timers at the next pump, oldest first, each holding what
    // it needs through Persistents.
    std::deque<Task> tasks;
    int next_timer_id = 1;
    std::uint64_t next_sequence = 1;
    bool in_checkpoint = false;
    int script_depth = 0; // entries from the host in progress
    // Every entry of the host into any realm here, nested: a frame's realm
    // ends only when this is back at zero.
    int host_depth = 0;
    bool ending = false; // the page's realm is being destroyed
    // The realm that stands in for the realms that have ended: an empty
    // document, no hooks, and tasks and timers that never run. A native of an
    // ended realm that a script still holds answers from it. After the
    // interpreter, so that it ends before it; the frames being closed after
    // it, since their ending hands it their records.
    std::unique_ptr<dom::Document> stand_in_document;
    std::unique_ptr<Realm> stand_in;
    std::vector<ChildFrame> closing;
    // The origin each ended realm had, by its record: a script may still hold
    // that window's WindowProxy or Location, and the cross-origin rules go on
    // judging them by the origin they had rather than the stand-in's.
    std::unordered_map<js::RealmRecord const*, OriginSnapshot> ended_origins;
};

struct Realm::Internals {
    Realm& realm;
    dom::Document& document;
    net::Url url;
    HostHooks hooks;
    // Documents scripts made (DOMParser, createHTMLDocument): owned for the
    // realm's life, so no wrapper into them can dangle.
    std::vector<std::unique_ptr<dom::Document>> extra_documents;
    // The agent this realm runs in, its own: declared after the documents its
    // wrappers point into, so that it ends first.
    std::unique_ptr<Agent> own_agent;
    Agent& agent;
    js::Interpreter& interpreter; // the agent's
    js::RealmRecord* realm_record; // this document's realm in it
    // The URL of this document's origin: its own, or an srcdoc document's
    // parent's.
    net::Url origin_url;
    // For a frame's realm, the realm of the document its iframe is in and
    // that iframe; null for a page's.
    Internals* parent_realm = nullptr;
    dom::Element* frame_element = nullptr;
    bool ended = false; // the agent's stand-in: no task or timer of its runs
    // A frame's window whose frame has closed, or has gone on to another
    // document: closed, and with no parent or top (HTML §7.2.2).
    bool discarded = false;
    // The domain document.domain gave this document's origin, which the
    // cross-origin rules compare (HTML §7.1.3); none until a script sets it.
    // An about:blank or srcdoc document has its parent's origin itself, not a
    // copy ("determining the origin", HTML §7.4.1), so the two share this: a
    // domain either of them sets is the other's too.
    class OriginDomain {
    public:
        std::string value_or(std::string fallback) const { return m_value->value_or(std::move(fallback)); }
        explicit operator bool() const { return m_value->has_value(); }
        OriginDomain& operator=(std::string value)
        {
            *m_value = std::move(value);
            return *this;
        }
        std::optional<std::string> const& get() const { return *m_value; }
        void share(OriginDomain const& parent) { m_value = parent.m_value; }

    private:
        std::shared_ptr<std::optional<std::string>> m_value = std::make_shared<std::optional<std::string>>();
    };
    OriginDomain domain;
    std::string window_name; // window.name, the frame's name to its parent
    // The frames of this document that have realms, in the order they were
    // opened; after the agent, so that they end before it.
    std::vector<ChildFrame> child_frames;
    // The WindowProxy of each iframe's frame here: one for as long as the
    // iframe stays in the tree, following it from document to document.
    std::unordered_map<dom::Element const*, WindowProxyObject*> navigables;
    // The window's members another origin may reach, as they were installed
    // (HTML §7.2.3.4): a script replacing one of its own (window.frames = …)
    // does not change what another origin is shown.
    std::unordered_map<std::string_view, js::PropertyDescriptor> cross_origin_members;
    // The window's attributes that hold one object or value, behind their
    // getters (history, navigator, the storages, a script's status).
    std::unordered_map<std::string, js::Value> window_values;
    // What scripts hold for this window, its WindowProxy; and whether an
    // object is this window, the proxy or the global object behind it.
    js::Object* window_proxy() const;
    bool is_window(js::Object const*) const;
    // Opens an iframe's document in a realm of its own here, when the host
    // answers for it, its mutation count starting past `mutations_from`; and
    // the frame of an iframe, when it has this origin.
    void open_frame(dom::Element& iframe, std::uint64_t mutations_from = 0);
    ChildFrame const* frame_of(dom::Element const& iframe) const;
    // Closes an iframe's frame here: its loop work erased, its window gone
    // at once, its realm ended at the agent's next safe point. A frame that
    // navigates keeps its WindowProxy for the document it opens next.
    void close_frame(dom::Element const& iframe, bool keep_window_proxy = false);
    // The realm of a document in this agent, found from the page down; null
    // for a document none of them owns.
    Internals* realm_of(dom::Document const& document);
    // Adopts a node into a document (DOM §4.2.4), re-homing every wrapper in
    // the subtree to that document's realm; a node leaving a tree closes the
    // frames of the iframes in it.
    void adopt_into(dom::Document& target, dom::Node& node);
    void frames_removed(dom::Node& subtree);
    // A subtree inserted into a connected tree: each iframe in it navigates,
    // in a task after the script that inserted it, as does an iframe whose
    // src or srcdoc a script changed.
    void frames_inserted(dom::Node& subtree);
    // An iframe of this document with nothing to show, as it is inserted by
    // the parser or a script: its initial about:blank document and that
    // document's load, both before the next line (HTML §4.8.5); and the
    // iframe's load event.
    void open_blank_frame(dom::Element& iframe);
    void fire_frame_load(dom::Element& iframe);
    void schedule_frame_navigation(dom::Element& iframe);
    void navigate_frame(dom::Element& iframe);
    // Holds the agent's host_depth for a public entry of the realm; the last
    // one out ends the frames closed meanwhile.
    struct HostEntry {
        explicit HostEntry(Agent&);
        ~HostEntry();
        HostEntry(HostEntry const&) = delete;
        HostEntry& operator=(HostEntry const&) = delete;
        Agent& agent;
    };

    // The interfaces, by name: each constructor's prototype object.
    std::unordered_map<std::string, js::Object*> prototypes;
    js::Object* prototype(std::string_view name) const;
    // Which HTML element interface a tag gets.
    std::unordered_map<std::string, std::string> tag_interfaces;

    // The window's own listeners and handlers: the global object is the
    // window, and the interpreter made it, so they live beside it.
    std::vector<ListenerEntry> window_listeners;
    HandlerMap window_handlers;

    // Queues a task on the agent's event loop, as this realm's.
    void post_task(std::function<void()> task);
    std::uint64_t next_listener_id = 1;
    // A deferred script, fetched when prepared and run when the parser is
    // done: a classic one by its source, a module by its record.
    struct PendingScript {
        dom::Element* element = nullptr;
        std::string source;
        std::string name;
        js::ModuleRecord* module = nullptr;
    };
    std::vector<PendingScript> deferred_scripts;
    // Inline modules keyed uniquely in the module map, each with the
    // document's URL as its base; and the credentials mode of the module
    // graph being loaded, which its dependencies inherit.
    std::unordered_map<std::string, net::Url> inline_module_bases;
    int inline_modules = 0;
    bool module_credentials_include = false;
    // The root <script>'s nonce and whether the parser inserted it: the
    // policy judges every fetch of the graph by them.
    std::string module_nonce;
    bool module_parser_inserted = true;
    std::unordered_set<dom::Element const*> started_scripts; // "already started" (§4.12.1)
    html::TreeBuilder* active_parser = nullptr; // set while the parser runs a script
    std::string ready_state = "loading";
    dom::Element* current_script = nullptr;
    js::Value current_event; // window.event
    std::uint64_t mutations = 0;
    ScriptStats stats;
    std::vector<std::pair<std::string, std::string>> cookies; // the fallback jar
    // A control's value and checkedness set by script when the host gave no
    // hooks for them (a test, --render): the dirty value, kept apart from
    // the attribute as the specification keeps it.
    std::unordered_map<dom::Element const*, std::string> fallback_values;
    std::unordered_map<dom::Element const*, bool> fallback_checked;
    dom::Element const* fallback_focus = nullptr;
    js::Object* location = nullptr;
    js::Object* history_state_holder = nullptr;
    js::Value history_state;
    int history_length = 1;
    double time_origin = 0;

    Internals(Realm& realm, dom::Document& document, net::Url url, HostHooks hooks);
    // A frame's: a realm of its own in its page's agent.
    Internals(Realm& realm, Agent& agent, dom::Document& document, net::Url url, HostHooks hooks);

    double now() const;
    void console(std::string_view level, std::string_view message) const;
    void trace(std::string_view message) const; // the hooks' trace, when given
    // Reports an uncaught exception: the console, the count, window.onerror.
    void report_uncaught(js::Value const& thrown, std::string_view where);

    // The document's Content Security Policy, asked through these: the
    // head's <meta> policies are adopted first each time.
    void adopt_meta_policies();
    net::RequestGuard request_guard(net::ResourceKind kind, std::string nonce = {}, bool parser_inserted = true);
    bool inline_refused(net::InlineKind kind, std::string_view nonce, std::string_view source);
    // eval, Function, a timer's string: refused unless the policy allows
    // 'unsafe-eval'; the message is the EvalError's.
    std::optional<std::string> compile_strings_refusal();
    // A sandbox directive without allow-scripts: no script runs at all.
    bool scripts_sandboxed() const;
    // Every entry from the host into script goes through these: the
    // microtask checkpoint on the way out, the time accounted.
    struct Entry {
        explicit Entry(Internals&);
        ~Entry();
        HostEntry host_entry; // first, so that it is the last out
        Internals& internals;
        double started;
        // The host enters this document's realm for the length of the entry.
        js::Interpreter::RealmScope realm_scope;
    };
    // Calls a script function from the host, reporting a throw.
    void call_reporting(js::Value const& callee, js::Value const& this_value, Args arguments, std::string_view where);

    // Wrappers.
    js::Object* wrap(dom::Node&);
    NodeWrapper* wrapper_of(js::Value const&) const; // null unless a node wrapper of this agent, not detached
    js::Object* prototype_for(dom::Node const&) const;

    // Events.
    EventObject* new_event(std::string_view interface, std::string_view type, bool bubbles, bool cancelable);
    // Dispatches `event` at `target` (a node wrapper, the window, or another
    // EventTargetObject); returns whether the default is still allowed.
    bool dispatch(EventObject& event, js::Object* target);
    // The listeners and handlers of a target, for install and dispatch.
    std::vector<ListenerEntry>* listeners_of(js::Object* target);
    HandlerMap* handlers_of(js::Object* target);

    // The scripts.
    void prepare_script(dom::Element& script, bool from_parser);
    void execute_script(dom::Element& script, std::string const& source, std::string const& name);
    // Module scripts: the map's hooks, the fetch of one module, an
    // element's graph prepared, run, and its evaluation watched.
    void install_module_hooks();
    std::string inline_module_key();
    net::Url module_base_of(std::string_view referrer_key) const;
    std::optional<std::string> fetch_module_source(std::string const& key, bool include_credentials, std::string& error);
    void prepare_module_script(dom::Element& script, bool from_parser);
    void execute_module(dom::Element& script, js::ModuleRecord& record, std::string const& name);
    void watch_module_evaluation(js::Value const& promise, std::string const& name);

    // Strings across the boundary.
    js::Value string(std::string_view utf8) { return js::Value::string(interpreter.string(utf8)); }
    std::optional<std::string> to_utf8(js::Value const&);
    Native throw_dom_exception(std::string_view name, std::string_view message);
};

// The realm a native was installed by.
inline Realm::Internals& internals_of(js::Interpreter& interpreter)
{
    return static_cast<Realm*>(interpreter.current_realm()->host_defined)->internals();
}

// Binary data across the interfaces (Binary.cpp).
// The bytes an ArrayBuffer, a typed array or a DataView holds right now —
// empty once detached or out of bounds — or nothing for any other value.
std::optional<std::span<std::uint8_t const>> buffer_source_bytes(js::Value const&);
// The Encoding Standard's UTF-8 encoder: a lone surrogate becomes U+FFFD.
std::string encode_utf8(std::u16string_view);
// A fresh Uint8Array over a copy of the bytes.
Native uint8_array_of(js::Interpreter&, std::span<std::uint8_t const>);
// A promise already settled with the value or the reason.
Native resolved_promise(js::Interpreter&, js::Value const&);
Native rejected_promise(js::Interpreter&, js::Value const& reason);
// A fresh Blob of these bytes and type.
BlobObject* new_blob(Realm::Internals&, std::vector<std::uint8_t> bytes, std::string type);
// AbortSignal (Tasks.cpp): a fresh signal, and aborting one — the flag,
// the reason (an AbortError DOMException when undefined), the event.
AbortSignalObject* new_abort_signal(Realm::Internals&);
void signal_abort(Realm::Internals&, AbortSignalObject&, js::Value const& reason);
// An AbortError DOMException as a value, for a rejection.
js::Value abort_error(Realm::Internals&, std::string_view message);

// The node behind `this`, or a TypeError "Illegal invocation".
std::optional<dom::Node*> this_node(js::Interpreter&, js::Value const& this_value);
std::optional<dom::Element*> this_element(js::Interpreter&, js::Value const& this_value);
std::optional<dom::Document*> this_document(js::Interpreter&, js::Value const& this_value);

// A NodeList (an Array with NodeList.prototype) of these nodes' wrappers.
js::Value node_list(Realm::Internals&, std::vector<dom::Node*> const& nodes);

// Generated from idl/ by tools/gen-bindings.cpp (generated/*.gen.cpp): the
// interfaces with their tags, and their reflected attributes.
namespace generated {
void install_html_element_interfaces(Realm::Internals&);
void install_reflected_attributes(Realm::Internals&);
}

// Installers, one per file.
void install_events(Realm::Internals&); // Events.cpp
void install_nodes(Realm::Internals&); // Node.cpp
void install_style(Realm::Internals&); // Style.cpp
void install_window(Realm::Internals&); // Window.cpp
void install_binary(Realm::Internals&); // Binary.cpp: TextEncoder, TextDecoder, Blob, File
void install_fetch(Realm::Internals&); // Fetch.cpp: Headers, Request, Response, FormData, fetch
void install_xhr(Realm::Internals&); // Xhr.cpp: XMLHttpRequest
void install_tasks(Realm::Internals&); // Tasks.cpp: AbortController, AbortSignal, MessageChannel, MessagePort, postMessage

// Objects the style file makes for the node bindings.
js::Value make_token_list(Realm::Internals&, dom::Element&, std::string attribute); // classList, relList
js::Value make_style_declaration(Realm::Internals&, dom::Element*, bool computed); // element.style, getComputedStyle
js::Value make_dataset(Realm::Internals&, dom::Element&);
// The element's wrapper as the node wrapper it is.
NodeWrapper& wrapper_for(Realm::Internals&, dom::Node&);

// Control state through the hooks, else the realm's fallback.
std::string control_value_of(Realm::Internals&, dom::Element const&);
void set_control_value_of(Realm::Internals&, dom::Element const&, std::string value);
bool control_checked_of(Realm::Internals&, dom::Element const&);
void set_control_checked_of(Realm::Internals&, dom::Element const&, bool checked);
// Focus moves: the hook, the events, the fallback.
void move_focus(Realm::Internals&, dom::Element const* element);
dom::Element const* focused_element(Realm::Internals&);

// Helpers shared by the installers.

// Makes an interface: a constructor on the global (throwing "Illegal
// constructor" when called unless `construct` is given) whose prototype
// inherits `parent`'s; registered under `name`.
js::Object* define_interface(Realm::Internals&, std::string_view name, js::Object* parent_prototype,
    js::NativeFunction::ConstructCallback construct = {}, int length = 0);
// An accessor pair on a prototype.
void define_getter(Realm::Internals&, js::Object& prototype, std::string_view name, js::NativeFunction::Callback getter,
    js::NativeFunction::Callback setter = {});
// An attribute reflected as a string (getAttribute / setAttribute).
void reflect_string(Realm::Internals&, js::Object& prototype, std::string_view property, std::string_view attribute);
// A boolean attribute reflected (presence / toggle).
void reflect_boolean(Realm::Internals&, js::Object& prototype, std::string_view property, std::string_view attribute);
// A URL-valued attribute: read resolved against the document, written as given.
void reflect_url(Realm::Internals&, js::Object& prototype, std::string_view property, std::string_view attribute);
// An integer attribute with a default when absent or unparsable.
void reflect_long(Realm::Internals&, js::Object& prototype, std::string_view property, std::string_view attribute, int fallback);
// Defines the on<type> handler accessors for these event types on a
// prototype or the global.
void define_event_handlers(Realm::Internals&, js::Object& target, std::span<std::string_view const> types);

// Attribute helpers that count as mutations.
void set_attribute(Realm::Internals&, dom::Element&, std::string_view name, std::string value);
bool remove_attribute(Realm::Internals&, dom::Element&, std::string_view name);
std::string attribute_or_empty(dom::Element const&, std::string_view name);

// ASCII lowercase / uppercase copies.
std::string ascii_lower(std::string_view);
std::string ascii_upper(std::string_view);
// WebIDL's unsigned long of a number: NaN and the infinities are 0, the rest
// truncated and taken modulo 2^32.
std::uint32_t to_unsigned_long(double);
// The HTML "space characters" split of a token list attribute.
std::vector<std::string> split_tokens(std::string_view);
std::string join_tokens(std::vector<std::string> const&);

}
