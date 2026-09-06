#include "platform/Random.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif

namespace sashfold::platform {

// getentropy(2): glibc 2.25 and macOS 10.12 onward; at most 256 bytes a
// call, so a longer request goes in pieces.
void fill_random(std::span<std::uint8_t> out)
{
    while (!out.empty()) {
        std::size_t const take = out.size() < 256 ? out.size() : 256;
        if (getentropy(out.data(), take) != 0) {
            std::fputs("sashfold: the operating system refused to supply random bytes\n", stderr);
            std::abort();
        }
        out = out.subspan(take);
    }
}

}
