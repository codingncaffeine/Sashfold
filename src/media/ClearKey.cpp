#include "media/ClearKey.h"

#include "core/Json.h"

#include <algorithm>
#include <cstring>

namespace sashfold::media {

namespace {

constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int value_of(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-')
        return 62;
    if (c == '_')
        return 63;
    return -1;
}

std::uint32_t be32(std::uint8_t const* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// The common system ID of ISO/IEC 23001-7 §8.1.3 (the W3C "cenc" format's).
constexpr std::uint8_t common_system_id[16] = { 0x10, 0x77, 0xEF, 0xEC, 0xC0, 0xB2, 0x4D, 0x02, 0xAC, 0xE3, 0x3C, 0x1E, 0x52, 0xE2,
    0xFB, 0x4B };

std::optional<std::vector<Bytes>> key_ids_from_pssh(std::span<std::uint8_t const> data)
{
    // A run of whole 'pssh' boxes and nothing else (§8.1.1).
    std::vector<Bytes> ids;
    std::size_t at = 0;
    while (at < data.size()) {
        if (data.size() - at < 32)
            return std::nullopt;
        std::uint32_t const size = be32(data.data() + at);
        if (size < 32 || size > data.size() - at || std::memcmp(data.data() + at + 4, "pssh", 4) != 0)
            return std::nullopt;
        std::span<std::uint8_t const> const box = data.subspan(at, size);
        std::uint8_t const version = box[8];
        if (version > 1)
            return std::nullopt;
        std::size_t p = 28;
        std::vector<Bytes> box_ids;
        if (version == 1) {
            std::uint32_t const count = be32(box.data() + p);
            p += 4;
            if (count > (box.size() - p) / 16)
                return std::nullopt;
            for (std::uint32_t i = 0; i < count; ++i, p += 16)
                box_ids.emplace_back(box.begin() + static_cast<std::ptrdiff_t>(p), box.begin() + static_cast<std::ptrdiff_t>(p + 16));
        }
        if (p + 4 > box.size() || be32(box.data() + p) != box.size() - p - 4)
            return std::nullopt;
        if (std::memcmp(box.data() + 12, common_system_id, 16) == 0) {
            for (Bytes& id : box_ids) {
                if (std::find(ids.begin(), ids.end(), id) == ids.end())
                    ids.push_back(std::move(id));
            }
        }
        at += size;
    }
    return ids;
}

std::optional<std::vector<Bytes>> key_ids_from_json(std::span<std::uint8_t const> data)
{
    std::optional<JsonValue> const json = JsonValue::parse(std::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
    if (!json || !json->is_object())
        return std::nullopt;
    JsonValue const* const kids = json->get("kids");
    if (!kids || !kids->is_array() || kids->as_array().empty())
        return std::nullopt;
    std::vector<Bytes> ids;
    for (JsonValue const& kid : kids->as_array()) {
        if (!kid.is_string())
            return std::nullopt;
        std::optional<Bytes> id = base64url_decode(kid.as_string());
        if (!id || id->empty() || id->size() > max_key_id_size)
            return std::nullopt;
        ids.push_back(std::move(*id));
    }
    return ids;
}

std::string json_string(std::string_view text)
{
    // Only base64url and the session type are written: nothing to escape.
    return "\"" + std::string(text) + "\"";
}

}

std::string base64url_encode(std::span<std::uint8_t const> bytes)
{
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        std::uint32_t const v = (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (bytes.size() - i == 1) {
        std::uint32_t const v = std::uint32_t(bytes[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
    } else if (bytes.size() - i == 2) {
        std::uint32_t const v = (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
    }
    return out;
}

std::optional<Bytes> base64url_decode(std::string_view text)
{
    if (text.size() % 4 == 1)
        return std::nullopt;
    Bytes out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char const c : text) {
        int const v = value_of(c);
        if (v < 0)
            return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(buffer >> bits));
        }
    }
    // The bits left over must be zero (RFC 4648 §3.5, canonical encoding).
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0)
        return std::nullopt;
    return out;
}

bool init_data_type_supported(std::string_view type)
{
    return type == "cenc" || type == "keyids" || type == "webm";
}

std::optional<std::vector<Bytes>> key_ids_from_init_data(std::string_view type, std::span<std::uint8_t const> data)
{
    if (data.empty() || data.size() > max_init_data_size)
        return std::nullopt;
    if (type == "cenc")
        return key_ids_from_pssh(data);
    if (type == "keyids")
        return key_ids_from_json(data);
    if (type == "webm") {
        if (data.size() > max_key_id_size)
            return std::nullopt;
        return std::vector<Bytes> { Bytes(data.begin(), data.end()) };
    }
    return std::nullopt;
}

std::string license_request(std::vector<Bytes> const& key_ids, std::string_view session_type)
{
    std::string out = "{\"kids\":[";
    for (std::size_t i = 0; i < key_ids.size(); ++i) {
        if (i > 0)
            out += ",";
        out += json_string(base64url_encode(key_ids[i]));
    }
    out += "],\"type\":" + json_string(session_type) + "}";
    return out;
}

std::optional<License> parse_license(std::span<std::uint8_t const> response)
{
    if (response.empty() || response.size() > max_license_size)
        return std::nullopt;
    // A license is ASCII JSON: anything else is not one.
    for (std::uint8_t const byte : response) {
        if (byte >= 0x80)
            return std::nullopt;
    }
    std::optional<JsonValue> const json = JsonValue::parse(std::string_view(reinterpret_cast<char const*>(response.data()), response.size()));
    if (!json || !json->is_object())
        return std::nullopt;
    JsonValue const* const keys = json->get("keys");
    if (!keys || !keys->is_array() || keys->as_array().empty())
        return std::nullopt;
    // A "type" says which kind of session the license is for: Clear Key
    // licenses here are temporary ones.
    if (JsonValue const* const type = json->get("type"); type && (!type->is_string() || type->as_string() != "temporary"))
        return std::nullopt;
    License license;
    for (JsonValue const& entry : keys->as_array()) {
        if (!entry.is_object())
            return std::nullopt;
        JsonValue const* const kty = entry.get("kty");
        JsonValue const* const k = entry.get("k");
        JsonValue const* const kid = entry.get("kid");
        if (!kty || !kty->is_string() || kty->as_string() != "oct" || !k || !k->is_string() || !kid || !kid->is_string())
            return std::nullopt;
        std::optional<Bytes> const key = base64url_decode(k->as_string());
        std::optional<Bytes> id = base64url_decode(kid->as_string());
        if (!key || key->size() != 16 || !id || id->empty() || id->size() > max_key_id_size)
            return std::nullopt;
        ContentKey content {};
        std::copy(key->begin(), key->end(), content.begin());
        license.keys.emplace_back(std::move(*id), content);
    }
    return license;
}

void KeyStore::add(std::uint64_t session, Bytes key_id, ContentKey key)
{
    std::lock_guard const lock(m_mutex);
    for (Entry& entry : m_entries) {
        if (entry.session == session && entry.key_id == key_id) {
            entry.key = key;
            ++m_generation;
            return;
        }
    }
    m_entries.push_back({ session, std::move(key_id), key });
    ++m_generation;
}

void KeyStore::remove_session(std::uint64_t session)
{
    std::lock_guard const lock(m_mutex);
    std::erase_if(m_entries, [session](Entry const& entry) { return entry.session == session; });
    ++m_generation;
}

std::optional<ContentKey> KeyStore::find(std::span<std::uint8_t const> key_id) const
{
    std::lock_guard const lock(m_mutex);
    // The newest key for the ID: a later license stands over an earlier one.
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (std::equal(it->key_id.begin(), it->key_id.end(), key_id.begin(), key_id.end()))
            return it->key;
    }
    return std::nullopt;
}

std::uint64_t KeyStore::generation() const
{
    std::lock_guard const lock(m_mutex);
    return m_generation;
}

}
