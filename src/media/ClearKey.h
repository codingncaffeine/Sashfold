#pragma once

// Clear Key, the key system every Encrypted Media Extensions user agent
// carries (EME §9.1, "org.w3.clearkey"): the keys travel in the clear as
// JSON Web Keys, and the content decryption module is no more than a table
// of keys by key ID. Here are its formats — the initialization data it
// reads key IDs from (the Initialization Data Format Registry's "cenc",
// "keyids" and "webm"), its license request and its license — and the
// table, which the page's thread writes and the decoding threads read.
//
// Only temporary sessions are offered, as every shipping browser offers
// for Clear Key: nothing a session holds outlives it, so there is no
// license release message and nothing is ever stored.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::media {

using Bytes = std::vector<std::uint8_t>;
using ContentKey = std::array<std::uint8_t, 16>;

// base64url without padding (RFC 4648 §5), as JSON Web Keys write their
// bytes. Decoding refuses padding and anything outside the alphabet.
std::string base64url_encode(std::span<std::uint8_t const> bytes);
std::optional<Bytes> base64url_decode(std::string_view text);

// The longest initialization data and license EME takes (§6.6.2, §6.6.3:
// 64 KiB), and the longest key ID Clear Key takes.
constexpr std::size_t max_init_data_size = 64 * 1024;
constexpr std::size_t max_license_size = 64 * 1024;
constexpr std::size_t max_key_id_size = 512;

// The key IDs a Clear Key license request names for initialization data of
// a type (EME §9.1.3 and the registry): "cenc", a run of whole 'pssh'
// boxes, of which the version 1 boxes of the common system ID carry key
// IDs (ISO/IEC 23001-7 §8.1, W3C "cenc" format); "keyids", a JSON object
// whose "kids" are base64url key IDs; "webm", one key ID. Nothing when
// the data is not valid for its type — and for a type Clear Key does not
// read. The list may be empty ("cenc" without the common system's box).
std::optional<std::vector<Bytes>> key_ids_from_init_data(std::string_view type, std::span<std::uint8_t const> data);
bool init_data_type_supported(std::string_view type);

// The license request message (§9.1.3): {"kids":[...],"type":"temporary"}.
std::string license_request(std::vector<Bytes> const& key_ids, std::string_view session_type);

// A license (§9.1.4): a JSON Web Key Set of symmetric keys
// ({"keys":[{"kty":"oct","k":...,"kid":...}],"type":...}). Nothing when it
// is not one: not ASCII, not JSON, no keys, a key not of type "oct", a key
// ID that does not decode, a key that is not 16 bytes.
struct License {
    std::vector<std::pair<Bytes, ContentKey>> keys;
};
std::optional<License> parse_license(std::span<std::uint8_t const> response);

// The keys of one MediaKeys object's sessions. Written on the page's
// thread as sessions are updated and closed; read from any thread by
// whoever decrypts. Each key is its session's: closing a session takes its
// keys out (a key ID two sessions hold stays while either does).
class KeyStore {
public:
    void add(std::uint64_t session, Bytes key_id, ContentKey key);
    void remove_session(std::uint64_t session);
    std::optional<ContentKey> find(std::span<std::uint8_t const> key_id) const;
    // One more at every change: a reader that waits for a key looks again.
    std::uint64_t generation() const;

private:
    struct Entry {
        std::uint64_t session = 0;
        Bytes key_id;
        ContentKey key {};
    };
    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
    std::uint64_t m_generation = 0;
};

}
