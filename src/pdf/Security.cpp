#include "pdf/Security.h"

#include "crypto/Aes.h"
#include "crypto/Md5.h"
#include "crypto/Sha2.h"

#include <algorithm>
#include <array>
#include <span>

namespace sashfold::pdf {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::array<std::uint8_t, 32> k_padding = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A,
};

Bytes bytes_of(std::string const& s)
{
    return Bytes(s.begin(), s.end());
}

Bytes padded(std::string const& password)
{
    Bytes out(password.begin(), password.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(32, password.size())));
    out.insert(out.end(), k_padding.begin(), k_padding.begin() + static_cast<std::ptrdiff_t>(32 - out.size()));
    return out;
}

void append(Bytes& out, Bytes const& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

Bytes md5(Bytes const& data)
{
    auto const digest = crypto::Md5::hash(data);
    return Bytes(digest.begin(), digest.end());
}

// AES-CBC without padding; `encrypt` false decrypts.
Bytes aes_cbc_raw(Bytes const& key, Bytes const& iv, Bytes const& data, bool encrypt)
{
    std::optional<crypto::Aes> const aes = crypto::Aes::with_key(key);
    if (!aes || iv.size() != 16)
        return {};
    Bytes out;
    out.reserve(data.size());
    crypto::AesBlock chain {};
    std::copy(iv.begin(), iv.end(), chain.begin());
    for (std::size_t at = 0; at + 16 <= data.size(); at += 16) {
        crypto::AesBlock block {};
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at), 16, block.begin());
        if (encrypt) {
            for (std::size_t i = 0; i < 16; ++i)
                block[i] ^= chain[i];
            chain = aes->encrypt(block);
            out.insert(out.end(), chain.begin(), chain.end());
        } else {
            crypto::AesBlock plain = aes->decrypt(block);
            for (std::size_t i = 0; i < 16; ++i)
                plain[i] ^= chain[i];
            chain = block;
            out.insert(out.end(), plain.begin(), plain.end());
        }
    }
    return out;
}

// Algorithm 2: the file key from a (user) password, revisions 2-4.
Bytes key_r4(std::string const& password, Dict const& encrypt, std::string const& id0, int revision, std::size_t length,
    bool encrypt_metadata)
{
    Bytes input = padded(password);
    append(input, bytes_of(encrypt.get("O").as_string().substr(0, 32)));
    auto const p = static_cast<std::uint32_t>(encrypt.get("P").as_int());
    for (int i = 0; i < 4; ++i)
        input.push_back(static_cast<std::uint8_t>(p >> (8 * i)));
    append(input, bytes_of(id0));
    if (revision >= 4 && !encrypt_metadata)
        input.insert(input.end(), 4, 0xFF);
    Bytes key = md5(input);
    if (revision >= 3) {
        for (int i = 0; i < 50; ++i)
            key = md5(Bytes(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(length)));
    }
    key.resize(length);
    return key;
}

// Algorithms 4/5 then 6: whether the key opens the user entry.
bool user_key_matches(Bytes const& key, Dict const& encrypt, std::string const& id0, int revision)
{
    Bytes const u = bytes_of(encrypt.get("U").as_string());
    if (revision == 2) {
        Bytes const expected = crypto::rc4(key, Bytes(k_padding.begin(), k_padding.end()));
        return u.size() >= 32 && std::equal(expected.begin(), expected.end(), u.begin());
    }
    Bytes input(k_padding.begin(), k_padding.end());
    append(input, bytes_of(id0));
    Bytes value = crypto::rc4(key, md5(input));
    for (int i = 1; i <= 19; ++i) {
        Bytes round_key = key;
        for (std::uint8_t& b : round_key)
            b = static_cast<std::uint8_t>(b ^ i);
        value = crypto::rc4(round_key, value);
    }
    return u.size() >= 16 && std::equal(value.begin(), value.begin() + 16, u.begin());
}

// Algorithm 7: the user password hidden in /O under the owner password.
std::string user_password_from_owner(std::string const& owner, Dict const& encrypt, int revision, std::size_t length)
{
    Bytes key = md5(padded(owner));
    if (revision >= 3) {
        for (int i = 0; i < 50; ++i)
            key = md5(key);
    }
    key.resize(length);
    Bytes value = bytes_of(encrypt.get("O").as_string().substr(0, 32));
    if (revision == 2) {
        value = crypto::rc4(key, value);
    } else {
        for (int i = 19; i >= 0; --i) {
            Bytes round_key = key;
            for (std::uint8_t& b : round_key)
                b = static_cast<std::uint8_t>(b ^ i);
            value = crypto::rc4(round_key, value);
        }
    }
    return std::string(value.begin(), value.end());
}

Bytes sha256(Bytes const& data)
{
    return crypto::hash_with(crypto::HashId::Sha256, data);
}

} // namespace

Bytes pdf_hash_r6(std::string const& password, Bytes const& salt, Bytes const& user_data)
{
    Bytes const pw = bytes_of(password.substr(0, 127));
    Bytes input = pw;
    append(input, salt);
    append(input, user_data);
    Bytes k = sha256(input);
    for (int round = 0;; ++round) {
        Bytes one = pw;
        append(one, k);
        append(one, user_data);
        Bytes k1;
        k1.reserve(one.size() * 64);
        for (int i = 0; i < 64; ++i)
            append(k1, one);
        Bytes const e = aes_cbc_raw(Bytes(k.begin(), k.begin() + 16), Bytes(k.begin() + 16, k.begin() + 32), k1, true);
        if (e.size() < 16)
            return {};
        unsigned sum = 0;
        for (std::size_t i = 0; i < 16; ++i)
            sum += e[i];
        crypto::HashId const id = sum % 3 == 0 ? crypto::HashId::Sha256
            : sum % 3 == 1                    ? crypto::HashId::Sha384
                                              : crypto::HashId::Sha512;
        k = crypto::hash_with(id, e);
        if (round >= 63 && e.back() <= round + 1 - 32)
            break;
    }
    k.resize(32);
    return k;
}

std::optional<SecurityHandler> SecurityHandler::open(Dict const& encrypt, std::string const& id0, std::string const& password,
    Status& status)
{
    status = Status::Unsupported;
    if (encrypt.get("Filter").as_name() != "Standard")
        return std::nullopt;
    int const v = static_cast<int>(encrypt.get("V").as_int(0));
    int const revision = static_cast<int>(encrypt.get("R").as_int(0));
    SecurityHandler handler;
    handler.m_permissions = static_cast<std::int32_t>(encrypt.get("P").as_int(-1));
    if (encrypt.get("EncryptMetadata").is_bool())
        handler.m_encrypt_metadata = encrypt.get("EncryptMetadata").as_bool();

    if (v == 4 || v == 5) {
        handler.m_crypt_filters = encrypt.get("CF").as_dict();
        handler.m_stream_method = handler.method_of(encrypt.get("StmF").is_name() ? encrypt.get("StmF").as_name() : "Identity");
        handler.m_string_method = handler.method_of(encrypt.get("StrF").is_name() ? encrypt.get("StrF").as_name() : "Identity");
    } else if (v == 1 || v == 2 || v == 3) {
        handler.m_stream_method = Method::Rc4;
        handler.m_string_method = Method::Rc4;
    } else {
        return std::nullopt;
    }

    if (revision >= 5) {
        // AES-256: the key is unwrapped from /UE or /OE.
        Bytes const u = bytes_of(encrypt.get("U").as_string());
        Bytes const o = bytes_of(encrypt.get("O").as_string());
        if (u.size() < 48 || o.size() < 48)
            return std::nullopt;
        auto const hash = [&](std::string const& pw, Bytes const& salt, Bytes const& data) {
            if (revision == 5) {
                Bytes input = bytes_of(pw.substr(0, 127));
                append(input, salt);
                append(input, data);
                return sha256(input);
            }
            return pdf_hash_r6(pw, salt, data);
        };
        Bytes const u48(u.begin(), u.begin() + 48);
        Bytes const zero_iv(16, 0);
        for (bool const as_owner : { false, true }) {
            Bytes const& entry = as_owner ? o : u;
            Bytes const data = as_owner ? u48 : Bytes {};
            Bytes const check = hash(password, Bytes(entry.begin() + 32, entry.begin() + 40), data);
            if (!std::equal(check.begin(), check.end(), entry.begin()) || check.size() != 32)
                continue;
            Bytes const intermediate = hash(password, Bytes(entry.begin() + 40, entry.begin() + 48), data);
            Bytes const wrapped = bytes_of(encrypt.get(as_owner ? "OE" : "UE").as_string());
            if (wrapped.size() < 32)
                return std::nullopt;
            handler.m_key = aes_cbc_raw(intermediate, zero_iv, Bytes(wrapped.begin(), wrapped.begin() + 32), false);
            handler.m_owner = as_owner;
            status = Status::Ok;
            return handler;
        }
        status = Status::NeedsPassword;
        return std::nullopt;
    }

    std::size_t length = 5;
    if (revision >= 3) {
        std::int64_t bits = encrypt.get("Length").as_int(40);
        if (v == 4) {
            // AESV2 is 128 bits whatever /Length says.
            bits = 128;
        }
        length = static_cast<std::size_t>(std::clamp<std::int64_t>(bits / 8, 5, 16));
    }
    Bytes key = key_r4(password, encrypt, id0, revision, length, handler.m_encrypt_metadata);
    if (user_key_matches(key, encrypt, id0, revision)) {
        handler.m_key = std::move(key);
        status = Status::Ok;
        return handler;
    }
    std::string const user = user_password_from_owner(password, encrypt, revision, length);
    key = key_r4(user, encrypt, id0, revision, length, handler.m_encrypt_metadata);
    if (user_key_matches(key, encrypt, id0, revision)) {
        handler.m_key = std::move(key);
        handler.m_owner = true;
        status = Status::Ok;
        return handler;
    }
    status = Status::NeedsPassword;
    return std::nullopt;
}

SecurityHandler::Method SecurityHandler::method_of(std::string const& filter_name) const
{
    if (filter_name == "Identity")
        return Method::None;
    Dict const& filter = m_crypt_filters.get(filter_name).as_dict();
    std::string const& cfm = filter.get("CFM").as_name();
    if (cfm == "V2")
        return Method::Rc4;
    if (cfm == "AESV2")
        return Method::Aes128;
    if (cfm == "AESV3")
        return Method::Aes256;
    return Method::None;
}

Bytes SecurityHandler::decrypt(Method method, Ref owner, Bytes const& data) const
{
    if (method == Method::None)
        return data;
    Bytes key = m_key;
    if (method != Method::Aes256) {
        // Algorithm 1: the object's own key.
        Bytes input = m_key;
        for (int i = 0; i < 3; ++i)
            input.push_back(static_cast<std::uint8_t>(owner.num >> (8 * i)));
        for (int i = 0; i < 2; ++i)
            input.push_back(static_cast<std::uint8_t>(owner.gen >> (8 * i)));
        if (method == Method::Aes128) {
            for (char const c : { 's', 'A', 'l', 'T' })
                input.push_back(static_cast<std::uint8_t>(c));
        }
        key = md5(input);
        key.resize(std::min<std::size_t>(m_key.size() + 5, 16));
    }
    if (method == Method::Rc4)
        return crypto::rc4(key, data);
    // AES: a 16-byte IV, then CBC, then PKCS#7 padding (dropped leniently).
    if (data.size() < 32)
        return {};
    Bytes const iv(data.begin(), data.begin() + 16);
    Bytes body(data.begin() + 16, data.begin() + 16 + static_cast<std::ptrdiff_t>((data.size() - 16) / 16 * 16));
    Bytes out = aes_cbc_raw(key, iv, body, false);
    if (!out.empty()) {
        std::uint8_t const pad = out.back();
        if (pad >= 1 && pad <= 16 && pad <= out.size())
            out.resize(out.size() - pad);
    }
    return out;
}

std::string SecurityHandler::decrypt_string(Ref owner, std::string const& data) const
{
    Bytes const out = decrypt(m_string_method, owner, bytes_of(data));
    return std::string(out.begin(), out.end());
}

Bytes SecurityHandler::decrypt_stream(Ref owner, Bytes const& data, std::string const& crypt_filter) const
{
    Method const method = crypt_filter.empty() ? m_stream_method : method_of(crypt_filter);
    return decrypt(method, owner, data);
}

}
