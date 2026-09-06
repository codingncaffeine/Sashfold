#pragma once
// The set of trust anchors the validator builds paths to: the system CA
// bundle, parsed once and indexed by subject name for issuer lookup.
#include "net/tls/X509.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sashfold::tls {

class TrustStore {
public:
    // Loads SSL_CERT_FILE, or the first of the distributions' usual bundle
    // paths; an empty store when none is found (every chain then fails).
    static TrustStore load();
    // A bundle from PEM text, for tests.
    static TrustStore from_pem(std::string_view pem);

    // The anchors whose subject equals `subject` (usually one).
    std::vector<Certificate const*> by_subject(Name const& subject) const;
    std::size_t size() const { return m_anchors.size(); }

private:
    std::vector<Certificate> m_anchors;
    std::multimap<std::string, std::size_t> m_by_subject; // subject DER as bytes → index
    void add(Certificate certificate);
};

}
