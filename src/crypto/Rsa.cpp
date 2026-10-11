#include "crypto/Rsa.h"

#include <algorithm>
#include <array>
#include <bit>
#include <vector>

namespace sashfold::crypto {

namespace {

// The DER DigestInfo prefixes of §9.2 note 1, one per hash.
std::span<std::uint8_t const> digest_info_prefix(HashId hash)
{
    static constexpr std::uint8_t sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static constexpr std::uint8_t sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
    static constexpr std::uint8_t sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    static constexpr std::uint8_t sha1[] = { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 };
    switch (hash) {
    case HashId::Sha256:
        return sha256;
    case HashId::Sha384:
        return sha384;
    case HashId::Sha512:
        return sha512;
    case HashId::Sha1:
        return sha1;
    }
    return {};
}

// RSAVP1 (§5.2.2) followed by I2OSP to `size` bytes; nullopt when the
// signature is out of range.
std::optional<std::vector<std::uint8_t>> encoded_message(RsaPublicKey const& key, std::span<std::uint8_t const> signature, std::size_t size)
{
    std::optional<BigInt> const s = BigInt::from_bytes(signature);
    if (!s || s->compare(key.n) >= 0)
        return std::nullopt;
    return s->mod_pow(key.e, key.n).to_bytes(size);
}

// MGF1 (§B.2.1): Hash(seed || counter) for counter = 0, 1, … until `length` bytes.
std::vector<std::uint8_t> mgf1(HashId hash, std::span<std::uint8_t const> seed, std::size_t length)
{
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> input(seed.begin(), seed.end());
    input.resize(seed.size() + 4);
    for (std::uint32_t counter = 0; out.size() < length; ++counter) {
        input[seed.size()] = static_cast<std::uint8_t>(counter >> 24);
        input[seed.size() + 1] = static_cast<std::uint8_t>(counter >> 16);
        input[seed.size() + 2] = static_cast<std::uint8_t>(counter >> 8);
        input[seed.size() + 3] = static_cast<std::uint8_t>(counter);
        std::vector<std::uint8_t> const block = hash_with(hash, input);
        out.insert(out.end(), block.begin(), block.end());
    }
    out.resize(length);
    return out;
}


using u64 = std::uint64_t;
__extension__ typedef unsigned __int128 u128;

// All ones when v is zero, else zero; no branch.
u64 zero_mask(u64 v)
{
    return ((v | (0 - v)) >> 63) - 1;
}

// Montgomery arithmetic modulo an odd m of k 64-bit limbs, the limbs least
// significant first. Every operation runs the same steps whatever the
// values are.
class Montgomery {
public:
    using Limbs = std::vector<u64>;

    explicit Montgomery(BigInt const& modulus)
        : m_k((modulus.bit_length() + 63) / 64)
        , m_modulus(modulus)
    {
        m_m = limbs(modulus);
        u64 inverse = m_m[0];
        for (int i = 0; i < 5; ++i)
            inverse *= 2 - m_m[0] * inverse;
        m_m0_inverse = 0 - inverse;
        BigInt const r = BigInt::from_u64(1).shift_left(64 * m_k).mod(modulus);
        m_one = limbs(r);
        m_r2 = limbs(r.mul(r).mod(modulus));
    }

    std::size_t size() const { return m_k; }
    Limbs const& one() const { return m_one; }

    Limbs limbs(BigInt const& value) const
    {
        Limbs out(m_k, 0);
        std::vector<std::uint8_t> const bytes = *value.to_bytes(8 * m_k);
        for (std::size_t i = 0; i < 8 * m_k; ++i)
            out[i / 8] |= u64 { bytes[8 * m_k - 1 - i] } << (8 * (i % 8));
        return out;
    }

    BigInt big(Limbs const& value) const
    {
        std::vector<std::uint8_t> bytes(8 * m_k);
        for (std::size_t i = 0; i < 8 * m_k; ++i)
            bytes[8 * m_k - 1 - i] = static_cast<std::uint8_t>(value[i / 8] >> (8 * (i % 8)));
        return *BigInt::from_bytes(bytes);
    }

    // a·b·R⁻¹ mod m by coarsely integrated operand scanning, as Ec.cpp's
    // fixed-width field does, with the same masked final subtraction.
    Limbs mul(Limbs const& a, Limbs const& b) const
    {
        std::size_t const k = m_k;
        std::vector<u64> t(k + 2, 0);
        for (std::size_t i = 0; i < k; ++i) {
            u64 carry = 0;
            for (std::size_t j = 0; j < k; ++j) {
                u128 const s = static_cast<u128>(a[j]) * b[i] + t[j] + carry;
                t[j] = static_cast<u64>(s);
                carry = static_cast<u64>(s >> 64);
            }
            u128 s = static_cast<u128>(t[k]) + carry;
            t[k] = static_cast<u64>(s);
            t[k + 1] = static_cast<u64>(s >> 64);
            u64 const q = t[0] * m_m0_inverse;
            s = static_cast<u128>(q) * m_m[0] + t[0];
            carry = static_cast<u64>(s >> 64);
            for (std::size_t j = 1; j < k; ++j) {
                s = static_cast<u128>(q) * m_m[j] + t[j] + carry;
                t[j - 1] = static_cast<u64>(s);
                carry = static_cast<u64>(s >> 64);
            }
            s = static_cast<u128>(t[k]) + carry;
            t[k - 1] = static_cast<u64>(s);
            t[k] = t[k + 1] + static_cast<u64>(s >> 64);
        }
        Limbs reduced(k);
        u64 borrow = 0;
        for (std::size_t i = 0; i < k; ++i) {
            u128 const d = static_cast<u128>(t[i]) - m_m[i] - borrow;
            reduced[i] = static_cast<u64>(d);
            borrow = static_cast<u64>(d >> 64) & 1;
        }
        // Keep the subtraction when there was a carry out or no borrow.
        u64 const keep = ~zero_mask(t[k] | (borrow ^ 1));
        Limbs out(k);
        for (std::size_t i = 0; i < k; ++i)
            out[i] = (reduced[i] & keep) | (t[i] & ~keep);
        return out;
    }

    // Into Montgomery form, from any value (reduced first).
    Limbs to(BigInt const& value) const { return mul(limbs(value.compare(m_modulus) >= 0 ? value.mod(m_modulus) : value), m_r2); }

    BigInt from(Limbs const& value) const
    {
        Limbs unit(m_k, 0);
        unit[0] = 1;
        return big(mul(value, unit));
    }

    // base^exponent for base in Montgomery form, by fixed 4-bit windows over
    // as many limbs as the modulus has; the table is read whole at every
    // window, so the exponent's bits choose no address.
    Limbs pow(Limbs const& base, BigInt const& exponent) const
    {
        std::array<Limbs, 16> table;
        table[0] = m_one;
        table[1] = base;
        for (std::size_t i = 2; i < 16; ++i)
            table[i] = mul(table[i - 1], base);
        Limbs const e = limbs_wide(exponent);
        Limbs r = m_one;
        Limbs entry(m_k);
        for (std::size_t w = 16 * e.size(); w-- > 0;) {
            for (int i = 0; i < 4; ++i)
                r = mul(r, r);
            u64 const nibble = (e[w / 16] >> (4 * (w % 16))) & 15;
            std::fill(entry.begin(), entry.end(), 0);
            for (u64 i = 0; i < 16; ++i) {
                u64 const mask = zero_mask(i ^ nibble);
                for (std::size_t j = 0; j < m_k; ++j)
                    entry[j] |= table[i][j] & mask;
            }
            r = mul(r, entry);
        }
        return r;
    }

private:
    // An exponent's limbs, as many as the modulus has or more if it is wider.
    Limbs limbs_wide(BigInt const& value) const
    {
        std::size_t const k = std::max(m_k, (value.bit_length() + 63) / 64);
        Limbs out(k, 0);
        std::vector<std::uint8_t> const bytes = *value.to_bytes(8 * k);
        for (std::size_t i = 0; i < 8 * k; ++i)
            out[i / 8] |= u64 { bytes[8 * k - 1 - i] } << (8 * (i % 8));
        return out;
    }

    std::size_t m_k;
    BigInt m_modulus;
    Limbs m_m;
    u64 m_m0_inverse = 0;
    Limbs m_one;
    Limbs m_r2;
};

BigInt gcd(BigInt a, BigInt b)
{
    while (!b.is_zero()) {
        BigInt const r = a.mod(b);
        a = b;
        b = r;
    }
    return a;
}

std::size_t byte_length(BigInt const& n)
{
    return (n.bit_length() + 7) / 8;
}

bool modulus_in_range(BigInt const& n)
{
    std::size_t const bits = n.bit_length();
    return n.is_odd() && bits >= 512 && bits <= BigInt::max_bits / 2;
}

// RSADP / RSASP1 (§5.1.2, §5.2.1) by the Chinese remainder theorem, checked
// against the public exponent before it is returned.
std::optional<BigInt> private_operation(RsaPrivateKey const& key, BigInt const& c)
{
    if (c.compare(key.n) >= 0)
        return std::nullopt;
    Montgomery const mp(key.p);
    Montgomery const mq(key.q);
    BigInt const m1 = mp.from(mp.pow(mp.to(c), key.dp));
    BigInt const m2 = mq.from(mq.pow(mq.to(c), key.dq));
    BigInt const h = key.qi.mod_mul(m1.mod_sub(m2.mod(key.p), key.p), key.p);
    BigInt const m = m2.add(h.mul(key.q));
    Montgomery const mn(key.n);
    if (!(mn.from(mn.pow(mn.to(m), key.e)) == c))
        return std::nullopt;
    return m;
}

BigInt public_operation(RsaPublicKey const& key, BigInt const& m)
{
    Montgomery const mn(key.n);
    return mn.from(mn.pow(mn.to(m), key.e));
}

// The odd primes below 2048, for trial division before Miller–Rabin.
std::vector<std::uint32_t> const& small_primes()
{
    static std::vector<std::uint32_t> const primes = [] {
        std::vector<std::uint32_t> out;
        for (std::uint32_t candidate = 3; candidate < 2048; candidate += 2) {
            bool prime = true;
            for (std::uint32_t const p : out) {
                if (p * p > candidate)
                    break;
                if (candidate % p == 0) {
                    prime = false;
                    break;
                }
            }
            if (prime)
                out.push_back(candidate);
        }
        return out;
    }();
    return primes;
}

// Miller–Rabin (FIPS 186-5 §B.3.1) with `rounds` random bases.
bool probably_prime(BigInt const& w, std::size_t rounds, RandomFill random)
{
    BigInt const one = BigInt::from_u64(1);
    BigInt const w1 = w.sub(one);
    std::size_t a = 0;
    while (!w1.bit(a))
        ++a;
    BigInt const m = w1.shift_right(a);
    Montgomery const mont(w);
    Montgomery::Limbs const mont_one = mont.one();
    Montgomery::Limbs const mont_minus_one = mont.to(w1);
    std::size_t const length = byte_length(w);
    for (std::size_t round = 0; round < rounds; ++round) {
        BigInt b;
        do {
            std::vector<std::uint8_t> bytes(length);
            random(bytes);
            b = BigInt::from_bytes(bytes)->mod(w);
        } while (b.compare(BigInt::from_u64(2)) < 0 || b.compare(w1) >= 0);
        Montgomery::Limbs z = mont.pow(mont.to(b), m);
        if (z == mont_one || z == mont_minus_one)
            continue;
        bool composite = true;
        for (std::size_t j = 1; j < a; ++j) {
            z = mont.mul(z, z);
            if (z == mont_minus_one) {
                composite = false;
                break;
            }
            if (z == mont_one)
                break;
        }
        if (composite)
            return false;
    }
    return true;
}

// A random prime of exactly `bits` bits with its top two bits set (so the
// product of two has the full length, §A.1.3) and gcd(p − 1, e) = 1.
BigInt random_prime(std::size_t bits, BigInt const& e, RandomFill random)
{
    std::size_t const length = (bits + 7) / 8;
    std::size_t const extra = 8 * length - bits;
    // Rounds for an error below 2^-100 on random candidates (FIPS 186-5
    // table B.1 asks fewer for larger primes).
    std::size_t const rounds = bits >= 1024 ? 5 : bits >= 512 ? 8 : 40;
    BigInt const one = BigInt::from_u64(1);
    for (;;) {
        std::vector<std::uint8_t> bytes(length);
        random(bytes);
        bytes[0] = static_cast<std::uint8_t>(bytes[0] & (0xffu >> extra));
        bytes[0] = static_cast<std::uint8_t>(bytes[0] | (0xc0u >> extra));
        if (extra == 7)
            bytes[1] = static_cast<std::uint8_t>(bytes[1] | 0x80u);
        bytes.back() = static_cast<std::uint8_t>(bytes.back() | 1u);
        bool divisible = false;
        for (std::uint32_t const p : small_primes()) {
            std::uint32_t residue = 0;
            for (std::uint8_t const byte : bytes)
                residue = (residue * 256 + byte) % p;
            if (residue == 0) {
                divisible = true;
                break;
            }
        }
        if (divisible)
            continue;
        BigInt const candidate = *BigInt::from_bytes(bytes);
        if (!(gcd(candidate.sub(one), e) == one))
            continue;
        if (probably_prime(candidate, rounds, random))
            return candidate;
    }
}

// The EMSA-PKCS1-v1_5 encoding (§9.2) to k bytes; nullopt when k is short.
std::optional<std::vector<std::uint8_t>> pkcs1_encoding(HashId hash, std::span<std::uint8_t const> digest, std::size_t k)
{
    std::span<std::uint8_t const> const prefix = digest_info_prefix(hash);
    std::size_t const t_length = prefix.size() + digest.size();
    if (k < t_length + 11)
        return std::nullopt;
    std::vector<std::uint8_t> em;
    em.reserve(k);
    em.push_back(0x00);
    em.push_back(0x01);
    em.insert(em.end(), k - t_length - 3, 0xff);
    em.push_back(0x00);
    em.insert(em.end(), prefix.begin(), prefix.end());
    em.insert(em.end(), digest.begin(), digest.end());
    return em;
}

bool verify_pkcs1(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature)
{
    if (digest.size() != digest_size(hash))
        return false;
    std::size_t const k = byte_length(key.n);
    if (signature.size() != k)
        return false;
    std::optional<std::vector<std::uint8_t>> const em = encoded_message(key, signature, k);
    if (!em)
        return false;
    std::optional<std::vector<std::uint8_t>> const expected = pkcs1_encoding(hash, digest, k);
    return expected && *em == *expected;
}

bool verify_pss(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature, std::size_t salt_length)
{
    if (digest.size() != digest_size(hash))
        return false;
    std::size_t const mod_bits = key.n.bit_length();
    std::size_t const k = (mod_bits + 7) / 8;
    if (signature.size() != k)
        return false;
    // EMSA-PSS-VERIFY (§9.1.2) over emBits = modBits − 1.
    std::size_t const em_bits = mod_bits - 1;
    std::size_t const em_len = (em_bits + 7) / 8;
    std::optional<std::vector<std::uint8_t>> const em_full = encoded_message(key, signature, k);
    if (!em_full)
        return false;
    std::span<std::uint8_t const> em(*em_full);
    if (em_len < k) {
        if (em[0] != 0)
            return false;
        em = em.subspan(k - em_len);
    }
    std::size_t const h_len = digest.size();
    if (em_len < h_len + salt_length + 2)
        return false;
    if (em[em_len - 1] != 0xbc)
        return false;
    std::span<std::uint8_t const> const masked_db = em.subspan(0, em_len - h_len - 1);
    std::span<std::uint8_t const> const h = em.subspan(em_len - h_len - 1, h_len);
    std::size_t const unused_bits = 8 * em_len - em_bits;
    if (unused_bits > 0 && (masked_db[0] >> (8 - unused_bits)) != 0)
        return false;
    std::vector<std::uint8_t> db = mgf1(hash, h, masked_db.size());
    for (std::size_t i = 0; i < db.size(); ++i)
        db[i] = static_cast<std::uint8_t>(db[i] ^ masked_db[i]);
    if (unused_bits > 0)
        db[0] = static_cast<std::uint8_t>(db[0] & (0xff >> unused_bits));
    std::size_t const ps_length = em_len - h_len - salt_length - 2;
    for (std::size_t i = 0; i < ps_length; ++i) {
        if (db[i] != 0)
            return false;
    }
    if (db[ps_length] != 0x01)
        return false;
    std::span<std::uint8_t const> const salt = std::span<std::uint8_t const>(db).subspan(ps_length + 1, salt_length);
    // M' = eight zero bytes || mHash || salt; H must equal Hash(M').
    std::vector<std::uint8_t> m_prime(8, 0);
    m_prime.insert(m_prime.end(), digest.begin(), digest.end());
    m_prime.insert(m_prime.end(), salt.begin(), salt.end());
    std::vector<std::uint8_t> const h_prime = hash_with(hash, m_prime);
    return std::equal(h.begin(), h.end(), h_prime.begin(), h_prime.end());
}

}

bool rsa_key_acceptable(RsaPublicKey const& key)
{
    std::size_t const bits = key.n.bit_length();
    if (bits < 2048 || bits > BigInt::max_bits / 2)
        return false;
    if (!key.n.is_odd())
        return false;
    if (!key.e.is_odd() || key.e.bit_length() > 64 || key.e.compare(BigInt::from_u64(3)) < 0)
        return false;
    return true;
}

bool rsa_verify_pkcs1_v15(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature)
{
    return rsa_key_acceptable(key) && verify_pkcs1(key, hash, digest, signature);
}

bool rsa_verify_pkcs1_v15_any(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature)
{
    return modulus_in_range(key.n) && verify_pkcs1(key, hash, digest, signature);
}

bool rsa_verify_pss(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature, std::size_t salt_length)
{
    return rsa_key_acceptable(key) && verify_pss(key, hash, digest, signature, salt_length);
}

bool rsa_verify_pss_any(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::span<std::uint8_t const> signature, std::size_t salt_length)
{
    return modulus_in_range(key.n) && verify_pss(key, hash, digest, signature, salt_length);
}

std::optional<BigInt> mod_inverse(BigInt const& a, BigInt const& m)
{
    if (m.is_zero())
        return std::nullopt;
    BigInt r0 = m;
    BigInt r1 = a.mod(m);
    BigInt t0;
    BigInt t1 = BigInt::from_u64(1).mod(m);
    while (!r1.is_zero()) {
        BigInt q;
        BigInt r;
        r0.divmod(r1, q, r);
        r0 = r1;
        r1 = r;
        BigInt const next = t0.mod_sub(q.mod(m).mod_mul(t1, m), m);
        t0 = t1;
        t1 = next;
    }
    if (!(r0 == BigInt::from_u64(1)))
        return std::nullopt;
    return t0;
}

std::optional<RsaPrivateKey> rsa_generate(std::size_t bits, BigInt const& e, RandomFill random)
{
    if (bits < 512 || bits > BigInt::max_bits / 2 || bits % 8 != 0)
        return std::nullopt;
    if (!e.is_odd() || e.compare(BigInt::from_u64(3)) < 0)
        return std::nullopt;
    BigInt const one = BigInt::from_u64(1);
    std::size_t const p_bits = (bits + 1) / 2;
    std::size_t const q_bits = bits - p_bits;
    for (;;) {
        BigInt p = random_prime(p_bits, e, random);
        BigInt q = random_prime(q_bits, e, random);
        if (p == q)
            continue;
        // |p − q| must not be small (§A.1.3 step 5.4): its top 100 bits.
        BigInt const difference = p.compare(q) > 0 ? p.sub(q) : q.sub(p);
        if (bits >= 400 && difference.bit_length() <= p_bits - 100)
            continue;
        if (p.compare(q) < 0)
            std::swap(p, q);
        BigInt const n = p.mul(q);
        if (n.bit_length() != bits)
            continue;
        BigInt const p1 = p.sub(one);
        BigInt const q1 = q.sub(one);
        BigInt quotient;
        BigInt remainder;
        p1.mul(q1).divmod(gcd(p1, q1), quotient, remainder);
        std::optional<BigInt> const d = mod_inverse(e, quotient);
        std::optional<BigInt> const qi = mod_inverse(q, p);
        if (!d || !qi)
            continue;
        return RsaPrivateKey { n, e, *d, p, q, d->mod(p1), d->mod(q1), *qi };
    }
}

std::optional<RsaPrivateKey> rsa_private_key(BigInt const& n, BigInt const& e, BigInt const& d, BigInt const& p, BigInt const& q,
    BigInt const& dp, BigInt const& dq, BigInt const& qi)
{
    BigInt const one = BigInt::from_u64(1);
    if (!modulus_in_range(n) || !e.is_odd() || e.compare(BigInt::from_u64(3)) < 0 || e.compare(n) >= 0)
        return std::nullopt;
    if (p.compare(one) <= 0 || q.compare(one) <= 0 || !p.is_odd() || !q.is_odd() || !(p.mul(q) == n))
        return std::nullopt;
    BigInt const p1 = p.sub(one);
    BigInt const q1 = q.sub(one);
    if (!(d.mod(p1) == dp) || !(d.mod(q1) == dq) || !(q.mod_mul(qi, p) == one))
        return std::nullopt;
    if (!(e.mod_mul(dp, p1) == one) || !(e.mod_mul(dq, q1) == one))
        return std::nullopt;
    return RsaPrivateKey { n, e, d, p, q, dp, dq, qi };
}

std::optional<std::vector<std::uint8_t>> rsa_sign_pkcs1_v15(RsaPrivateKey const& key, HashId hash, std::span<std::uint8_t const> digest)
{
    std::size_t const k = byte_length(key.n);
    std::optional<std::vector<std::uint8_t>> const em = pkcs1_encoding(hash, digest, k);
    if (!em)
        return std::nullopt;
    std::optional<BigInt> const s = private_operation(key, *BigInt::from_bytes(*em));
    if (!s)
        return std::nullopt;
    return s->to_bytes(k);
}

std::optional<std::vector<std::uint8_t>> rsa_sign_pss(RsaPrivateKey const& key, HashId hash, std::span<std::uint8_t const> digest, std::size_t salt_length, RandomFill random)
{
    // EMSA-PSS-ENCODE (§9.1.1) over emBits = modBits − 1.
    std::size_t const mod_bits = key.n.bit_length();
    std::size_t const k = (mod_bits + 7) / 8;
    std::size_t const em_bits = mod_bits - 1;
    std::size_t const em_len = (em_bits + 7) / 8;
    std::size_t const h_len = digest.size();
    if (em_len < h_len + salt_length + 2)
        return std::nullopt;
    std::vector<std::uint8_t> salt(salt_length);
    if (salt_length > 0)
        random(salt);
    std::vector<std::uint8_t> m_prime(8, 0);
    m_prime.insert(m_prime.end(), digest.begin(), digest.end());
    m_prime.insert(m_prime.end(), salt.begin(), salt.end());
    std::vector<std::uint8_t> const h = hash_with(hash, m_prime);
    std::vector<std::uint8_t> db(em_len - salt_length - h_len - 2, 0);
    db.push_back(0x01);
    db.insert(db.end(), salt.begin(), salt.end());
    std::vector<std::uint8_t> const mask = mgf1(hash, h, db.size());
    for (std::size_t i = 0; i < db.size(); ++i)
        db[i] = static_cast<std::uint8_t>(db[i] ^ mask[i]);
    std::size_t const unused_bits = 8 * em_len - em_bits;
    if (unused_bits > 0)
        db[0] = static_cast<std::uint8_t>(db[0] & (0xff >> unused_bits));
    std::vector<std::uint8_t> em = std::move(db);
    em.insert(em.end(), h.begin(), h.end());
    em.push_back(0xbc);
    std::optional<BigInt> const s = private_operation(key, *BigInt::from_bytes(em));
    if (!s)
        return std::nullopt;
    return s->to_bytes(k);
}

std::optional<std::vector<std::uint8_t>> rsa_oaep_encrypt(RsaPublicKey const& key, HashId hash, std::span<std::uint8_t const> label,
    std::span<std::uint8_t const> message, RandomFill random)
{
    if (!modulus_in_range(key.n))
        return std::nullopt;
    std::size_t const k = byte_length(key.n);
    std::size_t const h_len = digest_size(hash);
    if (k < 2 * h_len + 2 || message.size() > k - 2 * h_len - 2)
        return std::nullopt;
    std::vector<std::uint8_t> db = hash_with(hash, label);
    db.insert(db.end(), k - message.size() - 2 * h_len - 2, 0);
    db.push_back(0x01);
    db.insert(db.end(), message.begin(), message.end());
    std::vector<std::uint8_t> seed(h_len);
    random(seed);
    std::vector<std::uint8_t> const db_mask = mgf1(hash, seed, db.size());
    for (std::size_t i = 0; i < db.size(); ++i)
        db[i] = static_cast<std::uint8_t>(db[i] ^ db_mask[i]);
    std::vector<std::uint8_t> const seed_mask = mgf1(hash, db, h_len);
    for (std::size_t i = 0; i < h_len; ++i)
        seed[i] = static_cast<std::uint8_t>(seed[i] ^ seed_mask[i]);
    std::vector<std::uint8_t> em { 0x00 };
    em.insert(em.end(), seed.begin(), seed.end());
    em.insert(em.end(), db.begin(), db.end());
    return public_operation(key, *BigInt::from_bytes(em)).to_bytes(k);
}

// RSAES-OAEP-DECRYPT (§7.1.2). The checks of step 3g are gathered into one
// flag over every byte, so the time taken does not say which one failed
// (Manger's attack reads exactly that).
std::optional<std::vector<std::uint8_t>> rsa_oaep_decrypt(RsaPrivateKey const& key, HashId hash, std::span<std::uint8_t const> label,
    std::span<std::uint8_t const> ciphertext)
{
    std::size_t const k = byte_length(key.n);
    std::size_t const h_len = digest_size(hash);
    if (ciphertext.size() != k || k < 2 * h_len + 2)
        return std::nullopt;
    std::optional<BigInt> const c = BigInt::from_bytes(ciphertext);
    if (!c)
        return std::nullopt;
    std::optional<BigInt> const m = private_operation(key, *c);
    if (!m)
        return std::nullopt;
    std::vector<std::uint8_t> const em = *m->to_bytes(k);
    std::vector<std::uint8_t> seed(em.begin() + 1, em.begin() + 1 + static_cast<std::ptrdiff_t>(h_len));
    std::vector<std::uint8_t> db(em.begin() + 1 + static_cast<std::ptrdiff_t>(h_len), em.end());
    std::vector<std::uint8_t> const seed_mask = mgf1(hash, db, h_len);
    for (std::size_t i = 0; i < h_len; ++i)
        seed[i] = static_cast<std::uint8_t>(seed[i] ^ seed_mask[i]);
    std::vector<std::uint8_t> const db_mask = mgf1(hash, seed, db.size());
    for (std::size_t i = 0; i < db.size(); ++i)
        db[i] = static_cast<std::uint8_t>(db[i] ^ db_mask[i]);
    std::vector<std::uint8_t> const l_hash = hash_with(hash, label);
    unsigned bad = em[0];
    for (std::size_t i = 0; i < h_len; ++i)
        bad |= static_cast<unsigned>(db[i] ^ l_hash[i]);
    // The first nonzero byte after lHash must be 0x01; its index is kept
    // without a branch on the bytes.
    std::size_t one_index = 0;
    unsigned looking = 1;
    for (std::size_t i = h_len; i < db.size(); ++i) {
        unsigned const is_zero = static_cast<unsigned>(db[i] == 0);
        unsigned const is_one = static_cast<unsigned>(db[i] == 1);
        unsigned const found = looking & is_one;
        one_index = found != 0 ? i : one_index;
        bad |= looking & (is_zero ^ 1) & (is_one ^ 1);
        looking &= is_zero;
    }
    bad |= looking;
    if (bad != 0)
        return std::nullopt;
    return std::vector<std::uint8_t>(db.begin() + static_cast<std::ptrdiff_t>(one_index + 1), db.end());
}

}
