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

// A node's one wrapper (ADR 0001 §1).
class NodeWrapper final : public EventTargetObject {
public:
    NodeWrapper(js::Object* prototype, Realm& realm, dom::Node& node);
    ~NodeWrapper() override;
    dom::Node& node() const { return *m_node; }
    Realm& realm() const { return *m_realm; }
    void trace(js::Tracer&) override;

private:
    Realm* m_realm;
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

    void trace(js::Tracer& tracer) override;
};

// A DOMTokenList over one attribute of an element (classList, relList).
class TokenListObject final : public js::Object {
public:
    TokenListObject(js::Object* prototype, Realm& the_realm, dom::Element& the_element, std::string the_attribute)
        : Object(prototype, Class::Host)
        , realm(&the_realm)
        , element(&the_element)
        , attribute(std::move(the_attribute))
    {
    }
    Realm* realm;
    dom::Element* element;
    std::string attribute;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const&) const override;
    void trace(js::Tracer&) override;
};

// A CSSStyleDeclaration: an element's style attribute read and written
// property by property, or — read-only — its computed style.
class StyleDeclarationObject final : public js::Object {
public:
    StyleDeclarationObject(js::Object* prototype, Realm& the_realm, dom::Element* the_element, bool is_computed)
        : Object(prototype, Class::Host)
        , realm(&the_realm)
        , element(the_element)
        , computed(is_computed)
    {
    }
    Realm* realm;
    dom::Element* element;
    bool computed;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;
    void trace(js::Tracer&) override;
};

// element.dataset: the data-* attributes as properties.
class DatasetObject final : public js::Object {
public:
    DatasetObject(js::Object* prototype, Realm& the_realm, dom::Element& the_element)
        : Object(prototype, Class::Host)
        , realm(&the_realm)
        , element(&the_element)
    {
    }
    Realm* realm;
    dom::Element* element;
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const&) const override;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;
    bool delete_property(js::PropertyKey const&) override;
    std::vector<js::PropertyKey> own_keys() const override;
    void trace(js::Tracer&) override;
};

// localStorage and sessionStorage: a map of strings, reachable as
// properties too (storage.key = "v").
class StorageObject final : public js::Object {
public:
    explicit StorageObject(js::Object* prototype)
        : Object(prototype, Class::Host)
    {
    }
    std::vector<std::pair<std::string, std::string>> items; // insertion order, for key(n)
    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const&) const override;
    std::optional<js::Value> get(js::Interpreter&, js::PropertyKey const&, js::Value const& receiver) override;
    std::optional<bool> set(js::Interpreter&, js::PropertyKey const&, js::Value const&, js::Value const& receiver) override;
    bool delete_property(js::PropertyKey const&) override;
    std::vector<js::PropertyKey> own_keys() const override;
    std::string const* find(std::string_view key) const;
    void put_item(std::string key, std::string value);
    bool remove_item(std::string_view key);
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

struct Timer {
    int id = 0;
    double due = 0; // on the hooks' clock
    std::uint64_t sequence = 0; // ties broken by creation order
    double interval = -1; // repeats every so many ms; below zero = once
    bool animation_frame = false; // requestAnimationFrame: the callback takes a timestamp
    std::unique_ptr<js::Persistent> callback; // a function, or a string of source
    std::vector<std::unique_ptr<js::Persistent>> arguments;
};

struct Realm::Internals {
    Realm& realm;
    dom::Document& document;
    net::Url url;
    HostHooks hooks;
    // Documents scripts made (DOMParser, createHTMLDocument): owned for the
    // realm's life, so no wrapper into them can dangle.
    std::vector<std::unique_ptr<dom::Document>> extra_documents;
    js::Interpreter interpreter;

    // The interfaces, by name: each constructor's prototype object.
    std::unordered_map<std::string, js::Object*> prototypes;
    js::Object* prototype(std::string_view name) const;
    // Which HTML element interface a tag gets.
    std::unordered_map<std::string, std::string> tag_interfaces;

    // The window's own listeners and handlers: the global object is the
    // window, and the interpreter made it, so they live beside it.
    std::vector<ListenerEntry> window_listeners;
    HandlerMap window_handlers;

    std::vector<Timer> timers;
    int next_timer_id = 1;
    std::uint64_t next_sequence = 1;
    std::uint64_t next_listener_id = 1;
    bool in_checkpoint = false;
    // A deferred script, fetched when prepared and run when the parser is done.
    struct PendingScript {
        dom::Element* element = nullptr;
        std::string source;
        std::string name;
    };
    std::vector<PendingScript> deferred_scripts;
    std::unordered_set<dom::Element const*> started_scripts; // "already started" (§4.12.1)
    html::TreeBuilder* active_parser = nullptr; // set while the parser runs a script
    std::string ready_state = "loading";
    dom::Element* current_script = nullptr;
    js::Value current_event; // window.event
    std::uint64_t mutations = 0;
    ScriptStats stats;
    int script_depth = 0; // entries from the host in progress
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

    double now() const;
    void console(std::string_view level, std::string_view message) const;
    // Reports an uncaught exception: the console, the count, window.onerror.
    void report_uncaught(js::Value const& thrown, std::string_view where);
    // Every entry from the host into script goes through these: the
    // microtask checkpoint on the way out, the time accounted.
    struct Entry {
        explicit Entry(Internals&);
        ~Entry();
        Internals& internals;
        double started;
    };
    // Calls a script function from the host, reporting a throw.
    void call_reporting(js::Value const& callee, js::Value const& this_value, Args arguments, std::string_view where);

    // Wrappers.
    js::Object* wrap(dom::Node&);
    NodeWrapper* wrapper_of(js::Value const&) const; // null unless a node wrapper of this realm
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

    // Strings across the boundary.
    js::Value string(std::string_view utf8) { return js::Value::string(interpreter.string(utf8)); }
    std::optional<std::string> to_utf8(js::Value const&);
    Native throw_dom_exception(std::string_view name, std::string_view message);
};

// The realm a native was installed by.
inline Realm::Internals& internals_of(js::Interpreter& interpreter)
{
    return static_cast<Realm*>(interpreter.host)->internals();
}

// The node behind `this`, or a TypeError "Illegal invocation".
std::optional<dom::Node*> this_node(js::Interpreter&, js::Value const& this_value);
std::optional<dom::Element*> this_element(js::Interpreter&, js::Value const& this_value);
std::optional<dom::Document*> this_document(js::Interpreter&, js::Value const& this_value);

// A NodeList (an Array with NodeList.prototype) of these nodes' wrappers.
js::Value node_list(Realm::Internals&, std::vector<dom::Node*> const& nodes);

// Installers, one per file.
void install_events(Realm::Internals&); // Events.cpp
void install_nodes(Realm::Internals&); // Node.cpp
void install_style(Realm::Internals&); // Style.cpp
void install_window(Realm::Internals&); // Window.cpp

// Objects the style file makes for the node bindings.
js::Value make_token_list(Realm::Internals&, dom::Element&, std::string attribute); // classList, relList
js::Value make_style_declaration(Realm::Internals&, dom::Element*, bool computed); // element.style, getComputedStyle
js::Value make_dataset(Realm::Internals&, dom::Element&);

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
// The HTML "space characters" split of a token list attribute.
std::vector<std::string> split_tokens(std::string_view);
std::string join_tokens(std::vector<std::string> const&);

}
