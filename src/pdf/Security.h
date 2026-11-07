#pragma once

// The standard security handler (ISO 32000-2 §7.6.4): an encrypted file's
// key made from its password — the empty user password first, as every
// viewer opens a file that only restricts printing or copying — and the
// strings and streams of each object decrypted with RC4 (revisions 2-4),
// AES-128 (revision 4) or AES-256 (revisions 5 and 6).

#include "pdf/Object.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::pdf {

class SecurityHandler {
public:
    enum class Status { Ok, NeedsPassword, Unsupported };

    // From the trailer's /Encrypt dictionary (resolved) and the first
    // string of its /ID. `password` is tried as the user password, then as
    // the owner's.
    static std::optional<SecurityHandler> open(Dict const& encrypt, std::string const& id0, std::string const& password,
        Status& status);

    enum class Method { None, Rc4, Aes128, Aes256 };

    std::string decrypt_string(Ref owner, std::string const& data) const;
    // `crypt_filter` names the stream's own crypt filter (from a /Crypt
    // filter in its chain), empty for the document's default.
    std::vector<std::uint8_t> decrypt_stream(Ref owner, std::vector<std::uint8_t> const& data,
        std::string const& crypt_filter = {}) const;
    // The other way, for objects an update adds to the file: strings and
    // streams encrypted as the file's own are (AES with a fresh IV).
    std::string encrypt_string(Ref owner, std::string const& data) const;
    std::vector<std::uint8_t> encrypt_stream(Ref owner, std::vector<std::uint8_t> const& data) const;

    bool encrypt_metadata() const { return m_encrypt_metadata; }
    // Whether the password that opened the file was the owner's.
    bool opened_as_owner() const { return m_owner; }
    std::int32_t permissions() const { return m_permissions; }

private:
    std::vector<std::uint8_t> decrypt(Method method, Ref owner, std::vector<std::uint8_t> const& data) const;
    std::vector<std::uint8_t> encrypt(Method method, Ref owner, std::vector<std::uint8_t> const& data) const;
    std::vector<std::uint8_t> object_key(Method method, Ref owner) const;
    Method method_of(std::string const& filter_name) const;

    std::vector<std::uint8_t> m_key;
    Method m_string_method = Method::None;
    Method m_stream_method = Method::None;
    Dict m_crypt_filters;
    bool m_encrypt_metadata = true;
    bool m_owner = false;
    std::int32_t m_permissions = -1;
};

// Algorithm 2.B (revision 6): the hash of a password with a salt and, for
// the owner, the user key string. Exposed for its test vectors.
std::vector<std::uint8_t> pdf_hash_r6(std::string const& password, std::vector<std::uint8_t> const& salt,
    std::vector<std::uint8_t> const& user_data);

}
