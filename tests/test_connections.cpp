#include "Test.h"

#include "net/Connections.h"
#include "net/Http.h"
#include "platform/Net.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Persistent connections: the parser's keep-alive verdict on canned bytes,
// the pool's bookkeeping, and fetch() reusing, dropping and retrying
// connections against loopback servers that count what they see.

using namespace sashfold;

namespace {

struct CannedSource {
    std::vector<std::uint8_t> data;
    std::size_t at = 0;

    std::ptrdiff_t operator()(std::uint8_t* buffer, std::size_t size)
    {
        if (at >= data.size())
            return 0;
        std::size_t const take = std::min(size, data.size() - at);
        std::memcpy(buffer, data.data() + at, take);
        at += take;
        return static_cast<std::ptrdiff_t>(take);
    }
};

std::optional<net::RawResponse> parse_canned(std::string_view text)
{
    CannedSource source { std::vector<std::uint8_t>(text.begin(), text.end()), 0 };
    auto const read = [&source](std::uint8_t* buffer, std::size_t size) {
        return source(buffer, size);
    };
    return net::read_response(read, 1u << 20);
}

std::string text_of(std::vector<std::uint8_t> const& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

net::Url loopback_url(platform::TcpListener const& listener, std::string const& path)
{
    return *net::parse_url("http://127.0.0.1:" + std::to_string(listener.port()) + path);
}

// One request head from a client; nullopt once the client is gone.
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

// A loopback server that answers requests with `responses` in order, as
// many per connection as the client sends, and remembers every connection
// it accepted with the number of requests it carried and each request
// head. With `close_after_one` it hangs up after a connection's first
// response, the way a server whose idle timeout has passed does.
class CountingServer {
public:
    explicit CountingServer(std::vector<std::string> responses, bool close_after_one = false)
        : m_listener(*platform::TcpListener::listen_loopback())
        , m_responses(std::move(responses))
        , m_close_after_one(close_after_one)
        , m_thread([this] { run(); })
    {
    }

    ~CountingServer() { finish(); }

    net::Url url(std::string const& path) const { return loopback_url(m_listener, path); }

    // Stops the server whether or not the client sent every request. The
    // client must have dropped its pooled connections first.
    void finish()
    {
        if (!m_thread.joinable())
            return;
        m_stop = true;
        if (auto poke = platform::TcpSocket::connect("127.0.0.1", m_listener.port()))
            poke->close();
        m_thread.join();
    }

    std::vector<int> const& requests_per_connection() const { return m_requests; }
    std::vector<std::string> const& heads() const { return m_heads; }

private:
    void run()
    {
        std::size_t next = 0;
        while (next < m_responses.size()) {
            auto client = m_listener.accept();
            if (!client || m_stop)
                return;
            int served = 0;
            while (next < m_responses.size()) {
                std::optional<std::string> head = read_request(*client);
                if (!head)
                    break;
                m_heads.push_back(std::move(*head));
                std::string const& response = m_responses[next++];
                (void)client->send_all(reinterpret_cast<std::uint8_t const*>(response.data()),
                    response.size());
                ++served;
                if (m_close_after_one)
                    break;
            }
            m_requests.push_back(served);
            client->close();
        }
    }

    platform::TcpListener m_listener;
    std::vector<std::string> m_responses;
    bool m_close_after_one;
    std::atomic<bool> m_stop = false;
    std::vector<int> m_requests;
    std::vector<std::string> m_heads;
    std::thread m_thread; // last: everything above is ready when it starts
};

bool head_says(std::vector<std::string> const& heads, std::size_t index, std::string_view line)
{
    return heads.size() > index && heads[index].find(line) != std::string::npos;
}

} // namespace

int main()
{
    // --- The keep-alive verdict on canned responses -------------------------
    {
        auto const framed = parse_canned("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        CHECK(framed && framed->keep_alive);
        auto const closing
            = parse_canned("HTTP/1.1 200 OK\r\nConnection: Close\r\nContent-Length: 2\r\n\r\nok");
        CHECK(closing && !closing->keep_alive);
        auto const listed = parse_canned(
            "HTTP/1.1 200 OK\r\nConnection: Upgrade, close\r\nContent-Length: 2\r\n\r\nok");
        CHECK(listed && !listed->keep_alive);
        auto const second_header = parse_canned("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n"
                                                "Connection: close\r\nContent-Length: 2\r\n\r\nok");
        CHECK(second_header && !second_header->keep_alive); // every Connection header counts
        auto const chunked = parse_canned("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                          "2\r\nok\r\n0\r\nX-Trailer: 1\r\n\r\n");
        CHECK(chunked && chunked->keep_alive && text_of(chunked->body) == "ok");
        auto const to_close = parse_canned("HTTP/1.1 200 OK\r\n\r\nuntil the end");
        CHECK(to_close && !to_close->keep_alive && text_of(to_close->body) == "until the end");
        auto const old = parse_canned("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
        CHECK(old && !old->keep_alive); // 1.0 closes unless it says otherwise
        auto const old_kept = parse_canned(
            "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n\r\nok");
        CHECK(old_kept && old_kept->keep_alive);
        // Bytes past the body: the stream is out of step with us.
        auto const trailing = parse_canned("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokJUNK");
        CHECK(trailing && text_of(trailing->body) == "ok" && !trailing->keep_alive);
    }

    // --- Bodiless statuses and interim responses ------------------------------
    {
        auto const no_content = parse_canned("HTTP/1.1 204 No Content\r\n\r\n");
        CHECK(no_content && no_content->status == 204 && no_content->body.empty()
            && no_content->keep_alive);
        // A 304 may repeat the entity's Content-Length; no body follows it.
        auto const not_modified
            = parse_canned("HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n");
        CHECK(not_modified && not_modified->status == 304 && not_modified->body.empty()
            && not_modified->keep_alive);
        auto const hinted = parse_canned("HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\n"
                                         "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        CHECK(hinted && hinted->status == 200 && text_of(hinted->body) == "ok" && hinted->keep_alive
            && net::find_header(hinted->headers, "link") == nullptr);
        std::string endless;
        for (int i = 0; i < 10; ++i)
            endless += "HTTP/1.1 100 Continue\r\n\r\n";
        CHECK(!parse_canned(endless + "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n").has_value());
    }

    // --- The pool's bookkeeping -----------------------------------------------
    {
        auto listener = platform::TcpListener::listen_loopback();
        CHECK(listener.has_value());
        if (listener) {
            // A holder accepts and keeps every connection so open() succeeds
            // as often as the pool needs one.
            std::vector<platform::TcpSocket> held;
            std::atomic<bool> stop = false;
            std::thread holder([&] {
                while (!stop) {
                    auto client = listener->accept();
                    if (!client)
                        break;
                    held.push_back(std::move(*client));
                }
            });
            auto const open = [&] {
                std::string error;
                auto connection = net::Connection::open("127.0.0.1", listener->port(), false, error);
                CHECK(connection.has_value() && error.empty());
                return std::move(*connection);
            };

            std::string const a = net::origin_key(false, "a.example", 80);
            std::string const b = net::origin_key(false, "b.example", 80);
            std::string const c = net::origin_key(true, "c.example", 443);
            CHECK_EQ(a, "http://a.example:80");
            CHECK_EQ(c, "https://c.example:443");

            net::ConnectionPool pool(4, 2, 60);
            std::int64_t const now = 1'000'000;
            pool.give(a, open(), now);
            pool.give(a, open(), now + 1);
            pool.give(a, open(), now + 2);
            CHECK_EQ(pool.size(), std::size_t { 2 }); // per-origin cap: the oldest went
            pool.give(b, open(), now + 3);
            pool.give(b, open(), now + 4);
            CHECK_EQ(pool.size(), std::size_t { 4 });
            pool.give(c, open(), now + 5);
            CHECK_EQ(pool.size(), std::size_t { 4 }); // total cap: the longest idle went
            CHECK(pool.take(c, now + 5).has_value());
            CHECK_EQ(pool.size(), std::size_t { 3 });
            CHECK(!pool.take(net::origin_key(false, "nobody.example", 80), now + 5).has_value());
            CHECK_EQ(pool.stats().reused, std::size_t { 1 });
            // Idle expiry: a's survivor is 59 s old at now + 61 and stays;
            // b's older one is 61 s old at now + 64 and goes, its younger
            // sibling (60 s) is still within the allowance.
            CHECK(pool.take(a, now + 61).has_value());
            CHECK_EQ(pool.size(), std::size_t { 2 });
            CHECK(pool.take(b, now + 64).has_value());
            CHECK_EQ(pool.size(), std::size_t { 0 });
            CHECK(!pool.take(b, now + 64).has_value());
            CHECK_EQ(pool.stats().reused, std::size_t { 3 });

            pool.give(a, open(), now);
            pool.clear();
            CHECK_EQ(pool.size(), std::size_t { 0 });

            stop = true;
            if (auto poke = platform::TcpSocket::connect("127.0.0.1", listener->port()))
                poke->close();
            holder.join();
        }
    }

    // --- Two requests, one connection ----------------------------------------
    {
        CountingServer server({ "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst",
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond" });
        net::ConnectionPool pool;
        net::FetchOptions options;
        options.pool = &pool;
        net::FetchResult const first = net::fetch(server.url("/a"), options);
        CHECK(first.response && text_of(first.response->body) == "first");
        CHECK_EQ(pool.size(), std::size_t { 1 });
        net::FetchResult const second = net::fetch(server.url("/b"), options);
        CHECK(second.response && text_of(second.response->body) == "second");
        CHECK_EQ(pool.size(), std::size_t { 1 });
        pool.clear();
        server.finish();
        CHECK_EQ(server.requests_per_connection().size(), std::size_t { 1 });
        CHECK(!server.requests_per_connection().empty() && server.requests_per_connection()[0] == 2);
        CHECK_EQ(pool.stats().opened, std::size_t { 1 });
        CHECK_EQ(pool.stats().reused, std::size_t { 1 });
        CHECK_EQ(pool.stats().retried, std::size_t { 0 });
        CHECK(head_says(server.heads(), 0, "Connection: keep-alive\r\n"));
        CHECK(head_says(server.heads(), 1, "GET /b HTTP/1.1\r\n"));
    }

    // --- Without a pool every request closes its connection ------------------
    {
        CountingServer server({ "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok" });
        net::FetchResult const result = net::fetch(server.url("/plain"));
        CHECK(result.response && text_of(result.response->body) == "ok");
        server.finish();
        CHECK(head_says(server.heads(), 0, "Connection: close\r\n"));
    }

    // --- The server asks for a close -----------------------------------------
    {
        CountingServer server(
            { "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3\r\n\r\nbye",
                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain" });
        net::ConnectionPool pool;
        net::FetchOptions options;
        options.pool = &pool;
        net::FetchResult const first = net::fetch(server.url("/a"), options);
        CHECK(first.response && text_of(first.response->body) == "bye");
        CHECK_EQ(pool.size(), std::size_t { 0 });
        net::FetchResult const second = net::fetch(server.url("/b"), options);
        CHECK(second.response && text_of(second.response->body) == "again");
        pool.clear();
        server.finish();
        CHECK((server.requests_per_connection() == std::vector<int> { 1, 1 }));
        CHECK_EQ(pool.stats().opened, std::size_t { 2 });
        CHECK_EQ(pool.stats().reused, std::size_t { 0 });
        CHECK_EQ(pool.stats().retried, std::size_t { 0 });
    }

    // --- A body read to close uses the connection up -------------------------
    {
        CountingServer server({ "HTTP/1.0 200 OK\r\n\r\nuntil the end",
                                  "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnext" },
            /*close_after_one=*/true);
        net::ConnectionPool pool;
        net::FetchOptions options;
        options.pool = &pool;
        net::FetchResult const first = net::fetch(server.url("/a"), options);
        CHECK(first.response && text_of(first.response->body) == "until the end");
        CHECK_EQ(pool.size(), std::size_t { 0 });
        net::FetchResult const second = net::fetch(server.url("/b"), options);
        CHECK(second.response && text_of(second.response->body) == "next");
        pool.clear();
        server.finish();
        CHECK((server.requests_per_connection() == std::vector<int> { 1, 1 }));
        CHECK_EQ(pool.stats().opened, std::size_t { 2 });
        CHECK_EQ(pool.stats().retried, std::size_t { 0 });
    }

    // --- A pooled connection the server has since dropped: one retry ---------
    {
        CountingServer server({ "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nkept",
                                  "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfresh" },
            /*close_after_one=*/true);
        net::ConnectionPool pool;
        net::FetchOptions options;
        options.pool = &pool;
        net::FetchResult const first = net::fetch(server.url("/a"), options);
        CHECK(first.response && text_of(first.response->body) == "kept");
        CHECK_EQ(pool.size(), std::size_t { 1 }); // kept, though the server has hung up
        net::FetchResult const second = net::fetch(server.url("/b"), options);
        CHECK(second.response && text_of(second.response->body) == "fresh");
        pool.clear();
        server.finish();
        CHECK((server.requests_per_connection() == std::vector<int> { 1, 1 }));
        CHECK_EQ(pool.stats().opened, std::size_t { 2 });
        CHECK_EQ(pool.stats().reused, std::size_t { 1 });
        CHECK_EQ(pool.stats().retried, std::size_t { 1 });
    }

    // --- A redirect within the origin rides the same connection --------------
    {
        CountingServer server(
            { "HTTP/1.1 302 Found\r\nLocation: /landed\r\nContent-Length: 0\r\n\r\n",
                "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nlanded" });
        net::ConnectionPool pool;
        net::FetchOptions options;
        options.pool = &pool;
        net::FetchResult const result = net::fetch(server.url("/start"), options);
        CHECK(result.response && text_of(result.response->body) == "landed");
        CHECK(result.response && result.response->final_url.serialize_path() == "/landed");
        CHECK_EQ(pool.size(), std::size_t { 1 });
        pool.clear();
        server.finish();
        CHECK(server.requests_per_connection() == std::vector<int> { 2 });
        CHECK_EQ(pool.stats().opened, std::size_t { 1 });
        CHECK_EQ(pool.stats().reused, std::size_t { 1 });
    }

    return sashfold::test::report("connections");
}
