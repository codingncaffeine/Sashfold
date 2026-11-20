#pragma once
// CTAPHID, the FIDO HID transport (CTAP 2.1 §11.2), from the client's side:
// messages cut into fixed-size packets — an initialization packet
// (CID, command with bit 7 set, a 16-bit length, data) and continuation
// packets (CID, sequence 0 to 127, data) — a channel of our own asked for
// with INIT on the broadcast channel, and transactions on it.
//
// One transaction at a time per device. While it waits for the answer the
// device may send KEEPALIVE (processing, or waiting for a touch), which is
// passed up; packets on other channels are someone else's and are
// skipped; a busy device is asked again after a short wait; a cancelled
// request sends CTAPHID_CANCEL and keeps listening for the device's
// CTAP2_ERR_KEEPALIVE_CANCEL answer (§11.2.9.1.5). A device that goes away
// mid-message ends the transaction with Gone.
#include "auth/Transport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sashfold::auth {

namespace ctaphid {
inline constexpr std::uint32_t broadcast_cid = 0xffffffffu;

// Commands (§11.2.9), without bit 7.
inline constexpr std::uint8_t cmd_ping = 0x01;
inline constexpr std::uint8_t cmd_msg = 0x03;
inline constexpr std::uint8_t cmd_lock = 0x04;
inline constexpr std::uint8_t cmd_init = 0x06;
inline constexpr std::uint8_t cmd_wink = 0x08;
inline constexpr std::uint8_t cmd_cbor = 0x10;
inline constexpr std::uint8_t cmd_cancel = 0x11;
inline constexpr std::uint8_t cmd_keepalive = 0x3b;
inline constexpr std::uint8_t cmd_error = 0x3f;

// CTAPHID_ERROR codes (§11.2.9.1.6).
inline constexpr std::uint8_t err_invalid_cmd = 0x01;
inline constexpr std::uint8_t err_invalid_par = 0x02;
inline constexpr std::uint8_t err_invalid_len = 0x03;
inline constexpr std::uint8_t err_invalid_seq = 0x04;
inline constexpr std::uint8_t err_msg_timeout = 0x05;
inline constexpr std::uint8_t err_channel_busy = 0x06;
inline constexpr std::uint8_t err_lock_required = 0x0a;
inline constexpr std::uint8_t err_invalid_channel = 0x0b;
inline constexpr std::uint8_t err_other = 0x7f;

// KEEPALIVE status codes (§11.2.9.1.7).
inline constexpr std::uint8_t status_processing = 1;
inline constexpr std::uint8_t status_up_needed = 2;

// Capability flags of the INIT answer.
inline constexpr std::uint8_t capability_wink = 0x01;
inline constexpr std::uint8_t capability_cbor = 0x04;
inline constexpr std::uint8_t capability_nmsg = 0x08;

// The largest payload `report_size`-byte packets carry: one
// initialization packet and 128 continuations (7609 bytes at 64).
constexpr std::size_t max_payload(std::size_t report_size) { return (report_size - 7) + 128 * (report_size - 5); }

// The packets of one message on one channel, each report_size bytes,
// zero-padded. Empty when the payload is longer than max_payload.
std::vector<std::vector<std::uint8_t>> fragment(std::uint32_t cid, std::uint8_t command, std::span<std::uint8_t const> payload,
    std::size_t report_size);

// Reassembles one message from packets of one channel. Packets of other
// channels are the caller's to skip.
class Assembler {
public:
    enum class Step : std::uint8_t {
        NeedMore, // a packet was taken; the message is not whole yet
        Done, // the message is whole: command() and payload()
        Spurious, // a continuation with no message started: ignored (§11.2.5.4)
        Malformed, // a sequence out of order, a length beyond the maximum, a short packet
    };
    explicit Assembler(std::size_t report_size)
        : m_report_size(report_size)
    {
    }
    Step take(std::span<std::uint8_t const> packet);
    void reset();
    std::uint8_t command() const { return m_command; }
    std::vector<std::uint8_t> const& payload() const { return m_payload; }
    bool in_progress() const { return m_started; }

private:
    std::size_t m_report_size;
    bool m_started = false;
    std::uint8_t m_command = 0;
    std::size_t m_expected = 0;
    std::uint8_t m_next_sequence = 0;
    std::vector<std::uint8_t> m_payload;
};

inline std::uint32_t packet_cid(std::span<std::uint8_t const> packet)
{
    return packet.size() < 4 ? 0
                             : (std::uint32_t { packet[0] } << 24 | std::uint32_t { packet[1] } << 16 | std::uint32_t { packet[2] } << 8
                                   | std::uint32_t { packet[3] });
}
}

// Why a transaction ended without an answer.
enum class HidFailure : std::uint8_t {
    None,
    Timeout, // no answer before the deadline
    Cancelled, // the caller cancelled and the device did not answer the cancel
    Gone, // unplugged or the read/write failed
    Busy, // the device stayed busy with another channel past the deadline
    DeviceError, // a CTAPHID_ERROR other than busy (error_code says which)
    Malformed, // a packet that breaks the framing, or an answer of another command
    TooLarge, // the request does not fit the packet scheme
};

struct HidResponse {
    HidFailure failure = HidFailure::None;
    std::uint8_t error_code = 0; // for DeviceError
    std::uint8_t command = 0;
    std::vector<std::uint8_t> payload;
    bool ok() const { return failure == HidFailure::None; }
};

// A flag the request's owner raises from any thread; transactions check it
// between packets (at least every 50 ms) and answer promptly.
using CancelFlag = std::atomic<bool>;

class CtapHidDevice {
public:
    CtapHidDevice(std::unique_ptr<HidConnection> connection, DeviceInfo info);

    DeviceInfo const& info() const { return m_info; }

    // Allocates a channel (INIT on the broadcast channel with a fresh
    // 8-byte nonce; answers carrying another nonce are skipped). False when
    // the device does not answer in time.
    bool init(std::chrono::steady_clock::time_point deadline, CancelFlag const* cancel = nullptr);

    std::uint32_t channel() const { return m_cid; }
    std::uint8_t protocol_version() const { return m_protocol_version; }
    std::array<std::uint8_t, 3> device_version() const { return m_device_version; }
    std::uint8_t capabilities() const { return m_capabilities; }
    bool has_cbor() const { return (m_capabilities & ctaphid::capability_cbor) != 0; }
    bool has_msg() const { return (m_capabilities & ctaphid::capability_nmsg) == 0; }
    bool has_wink() const { return (m_capabilities & ctaphid::capability_wink) != 0; }

    // Called with each KEEPALIVE status while a transaction waits.
    using Keepalive = std::function<void(std::uint8_t status)>;

    // One transaction: the request on our channel, then packets until the
    // answer of the same command. When `cancel` rises, CTAPHID_CANCEL is
    // sent once (for CBOR requests) and the answer is still awaited for up
    // to a second; for other commands the transaction is abandoned and the
    // channel resynchronized with INIT.
    HidResponse transact(std::uint8_t command, std::span<std::uint8_t const> payload, std::chrono::steady_clock::time_point deadline,
        CancelFlag const* cancel = nullptr, Keepalive const& keepalive = {});

    bool ping(std::span<std::uint8_t const> data, std::chrono::steady_clock::time_point deadline);
    bool wink(std::chrono::steady_clock::time_point deadline);
    // Locks the device to our channel for `seconds` (0 releases it).
    bool lock(std::uint8_t seconds, std::chrono::steady_clock::time_point deadline);

    // Sends CTAPHID_CANCEL on our channel outside a transaction (another
    // thread may not call transact concurrently).
    bool send_cancel();

    bool gone() const { return m_gone; }

    // For tests: the nonce source (defaults to the platform's random).
    using NonceSource = void (*)(std::span<std::uint8_t> out);
    void set_nonce_source(NonceSource source) { m_nonce_source = source; }

private:
    bool send(std::uint32_t cid, std::uint8_t command, std::span<std::uint8_t const> payload);
    HidConnection::Read read(std::vector<std::uint8_t>& packet, std::chrono::steady_clock::time_point deadline);

    std::unique_ptr<HidConnection> m_connection;
    DeviceInfo m_info;
    std::size_t m_report_size;
    std::uint32_t m_cid = ctaphid::broadcast_cid;
    std::uint8_t m_protocol_version = 0;
    std::array<std::uint8_t, 3> m_device_version {};
    std::uint8_t m_capabilities = 0;
    bool m_gone = false;
    NonceSource m_nonce_source = nullptr;
};

}
