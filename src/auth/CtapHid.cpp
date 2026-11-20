#include "auth/CtapHid.h"

#include "platform/Random.h"

#include <algorithm>
#include <thread>

namespace sashfold::auth {

namespace ctaphid {

std::vector<std::vector<std::uint8_t>> fragment(std::uint32_t cid, std::uint8_t command, std::span<std::uint8_t const> payload,
    std::size_t report_size)
{
    std::vector<std::vector<std::uint8_t>> packets;
    if (report_size < 8 || payload.size() > max_payload(report_size) || payload.size() > 0xffff)
        return packets;
    auto const header = [cid](std::vector<std::uint8_t>& packet) {
        packet[0] = static_cast<std::uint8_t>(cid >> 24);
        packet[1] = static_cast<std::uint8_t>(cid >> 16);
        packet[2] = static_cast<std::uint8_t>(cid >> 8);
        packet[3] = static_cast<std::uint8_t>(cid);
    };
    std::vector<std::uint8_t> first(report_size, 0);
    header(first);
    first[4] = static_cast<std::uint8_t>(command | 0x80);
    first[5] = static_cast<std::uint8_t>(payload.size() >> 8);
    first[6] = static_cast<std::uint8_t>(payload.size());
    std::size_t taken = std::min(payload.size(), report_size - 7);
    std::copy_n(payload.begin(), taken, first.begin() + 7);
    packets.push_back(std::move(first));
    std::uint8_t sequence = 0;
    while (taken < payload.size()) {
        std::vector<std::uint8_t> next(report_size, 0);
        header(next);
        next[4] = sequence++;
        std::size_t const chunk = std::min(payload.size() - taken, report_size - 5);
        std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(taken), chunk, next.begin() + 5);
        taken += chunk;
        packets.push_back(std::move(next));
    }
    return packets;
}

void Assembler::reset()
{
    m_started = false;
    m_command = 0;
    m_expected = 0;
    m_next_sequence = 0;
    m_payload.clear();
}

Assembler::Step Assembler::take(std::span<std::uint8_t const> packet)
{
    if (packet.size() < m_report_size || m_report_size < 8)
        return Step::Malformed;
    std::uint8_t const fifth = packet[4];
    if ((fifth & 0x80) != 0) {
        // An initialization packet starts a message afresh.
        reset();
        std::size_t const length = std::size_t { packet[5] } << 8 | packet[6];
        if (length > max_payload(m_report_size))
            return Step::Malformed;
        m_started = true;
        m_command = static_cast<std::uint8_t>(fifth & 0x7f);
        m_expected = length;
        std::size_t const chunk = std::min(length, m_report_size - 7);
        m_payload.assign(packet.begin() + 7, packet.begin() + 7 + static_cast<std::ptrdiff_t>(chunk));
        return m_payload.size() == m_expected ? Step::Done : Step::NeedMore;
    }
    if (!m_started)
        return Step::Spurious;
    if (fifth != m_next_sequence) {
        reset();
        return Step::Malformed;
    }
    ++m_next_sequence;
    std::size_t const chunk = std::min(m_expected - m_payload.size(), m_report_size - 5);
    m_payload.insert(m_payload.end(), packet.begin() + 5, packet.begin() + 5 + static_cast<std::ptrdiff_t>(chunk));
    return m_payload.size() == m_expected ? Step::Done : Step::NeedMore;
}

}

namespace {

using Clock = std::chrono::steady_clock;

// The longest a read waits before the caller looks at its cancel flag.
constexpr auto read_slice = std::chrono::milliseconds(50);
// How long the answer to a cancelled CBOR request is waited for.
constexpr auto cancel_grace = std::chrono::milliseconds(1000);
// How long a busy device is left before it is asked again.
constexpr auto busy_retry = std::chrono::milliseconds(60);
// How long the answer to a resynchronizing INIT is waited for.
constexpr auto resync_wait = std::chrono::milliseconds(1000);

}

CtapHidDevice::CtapHidDevice(std::unique_ptr<HidConnection> connection, DeviceInfo info)
    : m_connection(std::move(connection))
    , m_info(std::move(info))
    , m_report_size(std::max<std::size_t>(8, std::min(m_info.input_report_size, m_info.output_report_size)))
{
}

bool CtapHidDevice::send(std::uint32_t cid, std::uint8_t command, std::span<std::uint8_t const> payload)
{
    if (m_gone)
        return false;
    auto const packets = ctaphid::fragment(cid, command, payload, m_report_size);
    if (packets.empty())
        return false;
    for (auto const& packet : packets) {
        // A report larger than the packet size (an output report longer
        // than the input one) is padded with zeros.
        std::vector<std::uint8_t> report = packet;
        report.resize(std::max(m_info.output_report_size, m_report_size), 0);
        if (!m_connection->write_report(report)) {
            m_gone = true;
            return false;
        }
    }
    return true;
}

HidConnection::Read CtapHidDevice::read(std::vector<std::uint8_t>& packet, Clock::time_point deadline)
{
    if (m_gone)
        return HidConnection::Read::Gone;
    auto const now = Clock::now();
    if (now >= deadline)
        return HidConnection::Read::Timeout;
    auto const wait = std::min<Clock::duration>(deadline - now, read_slice);
    auto const result = m_connection->read_report(packet, std::chrono::duration_cast<std::chrono::milliseconds>(wait) + std::chrono::milliseconds(1));
    if (result == HidConnection::Read::Gone)
        m_gone = true;
    return result;
}

bool CtapHidDevice::init(Clock::time_point deadline, CancelFlag const* cancel)
{
    std::array<std::uint8_t, 8> nonce {};
    if (m_nonce_source != nullptr)
        m_nonce_source(nonce);
    else
        platform::fill_random(nonce);
    if (!send(ctaphid::broadcast_cid, ctaphid::cmd_init, nonce))
        return false;
    ctaphid::Assembler assembler(m_report_size);
    std::vector<std::uint8_t> packet;
    while (true) {
        if (cancel != nullptr && cancel->load())
            return false;
        auto const result = read(packet, deadline);
        if (result == HidConnection::Read::Gone)
            return false;
        if (result == HidConnection::Read::Timeout) {
            if (Clock::now() >= deadline)
                return false;
            continue;
        }
        if (packet.size() < m_report_size || ctaphid::packet_cid(packet) != ctaphid::broadcast_cid)
            continue;
        auto const step = assembler.take(packet);
        if (step != ctaphid::Assembler::Step::Done)
            continue;
        auto const& payload = assembler.payload();
        // Another application's INIT answer has another nonce: not ours.
        if (assembler.command() != ctaphid::cmd_init || payload.size() < 17 || !std::equal(nonce.begin(), nonce.end(), payload.begin())) {
            assembler.reset();
            continue;
        }
        std::uint32_t const cid = ctaphid::packet_cid(std::span<std::uint8_t const>(payload).subspan(8, 4));
        if (cid == ctaphid::broadcast_cid || cid == 0)
            return false;
        m_cid = cid;
        m_protocol_version = payload[12];
        m_device_version = { payload[13], payload[14], payload[15] };
        m_capabilities = payload[16];
        return true;
    }
}

bool CtapHidDevice::resynchronize(Clock::time_point deadline)
{
    std::array<std::uint8_t, 8> nonce {};
    if (m_nonce_source != nullptr)
        m_nonce_source(nonce);
    else
        platform::fill_random(nonce);
    if (!send(m_cid, ctaphid::cmd_init, nonce))
        return false;
    // A device answers INIT at once, whatever it was doing.
    Clock::time_point const until = std::min(deadline, Clock::now() + resync_wait);
    ctaphid::Assembler assembler(m_report_size);
    std::vector<std::uint8_t> packet;
    while (true) {
        auto const result = read(packet, until);
        if (result == HidConnection::Read::Gone)
            return false;
        if (result == HidConnection::Read::Timeout) {
            if (Clock::now() >= until)
                return false;
            continue;
        }
        if (packet.size() < m_report_size || ctaphid::packet_cid(packet) != m_cid)
            continue;
        auto const step = assembler.take(packet);
        if (step == ctaphid::Assembler::Step::Spurious || step == ctaphid::Assembler::Step::NeedMore)
            continue;
        // Anything but this INIT's own answer is of the transaction left
        // behind (an INIT sent when it was cancelled included).
        bool const ours = step == ctaphid::Assembler::Step::Done && assembler.command() == ctaphid::cmd_init
            && assembler.payload().size() >= nonce.size() && std::equal(nonce.begin(), nonce.end(), assembler.payload().begin());
        assembler.reset();
        if (ours) {
            m_stale = false;
            return true;
        }
    }
}

HidResponse CtapHidDevice::transact(std::uint8_t command, std::span<std::uint8_t const> payload, Clock::time_point deadline,
    CancelFlag const* cancel, Keepalive const& keepalive)
{
    HidResponse response;
    if (payload.size() > ctaphid::max_payload(m_report_size)) {
        response.failure = HidFailure::TooLarge;
        return response;
    }
    if (m_stale && !resynchronize(deadline)) {
        response.failure = m_gone ? HidFailure::Gone : HidFailure::Timeout;
        return response;
    }
    if (!send(m_cid, command, payload)) {
        response.failure = HidFailure::Gone;
        return response;
    }
    ctaphid::Assembler assembler(m_report_size);
    std::vector<std::uint8_t> packet;
    bool cancel_sent = false;
    bool busy_seen = false;
    Clock::time_point effective_deadline = deadline;
    while (true) {
        if (cancel != nullptr && cancel->load() && !cancel_sent) {
            cancel_sent = true;
            if (command == ctaphid::cmd_cbor) {
                // The device answers the request itself with
                // CTAP2_ERR_KEEPALIVE_CANCEL; wait a little for it.
                send(m_cid, ctaphid::cmd_cancel, {});
                effective_deadline = std::min(deadline, Clock::now() + cancel_grace);
            } else {
                // No cancel for U2F messages: resynchronize the channel so
                // that a late answer cannot be taken for the next one's.
                std::array<std::uint8_t, 8> nonce {};
                platform::fill_random(nonce);
                send(m_cid, ctaphid::cmd_init, nonce);
                m_stale = true; // that INIT's answer, and perhaps the request's, are still to come
                response.failure = m_gone ? HidFailure::Gone : HidFailure::Cancelled;
                return response;
            }
        }
        auto const result = read(packet, effective_deadline);
        if (result == HidConnection::Read::Gone) {
            response.failure = HidFailure::Gone;
            return response;
        }
        if (result == HidConnection::Read::Timeout) {
            if (Clock::now() >= effective_deadline) {
                // The answer may still come, after this transaction is
                // over. A device that said it was busy and has not
                // answered the asking since is busy still, not silent.
                m_stale = true;
                response.failure = cancel_sent ? HidFailure::Cancelled : busy_seen ? HidFailure::Busy : HidFailure::Timeout;
                return response;
            }
            continue;
        }
        if (packet.size() < m_report_size || ctaphid::packet_cid(packet) != m_cid)
            continue; // another channel's traffic
        auto const step = assembler.take(packet);
        if (step == ctaphid::Assembler::Step::Spurious || step == ctaphid::Assembler::Step::NeedMore)
            continue;
        if (step == ctaphid::Assembler::Step::Malformed) {
            m_stale = true;
            response.failure = HidFailure::Malformed;
            return response;
        }
        std::uint8_t const answered = assembler.command();
        if (answered == ctaphid::cmd_keepalive) {
            if (keepalive && !assembler.payload().empty())
                keepalive(assembler.payload()[0]);
            assembler.reset();
            continue;
        }
        if (answered == ctaphid::cmd_error) {
            std::uint8_t const code = assembler.payload().empty() ? ctaphid::err_other : assembler.payload()[0];
            if (code == ctaphid::err_channel_busy && !cancel_sent) {
                // Another channel holds the device: ask again shortly.
                busy_seen = true;
                if (Clock::now() + busy_retry >= deadline) {
                    response.failure = HidFailure::Busy;
                    return response;
                }
                for (auto until = Clock::now() + busy_retry; Clock::now() < until;) {
                    if (cancel != nullptr && cancel->load())
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (cancel != nullptr && cancel->load()) {
                    response.failure = HidFailure::Cancelled;
                    return response;
                }
                assembler.reset();
                if (!send(m_cid, command, payload)) {
                    response.failure = HidFailure::Gone;
                    return response;
                }
                continue;
            }
            response.failure = HidFailure::DeviceError;
            response.error_code = code;
            return response;
        }
        if (answered != command) {
            m_stale = true;
            response.failure = HidFailure::Malformed;
            return response;
        }
        response.command = answered;
        response.payload = assembler.payload();
        return response;
    }
}

bool CtapHidDevice::ping(std::span<std::uint8_t const> data, Clock::time_point deadline)
{
    auto const response = transact(ctaphid::cmd_ping, data, deadline);
    return response.ok() && std::ranges::equal(response.payload, data);
}

bool CtapHidDevice::wink(Clock::time_point deadline)
{
    if (!has_wink())
        return false;
    return transact(ctaphid::cmd_wink, {}, deadline).ok();
}

bool CtapHidDevice::lock(std::uint8_t seconds, Clock::time_point deadline)
{
    std::uint8_t const data[] = { std::min<std::uint8_t>(seconds, 10) };
    return transact(ctaphid::cmd_lock, data, deadline).ok();
}

bool CtapHidDevice::send_cancel() { return send(m_cid, ctaphid::cmd_cancel, {}); }

}
