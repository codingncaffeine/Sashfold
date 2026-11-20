#include "Test.h"
#include "a11y/PageSources.h"
#include "a11y/Tree.h"
#include "css/StyleResolver.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Layout.h"
#include "platform/Accessibility.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include "platform/linux/DBus.h"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// The AT-SPI2 bridge (platform/linux/AccessibilityAtspi.cpp) over a real
// dbus-daemon this test starts on an abstract socket of its own: this test
// plays the registry (org.a11y.atspi.Socket.Embed, GetRegisteredEvents, the
// EventListenerRegistered signal) on one connection and an assistive tool on
// another, and reads a page's tree back through the bridge's exported
// interfaces with the engine's own D-Bus client. What it proves: the
// registration, the tree's shape and the interfaces' answers, actions
// reaching the page's thread, that no event is sent while no tool listens
// and the listened one is once one does, that a page which does not answer
// delays only the question that needed it (never the page's own thread),
// and that a document that goes is gone from the tree.

using namespace sashfold;

#if defined(__linux__)

namespace dbus = sashfold::platform::dbus;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document;
    css::StyleMap styles;
    layout::LayoutResult layout;
    std::unique_ptr<a11y::StyleMapSource> source;
    explicit Page(std::string_view html)
    {
        document = html::parse_document(html);
        styles = css::resolve_styles(*document);
        layout = layout::layout_document(*document, styles, 800);
        source = std::make_unique<a11y::StyleMapSource>(styles, &layout.root);
    }
    void refresh()
    {
        styles = css::resolve_styles(*document);
        layout = layout::layout_document(*document, styles, 800);
        source->set(styles, &layout.root);
    }
    dom::Element* by_id(std::string_view id) const
    {
        dom::Element* found = nullptr;
        dom::for_each_shadow_including(static_cast<dom::Node&>(*document), [&](dom::Node& node) {
            if (!found && node.is_element()) {
                if (dom::Attr const* attr = static_cast<dom::Element&>(node).find_attribute("id"); attr && attr->value == id)
                    found = &static_cast<dom::Element&>(node);
            }
        });
        return found;
    }
};

struct Sink final : a11y::ActionSink {
    std::vector<std::string> done;
    void click(dom::Element& element) override { done.push_back("click:" + element.local_name()); }
    void focus(dom::Element& element) override { done.push_back("focus:" + element.local_name()); }
};

// A dbus-daemon of this test's own (no service activated, nothing of the
// desktop's reached), the one process it starts and stops.
struct OwnBus {
    pid_t pid = -1;
    bool reachable = false;
    std::string address;
    OwnBus()
    {
        std::error_code error;
        std::string const folder = std::filesystem::absolute("atspi-scratch").string();
        std::filesystem::remove_all(folder, error);
        std::filesystem::create_directories(folder, error);
        address = "unix:abstract=sashfold-test-atspi-" + std::to_string(getpid());
        std::string const config = folder + "/bus.conf";
        FILE* const file = std::fopen(config.c_str(), "w");
        if (!file)
            return;
        std::fputs("<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                   " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                   "<busconfig><type>session</type>",
            file);
        std::fputs(("<listen>" + address + "</listen>").c_str(), file);
        std::fputs("<auth>EXTERNAL</auth><policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/>"
                   "<allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>\n",
            file);
        std::fclose(file);
        pid = fork();
        if (pid == 0) {
            std::string const config_arg = "--config-file=" + config;
            execl("/usr/bin/dbus-daemon", "dbus-daemon", config_arg.c_str(), "--nofork", "--nopidfile", static_cast<char*>(nullptr));
            _exit(127);
        }
        std::optional<dbus::SocketAddress> const where = dbus::parse_address(address);
        for (int waited = 0; waited < 500 && where && !reachable; ++waited) {
            reachable = dbus::Connection::open(*where) != nullptr;
            if (!reachable)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~OwnBus()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
        }
        std::error_code error;
        std::filesystem::remove_all("atspi-scratch", error);
    }
    bool up() const { return pid > 0 && reachable; }
};

constexpr char const* root_path = "/org/a11y/atspi/accessible/root";

std::string path_of(a11y::NodeId id) { return "/org/a11y/atspi/accessible/" + std::to_string(id); }

// The paths of an a(so) or (so) answer.
std::vector<std::string> paths_in(dbus::Message const& reply, std::string_view signature)
{
    std::vector<std::string> paths;
    dbus::Reader body(reply.body);
    std::optional<dbus::Value> const value = body.value(signature);
    if (!value)
        return paths;
    if (signature == "(so)")
        return { value->items.size() == 2 ? value->items[1].text : std::string() };
    for (dbus::Value const& item : value->items) {
        if (item.items.size() == 2)
            paths.push_back(item.items[1].text);
    }
    return paths;
}

void test_bridge_over_a_real_bus()
{
    if (access("/usr/bin/dbus-daemon", X_OK) != 0) {
        std::cerr << "test_atspi: no dbus-daemon here; the bridge's round trip is not run\n";
        return;
    }
    OwnBus bus;
    CHECK(bus.up());
    if (!bus.up())
        return;
    std::optional<dbus::SocketAddress> const where = dbus::parse_address(bus.address);

    // The registry: Embed answers with the desktop's root; no listener yet.
    std::unique_ptr<dbus::Connection> registry_connection = dbus::Connection::open(*where);
    CHECK(registry_connection != nullptr);
    if (!registry_connection)
        return;
    dbus::Service registry(*registry_connection);
    CHECK(registry.request_name("org.a11y.atspi.Registry"));
    std::string embedded_name;
    std::string embedded_path;
    auto socket = std::make_shared<dbus::Interface>();
    socket->name = "org.a11y.atspi.Socket";
    socket->introspection = "  <interface name=\"org.a11y.atspi.Socket\"><method name=\"Embed\"><arg type=\"(so)\" direction=\"in\"/>"
                            "<arg type=\"(so)\" direction=\"out\"/></method></interface>\n";
    socket->call = [&](dbus::Message const& call) -> std::optional<dbus::Reply> {
        if (call.member != "Embed")
            return std::nullopt;
        dbus::Reader args(call.body);
        if (std::optional<dbus::Value> const plug = args.value("(so)"); plug && plug->items.size() == 2) {
            embedded_name = plug->items[0].text;
            embedded_path = plug->items[1].text;
        }
        dbus::Reply reply;
        reply.signature = "(so)";
        reply.body.reference(registry_connection->unique_name(), root_path);
        return reply;
    };
    auto events = std::make_shared<dbus::Interface>();
    events->name = "org.a11y.atspi.Registry";
    events->introspection = "  <interface name=\"org.a11y.atspi.Registry\"><method name=\"GetRegisteredEvents\">"
                            "<arg type=\"a(ss)\" direction=\"out\"/></method></interface>\n";
    events->call = [](dbus::Message const& call) -> std::optional<dbus::Reply> {
        if (call.member != "GetRegisteredEvents")
            return std::nullopt;
        dbus::Reply reply;
        reply.signature = "a(ss)";
        reply.body.array(8, [] { });
        return reply;
    };
    registry.add_object(root_path, { socket });
    registry.add_object("/org/a11y/atspi/registry", { events });

    // A page, its tree, and the end the bridge reads it through.
    Page page("<!doctype html><html lang=\"en\"><title>Bridge test</title><h1 id=\"h\">Hello</h1><button id=\"b\">Go</button>"
              "<input id=\"f\" aria-label=\"Field\" value=\"ab\"></html>");
    a11y::Tree tree(*page.document, page.source.get());
    Sink sink;
    tree.set_action_sink(&sink);
    std::atomic<int> page_wakes { 0 };
    tree.set_page_wake([&page_wakes] { ++page_wakes; });
    auto link = std::make_shared<platform::AccessibleDocument>();
    // Its events wake the bridge's thread as a page's do in the shell.
    link->bridge = std::make_shared<a11y::Bridge>(tree, [] { platform::wake_accessibility(); });
    platform::AccessibleDocument::View view;
    view.shown = true;
    view.url = "https://example.test/page";
    view.title = "Bridge test";
    link->set_view(view);
    tree.update();

    // The bridge, on this bus; the registry answers it while it embeds.
    unsetenv("NO_AT_BRIDGE");
    setenv("AT_SPI_BUS_ADDRESS", bus.address.c_str(), 1);
    std::atomic<int> window_wakes { 0 };
    std::unique_ptr<platform::Accessibility> bridge = platform::open_accessibility("Bridge test", [&window_wakes] { ++window_wakes; });
    CHECK(bridge != nullptr);
    if (!bridge)
        return;
    platform::set_accessibility(bridge.get());
    // Out of reach before the bridge goes, however the test leaves.
    struct Unset {
        ~Unset() { platform::set_accessibility(nullptr); }
    } const unset;
    auto const pump = [&registry](int ms) {
        auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            if (std::optional<dbus::Message> const message = registry.next(20))
                registry.dispatch(*message);
        }
    };
    for (int waited = 0; waited < 100 && embedded_name.empty(); ++waited)
        pump(30);
    pump(100); // GetRegisteredEvents after Embed
    CHECK_EQ(embedded_path, std::string(root_path));
    CHECK(!embedded_name.empty());
    // Nobody but the registry has asked anything: no tree is wanted.
    CHECK(!bridge->wanted());

    // The window: a frame with a content node the document hangs under.
    a11y::NodeId const frame_id = a11y::NodeId { 1 } << 62;
    a11y::NodeId const content_id = frame_id + 1;
    {
        auto chrome = std::make_shared<a11y::Snapshot>();
        auto frame = std::make_shared<a11y::NodeData>();
        frame->id = frame_id;
        frame->role = a11y::Role::Window;
        frame->children = { content_id };
        frame->bounds = a11y::Rect { 0, 0, 800, 600 };
        auto content = std::make_shared<a11y::NodeData>();
        content->id = content_id;
        content->parent = frame_id;
        content->role = a11y::Role::Group;
        content->bounds = a11y::Rect { 0, 40, 800, 560 };
        chrome->root = frame_id;
        chrome->nodes.emplace(frame_id, frame);
        chrome->nodes.emplace(content_id, content);
        platform::WindowState state;
        state.chrome = chrome;
        state.content = content_id;
        state.title = "Bridge test - Window";
        state.width = 800;
        state.height = 600;
        state.content_x = 0;
        state.content_y = 40;
        state.content_width = 800;
        state.content_height = 560;
        bridge->set_window(std::move(state));
    }
    bridge->add_document(link);

    // The tool, on a connection of its own.
    std::unique_ptr<dbus::Connection> tool_connection = dbus::Connection::open(*where);
    CHECK(tool_connection != nullptr);
    if (!tool_connection)
        return;
    dbus::Service tool(*tool_connection);
    CHECK(tool.add_match("type='signal',interface='org.a11y.atspi.Event.Object'"));
    auto const ask = [&](std::string const& path, std::string const& interface, std::string const& member, std::string signature = {},
                         dbus::Writer body = {}) {
        return tool.call(dbus::Service::method_call(embedded_name, path, interface, member, std::move(signature), std::move(body)), 3000);
    };
    auto const children = [&](std::string const& path) {
        std::optional<dbus::Message> const reply = ask(path, "org.a11y.atspi.Accessible", "GetChildren");
        return reply && reply->type == dbus::MessageType::MethodReturn ? paths_in(*reply, "a(so)") : std::vector<std::string> {};
    };
    auto const role = [&](std::string const& path) -> std::int64_t {
        std::optional<dbus::Message> const reply = ask(path, "org.a11y.atspi.Accessible", "GetRole");
        if (!reply || reply->type != dbus::MessageType::MethodReturn)
            return -1;
        dbus::Reader body(reply->body);
        return body.uint32().value_or(0);
    };

    // The shape: application → frame → content → the document.
    std::vector<std::string> const top = children(root_path);
    CHECK_EQ(top.size(), std::size_t { 1 });
    CHECK_EQ(top.empty() ? std::string() : top[0], path_of(frame_id));
    CHECK(bridge->wanted()); // a tool asked
    CHECK_EQ(role(root_path), std::int64_t { 75 }); // application
    CHECK_EQ(role(path_of(frame_id)), std::int64_t { 23 }); // frame
    std::vector<std::string> const documents = children(path_of(content_id));
    CHECK_EQ(documents.size(), std::size_t { 1 });
    std::string const document_path = path_of(tree.root());
    CHECK_EQ(documents.empty() ? std::string() : documents[0], document_path);
    CHECK_EQ(role(document_path), std::int64_t { 95 }); // document web
    std::vector<std::string> const nodes = children(document_path);
    CHECK_EQ(nodes.size(), std::size_t { 3 });
    if (nodes.size() != 3)
        return;
    CHECK_EQ(role(nodes[0]), std::int64_t { 83 }); // heading
    CHECK_EQ(role(nodes[1]), std::int64_t { 43 }); // push button
    CHECK_EQ(role(nodes[2]), std::int64_t { 79 }); // entry

    // Its name, a property.
    {
        dbus::Writer body;
        body.string("org.a11y.atspi.Accessible");
        body.string("Name");
        std::optional<dbus::Message> const reply = ask(nodes[1], "org.freedesktop.DBus.Properties", "Get", "ss", std::move(body));
        CHECK(reply && reply->type == dbus::MessageType::MethodReturn);
        if (reply) {
            dbus::Reader read(reply->body);
            std::optional<dbus::Value> const value = read.value("v");
            CHECK(value && !value->items.empty() && value->items[0].text == "Go");
        }
    }
    // The heading's text; the field's.
    auto const text_of = [&](std::string const& path) {
        dbus::Writer body;
        body.int32(0);
        body.int32(-1);
        std::optional<dbus::Message> const reply = ask(path, "org.a11y.atspi.Text", "GetText", "ii", std::move(body));
        if (!reply || reply->type != dbus::MessageType::MethodReturn)
            return std::string("<no answer>");
        dbus::Reader read(reply->body);
        return read.string().value_or("<unread>");
    };
    CHECK_EQ(text_of(nodes[0]), std::string("Hello"));
    CHECK_EQ(text_of(nodes[2]), std::string("ab"));
    // Its box in window coordinates: the page's, below the content's top.
    {
        dbus::Writer body;
        body.uint32(1);
        std::optional<dbus::Message> const reply = ask(nodes[1], "org.a11y.atspi.Component", "GetExtents", "u", std::move(body));
        CHECK(reply && reply->type == dbus::MessageType::MethodReturn);
        a11y::NodeData const* const button = tree.node(tree.id_of(*page.by_id("b")));
        if (reply && button && button->bounds) {
            dbus::Reader read(reply->body);
            std::optional<dbus::Value> const box = read.value("(iiii)");
            CHECK(box && box->items.size() == 4);
            if (box && box->items.size() == 4) {
                CHECK_EQ(static_cast<std::int32_t>(box->items[0].number), static_cast<std::int32_t>(std::lround(button->bounds->x)));
                CHECK_EQ(static_cast<std::int32_t>(box->items[1].number), static_cast<std::int32_t>(std::lround(button->bounds->y + 40)));
            }
        }
    }

    // An action reaches the page's thread: posted (the page woken), done
    // when the page serves.
    {
        int const wakes = page_wakes.load();
        dbus::Writer body;
        body.int32(0);
        std::optional<dbus::Message> const reply = ask(nodes[1], "org.a11y.atspi.Action", "DoAction", "i", std::move(body));
        CHECK(reply && reply->type == dbus::MessageType::MethodReturn);
        CHECK(page_wakes.load() > wakes);
        CHECK(sink.done.empty()); // not before the page's turn
        tree.serve();
        CHECK_EQ(sink.done.size(), std::size_t { 1 });
        CHECK_EQ(sink.done.empty() ? std::string() : sink.done[0], std::string("click:button"));
    }

    // Events: none while nobody listens, the one listened for once a tool
    // listens.
    auto const text_events = [&](int ms) {
        int count = 0;
        auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            std::optional<dbus::Message> const message = tool.next(20);
            if (message && message->type == dbus::MessageType::Signal && message->interface == "org.a11y.atspi.Event.Object"
                && message->member == "TextChanged" && message->path == nodes[0])
                ++count;
        }
        return count;
    };
    auto const retitle = [&](std::string const& words) {
        dom::Element* const heading = page.by_id("h");
        for (dom::Node* child : heading->children()) {
            if (child->is_text()) {
                static_cast<dom::Text*>(child)->data = words;
                static_cast<dom::Text*>(child)->mark_style_data();
            }
        }
        page.refresh();
        tree.update();
    };
    retitle("Changed");
    CHECK_EQ(text_events(400), 0);
    {
        dbus::Writer body;
        body.string(tool_connection->unique_name());
        body.string("object:text-changed");
        registry.emit("/org/a11y/atspi/registry", "org.a11y.atspi.Registry", "EventListenerRegistered", "ss", std::move(body));
    }
    pump(200);
    retitle("Changed again");
    int const heard = text_events(800);
    CHECK(heard >= 1); // the text deleted and inserted
    CHECK(heard <= 2);

    // A page that does not answer delays only the question that needed it:
    // the bridge gives up after its bound, and the page's own thread (this
    // one) is never held meanwhile.
    {
        std::atomic<bool> answered { false };
        std::chrono::milliseconds took { 0 };
        std::thread asker([&] {
            auto const began = std::chrono::steady_clock::now();
            dbus::Writer body;
            body.int32(0);
            body.uint32(1);
            std::optional<dbus::Message> const reply = ask(nodes[0], "org.a11y.atspi.Text", "GetCharacterExtents", "iu", std::move(body));
            took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
            answered = reply.has_value();
        });
        auto longest = std::chrono::steady_clock::duration::zero();
        auto const until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        while (std::chrono::steady_clock::now() < until) {
            auto const began = std::chrono::steady_clock::now();
            tree.update(); // the page's turn, without serving the bridge
            longest = std::max(longest, std::chrono::steady_clock::now() - began);
        }
        asker.join();
        CHECK(answered.load());
        CHECK(took.count() < 2000);
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(longest).count() < 50);
        tree.serve(); // the late query runs and its answer goes nowhere
    }

    // The document goes: the bridge lets go of the tree, and the tool sees
    // it gone.
    link->bridge->detach();
    bridge->remove_document(link);
    pump(100);
    CHECK_EQ(children(path_of(content_id)).size(), std::size_t { 0 });
    {
        std::optional<dbus::Message> const reply = ask(document_path, "org.a11y.atspi.Accessible", "GetRole");
        CHECK(reply && reply->type == dbus::MessageType::Error);
    }
    platform::set_accessibility(nullptr);
    bridge.reset();
    unsetenv("AT_SPI_BUS_ADDRESS");
}

// NO_AT_BRIDGE=1 is a desktop's word that no bridge is wanted; with no
// accessibility bus reachable the bridge is quietly nothing.
void test_turned_off()
{
    setenv("NO_AT_BRIDGE", "1", 1);
    CHECK(platform::open_accessibility("Bridge test", {}) == nullptr);
    unsetenv("NO_AT_BRIDGE");
    setenv("AT_SPI_BUS_ADDRESS", "unix:abstract=sashfold-test-atspi-nobody-here", 1);
    {
        std::unique_ptr<platform::Accessibility> const bridge = platform::open_accessibility("Bridge test", {});
        CHECK(bridge != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(bridge && !bridge->wanted());
    }
    unsetenv("AT_SPI_BUS_ADDRESS");
}

// What a page's turn pays for the bridge while no tool is there: the one
// question it asks (accessibility_wanted), with no bridge set and with one
// set that no tool has asked anything of. Said in ns per turn; no tree is
// made either way (the shell makes one only once this says yes).
void test_cost_when_off()
{
    auto const per_call = [] {
        constexpr int calls = 1'000'000;
        int yes = 0;
        auto const began = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; ++i)
            yes += platform::accessibility_wanted() ? 1 : 0;
        double const ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - began).count() / calls;
        CHECK_EQ(yes, 0);
        return ns;
    };
    double const without = per_call();
    setenv("AT_SPI_BUS_ADDRESS", "unix:abstract=sashfold-test-atspi-nobody-here", 1);
    double with = 0;
    {
        std::unique_ptr<platform::Accessibility> const bridge = platform::open_accessibility("Bridge test", {});
        CHECK(bridge != nullptr);
        platform::set_accessibility(bridge.get());
        with = per_call();
        platform::set_accessibility(nullptr);
    }
    unsetenv("AT_SPI_BUS_ADDRESS");
    std::cout << "test_atspi: a page turn's question with no bridge " << without << " ns, with a bridge no tool uses " << with << " ns\n";
    CHECK(without < 100);
    CHECK(with < 1000);
}

}

#endif

int main()
{
#if defined(__linux__)
    test_turned_off();
    test_cost_when_off();
    test_bridge_over_a_real_bus();
#endif
    // With no bridge set, the page threads' question costs one atomic read
    // and is no.
    CHECK(!platform::accessibility_wanted());
    return sashfold::test::report("atspi");
}
