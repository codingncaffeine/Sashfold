#include "Test.h"

#include "net/Cache.h"
#include "net/Hsts.h"
#include "net/Http.h"
#include "net/Url.h"

#include <chrono>

using namespace sashfold;
using namespace sashfold::net;

namespace {

Url url_of(std::string const& text)
{
    auto const url = parse_url(text);
    CHECK(url.has_value());
    return url ? *url : Url {};
}

std::vector<Header> sts(std::initializer_list<char const*> values)
{
    std::vector<Header> headers;
    headers.push_back({ "Server", "test" });
    for (char const* value : values)
        headers.push_back({ "Strict-Transport-Security", value });
    return headers;
}

}

int main()
{
    // --- the header (RFC 6797 §6.1) ------------------------------------------
    {
        auto const plain = parse_strict_transport_security("max-age=31536000");
        CHECK(plain.has_value());
        CHECK_EQ(plain ? plain->max_age : -1, std::int64_t { 31536000 });
        CHECK(plain && !plain->include_subdomains);

        // Names in any case, spaces around everything, a value in quotes,
        // directives in either order, empty ones between semicolons.
        auto const loose = parse_strict_transport_security(" includeSubDomains ;; MAX-AGE = \"600\" ; ");
        CHECK(loose.has_value());
        CHECK_EQ(loose ? loose->max_age : -1, std::int64_t { 600 });
        CHECK(loose && loose->include_subdomains);

        // A directive nobody knows is passed over, with or without a value.
        auto const unknown = parse_strict_transport_security("max-age=5; preload; future=\"a;b\"");
        CHECK(unknown.has_value());
        CHECK_EQ(unknown ? unknown->max_age : -1, std::int64_t { 5 });

        // More seconds than fit is as long as can be said.
        auto const huge = parse_strict_transport_security("max-age=99999999999999999999999999");
        CHECK(huge.has_value());
        CHECK(huge && huge->max_age > std::int64_t { 1 } << 60);

        // Not valid: no max-age, one that is no number of seconds, a
        // directive twice, a value where none goes, a name that is no token,
        // a quote left open.
        CHECK(!parse_strict_transport_security("").has_value());
        CHECK(!parse_strict_transport_security("includeSubDomains").has_value());
        CHECK(!parse_strict_transport_security("max-age").has_value());
        CHECK(!parse_strict_transport_security("max-age=").has_value());
        CHECK(!parse_strict_transport_security("max-age=-1").has_value());
        CHECK(!parse_strict_transport_security("max-age=1.5").has_value());
        CHECK(!parse_strict_transport_security("max-age=12 months").has_value());
        CHECK(!parse_strict_transport_security("max-age=1; max-age=2").has_value());
        CHECK(!parse_strict_transport_security("max-age=1; includeSubDomains; INCLUDESUBDOMAINS").has_value());
        CHECK(!parse_strict_transport_security("max-age=1; includeSubDomains=yes").has_value());
        CHECK(!parse_strict_transport_security("max age=1").has_value());
        CHECK(!parse_strict_transport_security("max-age=\"1").has_value());
    }

    std::int64_t const now = 1'000'000;

    // --- what is remembered, and from whom (§8.1) ----------------------------
    {
        TransportSecurity known;
        CHECK(!known.known("example.test", now));

        // Over plain HTTP the header means nothing.
        known.note(url_of("http://example.test/"), sts({ "max-age=600" }), now);
        CHECK(!known.known("example.test", now));
        CHECK_EQ(known.changes(), std::uint64_t { 0 });

        // An address that is an IP literal is never remembered.
        known.note(url_of("https://192.0.2.7/"), sts({ "max-age=600" }), now);
        known.note(url_of("https://[2001:db8::1]/"), sts({ "max-age=600" }), now);
        CHECK(!known.known("192.0.2.7", now));
        CHECK_EQ(known.size(), std::size_t { 0 });

        // A header that is not valid is as none; the first header alone
        // counts when there are several.
        known.note(url_of("https://example.test/"), sts({ "max-age=nope" }), now);
        CHECK(!known.known("example.test", now));
        known.note(url_of("https://example.test/"), sts({ "max-age=600", "max-age=0" }), now);
        CHECK(known.known("example.test", now));
        CHECK(known.known("EXAMPLE.test.", now));
        CHECK_EQ(known.changes(), std::uint64_t { 1 });

        // Its subdomains were not named, and its parent never is.
        CHECK(!known.known("www.example.test", now));
        CHECK(!known.known("test", now));
        CHECK(!known.known("notexample.test", now));

        // It lasts as long as it asked, and no longer.
        CHECK(known.known("example.test", now + 599));
        CHECK(!known.known("example.test", now + 600));

        // Said again, the time starts again, and subdomains may join.
        known.note(url_of("https://example.test/"), sts({ "max-age=1000; includeSubDomains" }), now + 100);
        CHECK(known.known("example.test", now + 1099));
        CHECK(known.known("www.example.test", now + 100));
        CHECK(known.known("a.b.example.test", now + 100));
        CHECK(!known.known("badexample.test", now + 100));

        // A subdomain's own word does not reach its parent or its siblings.
        TransportSecurity other;
        other.note(url_of("https://deep.example.test/"), sts({ "max-age=600; includeSubDomains" }), now);
        CHECK(other.known("x.deep.example.test", now));
        CHECK(!other.known("example.test", now));
        CHECK(!other.known("side.example.test", now));

        // max-age=0 takes the host off the list.
        known.note(url_of("https://example.test/"), sts({ "max-age=0" }), now + 100);
        CHECK(!known.known("example.test", now + 100));
        CHECK(!known.known("www.example.test", now + 100));
        CHECK_EQ(known.size(), std::size_t { 0 });
    }

    // --- the address as it goes out (§8.3) -----------------------------------
    {
        TransportSecurity known;
        known.note(url_of("https://example.test/"), sts({ "max-age=600; includeSubDomains" }), now);
        CHECK_EQ(known.upgraded(url_of("http://example.test/a?b#c"), now).serialize(), std::string("https://example.test/a?b#c"));
        CHECK_EQ(known.upgraded(url_of("http://www.example.test:80/"), now).serialize(), std::string("https://www.example.test/"));
        // A port that is not the default stays.
        CHECK_EQ(known.upgraded(url_of("http://example.test:8080/"), now).serialize(), std::string("https://example.test:8080/"));
        CHECK_EQ(known.upgraded(url_of("ws://example.test/socket"), now).serialize(), std::string("wss://example.test/socket"));
        // What is https already, what is another host's, what has expired
        // and what is no web address come back as they were.
        CHECK_EQ(known.upgraded(url_of("https://example.test/"), now).serialize(), std::string("https://example.test/"));
        CHECK_EQ(known.upgraded(url_of("http://other.test/"), now).serialize(), std::string("http://other.test/"));
        CHECK_EQ(known.upgraded(url_of("http://example.test/"), now + 600).serialize(), std::string("http://example.test/"));
        CHECK_EQ(known.upgraded(url_of("ftp://example.test/"), now).serialize(), std::string("ftp://example.test/"));
    }

    // --- the list as a file ---------------------------------------------------
    {
        TransportSecurity known;
        known.note(url_of("https://one.test/"), sts({ "max-age=600" }), now);
        known.note(url_of("https://two.test/"), sts({ "max-age=50; includeSubDomains" }), now);
        CHECK_EQ(known.serialize(now), std::string("one.test\t1000600\t0\ntwo.test\t1000050\t1\n"));
        // What has expired is not written.
        CHECK_EQ(known.serialize(now + 60), std::string("one.test\t1000600\t0\n"));

        TransportSecurity read;
        read.load("# a comment\r\none.test\t1000600\t0\r\ntwo.test\t1000050\t1\nold.test\t5\t0\nbroken line\nbad.test\tsoon\t0\nodd.test\t1000600\t2\n", now);
        CHECK_EQ(read.size(), std::size_t { 2 });
        CHECK(read.known("one.test", now));
        CHECK(!read.known("www.one.test", now));
        CHECK(read.known("www.two.test", now));
        CHECK(!read.known("old.test", now));
        CHECK(!read.known("bad.test", now));
        CHECK(!read.known("odd.test", now));
        // Reading what is already held changes nothing.
        std::uint64_t const before = read.changes();
        read.load(read.serialize(now), now);
        CHECK_EQ(read.changes(), before);
    }

    // --- no more hosts than the ceiling ---------------------------------------
    {
        TransportSecurity known;
        for (std::size_t i = 0; i < TransportSecurity::max_hosts; ++i)
            known.note(url_of("https://h" + std::to_string(i) + ".test/"), sts({ i == 7 ? "max-age=10" : "max-age=600" }), now);
        CHECK_EQ(known.size(), TransportSecurity::max_hosts);
        // The one that expires soonest makes room.
        known.note(url_of("https://late.test/"), sts({ "max-age=600" }), now);
        CHECK_EQ(known.size(), TransportSecurity::max_hosts);
        CHECK(known.known("late.test", now));
        CHECK(!known.known("h7.test", now));
        CHECK(known.known("h8.test", now));
    }

    // --- a fetch asks for the https address before anything else -------------
    // No network: a copy of the https page is in the cache, nothing listens
    // on the port, and the request is for the http address. With the host
    // known the fetch is answered by the copy, as the https address; with
    // no list it goes for the http address and finds nothing there.
    {
        using namespace std::chrono;
        std::int64_t const real_now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        HttpCache cache;
        FetchResponse page;
        page.status = 200;
        page.status_text = "OK";
        page.headers.push_back({ "Cache-Control", "max-age=600" });
        page.headers.push_back({ "Content-Type", "text/plain" });
        page.body = { 's', 'a', 'f', 'e' };
        page.final_url = url_of("https://localhost:1/page");
        CHECK(cache.store(page.final_url, page, real_now));

        TransportSecurity known;
        known.load("localhost\t" + std::to_string(real_now + 600) + "\t0\n", real_now);

        FetchOptions options;
        options.cache = &cache;
        options.transport_security = &known;
        FetchResult const upgraded = fetch(url_of("http://localhost:1/page"), options);
        CHECK(upgraded.response.has_value());
        if (upgraded.response) {
            CHECK_EQ(upgraded.response->final_url.serialize(), std::string("https://localhost:1/page"));
            CHECK_EQ(upgraded.response->body.size(), std::size_t { 4 });
        }
        CHECK_EQ(upgraded.timing.requests, 0);

        FetchOptions plain;
        plain.cache = &cache;
        FetchResult const as_written = fetch(url_of("http://localhost:1/page"), plain);
        CHECK(!as_written.response.has_value());
    }

    return sashfold::test::report("hsts");
}
