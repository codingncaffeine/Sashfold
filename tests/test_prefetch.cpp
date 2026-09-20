#include "Test.h"

#include "net/Filters.h"
#include "net/Url.h"
#include "platform/Net.h"
#include "ui/ShellLoader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A page's resources asked for AHEAD of the page's own asking (ShellLoader::
// prefetch): fetched together on the pool's threads, claimed by the request
// that follows instead of being fetched again, and still judged — by the
// lists before anything leaves, by the page's guard where a redirect ended.

using namespace sashfold;

namespace {

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

// A loopback server that takes every connection on a thread of its own,
// waits `delay_ms`, and answers with the path asked for as the body — or,
// for /hop, with a redirect to /landed. It remembers every path asked for.
class SlowServer {
public:
    explicit SlowServer(int delay_ms)
        : m_listener(*platform::TcpListener::listen_loopback())
        , m_delay_ms(delay_ms)
        , m_thread([this] { run(); })
    {
    }

    ~SlowServer()
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

    std::vector<std::string> asked() const
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        return m_asked;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(m_delay_ms));
        std::string response;
        if (path == "/hop") {
            response = "HTTP/1.1 302 Found\r\nLocation: /landed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else {
            std::string const body = "body of " + path;
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nContent-Length: "
                + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        }
        (void)client.send_all(reinterpret_cast<std::uint8_t const*>(response.data()), response.size());
        client.close();
    }

    platform::TcpListener m_listener;
    int m_delay_ms;
    std::atomic<bool> m_stop = false;
    mutable std::mutex m_mutex;
    std::vector<std::string> m_asked;
    std::vector<std::thread> m_handlers;
    std::thread m_thread; // last: everything above is ready when it starts
};

std::string body_of(net::FetchResult const& result)
{
    return result.response ? std::string(result.response->body.begin(), result.response->body.end()) : "error: " + result.error;
}

double ms_since(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}

std::size_t count_of(std::vector<std::string> const& asked, std::string const& path)
{
    std::size_t count = 0;
    for (std::string const& one : asked)
        count += one == path ? 1 : 0;
    return count;
}

}

int main()
{
    // Six resources that each take 200 ms, asked for ahead and then asked
    // for by the page one after another: about 200 ms in all, not 1200 —
    // and the server was asked for each of them once.
    {
        SlowServer server(200);
        ui::ShellLoader loader;
        net::Url const page = server.url("/page");
        std::vector<std::string> const paths { "/a.css", "/b.js", "/c.png", "/d.png", "/e.png", "/f.woff2" };
        std::vector<net::ResourceKind> const kinds { net::ResourceKind::Stylesheet, net::ResourceKind::Script,
            net::ResourceKind::Image, net::ResourceKind::Image, net::ResourceKind::Image, net::ResourceKind::Font };
        auto const started = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < paths.size(); ++i)
            loader.prefetch(server.url(paths[i]), page, page.serialize(), kinds[i]);
        // Asking ahead waits for nothing.
        CHECK(ms_since(started) < 150);
        for (std::size_t i = 0; i < paths.size(); ++i)
            CHECK_EQ(body_of(loader.load_subresource(server.url(paths[i]), page, page.serialize(), kinds[i])), "body of " + paths[i]);
        double const took = ms_since(started);
        if (took < 190 || took >= 800)
            std::cerr << "six fetches of 200 ms took " << took << " ms\n";
        CHECK(took >= 190);
        CHECK(took < 800);
        std::vector<std::string> const asked = server.asked();
        CHECK_EQ(asked.size(), paths.size());
        for (std::string const& path : paths)
            CHECK_EQ(count_of(asked, path), std::size_t { 1 });
        // Claimed once: the same address asked for again is fetched again.
        CHECK_EQ(body_of(loader.load_subresource(server.url("/a.css"), page, page.serialize(), net::ResourceKind::Stylesheet)),
            std::string("body of /a.css"));
        CHECK_EQ(count_of(server.asked(), "/a.css"), std::size_t { 2 });
        // Asked for ahead as one kind, asked for by the page as another: not
        // the same request, and the page's own goes out.
        loader.prefetch(server.url("/as-image"), page, page.serialize(), net::ResourceKind::Image);
        CHECK_EQ(body_of(loader.load_subresource(server.url("/as-image"), page, page.serialize(), net::ResourceKind::Script)),
            std::string("body of /as-image"));
    }

    // What the lists refuse is never asked for ahead, and a local file or a
    // data: address is nobody's to fetch on a thread.
    {
        SlowServer server(10);
        ui::ShellLoader loader;
        net::Blocklists lists;
        lists.add(net::FilterList::parse("/tracker.js\n", "test"));
        loader.set_blocklists(std::move(lists));
        net::Url const page = server.url("/page");
        loader.prefetch(server.url("/tracker.js"), page, "", net::ResourceKind::Script);
        loader.prefetch(*net::parse_url("file:///etc/hostname"), page, "", net::ResourceKind::Image);
        loader.prefetch(*net::parse_url("data:text/plain,hello"), page, "", net::ResourceKind::Image);
        loader.prefetch(server.url("/fine.js"), page, "", net::ResourceKind::Script);
        CHECK_EQ(body_of(loader.load_subresource(server.url("/fine.js"), page, "", net::ResourceKind::Script)),
            std::string("body of /fine.js"));
        CHECK(body_of(loader.load_subresource(server.url("/tracker.js"), page, "", net::ResourceKind::Script)).starts_with("error: blocked by test"));
        CHECK_EQ(count_of(server.asked(), "/tracker.js"), std::size_t { 0 });
        CHECK_EQ(server.asked().size(), std::size_t { 1 });
    }

    // A redirect followed ahead of the page's asking was judged by the lists
    // alone; where it ended is judged by the page's guard when it is claimed.
    {
        SlowServer server(10);
        ui::ShellLoader loader;
        net::Url const page = server.url("/page");
        net::RequestGuard strict;
        strict.refusal = [](net::Url const& url, bool) -> std::optional<std::string> {
            if (url.serialize_path() == "/landed")
                return "refused by the page's policy";
            return std::nullopt;
        };
        loader.prefetch(server.url("/hop"), page, "", net::ResourceKind::Image);
        CHECK_EQ(body_of(loader.load_subresource(server.url("/hop"), page, "", net::ResourceKind::Image, strict)),
            std::string("error: refused by the page's policy"));
        // With no such guard it is what it is.
        loader.prefetch(server.url("/hop"), page, "", net::ResourceKind::Image);
        CHECK_EQ(body_of(loader.load_subresource(server.url("/hop"), page, "", net::ResourceKind::Image)), std::string("body of /landed"));
    }

    // A loader that goes away with fetches asked for and never claimed.
    {
        SlowServer server(100);
        {
            ui::ShellLoader loader;
            net::Url const page = server.url("/page");
            for (int i = 0; i < 12; ++i)
                loader.prefetch(server.url("/unclaimed-" + std::to_string(i)), page, "", net::ResourceKind::Image);
        }
        CHECK(server.asked().size() <= 12);
    }

    return test::report("prefetch");
}
