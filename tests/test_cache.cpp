#include "Test.h"

#include "net/Cache.h"
#include "net/Http.h"
#include "net/Url.h"
#include "platform/Net.h"

#include <string>
#include <thread>
#include <vector>

// The freshness rules header by header, the cache's storage policy, and the
// choke-point wiring over loopback sockets: a hit never opens a connection.

using namespace sashfold;
using namespace sashfold::net;

namespace {

using Headers = std::vector<Header>;

Url url_of(std::string const& text)
{
    auto const url = parse_url(text);
    CHECK(url.has_value());
    return url ? *url : Url {};
}

FetchResponse ok(Headers headers, std::string const& body = "body", int status = 200)
{
    FetchResponse response;
    response.status = status;
    response.status_text = status == 200 ? "OK" : "Not OK";
    response.headers = std::move(headers);
    response.body.assign(body.begin(), body.end());
    return response;
}

std::string body_text(FetchResponse const& response)
{
    return std::string(response.body.begin(), response.body.end());
}

// Serves `times` connections with a fixed response, then exits.
void serve(platform::TcpListener& listener, std::string response, int times)
{
    for (int i = 0; i < times; ++i) {
        auto client = listener.accept();
        if (!client)
            return;
        std::uint8_t buffer[4096];
        (void)client->receive(buffer, sizeof buffer);
        (void)client->send_all(reinterpret_cast<std::uint8_t const*>(response.data()),
            response.size());
        client->close();
    }
}

std::string loopback(platform::TcpListener const& listener, std::string const& path)
{
    return "http://127.0.0.1:" + std::to_string(listener.port()) + path;
}

} // namespace

int main()
{
    std::int64_t const now = 1'700'000'000;

    // --- Explicit freshness ---------------------------------------------------
    CHECK(!fresh_until(Headers { { "Content-Type", "text/html" } }, now).has_value());
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "max-age=60" } }, now).value_or(0), now + 60);
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "public, max-age=\"60\"" } }, now).value_or(0),
        now + 60); // the quoted form must be accepted
    CHECK_EQ(fresh_until(Headers { { "cache-control", "MAX-AGE = 60" } }, now).value_or(0), now + 60);
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "max-age=60" }, { "Age", "20" } }, now)
                 .value_or(0),
        now + 40); // Age already spent
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "max-age=60" }, { "Age", "soon" } }, now)
                 .value_or(0),
        now + 60); // an invalid Age is ignored
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "max-age=abc" } }, now).value_or(-1), now);
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "max-age=99999999999999999999" } }, now)
                 .value_or(0),
        now + (std::int64_t { 1 } << 31)); // saturates, never overflows
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "max-age=60, max-age=600" } }, now)
                 .value_or(0),
        now + 60); // first instance wins
    CHECK(!fresh_until(Headers { { "Cache-Control", "max-age=60, no-store" } }, now).has_value());
    CHECK(!fresh_until(Headers { { "Cache-Control", "no-cache" } }, now).has_value());
    CHECK(!fresh_until(Headers { { "Cache-Control", "max-age=60" }, { "Cache-Control", "no-store" } },
        now)
               .has_value()); // directives split across lines still combine
    CHECK_EQ(fresh_until(Headers { { "Cache-Control", "ext=\"no-store, x\", max-age=60" } }, now)
                 .value_or(0),
        now + 60); // a quoted no-store is an argument, not a directive

    CHECK_EQ(fresh_until(Headers { { "Expires", "Wed, 09 Jun 2021 10:18:14 GMT" } }, now)
                 .value_or(0),
        std::int64_t { 1623233894 }); // absolute when the origin sent no Date
    CHECK_EQ(fresh_until(Headers { { "Expires", "Wed, 09 Jun 2021 10:19:14 GMT" },
                             { "Date", "Wed, 09 Jun 2021 10:18:14 GMT" } },
                 now)
                 .value_or(0),
        now + 60); // lifetime measured by the origin's clock, applied to ours
    CHECK_EQ(fresh_until(Headers { { "Expires", "0" } }, now).value_or(-1), now);
    CHECK_EQ(fresh_until(Headers { { "Expires", "Wed, 09 Jun 2021 10:18:14 GMT" },
                             { "Cache-Control", "max-age=60" } },
                 now)
                 .value_or(0),
        now + 60); // max-age beats Expires

    CHECK(fresh_until(Headers { { "Cache-Control", "max-age=60" }, { "Vary", "Accept-Encoding, accept" } },
        now)
              .has_value());
    CHECK(!fresh_until(Headers { { "Cache-Control", "max-age=60" }, { "Vary", "Cookie" } }, now)
               .has_value());
    CHECK(!fresh_until(Headers { { "Cache-Control", "max-age=60" }, { "Vary", "*" } }, now)
               .has_value());

    // --- Storage policy -------------------------------------------------------
    {
        MemoryCache cache;
        Url const page = url_of("http://example.com/a?x=1#section");
        CHECK(cache.store(page, ok({ { "Cache-Control", "max-age=60" } }, "hello"), now));
        CHECK_EQ(cache.size(), std::size_t { 1 });
        FetchResponse const* const hit = cache.lookup(url_of("http://example.com/a?x=1"), now + 59);
        CHECK(hit != nullptr);
        if (hit) {
            CHECK(hit->from_cache);
            CHECK_EQ(hit->status, 200);
            CHECK_EQ(body_text(*hit), "hello");
        }
        CHECK(cache.lookup(page, now + 60) == nullptr); // the boundary is exclusive
        CHECK(cache.lookup(url_of("http://example.com/a?x=2"), now) == nullptr);
        CHECK(cache.lookup(url_of("https://example.com/a?x=1"), now) == nullptr);
    }
    {
        MemoryCache cache;
        Url const page = url_of("http://example.com/");
        CHECK(!cache.store(page, ok({ { "Cache-Control", "max-age=60" } }, "x", 404), now));
        CHECK(!cache.store(page, ok({ { "Cache-Control", "no-store" } }), now));
        CHECK(!cache.store(page, ok({ { "Content-Type", "text/html" } }), now));
        CHECK(!cache.store(page, ok({ { "Cache-Control", "max-age=0" } }), now));
        CHECK(!cache.store(page, ok({ { "Cache-Control", "max-age=60" }, { "Age", "60" } }), now));
        CHECK_EQ(cache.size(), std::size_t { 0 });
    }
    {
        // Set-Cookie never rides a cache hit; the other headers do.
        MemoryCache cache;
        Url const page = url_of("http://example.com/");
        CHECK(cache.store(page,
            ok({ { "Set-Cookie", "sid=1" }, { "Content-Type", "text/html" },
                { "Cache-Control", "max-age=60" }, { "set-cookie", "t=2" } }),
            now));
        FetchResponse const* const hit = cache.lookup(page, now);
        CHECK(hit != nullptr);
        if (hit) {
            CHECK(find_header(hit->headers, "set-cookie") == nullptr);
            CHECK(find_header(hit->headers, "content-type") != nullptr);
            CHECK_EQ(hit->headers.size(), std::size_t { 2 });
        }
    }
    {
        // Caps: an oversized body is refused; the total evicts oldest-first,
        // after stale entries have been swept. Each 200-byte entry costs
        // 243 bytes (body + 20-byte key + 23 bytes of Cache-Control), so
        // three fit in 800 and a fourth does not.
        MemoryCache cache(800, 400);
        Url const big = url_of("http://example.com/big");
        CHECK(!cache.store(big, ok({ { "Cache-Control", "max-age=60" } }, std::string(401, 'b')), now));
        Url const a = url_of("http://example.com/a");
        Url const b = url_of("http://example.com/b");
        Url const c = url_of("http://example.com/c");
        Url const d = url_of("http://example.com/d");
        CHECK(cache.store(a, ok({ { "Cache-Control", "max-age=10" } }, std::string(200, 'a')), now));
        CHECK(cache.store(b, ok({ { "Cache-Control", "max-age=60" } }, std::string(200, 'b')), now + 1));
        CHECK(cache.store(c, ok({ { "Cache-Control", "max-age=60" } }, std::string(200, 'c')), now + 2));
        CHECK_EQ(cache.size(), std::size_t { 3 });
        // At now+10, a is stale: it is swept and b, c stay.
        CHECK(cache.store(d, ok({ { "Cache-Control", "max-age=60" } }, std::string(200, 'd')), now + 10));
        CHECK(cache.lookup(a, now + 10) == nullptr);
        CHECK(cache.lookup(b, now + 10) != nullptr);
        CHECK(cache.lookup(c, now + 10) != nullptr);
        CHECK(cache.lookup(d, now + 10) != nullptr);
        // Another entry no longer fits: b, the oldest fresh one, goes.
        Url const e = url_of("http://example.com/e");
        CHECK(cache.store(e, ok({ { "Cache-Control", "max-age=60" } }, std::string(200, 'e')), now + 11));
        CHECK(cache.lookup(b, now + 11) == nullptr);
        CHECK(cache.lookup(c, now + 11) != nullptr);
        CHECK(cache.lookup(e, now + 11) != nullptr);
        CHECK(cache.bytes() <= 1000);
        // Re-storing a key replaces it without double counting.
        std::size_t const before = cache.bytes();
        CHECK(cache.store(e, ok({ { "Cache-Control", "max-age=60" } }, std::string(200, 'E')), now + 12));
        CHECK_EQ(cache.bytes(), before);
        FetchResponse const* const hit = cache.lookup(e, now + 12);
        CHECK(hit && body_text(*hit) == std::string(200, 'E'));
    }

    // --- Through the choke point: a hit never opens a connection -------------
    {
        auto listener = platform::TcpListener::listen_loopback();
        CHECK(listener.has_value());
        if (listener) {
            std::thread server(serve, std::ref(*listener),
                std::string("HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\n"
                            "Set-Cookie: sid=1\r\nContent-Length: 5\r\n\r\nfresh"),
                1);
            MemoryCache cache;
            FetchOptions options;
            options.cache = &cache;
            Url const url = url_of(loopback(*listener, "/page"));
            FetchResult const first = fetch(url, options);
            CHECK(first.response.has_value());
            if (first.response) {
                CHECK(!first.response->from_cache);
                CHECK_EQ(body_text(*first.response), "fresh");
            }
            server.join();
            listener->close(); // nothing listens now: a second connection would fail
            FetchResult const second = fetch(url, options);
            CHECK(second.response.has_value());
            if (second.response) {
                CHECK(second.response->from_cache);
                CHECK_EQ(second.response->status, 200);
                CHECK_EQ(body_text(*second.response), "fresh");
                CHECK(find_header(second.response->headers, "set-cookie") == nullptr);
                CHECK_EQ(second.response->final_url.serialize(), url.serialize());
            }
            CHECK_EQ(cache.size(), std::size_t { 1 });
        }
    }
    {
        // no-store: every load is a real load.
        auto listener = platform::TcpListener::listen_loopback();
        CHECK(listener.has_value());
        if (listener) {
            std::thread server(serve, std::ref(*listener),
                std::string("HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Length: 4\r\n\r\nlive"),
                2);
            MemoryCache cache;
            FetchOptions options;
            options.cache = &cache;
            Url const url = url_of(loopback(*listener, "/live"));
            for (int i = 0; i < 2; ++i) {
                FetchResult const result = fetch(url, options);
                CHECK(result.response.has_value());
                if (result.response) {
                    CHECK(!result.response->from_cache);
                    CHECK_EQ(body_text(*result.response), "live");
                }
            }
            server.join();
            CHECK_EQ(cache.size(), std::size_t { 0 });
        }
    }
    {
        // A redirect into a cached page: the hop goes out, the target does not.
        auto target = platform::TcpListener::listen_loopback();
        auto entry = platform::TcpListener::listen_loopback();
        CHECK(target.has_value() && entry.has_value());
        if (target && entry) {
            std::string const hop = "HTTP/1.1 302 Found\r\nLocation: " + loopback(*target, "/landed")
                + "\r\nContent-Length: 0\r\n\r\n";
            std::thread entry_server(serve, std::ref(*entry), hop, 2);
            std::thread target_server(serve, std::ref(*target),
                std::string("HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\nContent-Length: 6\r\n\r\nlanded"),
                1);
            MemoryCache cache;
            FetchOptions options;
            options.cache = &cache;
            Url const start = url_of(loopback(*entry, "/start"));
            FetchResult const first = fetch(start, options);
            CHECK(first.response && !first.response->from_cache);
            target_server.join();
            target->close();
            FetchResult const second = fetch(start, options);
            CHECK(second.response.has_value());
            if (second.response) {
                CHECK(second.response->from_cache);
                CHECK_EQ(body_text(*second.response), "landed");
                CHECK_EQ(second.response->final_url.serialize(), loopback(*target, "/landed"));
            }
            entry_server.join();
            CHECK(cache.lookup(start, now) == nullptr); // only the 200 was stored
        }
    }

    return sashfold::test::report("cache");
}
