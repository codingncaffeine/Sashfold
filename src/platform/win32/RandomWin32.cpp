#include "platform/Random.h"

#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bcrypt.h>

namespace sashfold::platform {

// BCryptGenRandom with the system-preferred generator (bcrypt.dll, an OS
// library on the pledge allowlist).
void fill_random(std::span<std::uint8_t> out)
{
    while (!out.empty()) {
        ULONG const take = out.size() < 0x10000 ? static_cast<ULONG>(out.size()) : 0x10000;
        NTSTATUS const status = BCryptGenRandom(nullptr, out.data(), take, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) {
            std::fputs("sashfold: the operating system refused to supply random bytes\n", stderr);
            std::abort();
        }
        out = out.subspan(take);
    }
}

}
