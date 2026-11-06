#include "platform/CaretBlink.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sashfold::platform {

void start_caret_blink_lookup()
{
}

std::optional<CaretBlink> caret_blink()
{
    UINT const half = GetCaretBlinkTime();
    if (half == 0 || half == INFINITE)
        return std::nullopt;
    CaretBlink blink;
    blink.half_ms = static_cast<int>(half);
    // Windows 10 and later rest the caret after a while (5 s by default);
    // an older Windows has no such setting and blinks on.
    constexpr UINT get_caret_timeout = 0x2022; // SPI_GETCARETTIMEOUT
    DWORD timeout = 0;
    if (SystemParametersInfoW(get_caret_timeout, 0, &timeout, 0) && timeout != INFINITE)
        blink.timeout_ms = static_cast<int>(timeout);
    return blink;
}

}
