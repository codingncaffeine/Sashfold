#include "auth/Transport.h"

#include <thread>

// Systems without a security-key transport of ours yet: no keys are ever
// found, so a ceremony shows "insert your key" until its timeout or the
// user's cancel. Owed: on Windows the browser must go through the
// system's WebAuthn API (webauthn.dll), which owns the keys; on macOS,
// IOHIDManager matched on usage page 0xF1D0 gives the same reports
// hidraw does on Linux.

namespace sashfold::auth {

namespace {

class NoTransport final : public Transport {
public:
    std::vector<DeviceInfo> devices() override { return {}; }
    std::unique_ptr<HidConnection> open(DeviceInfo const&) override { return nullptr; }
    bool wait_for_change(std::chrono::milliseconds timeout) override
    {
        std::this_thread::sleep_for(timeout);
        return false;
    }
};

}

std::unique_ptr<Transport> system_transport() { return std::make_unique<NoTransport>(); }

}
