#pragma once

// HTTP/1.1, ours: request serialization, response parsing (status line,
// headers, Content-Length / chunked / read-to-close framing), content
// decoding (gzip, deflate in both zlib and raw spellings), redirect
// following, and persistent connections through a session's ConnectionPool
// — all behind fetch(), the one choke point every load passes through.
// https runs over the platform Tls seam where a backend exists.

#include "net/Url.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::net {

struct Header {
    std::string name; // stored as received; lookups are case-insensitive
    std::string value;
};

struct FetchResponse {
    int status = 0;
    std::string status_text;
    std::vector<Header> headers;
    std::vector<std::uint8_t> body; // after content decoding
    Url final_url; // where the redirect chain landed
    bool from_cache = false; // served by the MemoryCache, no network touched
    bool redirected = false; // at least one redirect was followed on the way
};

// A request a page's script makes (fetch, XMLHttpRequest), as the loader
// and the host hook carry it: the method, the headers the script set, the
// body, whether cookies may flow, and whether a redirect is followed or
// handed back as the response.
struct ResourceRequest {
    std::string method = "GET";
    std::vector<Header> headers;
    std::vector<std::uint8_t> body;
    bool credentials = true;
    bool follow_redirects = true;
};

class ConnectionPool;
class CookieJar;
class MemoryCache;

struct FetchOptions {
    int max_redirects = 20;
    std::size_t max_body = 64u * 1024u * 1024u;
    // Cookies flow only when a jar is given. first_party is the top-level
    // document URL; when it names another host, the jar stays closed in both
    // directions (third-party cookies blocked by default). Null
    // first_party marks the request itself as the navigation.
    CookieJar* cookie_jar = nullptr;
    Url const* first_party = nullptr;
    // Referer header value, already policy-shaped by the caller (the shell
    // applies strict-origin-when-cross-origin); empty sends none.
    std::string referrer;
    // The session cache: consulted before every connection
    // and fed by every cacheable 200. Null means no caching at all.
    MemoryCache* cache = nullptr;
    // The session's persistent connections: a request goes out on a pooled
    // connection to its origin when one is idle, and the connection it used
    // is kept when the response leaves it reusable. Null means one
    // connection per request, closed after the response.
    ConnectionPool* pool = nullptr;
    // The request beyond a plain GET: the method, headers the caller adds
    // (each replacing the default of its name; Host, Content-Length,
    // Connection, Accept-Encoding and Cookie stay the exchange's own), and
    // a body sent with its length. A redirect keeps the method and body on
    // 307 and 308 and turns a POST into a GET on 301, 302 and 303, as
    // browsers do; with follow_redirects false a 3xx comes back as the
    // response itself. Only a GET consults or feeds the cache.
    std::string method = "GET";
    std::vector<Header> headers;
    std::vector<std::uint8_t> body;
    bool follow_redirects = true;
};

struct FetchResult {
    std::optional<FetchResponse> response;
    std::string error; // set when response is empty
};

// The choke point.
FetchResult fetch(Url const& url, FetchOptions const& options = {});

std::string const* find_header(std::vector<Header> const& headers, std::string_view name);

// The UA policy token: compat-shaped, honest suffix.
std::string_view user_agent();

// Exposed for tests: parses one HTTP/1.1 response from a read callback
// (>0 bytes, 0 close, <0 error), applying framing but not content decoding.
// Interim 1xx responses are skipped; 204 and 304 carry no body.
struct RawResponse {
    int status = 0;
    std::string status_text;
    std::vector<Header> headers;
    std::vector<std::uint8_t> body;
    // The connection can carry another request: the body was delimited
    // (not read to close), nothing followed it, and neither the version nor
    // a Connection header asked for a close.
    bool keep_alive = false;
};
std::optional<RawResponse> read_response(
    std::function<std::ptrdiff_t(std::uint8_t*, std::size_t)> const& read,
    std::size_t max_body, bool head = false); // head: a HEAD's response carries no body

// Exposed for tests: content decoding per Content-Encoding.
std::optional<std::vector<std::uint8_t>> decode_content(std::string_view encoding,
    std::vector<std::uint8_t> const& body, std::size_t max_output);

}
