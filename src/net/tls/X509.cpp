#include "net/tls/X509.h"

#include "core/Base64.h"
#include "net/tls/Der.h"

#include <algorithm>

namespace sashfold::tls {

namespace {

std::vector<std::uint8_t> to_vector(std::span<std::uint8_t const> bytes)
{
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::optional<crypto::HashId> hash_from_oid(std::string_view id)
{
    if (id == oid::sha256)
        return crypto::HashId::Sha256;
    if (id == oid::sha384)
        return crypto::HashId::Sha384;
    if (id == oid::sha512)
        return crypto::HashId::Sha512;
    return std::nullopt;
}

// AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters ANY OPTIONAL }.
// The RSASSA-PSS parameters (RFC 8017 A.2.3) name the hash, the MGF and
// the salt length; every other algorithm's parameters are ignored.
std::optional<SignatureAlgorithm> parse_signature_algorithm(DerElement const& element)
{
    if (!element.tag.is(DerType::Sequence))
        return std::nullopt;
    DerReader reader(element.content);
    std::optional<DerElement> const oid_element = reader.next(DerType::Oid);
    if (!oid_element)
        return std::nullopt;
    std::optional<std::string> const id = der_oid(*oid_element);
    if (!id)
        return std::nullopt;
    SignatureAlgorithm algorithm;
    algorithm.oid = *id;
    if (*id == oid::sha256_with_rsa || *id == oid::sha384_with_rsa || *id == oid::sha512_with_rsa) {
        algorithm.kind = SignatureKind::RsaPkcs1;
        algorithm.hash = *id == oid::sha256_with_rsa ? crypto::HashId::Sha256 : *id == oid::sha384_with_rsa ? crypto::HashId::Sha384 : crypto::HashId::Sha512;
    } else if (*id == oid::ecdsa_with_sha256 || *id == oid::ecdsa_with_sha384 || *id == oid::ecdsa_with_sha512) {
        algorithm.kind = SignatureKind::Ecdsa;
        algorithm.hash = *id == oid::ecdsa_with_sha256 ? crypto::HashId::Sha256 : *id == oid::ecdsa_with_sha384 ? crypto::HashId::Sha384 : crypto::HashId::Sha512;
    } else if (*id == oid::rsassa_pss) {
        std::optional<DerElement> const params = reader.next(DerType::Sequence);
        if (!params)
            return std::nullopt;
        DerReader p(params->content);
        std::optional<crypto::HashId> hash;
        std::optional<crypto::HashId> mgf_hash;
        std::uint64_t salt_length = 20;
        if (std::optional<DerElement> const h = p.next_context(0)) {
            DerReader hh(h->content);
            std::optional<DerElement> const seq = hh.next(DerType::Sequence);
            if (!seq)
                return std::nullopt;
            DerReader s(seq->content);
            std::optional<DerElement> const ho = s.next(DerType::Oid);
            if (!ho)
                return std::nullopt;
            hash = hash_from_oid(der_oid(*ho).value_or(""));
        }
        if (std::optional<DerElement> const m = p.next_context(1)) {
            DerReader mm(m->content);
            std::optional<DerElement> const seq = mm.next(DerType::Sequence);
            if (!seq)
                return std::nullopt;
            DerReader s(seq->content);
            std::optional<DerElement> const mo = s.next(DerType::Oid);
            std::optional<DerElement> const inner = s.next(DerType::Sequence);
            if (!mo || !inner || der_oid(*mo).value_or("") != oid::mgf1)
                return std::nullopt;
            DerReader ii(inner->content);
            std::optional<DerElement> const ho = ii.next(DerType::Oid);
            if (!ho)
                return std::nullopt;
            mgf_hash = hash_from_oid(der_oid(*ho).value_or(""));
        }
        if (std::optional<DerElement> const s = p.next_context(2)) {
            DerReader ss(s->content);
            std::optional<DerElement> const integer = ss.next(DerType::Integer);
            std::optional<std::uint64_t> const value = integer ? der_small_integer(*integer) : std::nullopt;
            if (!value)
                return std::nullopt;
            salt_length = *value;
        }
        // The SHA-1 defaults are refused: a PSS signature here names SHA-2
        // for both the hash and the mask, and the same one for both.
        if (!hash || !mgf_hash || *hash != *mgf_hash || salt_length > 64)
            return std::nullopt;
        algorithm.kind = SignatureKind::RsaPss;
        algorithm.hash = *hash;
        algorithm.pss_salt_length = salt_length;
    } else {
        algorithm.kind = SignatureKind::Unsupported;
    }
    return algorithm;
}

std::string attribute_label(std::string_view type)
{
    if (type == oid::common_name)
        return "CN";
    if (type == "2.5.4.10")
        return "O";
    if (type == "2.5.4.11")
        return "OU";
    if (type == "2.5.4.6")
        return "C";
    if (type == "2.5.4.7")
        return "L";
    if (type == "2.5.4.8")
        return "ST";
    return std::string(type);
}

// Name ::= SEQUENCE OF RelativeDistinguishedName (SET OF AttributeTypeAndValue).
std::optional<Name> parse_name(DerElement const& element)
{
    if (!element.tag.is(DerType::Sequence))
        return std::nullopt;
    Name name;
    name.der = to_vector(element.whole);
    DerReader rdns(element.content);
    while (!rdns.at_end()) {
        std::optional<DerElement> const set = rdns.next(DerType::Set);
        if (!set)
            return std::nullopt;
        DerReader attributes(set->content);
        while (!attributes.at_end()) {
            std::optional<DerElement> const attribute = attributes.next(DerType::Sequence);
            if (!attribute)
                return std::nullopt;
            DerReader a(attribute->content);
            std::optional<DerElement> const type_element = a.next(DerType::Oid);
            std::optional<DerElement> const value = type_element ? a.next() : std::nullopt;
            if (!type_element || !value || !a.at_end())
                return std::nullopt;
            std::optional<std::string> const type = der_oid(*type_element);
            if (!type)
                return std::nullopt;
            std::optional<std::string> const text = der_string(*value);
            if (!text)
                continue; // a value in a form we do not read: the DER still compares
            if (!name.display.empty())
                name.display += ", ";
            name.display += attribute_label(*type) + "=" + *text;
            if (*type == oid::common_name && name.common_name.empty())
                name.common_name = *text;
        }
    }
    return name;
}

// GeneralNames ::= SEQUENCE OF GeneralName; the kinds the policy reads
// are dNSName [2], iPAddress [7] and uniformResourceIdentifier [6].
std::optional<GeneralNames> parse_general_names(std::span<std::uint8_t const> content)
{
    GeneralNames names;
    DerReader reader(content);
    while (!reader.at_end()) {
        std::optional<DerElement> const name = reader.next();
        if (!name || name->tag.cls != DerClass::Context)
            return std::nullopt;
        switch (name->tag.number) {
        case 2: {
            for (std::uint8_t const b : name->content) {
                if (b >= 0x80 || b == 0)
                    return std::nullopt;
            }
            names.dns_names.emplace_back(name->content.begin(), name->content.end());
            break;
        }
        case 6:
            names.uris.emplace_back(name->content.begin(), name->content.end());
            break;
        case 7:
            if (name->content.size() != 4 && name->content.size() != 16)
                return std::nullopt;
            names.ip_addresses.push_back(to_vector(name->content));
            break;
        default:
            names.has_other = true;
            break;
        }
    }
    return names;
}

}

std::optional<PublicKey> parse_public_key(std::span<std::uint8_t const> spki)
{
    DerReader top(spki);
    std::optional<DerElement> const element = top.next(DerType::Sequence);
    if (!element || !top.at_end())
        return std::nullopt;
    PublicKey key;
    key.spki = to_vector(element->whole);
    DerReader reader(element->content);
    std::optional<DerElement> const algorithm = reader.next(DerType::Sequence);
    std::optional<DerElement> const bits_element = algorithm ? reader.next(DerType::BitString) : std::nullopt;
    if (!algorithm || !bits_element || !reader.at_end())
        return std::nullopt;
    DerReader a(algorithm->content);
    std::optional<DerElement> const id_element = a.next(DerType::Oid);
    std::optional<std::string> const id = id_element ? der_oid(*id_element) : std::nullopt;
    std::optional<std::span<std::uint8_t const>> const bits = der_bit_string(*bits_element);
    if (!id || !bits)
        return std::nullopt;
    if (*id == oid::rsa_encryption) {
        // RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
        DerReader k(*bits);
        std::optional<DerElement> const sequence = k.next(DerType::Sequence);
        if (!sequence || !k.at_end())
            return std::nullopt;
        DerReader s(sequence->content);
        std::optional<DerElement> const n_element = s.next(DerType::Integer);
        std::optional<DerElement> const e_element = n_element ? s.next(DerType::Integer) : std::nullopt;
        if (!n_element || !e_element || !s.at_end())
            return std::nullopt;
        std::optional<std::span<std::uint8_t const>> const n = der_integer(*n_element);
        std::optional<std::span<std::uint8_t const>> const e = der_integer(*e_element);
        if (!n || !e)
            return std::nullopt;
        std::optional<crypto::BigInt> const modulus = crypto::BigInt::from_bytes(*n);
        std::optional<crypto::BigInt> const exponent = crypto::BigInt::from_bytes(*e);
        if (!modulus || !exponent)
            return std::nullopt;
        key.kind = PublicKey::Kind::Rsa;
        key.rsa.n = *modulus;
        key.rsa.e = *exponent;
    } else if (*id == oid::ec_public_key) {
        std::optional<DerElement> const curve_element = a.next(DerType::Oid);
        std::optional<std::string> const curve_id = curve_element ? der_oid(*curve_element) : std::nullopt;
        if (!curve_id)
            return std::nullopt;
        if (*curve_id == oid::prime256v1)
            key.curve = crypto::CurveId::P256;
        else if (*curve_id == oid::secp384r1)
            key.curve = crypto::CurveId::P384;
        else
            return key; // a curve we do not have: the key stays Unsupported
        std::optional<crypto::EcPoint> const point = crypto::ec_decode_point(key.curve, *bits);
        if (!point)
            return std::nullopt; // a point off the curve is a malformed key, not an unsupported one
        key.kind = PublicKey::Kind::Ec;
        key.ec = *point;
    }
    return key;
}

namespace {

// Extension ::= SEQUENCE { extnID OID, critical BOOLEAN DEFAULT FALSE,
// extnValue OCTET STRING }. Each is read once; a second copy of one
// refuses the certificate (RFC 5280 §4.2), and an unrecognised critical
// one marks it for the validator.
bool parse_extensions(Certificate& cert, std::span<std::uint8_t const> content)
{
    std::vector<std::string> seen;
    DerReader reader(content);
    while (!reader.at_end()) {
        std::optional<DerElement> const extension = reader.next(DerType::Sequence);
        if (!extension)
            return false;
        DerReader e(extension->content);
        std::optional<DerElement> const id_element = e.next(DerType::Oid);
        std::optional<std::string> const id = id_element ? der_oid(*id_element) : std::nullopt;
        if (!id)
            return false;
        bool critical = false;
        if (std::optional<DerTag> const tag = e.peek_tag(); tag && tag->is(DerType::Boolean)) {
            std::optional<DerElement> const boolean = e.next();
            std::optional<bool> const flag = boolean ? der_boolean(*boolean) : std::nullopt;
            if (!flag)
                return false;
            critical = *flag;
        }
        std::optional<DerElement> const value_element = e.next(DerType::OctetString);
        if (!value_element || !e.at_end())
            return false;
        if (std::find(seen.begin(), seen.end(), *id) != seen.end())
            return false;
        seen.push_back(*id);
        DerReader v(value_element->content);
        if (*id == oid::basic_constraints) {
            std::optional<DerElement> const sequence = v.next(DerType::Sequence);
            if (!sequence || !v.at_end())
                return false;
            DerReader s(sequence->content);
            cert.has_basic_constraints = true;
            if (std::optional<DerTag> const tag = s.peek_tag(); tag && tag->is(DerType::Boolean)) {
                std::optional<DerElement> const boolean = s.next();
                std::optional<bool> const ca = boolean ? der_boolean(*boolean) : std::nullopt;
                if (!ca)
                    return false;
                cert.is_ca = *ca;
            }
            if (std::optional<DerTag> const tag = s.peek_tag(); tag && tag->is(DerType::Integer)) {
                std::optional<DerElement> const integer = s.next();
                std::optional<std::uint64_t> const length = integer ? der_small_integer(*integer) : std::nullopt;
                if (!length)
                    return false;
                cert.path_length = *length;
            }
            if (!s.at_end())
                return false;
        } else if (*id == oid::key_usage) {
            std::optional<DerElement> const bits_element = v.next(DerType::BitString);
            if (!bits_element || !v.at_end())
                return false;
            auto const bits = der_bit_string_with_unused(*bits_element);
            if (!bits)
                return false;
            cert.has_key_usage = true;
            if (!bits->second.empty()) {
                std::uint8_t const first = bits->second[0];
                cert.key_usage_digital_signature = (first & 0x80) != 0;
                cert.key_usage_key_cert_sign = (first & 0x04) != 0;
                cert.key_usage_crl_sign = (first & 0x02) != 0;
            }
        } else if (*id == oid::extended_key_usage) {
            std::optional<DerElement> const sequence = v.next(DerType::Sequence);
            if (!sequence || !v.at_end())
                return false;
            cert.has_extended_key_usage = true;
            DerReader s(sequence->content);
            while (!s.at_end()) {
                std::optional<DerElement> const purpose = s.next(DerType::Oid);
                std::optional<std::string> const purpose_id = purpose ? der_oid(*purpose) : std::nullopt;
                if (!purpose_id)
                    return false;
                if (*purpose_id == oid::server_auth)
                    cert.eku_server_auth = true;
                if (*purpose_id == oid::any_extended_key_usage)
                    cert.eku_any = true;
            }
        } else if (*id == oid::subject_alt_name) {
            std::optional<DerElement> const sequence = v.next(DerType::Sequence);
            if (!sequence || !v.at_end())
                return false;
            std::optional<GeneralNames> const names = parse_general_names(sequence->content);
            if (!names)
                return false;
            cert.has_subject_alt_name = true;
            cert.subject_alt_name = *names;
        } else if (*id == oid::name_constraints) {
            std::optional<DerElement> const sequence = v.next(DerType::Sequence);
            if (!sequence || !v.at_end())
                return false;
            NameConstraints constraints;
            auto subtrees = [&](DerElement const& list, bool permitted) -> bool {
                DerReader l(list.content);
                while (!l.at_end()) {
                    std::optional<DerElement> const subtree = l.next(DerType::Sequence);
                    if (!subtree)
                        return false;
                    DerReader st(subtree->content);
                    std::optional<DerElement> const base = st.next();
                    if (!base || base->tag.cls != DerClass::Context)
                        return false;
                    if (base->tag.number == 2) {
                        std::string dns(base->content.begin(), base->content.end());
                        (permitted ? constraints.permitted_dns : constraints.excluded_dns).push_back(dns);
                    } else if (permitted) {
                        constraints.permitted_has_other_kinds = true;
                    }
                    while (!st.at_end()) { // minimum [0] and maximum [1], unused
                        if (!st.next())
                            return false;
                    }
                }
                return true;
            };
            DerReader s(sequence->content);
            if (std::optional<DerElement> const permitted = s.next_context(0)) {
                if (!subtrees(*permitted, true))
                    return false;
            }
            if (std::optional<DerElement> const excluded = s.next_context(1)) {
                if (!subtrees(*excluded, false))
                    return false;
            }
            if (!s.at_end())
                return false;
            cert.name_constraints = constraints;
        } else if (*id == oid::subject_key_identifier) {
            std::optional<DerElement> const octets = v.next(DerType::OctetString);
            if (!octets || !v.at_end())
                return false;
            cert.subject_key_identifier = to_vector(octets->content);
        } else if (*id == oid::authority_key_identifier) {
            std::optional<DerElement> const sequence = v.next(DerType::Sequence);
            if (!sequence || !v.at_end())
                return false;
            DerReader s(sequence->content);
            if (std::optional<DerElement> const key_id = s.next_context(0))
                cert.authority_key_identifier = to_vector(key_id->content);
        } else if (*id == oid::crl_distribution_points) {
            std::optional<DerElement> const sequence = v.next(DerType::Sequence);
            if (!sequence || !v.at_end())
                return false;
            DerReader s(sequence->content);
            while (!s.at_end()) {
                std::optional<DerElement> const point = s.next(DerType::Sequence);
                if (!point)
                    return false;
                DerReader d(point->content);
                if (std::optional<DerElement> const name = d.next_context(0)) {
                    DerReader n(name->content);
                    if (std::optional<DerElement> const full = n.next_context(0)) {
                        std::optional<GeneralNames> const names = parse_general_names(full->content);
                        if (!names)
                            return false;
                        for (std::string const& uri : names->uris) {
                            if (uri.starts_with("http://"))
                                cert.crl_distribution_points.push_back(uri);
                        }
                    }
                }
            }
        } else if (*id == oid::authority_info_access) {
            std::optional<DerElement> const sequence = v.next(DerType::Sequence);
            if (!sequence || !v.at_end())
                return false;
            DerReader s(sequence->content);
            while (!s.at_end()) {
                std::optional<DerElement> const access = s.next(DerType::Sequence);
                if (!access)
                    return false;
                DerReader a(access->content);
                std::optional<DerElement> const method_element = a.next(DerType::Oid);
                std::optional<std::string> const method = method_element ? der_oid(*method_element) : std::nullopt;
                std::optional<DerElement> const location = method ? a.next() : std::nullopt;
                if (!method || !location)
                    return false;
                if (*method == oid::ocsp && location->tag.is_context(6))
                    cert.ocsp_responders.emplace_back(location->content.begin(), location->content.end());
            }
        } else if (*id == oid::certificate_policies || *id == "1.3.6.1.4.1.11129.2.4.2" || *id == "2.5.29.9" || *id == "2.5.29.16") {
            // Policies, the SCT list, directory attributes and the private
            // key period: recognised, nothing read.
        } else if (critical) {
            cert.unknown_critical_extension = true;
        }
    }
    return true;
}

bool same_bytes(std::span<std::uint8_t const> a, std::span<std::uint8_t const> b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

}

std::optional<Certificate> parse_certificate(std::span<std::uint8_t const> der)
{
    // Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue BIT STRING }
    DerReader top(der);
    std::optional<DerElement> const outer = top.next(DerType::Sequence);
    if (!outer || !top.at_end())
        return std::nullopt;
    Certificate cert;
    cert.der = to_vector(der);
    DerReader c(outer->content);
    std::optional<DerElement> const tbs = c.next(DerType::Sequence);
    if (!tbs)
        return std::nullopt;
    cert.tbs_offset = static_cast<std::size_t>(tbs->whole.data() - der.data());
    cert.tbs_length = tbs->whole.size();
    std::optional<DerElement> const algorithm_element = c.next(DerType::Sequence);
    std::optional<DerElement> const signature_element = algorithm_element ? c.next(DerType::BitString) : std::nullopt;
    if (!algorithm_element || !signature_element || !c.at_end())
        return std::nullopt;
    std::optional<SignatureAlgorithm> const algorithm = parse_signature_algorithm(*algorithm_element);
    std::optional<std::span<std::uint8_t const>> const signature = der_bit_string(*signature_element);
    if (!algorithm || !signature)
        return std::nullopt;
    cert.signature_algorithm = *algorithm;
    cert.signature = to_vector(*signature);

    // TBSCertificate ::= SEQUENCE { version [0] EXPLICIT, serialNumber,
    // signature, issuer, validity, subject, subjectPublicKeyInfo,
    // issuerUniqueID [1], subjectUniqueID [2], extensions [3] EXPLICIT }
    DerReader t(tbs->content);
    if (t.peek_context(0)) {
        std::optional<DerElement> const wrapper = t.next_context(0);
        if (!wrapper)
            return std::nullopt;
        DerReader w(wrapper->content);
        std::optional<DerElement> const integer = w.next(DerType::Integer);
        std::optional<std::uint64_t> const version = integer ? der_small_integer(*integer) : std::nullopt;
        if (!version || !w.at_end() || *version > 2)
            return std::nullopt;
        cert.version = static_cast<int>(*version) + 1;
    }
    std::optional<DerElement> const serial_element = t.next(DerType::Integer);
    std::optional<std::span<std::uint8_t const>> const serial = serial_element ? der_integer(*serial_element) : std::nullopt;
    if (!serial || serial->size() > 32)
        return std::nullopt;
    cert.serial = to_vector(*serial);
    std::optional<DerElement> const inner_algorithm = t.next(DerType::Sequence);
    if (!inner_algorithm || !same_bytes(inner_algorithm->whole, algorithm_element->whole))
        return std::nullopt; // §4.1.1.2: the two algorithm fields agree
    std::optional<DerElement> const issuer = t.next(DerType::Sequence);
    std::optional<Name> const issuer_name = issuer ? parse_name(*issuer) : std::nullopt;
    if (!issuer_name)
        return std::nullopt;
    cert.issuer = *issuer_name;
    std::optional<DerElement> const validity = t.next(DerType::Sequence);
    if (!validity)
        return std::nullopt;
    DerReader val(validity->content);
    std::optional<DerElement> const before = val.next();
    std::optional<DerElement> const after = before ? val.next() : std::nullopt;
    if (!before || !after || !val.at_end())
        return std::nullopt;
    std::optional<std::int64_t> const not_before = der_time(*before);
    std::optional<std::int64_t> const not_after = der_time(*after);
    if (!not_before || !not_after)
        return std::nullopt;
    cert.not_before = *not_before;
    cert.not_after = *not_after;
    std::optional<DerElement> const subject = t.next(DerType::Sequence);
    std::optional<Name> const subject_name = subject ? parse_name(*subject) : std::nullopt;
    if (!subject_name)
        return std::nullopt;
    cert.subject = *subject_name;
    std::optional<DerElement> const spki = t.next(DerType::Sequence);
    std::optional<PublicKey> const key = spki ? parse_public_key(spki->whole) : std::nullopt;
    if (!key)
        return std::nullopt;
    cert.public_key = *key;
    if (t.peek_context(1))
        t.next();
    if (t.peek_context(2))
        t.next();
    if (t.peek_context(3)) {
        if (cert.version != 3)
            return std::nullopt;
        std::optional<DerElement> const wrapper = t.next_context(3);
        if (!wrapper)
            return std::nullopt;
        DerReader w(wrapper->content);
        std::optional<DerElement> const list = w.next(DerType::Sequence);
        if (!list || !w.at_end() || !parse_extensions(cert, list->content))
            return std::nullopt;
    }
    if (!t.at_end())
        return std::nullopt;
    return cert;
}

std::optional<Crl> parse_crl(std::span<std::uint8_t const> der)
{
    // CertificateList ::= SEQUENCE { tbsCertList, signatureAlgorithm, signatureValue }
    DerReader top(der);
    std::optional<DerElement> const outer = top.next(DerType::Sequence);
    if (!outer || !top.at_end())
        return std::nullopt;
    Crl crl;
    crl.der = to_vector(der);
    DerReader c(outer->content);
    std::optional<DerElement> const tbs = c.next(DerType::Sequence);
    if (!tbs)
        return std::nullopt;
    crl.tbs_offset = static_cast<std::size_t>(tbs->whole.data() - der.data());
    crl.tbs_length = tbs->whole.size();
    std::optional<DerElement> const algorithm_element = c.next(DerType::Sequence);
    std::optional<DerElement> const signature_element = algorithm_element ? c.next(DerType::BitString) : std::nullopt;
    if (!algorithm_element || !signature_element || !c.at_end())
        return std::nullopt;
    std::optional<SignatureAlgorithm> const algorithm = parse_signature_algorithm(*algorithm_element);
    std::optional<std::span<std::uint8_t const>> const signature = der_bit_string(*signature_element);
    if (!algorithm || !signature)
        return std::nullopt;
    crl.signature_algorithm = *algorithm;
    crl.signature = to_vector(*signature);

    // TBSCertList ::= SEQUENCE { version INTEGER OPTIONAL, signature,
    // issuer, thisUpdate, nextUpdate OPTIONAL, revokedCertificates
    // OPTIONAL, crlExtensions [0] EXPLICIT OPTIONAL }
    DerReader t(tbs->content);
    if (std::optional<DerTag> const tag = t.peek_tag(); tag && tag->is(DerType::Integer)) {
        std::optional<DerElement> const integer = t.next();
        std::optional<std::uint64_t> const version = integer ? der_small_integer(*integer) : std::nullopt;
        if (!version || *version != 1)
            return std::nullopt;
    }
    std::optional<DerElement> const inner_algorithm = t.next(DerType::Sequence);
    if (!inner_algorithm || !same_bytes(inner_algorithm->whole, algorithm_element->whole))
        return std::nullopt;
    std::optional<DerElement> const issuer = t.next(DerType::Sequence);
    std::optional<Name> const issuer_name = issuer ? parse_name(*issuer) : std::nullopt;
    if (!issuer_name)
        return std::nullopt;
    crl.issuer = *issuer_name;
    std::optional<DerElement> const this_update = t.next();
    std::optional<std::int64_t> const this_time = this_update ? der_time(*this_update) : std::nullopt;
    if (!this_time)
        return std::nullopt;
    crl.this_update = *this_time;
    if (std::optional<DerTag> const tag = t.peek_tag(); tag && (tag->is(DerType::UtcTime) || tag->is(DerType::GeneralizedTime))) {
        std::optional<DerElement> const time_element = t.next();
        std::optional<std::int64_t> const next_time = time_element ? der_time(*time_element) : std::nullopt;
        if (!next_time)
            return std::nullopt;
        crl.next_update = *next_time;
    }
    if (std::optional<DerTag> const tag = t.peek_tag(); tag && tag->is(DerType::Sequence)) {
        std::optional<DerElement> const revoked = t.next();
        if (!revoked)
            return std::nullopt;
        DerReader entries(revoked->content);
        while (!entries.at_end()) {
            std::optional<DerElement> const entry = entries.next(DerType::Sequence);
            if (!entry)
                return std::nullopt;
            DerReader e(entry->content);
            std::optional<DerElement> const serial_element = e.next(DerType::Integer);
            std::optional<std::span<std::uint8_t const>> const serial = serial_element ? der_integer(*serial_element) : std::nullopt;
            std::optional<DerElement> const date = serial ? e.next() : std::nullopt;
            if (!serial || !date || !der_time(*date))
                return std::nullopt;
            crl.revoked_serials.push_back(to_vector(*serial));
        }
    }
    if (t.peek_context(0)) {
        std::optional<DerElement> const wrapper = t.next_context(0);
        if (!wrapper)
            return std::nullopt;
        DerReader w(wrapper->content);
        std::optional<DerElement> const list = w.next(DerType::Sequence);
        if (!list || !w.at_end())
            return std::nullopt;
        DerReader x(list->content);
        while (!x.at_end()) {
            std::optional<DerElement> const extension = x.next(DerType::Sequence);
            if (!extension)
                return std::nullopt;
            DerReader e(extension->content);
            std::optional<DerElement> const id_element = e.next(DerType::Oid);
            std::optional<std::string> const id = id_element ? der_oid(*id_element) : std::nullopt;
            if (!id)
                return std::nullopt;
            bool critical = false;
            if (std::optional<DerTag> const tag = e.peek_tag(); tag && tag->is(DerType::Boolean)) {
                std::optional<DerElement> const boolean = e.next();
                std::optional<bool> const flag = boolean ? der_boolean(*boolean) : std::nullopt;
                if (!flag)
                    return std::nullopt;
                critical = *flag;
            }
            std::optional<DerElement> const value = e.next(DerType::OctetString);
            if (!value || !e.at_end())
                return std::nullopt;
            if (*id == oid::authority_key_identifier) {
                DerReader v(value->content);
                std::optional<DerElement> const sequence = v.next(DerType::Sequence);
                if (!sequence)
                    return std::nullopt;
                DerReader s(sequence->content);
                if (std::optional<DerElement> const key_id = s.next_context(0))
                    crl.authority_key_identifier = to_vector(key_id->content);
            } else if (*id == oid::crl_number) {
                // cRLNumber: nothing read.
            } else if (*id == oid::issuing_distribution_point) {
                // IssuingDistributionPoint ::= SEQUENCE { distributionPoint [0]
                // DistributionPointName OPTIONAL, onlyContainsUserCerts [1]
                // BOOLEAN DEFAULT FALSE, onlyContainsCACerts [2] BOOLEAN DEFAULT
                // FALSE, onlySomeReasons [3] ReasonFlags OPTIONAL, indirectCRL
                // [4] BOOLEAN DEFAULT FALSE, onlyContainsAttributeCerts [5]
                // BOOLEAN DEFAULT FALSE } — the booleans are implicitly tagged,
                // so their content is the one byte DER allows.
                Crl::IssuingDistributionPoint point;
                DerReader v(value->content);
                std::optional<DerElement> const sequence = v.next(DerType::Sequence);
                if (!sequence || !v.at_end())
                    return std::nullopt;
                DerReader s(sequence->content);
                if (std::optional<DerElement> const name = s.next_context(0)) {
                    point.has_point = true;
                    DerReader n(name->content);
                    if (std::optional<DerElement> const full = n.next_context(0)) {
                        std::optional<GeneralNames> const names = parse_general_names(full->content);
                        if (!names)
                            return std::nullopt;
                        for (std::string const& uri : names->uris) {
                            if (uri.starts_with("http://"))
                                point.uris.push_back(uri);
                        }
                    }
                    // A nameRelativeToCRLIssuer [1] is a name this reader
                    // cannot match a certificate's point against: the point
                    // stays named with no URL, which covers_leaf reads as
                    // no information.
                }
                auto flag = [&](std::uint8_t number, bool& out) {
                    if (std::optional<DerElement> const element = s.next_context(number)) {
                        if (element->content.size() != 1 || (element->content[0] != 0x00 && element->content[0] != 0xff))
                            return false;
                        out = element->content[0] == 0xff;
                    }
                    return true;
                };
                if (!flag(1, point.only_user_certs) || !flag(2, point.only_ca_certs))
                    return std::nullopt;
                if (s.next_context(3))
                    point.only_some_reasons = true;
                if (!flag(4, point.indirect) || !flag(5, point.only_attribute_certs) || !s.at_end())
                    return std::nullopt;
                crl.issuing_distribution_point = point;
            } else if (critical) {
                return std::nullopt; // §5.2: an unrecognised critical extension makes the CRL unusable (delta CRLs included)
            }
        }
    }
    if (!t.at_end())
        return std::nullopt;
    return crl;
}

bool Crl::revokes(std::span<std::uint8_t const> serial) const
{
    for (std::vector<std::uint8_t> const& revoked : revoked_serials) {
        if (same_bytes(revoked, serial))
            return true;
    }
    return false;
}

bool Crl::covers_leaf(std::string const& point) const
{
    if (!issuing_distribution_point)
        return true;
    IssuingDistributionPoint const& idp = *issuing_distribution_point;
    if (idp.only_ca_certs || idp.only_attribute_certs || idp.indirect)
        return false;
    if (idp.has_point)
        return std::find(idp.uris.begin(), idp.uris.end(), point) != idp.uris.end();
    return true;
}

std::vector<std::vector<std::uint8_t>> pem_decode(std::string_view text, std::string_view label)
{
    std::vector<std::vector<std::uint8_t>> out;
    std::string const begin = "-----BEGIN " + std::string(label) + "-----";
    std::string const end = "-----END " + std::string(label) + "-----";
    std::size_t offset = 0;
    for (;;) {
        std::size_t const start = text.find(begin, offset);
        if (start == std::string_view::npos)
            break;
        std::size_t const body = start + begin.size();
        std::size_t const stop = text.find(end, body);
        if (stop == std::string_view::npos)
            break;
        if (std::optional<std::vector<std::uint8_t>> bytes = base64_decode(text.substr(body, stop - body)))
            out.push_back(std::move(*bytes));
        offset = stop + end.size();
    }
    return out;
}

bool verify_signature(PublicKey const& signer, SignatureAlgorithm const& algorithm, std::span<std::uint8_t const> signed_bytes, std::span<std::uint8_t const> signature)
{
    std::vector<std::uint8_t> const digest = crypto::hash_with(algorithm.hash, signed_bytes);
    switch (algorithm.kind) {
    case SignatureKind::RsaPkcs1:
        return signer.kind == PublicKey::Kind::Rsa && crypto::rsa_verify_pkcs1_v15(signer.rsa, algorithm.hash, digest, signature);
    case SignatureKind::RsaPss:
        return signer.kind == PublicKey::Kind::Rsa && crypto::rsa_verify_pss(signer.rsa, algorithm.hash, digest, signature, algorithm.pss_salt_length);
    case SignatureKind::Ecdsa: {
        if (signer.kind != PublicKey::Kind::Ec)
            return false;
        // Ecdsa-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER }
        DerReader top(signature);
        std::optional<DerElement> const sequence = top.next(DerType::Sequence);
        if (!sequence || !top.at_end())
            return false;
        DerReader s(sequence->content);
        std::optional<DerElement> const r_element = s.next(DerType::Integer);
        std::optional<DerElement> const s_element = r_element ? s.next(DerType::Integer) : std::nullopt;
        if (!r_element || !s_element || !s.at_end())
            return false;
        std::optional<std::span<std::uint8_t const>> const r_bytes = der_integer(*r_element);
        std::optional<std::span<std::uint8_t const>> const s_bytes = der_integer(*s_element);
        if (!r_bytes || !s_bytes)
            return false;
        std::optional<crypto::BigInt> const r = crypto::BigInt::from_bytes(*r_bytes);
        std::optional<crypto::BigInt> const sv = crypto::BigInt::from_bytes(*s_bytes);
        if (!r || !sv)
            return false;
        return crypto::ecdsa_verify(signer.curve, signer.ec, digest, *r, *sv);
    }
    case SignatureKind::Unsupported:
        return false;
    }
    return false;
}

}
