#pragma once
// The client's own steps of a WebAuthn ceremony that are not the page's
// script interface and not a key's protocol: the collected client data
// and its JSON serialization (WebAuthn Level 3 §5.8.1, §5.8.1.1), the RP
// ID's check against the caller's origin (§5.1.3 step 8, §5.1.4 step 8,
// with HTML's "is a registrable domain suffix of or is equal to"), and the
// ceremony's timeout as the client adjusts the page's (§15.1).
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sashfold::auth {

// CollectedClientData (§5.8.1) as the client builds it for one ceremony.
struct CollectedClientData {
    std::string type; // "webauthn.create" or "webauthn.get"
    std::string challenge; // base64url of the page's challenge, no padding
    std::string origin; // the caller's origin, serialized
    bool cross_origin = false; // the caller is not same-origin with its ancestors
    std::optional<std::string> top_origin; // the top-level origin, when cross_origin
};

// §5.8.1.1 "Serialization": the members in the specification's order, each
// string through CCDToString, nothing between tokens. The bytes a relying
// party hashes and parses, so they are fixed to the byte.
std::string serialize_client_data(CollectedClientData const& data);

// CCDToString (§5.8.1.1): a JSON string whose escapes are exactly
// \" \\ and \uXXXX (four lower-case hex digits) for every other code
// point below U+0020; everything else is copied as UTF-8.
std::string ccd_to_string(std::string_view utf8);

// The client data's base64url (RFC 4648 §5, no padding), the challenge's
// form in it.
std::string base64url(std::span<std::uint8_t const> bytes);

// HTML §7.1.1.2.1 "is a registrable domain suffix of or is equal to":
// `host_suffix` is a host, equal to `original_host`, or — both domains —
// a dot-separated suffix of it that is no public suffix and no shorter
// than the original host's registrable domain. An empty or unparsable
// suffix (a port, a space, an IP address not equal to the original) is
// none.
bool is_registrable_suffix_or_equal(std::string_view host_suffix, std::string_view original_host);

// Whether `rp_id` may be used by a document of `origin` (scheme, host): a
// secure origin — https, or a host of localhost — whose effective domain
// it is, or of which it is a registrable domain suffix (§5.1.3 step 8).
bool rp_id_allowed(std::string_view rp_id, std::string_view scheme, std::string_view host);

// The page's `timeout` brought into the client's range, or the default
// when it gave none (§15.1 "Recommended Range and Default for a WebAuthn
// Ceremony Timeout": 300000 to 600000 ms, 300000 when unsaid).
std::chrono::milliseconds ceremony_timeout(std::optional<std::uint32_t> page_value);

}
