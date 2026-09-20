#include "Test.h"

#include "net/Url.h"
#include "platform/Net.h"
#include "ui/Browser.h"
#include "ui/ShellLoader.h"
#include "ui/Theme.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A page of the web loaded without the window being held still: the document
// is fetched on another thread, the tab showing what it showed meanwhile; the
// stylesheets and scripts its markup names are fetched together and waited
// for a tick at a time, not by the parser; and a load on its way gives way to
// the next one asked for, and to Back.

using namespace sashfold;

namespace {

struct Served {
    std::string type;
    std::string body;
    int delay_ms = 0;
};

std::optional<std::string> read_request(platform::TcpSocket& client)
{
    std::string head;
    std::uint8_t buffer[4096];
    while (head.find("\r\n\r\n") == std::string::npos) {
        std::ptrdiff_t const received = client.receive(buffer, sizeof buffer);
        if (received <= 0)
            return std::nullopt;
        head.append(reinterpret_cast<char const*>(buffer), static_cast<std::size_t>(received));
    }
    return head;
}

// A loopback server of a few pages and their parts, each answered after its
// own delay on a thread of its own; it remembers every path asked for.
class SiteServer {
public:
    explicit SiteServer(std::map<std::string, Served> site)
        : m_listener(*platform::TcpListener::listen_loopback())
        , m_site(std::move(site))
        , m_thread([this] { run(); })
    {
    }

    ~SiteServer()
    {
        m_stop = true;
        if (auto poke = platform::TcpSocket::connect("127.0.0.1", m_listener.port()))
            poke->close();
        m_thread.join();
        for (std::thread& handler : m_handlers)
            handler.join();
    }

    net::Url url(std::string const& path) const
    {
        return *net::parse_url("http://127.0.0.1:" + std::to_string(m_listener.port()) + path);
    }

    std::size_t asked(std::string const& path) const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        std::size_t count = 0;
        for (std::string const& one : m_asked)
            count += one == path ? 1 : 0;
        return count;
    }

private:
    void run()
    {
        for (;;) {
            auto client = m_listener.accept();
            if (!client || m_stop)
                return;
            auto held = std::make_shared<platform::TcpSocket>(std::move(*client));
            m_handlers.emplace_back([this, held] { serve(*held); });
        }
    }

    void serve(platform::TcpSocket& client)
    {
        std::optional<std::string> const head = read_request(client);
        if (!head)
            return;
        std::string const path = head->substr(4, head->find(' ', 4) - 4);
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            m_asked.push_back(path);
        }
        auto const found = m_site.find(path);
        std::string response;
        if (found == m_site.end()) {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(found->second.delay_ms));
            response = "HTTP/1.1 200 OK\r\nContent-Type: " + found->second.type + "\r\nCache-Control: no-store\r\nContent-Length: "
                + std::to_string(found->second.body.size()) + "\r\nConnection: close\r\n\r\n" + found->second.body;
        }
        (void)client.send_all(reinterpret_cast<std::uint8_t const*>(response.data()), response.size());
        client.close();
    }

    platform::TcpListener m_listener;
    std::map<std::string, Served> m_site;
    std::atomic<bool> m_stop = false;
    mutable std::mutex m_mutex;
    std::vector<std::string> m_asked;
    std::vector<std::thread> m_handlers;
    std::thread m_thread; // last: everything above is ready when it starts
};

double ms_since(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}

void load_fully(ui::Browser& browser)
{
    auto const started = std::chrono::steady_clock::now();
    while (browser.has_pending_load() && ms_since(started) < 10000)
        browser.tick();
}

std::map<std::string, Served> site()
{
    return {
        { "/first", { "text/html", "<!doctype html><title>First</title><p>one", 0 } },
        { "/second", { "text/html", "<!doctype html><title>Second</title><p>two", 0 } },
        { "/slow", { "text/html", "<!doctype html><title>Slow</title><p>slow", 400 } },
        { "/page", { "text/html",
                       "<!doctype html><title>Arrived</title><link rel=stylesheet href=/look.css><script src=/does.js></script><p>page",
                       150 } },
        { "/look.css", { "text/css", "p { color: green }", 400 } },
        { "/does.js", { "text/javascript", "document.title = 'Scripted';", 400 } },
    };
}

}

int main()
{
    // The document comes on another thread: a tick while it is on its way
    // takes a moment, not the fetch, there is no step to do NOW, and the tab
    // shows what it showed. What the markup names is fetched together, each
    // once, and the page is there when the slowest of it is — not their sum.
    {
        SiteServer server(site());
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        browser.open(server.url("/first"));
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("First"));

        auto const started = std::chrono::steady_clock::now();
        browser.open(server.url("/page"));
        CHECK(browser.has_pending_load());
        browser.tick(); // the load is begun
        auto const one_tick = std::chrono::steady_clock::now();
        browser.tick();
        CHECK(ms_since(one_tick) < 100);
        CHECK(browser.has_pending_load());
        CHECK(!browser.load_ready());
        CHECK_EQ(browser.page_title(), std::string("First"));
        load_fully(browser);
        double const took = ms_since(started);
        CHECK_EQ(browser.page_title(), std::string("Scripted"));
        if (took < 540 || took >= 900)
            std::cerr << "the page and its two parts took " << took << " ms\n";
        CHECK(took >= 540); // the document's 150, then the two together at 400
        CHECK(took < 900); // and not 150 + 400 + 400
        CHECK_EQ(server.asked("/page"), std::size_t { 1 });
        CHECK_EQ(server.asked("/look.css"), std::size_t { 1 });
        CHECK_EQ(server.asked("/does.js"), std::size_t { 1 });
        CHECK(!browser.has_pending_load());
    }

    // A load on its way gives way to the next one asked for: the first never
    // shows, however long it is waited for.
    {
        SiteServer server(site());
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        browser.open(server.url("/first"));
        load_fully(browser);
        browser.open(server.url("/slow"));
        browser.tick();
        browser.open(server.url("/second"));
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("Second"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("Second"));
        // Back is First: the load that was let go of left nothing in the history.
        browser.back();
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("First"));
    }

    // And to Back: the page gone back to stays, the load that was on its way
    // for the page left behind never lands on it.
    {
        SiteServer server(site());
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        browser.open(server.url("/first"));
        load_fully(browser);
        browser.open(server.url("/second"));
        load_fully(browser);
        browser.open(server.url("/slow"));
        browser.tick();
        browser.back();
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("First"));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("First"));
    }

    // A page that is not there is said to be so, as it always was.
    {
        SiteServer server(site());
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        browser.open(server.url("/nowhere"));
        load_fully(browser);
        CHECK(browser.status_text().find("404") != std::string::npos);
    }

    return test::report("navigation");
}
