#pragma once

// The WHATWG URL parser — a security component: it defines origin identity.
// The basic URL parser state machine, host parsing (domains, IPv4, IPv6,
// opaque hosts), percent-encoding, and the serializers, from the live spec.
//
// Honest gap, graded by the vendored WPT urltestdata suite: domain-to-ASCII
// currently lowercases ASCII and Punycode-encodes non-ASCII labels directly,
// without the full UTS46 mapping table (that arrives with tools/gen-unicode,
// plan §5.2). The fixture score is ratcheted in CI and climbs from here.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::net {

struct Url {
    enum class HostKind : std::uint8_t {
        None, // no host (e.g. opaque paths, most file: URLs have Empty instead)
        Empty, // present but empty (file://, or http:// pre-host errors)
        Domain,
        Ipv4,
        Ipv6,
        Opaque, // non-special schemes
    };

    std::string scheme; // lowercase, without the ':'
    std::string username;
    std::string password;
    HostKind host_kind = HostKind::None;
    std::string host; // serialized; IPv6 stored without brackets
    std::optional<std::uint16_t> port; // absent when default or none
    bool has_opaque_path = false;
    std::vector<std::string> path; // opaque path: one joined element
    std::optional<std::string> query; // without '?'
    std::optional<std::string> fragment; // without '#'

    bool is_special() const;
    bool includes_credentials() const { return !username.empty() || !password.empty(); }
    bool has_host() const { return host_kind != HostKind::None; }

    std::string serialize(bool exclude_fragment = false) const;
    std::string serialize_host() const; // brackets around IPv6
    std::string serialize_path() const;
    std::string serialize_origin() const; // "null" for opaque origins

    // The WPT/CSSOM-shaped component views.
    std::string protocol() const { return scheme + ":"; }
    std::string host_with_port() const;
    std::string port_string() const;
    std::string search() const { return query && !query->empty() ? "?" + *query : ""; }
    std::string hash() const { return fragment && !fragment->empty() ? "#" + *fragment : ""; }
};

// The basic URL parser (no state override, no encoding override).
// nullopt == failure.
std::optional<Url> parse_url(std::string_view input, Url const* base = nullptr);

// Exposed for tests: RFC 3492 Punycode encoding of one label (no "xn--").
std::optional<std::string> punycode_encode(std::u32string_view label);

}
