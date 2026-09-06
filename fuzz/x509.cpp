// libFuzzer harness for the DER, certificate and CRL readers: hostile
// bytes through parse_certificate, parse_crl and parse_public_key — no
// crash, no sanitizer finding, and never a read past the input.

#include "net/tls/X509.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    std::vector<std::uint8_t> const owned(data, data + size);
    std::span<std::uint8_t const> const input(owned);
    (void)sashfold::tls::parse_certificate(input);
    (void)sashfold::tls::parse_crl(input);
    (void)sashfold::tls::parse_public_key(input);
    return 0;
}
