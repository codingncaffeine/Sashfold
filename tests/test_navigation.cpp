#include "Test.h"

#include "net/Url.h"
#include "platform/Net.h"
#include "text/FontManager.h"
#include "text/SashfoldMono.h"
#include "ui/Browser.h"
#include "ui/ShellLoader.h"
#include "ui/Theme.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

// A request's head and, after the blank line, as much body as the head's
// Content-Length says there is.
std::optional<std::string> read_request(platform::TcpSocket& client)
{
    std::string request;
    std::uint8_t buffer[4096];
    auto const more = [&] {
        std::ptrdiff_t const received = client.receive(buffer, sizeof buffer);
        if (received <= 0)
            return false;
        request.append(reinterpret_cast<char const*>(buffer), static_cast<std::size_t>(received));
        return true;
    };
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (!more())
            return std::nullopt;
    }
    std::size_t const head_end = request.find("\r\n\r\n") + 4;
    std::string lowered = request.substr(0, head_end);
    for (char& c : lowered)
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    std::size_t length = 0;
    if (std::size_t const at = lowered.find("content-length:"); at != std::string::npos)
        length = static_cast<std::size_t>(std::atoi(lowered.c_str() + at + 15));
    while (request.size() < head_end + length) {
        if (!more())
            return std::nullopt;
    }
    return request;
}

// One header's value out of a request, by its name in lowercase.
std::string header_of(std::string const& request, std::string const& name)
{
    std::size_t const head_end = request.find("\r\n\r\n");
    std::string lowered = request.substr(0, head_end);
    for (char& c : lowered)
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    std::size_t const at = lowered.find("\r\n" + name + ":");
    if (at == std::string::npos)
        return "(none)";
    std::size_t const from = at + 2 + name.size() + 1;
    std::size_t const to = request.find("\r\n", from);
    std::string value = request.substr(from, to - from);
    while (!value.empty() && value.front() == ' ')
        value.erase(0, 1);
    return value;
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
        std::size_t const method_end = head->find(' ');
        std::string const method = head->substr(0, method_end);
        std::string const path = head->substr(method_end + 1, head->find(' ', method_end + 1) - method_end - 1);
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            m_asked.push_back(path);
        }
        auto const found = m_site.find(path);
        std::string response;
        if (path == "/echo") {
            // What was sent, said back: the method and the body as the title,
            // the type and the origin it came with in the page.
            std::string const sent = head->substr(head->find("\r\n\r\n") + 4);
            std::string const page = "<!doctype html><title>" + method + " " + sent + "</title><p>type "
                + header_of(*head, "content-type") + "</p><p>origin " + header_of(*head, "origin") + "</p>";
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nContent-Length: "
                + std::to_string(page.size()) + "\r\nConnection: close\r\n\r\n" + page;
        } else if (path == "/moved") {
            response = "HTTP/1.1 303 See Other\r\nLocation: /second\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else if (found == m_site.end()) {
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
        // A sheet that is not there, and a script that adds a sheet later, so
        // the page's sheets are collected a second time.
        { "/gone", { "text/html",
                       "<!doctype html><title>Gone</title><link rel=stylesheet href=/gone.css><script>setTimeout(function () {"
                       " var s = document.createElement('style'); s.textContent = 'p { margin: 0 }'; document.head.appendChild(s);"
                       " document.title = 'Restyled'; }, 0);</script><p>gone",
                       0 } },
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

    // A sheet that did not arrive is not asked for again when the page's
    // sheets are collected a second time: a browser fetches a link's sheet
    // once, and a server that is refusing is not helped by being asked at
    // every restyle.
    {
        SiteServer server(site());
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        browser.open(server.url("/gone"));
        load_fully(browser);
        CHECK_EQ(server.asked("/gone.css"), std::size_t { 1 });
        auto const started = std::chrono::steady_clock::now();
        while (browser.page_title() != "Restyled" && ms_since(started) < 3000) {
            browser.run_scripts();
            browser.tick();
        }
        CHECK_EQ(browser.page_title(), std::string("Restyled"));
        CHECK_EQ(browser.page_text(), std::string("gone")); // laid out again, with the sheet the script added
        CHECK_EQ(server.asked("/gone.css"), std::size_t { 1 });
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

    // A form that posts: a press of Enter in its field sends the data set as
    // the body of a navigation, with its type and the origin it came from;
    // a script's form.submit() does the same; a 303 met on the way is followed
    // with a GET; and Back is the form again.
    {
        std::map<std::string, Served> pages = site();
        pages["/form"] = { "text/html",
            "<!doctype html><title>Form</title><form method=post action=/echo><input name=a value=hello>"
            "<input name=b value='1 2'><input type=submit value=Go></form>",
            0 };
        pages["/auto"] = { "text/html",
            "<!doctype html><title>Auto</title><form id=f method=post action=/echo><input type=hidden name=token value=t0k></form>"
            "<script>document.getElementById('f').submit()</script>",
            0 };
        pages["/mover"] = { "text/html",
            "<!doctype html><title>Mover</title><form method=post action=/moved><input name=x value=1></form>", 0 };
        SiteServer server(std::move(pages));
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        platform::KeyEvent enter;
        enter.key = platform::Key::Enter;

        browser.open(server.url("/form"));
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("Form"));
        CHECK(browser.focus_control("a"));
        browser.key_down(enter);
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("POST a=hello&b=1+2"));
        std::string const said = browser.page_text();
        CHECK(said.find("type application/x-www-form-urlencoded") != std::string::npos);
        CHECK(said.find("origin http://127.0.0.1:") != std::string::npos);
        browser.back();
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("Form"));

        browser.open(server.url("/auto"));
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("POST token=t0k"));

        browser.open(server.url("/mover"));
        load_fully(browser);
        CHECK(browser.focus_control("x"));
        browser.key_down(enter);
        load_fully(browser);
        CHECK_EQ(browser.page_title(), std::string("Second"));
        CHECK_EQ(server.asked("/moved"), std::size_t { 1 });
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

    // A font on its way does not hold the page: the page is there in the
    // fonts the machine has, still owed its own, and laid out again in it
    // when it comes — fetched the once it was asked for ahead.
    {
        std::map<std::string, Served> fonted = site();
        std::vector<std::uint8_t> const ttf = text::SashfoldMono::instance().to_truetype();
        fonted["/fonted"] = { "text/html",
            "<!doctype html><title>Fonted</title><style>@font-face { font-family: Late; src: url(/late.ttf) }"
            " p { font-family: Late }</style><p>text",
            0 };
        fonted["/late.ttf"] = { "font/ttf", std::string(ttf.begin(), ttf.end()), 400 };
        SiteServer server(fonted);
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        auto const started = std::chrono::steady_clock::now();
        browser.open(server.url("/fonted"));
        while (browser.page_title() != "Fonted" && ms_since(started) < 5000)
            browser.tick();
        double const shown = ms_since(started);
        CHECK_EQ(browser.page_title(), std::string("Fonted"));
        if (shown >= 400)
            std::cerr << "the page waited " << shown << " ms for its font\n";
        CHECK(shown < 400); // not held for the font's 400
        CHECK_EQ(text::FontManager::instance().page_font_count(), 0u); // laid out without it
        CHECK(browser.has_pending_load()); // and owed it
        load_fully(browser);
        double const swapped = ms_since(started);
        CHECK(swapped >= 400);
        CHECK(swapped < 2000);
        CHECK_EQ(text::FontManager::instance().page_font_count(), 1u);
        CHECK(!browser.has_pending_load());
        CHECK_EQ(server.asked("/late.ttf"), std::size_t { 1 });
    }

    // A page that states its policy in a <meta>, and a policy that admits a
    // script by its nonce alone, still have their scripts asked for ahead:
    // the two slow ones come together rather than one after the other as
    // the parser meets them, each fetched the once, and the one the policy
    // forbids — no nonce, and 'strict-dynamic' sets the hosts aside — is
    // never asked for at all.
    {
        std::map<std::string, Served> scripted = site();
        scripted["/scripted"] = { "text/html",
            "<!doctype html><meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'nonce-n0nce' 'strict-dynamic' http://127.0.0.1:*\">"
            "<title>Scripted</title><script src=/a.js nonce=n0nce></script><script src=/b.js nonce=n0nce></script>"
            "<script src=/c.js></script><p>text",
            0 };
        scripted["/a.js"] = { "text/javascript", "document.title = 'A';", 400 };
        scripted["/b.js"] = { "text/javascript", "document.title = document.title + 'B';", 400 };
        scripted["/c.js"] = { "text/javascript", "document.title = 'never';", 0 };
        SiteServer server(scripted);
        ui::ShellLoader loader;
        ui::Browser browser(loader, ui::Theme {}, 800, 600);
        auto const started = std::chrono::steady_clock::now();
        browser.open(server.url("/scripted"));
        load_fully(browser);
        double const took = ms_since(started);
        CHECK_EQ(browser.page_title(), std::string("AB"));
        if (took >= 700)
            std::cerr << "the page took " << took << " ms for two 400 ms scripts\n";
        CHECK(took < 700); // together: one wait of 400, not two
        CHECK_EQ(server.asked("/a.js"), std::size_t { 1 });
        CHECK_EQ(server.asked("/b.js"), std::size_t { 1 });
        CHECK_EQ(server.asked("/c.js"), std::size_t { 0 });
    }

    return test::report("navigation");
}
