#pragma once

// The cookie jar (RFC 6265): Set-Cookie parsing including the cookie-date
// grammar, domain and path matching, expiry, and per-request assembly — with
// the policy built in: THIRD-PARTY COOKIES ARE BLOCKED BY DEFAULT.
// A request whose host differs from the first-party (top-level document)
// host neither sends nor stores cookies. That is stricter than the spec —
// site-level grouping needs the public-suffix list, which arrives with the
// content-blocking era — and honest about it.
//
// The jar is session-only for now: nothing touches disk until the profile
// story decides what persistence should mean.

#include "net/Http.h"
#include "net/Url.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::net {

struct Cookie {
    std::string name;
    std::string value;
    std::string domain; // canonical lowercase, no leading dot
    std::string path;
    bool host_only = true;
    bool secure = false;
    bool http_only = false;
    std::optional<std::int64_t> expires; // unix seconds; absent = session cookie
    std::int64_t created = 0; // insertion order for the §5.4 sort
};

class CookieJar {
public:
    // Stores every Set-Cookie header of a response received for url. A
    // null first_party means the request IS the top-level navigation.
    void store(Url const& url, Url const* first_party,
        std::vector<Header> const& response_headers, std::int64_t now);

    // The Cookie header value for a request ("" = send no header).
    std::string cookie_header(Url const& url, Url const* first_party, std::int64_t now) const;

    std::size_t size() const { return m_cookies.size(); }

private:
    std::vector<Cookie> m_cookies;
    std::int64_t m_counter = 0; // creation tiebreaker
};

// Exposed for tests: RFC 6265 §5.1.1 cookie-date -> unix seconds.
std::optional<std::int64_t> parse_cookie_date(std::string_view text);

}
