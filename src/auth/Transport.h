#pragma once
// Where security keys are found and how their reports move: the seam
// between the protocol code (CTAPHID, CTAP2, U2F), which is the same on
// every system, and each system's way of reaching a HID device.
//
// On Linux the system transport reads /sys/class/hidraw for devices whose
// report descriptor declares the FIDO usage page and speaks to
// /dev/hidrawN (platform/linux/FidoHidLinux.cpp). Elsewhere it is the
// None transport (platform/FidoHidNone.cpp), which finds no keys: Windows
// routes security keys through its own WebAuthn API by policy (the
// browser must call webauthn.dll's WebAuthNAuthenticatorMakeCredential
// and ...GetAssertion rather than open the HID device, which an
// unelevated process may not), and macOS needs IOHIDManager matched on
// usage page 0xF1D0. Tests use a fake one (tests/SoftAuthenticator.h).
//
// Every call here may block: they are made on the request's own thread
// (auth/Client.h), never on the window's.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sashfold::auth {

struct DeviceInfo {
    // What the transport opens the device by; stable while it stays
    // plugged in ("/dev/hidraw3" on Linux).
    std::string path;
    // The product's own name for display, when the system gives one.
    std::string product;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    // The FIDO reports' sizes from the report descriptor (64 for every key
    // the specification knows; CTAPHID packets are this long).
    std::size_t input_report_size = 64;
    std::size_t output_report_size = 64;

    bool operator==(DeviceInfo const&) const = default;
};

// One open device: whole reports in and out, without report IDs (the
// transport adds and strips them).
class HidConnection {
public:
    virtual ~HidConnection() = default;

    // Sends one output report of output_report_size bytes. False when the
    // device has gone or the write failed.
    virtual bool write_report(std::span<std::uint8_t const> report) = 0;

    enum class Read : std::uint8_t { Report, Timeout, Gone };
    // Waits up to `timeout` for one input report.
    virtual Read read_report(std::vector<std::uint8_t>& report, std::chrono::milliseconds timeout) = 0;
};

class Transport {
public:
    virtual ~Transport() = default;

    // The FIDO devices present now.
    virtual std::vector<DeviceInfo> devices() = 0;
    // Opens one for reading and writing; null when it cannot be opened
    // (gone already, or not permitted).
    virtual std::unique_ptr<HidConnection> open(DeviceInfo const& device) = 0;
    // Waits up to `timeout` for devices to come or go. True when something
    // may have changed (the caller then asks devices() again); false when
    // the time ran out with nothing seen. A transport that cannot watch
    // answers true after the timeout, so callers look again; the None
    // transport, which never has a key, answers false.
    virtual bool wait_for_change(std::chrono::milliseconds timeout) = 0;
};

// The system's transport (see the top of this file).
std::unique_ptr<Transport> system_transport();

}
