#pragma once

// HTTP/1.1, ours (M3): request serialization, response parsing (status line,
// headers, Content-Length / chunked / read-to-close framing), content
// decoding (gzip, deflate in both zlib and raw spellings), and redirect
// following — all behind fetch(), the one choke point every load will pass
// through. Plain http for now; https arrives when the Tls seam lands and
// until then returns a clear error.

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
};

struct FetchOptions {
    int max_redirects = 20;
    std::size_t max_body = 64u * 1024u * 1024u;
};

struct FetchResult {
    std::optional<FetchResponse> response;
    std::string error; // set when response is empty
};

// The choke point.
FetchResult fetch(Url const& url, FetchOptions const& options = {});

std::string const* find_header(std::vector<Header> const& headers, std::string_view name);

// The UA policy token (plan M3): compat-shaped, honest suffix.
std::string_view user_agent();

// Exposed for tests: parses one HTTP/1.1 response from a read callback
// (>0 bytes, 0 close, <0 error), applying framing but not content decoding.
struct RawResponse {
    int status = 0;
    std::string status_text;
    std::vector<Header> headers;
    std::vector<std::uint8_t> body;
};
std::optional<RawResponse> read_response(
    std::function<std::ptrdiff_t(std::uint8_t*, std::size_t)> const& read,
    std::size_t max_body);

// Exposed for tests: content decoding per Content-Encoding.
std::optional<std::vector<std::uint8_t>> decode_content(std::string_view encoding,
    std::vector<std::uint8_t> const& body, std::size_t max_output);

}
