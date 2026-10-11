#include "net/tls/Validate.h"

#include "crypto/Sha2.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace sashfold::tls {

namespace {

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A hash of SHA-256 or stronger; SHA-1 and MD5 in a signature are refused
// outright (design §6).
bool strong_enough(SignatureAlgorithm const& algorithm)
{
    return algorithm.kind != SignatureKind::Unsupported;
}

bool key_strong_enough(PublicKey const& key)
{
    if (key.kind == PublicKey::Kind::Rsa)
        return key.rsa.n.bit_length() >= 2048;
    if (key.kind == PublicKey::Kind::Ec)
        return true; // only P-256 and P-384 are ever parsed
    return false;
}

// One label of a wildcard never matches a name with none, and a wildcard
// matches exactly one label, never a dotted span.
bool wildcard_matches(std::string const& pattern, std::string const& host)
{
    // pattern is "*." + rest; host must be "<label>." + rest with one label.
    std::string const rest = pattern.substr(2);
    std::size_t const dot = host.find('.');
    if (dot == std::string::npos)
        return false;
    // The wildcard may not stand for a public-suffix-level name: require
    // the remainder to carry at least two labels (one dot).
    if (rest.find('.') == std::string::npos)
        return false;
    return host.substr(dot + 1) == rest;
}

}

bool host_matches(std::string const& presented_raw, std::string const& host_raw)
{
    std::string const presented = lower(presented_raw);
    std::string const host = lower(host_raw);
    if (presented.empty() || host.empty())
        return false;
    if (presented.size() > 2 && presented[0] == '*' && presented[1] == '.')
        return wildcard_matches(presented, host);
    return presented == host;
}

namespace {

bool leaf_matches_host(Certificate const& leaf, std::string const& host)
{
    if (host.empty())
        return false;
    // An IP literal matches only an iPAddress SAN; a name matches a
    // dNSName SAN. CN is never consulted (browsers dropped it in 2017).
    bool const is_ipv4 = host.find_first_not_of("0123456789.") == std::string::npos && std::count(host.begin(), host.end(), '.') == 3;
    if (is_ipv4) {
        std::vector<std::uint8_t> wanted;
        std::size_t start = 0;
        for (int i = 0; i < 4; ++i) {
            std::size_t const dot = host.find('.', start);
            wanted.push_back(static_cast<std::uint8_t>(std::stoi(host.substr(start, dot - start))));
            start = dot + 1;
        }
        for (std::vector<std::uint8_t> const& ip : leaf.subject_alt_name.ip_addresses) {
            if (ip == wanted)
                return true;
        }
        return false;
    }
    for (std::string const& name : leaf.subject_alt_name.dns_names) {
        if (host_matches(name, host))
            return true;
    }
    return false;
}

// The DNS-name constraints of a CA applied to the leaf's names (design §6;
// permitted and excluded subtrees, RFC 5280 §4.2.1.10).
bool name_constraints_ok(NameConstraints const& constraints, GeneralNames const& names)
{
    auto suffix_match = [](std::string const& base, std::string const& name) {
        std::string const b = lower(base);
        std::string const n = lower(name);
        if (b.empty())
            return true; // an empty base matches everything
        if (b[0] == '.')
            return n.size() > b.size() && n.compare(n.size() - b.size(), b.size(), b) == 0;
        return n == b || (n.size() > b.size() + 1 && n[n.size() - b.size() - 1] == '.' && n.compare(n.size() - b.size(), b.size(), b) == 0);
    };
    for (std::string const& name : names.dns_names) {
        for (std::string const& excluded : constraints.excluded_dns) {
            if (suffix_match(excluded, name))
                return false;
        }
        if (!constraints.permitted_dns.empty()) {
            bool permitted = false;
            for (std::string const& base : constraints.permitted_dns)
                permitted = permitted || suffix_match(base, name);
            if (!permitted)
                return false;
        }
    }
    return true;
}

}

Verdict validate_chain(std::vector<Certificate> const& chain, std::string const& host, std::int64_t now,
    TrustStore const& store, HttpFetch const& fetch_crl)
{
    if (chain.empty())
        return { false, "the server sent no certificate" };
    Certificate const& leaf = chain[0];

    // The leaf must be for this host and, when it says so, usable for
    // server authentication.
    if (!host.empty() && !leaf_matches_host(leaf, host))
        return { false, "the certificate is not valid for this host" };
    if (leaf.has_extended_key_usage && !leaf.eku_server_auth && !leaf.eku_any)
        return { false, "the certificate is not valid for server authentication" };

    // Build the path: from the leaf, follow issuers by subject-name match
    // (preferring an authority-key-identifier match), through the chain the
    // server sent, to an anchor in the store. At most eight links.
    std::vector<Certificate const*> path { &leaf };
    std::vector<Certificate const*> pool;
    for (std::size_t i = 1; i < chain.size(); ++i)
        pool.push_back(&chain[i]);

    Certificate const* anchor = nullptr;
    for (int depth = 0; depth < 8; ++depth) {
        Certificate const* const current = path.back();
        if (current->is_self_issued() && depth > 0)
            return { false, "the chain does not lead to a trusted anchor" };
        // A trust anchor by subject and key: prefer it, and stop.
        bool found_anchor = false;
        for (Certificate const* candidate : store.by_subject(current->issuer)) {
            if (now < candidate->not_before || now > candidate->not_after)
                continue;
            if (!verify_signature(candidate->public_key, current->signature_algorithm, current->tbs(), current->signature))
                continue;
            anchor = candidate;
            found_anchor = true;
            break;
        }
        if (found_anchor)
            break;
        // Otherwise an intermediate the server sent.
        Certificate const* next = nullptr;
        for (Certificate const* candidate : pool) {
            if (!(candidate->subject == current->issuer))
                continue;
            if (!candidate->has_basic_constraints || !candidate->is_ca)
                continue;
            if (candidate->has_key_usage && !candidate->key_usage_key_cert_sign)
                continue;
            if (!verify_signature(candidate->public_key, current->signature_algorithm, current->tbs(), current->signature))
                continue;
            next = candidate;
            break;
        }
        if (next == nullptr)
            return { false, "the chain does not lead to a trusted anchor" };
        path.push_back(next);
    }
    if (anchor == nullptr)
        return { false, "the chain does not lead to a trusted anchor" };

    // Every certificate in the path: within its validity window, a strong
    // enough signature and key, and (for CAs) the basic constraints. The
    // leaf is index 0.
    std::vector<Certificate const*> full = path;
    full.push_back(anchor);
    for (std::size_t i = 0; i < full.size(); ++i) {
        Certificate const& cert = *full[i];
        if (now < cert.not_before)
            return { false, "a certificate in the chain is not yet valid" };
        if (now > cert.not_after)
            return { false, "a certificate in the chain has expired" };
        if (cert.unknown_critical_extension)
            return { false, "a certificate carries an unsupported critical extension" };
        if (!key_strong_enough(cert.public_key))
            return { false, "a certificate's key is too weak" };
        bool const is_ca_position = i > 0;
        if (is_ca_position) {
            if (!cert.has_basic_constraints || !cert.is_ca)
                return { false, "a certificate acting as a CA is not marked as one" };
            if (cert.has_key_usage && !cert.key_usage_key_cert_sign)
                return { false, "a CA certificate may not sign certificates" };
            // pathLenConstraint: the number of non-self-issued intermediates
            // below this CA may not exceed it.
            if (cert.path_length.has_value()) {
                std::size_t const below = i - 1; // intermediates between this and the leaf
                if (below > *cert.path_length)
                    return { false, "the chain is longer than a CA's path-length constraint allows" };
            }
        }
        if (i + 1 < full.size() && !strong_enough(cert.signature_algorithm))
            return { false, "a certificate is signed with an unsupported or weak algorithm" };
    }

    // Name constraints of every CA in the path, applied to the leaf.
    for (std::size_t i = 1; i < full.size(); ++i) {
        if (full[i]->name_constraints.has_value()) {
            if (full[i]->name_constraints->permitted_has_other_kinds) {
                // A constraint on a name kind we do not evaluate: be safe
                // only about the DNS names we can check.
            }
            if (!name_constraints_ok(*full[i]->name_constraints, leaf.subject_alt_name))
                return { false, "the certificate violates a CA's name constraints" };
        }
    }

    // Revocation: the leaf's first HTTP CRL, when a fetcher is given. A CRL
    // that cannot be fetched or parsed, one not signed by the issuer, or one
    // whose issuing distribution point puts the leaf outside its scope is a
    // soft failure (design §6): no information, not a refusal.
    if (fetch_crl && !leaf.crl_distribution_points.empty()) {
        Certificate const* const issuer = full.size() > 1 ? full[1] : nullptr;
        std::string const& point = leaf.crl_distribution_points.front();
        std::vector<std::uint8_t> const bytes = fetch_crl(point);
        if (!bytes.empty()) {
            if (std::optional<Crl> const crl = parse_crl(bytes); crl && crl->covers_leaf(point)) {
                bool const signed_ok = issuer && verify_signature(issuer->public_key, crl->signature_algorithm, crl->tbs(), crl->signature);
                if (signed_ok && crl->revokes(leaf.serial))
                    return { false, "the certificate has been revoked" };
            }
        }
    }

    return { true, {} };
}

namespace {

// A kept list's file: this line, the URL, the expiry (seconds since the
// epoch), the length, then the list's bytes. Anything else is no list.
constexpr std::string_view kept_magic = "sashfold-crl 1";

}

void CrlCache::set_directory(std::string directory)
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    m_directory = std::move(directory);
}

std::size_t CrlCache::fetches() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return m_fetches;
}

CrlCache::Entry const* CrlCache::find(std::string const& url, std::int64_t now) const
{
    for (auto const& [key, entry] : m_entries) {
        if (key == url && now < entry.expires)
            return &entry;
    }
    return nullptr;
}

std::string CrlCache::path_for(std::string const& url) const
{
    // The file is named by the URL's hash: a URL is no file name.
    crypto::Sha256::Digest const digest = crypto::Sha256::hash(
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(url.data()), url.size()));
    std::string name;
    for (std::size_t i = 0; i < 16; ++i) {
        name += "0123456789abcdef"[digest[i] >> 4];
        name += "0123456789abcdef"[digest[i] & 15];
    }
    return (std::filesystem::path(m_directory) / (name + ".crl")).string();
}

std::optional<CrlCache::Entry> CrlCache::read_kept(std::string const& url, std::int64_t now) const
{
    std::ifstream file(path_for(url), std::ios::binary);
    if (!file)
        return std::nullopt;
    std::string magic;
    std::string kept_url;
    std::string expires_text;
    std::string length_text;
    if (!std::getline(file, magic) || magic != kept_magic || !std::getline(file, kept_url) || kept_url != url
        || !std::getline(file, expires_text) || !std::getline(file, length_text))
        return std::nullopt;
    Entry entry;
    std::size_t length = 0;
    auto const expires_end = expires_text.data() + expires_text.size();
    auto const length_end = length_text.data() + length_text.size();
    if (std::from_chars(expires_text.data(), expires_end, entry.expires).ptr != expires_end
        || std::from_chars(length_text.data(), length_end, length).ptr != length_end)
        return std::nullopt;
    if (now >= entry.expires || length == 0 || length > 64u * 1024u * 1024u)
        return std::nullopt;
    entry.bytes.resize(length);
    if (!file.read(reinterpret_cast<char*>(entry.bytes.data()), static_cast<std::streamsize>(length)) || file.peek() != EOF)
        return std::nullopt;
    return entry;
}

void CrlCache::keep(std::string const& url, Entry const& entry) const
{
    // Written beside its place and renamed into it, so a reader never
    // sees half a list.
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    std::string const path = path_for(url);
    std::string const partial = path + ".part";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file)
            return;
        file << kept_magic << '\n' << url << '\n' << entry.expires << '\n' << entry.bytes.size() << '\n';
        file.write(reinterpret_cast<char const*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
        if (!file)
            return;
    }
    std::filesystem::rename(partial, path, error);
    if (error)
        std::filesystem::remove(partial, error);
}

std::vector<std::uint8_t> CrlCache::get(std::string const& url, std::int64_t now)
{
    std::string directory;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            if (Entry const* const entry = find(url, now))
                return entry->bytes;
            if (std::find(m_in_flight.begin(), m_in_flight.end(), url) == m_in_flight.end())
                break;
            m_arrived.wait(lock);
        }
        m_in_flight.push_back(url);
        directory = m_directory;
    }
    // The mark comes off on every way out, a throw included, or the
    // handshakes waiting on this list would wait for ever.
    struct InFlight {
        CrlCache& cache;
        std::string const& url;
        ~InFlight()
        {
            std::lock_guard<std::mutex> const lock(cache.m_mutex);
            std::erase(cache.m_in_flight, url);
            cache.m_arrived.notify_all();
        }
    } in_flight { *this, url };
    // Outside the lock: a download holds up only the handshakes that want
    // the same list.
    std::optional<Entry> found = directory.empty() ? std::nullopt : read_kept(url, now);
    if (!found) {
        Entry fetched;
        if (m_fetch) {
            {
                std::lock_guard<std::mutex> const lock(m_mutex);
                ++m_fetches;
            }
            fetched.bytes = m_fetch(url);
        }
        fetched.expires = now + failure_hold_seconds;
        if (!fetched.bytes.empty()) {
            fetched.expires = now + unnamed_hold_seconds;
            if (std::optional<Crl> const crl = parse_crl(fetched.bytes); crl && crl->next_update)
                fetched.expires = std::min(*crl->next_update, now + longest_hold_seconds);
            if (!directory.empty() && fetched.expires > now)
                keep(url, fetched);
        }
        found = std::move(fetched);
    }
    // The entry goes in before the mark comes off (InFlight's destructor
    // runs after this), so a waiter that wakes finds it.
    std::lock_guard<std::mutex> const lock(m_mutex);
    std::erase_if(m_entries, [&](auto const& entry) { return entry.first == url || now >= entry.second.expires; });
    m_entries.emplace_back(url, *found);
    return found->bytes;
}

HttpFetch CrlCache::fetcher(std::int64_t now)
{
    return [this, now](std::string const& url) { return get(url, now); };
}

}
