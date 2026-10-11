#include "crypto/X25519.h"

#include "crypto/Field25519.h"

#include <cstring>

namespace sashfold::crypto {

using namespace field25519;

bool x25519(X25519Key& out, X25519Key const& scalar, X25519Key const& point)
{
    // §5: clamp the scalar; the u-coordinate's top bit is masked by the
    // decoder.
    std::uint8_t k[32];
    std::memcpy(k, scalar.data(), 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    Fe const x1 = fe_frombytes(point.data());
    Fe x2 = { { 1, 0, 0, 0, 0 } };
    Fe z2 = { { 0, 0, 0, 0, 0 } };
    Fe x3 = x1;
    Fe z3 = { { 1, 0, 0, 0, 0 } };
    Fe const a24 = { { 121665, 0, 0, 0, 0 } };
    std::uint64_t swap = 0;
    for (int t = 254; t >= 0; --t) {
        std::uint64_t const k_t = (k[t / 8] >> (t % 8)) & 1;
        swap ^= k_t;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = k_t;
        Fe const a = fe_add(x2, z2);
        Fe const aa = fe_sq(a);
        Fe const b = fe_sub(x2, z2);
        Fe const bb = fe_sq(b);
        Fe const e = fe_sub(aa, bb);
        Fe const c = fe_add(x3, z3);
        Fe const d = fe_sub(x3, z3);
        Fe const da = fe_mul(d, a);
        Fe const cb = fe_mul(c, b);
        x3 = fe_sq(fe_add(da, cb));
        z3 = fe_mul(x1, fe_sq(fe_sub(da, cb)));
        x2 = fe_mul(aa, bb);
        z2 = fe_mul(e, fe_add(aa, fe_mul(a24, e)));
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_tobytes(out.data(), fe_mul(x2, fe_invert(z2)));

    // RFC 7748 §6.1 / RFC 8446 §7.4.2: an all-zero output means a
    // low-order point and must be refused; checked without an early exit.
    std::uint8_t any = 0;
    for (std::uint8_t const byte : out)
        any = static_cast<std::uint8_t>(any | byte);
    return any != 0;
}

X25519Key x25519_public(X25519Key const& scalar)
{
    X25519Key const base = { { 9 } };
    X25519Key out;
    x25519(out, scalar, base);
    return out;
}

}
