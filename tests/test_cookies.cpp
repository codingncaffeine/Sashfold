#include "Test.h"

#include "net/Cookies.h"
#include "net/Url.h"

using namespace sashfold;
using namespace sashfold::net;

namespace {

Url url_of(std::string const& text)
{
    auto const url = parse_url(text);
    CHECK(url.has_value());
    return url ? *url : Url {};
}

std::vector<Header> set_cookie(std::initializer_list<char const*> values)
{
    std::vector<Header> headers;
    headers.push_back({ "Server", "test" });
    for (char const* value : values)
        headers.push_back({ "Set-Cookie", value });
    return headers;
}

}

int main()
{
    // The cookie-date grammar (RFC 6265 §5.1.1) in its natural habitats.
    CHECK_EQ(parse_cookie_date("Wed, 09 Jun 2021 10:18:14 GMT").value_or(0),
        std::int64_t { 1623233894 });
    CHECK_EQ(parse_cookie_date("09-Jun-21 10:18:14").value_or(0), std::int64_t { 1623233894 });
    CHECK_EQ(parse_cookie_date("Jun 9 2021 10:18:14").value_or(0), std::int64_t { 1623233894 });
    CHECK_EQ(parse_cookie_date("Thu, 01 Jan 1970 00:00:00 GMT").value_or(-1), std::int64_t { 0 });
    // Two-digit years split at 70.
    CHECK_EQ(parse_cookie_date("31-Dec-69 23:59:59").value_or(0), std::int64_t { 3155759999 });
    CHECK(parse_cookie_date("01-Jan-70 00:00:00").value_or(-1) == 0);
    CHECK(!parse_cookie_date("Jun 2021 10:18:14").has_value()); // no day
    CHECK(!parse_cookie_date("32 Jun 2021 10:18:14").has_value()); // day out of range
    CHECK(!parse_cookie_date("09 Jun 12345 10:18:14").has_value()); // 5-digit year token

    std::int64_t const now = 1'700'000'000;
    Url const page = url_of("http://example.com/shop/cart");
    Url const secure_page = url_of("https://example.com/shop/cart");

    // Basics: store, send back, and keep to the host.
    {
        CookieJar jar;
        jar.store(page, nullptr, set_cookie({ "sid=abc123" }), now);
        CHECK_EQ(jar.cookie_header(page, nullptr, now), std::string("sid=abc123"));
        CHECK_EQ(jar.cookie_header(url_of("http://other.com/"), nullptr, now), std::string(""));
        // Host-only: the bare domain does not leak to subdomains.
        CHECK_EQ(jar.cookie_header(url_of("http://sub.example.com/"), nullptr, now),
            std::string(""));
    }

    // Domain attribute widens to subdomains; a foreign domain drops the cookie.
    {
        CookieJar jar;
        Url const www = url_of("http://www.example.com/");
        jar.store(www, nullptr, set_cookie({ "wide=1; Domain=example.com", "bad=1; Domain=other.com" }),
            now);
        CHECK_EQ(jar.size(), std::size_t { 1 });
        CHECK_EQ(jar.cookie_header(url_of("http://example.com/"), nullptr, now),
            std::string("wide=1"));
        CHECK_EQ(jar.cookie_header(url_of("http://deep.sub.example.com/"), nullptr, now),
            std::string("wide=1"));
        CHECK_EQ(jar.cookie_header(url_of("http://notexample.com/"), nullptr, now),
            std::string(""));
    }

    // Paths: the default path comes from the request; matching is prefix-wise.
    {
        CookieJar jar;
        jar.store(page, nullptr, set_cookie({ "here=1" }), now); // default path /shop
        jar.store(page, nullptr, set_cookie({ "everywhere=1; Path=/" }), now);
        CHECK_EQ(jar.cookie_header(url_of("http://example.com/shop/checkout"), nullptr, now),
            std::string("here=1; everywhere=1")); // longer path first
        CHECK_EQ(jar.cookie_header(url_of("http://example.com/blog"), nullptr, now),
            std::string("everywhere=1"));
        CHECK_EQ(jar.cookie_header(url_of("http://example.com/shopping"), nullptr, now),
            std::string("everywhere=1")); // /shop does not match /shopping
    }

    // Secure: https-only in both directions.
    {
        CookieJar jar;
        jar.store(page, nullptr, set_cookie({ "s=1; Secure" }), now);
        CHECK_EQ(jar.size(), std::size_t { 0 }); // refused over http
        jar.store(secure_page, nullptr, set_cookie({ "s=1; Secure" }), now);
        CHECK_EQ(jar.size(), std::size_t { 1 });
        CHECK_EQ(jar.cookie_header(secure_page, nullptr, now), std::string("s=1"));
        CHECK_EQ(jar.cookie_header(page, nullptr, now), std::string(""));
    }

    // Expiry: Max-Age wins over Expires; zero or negative deletes.
    {
        CookieJar jar;
        jar.store(page, nullptr,
            set_cookie({ "t=1; Max-Age=50; Expires=Wed, 09 Jun 2021 10:18:14 GMT" }), now);
        CHECK_EQ(jar.cookie_header(page, nullptr, now + 49), std::string("t=1"));
        CHECK_EQ(jar.cookie_header(page, nullptr, now + 51), std::string(""));
        jar.store(page, nullptr, set_cookie({ "t=1; Max-Age=100" }), now);
        jar.store(page, nullptr, set_cookie({ "t=gone; Max-Age=0" }), now);
        CHECK_EQ(jar.cookie_header(page, nullptr, now + 1), std::string(""));
    }

    // Replacement: same (name, domain, path) overwrites in place.
    {
        CookieJar jar;
        jar.store(page, nullptr, set_cookie({ "v=old" }), now);
        jar.store(page, nullptr, set_cookie({ "v=new" }), now);
        CHECK_EQ(jar.size(), std::size_t { 1 });
        CHECK_EQ(jar.cookie_header(page, nullptr, now), std::string("v=new"));
    }

    // The third-party gate: a cross-host first party closes the jar entirely.
    {
        CookieJar jar;
        Url const first_party = url_of("http://news.site/");
        jar.store(page, &first_party, set_cookie({ "track=me" }), now);
        CHECK_EQ(jar.size(), std::size_t { 0 });
        // Same-host first party behaves as first-party.
        Url const same = url_of("http://example.com/");
        jar.store(page, &same, set_cookie({ "ok=1" }), now);
        CHECK_EQ(jar.size(), std::size_t { 1 });
        // Stored first-party cookies do not travel on third-party requests.
        CHECK_EQ(jar.cookie_header(page, &first_party, now), std::string(""));
        CHECK_EQ(jar.cookie_header(page, &same, now), std::string("ok=1"));
    }

    return sashfold::test::report("cookies");
}
