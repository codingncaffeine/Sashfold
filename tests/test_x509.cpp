// The DER reader and the X.509 parser over the test PKI in
// tests/fixtures/x509 (made with OpenSSL: an RSA root, an ECDSA
// intermediate, leaves for the cases the validator will judge, and a CRL),
// then over the machine's own CA bundle when it has one.
#include "Test.h"

#include "net/tls/Der.h"
#include "net/tls/X509.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

std::string fixtures;

std::string read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> fixture_der(std::string const& name)
{
    std::vector<std::vector<std::uint8_t>> const blocks = tls::pem_decode(read_file(fixtures + "/" + name));
    if (blocks.size() != 1) {
        test::fail("not one PEM block: " + name, __FILE__, __LINE__);
        return {};
    }
    return blocks[0];
}

std::optional<tls::Certificate> fixture(std::string const& name)
{
    std::optional<tls::Certificate> cert = tls::parse_certificate(fixture_der(name));
    if (!cert)
        test::fail("did not parse: " + name, __FILE__, __LINE__);
    return cert;
}

void test_der()
{
    // A SEQUENCE of an INTEGER and a BOOLEAN, and the malformations DER forbids.
    std::vector<std::uint8_t> const good = { 0x30, 0x06, 0x02, 0x01, 0x05, 0x01, 0x01, 0xff };
    tls::DerReader reader(good);
    std::optional<tls::DerElement> const sequence = reader.next(tls::DerType::Sequence);
    CHECK(sequence.has_value() && reader.at_end());
    tls::DerReader inner(sequence->content);
    std::optional<tls::DerElement> const integer = inner.next(tls::DerType::Integer);
    CHECK(integer.has_value() && tls::der_small_integer(*integer) == std::uint64_t(5));
    std::optional<tls::DerElement> const boolean = inner.next();
    CHECK(boolean.has_value() && tls::der_boolean(*boolean) == true && inner.at_end());
    std::vector<std::uint8_t> const truncated = { 0x30, 0x06, 0x02, 0x01, 0x05 };
    CHECK(!tls::DerReader(truncated).next().has_value());
    std::vector<std::uint8_t> const indefinite = { 0x30, 0x80, 0x00, 0x00 };
    CHECK(!tls::DerReader(indefinite).next().has_value());
    std::vector<std::uint8_t> const long_form_for_short = { 0x02, 0x81, 0x01, 0x05 };
    CHECK(!tls::DerReader(long_form_for_short).next().has_value());
    std::vector<std::uint8_t> const leading_zero_length = { 0x04, 0x82, 0x00, 0x80 };
    CHECK(!tls::DerReader(leading_zero_length).next().has_value());
    std::vector<std::uint8_t> const bad_boolean = { 0x01, 0x01, 0x01 };
    CHECK(!tls::der_boolean(*tls::DerReader(bad_boolean).next()).has_value());
    std::vector<std::uint8_t> const negative = { 0x02, 0x01, 0x80 };
    CHECK(!tls::der_integer(*tls::DerReader(negative).next()).has_value());
    std::vector<std::uint8_t> const padded = { 0x02, 0x02, 0x00, 0x05 };
    CHECK(!tls::der_integer(*tls::DerReader(padded).next()).has_value());
    std::vector<std::uint8_t> const needs_pad = { 0x02, 0x02, 0x00, 0x80 };
    CHECK(tls::der_small_integer(*tls::DerReader(needs_pad).next()) == std::uint64_t(0x80));
    std::vector<std::uint8_t> const oid = { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
    CHECK_EQ(tls::der_oid(*tls::DerReader(oid).next()).value_or("?"), "1.2.840.113549.1.1.11");
    std::vector<std::uint8_t> const oid2 = { 0x06, 0x03, 0x55, 0x1d, 0x13 };
    CHECK_EQ(tls::der_oid(*tls::DerReader(oid2).next()).value_or("?"), "2.5.29.19");
    std::vector<std::uint8_t> const utc = { 0x17, 0x0d, '2', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z' };
    CHECK(tls::der_time(*tls::DerReader(utc).next()) == tls::seconds_from_civil(2026, 1, 1, 0, 0, 0));
    std::vector<std::uint8_t> const gen = { 0x18, 0x0f, '2', '0', '5', '0', '1', '2', '3', '1', '2', '3', '5', '9', '5', '9', 'Z' };
    CHECK(tls::der_time(*tls::DerReader(gen).next()) == tls::seconds_from_civil(2050, 12, 31, 23, 59, 59));
    std::vector<std::uint8_t> const no_z = { 0x17, 0x0d, '2', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', '0' };
    CHECK(!tls::der_time(*tls::DerReader(no_z).next()).has_value());
    CHECK_EQ(tls::seconds_from_civil(1970, 1, 1, 0, 0, 0), std::int64_t(0));
    CHECK_EQ(tls::seconds_from_civil(2000, 3, 1, 0, 0, 0), std::int64_t(951868800));
    CHECK_EQ(tls::seconds_from_civil(2026, 9, 6, 5, 0, 0), std::int64_t(1788670800));
    std::vector<std::uint8_t> const bits = { 0x03, 0x02, 0x05, 0xa0 };
    auto const with_unused = tls::der_bit_string_with_unused(*tls::DerReader(bits).next());
    CHECK(with_unused.has_value() && with_unused->first == 5 && with_unused->second.size() == 1);
    std::vector<std::uint8_t> const dirty_bits = { 0x03, 0x02, 0x05, 0xa1 };
    CHECK(!tls::der_bit_string_with_unused(*tls::DerReader(dirty_bits).next()).has_value());
    std::vector<std::uint8_t> const bmp = { 0x1e, 0x04, 0x00, 0x41, 0x20, 0xac };
    CHECK_EQ(tls::der_string(*tls::DerReader(bmp).next()).value_or("?"), "A\xe2\x82\xac");
}

void test_certificates()
{
    std::optional<tls::Certificate> const root = fixture("root.pem");
    std::optional<tls::Certificate> const inter = fixture("inter.pem");
    std::optional<tls::Certificate> const leaf = fixture("leaf.pem");
    if (!root || !inter || !leaf)
        return;
    CHECK_EQ(leaf->version, 3);
    CHECK_EQ(leaf->subject.common_name, "leaf"); // the SAN carries the host; the CN is the file stem
    CHECK_EQ(leaf->subject.display, "CN=leaf");
    CHECK_EQ(leaf->issuer.common_name, "Sashfold Test Intermediate");
    CHECK(leaf->issuer == inter->subject);
    CHECK(!leaf->is_self_issued());
    CHECK_EQ(leaf->not_before, tls::seconds_from_civil(2026, 1, 1, 0, 0, 0));
    CHECK_EQ(leaf->not_after, tls::seconds_from_civil(2036, 1, 1, 0, 0, 0));
    CHECK(leaf->public_key.kind == tls::PublicKey::Kind::Ec && leaf->public_key.curve == crypto::CurveId::P256);
    CHECK(leaf->has_basic_constraints && !leaf->is_ca && !leaf->path_length.has_value());
    CHECK(leaf->has_key_usage && leaf->key_usage_digital_signature && !leaf->key_usage_key_cert_sign);
    CHECK(leaf->has_extended_key_usage && leaf->eku_server_auth);
    CHECK(leaf->has_subject_alt_name);
    CHECK_EQ(leaf->subject_alt_name.dns_names.size(), std::size_t(2));
    CHECK_EQ(leaf->subject_alt_name.dns_names[0], "sashfold.test");
    CHECK_EQ(leaf->subject_alt_name.dns_names[1], "*.sub.sashfold.test");
    CHECK_EQ(leaf->subject_alt_name.ip_addresses.size(), std::size_t(1));
    CHECK(leaf->subject_alt_name.ip_addresses[0] == std::vector<std::uint8_t>({ 127, 0, 0, 1 }));
    CHECK_EQ(leaf->crl_distribution_points.size(), std::size_t(1));
    CHECK_EQ(leaf->crl_distribution_points[0], "http://crl.sashfold.test/inter.crl");
    CHECK(leaf->signature_algorithm.kind == tls::SignatureKind::Ecdsa && leaf->signature_algorithm.hash == crypto::HashId::Sha256);
    CHECK(leaf->authority_key_identifier == inter->subject_key_identifier);
    CHECK(!leaf->subject_key_identifier.empty());
    CHECK(!leaf->unknown_critical_extension);
    // Signatures: the leaf under the intermediate, the intermediate under
    // the root, the root under itself — and not under the wrong key.
    CHECK(tls::verify_signature(inter->public_key, leaf->signature_algorithm, leaf->tbs(), leaf->signature));
    CHECK(!tls::verify_signature(root->public_key, leaf->signature_algorithm, leaf->tbs(), leaf->signature));
    CHECK(inter->signature_algorithm.kind == tls::SignatureKind::RsaPkcs1);
    CHECK(tls::verify_signature(root->public_key, inter->signature_algorithm, inter->tbs(), inter->signature));
    CHECK(root->is_self_issued() && root->is_ca && root->has_basic_constraints);
    CHECK(root->public_key.kind == tls::PublicKey::Kind::Rsa && root->public_key.rsa.n.bit_length() == std::size_t(2048));
    CHECK(tls::verify_signature(root->public_key, root->signature_algorithm, root->tbs(), root->signature));
    CHECK(inter->is_ca && inter->path_length == std::uint64_t(0));
    CHECK(inter->key_usage_key_cert_sign && inter->key_usage_crl_sign);
    // A tampered TBS or signature does not verify.
    std::vector<std::uint8_t> tampered = leaf->der;
    tampered[leaf->tbs_offset + leaf->tbs_length - 1] ^= 1; // the last byte of the last extension: a URI character
    std::optional<tls::Certificate> const changed = tls::parse_certificate(tampered);
    CHECK(changed.has_value());
    if (changed)
        CHECK(!tls::verify_signature(inter->public_key, changed->signature_algorithm, changed->tbs(), changed->signature));
    std::vector<std::uint8_t> bad_signature = leaf->signature;
    bad_signature[10] ^= 1;
    CHECK(!tls::verify_signature(inter->public_key, leaf->signature_algorithm, leaf->tbs(), bad_signature));
    // Other fixtures: keys, constraints, validity.
    std::optional<tls::Certificate> const rsa_leaf = fixture("leaf-rsa.pem");
    if (rsa_leaf) {
        CHECK(rsa_leaf->public_key.kind == tls::PublicKey::Kind::Rsa);
        CHECK(tls::verify_signature(inter->public_key, rsa_leaf->signature_algorithm, rsa_leaf->tbs(), rsa_leaf->signature));
    }
    std::optional<tls::Certificate> const constrained = fixture("inter-nc.pem");
    if (constrained) {
        CHECK(constrained->name_constraints.has_value());
        if (constrained->name_constraints) {
            CHECK_EQ(constrained->name_constraints->permitted_dns.size(), std::size_t(1));
            CHECK_EQ(constrained->name_constraints->permitted_dns[0], ".allowed.test");
            CHECK(constrained->name_constraints->excluded_dns.empty());
        }
    }
    std::optional<tls::Certificate> const expired = fixture("leaf-expired.pem");
    if (expired)
        CHECK_EQ(expired->not_after, tls::seconds_from_civil(2021, 1, 1, 0, 0, 0));
    std::optional<tls::Certificate> const self = fixture("self.pem");
    if (self) {
        CHECK(self->is_self_issued());
        CHECK(tls::verify_signature(self->public_key, self->signature_algorithm, self->tbs(), self->signature));
    }
    // Malformed inputs: a byte short, a byte long, nothing.
    std::vector<std::uint8_t> short_der = leaf->der;
    short_der.pop_back();
    CHECK(!tls::parse_certificate(short_der).has_value());
    std::vector<std::uint8_t> long_der = leaf->der;
    long_der.push_back(0);
    CHECK(!tls::parse_certificate(long_der).has_value());
    CHECK(!tls::parse_certificate({}).has_value());
    // Two blocks in one text decode to two certificates.
    std::string const both = read_file(fixtures + "/root.pem") + read_file(fixtures + "/inter.pem");
    CHECK_EQ(tls::pem_decode(both).size(), std::size_t(2));
}

void test_crl()
{
    std::optional<tls::Certificate> const inter = fixture("inter.pem");
    std::optional<tls::Certificate> const revoked = fixture("leaf-revoked.pem");
    std::optional<tls::Certificate> const leaf = fixture("leaf.pem");
    std::string const crl_bytes = read_file(fixtures + "/inter.crl");
    std::optional<tls::Crl> const crl = tls::parse_crl(std::vector<std::uint8_t>(crl_bytes.begin(), crl_bytes.end()));
    CHECK(crl.has_value());
    if (!crl || !inter || !revoked || !leaf)
        return;
    CHECK(crl->issuer == inter->subject);
    CHECK(crl->next_update.has_value());
    CHECK_EQ(crl->revoked_serials.size(), std::size_t(1));
    CHECK(crl->revokes(revoked->serial));
    CHECK(!crl->revokes(leaf->serial));
    CHECK(tls::verify_signature(inter->public_key, crl->signature_algorithm, crl->tbs(), crl->signature));
    CHECK(crl->authority_key_identifier.empty()); // openssl ca writes only a CRL number
    CHECK(!tls::parse_crl(leaf->der).has_value());
}

void test_system_bundle()
{
    // The machine's CA bundle, when it has one: every block parses, and
    // there are more than a few dozen of them.
    char const* candidates[] = { "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem" };
    for (char const* path : candidates) {
        std::string const text = read_file(path);
        if (text.empty())
            continue;
        std::vector<std::vector<std::uint8_t>> const blocks = tls::pem_decode(text);
        std::size_t parsed = 0;
        std::size_t rsa = 0;
        std::size_t ec = 0;
        for (std::vector<std::uint8_t> const& der : blocks) {
            std::optional<tls::Certificate> const cert = tls::parse_certificate(der);
            if (!cert)
                continue;
            ++parsed;
            if (cert->public_key.kind == tls::PublicKey::Kind::Rsa)
                ++rsa;
            if (cert->public_key.kind == tls::PublicKey::Kind::Ec)
                ++ec;
            CHECK(cert->is_ca);
            CHECK(cert->is_self_issued());
            if (cert->public_key.kind == tls::PublicKey::Kind::Unsupported || cert->signature_algorithm.kind == tls::SignatureKind::Unsupported)
                continue; // a curve or a scheme we do not have (P-521, say): parsed, never trusted
            bool const verified = tls::verify_signature(cert->public_key, cert->signature_algorithm, cert->tbs(), cert->signature);
            if (!verified)
                std::printf("self-signature did not verify: %s (%s)\n", cert->subject.display.c_str(), cert->signature_algorithm.oid.c_str());
            CHECK(verified);
        }
        std::printf("bundle %s: %zu blocks, %zu parsed (%zu RSA, %zu EC)\n", path, blocks.size(), parsed, rsa, ec);
        CHECK_EQ(parsed, blocks.size());
        CHECK(parsed > 50);
        return;
    }
    std::printf("no CA bundle on this machine; the bundle check did not run\n");
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_x509 <fixtures-dir>\n");
        return 2;
    }
    fixtures = argv[1];
    test_der();
    test_certificates();
    test_crl();
    test_system_bundle();
    return sashfold::test::report("x509");
}
