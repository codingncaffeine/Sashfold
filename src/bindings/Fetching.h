#pragma once

// The fetching core that fetch() and XMLHttpRequest share (Fetch.cpp): the
// request as the page made it, with its mode and credentials, and what came
// back once the CORS decision was made — the response the page may see, an
// opaque one, or a network error with the reason for the console.

#include "bindings/Realm.h"
#include "net/Http.h"
#include "net/Url.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::bindings {

enum class FetchMode : std::uint8_t { SameOrigin, Cors, NoCors };
enum class FetchCredentials : std::uint8_t { Omit, SameOrigin, Include };
enum class FetchRedirect : std::uint8_t { Follow, Error, Manual };

struct PageRequest {
    net::Url url;
    std::string method = "GET";
    std::vector<net::Header> headers; // as the page set them, forbidden names already kept out
    std::optional<std::vector<std::uint8_t>> body;
    std::string body_type; // the Content-Type the body's source implies, when the page set none
    FetchMode mode = FetchMode::Cors;
    FetchCredentials credentials = FetchCredentials::SameOrigin;
    FetchRedirect redirect = FetchRedirect::Follow;
    std::string destination; // "script" for a module script; empty for the page's own fetches
    // For the page's Content Security Policy: the root <script>'s nonce
    // when the request is a module graph's, and whether the parser
    // inserted that element.
    std::string nonce;
    bool parser_inserted = true;
};

struct FetchOutcome {
    bool ok = false; // false = a network error; `error` says why
    std::string error;
    std::string type = "basic"; // basic, cors, opaque, opaqueredirect
    int status = 0;
    std::string status_text;
    std::vector<net::Header> headers; // what the page may see
    std::vector<std::uint8_t> body;
    net::Url url; // where the response came from; empty for an opaque one
    bool redirected = false;
};

// Carries the request out through the host's loader, synchronously: the
// same-origin check, the preflight when one is due, the request itself,
// the CORS check on the answer. Runs no script.
FetchOutcome perform_fetch(Realm::Internals&, PageRequest const&);
// The MIME essence of a Content-Type value: the type/subtype, lowercased.
std::string mime_essence(std::string_view value);

FetchMode fetch_mode_of(std::string_view mode);
FetchCredentials fetch_credentials_of(std::string_view credentials);

}
