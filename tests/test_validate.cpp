// Certificate path validation (src/net/tls/Validate.cpp) over the test PKI
// in tests/fixtures/x509: a valid chain to a trusted root is accepted, and
// each way a chain can be bad — expired, self-signed, an untrusted root,
// the wrong host, a revoked serial, a name-constraint violation — is
// refused with a reason. Deterministic, no network. The security-critical
// half of the TLS client.
#include "Test.h"

#include "net/tls/TrustStore.h"
#include "net/tls/Validate.h"
#include "net/tls/X509.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

std::string fixtures;
std::int64_t const now = 1893456000; // 2030-01-01, inside every non-expired fixture

std::string read_file(std::string const& name)
{
    std::ifstream in(fixtures + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
}

tls::Certificate cert(std::string const& name)
{
    std::vector<std::vector<std::uint8_t>> const blocks = tls::pem_decode(read_file(name));
    return *tls::parse_certificate(blocks.at(0));
}

void test_host_matches()
{
    CHECK(tls::host_matches("sashfold.test", "sashfold.test"));
    CHECK(tls::host_matches("SashFold.Test", "sashfold.test")); // case-insensitive
    CHECK(!tls::host_matches("sashfold.test", "www.sashfold.test"));
    CHECK(tls::host_matches("*.sub.sashfold.test", "a.sub.sashfold.test"));
    CHECK(!tls::host_matches("*.sub.sashfold.test", "a.b.sub.sashfold.test")); // one label only
    CHECK(!tls::host_matches("*.sub.sashfold.test", "sub.sashfold.test")); // needs a label
    CHECK(!tls::host_matches("*.test", "example.test")); // no public-suffix-level wildcard
    CHECK(!tls::host_matches("*", "anything"));
}

void test_valid_chain()
{
    tls::TrustStore const store = tls::TrustStore::from_pem(read_file("root.pem"));
    CHECK_EQ(store.size(), std::size_t(1));
    std::vector<tls::Certificate> const chain = { cert("leaf.pem"), cert("inter.pem") };
    tls::Verdict const v = tls::validate_chain(chain, "sashfold.test", now, store, nullptr);
    CHECK(v.trusted);
    CHECK_EQ(v.reason, std::string());
    // A wildcard SAN host, and the IP SAN.
    CHECK(tls::validate_chain(chain, "anything.sub.sashfold.test", now, store, nullptr).trusted);
    CHECK(tls::validate_chain(chain, "127.0.0.1", now, store, nullptr).trusted);
    // The RSA leaf verifies too (RSA-signed leaf under the ECDSA CA... it is
    // ECDSA-signed; the RSA key is the leaf's own).
    std::vector<tls::Certificate> const rsa_chain = { cert("leaf-rsa.pem"), cert("inter.pem") };
    CHECK(tls::validate_chain(rsa_chain, "rsa.sashfold.test", now, store, nullptr).trusted);
}

void test_refusals()
{
    tls::TrustStore const store = tls::TrustStore::from_pem(read_file("root.pem"));
    auto refused = [&](std::string const& leaf, std::string const& inter, std::string const& host) {
        std::vector<tls::Certificate> chain = { cert(leaf) };
        if (!inter.empty())
            chain.push_back(cert(inter));
        return tls::validate_chain(chain, host, now, store, nullptr);
    };
    // The wrong host.
    CHECK(!refused("leaf.pem", "inter.pem", "evil.test").trusted);
    // Expired.
    tls::Verdict const expired = refused("leaf-expired.pem", "inter.pem", "expired.sashfold.test");
    CHECK(!expired.trusted);
    CHECK(expired.reason.find("expired") != std::string::npos);
    // Self-signed, not an anchor.
    CHECK(!refused("self.pem", "", "self.sashfold.test").trusted);
    // A leaf whose chain leads to a root not in the store.
    CHECK(!refused("leaf-other-root.pem", "", "other.sashfold.test").trusted);
    // A chain missing its intermediate cannot reach the anchor.
    CHECK(!refused("leaf.pem", "", "sashfold.test").trusted);
    // Expired even for the right host.
    CHECK(!validate_chain({ cert("leaf.pem"), cert("inter.pem") }, "sashfold.test", 100, store, nullptr).trusted); // before notBefore
}

void test_name_constraints()
{
    tls::TrustStore const store = tls::TrustStore::from_pem(read_file("root.pem"));
    std::vector<tls::Certificate> const ok = { cert("leaf-nc-ok.pem"), cert("inter-nc.pem") };
    CHECK(tls::validate_chain(ok, "a.allowed.test", now, store, nullptr).trusted);
    std::vector<tls::Certificate> const bad = { cert("leaf-nc-bad.pem"), cert("inter-nc.pem") };
    tls::Verdict const v = tls::validate_chain(bad, "forbidden.test", now, store, nullptr);
    CHECK(!v.trusted);
    CHECK(v.reason.find("name constraints") != std::string::npos);
}

void test_revocation()
{
    tls::TrustStore const store = tls::TrustStore::from_pem(read_file("root.pem"));
    std::string const crl = read_file("inter.crl");
    std::vector<std::uint8_t> const crl_bytes(crl.begin(), crl.end());
    auto fetch = [&](std::string const&) { return crl_bytes; };
    // The revoked leaf is refused once the CRL is consulted, accepted when
    // it is not (the soft-fail path with no fetcher).
    std::vector<tls::Certificate> const revoked = { cert("leaf-revoked.pem"), cert("inter.pem") };
    CHECK(tls::validate_chain(revoked, "revoked.sashfold.test", now, store, nullptr).trusted);
    tls::Verdict const v = tls::validate_chain(revoked, "revoked.sashfold.test", now, store, fetch);
    CHECK(!v.trusted);
    CHECK(v.reason.find("revoked") != std::string::npos);
    // A non-revoked leaf passes the same CRL.
    std::vector<tls::Certificate> const good = { cert("leaf.pem"), cert("inter.pem") };
    CHECK(tls::validate_chain(good, "sashfold.test", now, store, fetch).trusted);
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_validate <fixtures-dir>\n");
        return 2;
    }
    fixtures = argv[1];
    test_host_matches();
    test_valid_chain();
    test_refusals();
    test_name_constraints();
    test_revocation();
    return sashfold::test::report("validate");
}
