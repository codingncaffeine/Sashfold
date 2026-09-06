#include "net/tls/TrustStore.h"

#include <fstream>
#include <iterator>

namespace sashfold::tls {

void TrustStore::add(Certificate certificate)
{
    std::vector<std::uint8_t> const key = certificate.subject.der;
    m_anchors.push_back(std::move(certificate));
    m_by_subject.emplace(key, m_anchors.size() - 1);
}

TrustStore TrustStore::from_pem(std::string_view pem)
{
    TrustStore store;
    for (std::vector<std::uint8_t> const& der : pem_decode(pem)) {
        if (std::optional<Certificate> certificate = parse_certificate(der))
            store.add(std::move(*certificate));
    }
    return store;
}

TrustStore TrustStore::load()
{
    char const* const override_path = std::getenv("SSL_CERT_FILE");
    char const* const candidates[] = {
        override_path,
        "/etc/ssl/certs/ca-certificates.crt", // Debian, Arch, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt", // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem", // openSUSE
        "/etc/ssl/cert.pem", // macOS, some BSDs
    };
    for (char const* const path : candidates) {
        if (path == nullptr)
            continue;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            continue;
        std::string const text(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
        if (text.empty())
            continue;
        return from_pem(text);
    }
    return {};
}

std::vector<Certificate const*> TrustStore::by_subject(Name const& subject) const
{
    std::vector<Certificate const*> out;
    auto const range = m_by_subject.equal_range(subject.der);
    for (auto it = range.first; it != range.second; ++it)
        out.push_back(&m_anchors[it->second]);
    return out;
}

}
