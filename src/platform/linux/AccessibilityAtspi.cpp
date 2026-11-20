#include "platform/Accessibility.h"
#include "platform/linux/AtspiMap.h"
#include "platform/linux/AtspiText.h"
#include "platform/linux/DBus.h"

#include "css/ComputedStyle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <set>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

// The application side of AT-SPI2 (at-spi2-core's xml/ interface
// descriptions are the specification): the bridge finds the accessibility
// bus (org.a11y.Bus.GetAddress on the session bus), connects to it, says it
// is there (org.a11y.atspi.Socket.Embed to the registry, the application's
// root at /org/a11y/atspi/accessible/root), and answers the interfaces of
// every node of the window's tree, each at /org/a11y/atspi/accessible/<id>.
// It emits the events the registry says a tool listens for, and none other.
//
// THE TREE IT SHOWS. application → frame (the window) → the chrome's nodes
// → under the chrome's content node, the document of the tab in front, then
// the documents of tabs behind it (neither showing nor visible). A page's
// text runs are not objects of their own, as every web engine has it on
// this platform: each node's text is the text of its runs with an embedded
// object character (U+FFFC) for each child object (atspi Hypertext), and its
// offsets count characters (code points).

namespace sashfold::platform {

namespace {

using a11y::NodeData;
using a11y::NodeId;
using a11y::Role;
using a11y::Snapshot;
using a11y::State;

constexpr char const* root_path = "/org/a11y/atspi/accessible/root";
constexpr std::string_view object_prefix = "/org/a11y/atspi/accessible/";
constexpr char const* null_path = "/org/a11y/atspi/null";
constexpr char const* cache_path = "/org/a11y/atspi/cache";
constexpr char const* registry_name = "org.a11y.atspi.Registry";
constexpr char const* registry_path = "/org/a11y/atspi/registry";
constexpr std::string_view interface_prefix = "org.a11y.atspi.";

// The application's own root, which is no node of a tree.
constexpr NodeId app_id = ~NodeId { 0 };

// How long the bridge's thread waits for a page's thread to answer what
// only it can (text geometry and style): never longer, so that a page busy
// with its scripts delays a tool's question and stalls nothing else.
constexpr auto page_answer_timeout = std::chrono::milliseconds(400);

std::string path_of(NodeId id)
{
    return id == app_id ? std::string(root_path) : std::string(object_prefix) + std::to_string(id);
}

std::optional<NodeId> id_of_path(std::string_view path)
{
    if (path == root_path)
        return app_id;
    if (!path.starts_with(object_prefix))
        return std::nullopt;
    std::string_view const digits = path.substr(object_prefix.size());
    if (digits.empty() || digits.size() > 20)
        return std::nullopt;
    NodeId id = 0;
    for (char const c : digits) {
        if (c < '0' || c > '9')
            return std::nullopt;
        id = id * 10 + static_cast<NodeId>(c - '0');
    }
    return id;
}

bool has(NodeData const& node, State s) { return (node.states & a11y::state_bit(s)) != 0; }

// A run of text in a page that is no object of its own: its text joins its
// parent's.
bool text_like(NodeData const& node)
{
    return node.role == Role::StaticText || node.role == Role::LineBreak || node.role == Role::ListMarker
        || node.kind == NodeData::Kind::Text || node.kind == NodeData::Kind::LineBreak || node.kind == NodeData::Kind::Marker;
}

// An event a tool may listen for, as the registry names them
// ("object:state-changed:focused", "Object:StateChanged:Focused", "focus:"):
// compared lower-cased with '-' and '_' left out.
struct EventKey {
    std::string category;
    std::string kind;
    std::string detail;
};

std::string folded(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        if (c == '-' || c == '_')
            continue;
        out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return out;
}

EventKey key_of(std::string_view name)
{
    EventKey key;
    std::size_t const first = name.find(':');
    key.category = folded(name.substr(0, first));
    if (first != std::string_view::npos) {
        std::string_view const rest = name.substr(first + 1);
        std::size_t const second = rest.find(':');
        key.kind = folded(rest.substr(0, second));
        if (second != std::string_view::npos)
            key.detail = folded(rest.substr(second + 1));
    }
    return key;
}

bool listens(EventKey const& registered, EventKey const& event)
{
    if (registered.category.empty())
        return true;
    if (registered.category != event.category)
        return false;
    if (!registered.kind.empty() && registered.kind != event.kind)
        return false;
    return registered.detail.empty() || event.detail.starts_with(registered.detail);
}

// An interface's introspection from its members: "Name(in)out" for a
// method, ":Name sig" a signal's arguments, "=Name sig r|rw" a property.
std::string introspection_of(std::string_view name, std::initializer_list<char const*> members)
{
    std::string xml = "  <interface name=\"" + std::string(name) + "\">\n";
    for (std::string_view member : members) {
        if (member.starts_with('=')) {
            std::size_t const space = member.find(' ');
            std::size_t const access = member.rfind(' ');
            xml += "    <property name=\"" + std::string(member.substr(1, space - 1)) + "\" type=\"" + std::string(member.substr(space + 1, access - space - 1))
                + "\" access=\"" + (member.substr(access + 1) == "rw" ? "readwrite" : "read") + "\"/>\n";
            continue;
        }
        bool const signal = member.starts_with(':');
        if (signal)
            member.remove_prefix(1);
        std::size_t const open = member.find('(');
        std::size_t const close = member.find(')');
        std::string_view const in = member.substr(open + 1, close - open - 1);
        std::string_view const out = member.substr(close + 1);
        xml += std::string("    <") + (signal ? "signal" : "method") + " name=\"" + std::string(member.substr(0, open)) + "\">";
        auto const args = [&xml, signal](std::string_view types, char const* direction) {
            std::size_t at = 0;
            while (at < types.size()) {
                std::size_t const length = dbus::complete_type_length(types, at);
                if (length == 0)
                    break;
                xml += "<arg type=\"" + std::string(types.substr(at, length)) + "\"" + (signal ? std::string() : std::string(" direction=\"") + direction + "\"") + "/>";
                at += length;
            }
        };
        args(in, "in");
        args(out, "out");
        xml += std::string("</") + (signal ? "signal" : "method") + ">\n";
    }
    return xml + "  </interface>\n";
}

class AtspiBridge final : public Accessibility {
public:
    AtspiBridge(std::string application_name, std::function<void()> wake_window);
    ~AtspiBridge() override;

    bool wanted() const override { return m_wanted.load(std::memory_order_relaxed); }
    void add_document(std::shared_ptr<AccessibleDocument> document) override;
    void remove_document(std::shared_ptr<AccessibleDocument> const& document) override;
    void set_window(WindowState state) override;
    std::vector<WindowAction> take_window_actions() override;
    void wake() override;

private:
    // --- What the bridge's thread reads the tree through ------------------
    struct Doc {
        std::shared_ptr<AccessibleDocument> link;
        std::shared_ptr<Snapshot const> snapshot; // read by the tools' questions
        std::shared_ptr<Snapshot const> told; // what the last events were computed against
        AccessibleDocument::View view;
        bool announced = false;
    };
    struct Ref {
        NodeData const* node = nullptr;
        Doc const* doc = nullptr; // null: the chrome's node, or the application
        bool app = false;
        NodeId id = 0;
        explicit operator bool() const { return node != nullptr || app; }
        bool chrome() const { return node != nullptr && doc == nullptr; }
        bool frame(WindowState const& window) const { return chrome() && window.chrome && window.chrome->root == id; }
    };

    void run();
    bool connect();
    void serve();
    void take_changes();
    void emit_document_events(Doc& doc, std::vector<a11y::Event> const& events);
    void emit_chrome_events(std::shared_ptr<Snapshot const> const& before, std::vector<a11y::Event> const& events);
    void diff_children(Ref const& before_parent, Ref const& after_parent, NodeId parent);

    Ref find(NodeId id) const;
    Ref find_in(NodeId id, Snapshot const* chrome, std::vector<Doc> const& docs) const;
    Ref parent_of(Ref const& ref) const;
    std::vector<NodeId> children_of(Ref const& ref) const;
    std::vector<NodeId> children_in(Ref const& ref, Snapshot const* chrome, std::vector<Doc> const& docs) const;
    int index_in_parent(Ref const& ref) const;
    atspi::Context context_of(Ref const& ref) const;
    std::uint32_t role_of(Ref const& ref) const;
    atspi::StateSet states_of(Ref const& ref) const;
    std::vector<std::string> interfaces_of(Ref const& ref) const;
    std::optional<a11y::Rect> window_bounds(Ref const& ref) const;
    std::string name_of(Ref const& ref) const;
    atspi::FlatText flat_text(Ref const& ref) const;
    atspi::FlatText flat_text_in(Ref const& ref, Snapshot const* chrome, std::vector<Doc> const& docs) const;
    bool offers_text(Ref const& ref) const;
    void write_reference(dbus::Writer& out, NodeId id) const;
    void write_null(dbus::Writer& out) const;
    dbus::Value reference_value(NodeId id) const;

    // Runs on the page's thread what only it can answer, and waits for it
    // (at most page_answer_timeout): nothing when it did not answer in time.
    template <typename T>
    std::optional<T> ask_page(Doc const& doc, std::function<T(a11y::Tree&)> question) const;
    // Posts an action to the page's thread (not waited for).
    bool tell_page(Doc const& doc, a11y::Tree::Query query) const;
    void tell_window(WindowAction action);

    // The interfaces' answers.
    std::optional<dbus::Reply> accessible(dbus::Message const& call);
    std::optional<dbus::Value> accessible_property(std::string const& path, std::string_view property);
    std::optional<dbus::Reply> application(dbus::Message const& call);
    std::optional<dbus::Reply> component(dbus::Message const& call);
    std::optional<dbus::Reply> action(dbus::Message const& call);
    std::optional<dbus::Reply> text(dbus::Message const& call);
    std::optional<dbus::Reply> editable_text(dbus::Message const& call);
    std::optional<dbus::Value> value_property(std::string const& path, std::string_view property);
    std::optional<dbus::Reply> set_value(std::string const& path, dbus::Value const& value);
    std::optional<dbus::Reply> selection(dbus::Message const& call);
    std::optional<dbus::Reply> table(dbus::Message const& call);
    std::optional<dbus::Value> table_property(std::string const& path, std::string_view property);
    std::optional<dbus::Reply> table_cell(dbus::Message const& call);
    std::optional<dbus::Value> table_cell_property(std::string const& path, std::string_view property);
    std::optional<dbus::Reply> hyperlink(dbus::Message const& call);
    std::optional<dbus::Value> hyperlink_property(std::string const& path, std::string_view property);
    std::optional<dbus::Reply> hypertext(dbus::Message const& call);
    std::optional<dbus::Reply> image(dbus::Message const& call);
    std::optional<dbus::Reply> document(dbus::Message const& call);
    std::optional<dbus::Reply> collection(dbus::Message const& call);
    std::optional<dbus::Reply> cache(dbus::Message const& call);
    void write_cache_item(dbus::Writer& out, Ref const& ref) const;
    void export_interfaces();

    // Text helpers, on a node offering Text.
    std::vector<atspi::Range> layout_lines(Ref const& ref, atspi::FlatText const& flat) const;
    std::optional<std::size_t> offset_at_point(Ref const& ref, atspi::FlatText const& flat, float x, float y) const;
    a11y::Rect in_coordinates(a11y::Rect rect, Ref const& ref, std::uint32_t type) const;
    std::vector<NodeId> descendants(Ref const& ref, bool deep) const;
    std::optional<a11y::Rect> character_extents(Ref const& ref, atspi::FlatText const& flat, std::size_t from, std::size_t to) const;
    std::vector<std::pair<std::string, std::string>> text_attributes(Ref const& ref, atspi::FlatText const& flat, std::size_t offset,
        std::size_t& start, std::size_t& end) const;

    // Events.
    bool listened(std::string_view interface, std::string_view member, std::string_view detail) const;
    void emit(NodeId source, std::string_view interface, std::string_view member, std::string_view detail, std::int32_t detail1,
        std::int32_t detail2, dbus::Value any);
    void emit_text_diff(NodeId id, atspi::FlatText const& before, atspi::FlatText const& after);

    std::string m_application_name;
    std::function<void()> m_wake_window;
    std::atomic<bool> m_wanted { false };
    std::atomic<bool> m_stop { false };
    int m_wake_fd = -1;
    std::thread m_thread;

    // Shared with the window's and the pages' threads.
    std::mutex m_lock;
    std::vector<std::shared_ptr<AccessibleDocument>> m_adding;
    std::vector<std::shared_ptr<AccessibleDocument>> m_removing;
    std::optional<WindowState> m_window_new;
    std::vector<a11y::Event> m_window_events;
    std::vector<WindowAction> m_actions;

    // The bridge's thread's own.
    std::unique_ptr<dbus::Connection> m_bus;
    std::unique_ptr<dbus::Service> m_service;
    std::string m_desktop_name;
    std::string m_desktop_path;
    std::int32_t m_application_id = 0;
    std::vector<std::pair<std::string, EventKey>> m_listeners; // by the listening tool's bus name
    std::vector<Doc> m_docs;
    WindowState m_window;
    bool m_window_announced = false;
    bool m_cache_used = false; // a tool read GetItems: it hears AddAccessible and RemoveAccessible
    std::shared_ptr<dbus::Interface const> m_accessible, m_application, m_component, m_action, m_text, m_editable_text, m_value,
        m_selection, m_table, m_table_cell, m_hyperlink, m_hypertext, m_image, m_document, m_collection, m_cache;
};

// --- The other threads' side ----------------------------------------------------------

AtspiBridge::AtspiBridge(std::string application_name, std::function<void()> wake_window)
    : m_application_name(std::move(application_name))
    , m_wake_window(std::move(wake_window))
{
    m_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    m_thread = std::thread([this] { run(); });
}

AtspiBridge::~AtspiBridge()
{
    m_stop = true;
    wake();
    if (m_thread.joinable())
        m_thread.join();
    if (m_wake_fd >= 0)
        close(m_wake_fd);
}

void AtspiBridge::wake()
{
    if (m_wake_fd < 0)
        return;
    std::uint64_t const one = 1;
    // Non-blocking: a counter that is full already wakes the thread.
    [[maybe_unused]] ssize_t const wrote = write(m_wake_fd, &one, sizeof one);
}

void AtspiBridge::add_document(std::shared_ptr<AccessibleDocument> document)
{
    {
        std::lock_guard<std::mutex> const hold(m_lock);
        m_adding.push_back(std::move(document));
    }
    wake();
}

void AtspiBridge::remove_document(std::shared_ptr<AccessibleDocument> const& document)
{
    {
        std::lock_guard<std::mutex> const hold(m_lock);
        m_removing.push_back(document);
    }
    wake();
}

void AtspiBridge::set_window(WindowState state)
{
    {
        std::lock_guard<std::mutex> const hold(m_lock);
        // The events of every state handed over and not yet taken, in order.
        for (a11y::Event& event : state.events)
            m_window_events.push_back(std::move(event));
        state.events.clear();
        // A tool that stopped reading cannot make this grow without end.
        if (m_window_events.size() > 4096) {
            m_window_events.clear();
            m_window_events.push_back({ a11y::Event::Type::TreeChanged, 0 });
        }
        m_window_new = std::move(state);
    }
    wake();
}

std::vector<WindowAction> AtspiBridge::take_window_actions()
{
    std::lock_guard<std::mutex> const hold(m_lock);
    std::vector<WindowAction> actions;
    actions.swap(m_actions);
    return actions;
}

void AtspiBridge::tell_window(WindowAction action)
{
    {
        std::lock_guard<std::mutex> const hold(m_lock);
        if (m_actions.size() < 256)
            m_actions.push_back(std::move(action));
    }
    if (m_wake_window)
        m_wake_window();
}

// --- The bridge's thread ------------------------------------------------------------

void AtspiBridge::run()
{
    if (!connect())
        return; // no accessibility bus: quietly nothing
    serve();
}

bool AtspiBridge::connect()
{
    // The accessibility bus: where AT_SPI_BUS_ADDRESS says, else where the
    // session bus's org.a11y.Bus says (which starts it when it is not up).
    std::string address;
    if (char const* const given = std::getenv("AT_SPI_BUS_ADDRESS"); given && *given)
        address = given;
    if (address.empty()) {
        std::optional<dbus::SocketAddress> const session_address = dbus::session_bus_address();
        if (!session_address)
            return false;
        std::unique_ptr<dbus::Connection> session = dbus::Connection::open(*session_address);
        if (!session)
            return false;
        dbus::Service asker(*session);
        std::optional<dbus::Message> const reply = asker.call(dbus::Service::method_call("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress"), 3000);
        if (!reply || reply->type != dbus::MessageType::MethodReturn)
            return false;
        dbus::Reader body(reply->body);
        std::optional<std::string> given = body.string();
        if (!given || given->empty())
            return false;
        address = std::move(*given);
    }
    std::optional<dbus::SocketAddress> const where = dbus::parse_address(address);
    if (!where || m_stop)
        return false;
    m_bus = dbus::Connection::open(*where);
    if (!m_bus)
        return false;
    m_service = std::make_unique<dbus::Service>(*m_bus);
    export_interfaces();
    // The registry's word on which events are listened for, from now on.
    m_service->add_match(std::string("type='signal',interface='org.a11y.atspi.Registry',member='EventListenerRegistered'"));
    m_service->add_match(std::string("type='signal',interface='org.a11y.atspi.Registry',member='EventListenerDeregistered'"));
    // Embedded: the registry puts the application under the desktop.
    dbus::Writer embed;
    embed.reference(m_bus->unique_name(), root_path);
    std::optional<dbus::Message> const embedded = m_service->call(
        dbus::Service::method_call(registry_name, root_path, "org.a11y.atspi.Socket", "Embed", "(so)", std::move(embed)), 3000);
    if (embedded && embedded->type == dbus::MessageType::MethodReturn) {
        dbus::Reader body(embedded->body);
        if (std::optional<dbus::Value> const desktop = body.value("(so)")) {
            m_desktop_name = desktop->items[0].text;
            m_desktop_path = desktop->items[1].text;
        }
    }
    std::optional<dbus::Message> const registered = m_service->call(
        dbus::Service::method_call(registry_name, registry_path, "org.a11y.atspi.Registry", "GetRegisteredEvents"), 3000);
    if (registered && registered->type == dbus::MessageType::MethodReturn) {
        dbus::Reader body(registered->body);
        if (std::optional<dbus::Value> const events = body.value("a(ss)")) {
            for (dbus::Value const& event : events->items)
                m_listeners.emplace_back(event.items[0].text, key_of(event.items[1].text));
        }
    }
    if (!m_listeners.empty() && !m_wanted.exchange(true) && m_wake_window)
        m_wake_window();
    return true;
}

void AtspiBridge::serve()
{
    while (!m_stop) {
        // A tool's question, the registry's word, or a wake from the
        // window or a page: whichever comes first.
        std::optional<dbus::Message> const message = m_service->next(m_service->has_kept() ? 0 : 1000, m_wake_fd);
        if (m_bus->closed())
            return;
        std::uint64_t count = 0;
        [[maybe_unused]] ssize_t const got = read(m_wake_fd, &count, sizeof count);
        take_changes();
        if (!message)
            continue;
        if (message->type == dbus::MessageType::Signal && message->interface == "org.a11y.atspi.Registry") {
            dbus::Reader body(message->body);
            if (message->member == "EventListenerRegistered") {
                std::optional<std::string> const bus = body.string();
                std::optional<std::string> const event = body.string();
                if (bus && event)
                    m_listeners.emplace_back(*bus, key_of(*event));
            } else if (message->member == "EventListenerDeregistered") {
                std::optional<std::string> const bus = body.string();
                std::optional<std::string> const event = body.string();
                if (bus && event) {
                    EventKey const key = key_of(*event);
                    auto const found = std::ranges::find_if(m_listeners, [&](auto const& listener) {
                        return listener.first == *bus && listener.second.category == key.category && listener.second.kind == key.kind
                            && listener.second.detail == key.detail;
                    });
                    if (found != m_listeners.end())
                        m_listeners.erase(found);
                }
            }
            if (!m_listeners.empty() && !m_wanted.exchange(true) && m_wake_window)
                m_wake_window();
            continue;
        }
        if (message->type != dbus::MessageType::MethodCall)
            continue;
        // A question from a tool (not the registry's own bookkeeping): the
        // trees are wanted from now on, and the window and the pages hear so.
        if (message->sender != m_desktop_name && !m_wanted.exchange(true) && m_wake_window)
            m_wake_window();
        m_service->dispatch(*message);
    }
}

AtspiBridge::Ref AtspiBridge::find(NodeId id) const { return find_in(id, m_window.chrome.get(), m_docs); }

AtspiBridge::Ref AtspiBridge::find_in(NodeId id, Snapshot const* chrome, std::vector<Doc> const& docs) const
{
    Ref ref;
    ref.id = id;
    if (id == app_id) {
        ref.app = true;
        return ref;
    }
    if (chrome) {
        if (NodeData const* const node = chrome->find(id)) {
            ref.node = node;
            return ref;
        }
    }
    for (Doc const& doc : docs) {
        Snapshot const* const snapshot = doc.snapshot.get();
        if (!snapshot)
            continue;
        if (NodeData const* const node = snapshot->find(id)) {
            if (text_like(*node))
                return {}; // a run of text is no object
            ref.node = node;
            ref.doc = &doc;
            return ref;
        }
    }
    return {};
}

AtspiBridge::Ref AtspiBridge::parent_of(Ref const& ref) const
{
    if (ref.app || !ref.node)
        return {};
    if (ref.doc) {
        if (ref.node->parent != 0)
            return find(ref.node->parent);
        return m_window.content != 0 ? find(m_window.content) : Ref {};
    }
    if (ref.node->parent != 0)
        return find(ref.node->parent);
    Ref app;
    app.app = true;
    app.id = app_id;
    return app;
}

std::vector<NodeId> AtspiBridge::children_of(Ref const& ref) const { return children_in(ref, m_window.chrome.get(), m_docs); }

std::vector<NodeId> AtspiBridge::children_in(Ref const& ref, Snapshot const* chrome, std::vector<Doc> const& docs) const
{
    std::vector<NodeId> children;
    if (ref.app) {
        if (chrome && chrome->root != 0)
            children.push_back(chrome->root);
        return children;
    }
    if (!ref.node)
        return children;
    if (!ref.doc) {
        children = ref.node->children;
        if (ref.id == m_window.content) {
            for (Doc const& doc : docs) {
                if (doc.snapshot && doc.snapshot->root != 0 && doc.announced)
                    children.push_back(doc.snapshot->root);
            }
        }
        return children;
    }
    Snapshot const& snapshot = *ref.doc->snapshot;
    for (NodeId const child : ref.node->children) {
        NodeData const* const node = snapshot.find(child);
        if (node && !text_like(*node))
            children.push_back(child);
    }
    (void)docs;
    return children;
}

int AtspiBridge::index_in_parent(Ref const& ref) const
{
    if (ref.app)
        return -1;
    Ref const parent = parent_of(ref);
    if (!parent)
        return ref.frame(m_window) ? 0 : -1;
    std::vector<NodeId> const siblings = children_of(parent);
    auto const at = std::ranges::find(siblings, ref.id);
    return at == siblings.end() ? -1 : static_cast<int>(at - siblings.begin());
}

bool AtspiBridge::offers_text(Ref const& ref) const
{
    if (ref.app || !ref.node)
        return false;
    NodeData const& node = *ref.node;
    if (has(node, State::Editable) || node.role == Role::RootWebArea)
        return true;
    if (!ref.doc)
        return !node.text.empty();
    // A page node whose text is its runs' and its children's (embedded
    // objects): every node that holds text or has children; a leaf with
    // nothing to read (a picture) offers none.
    return !node.text.empty() || !node.children.empty();
}

atspi::FlatText AtspiBridge::flat_text(Ref const& ref) const { return flat_text_in(ref, m_window.chrome.get(), m_docs); }

atspi::FlatText AtspiBridge::flat_text_in(Ref const& ref, Snapshot const*, std::vector<Doc> const&) const
{
    atspi::FlatText flat;
    if (!ref.node)
        return flat;
    NodeData const& node = *ref.node;
    // The chrome's node, a field or a leaf: its own text.
    if (!ref.doc || node.children.empty()) {
        flat.append_text(node.text, ref.id, 0);
        return flat;
    }
    Snapshot const& snapshot = *ref.doc->snapshot;
    for (NodeId const child : node.children) {
        NodeData const* const found = snapshot.find(child);
        if (!found)
            continue;
        if (found->role == Role::LineBreak || found->kind == NodeData::Kind::LineBreak)
            flat.append_text("\n", child, 0);
        else if (text_like(*found))
            flat.append_text(found->text.empty() ? found->name : found->text, child, 0);
        else
            flat.append_object(child);
    }
    return flat;
}

atspi::Context AtspiBridge::context_of(Ref const& ref) const
{
    atspi::Context context;
    context.chrome = ref.chrome();
    context.frame = ref.frame(m_window);
    context.window_active = m_window.active;
    context.document_shown = !ref.doc || ref.doc->view.shown;
    context.has_text = offers_text(ref);
    context.editable_text = ref.node && has(*ref.node, State::Editable) && ref.node->role != Role::RootWebArea;
    return context;
}

std::uint32_t AtspiBridge::role_of(Ref const& ref) const
{
    if (ref.app)
        return atspi::role::application;
    return atspi::role_of(*ref.node, context_of(ref));
}

atspi::StateSet AtspiBridge::states_of(Ref const& ref) const
{
    if (ref.app || !ref.node)
        return 0;
    atspi::StateSet states = atspi::states_of(*ref.node, context_of(ref));
    // A document that is loading is busy.
    if (ref.doc && ref.node->role == Role::RootWebArea && ref.doc->view.loading)
        states |= atspi::bit(atspi::state::busy);
    // A page's node scrolled out of the content area is not showing (the
    // model does not know the viewport; the bridge does).
    if (ref.doc && ref.node->role != Role::RootWebArea && m_window.content_width > 0) {
        std::optional<a11y::Rect> const box = window_bounds(ref);
        if (box && (box->x + box->width <= m_window.content_x || box->y + box->height <= m_window.content_y
                || box->x >= m_window.content_x + m_window.content_width || box->y >= m_window.content_y + m_window.content_height))
            states &= ~atspi::bit(atspi::state::showing);
    }
    return states;
}

std::string AtspiBridge::name_of(Ref const& ref) const
{
    if (ref.app)
        return m_application_name;
    if (ref.frame(m_window))
        return m_window.title;
    return ref.node ? ref.node->name : std::string();
}

std::vector<std::string> AtspiBridge::interfaces_of(Ref const& ref) const
{
    std::vector<std::string> names;
    auto const add = [&names](char const* name) { names.push_back(std::string(interface_prefix) + name); };
    add("Accessible");
    if (ref.app) {
        add("Application");
        return names;
    }
    NodeData const& node = *ref.node;
    add("Component");
    if (!node.actions.empty())
        add("Action");
    if (offers_text(ref)) {
        add("Text");
        if (ref.doc)
            add("Hypertext");
    }
    if (has(node, State::Editable) && node.role != Role::RootWebArea)
        add("EditableText");
    if (node.value_now || node.value_min || node.value_max)
        add("Value");
    if (node.role == Role::Image || node.role == Role::DocCover || node.role == Role::GraphicsSymbol)
        add("Image");
    if (node.role == Role::Table || node.role == Role::Grid || node.role == Role::TreeGrid)
        add("Table");
    if (node.role == Role::Cell || node.role == Role::GridCell || node.role == Role::ColumnHeader || node.role == Role::RowHeader)
        add("TableCell");
    if (node.role == Role::ListBox || node.role == Role::Tree || node.role == Role::TreeGrid || node.role == Role::Grid
        || node.role == Role::TabList || node.role == Role::Menu || node.role == Role::MenuBar || node.role == Role::RadioGroup)
        add("Selection");
    if (ref.doc) {
        if (node.role == Role::RootWebArea && node.parent == 0)
            add("Document");
        else
            add("Hyperlink");
        add("Collection");
    }
    return names;
}

std::optional<a11y::Rect> AtspiBridge::window_bounds(Ref const& ref) const
{
    if (ref.app || !ref.node)
        return std::nullopt;
    if (ref.frame(m_window))
        return a11y::Rect { 0, 0, static_cast<float>(m_window.width), static_cast<float>(m_window.height) };
    if (!ref.node->bounds)
        return std::nullopt;
    a11y::Rect rect = *ref.node->bounds;
    if (ref.doc) {
        rect.x += m_window.content_x - ref.doc->view.scroll_x;
        rect.y += m_window.content_y - ref.doc->view.scroll_y;
    }
    return rect;
}

void AtspiBridge::write_reference(dbus::Writer& out, NodeId id) const { out.reference(m_bus->unique_name(), path_of(id)); }

void AtspiBridge::write_null(dbus::Writer& out) const { out.reference(m_bus->unique_name(), null_path); }

dbus::Value AtspiBridge::reference_value(NodeId id) const
{
    return dbus::Value::structure({ dbus::Value::string(m_bus->unique_name()), dbus::Value::object_path(path_of(id)) });
}

template <typename T>
std::optional<T> AtspiBridge::ask_page(Doc const& doc, std::function<T(a11y::Tree&)> question) const
{
    auto promise = std::make_shared<std::promise<T>>();
    std::future<T> answer = promise->get_future();
    if (!doc.link->bridge->post([promise, question = std::move(question)](a11y::Tree& tree) { promise->set_value(question(tree)); }))
        return std::nullopt;
    if (answer.wait_for(page_answer_timeout) != std::future_status::ready)
        return std::nullopt; // the page did not get to it: the query stays queued, its answer unread
    try {
        return answer.get();
    } catch (std::future_error const&) {
        return std::nullopt; // the tree went with the query unrun
    }
}

bool AtspiBridge::tell_page(Doc const& doc, a11y::Tree::Query query) const { return doc.link->bridge->post(std::move(query)); }

// --- Changes: documents, the window, and their events -----------------------------------------

bool AtspiBridge::listened(std::string_view interface, std::string_view member, std::string_view detail) const
{
    if (m_listeners.empty())
        return false;
    EventKey event;
    event.category = folded(interface.substr(interface.rfind('.') + 1));
    event.kind = folded(member);
    event.detail = folded(detail);
    for (auto const& [bus, key] : m_listeners) {
        if (listens(key, event))
            return true;
    }
    return false;
}

void AtspiBridge::emit(NodeId source, std::string_view interface, std::string_view member, std::string_view detail, std::int32_t detail1,
    std::int32_t detail2, dbus::Value any)
{
    if (!listened(interface, member, detail))
        return;
    dbus::Writer body;
    body.string(detail);
    body.int32(detail1);
    body.int32(detail2);
    body.value(any); // a variant already
    body.array(8, [] { }); // no properties
    m_service->emit(path_of(source), std::string(interface), std::string(member), "siiva{sv}", std::move(body));
}

void AtspiBridge::take_changes()
{
    std::vector<std::shared_ptr<AccessibleDocument>> adding;
    std::vector<std::shared_ptr<AccessibleDocument>> removing;
    std::optional<WindowState> window;
    std::vector<a11y::Event> window_events;
    {
        std::lock_guard<std::mutex> const hold(m_lock);
        adding.swap(m_adding);
        removing.swap(m_removing);
        window = std::move(m_window_new);
        m_window_new.reset();
        window_events.swap(m_window_events);
    }
    constexpr char const* object_events = "org.a11y.atspi.Event.Object";
    if (window) {
        std::shared_ptr<Snapshot const> const before = m_window.chrome;
        bool const was_active = m_window.active;
        std::string const old_title = m_window.title;
        m_window = std::move(*window);
        if (m_window.chrome && !m_window_announced) {
            m_window_announced = true;
            emit(app_id, object_events, "ChildrenChanged", "add", 0, 0, dbus::Value::variant(reference_value(m_window.chrome->root)));
            emit(m_window.chrome->root, "org.a11y.atspi.Event.Window", "Create", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
        }
        if (m_window.chrome) {
            if (was_active != m_window.active)
                emit(m_window.chrome->root, "org.a11y.atspi.Event.Window", m_window.active ? "Activate" : "Deactivate", "", 0, 0,
                    dbus::Value::variant(dbus::Value::int32(0)));
            if (old_title != m_window.title && before)
                emit(m_window.chrome->root, object_events, "PropertyChange", "accessible-name", 0, 0, dbus::Value::variant(dbus::Value::string(m_window.title)));
        }
        if (before && m_window.chrome)
            emit_chrome_events(before, window_events);
    }
    // Documents that came: under the content node, said so.
    for (std::shared_ptr<AccessibleDocument>& link : adding) {
        Doc doc;
        doc.link = std::move(link);
        doc.snapshot = doc.link->bridge->snapshot();
        doc.told = doc.snapshot;
        doc.view = doc.link->view();
        // The shown one first.
        if (doc.view.shown)
            m_docs.insert(m_docs.begin(), std::move(doc));
        else
            m_docs.push_back(std::move(doc));
    }
    for (std::shared_ptr<AccessibleDocument> const& link : removing) {
        auto const found = std::ranges::find_if(m_docs, [&link](Doc const& doc) { return doc.link == link; });
        if (found == m_docs.end())
            continue;
        if (found->announced && found->told && m_window.content != 0) {
            emit(m_window.content, object_events, "ChildrenChanged", "remove", static_cast<std::int32_t>(found - m_docs.begin()), 0,
                dbus::Value::variant(reference_value(found->told->root)));
            if (m_cache_used) {
                dbus::Writer body;
                body.reference(m_bus->unique_name(), path_of(found->told->root));
                m_service->emit(cache_path, "org.a11y.atspi.Cache", "RemoveAccessible", "(so)", std::move(body));
            }
        }
        m_docs.erase(found);
    }
    // Each document: its view, its snapshot, its events.
    bool reorder = false;
    for (Doc& doc : m_docs) {
        AccessibleDocument::View const view = doc.link->view();
        bool const shown_changed = view.shown != doc.view.shown;
        bool const loading_changed = view.loading != doc.view.loading;
        doc.view = view;
        std::vector<a11y::Event> const events = doc.link->bridge->take_events();
        doc.snapshot = doc.link->bridge->snapshot();
        if (!doc.snapshot)
            continue;
        if (!doc.announced && m_window.content != 0 && doc.snapshot->root != 0) {
            doc.announced = true;
            doc.told = doc.snapshot;
            int const index = static_cast<int>(children_of(find(m_window.content)).size()) - 1;
            emit(m_window.content, object_events, "ChildrenChanged", "add", index, 0, dbus::Value::variant(reference_value(doc.snapshot->root)));
            continue;
        }
        if (shown_changed) {
            reorder = true;
            emit(doc.snapshot->root, object_events, "StateChanged", "showing", view.shown ? 1 : 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
        }
        if (loading_changed)
            emit(doc.snapshot->root, object_events, "StateChanged", "busy", view.loading ? 1 : 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
        emit_document_events(doc, events);
        doc.told = doc.snapshot;
    }
    if (reorder)
        std::ranges::stable_partition(m_docs, [](Doc const& doc) { return doc.view.shown; });
}

void AtspiBridge::emit_text_diff(NodeId id, atspi::FlatText const& before, atspi::FlatText const& after)
{
    constexpr char const* object_events = "org.a11y.atspi.Event.Object";
    atspi::TextChange const change = atspi::diff_text(before.text, after.text);
    if (change.removed_length > 0)
        emit(id, object_events, "TextChanged", "delete", static_cast<std::int32_t>(change.start), static_cast<std::int32_t>(change.removed_length),
            dbus::Value::variant(dbus::Value::string(change.removed)));
    if (change.inserted_length > 0)
        emit(id, object_events, "TextChanged", "insert", static_cast<std::int32_t>(change.start), static_cast<std::int32_t>(change.inserted_length),
            dbus::Value::variant(dbus::Value::string(change.inserted)));
}

void AtspiBridge::emit_document_events(Doc& doc, std::vector<a11y::Event> const& events)
{
    constexpr char const* object_events = "org.a11y.atspi.Event.Object";
    Snapshot const* const before = doc.told.get();
    Snapshot const* const after = doc.snapshot.get();
    // The objects whose children or text changed, said once each after the
    // rest, from the two snapshots: the model's events say where, the
    // snapshots what (an index, an offset).
    std::set<NodeId> changed_children;
    std::set<NodeId> changed_text;
    auto const exposed_parent = [&](Snapshot const* snapshot, NodeId id) -> NodeId {
        NodeData const* node = snapshot ? snapshot->find(id) : nullptr;
        if (node && text_like(*node))
            return node->parent;
        return id;
    };
    // A focus move the model says twice (its focused state, and Focus) is
    // said once as state-changed:focused, once as the focus event.
    std::set<NodeId> focused_by_state;
    for (a11y::Event const& event : events) {
        if (event.type == a11y::Event::Type::StateChanged && event.state == State::Focused && event.on)
            focused_by_state.insert(event.node);
    }
    for (a11y::Event const& event : events) {
        using Type = a11y::Event::Type;
        NodeData const* const node = after->find(event.node);
        switch (event.type) {
        case Type::ChildrenChanged:
            changed_children.insert(exposed_parent(after, event.node));
            changed_text.insert(exposed_parent(after, event.node));
            break;
        case Type::TextInserted:
        case Type::TextRemoved:
            changed_text.insert(exposed_parent(after, event.node));
            if (node && !text_like(*node) && node->children.empty() && has(*node, State::Editable)) {
                // A field: its own text, said straight from the event (its
                // offsets UTF-16, AT-SPI's characters).
                changed_text.erase(event.node);
                NodeData const* const old = before ? before->find(event.node) : nullptr;
                std::string const& basis = event.type == Type::TextInserted ? node->text : (old ? old->text : node->text);
                std::uint32_t const start = atspi::code_points_before(basis, event.offset);
                emit(event.node, object_events, "TextChanged", event.type == Type::TextInserted ? "insert" : "delete",
                    static_cast<std::int32_t>(start), static_cast<std::int32_t>(atspi::code_point_count(event.text)),
                    dbus::Value::variant(dbus::Value::string(event.text)));
            }
            break;
        case Type::NameChanged:
            if (node && text_like(*node))
                changed_text.insert(node->parent);
            else if (node)
                emit(event.node, object_events, "PropertyChange", "accessible-name", 0, 0, dbus::Value::variant(dbus::Value::string(node->name)));
            break;
        case Type::DescriptionChanged:
            if (node)
                emit(event.node, object_events, "PropertyChange", "accessible-description", 0, 0, dbus::Value::variant(dbus::Value::string(node->description)));
            break;
        case Type::RoleChanged:
            if (node) {
                Ref const ref = find(event.node);
                if (ref)
                    emit(event.node, object_events, "PropertyChange", "accessible-role", 0, 0, dbus::Value::variant(dbus::Value::uint32(role_of(ref))));
            }
            break;
        case Type::StateChanged: {
            auto const [name, number] = atspi::state_of_event(event.state, node);
            if (name.empty() || (node && text_like(*node)))
                break;
            // Disabled, offscreen and invisible are said as what AT-SPI
            // has: enabled (and sensitive), showing, visible, turned the other way.
            bool const inverted = event.state == State::Disabled || event.state == State::Offscreen || event.state == State::Invisible;
            bool const on = inverted ? !event.on : event.on;
            emit(event.node, object_events, "StateChanged", name, on ? 1 : 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            if (event.state == State::Disabled)
                emit(event.node, object_events, "StateChanged", "sensitive", on ? 1 : 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            if (event.state == State::Expanded) {
                NodeData const* const old = before ? before->find(event.node) : nullptr;
                if (old && has(*old, State::Expandable))
                    emit(event.node, object_events, "StateChanged", "collapsed", event.on ? 0 : 1, 0, dbus::Value::variant(dbus::Value::int32(0)));
            }
            break;
        }
        case Type::ValueChanged:
            if (node && (node->value_now || node->value_min || node->value_max))
                emit(event.node, object_events, "PropertyChange", "accessible-value", 0, 0,
                    dbus::Value::variant(dbus::Value::float64(node->value_now.value_or(0))));
            else if (node)
                changed_text.insert(event.node);
            break;
        case Type::CaretMoved:
            if (node) {
                std::uint32_t const at = atspi::code_points_before(node->text, event.offset);
                emit(event.node, object_events, "TextCaretMoved", "", static_cast<std::int32_t>(at), 0, dbus::Value::variant(dbus::Value::int32(0)));
            }
            break;
        case Type::TextSelectionChanged:
            emit(event.node, object_events, "TextSelectionChanged", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            break;
        case Type::SelectionChanged:
            emit(event.node, object_events, "SelectionChanged", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            break;
        case Type::Focus: {
            // The focus moved here: the node's focused state, and the legacy
            // focus event; a composite's active descendant besides.
            if (!node)
                break;
            if (!focused_by_state.contains(event.node))
                emit(event.node, object_events, "StateChanged", "focused", 1, 0, dbus::Value::variant(dbus::Value::int32(0)));
            emit(event.node, "org.a11y.atspi.Event.Focus", "Focus", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            for (NodeId walk = node->parent; walk != 0;) {
                NodeData const* const ancestor = after->find(walk);
                if (!ancestor)
                    break;
                for (auto const& [relation, targets] : ancestor->relations) {
                    if (relation == a11y::Relation::ActiveDescendant && std::ranges::find(targets, event.node) != targets.end())
                        emit(walk, object_events, "ActiveDescendantChanged", "", 0, 0, dbus::Value::variant(reference_value(event.node)));
                }
                walk = ancestor->parent;
            }
            break;
        }
        case Type::BoundsChanged:
            if (node && !text_like(*node)) {
                Ref const ref = find(event.node);
                if (std::optional<a11y::Rect> const rect = window_bounds(ref)) {
                    emit(event.node, object_events, "BoundsChanged", "", 0, 0,
                        dbus::Value::variant(dbus::Value::structure({ dbus::Value::int32(static_cast<std::int32_t>(rect->x)),
                            dbus::Value::int32(static_cast<std::int32_t>(rect->y)), dbus::Value::int32(static_cast<std::int32_t>(rect->width)),
                            dbus::Value::int32(static_cast<std::int32_t>(rect->height)) })));
                }
            }
            break;
        case Type::Announcement:
            // AtspiLive: 1 polite, 2 assertive.
            emit(event.node != 0 ? event.node : after->root, object_events, "Announcement", "", event.politeness == "assertive" ? 2 : 1, 0,
                dbus::Value::variant(dbus::Value::string(event.text)));
            break;
        case Type::LoadComplete:
            emit(after->root, "org.a11y.atspi.Event.Document", "LoadComplete", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            break;
        case Type::TreeChanged:
            // The queue overflowed: the document's children said again.
            changed_children.insert(after->root);
            break;
        }
    }
    std::vector<Doc> const before_docs = [&] {
        std::vector<Doc> docs;
        Doc old = doc;
        old.snapshot = doc.told;
        docs.push_back(std::move(old));
        return docs;
    }();
    for (NodeId const id : changed_children) {
        Ref const now = find(id);
        Ref const then = find_in(id, nullptr, before_docs);
        if (!now || !then)
            continue;
        std::vector<NodeId> const old_children = children_in(then, nullptr, before_docs);
        std::vector<NodeId> const new_children = children_of(now);
        for (std::size_t i = old_children.size(); i-- > 0;) {
            if (std::ranges::find(new_children, old_children[i]) == new_children.end()) {
                emit(id, object_events, "ChildrenChanged", "remove", static_cast<std::int32_t>(i), 0, dbus::Value::variant(reference_value(old_children[i])));
                if (m_cache_used) {
                    dbus::Writer body;
                    body.reference(m_bus->unique_name(), path_of(old_children[i]));
                    m_service->emit(cache_path, "org.a11y.atspi.Cache", "RemoveAccessible", "(so)", std::move(body));
                }
            }
        }
        for (std::size_t i = 0; i < new_children.size(); ++i) {
            if (std::ranges::find(old_children, new_children[i]) == old_children.end()) {
                emit(id, object_events, "ChildrenChanged", "add", static_cast<std::int32_t>(i), 0, dbus::Value::variant(reference_value(new_children[i])));
                if (m_cache_used) {
                    dbus::Writer body;
                    write_cache_item(body, find(new_children[i]));
                    m_service->emit(cache_path, "org.a11y.atspi.Cache", "AddAccessible", "((so)(so)(so)iiassusau)", std::move(body));
                }
            }
        }
    }
    for (NodeId const id : changed_text) {
        Ref const now = find(id);
        Ref const then = find_in(id, nullptr, before_docs);
        if (!now || !then || !offers_text(now))
            continue;
        emit_text_diff(id, flat_text_in(then, nullptr, before_docs), flat_text(now));
    }
}

void AtspiBridge::emit_chrome_events(std::shared_ptr<Snapshot const> const& before, std::vector<a11y::Event> const& events)
{
    constexpr char const* object_events = "org.a11y.atspi.Event.Object";
    // The chrome says its own events (focus, states, text); its children
    // are found changed by comparing the two snapshots, node by node.
    for (a11y::Event const& event : events) {
        using Type = a11y::Event::Type;
        NodeData const* const node = m_window.chrome->find(event.node);
        NodeData const* const old = before->find(event.node);
        switch (event.type) {
        case Type::Focus:
            if (!node)
                break;
            emit(event.node, object_events, "StateChanged", "focused", 1, 0, dbus::Value::variant(dbus::Value::int32(0)));
            emit(event.node, "org.a11y.atspi.Event.Focus", "Focus", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            break;
        case Type::StateChanged: {
            auto const [name, number] = atspi::state_of_event(event.state, node);
            if (!name.empty())
                emit(event.node, object_events, "StateChanged", name, event.on ? 1 : 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            break;
        }
        case Type::NameChanged:
            if (node)
                emit(event.node, object_events, "PropertyChange", "accessible-name", 0, 0, dbus::Value::variant(dbus::Value::string(node->name)));
            break;
        case Type::TextInserted:
        case Type::TextRemoved:
            if (node && old) {
                atspi::FlatText then;
                then.append_text(old->text, event.node, 0);
                atspi::FlatText now;
                now.append_text(node->text, event.node, 0);
                emit_text_diff(event.node, then, now);
            }
            break;
        case Type::CaretMoved:
            if (node)
                emit(event.node, object_events, "TextCaretMoved", "", static_cast<std::int32_t>(atspi::code_points_before(node->text, event.offset)), 0,
                    dbus::Value::variant(dbus::Value::int32(0)));
            break;
        case Type::TextSelectionChanged:
            emit(event.node, object_events, "TextSelectionChanged", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            break;
        case Type::SelectionChanged:
            emit(event.node, object_events, "SelectionChanged", "", 0, 0, dbus::Value::variant(dbus::Value::int32(0)));
            break;
        default:
            break;
        }
    }
    // Children: every chrome node present in both whose children differ.
    for (auto const& [id, node] : m_window.chrome->nodes) {
        NodeData const* const old = before->find(id);
        if (!old || old->children == node->children)
            continue;
        for (std::size_t i = old->children.size(); i-- > 0;) {
            if (std::ranges::find(node->children, old->children[i]) == node->children.end())
                emit(id, object_events, "ChildrenChanged", "remove", static_cast<std::int32_t>(i), 0, dbus::Value::variant(reference_value(old->children[i])));
        }
        for (std::size_t i = 0; i < node->children.size(); ++i) {
            if (std::ranges::find(old->children, node->children[i]) == old->children.end())
                emit(id, object_events, "ChildrenChanged", "add", static_cast<std::int32_t>(i), 0, dbus::Value::variant(reference_value(node->children[i])));
        }
    }
}

// --- The interfaces ---------------------------------------------------------------------

dbus::Reply returns(std::string signature)
{
    dbus::Reply reply;
    reply.signature = std::move(signature);
    return reply;
}

dbus::Reply returns_bool(bool value)
{
    dbus::Reply reply = returns("b");
    reply.body.boolean(value);
    return reply;
}

dbus::Reply returns_int(std::int32_t value)
{
    dbus::Reply reply = returns("i");
    reply.body.int32(value);
    return reply;
}

dbus::Reply returns_string(std::string_view value)
{
    dbus::Reply reply = returns("s");
    reply.body.string(value);
    return reply;
}

void write_rect(dbus::Writer& out, a11y::Rect const& rect)
{
    out.int32(static_cast<std::int32_t>(std::lround(rect.x)));
    out.int32(static_cast<std::int32_t>(std::lround(rect.y)));
    out.int32(static_cast<std::int32_t>(std::lround(rect.width)));
    out.int32(static_cast<std::int32_t>(std::lround(rect.height)));
}

void write_string_map(dbus::Writer& out, std::vector<std::pair<std::string, std::string>> const& map)
{
    out.array(8, [&] {
        for (auto const& [name, value] : map) {
            out.align(8);
            out.string(name);
            out.string(value);
        }
    });
}

std::optional<dbus::Value> version_only(std::string const&, std::string_view property)
{
    if (property == "version")
        return dbus::Value::uint32(1);
    return std::nullopt;
}

a11y::Rect AtspiBridge::in_coordinates(a11y::Rect rect, Ref const& ref, std::uint32_t type) const
{
    // AtspiCoordType: 0 screen, 1 window, 2 parent. A Wayland client is
    // not told where its window is on the screen: screen coordinates are
    // the window's, as GTK and the other engines answer there.
    if (type == 2) {
        if (std::optional<a11y::Rect> const parent = window_bounds(parent_of(ref))) {
            rect.x -= parent->x;
            rect.y -= parent->y;
        }
    }
    return rect;
}

std::vector<NodeId> AtspiBridge::descendants(Ref const& ref, bool deep) const
{
    std::vector<NodeId> out;
    std::vector<NodeId> stack = children_of(ref);
    std::ranges::reverse(stack);
    while (!stack.empty()) {
        NodeId const id = stack.back();
        stack.pop_back();
        out.push_back(id);
        if (!deep)
            continue;
        std::vector<NodeId> below = children_of(find(id));
        std::ranges::reverse(below);
        stack.insert(stack.end(), below.begin(), below.end());
    }
    return out;
}

void AtspiBridge::export_interfaces()
{
    using Property = dbus::Interface::Property;
    auto const make = [this](char const* short_name, std::string xml, std::vector<Property> properties,
                          std::optional<dbus::Reply> (AtspiBridge::*method)(dbus::Message const&),
                          std::optional<dbus::Value> (AtspiBridge::*property)(std::string const&, std::string_view)) {
        auto interface = std::make_shared<dbus::Interface>();
        interface->name = std::string(interface_prefix) + short_name;
        interface->introspection = std::move(xml);
        interface->properties = std::move(properties);
        if (method)
            interface->call = [this, method](dbus::Message const& call) { return (this->*method)(call); };
        if (property)
            interface->get = [this, property](std::string const& path, std::string_view name) { return (this->*property)(path, name); };
        else
            interface->get = version_only;
        return interface;
    };
    m_accessible = make("Accessible",
        introspection_of("org.a11y.atspi.Accessible",
            { "GetChildAtIndex(i)(so)", "GetChildren()a(so)", "GetIndexInParent()i", "GetRelationSet()a(ua(so))", "GetRole()u",
                "GetRoleName()s", "GetLocalizedRoleName()s", "GetState()au", "GetAttributes()a{ss}", "GetApplication()(so)",
                "GetInterfaces()as", "=version u r", "=Name s r", "=Description s r", "=Parent (so) r", "=ChildCount i r",
                "=Locale s r", "=AccessibleId s r", "=HelpText s r" }),
        { { "version", "u" }, { "Name", "s" }, { "Description", "s" }, { "Parent", "(so)" }, { "ChildCount", "i" },
            { "Locale", "s" }, { "AccessibleId", "s" }, { "HelpText", "s" } },
        &AtspiBridge::accessible, &AtspiBridge::accessible_property);
    {
        auto application = std::make_shared<dbus::Interface>(*make("Application",
            introspection_of("org.a11y.atspi.Application",
                { "GetLocale(u)s", "GetApplicationBusAddress()s", "=ToolkitName s r", "=Version s r", "=ToolkitVersion s r",
                    "=AtspiVersion s r", "=InterfaceVersion u r", "=Id i rw" }),
            { { "ToolkitName", "s" }, { "Version", "s" }, { "ToolkitVersion", "s" }, { "AtspiVersion", "s" }, { "InterfaceVersion", "u" },
                { "Id", "i", true } },
            &AtspiBridge::application, nullptr));
        application->get = [this](std::string const&, std::string_view name) -> std::optional<dbus::Value> {
            if (name == "ToolkitName")
                return dbus::Value::string("Sashfold");
            if (name == "Version" || name == "ToolkitVersion")
                return dbus::Value::string(SASHFOLD_VERSION);
            if (name == "AtspiVersion")
                return dbus::Value::string("2.1");
            if (name == "InterfaceVersion")
                return dbus::Value::uint32(0);
            if (name == "Id")
                return dbus::Value::int32(m_application_id);
            return std::nullopt;
        };
        application->set = [this](std::string const&, std::string_view, dbus::Value const& value) -> std::optional<dbus::Reply> {
            m_application_id = static_cast<std::int32_t>(value.as_int());
            return std::nullopt;
        };
        m_application = std::move(application);
    }
    m_component = make("Component",
        introspection_of("org.a11y.atspi.Component",
            { "Contains(iiu)b", "GetAccessibleAtPoint(iiu)(so)", "GetExtents(u)(iiii)", "GetPosition(u)ii", "GetSize()ii",
                "GetLayer()u", "GetMDIZOrder()n", "GrabFocus()b", "GetAlpha()d", "SetExtents(iiiiu)b", "SetPosition(iiu)b",
                "SetSize(ii)b", "ScrollTo(u)b", "ScrollToPoint(uii)b", "=version u r" }),
        { { "version", "u" } }, &AtspiBridge::component, nullptr);
    {
        auto action = std::make_shared<dbus::Interface>(*make("Action",
            introspection_of("org.a11y.atspi.Action",
                { "GetDescription(i)s", "GetName(i)s", "GetLocalizedName(i)s", "GetKeyBinding(i)s", "GetActions()a(sss)",
                    "DoAction(i)b", "=version u r", "=NActions i r" }),
            { { "version", "u" }, { "NActions", "i" } }, &AtspiBridge::action, nullptr));
        action->get = [this](std::string const& path, std::string_view name) -> std::optional<dbus::Value> {
            if (name == "version")
                return dbus::Value::uint32(1);
            Ref const ref = find(id_of_path(path).value_or(0));
            if (name == "NActions" && ref.node)
                return dbus::Value::int32(static_cast<std::int32_t>(ref.node->actions.size()));
            return std::nullopt;
        };
        m_action = std::move(action);
    }
    {
        auto text = std::make_shared<dbus::Interface>(*make("Text",
            introspection_of("org.a11y.atspi.Text",
                { "GetStringAtOffset(iu)sii", "GetText(ii)s", "SetCaretOffset(i)b", "GetTextBeforeOffset(iu)sii",
                    "GetTextAtOffset(iu)sii", "GetTextAfterOffset(iu)sii", "GetCharacterAtOffset(i)i", "GetAttributeValue(is)s",
                    "GetAttributes(i)a{ss}ii", "GetDefaultAttributes()a{ss}", "GetCharacterExtents(iu)iiii", "GetOffsetAtPoint(iiu)i",
                    "GetNSelections()i", "GetSelection(i)ii", "AddSelection(ii)b", "RemoveSelection(i)b", "SetSelection(iii)b",
                    "GetRangeExtents(iiu)iiii", "GetBoundedRanges(iiiiuuu)a(iisv)", "GetAttributeRun(ib)a{ss}ii",
                    "GetDefaultAttributeSet()a{ss}", "ScrollSubstringTo(iiu)b", "ScrollSubstringToPoint(iiuii)b", "=version u r",
                    "=CharacterCount i r", "=CaretOffset i r" }),
            { { "version", "u" }, { "CharacterCount", "i" }, { "CaretOffset", "i" } }, &AtspiBridge::text, nullptr));
        text->get = [this](std::string const& path, std::string_view name) -> std::optional<dbus::Value> {
            if (name == "version")
                return dbus::Value::uint32(1);
            Ref const ref = find(id_of_path(path).value_or(0));
            if (!ref.node)
                return std::nullopt;
            if (name == "CharacterCount")
                return dbus::Value::int32(static_cast<std::int32_t>(flat_text(ref).length));
            if (name == "CaretOffset") {
                if (ref.node->caret && has(*ref.node, State::Editable))
                    return dbus::Value::int32(static_cast<std::int32_t>(atspi::code_points_before(ref.node->text, *ref.node->caret)));
                return dbus::Value::int32(-1);
            }
            return std::nullopt;
        };
        m_text = std::move(text);
    }
    m_editable_text = make("EditableText",
        introspection_of("org.a11y.atspi.EditableText",
            { "SetTextContents(s)b", "InsertText(isi)b", "CopyText(ii)", "CutText(ii)b", "DeleteText(ii)b", "PasteText(i)b",
                "=version u r" }),
        { { "version", "u" } }, &AtspiBridge::editable_text, nullptr);
    {
        auto value = std::make_shared<dbus::Interface>(*make("Value",
            introspection_of("org.a11y.atspi.Value",
                { "=version u r", "=MinimumValue d r", "=MaximumValue d r", "=MinimumIncrement d r", "=CurrentValue d rw", "=Text s r" }),
            { { "version", "u" }, { "MinimumValue", "d" }, { "MaximumValue", "d" }, { "MinimumIncrement", "d" },
                { "CurrentValue", "d", true }, { "Text", "s" } },
            nullptr, &AtspiBridge::value_property));
        value->set = [this](std::string const& path, std::string_view, dbus::Value const& given) { return set_value(path, given); };
        m_value = std::move(value);
    }
    m_selection = make("Selection",
        introspection_of("org.a11y.atspi.Selection",
            { "GetSelectedChild(i)(so)", "SelectChild(i)b", "DeselectSelectedChild(i)b", "IsChildSelected(i)b", "SelectAll()b",
                "ClearSelection()b", "DeselectChild(i)b", "=version u r", "=NSelectedChildren i r" }),
        { { "version", "u" }, { "NSelectedChildren", "i" } }, &AtspiBridge::selection, nullptr);
    {
        auto selection = std::make_shared<dbus::Interface>(*m_selection);
        selection->get = [this](std::string const& path, std::string_view name) -> std::optional<dbus::Value> {
            if (name == "version")
                return dbus::Value::uint32(1);
            if (name != "NSelectedChildren")
                return std::nullopt;
            Ref const ref = find(id_of_path(path).value_or(0));
            std::int32_t count = 0;
            bool const deep = ref.node && (ref.node->role == Role::Tree || ref.node->role == Role::TreeGrid || ref.node->role == Role::Grid);
            for (NodeId const id : descendants(ref, deep)) {
                Ref const child = find(id);
                if (child.node && has(*child.node, State::Selected))
                    ++count;
            }
            return dbus::Value::int32(count);
        };
        m_selection = std::move(selection);
    }
    m_table = make("Table",
        introspection_of("org.a11y.atspi.Table",
            { "GetAccessibleAt(ii)(so)", "GetIndexAt(ii)i", "GetRowAtIndex(i)i", "GetColumnAtIndex(i)i", "GetRowDescription(i)s",
                "GetColumnDescription(i)s", "GetRowExtentAt(ii)i", "GetColumnExtentAt(ii)i", "GetRowHeader(i)(so)",
                "GetColumnHeader(i)(so)", "GetSelectedRows()ai", "GetSelectedColumns()ai", "IsRowSelected(i)b",
                "IsColumnSelected(i)b", "IsSelected(ii)b", "AddRowSelection(i)b", "AddColumnSelection(i)b",
                "RemoveRowSelection(i)b", "RemoveColumnSelection(i)b", "GetRowColumnExtentsAtIndex(i)biiiib", "=version u r",
                "=NRows i r", "=NColumns i r", "=Caption (so) r", "=Summary (so) r", "=NSelectedRows i r", "=NSelectedColumns i r" }),
        { { "version", "u" }, { "NRows", "i" }, { "NColumns", "i" }, { "Caption", "(so)" }, { "Summary", "(so)" },
            { "NSelectedRows", "i" }, { "NSelectedColumns", "i" } },
        &AtspiBridge::table, &AtspiBridge::table_property);
    m_table_cell = make("TableCell",
        introspection_of("org.a11y.atspi.TableCell",
            { "GetRowColumnSpan()biiii", "GetColumnHeaderCells()a(so)", "GetRowHeaderCells()a(so)", "=version u r",
                "=ColumnSpan i r", "=Position (ii) r", "=RowSpan i r", "=Table (so) r" }),
        { { "version", "u" }, { "ColumnSpan", "i" }, { "Position", "(ii)" }, { "RowSpan", "i" }, { "Table", "(so)" } },
        &AtspiBridge::table_cell, &AtspiBridge::table_cell_property);
    m_hyperlink = make("Hyperlink",
        introspection_of("org.a11y.atspi.Hyperlink",
            { "GetObject(i)(so)", "GetURI(i)s", "IsValid()b", "=version u r", "=NAnchors n r", "=StartIndex i r", "=EndIndex i r" }),
        { { "version", "u" }, { "NAnchors", "n" }, { "StartIndex", "i" }, { "EndIndex", "i" } }, &AtspiBridge::hyperlink,
        &AtspiBridge::hyperlink_property);
    m_hypertext = make("Hypertext", introspection_of("org.a11y.atspi.Hypertext", { "GetNLinks()i", "GetLink(i)(so)", "GetLinkIndex(i)i", "=version u r" }),
        { { "version", "u" } }, &AtspiBridge::hypertext, nullptr);
    {
        auto image = std::make_shared<dbus::Interface>(*make("Image",
            introspection_of("org.a11y.atspi.Image",
                { "GetImageExtents(u)(iiii)", "GetImagePosition(u)ii", "GetImageSize()ii", "=version u r", "=ImageDescription s r",
                    "=ImageLocale s r" }),
            { { "version", "u" }, { "ImageDescription", "s" }, { "ImageLocale", "s" } }, &AtspiBridge::image, nullptr));
        image->get = [this](std::string const& path, std::string_view name) -> std::optional<dbus::Value> {
            if (name == "version")
                return dbus::Value::uint32(1);
            Ref const ref = find(id_of_path(path).value_or(0));
            if (!ref.node)
                return std::nullopt;
            if (name == "ImageDescription")
                return dbus::Value::string(ref.node->name);
            if (name == "ImageLocale")
                return dbus::Value::string(ref.node->language);
            return std::nullopt;
        };
        m_image = std::move(image);
    }
    {
        auto document = std::make_shared<dbus::Interface>(*make("Document",
            introspection_of("org.a11y.atspi.Document",
                { "GetLocale()s", "GetAttributeValue(s)s", "GetAttributes()a{ss}", "GetTextSelections()a((so)i(so)ib)",
                    "SetTextSelections(a((so)i(so)ib))b", "=version u r", "=CurrentPageNumber i r", "=PageCount i r" }),
            { { "version", "u" }, { "CurrentPageNumber", "i" }, { "PageCount", "i" } }, &AtspiBridge::document, nullptr));
        document->get = [](std::string const&, std::string_view name) -> std::optional<dbus::Value> {
            if (name == "version")
                return dbus::Value::uint32(1);
            if (name == "CurrentPageNumber" || name == "PageCount")
                return dbus::Value::int32(-1); // a web page has no pages
            return std::nullopt;
        };
        m_document = std::move(document);
    }
    m_collection = make("Collection",
        introspection_of("org.a11y.atspi.Collection",
            { "GetMatches((aiia{ss}iaiiasib)uib)a(so)", "GetMatchesTo(o(aiia{ss}iaiiasib)uubib)a(so)",
                "GetMatchesFrom(o(aiia{ss}iaiiasib)uuib)a(so)", "GetActiveDescendant()(so)", "=version u r" }),
        { { "version", "u" } }, &AtspiBridge::collection, nullptr);
    m_cache = make("Cache",
        introspection_of("org.a11y.atspi.Cache",
            { "GetItems()a((so)(so)(so)iiassusau)", ":AddAccessible()((so)(so)(so)iiassusau)", ":RemoveAccessible()(so)", "=version u r" }),
        { { "version", "u" } }, &AtspiBridge::cache, nullptr);

    m_service->add_object(root_path, { m_accessible, m_application });
    m_service->add_object(cache_path, { m_cache });
    m_service->add_subtree(std::string(object_prefix), [this](std::string_view path) -> std::optional<std::vector<std::shared_ptr<dbus::Interface const>>> {
        std::optional<NodeId> const id = id_of_path(path);
        Ref const ref = id ? find(*id) : Ref {};
        if (!ref || ref.app)
            return std::nullopt;
        std::vector<std::shared_ptr<dbus::Interface const>> interfaces;
        for (std::string const& name : interfaces_of(ref)) {
            std::string_view const short_name = std::string_view(name).substr(interface_prefix.size());
            for (auto const* candidate : { &m_accessible, &m_component, &m_action, &m_text, &m_editable_text, &m_value, &m_selection,
                     &m_table, &m_table_cell, &m_hyperlink, &m_hypertext, &m_image, &m_document, &m_collection }) {
                if (*candidate && (*candidate)->name.ends_with(short_name) && (*candidate)->name.size() == name.size())
                    interfaces.push_back(*candidate);
            }
        }
        return interfaces;
    });
}

std::optional<dbus::Reply> AtspiBridge::accessible(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    std::string const& member = call.member;
    dbus::Reader args(call.body);
    if (member == "GetChildAtIndex" && call.signature == "i") {
        std::optional<std::int32_t> const index = args.int32();
        std::vector<NodeId> const children = children_of(ref);
        dbus::Reply reply = returns("(so)");
        if (index && *index >= 0 && static_cast<std::size_t>(*index) < children.size())
            write_reference(reply.body, children[static_cast<std::size_t>(*index)]);
        else
            write_null(reply.body);
        return reply;
    }
    if (member == "GetChildren") {
        dbus::Reply reply = returns("a(so)");
        std::vector<NodeId> const children = children_of(ref);
        reply.body.array(8, [&] {
            for (NodeId const child : children)
                write_reference(reply.body, child);
        });
        return reply;
    }
    if (member == "GetIndexInParent")
        return returns_int(index_in_parent(ref));
    if (member == "GetRelationSet") {
        dbus::Reply reply = returns("a(ua(so))");
        reply.body.array(8, [&] {
            if (!ref.node)
                return;
            for (auto const& [relation, targets] : ref.node->relations) {
                std::uint32_t const type = atspi::relation_of(relation);
                if (type == 0)
                    continue;
                std::vector<NodeId> exposed;
                for (NodeId target : targets) {
                    // A run of text named by a relation stands as its parent.
                    if (ref.doc) {
                        NodeData const* const node = ref.doc->snapshot->find(target);
                        if (node && text_like(*node))
                            target = node->parent;
                    }
                    if (find(target) && std::ranges::find(exposed, target) == exposed.end())
                        exposed.push_back(target);
                }
                if (exposed.empty())
                    continue;
                reply.body.align(8);
                reply.body.uint32(type);
                reply.body.array(8, [&] {
                    for (NodeId const target : exposed)
                        write_reference(reply.body, target);
                });
            }
        });
        return reply;
    }
    if (member == "GetRole") {
        dbus::Reply reply = returns("u");
        reply.body.uint32(role_of(ref));
        return reply;
    }
    if (member == "GetRoleName" || member == "GetLocalizedRoleName")
        return returns_string(atspi::role_name(role_of(ref)));
    if (member == "GetState") {
        atspi::StateSet const states = states_of(ref);
        dbus::Reply reply = returns("au");
        reply.body.array(4, [&] {
            reply.body.uint32(static_cast<std::uint32_t>(states));
            reply.body.uint32(static_cast<std::uint32_t>(states >> 32));
        });
        return reply;
    }
    if (member == "GetAttributes") {
        dbus::Reply reply = returns("a{ss}");
        std::vector<std::pair<std::string, std::string>> attributes;
        if (ref.app || ref.frame(m_window))
            attributes.emplace_back("toolkit", "Sashfold");
        else if (ref.node)
            attributes = atspi::attributes_of(*ref.node);
        write_string_map(reply.body, attributes);
        return reply;
    }
    if (member == "GetApplication") {
        dbus::Reply reply = returns("(so)");
        write_reference(reply.body, app_id);
        return reply;
    }
    if (member == "GetInterfaces") {
        dbus::Reply reply = returns("as");
        std::vector<std::string> const names = interfaces_of(ref);
        reply.body.array(4, [&] {
            for (std::string const& name : names)
                reply.body.string(name);
        });
        return reply;
    }
    return std::nullopt;
}

std::optional<dbus::Value> AtspiBridge::accessible_property(std::string const& path, std::string_view property)
{
    Ref const ref = find(id_of_path(path).value_or(0));
    if (!ref)
        return std::nullopt;
    if (property == "version")
        return dbus::Value::uint32(1);
    if (property == "Name")
        return dbus::Value::string(name_of(ref));
    if (property == "Description")
        return dbus::Value::string(ref.node ? ref.node->description : std::string());
    if (property == "Parent") {
        if (ref.app) {
            if (m_desktop_name.empty())
                return dbus::Value::structure({ dbus::Value::string(m_bus->unique_name()), dbus::Value::object_path(null_path) });
            return dbus::Value::structure({ dbus::Value::string(m_desktop_name), dbus::Value::object_path(m_desktop_path) });
        }
        Ref const parent = parent_of(ref);
        return parent ? reference_value(parent.app ? app_id : parent.id)
                      : dbus::Value::structure({ dbus::Value::string(m_bus->unique_name()), dbus::Value::object_path(null_path) });
    }
    if (property == "ChildCount")
        return dbus::Value::int32(static_cast<std::int32_t>(children_of(ref).size()));
    if (property == "Locale") {
        if (ref.node && !ref.node->language.empty())
            return dbus::Value::string(ref.node->language);
        char const* const lang = std::getenv("LANG");
        std::string locale = lang && *lang ? lang : "C";
        return dbus::Value::string(locale.substr(0, locale.find('.')));
    }
    if (property == "AccessibleId" || property == "HelpText")
        return dbus::Value::string(std::string());
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::application(dbus::Message const& call)
{
    if (call.member == "GetLocale") {
        char const* const lang = std::getenv("LANG");
        std::string const locale = lang && *lang ? lang : "C";
        return returns_string(locale.substr(0, locale.find('.')));
    }
    if (call.member == "GetApplicationBusAddress")
        return returns_string("");
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::component(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    dbus::Reader args(call.body);
    std::string const& member = call.member;
    std::optional<a11y::Rect> const bounds = window_bounds(ref);
    auto const contains = [](a11y::Rect const& rect, float x, float y) {
        return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
    };
    if (member == "Contains" || member == "GetAccessibleAtPoint") {
        std::optional<std::int32_t> const x = args.int32();
        std::optional<std::int32_t> const y = args.int32();
        std::optional<std::uint32_t> const type = args.uint32();
        if (!x || !y || !type)
            return dbus::Reply::failure(std::string(dbus::error_invalid_args), "(iiu)");
        float px = static_cast<float>(*x);
        float py = static_cast<float>(*y);
        if (*type == 2) {
            if (std::optional<a11y::Rect> const parent = window_bounds(parent_of(ref))) {
                px += parent->x;
                py += parent->y;
            }
        }
        if (member == "Contains")
            return returns_bool(bounds && contains(*bounds, px, py));
        // The deepest object under the point, the last-drawn sibling first;
        // a document behind the tab in front is not under any point.
        NodeId found = 0;
        Ref walk = ref;
        for (;;) {
            NodeId next = 0;
            std::vector<NodeId> const children = children_of(walk);
            for (auto child = children.rbegin(); child != children.rend(); ++child) {
                Ref const candidate = find(*child);
                if (candidate.doc && !candidate.doc->view.shown)
                    continue;
                std::optional<a11y::Rect> const box = window_bounds(candidate);
                if (box && contains(*box, px, py)) {
                    next = *child;
                    break;
                }
            }
            if (next == 0)
                break;
            found = next;
            walk = find(next);
        }
        dbus::Reply reply = returns("(so)");
        if (found != 0)
            write_reference(reply.body, found);
        else
            write_null(reply.body);
        return reply;
    }
    if (member == "GetExtents" || member == "GetPosition") {
        std::optional<std::uint32_t> const type = args.uint32();
        a11y::Rect const rect = in_coordinates(bounds.value_or(a11y::Rect { -1, -1, -1, -1 }), ref, type.value_or(1));
        if (member == "GetExtents") {
            dbus::Reply reply = returns("(iiii)");
            reply.body.align(8);
            write_rect(reply.body, bounds ? rect : a11y::Rect { -1, -1, -1, -1 });
            return reply;
        }
        dbus::Reply reply = returns("ii");
        reply.body.int32(bounds ? static_cast<std::int32_t>(std::lround(rect.x)) : -1);
        reply.body.int32(bounds ? static_cast<std::int32_t>(std::lround(rect.y)) : -1);
        return reply;
    }
    if (member == "GetSize") {
        dbus::Reply reply = returns("ii");
        reply.body.int32(bounds ? static_cast<std::int32_t>(std::lround(bounds->width)) : -1);
        reply.body.int32(bounds ? static_cast<std::int32_t>(std::lround(bounds->height)) : -1);
        return reply;
    }
    if (member == "GetLayer") {
        // AtspiComponentLayer: 3 widget, 5 popup (a menu), 7 window.
        dbus::Reply reply = returns("u");
        std::uint32_t const role = role_of(ref);
        reply.body.uint32(ref.frame(m_window) ? 7 : role == atspi::role::menu || role == atspi::role::popup_menu ? 5 : 3);
        return reply;
    }
    if (member == "GetMDIZOrder") {
        dbus::Reply reply = returns("n");
        reply.body.int16(ref.frame(m_window) ? 0 : -1);
        return reply;
    }
    if (member == "GetAlpha") {
        dbus::Reply reply = returns("d");
        reply.body.float64(1.0);
        return reply;
    }
    if (member == "GrabFocus") {
        if (!has(*ref.node, State::Focusable) && !ref.frame(m_window))
            return returns_bool(false);
        if (ref.doc)
            return returns_bool(tell_page(*ref.doc, [id = ref.id](a11y::Tree& tree) { tree.focus(id); }));
        tell_window({ ref.id, "focus", {}, 0, 0 });
        return returns_bool(true);
    }
    if (member == "ScrollTo" || member == "ScrollToPoint") {
        if (ref.doc && member == "ScrollTo")
            return returns_bool(tell_page(*ref.doc, [id = ref.id](a11y::Tree& tree) { tree.do_action(id, "scroll-into-view"); }));
        return returns_bool(false);
    }
    if (member == "SetExtents" || member == "SetPosition" || member == "SetSize")
        return returns_bool(false); // nothing of a page or the chrome is moved by a tool
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::action(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    std::vector<std::string> const& actions = ref.node->actions;
    if (call.member == "GetActions") {
        dbus::Reply reply = returns("a(sss)");
        reply.body.array(8, [&] {
            for (std::size_t i = 0; i < actions.size(); ++i) {
                reply.body.align(8);
                reply.body.string(actions[i]);
                reply.body.string(actions[i]);
                reply.body.string(i == 0 ? ref.node->key_shortcuts : std::string());
            }
        });
        return reply;
    }
    dbus::Reader args(call.body);
    std::optional<std::int32_t> const index = args.int32();
    bool const valid = index && *index >= 0 && static_cast<std::size_t>(*index) < actions.size();
    std::string const name = valid ? actions[static_cast<std::size_t>(*index)] : std::string();
    if (call.member == "GetName" || call.member == "GetLocalizedName" || call.member == "GetDescription")
        return returns_string(name);
    if (call.member == "GetKeyBinding")
        return returns_string(valid && *index == 0 ? ref.node->key_shortcuts : std::string());
    if (call.member == "DoAction") {
        if (!valid)
            return returns_bool(false);
        if (ref.doc)
            return returns_bool(tell_page(*ref.doc, [id = ref.id, name](a11y::Tree& tree) { tree.do_action(id, name); }));
        tell_window({ ref.id, name, {}, 0, 0 });
        return returns_bool(true);
    }
    return std::nullopt;
}

std::vector<atspi::Range> AtspiBridge::layout_lines(Ref const& ref, atspi::FlatText const& flat) const
{
    // The lines as the page laid the text out: each run's inline boxes,
    // asked of the page; a box on another line than the one before it
    // starts a line. Without a page (the chrome), or should the page not
    // answer in time, the text's own newlines.
    if (!ref.doc || flat.pieces.empty())
        return atspi::lines_by_newline(flat.text);
    struct Segment {
        std::size_t start = 0;
        std::size_t end = 0;
        float top = 0;
        float bottom = 0;
    };
    struct Leaf {
        NodeId id;
        std::size_t start;
        std::string text;
    };
    std::vector<Leaf> leaves;
    std::vector<Segment> segments;
    for (atspi::FlatText::Piece const& piece : flat.pieces) {
        if (piece.object) {
            if (std::optional<a11y::Rect> const box = find(piece.node).node ? find(piece.node).node->bounds : std::nullopt)
                segments.push_back({ piece.start, piece.end, box->y, box->y + box->height });
            continue;
        }
        NodeData const* const node = ref.doc->snapshot->find(piece.node);
        if (node && node != ref.node)
            leaves.push_back({ piece.node, piece.start, node->text });
    }
    if (!leaves.empty()) {
        std::optional<std::vector<Segment>> const boxes = ask_page<std::vector<Segment>>(*ref.doc, [leaves](a11y::Tree& tree) {
            std::vector<Segment> found;
            for (Leaf const& leaf : leaves) {
                for (a11y::Tree::InlineTextBox const& box : tree.inline_text_boxes(leaf.id))
                    found.push_back({ leaf.start + atspi::code_points_before(leaf.text, box.start), leaf.start + atspi::code_points_before(leaf.text, box.end),
                        box.bounds.y, box.bounds.y + box.bounds.height });
            }
            return found;
        });
        if (!boxes)
            return atspi::lines_by_newline(flat.text);
        segments.insert(segments.end(), boxes->begin(), boxes->end());
    }
    std::ranges::sort(segments, [](Segment const& a, Segment const& b) { return a.start < b.start; });
    std::vector<atspi::Range> lines;
    float top = 0;
    float bottom = 0;
    for (Segment const& segment : segments) {
        // The same line while the boxes overlap vertically.
        bool const same = !lines.empty() && segment.top < bottom && segment.bottom > top;
        if (same) {
            lines.back().end = std::max(lines.back().end, segment.end);
            top = std::min(top, segment.top);
            bottom = std::max(bottom, segment.bottom);
        } else {
            if (!lines.empty())
                lines.back().end = std::max(lines.back().end, segment.start);
            lines.push_back({ lines.empty() ? 0 : segment.start, segment.end });
            top = segment.top;
            bottom = segment.bottom;
        }
    }
    if (lines.empty())
        return atspi::lines_by_newline(flat.text);
    lines.back().end = flat.length;
    return lines;
}

std::optional<a11y::Rect> AtspiBridge::character_extents(Ref const& ref, atspi::FlatText const& flat, std::size_t from, std::size_t to) const
{
    // The union of the boxes the characters from and to have: a run's from
    // the page's layout, an embedded object's its own.
    std::optional<a11y::Rect> total;
    auto const join = [&total](a11y::Rect const& rect) {
        if (!total) {
            total = rect;
            return;
        }
        float const right = std::max(total->x + total->width, rect.x + rect.width);
        float const bottom = std::max(total->y + total->height, rect.y + rect.height);
        total->x = std::min(total->x, rect.x);
        total->y = std::min(total->y, rect.y);
        total->width = right - total->x;
        total->height = bottom - total->y;
    };
    struct Ask {
        NodeId id;
        std::uint32_t from;
        std::uint32_t to;
    };
    std::vector<Ask> asks;
    for (atspi::FlatText::Piece const& piece : flat.pieces) {
        if (piece.end <= from || piece.start >= to)
            continue;
        if (piece.object) {
            if (std::optional<a11y::Rect> const box = window_bounds(find(piece.node)))
                join(*box);
            continue;
        }
        NodeData const* const node = ref.doc ? ref.doc->snapshot->find(piece.node) : nullptr;
        if (!node || node == ref.node || !ref.doc) {
            // A field's own text (or the chrome's): its box stands for it.
            if (std::optional<a11y::Rect> const box = window_bounds(ref))
                join(*box);
            continue;
        }
        std::size_t const a = std::max(from, piece.start) - piece.start;
        std::size_t const b = std::min(to, piece.end) - piece.start;
        asks.push_back({ piece.node, atspi::utf16_before(node->text, static_cast<std::uint32_t>(a)),
            atspi::utf16_before(node->text, static_cast<std::uint32_t>(b)) });
    }
    if (!asks.empty() && ref.doc) {
        std::optional<std::vector<a11y::Rect>> const rects = ask_page<std::vector<a11y::Rect>>(*ref.doc, [asks](a11y::Tree& tree) {
            std::vector<a11y::Rect> found;
            for (Ask const& ask : asks) {
                std::vector<a11y::Rect> const boxes = tree.text_bounds(ask.id, ask.from, ask.to);
                found.insert(found.end(), boxes.begin(), boxes.end());
            }
            return found;
        });
        if (rects) {
            for (a11y::Rect rect : *rects) {
                rect.x += m_window.content_x - ref.doc->view.scroll_x;
                rect.y += m_window.content_y - ref.doc->view.scroll_y;
                join(rect);
            }
        }
    }
    return total;
}

std::optional<std::size_t> AtspiBridge::offset_at_point(Ref const& ref, atspi::FlatText const& flat, float x, float y) const
{
    // An embedded object under the point is its own character; a run's
    // characters are asked of the page one box at a time.
    for (atspi::FlatText::Piece const& piece : flat.pieces) {
        if (!piece.object)
            continue;
        if (std::optional<a11y::Rect> const box = window_bounds(find(piece.node));
            box && x >= box->x && y >= box->y && x < box->x + box->width && y < box->y + box->height)
            return piece.start;
    }
    if (!ref.doc)
        return std::nullopt;
    struct Leaf {
        NodeId id;
        std::size_t start;
        std::string text;
    };
    std::vector<Leaf> leaves;
    for (atspi::FlatText::Piece const& piece : flat.pieces) {
        NodeData const* const node = piece.object ? nullptr : ref.doc->snapshot->find(piece.node);
        if (node && node != ref.node)
            leaves.push_back({ piece.node, piece.start, node->text });
    }
    float const page_x = x - m_window.content_x + ref.doc->view.scroll_x;
    float const page_y = y - m_window.content_y + ref.doc->view.scroll_y;
    std::optional<std::optional<std::size_t>> const answer = ask_page<std::optional<std::size_t>>(*ref.doc, [leaves, page_x, page_y](a11y::Tree& tree) -> std::optional<std::size_t> {
        for (Leaf const& leaf : leaves) {
            for (a11y::Tree::InlineTextBox const& box : tree.inline_text_boxes(leaf.id)) {
                a11y::Rect const& r = box.bounds;
                if (page_x < r.x || page_y < r.y || page_x >= r.x + r.width || page_y >= r.y + r.height)
                    continue;
                for (std::uint32_t at = box.start; at < box.end; ++at) {
                    for (a11y::Rect const& c : tree.text_bounds(leaf.id, at, at + 1)) {
                        if (page_x >= c.x && page_x < c.x + c.width)
                            return leaf.start + atspi::code_points_before(leaf.text, at);
                    }
                }
                return leaf.start + atspi::code_points_before(leaf.text, box.start);
            }
        }
        return std::nullopt;
    });
    return answer ? *answer : std::nullopt;
}

std::vector<std::pair<std::string, std::string>> AtspiBridge::text_attributes(Ref const& ref, atspi::FlatText const& flat, std::size_t offset,
    std::size_t& start, std::size_t& end) const
{
    // A run's attributes are its element's style (the ATK text attribute
    // names: family-name, size in points, weight, style, fg-color,
    // bg-color, underline, strikethrough, language, invalid); an embedded
    // object is a run of its own with none.
    start = 0;
    end = flat.length;
    atspi::FlatText::Piece const* const piece = flat.piece_at(offset);
    if (!piece)
        return {};
    start = piece->start;
    end = piece->end;
    std::vector<std::pair<std::string, std::string>> attributes;
    if (piece->object || !ref.doc)
        return attributes;
    NodeData const* const leaf = ref.doc->snapshot->find(piece->node);
    if (!leaf)
        return attributes;
    if (!leaf->language.empty() || !ref.node->language.empty())
        attributes.emplace_back("language", leaf->language.empty() ? ref.node->language : leaf->language);
    for (NodeId walk = piece->node; walk != 0;) {
        NodeData const* const node = ref.doc->snapshot->find(walk);
        if (!node)
            break;
        if (node->invalid == "spelling" || node->invalid == "grammar") {
            attributes.emplace_back("invalid", node->invalid);
            break;
        }
        walk = node->parent;
    }
    NodeId const id = piece->node;
    std::optional<std::vector<std::pair<std::string, std::string>>> const style = ask_page<std::vector<std::pair<std::string, std::string>>>(*ref.doc,
        [id](a11y::Tree& tree) {
            std::vector<std::pair<std::string, std::string>> out;
            NodeData const* const node = tree.node(id);
            dom::Node const* const dom = node ? node->dom : nullptr;
            dom::Node const* const holder = dom && !dom->is_element() ? dom->parent() : dom;
            a11y::PageSource* const source = tree.semantics().source();
            if (!holder || !holder->is_element() || !source)
                return out;
            css::ComputedStyle const* const computed = source->style(static_cast<dom::Element const&>(*holder));
            if (!computed)
                return out;
            if (computed->font_family && !computed->font_family->empty())
                out.emplace_back("family-name", computed->font_family->front());
            char size[32];
            std::snprintf(size, sizeof size, "%gpt", static_cast<double>(computed->font_size) * 0.75);
            out.emplace_back("size", size);
            out.emplace_back("weight", std::to_string(computed->font_weight));
            out.emplace_back("style", computed->font_style == css::FontStyle::Italic ? "italic" : computed->font_style == css::FontStyle::Oblique ? "oblique" : "normal");
            auto const color = [](Color const& c) { return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b); };
            out.emplace_back("fg-color", color(computed->color));
            if (computed->background_color.a != 0)
                out.emplace_back("bg-color", color(computed->background_color));
            out.emplace_back("underline", computed->text_decoration == css::TextDecorationLine::Underline ? "single" : "none");
            out.emplace_back("strikethrough", computed->text_decoration == css::TextDecorationLine::LineThrough ? "true" : "false");
            return out;
        });
    if (style)
        attributes.insert(attributes.end(), style->begin(), style->end());
    return attributes;
}

std::optional<dbus::Reply> AtspiBridge::text(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    atspi::FlatText const flat = flat_text(ref);
    std::string const& member = call.member;
    dbus::Reader args(call.body);
    auto const clamp = [&flat](std::int32_t offset) {
        return offset < 0 ? flat.length : std::min(static_cast<std::size_t>(offset), flat.length);
    };
    auto const range_reply = [&flat](atspi::Range const range) {
        dbus::Reply reply = returns("sii");
        reply.body.string(atspi::slice(flat.text, range.start, range.end));
        reply.body.int32(static_cast<std::int32_t>(range.start));
        reply.body.int32(static_cast<std::int32_t>(range.end));
        return reply;
    };
    if (member == "GetText") {
        std::optional<std::int32_t> const from = args.int32();
        std::optional<std::int32_t> const to = args.int32();
        if (!from || !to)
            return dbus::Reply::failure(std::string(dbus::error_invalid_args), "(ii)");
        std::size_t const start = std::min(static_cast<std::size_t>(std::max(0, *from)), flat.length);
        return returns_string(atspi::slice(flat.text, start, std::max(start, clamp(*to))));
    }
    if (member == "GetCharacterAtOffset") {
        std::optional<std::int32_t> const at = args.int32();
        return returns_int(at && *at >= 0 ? static_cast<std::int32_t>(atspi::character_at(flat.text, static_cast<std::size_t>(*at))) : 0);
    }
    if (member == "GetTextAtOffset" || member == "GetTextBeforeOffset" || member == "GetTextAfterOffset" || member == "GetStringAtOffset") {
        std::optional<std::int32_t> const at = args.int32();
        std::optional<std::uint32_t> const kind = args.uint32();
        if (!at || !kind)
            return dbus::Reply::failure(std::string(dbus::error_invalid_args), "(iu)");
        std::size_t const offset = clamp(*at);
        bool const lines_needed = member == "GetStringAtOffset" ? *kind == 3 : (*kind == 5 || *kind == 6);
        std::vector<atspi::Range> const lines = lines_needed ? layout_lines(ref, flat) : std::vector<atspi::Range> {};
        if (member == "GetStringAtOffset")
            return range_reply(atspi::granule_at(flat.text, lines, offset, *kind));
        if (member == "GetTextAtOffset")
            return range_reply(atspi::boundary_at(flat.text, lines, offset, *kind));
        if (member == "GetTextBeforeOffset")
            return range_reply(atspi::boundary_before(flat.text, lines, offset, *kind));
        return range_reply(atspi::boundary_after(flat.text, lines, offset, *kind));
    }
    if (member == "SetCaretOffset" || member == "SetSelection" || member == "AddSelection") {
        std::int32_t selection = 0;
        if (member == "SetSelection")
            selection = args.int32().value_or(-1);
        std::optional<std::int32_t> const from = args.int32();
        std::optional<std::int32_t> const to = member == "SetCaretOffset" ? from : args.int32();
        if (!from || !to || selection != 0 || !has(*ref.node, State::Editable))
            return returns_bool(false);
        std::uint32_t const a = atspi::utf16_before(ref.node->text, static_cast<std::uint32_t>(clamp(*from)));
        std::uint32_t const b = atspi::utf16_before(ref.node->text, static_cast<std::uint32_t>(clamp(*to)));
        if (ref.doc)
            return returns_bool(tell_page(*ref.doc, [id = ref.id, a, b](a11y::Tree& tree) { tree.set_text_selection(id, a, b); }));
        tell_window({ ref.id, a == b ? "set-caret" : "set-selection", {}, a, b });
        return returns_bool(true);
    }
    if (member == "RemoveSelection") {
        if (!has(*ref.node, State::Editable) || !ref.node->caret)
            return returns_bool(false);
        std::uint32_t const caret = *ref.node->caret;
        if (ref.doc)
            return returns_bool(tell_page(*ref.doc, [id = ref.id, caret](a11y::Tree& tree) { tree.set_text_selection(id, caret, caret); }));
        tell_window({ ref.id, "set-caret", {}, caret, caret });
        return returns_bool(true);
    }
    if (member == "GetNSelections") {
        auto const& selected = ref.node->text_selection;
        return returns_int(selected && selected->first != selected->second ? 1 : 0);
    }
    if (member == "GetSelection") {
        auto const& selected = ref.node->text_selection;
        dbus::Reply reply = returns("ii");
        std::int32_t const index = args.int32().value_or(-1);
        if (index == 0 && selected && selected->first != selected->second) {
            std::uint32_t const a = std::min(selected->first, selected->second);
            std::uint32_t const b = std::max(selected->first, selected->second);
            reply.body.int32(static_cast<std::int32_t>(atspi::code_points_before(ref.node->text, a)));
            reply.body.int32(static_cast<std::int32_t>(atspi::code_points_before(ref.node->text, b)));
        } else {
            reply.body.int32(0);
            reply.body.int32(0);
        }
        return reply;
    }
    if (member == "GetCharacterExtents" || member == "GetRangeExtents") {
        std::optional<std::int32_t> const from = args.int32();
        std::optional<std::int32_t> const to = member == "GetRangeExtents" ? args.int32() : std::optional<std::int32_t>(from ? *from + 1 : 0);
        std::uint32_t const type = args.uint32().value_or(1);
        std::optional<a11y::Rect> const box = from && to ? character_extents(ref, flat, clamp(*from), clamp(*to)) : std::nullopt;
        dbus::Reply reply = returns("iiii");
        write_rect(reply.body, box ? in_coordinates(*box, ref, type) : a11y::Rect { 0, 0, 0, 0 });
        return reply;
    }
    if (member == "GetOffsetAtPoint") {
        std::optional<std::int32_t> const x = args.int32();
        std::optional<std::int32_t> const y = args.int32();
        std::uint32_t const type = args.uint32().value_or(1);
        float px = static_cast<float>(x.value_or(0));
        float py = static_cast<float>(y.value_or(0));
        if (type == 2) {
            if (std::optional<a11y::Rect> const parent = window_bounds(parent_of(ref))) {
                px += parent->x;
                py += parent->y;
            }
        }
        std::optional<std::size_t> const at = offset_at_point(ref, flat, px, py);
        return returns_int(at ? static_cast<std::int32_t>(*at) : -1);
    }
    if (member == "GetAttributes" || member == "GetAttributeRun" || member == "GetAttributeValue") {
        std::int32_t const at = args.int32().value_or(0);
        std::size_t start = 0;
        std::size_t end = 0;
        std::vector<std::pair<std::string, std::string>> const attributes = text_attributes(ref, flat, clamp(at), start, end);
        if (member == "GetAttributeValue") {
            std::string const name = args.string().value_or("");
            for (auto const& [key, value] : attributes) {
                if (key == name)
                    return returns_string(value);
            }
            return returns_string("");
        }
        dbus::Reply reply = returns("a{ss}ii");
        write_string_map(reply.body, attributes);
        reply.body.int32(static_cast<std::int32_t>(start));
        reply.body.int32(static_cast<std::int32_t>(end));
        return reply;
    }
    if (member == "GetDefaultAttributes" || member == "GetDefaultAttributeSet") {
        dbus::Reply reply = returns("a{ss}");
        std::size_t start = 0;
        std::size_t end = 0;
        // The node's first run's attributes stand as its default.
        write_string_map(reply.body, flat.length > 0 ? text_attributes(ref, flat, 0, start, end) : std::vector<std::pair<std::string, std::string>> {});
        return reply;
    }
    if (member == "GetBoundedRanges") {
        dbus::Reply reply = returns("a(iisv)");
        reply.body.array(8, [] { });
        return reply;
    }
    if (member == "ScrollSubstringTo") {
        if (ref.doc)
            return returns_bool(tell_page(*ref.doc, [id = ref.id](a11y::Tree& tree) { tree.do_action(id, "scroll-into-view"); }));
        return returns_bool(false);
    }
    if (member == "ScrollSubstringToPoint")
        return returns_bool(false);
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::editable_text(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node || !has(*ref.node, State::Editable) || has(*ref.node, State::ReadOnly))
        return call.member == "CopyText" ? std::optional<dbus::Reply>(returns("")) : std::optional<dbus::Reply>(returns_bool(false));
    std::string const& text = ref.node->text;
    std::size_t const length = atspi::code_point_count(text);
    dbus::Reader args(call.body);
    std::optional<std::string> value;
    if (call.member == "SetTextContents") {
        value = args.string();
    } else if (call.member == "InsertText") {
        std::int32_t const at = args.int32().value_or(0);
        std::string const inserted = args.string().value_or("");
        std::int32_t const count = args.int32().value_or(-1);
        std::size_t const position = std::min(static_cast<std::size_t>(std::max(0, at)), length);
        std::string const piece = count < 0 ? inserted : atspi::slice(inserted, 0, static_cast<std::size_t>(count));
        value = atspi::slice(text, 0, position) + piece + atspi::slice(text, position, length);
    } else if (call.member == "DeleteText" || call.member == "CutText") {
        std::int32_t const from = args.int32().value_or(0);
        std::int32_t const to = args.int32().value_or(0);
        std::size_t const a = std::min(static_cast<std::size_t>(std::max(0, from)), length);
        std::size_t const b = to < 0 ? length : std::min(static_cast<std::size_t>(to), length);
        if (call.member == "CutText")
            return returns_bool(false); // the clipboard is the window's, which a tool does not reach
        value = atspi::slice(text, 0, a) + atspi::slice(text, std::max(a, b), length);
    } else if (call.member == "CopyText") {
        return returns("");
    } else if (call.member == "PasteText") {
        return returns_bool(false);
    } else {
        return std::nullopt;
    }
    if (!value)
        return dbus::Reply::failure(std::string(dbus::error_invalid_args), "no text");
    if (ref.doc)
        return returns_bool(tell_page(*ref.doc, [id = ref.id, value = *value](a11y::Tree& tree) { tree.set_value(id, value); }));
    tell_window({ ref.id, "set-text", *value, 0, 0 });
    return returns_bool(true);
}

std::optional<dbus::Value> AtspiBridge::value_property(std::string const& path, std::string_view property)
{
    Ref const ref = find(id_of_path(path).value_or(0));
    if (!ref.node)
        return std::nullopt;
    NodeData const& node = *ref.node;
    if (property == "version")
        return dbus::Value::uint32(1);
    if (property == "MinimumValue")
        return dbus::Value::float64(node.value_min.value_or(0));
    if (property == "MaximumValue")
        return dbus::Value::float64(node.value_max.value_or(0));
    if (property == "MinimumIncrement")
        return dbus::Value::float64(node.value_step.value_or(0));
    if (property == "CurrentValue")
        return dbus::Value::float64(node.value_now.value_or(0));
    if (property == "Text")
        return dbus::Value::string(node.value_text);
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::set_value(std::string const& path, dbus::Value const& value)
{
    Ref const ref = find(id_of_path(path).value_or(0));
    if (!ref.doc || has(*ref.node, State::ReadOnly) || has(*ref.node, State::Disabled))
        return dbus::Reply::failure(std::string(dbus::error_property_read_only), "the value cannot be set");
    char text[64];
    std::snprintf(text, sizeof text, "%g", value.real);
    tell_page(*ref.doc, [id = ref.id, set = std::string(text)](a11y::Tree& tree) { tree.set_value(id, set); });
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::selection(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    // Its children (a tree's and a grid's items at any depth) that can be
    // selected; ARIA lets a tool do nothing to a selection but what a
    // node's own "select" action does (role-map-listbox and the others:
    // the methods answer false).
    bool const deep = ref.node->role == Role::Tree || ref.node->role == Role::TreeGrid || ref.node->role == Role::Grid;
    std::vector<NodeId> const children = children_of(ref);
    std::vector<NodeId> selected;
    for (NodeId const id : descendants(ref, deep)) {
        Ref const child = find(id);
        if (child.node && has(*child.node, State::Selected))
            selected.push_back(id);
    }
    dbus::Reader args(call.body);
    std::int32_t const index = args.int32().value_or(-1);
    if (call.member == "GetSelectedChild") {
        dbus::Reply reply = returns("(so)");
        if (index >= 0 && static_cast<std::size_t>(index) < selected.size())
            write_reference(reply.body, selected[static_cast<std::size_t>(index)]);
        else
            write_null(reply.body);
        return reply;
    }
    if (call.member == "IsChildSelected")
        return returns_bool(index >= 0 && static_cast<std::size_t>(index) < children.size()
            && std::ranges::find(selected, children[static_cast<std::size_t>(index)]) != selected.end());
    if (call.member == "SelectChild") {
        if (index < 0 || static_cast<std::size_t>(index) >= children.size())
            return returns_bool(false);
        Ref const child = find(children[static_cast<std::size_t>(index)]);
        if (!child.node || std::ranges::find(child.node->actions, "select") == child.node->actions.end())
            return returns_bool(false);
        if (child.doc)
            return returns_bool(tell_page(*child.doc, [id = child.id](a11y::Tree& tree) { tree.do_action(id, "select"); }));
        tell_window({ child.id, "select", {}, 0, 0 });
        return returns_bool(true);
    }
    if (call.member == "DeselectSelectedChild" || call.member == "DeselectChild" || call.member == "SelectAll" || call.member == "ClearSelection")
        return returns_bool(false);
    return std::nullopt;
}

namespace {

bool cell_role(Role role) { return role == Role::Cell || role == Role::GridCell || role == Role::ColumnHeader || role == Role::RowHeader; }
bool table_role(Role role) { return role == Role::Table || role == Role::Grid || role == Role::TreeGrid; }

}

std::optional<dbus::Value> AtspiBridge::table_property(std::string const& path, std::string_view property)
{
    Ref const ref = find(id_of_path(path).value_or(0));
    if (!ref.node)
        return std::nullopt;
    if (property == "version")
        return dbus::Value::uint32(1);
    if (property == "NRows")
        return dbus::Value::int32(ref.node->row_count);
    if (property == "NColumns")
        return dbus::Value::int32(ref.node->column_count);
    if (property == "NSelectedRows" || property == "NSelectedColumns")
        return dbus::Value::int32(0);
    if (property == "Caption") {
        for (NodeId const id : children_of(ref)) {
            Ref const child = find(id);
            if (child.node && child.node->role == Role::Caption)
                return reference_value(id);
        }
    }
    if (property == "Caption" || property == "Summary")
        return dbus::Value::structure({ dbus::Value::string(m_bus->unique_name()), dbus::Value::object_path(null_path) });
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::table(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    std::int32_t const columns = std::max(1, ref.node->column_count);
    // The table's cells, not those of a table inside it.
    std::vector<Ref> cells;
    {
        std::vector<NodeId> stack = children_of(ref);
        while (!stack.empty()) {
            Ref const walk = find(stack.back());
            stack.pop_back();
            if (!walk.node || table_role(walk.node->role))
                continue;
            if (cell_role(walk.node->role)) {
                cells.push_back(walk);
                continue;
            }
            for (NodeId const child : children_of(walk))
                stack.push_back(child);
        }
    }
    auto const cell_at = [&cells](std::int32_t row, std::int32_t column) -> Ref const* {
        for (Ref const& cell : cells) {
            NodeData const& n = *cell.node;
            if (row >= n.row_index && row < n.row_index + std::max(1, n.row_span) && column >= n.column_index
                && column < n.column_index + std::max(1, n.column_span))
                return &cell;
        }
        return nullptr;
    };
    dbus::Reader args(call.body);
    std::string const& member = call.member;
    std::int32_t const first = args.int32().value_or(-1);
    std::int32_t const second = (member == "GetAccessibleAt" || member == "GetIndexAt" || member == "GetRowExtentAt" || member == "GetColumnExtentAt" || member == "IsSelected")
        ? args.int32().value_or(-1)
        : 0;
    if (member == "GetAccessibleAt" || member == "GetRowHeader" || member == "GetColumnHeader") {
        Ref const* found = nullptr;
        if (member == "GetAccessibleAt") {
            found = cell_at(first, second);
        } else {
            Role const wanted = member == "GetRowHeader" ? Role::RowHeader : Role::ColumnHeader;
            for (Ref const& cell : cells) {
                if (cell.node->role == wanted && (wanted == Role::RowHeader ? cell.node->row_index == first : cell.node->column_index == first)) {
                    found = &cell;
                    break;
                }
            }
        }
        dbus::Reply reply = returns("(so)");
        if (found)
            write_reference(reply.body, found->id);
        else
            write_null(reply.body);
        return reply;
    }
    if (member == "GetIndexAt")
        return returns_int(first < 0 || second < 0 ? -1 : first * columns + second);
    if (member == "GetRowAtIndex")
        return returns_int(first < 0 ? -1 : first / columns);
    if (member == "GetColumnAtIndex")
        return returns_int(first < 0 ? -1 : first % columns);
    if (member == "GetRowDescription" || member == "GetColumnDescription") {
        bool const row = member == "GetRowDescription";
        for (Ref const& cell : cells) {
            if (cell.node->role == (row ? Role::RowHeader : Role::ColumnHeader) && (row ? cell.node->row_index : cell.node->column_index) == first)
                return returns_string(cell.node->name);
        }
        return returns_string("");
    }
    if (member == "GetRowExtentAt" || member == "GetColumnExtentAt") {
        Ref const* const cell = cell_at(first, second);
        return returns_int(cell ? (member == "GetRowExtentAt" ? cell->node->row_span : cell->node->column_span) : 0);
    }
    if (member == "GetSelectedRows" || member == "GetSelectedColumns") {
        dbus::Reply reply = returns("ai");
        reply.body.array(4, [] { });
        return reply;
    }
    if (member == "IsRowSelected" || member == "IsColumnSelected" || member == "IsSelected" || member == "AddRowSelection"
        || member == "AddColumnSelection" || member == "RemoveRowSelection" || member == "RemoveColumnSelection")
        return returns_bool(false);
    if (member == "GetRowColumnExtentsAtIndex") {
        dbus::Reply reply = returns("biiiib");
        Ref const* const cell = first >= 0 ? cell_at(first / columns, first % columns) : nullptr;
        reply.body.boolean(cell != nullptr);
        reply.body.int32(cell ? cell->node->row_index : -1);
        reply.body.int32(cell ? cell->node->column_index : -1);
        reply.body.int32(cell ? cell->node->row_span : 0);
        reply.body.int32(cell ? cell->node->column_span : 0);
        reply.body.boolean(false);
        return reply;
    }
    return std::nullopt;
}

std::optional<dbus::Value> AtspiBridge::table_cell_property(std::string const& path, std::string_view property)
{
    Ref const ref = find(id_of_path(path).value_or(0));
    if (!ref.node)
        return std::nullopt;
    if (property == "version")
        return dbus::Value::uint32(1);
    if (property == "ColumnSpan")
        return dbus::Value::int32(ref.node->column_span);
    if (property == "RowSpan")
        return dbus::Value::int32(ref.node->row_span);
    if (property == "Position")
        return dbus::Value::structure({ dbus::Value::int32(ref.node->row_index), dbus::Value::int32(ref.node->column_index) });
    if (property == "Table") {
        for (Ref walk = parent_of(ref); walk && walk.node; walk = parent_of(walk)) {
            if (table_role(walk.node->role))
                return reference_value(walk.id);
        }
        return dbus::Value::structure({ dbus::Value::string(m_bus->unique_name()), dbus::Value::object_path(null_path) });
    }
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::table_cell(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    if (call.member == "GetRowColumnSpan") {
        dbus::Reply reply = returns("biiii");
        reply.body.boolean(true);
        reply.body.int32(ref.node->row_index);
        reply.body.int32(ref.node->column_index);
        reply.body.int32(ref.node->row_span);
        reply.body.int32(ref.node->column_span);
        return reply;
    }
    if (call.member == "GetColumnHeaderCells" || call.member == "GetRowHeaderCells") {
        Role const wanted = call.member == "GetColumnHeaderCells" ? Role::ColumnHeader : Role::RowHeader;
        dbus::Reply reply = returns("a(so)");
        reply.body.array(8, [&] {
            for (NodeId const header : ref.node->header_cells) {
                Ref const cell = find(header);
                if (cell.node && cell.node->role == wanted)
                    write_reference(reply.body, header);
            }
        });
        return reply;
    }
    return std::nullopt;
}

std::optional<dbus::Value> AtspiBridge::hyperlink_property(std::string const& path, std::string_view property)
{
    Ref const ref = find(id_of_path(path).value_or(0));
    if (!ref.node)
        return std::nullopt;
    if (property == "version")
        return dbus::Value::uint32(1);
    if (property == "NAnchors")
        return dbus::Value::int16(1);
    if (property == "StartIndex" || property == "EndIndex") {
        // Where its embedded object character is in its parent's text.
        Ref const parent = parent_of(ref);
        atspi::FlatText const flat = flat_text(parent);
        for (atspi::FlatText::Piece const& piece : flat.pieces) {
            if (piece.object && piece.node == ref.id)
                return dbus::Value::int32(static_cast<std::int32_t>(property == "StartIndex" ? piece.start : piece.end));
        }
        return dbus::Value::int32(-1);
    }
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::hyperlink(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    dbus::Reader args(call.body);
    std::int32_t const index = args.int32().value_or(-1);
    if (call.member == "GetObject") {
        dbus::Reply reply = returns("(so)");
        if (index == 0)
            write_reference(reply.body, ref.id);
        else
            write_null(reply.body);
        return reply;
    }
    if (call.member == "GetURI")
        return returns_string(index == 0 ? ref.node->url : std::string());
    if (call.member == "IsValid")
        return returns_bool(true);
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::hypertext(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    atspi::FlatText const flat = flat_text(ref);
    std::vector<atspi::FlatText::Piece const*> const objects = flat.objects();
    dbus::Reader args(call.body);
    if (call.member == "GetNLinks")
        return returns_int(static_cast<std::int32_t>(objects.size()));
    std::int32_t const index = args.int32().value_or(-1);
    if (call.member == "GetLink") {
        dbus::Reply reply = returns("(so)");
        if (index >= 0 && static_cast<std::size_t>(index) < objects.size())
            write_reference(reply.body, objects[static_cast<std::size_t>(index)]->node);
        else
            write_null(reply.body);
        return reply;
    }
    if (call.member == "GetLinkIndex") {
        for (std::size_t i = 0; i < objects.size(); ++i) {
            if (index >= 0 && objects[i]->start == static_cast<std::size_t>(index))
                return returns_int(static_cast<std::int32_t>(i));
        }
        return returns_int(-1);
    }
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::image(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    dbus::Reader args(call.body);
    std::optional<a11y::Rect> const bounds = window_bounds(ref);
    a11y::Rect const rect = in_coordinates(bounds.value_or(a11y::Rect { -1, -1, -1, -1 }), ref, args.uint32().value_or(1));
    if (call.member == "GetImageExtents") {
        dbus::Reply reply = returns("(iiii)");
        reply.body.align(8);
        write_rect(reply.body, rect);
        return reply;
    }
    if (call.member == "GetImagePosition" || call.member == "GetImageSize") {
        dbus::Reply reply = returns("ii");
        bool const position = call.member == "GetImagePosition";
        reply.body.int32(static_cast<std::int32_t>(std::lround(position ? rect.x : rect.width)));
        reply.body.int32(static_cast<std::int32_t>(std::lround(position ? rect.y : rect.height)));
        return reply;
    }
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::document(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.doc)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    std::vector<std::pair<std::string, std::string>> const attributes = {
        { "DocURL", ref.doc->view.url }, { "MimeType", ref.doc->view.mime_type }, { "Title", ref.node->name } };
    if (call.member == "GetLocale")
        return returns_string(ref.node->language);
    if (call.member == "GetAttributes") {
        dbus::Reply reply = returns("a{ss}");
        write_string_map(reply.body, attributes);
        return reply;
    }
    if (call.member == "GetAttributeValue") {
        dbus::Reader args(call.body);
        std::string const name = args.string().value_or("");
        for (auto const& [key, value] : attributes) {
            if (key == name)
                return returns_string(value);
        }
        return returns_string("");
    }
    if (call.member == "GetTextSelections") {
        dbus::Reply reply = returns("a((so)i(so)ib)");
        reply.body.array(8, [] { });
        return reply;
    }
    if (call.member == "SetTextSelections")
        return returns_bool(false);
    return std::nullopt;
}

std::optional<dbus::Reply> AtspiBridge::collection(dbus::Message const& call)
{
    Ref const ref = find(id_of_path(call.path).value_or(0));
    if (!ref.node)
        return dbus::Reply::failure(std::string(dbus::error_unknown_object), "gone");
    if (call.member == "GetActiveDescendant") {
        dbus::Reply reply = returns("(so)");
        for (auto const& [relation, targets] : ref.node->relations) {
            if (relation == a11y::Relation::ActiveDescendant && !targets.empty() && find(targets.front())) {
                write_reference(reply.body, targets.front());
                return reply;
            }
        }
        write_null(reply.body);
        return reply;
    }
    bool const from = call.member == "GetMatchesFrom";
    bool const to = call.member == "GetMatchesTo";
    if (call.member != "GetMatches" && !from && !to)
        return std::nullopt;
    dbus::Reader args(call.body);
    std::optional<std::vector<dbus::Value>> const values = args.values(call.signature);
    if (!values)
        return dbus::Reply::failure(std::string(dbus::error_invalid_args), "the match rule does not parse");
    std::size_t at = 0;
    NodeId current = 0;
    if (from || to) {
        current = id_of_path((*values)[at].text).value_or(0);
        ++at;
    }
    if ((*values)[at].type != "(aiia{ss}iaiiasib)")
        return dbus::Reply::failure(std::string(dbus::error_invalid_args), "(aiia{ss}iaiiasib)");
    dbus::Value const& rule = (*values)[at++];
    std::uint32_t const sort = static_cast<std::uint32_t>((*values)[at++].number);
    std::uint32_t const tree = from || to ? static_cast<std::uint32_t>((*values)[at++].number) : 2;
    if (to)
        ++at; // limit_scope
    std::int32_t const count = static_cast<std::int32_t>((*values)[at++].as_int());
    bool const traverse = (*values)[at].number != 0;

    // The rule (AtspiMatchRule): states and roles as bit sets in words of
    // 32, attributes, interfaces, each with its AtspiCollectionMatchType
    // (1 all, 2 any, 3 none, 4 empty), and whether the answer is inverted.
    auto const bits = [](dbus::Value const& words) {
        std::vector<std::uint32_t> set;
        for (dbus::Value const& word : words.items)
            set.push_back(static_cast<std::uint32_t>(word.number));
        return set;
    };
    std::vector<std::uint32_t> const states = bits(rule.items[0]);
    std::int32_t const state_match = static_cast<std::int32_t>(rule.items[1].as_int());
    dbus::Value const& wanted_attributes = rule.items[2];
    std::int32_t const attribute_match = static_cast<std::int32_t>(rule.items[3].as_int());
    std::vector<std::uint32_t> const roles = bits(rule.items[4]);
    std::int32_t const role_match = static_cast<std::int32_t>(rule.items[5].as_int());
    dbus::Value const& wanted_interfaces = rule.items[6];
    std::int32_t const interface_match = static_cast<std::int32_t>(rule.items[7].as_int());
    bool const invert = rule.items[8].number != 0;
    auto const in_set = [](std::vector<std::uint32_t> const& set, std::uint32_t bit) {
        return bit / 32 < set.size() && (set[bit / 32] >> (bit % 32) & 1) != 0;
    };
    auto const set_empty = [](std::vector<std::uint32_t> const& set) {
        return std::ranges::all_of(set, [](std::uint32_t word) { return word == 0; });
    };
    auto const matches = [&](Ref const& candidate) {
        // States: every wanted one, any, none.
        atspi::StateSet const have = states_of(candidate);
        bool all_states = true;
        bool any_state = false;
        for (unsigned s = 0; s < 64; ++s) {
            if (!in_set(states, s))
                continue;
            bool const set = (have >> s & 1) != 0;
            all_states = all_states && set;
            any_state = any_state || set;
        }
        bool const no_states = set_empty(states);
        bool const states_fit = state_match == 1 ? all_states : state_match == 2 ? (no_states || any_state) : state_match == 3 ? !any_state : true;
        // Roles: the one it has, among those wanted.
        std::uint32_t const role = role_of(candidate);
        bool const no_roles = set_empty(roles);
        bool const role_in = in_set(roles, role);
        bool const roles_fit = role_match == 1 ? (no_roles || role_in) : role_match == 2 ? (no_roles || role_in) : role_match == 3 ? !role_in : true;
        // Attributes, by name and value.
        std::vector<std::pair<std::string, std::string>> const attributes = candidate.node ? atspi::attributes_of(*candidate.node)
                                                                                           : std::vector<std::pair<std::string, std::string>> {};
        std::size_t attributes_held = 0;
        for (dbus::Value const& entry : wanted_attributes.items) {
            for (auto const& [name, value] : attributes) {
                if (name == entry.items[0].text && value == entry.items[1].text)
                    ++attributes_held;
            }
        }
        std::size_t const attributes_wanted = wanted_attributes.items.size();
        bool const attributes_fit = attributes_wanted == 0 || attribute_match == 0 || attribute_match == 4
            || (attribute_match == 1 ? attributes_held == attributes_wanted : attribute_match == 2 ? attributes_held > 0 : attributes_held == 0);
        // Interfaces, by name with or without the prefix.
        std::vector<std::string> const offered = interfaces_of(candidate);
        std::size_t interfaces_held = 0;
        for (dbus::Value const& name : wanted_interfaces.items) {
            std::string const short_name = folded(name.text.starts_with(interface_prefix) ? name.text.substr(interface_prefix.size()) : name.text);
            for (std::string const& offer : offered) {
                if (folded(std::string_view(offer).substr(interface_prefix.size())) == short_name)
                    ++interfaces_held;
            }
        }
        std::size_t const interfaces_wanted = wanted_interfaces.items.size();
        bool const interfaces_fit = interfaces_wanted == 0 || interface_match == 0 || interface_match == 4
            || (interface_match == 1 ? interfaces_held == interfaces_wanted : interface_match == 2 ? interfaces_held > 0 : interfaces_held == 0);
        bool const fit = states_fit && roles_fit && attributes_fit && interfaces_fit;
        return invert ? !fit : fit;
    };
    // The candidates, in document (canonical) order.
    std::vector<NodeId> candidates;
    if (from || to) {
        std::vector<NodeId> const all = descendants(ref, true);
        auto const here = std::ranges::find(all, current);
        Ref const current_ref = find(current);
        if (tree == 0 && from) { // restrict to the current object's children
            candidates = children_of(current_ref);
        } else if (tree == 1) { // its siblings, after it (from) or before it (to)
            std::vector<NodeId> const siblings = children_of(parent_of(current_ref));
            auto const self = std::ranges::find(siblings, current);
            if (self != siblings.end()) {
                if (from)
                    candidates.assign(self + 1, siblings.end());
                else
                    candidates.assign(siblings.begin(), self);
            }
        } else if (here != all.end()) { // in order: everything after it, or before it
            if (from)
                candidates.assign(here + 1, all.end());
            else
                candidates.assign(all.begin(), here);
        }
    } else {
        candidates = descendants(ref, true);
    }
    // libatspi documents `traverse` as not supported: every descendant is
    // a candidate either way.
    (void)traverse;
    // AtspiCollectionSortOrder 4 and 5: the reverse.
    if (sort == 4 || sort == 5 || (to && sort != 1))
        std::ranges::reverse(candidates);
    dbus::Reply reply = returns("a(so)");
    reply.body.array(8, [&] {
        std::int32_t taken = 0;
        for (NodeId const id : candidates) {
            if (count > 0 && taken >= count)
                break;
            Ref const candidate = find(id);
            if (!candidate || !matches(candidate))
                continue;
            write_reference(reply.body, id);
            ++taken;
        }
    });
    return reply;
}

void AtspiBridge::write_cache_item(dbus::Writer& out, Ref const& ref) const
{
    NodeId const id = ref.app ? app_id : ref.id;
    Ref const parent = parent_of(ref);
    out.align(8);
    write_reference(out, id);
    write_reference(out, app_id);
    if (ref.app)
        out.reference(m_desktop_name.empty() ? m_bus->unique_name() : m_desktop_name, m_desktop_name.empty() ? std::string(null_path) : m_desktop_path);
    else if (parent)
        write_reference(out, parent.app ? app_id : parent.id);
    else
        write_null(out);
    out.int32(index_in_parent(ref));
    out.int32(static_cast<std::int32_t>(children_of(ref).size()));
    std::vector<std::string> const interfaces = interfaces_of(ref);
    out.array(4, [&] {
        for (std::string const& name : interfaces)
            out.string(name);
    });
    out.string(name_of(ref));
    out.uint32(role_of(ref));
    out.string(ref.node ? ref.node->description : std::string());
    atspi::StateSet const states = states_of(ref);
    out.array(4, [&] {
        out.uint32(static_cast<std::uint32_t>(states));
        out.uint32(static_cast<std::uint32_t>(states >> 32));
    });
}

std::optional<dbus::Reply> AtspiBridge::cache(dbus::Message const& call)
{
    if (call.member != "GetItems")
        return std::nullopt;
    m_cache_used = true;
    dbus::Reply reply = returns("a((so)(so)(so)iiassusau)");
    Ref app;
    app.app = true;
    app.id = app_id;
    std::vector<NodeId> all = descendants(app, true);
    reply.body.array(8, [&] {
        write_cache_item(reply.body, app);
        for (NodeId const id : all)
            write_cache_item(reply.body, find(id));
    });
    return reply;
}

}

std::unique_ptr<Accessibility> open_accessibility(std::string const& application_name, std::function<void()> wake_window)
{
    // NO_AT_BRIDGE=1 is how a desktop session turns every toolkit's bridge off.
    if (char const* const off = std::getenv("NO_AT_BRIDGE"); off && std::string_view(off) == "1")
        return nullptr;
    return std::make_unique<AtspiBridge>(application_name, std::move(wake_window));
}

}
