#pragma once
// A software security key FOR TESTS — never part of the engine. It is
// written from the specifications, on the authenticator's side of them:
//
//   CTAPHID (CTAP 2.1 §11.2): channel allocation, busy channels, LOCK,
//     PING, WINK, CANCEL, KEEPALIVE every 100 ms while it waits for a touch
//     (or while "processing", when told to be slow), message timeouts.
//   CTAP 2.1 (§6): getInfo, makeCredential, getAssertion, getNextAssertion,
//     clientPIN with PIN/UV auth protocols one and two, selection, reset;
//     resident keys, built-in UV, alwaysUv, makeCredUvNotRqd, the
//     credProtect, hmac-secret, credBlob, largeBlobKey and minPinLength
//     extensions; "packed" attestation, self or with a test certificate.
//   U2F (FIDO U2F Raw Message Formats v1.2) through CTAPHID_MSG: REGISTER,
//     AUTHENTICATE (check-only, enforce, don't-enforce), VERSION, with a
//     "fido-u2f" attestation certificate; "test of user presence required"
//     until it is touched.
//
// The user is scripted: a touch immediately, after N ms, or never, and a
// built-in UV that succeeds or fails. Several of these on one FakeTransport
// are several keys plugged in at once; any of them can be unplugged.
//
// Its framing and command parsing are its own (they do not call the client
// code they test); the PIN/UV protocol primitives and CBOR are the
// engine's, which the specification's vectors check separately.
#include "FakeHid.h"

#include "auth/Cbor.h"
#include "auth/Cose.h"
#include "auth/Ctap2.h"
#include "auth/DerWriter.h"
#include "auth/PinProtocol.h"
#include "auth/Transport.h"
#include "crypto/Ec.h"
#include "crypto/Ed25519.h"
#include "crypto/Hmac.h"
#include "crypto/Sha2.h"
#include "platform/Random.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace sashfold::test {

using SoftClock = std::chrono::steady_clock;

// What the scripted user does when the key asks for a touch.
struct TouchPlan {
    enum class Kind : std::uint8_t { Immediately, After, Never };
    Kind kind = Kind::Immediately;
    int milliseconds = 0;

    static TouchPlan immediately() { return {}; }
    static TouchPlan after(int ms) { return { Kind::After, ms }; }
    static TouchPlan never() { return { Kind::Never, 0 }; }
};

struct SoftOptions {
    enum class Protocol : std::uint8_t { U2fOnly, Ctap20, Ctap21 };
    Protocol protocol = Protocol::Ctap21;
    bool speaks_u2f = true; // a CTAP2 key that also takes CTAPHID_MSG
    bool resident_keys = true;
    bool built_in_uv = false; // a fingerprint reader, say
    bool uv_enrolled = true;
    bool client_pin = true; // the clientPin feature (set or not)
    std::optional<std::string> pin; // set at start
    std::vector<std::uint8_t> pin_protocols { 2, 1 };
    bool always_uv = false;
    bool make_cred_uv_not_rqd = true;
    bool force_pin_change = false;
    std::size_t min_pin_length = 4;
    std::vector<std::string> min_pin_length_rp_ids;
    bool cred_protect = true;
    bool hmac_secret = true;
    bool cred_blob = true;
    bool large_blob_key = true;
    bool min_pin_length_extension = true;
    std::size_t max_cred_blob_length = 32;
    std::size_t max_credential_count_in_list = 8;
    std::size_t max_credential_id_length = 128;
    std::size_t max_msg_size = 1200;
    std::vector<std::int64_t> algorithms { auth::cose::alg_es256, auth::cose::alg_eddsa };
    bool self_attestation = false; // otherwise packed with the test certificate
    bool wink = true;
    std::array<std::uint8_t, 16> aaguid { 0x5e, 0x1f, 0x70, 0x0a, 0x7e, 0x57, 0x4b, 0x00, 0x9a, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    int keepalive_ms = 100;
    int processing_ms = 0; // a slow key: this long of PROCESSING keepalives first
    int up_timeout_ms = 30000;
    int message_timeout_ms = 500; // CTAPHID transaction timeout
    std::size_t report_size = 64;
};

// The key's view of the scripted user and of what happened, for tests.
struct SoftStats {
    std::atomic<int> keepalives_sent { 0 };
    std::atomic<int> presence_requests { 0 };
    std::atomic<int> touches { 0 };
    std::atomic<int> cancels_honored { 0 };
    std::atomic<int> winks { 0 };
    std::atomic<int> busy_errors { 0 };
    std::atomic<int> u2f_requests { 0 };
    std::atomic<int> cbor_requests { 0 };
};

class SoftAuthenticator {
public:
    struct Credential {
        std::vector<std::uint8_t> id;
        std::array<std::uint8_t, 32> rp_id_hash {};
        std::string rp_id;
        auth::UserEntity user;
        std::int64_t alg = auth::cose::alg_es256;
        std::array<std::uint8_t, 32> private_key {}; // P-256 scalar or Ed25519 seed
        bool discoverable = false;
        int cred_protect = 1;
        std::array<std::uint8_t, 32> cred_random_with_uv {};
        std::array<std::uint8_t, 32> cred_random_without_uv {};
        std::optional<std::vector<std::uint8_t>> cred_blob;
        std::optional<std::vector<std::uint8_t>> large_blob_key;
        std::uint32_t sign_count = 0;
        std::uint64_t created = 0;
        bool from_u2f = false;
    };

    explicit SoftAuthenticator(SoftOptions options = {})
        : m_options(std::move(options))
        , m_link(std::make_shared<FakeLink>(m_options.report_size))
    {
        if (m_options.pin)
            m_pin_hash = auth::pin::pin_hash(*m_options.pin);
        m_force_pin_change = m_options.force_pin_change;
        regenerate_key_agreement();
        reset_token();
        attestation_setup();
        m_thread = std::thread([this] { run(); });
    }

    ~SoftAuthenticator()
    {
        m_stop = true;
        m_link->unplug();
        if (m_thread.joinable())
            m_thread.join();
    }

    SoftAuthenticator(SoftAuthenticator const&) = delete;
    SoftAuthenticator& operator=(SoftAuthenticator const&) = delete;

    std::unique_ptr<auth::HidConnection> connect() { return m_link->connect(); }
    std::shared_ptr<FakeLink> link() { return m_link; }
    SoftOptions const& options() const { return m_options; }
    SoftStats& stats() { return m_stats; }

    void set_touch(TouchPlan plan)
    {
        std::lock_guard lock(m_touch_mutex);
        m_touch = plan;
    }
    void set_uv_succeeds(bool succeeds) { m_uv_succeeds = succeeds; }
    // Unplugged and plugged back in: the state that lasts only until power
    // is lost goes (§6.5.2: the PIN mismatch run, the key agreement key,
    // the token).
    void power_cycle()
    {
        std::lock_guard lock(m_mutex);
        m_consecutive_mismatches = 0;
        m_power_cycle_needed = false;
        regenerate_key_agreement();
        reset_token();
    }
    void unplug() { m_link->unplug(); }

    std::vector<Credential> credentials()
    {
        std::lock_guard lock(m_mutex);
        return m_credentials;
    }
    void add_credential(Credential credential)
    {
        std::lock_guard lock(m_mutex);
        credential.created = ++m_creation_counter;
        m_credentials.push_back(std::move(credential));
    }
    std::optional<std::array<std::uint8_t, 16>> pin_hash()
    {
        std::lock_guard lock(m_mutex);
        return m_pin_hash;
    }
    int pin_retries()
    {
        std::lock_guard lock(m_mutex);
        return m_pin_retries;
    }
    int uv_retries()
    {
        std::lock_guard lock(m_mutex);
        return m_uv_retries;
    }
    // The test attestation certificate (DER) and its public key.
    std::vector<std::uint8_t> const& attestation_certificate() const { return m_attestation_cert; }
    crypto::EcPoint const& attestation_public_key() const { return m_attestation_point; }

    // A plain-text name of the status a response began with, for tests.
    static std::uint8_t status_of(std::span<std::uint8_t const> response) { return response.empty() ? 0xff : response[0]; }

private:
    using Status = auth::ctap2::Status;

    // ---------- CTAPHID, device side (CTAP 2.1 §11.2) ----------

    static constexpr std::uint32_t broadcast = 0xffffffffu;

    static std::uint32_t cid_of(std::span<std::uint8_t const> p)
    {
        return std::uint32_t { p[0] } << 24 | std::uint32_t { p[1] } << 16 | std::uint32_t { p[2] } << 8 | p[3];
    }

    void send_message(std::uint32_t cid, std::uint8_t command, std::span<std::uint8_t const> payload)
    {
        std::size_t const size = m_options.report_size;
        std::vector<std::uint8_t> packet(size, 0);
        auto put_cid = [&] {
            packet[0] = static_cast<std::uint8_t>(cid >> 24);
            packet[1] = static_cast<std::uint8_t>(cid >> 16);
            packet[2] = static_cast<std::uint8_t>(cid >> 8);
            packet[3] = static_cast<std::uint8_t>(cid);
        };
        put_cid();
        packet[4] = static_cast<std::uint8_t>(0x80 | command);
        packet[5] = static_cast<std::uint8_t>(payload.size() >> 8);
        packet[6] = static_cast<std::uint8_t>(payload.size());
        std::size_t offset = std::min(payload.size(), size - 7);
        std::copy_n(payload.begin(), offset, packet.begin() + 7);
        m_link->device_write(packet);
        for (std::uint8_t seq = 0; offset < payload.size(); ++seq) {
            std::fill(packet.begin(), packet.end(), 0);
            put_cid();
            packet[4] = seq;
            std::size_t const chunk = std::min(payload.size() - offset, size - 5);
            std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset), chunk, packet.begin() + 5);
            offset += chunk;
            m_link->device_write(packet);
        }
    }

    void send_error(std::uint32_t cid, std::uint8_t code)
    {
        std::uint8_t const data[] = { code };
        send_message(cid, 0x3f, data);
    }

    void send_keepalive(std::uint32_t cid, std::uint8_t status)
    {
        std::uint8_t const data[] = { status };
        send_message(cid, 0x3b, data);
        ++m_stats.keepalives_sent;
    }

    struct Pending {
        bool active = false;
        std::uint32_t cid = 0;
        std::uint8_t command = 0;
        std::size_t length = 0;
        std::uint8_t next_seq = 0;
        std::vector<std::uint8_t> data;
        SoftClock::time_point started;
    };

    bool locked_by_other(std::uint32_t cid) const { return m_lock_cid != 0 && m_lock_cid != cid && SoftClock::now() < m_lock_until; }

    void allocate_channel(std::span<std::uint8_t const> packet)
    {
        std::size_t const length = std::size_t { packet[5] } << 8 | packet[6];
        if (length != 8) {
            send_error(broadcast, 0x03);
            return;
        }
        std::uint32_t const cid = ++m_next_cid;
        m_channels.insert(cid);
        std::vector<std::uint8_t> answer(packet.begin() + 7, packet.begin() + 15);
        answer.push_back(static_cast<std::uint8_t>(cid >> 24));
        answer.push_back(static_cast<std::uint8_t>(cid >> 16));
        answer.push_back(static_cast<std::uint8_t>(cid >> 8));
        answer.push_back(static_cast<std::uint8_t>(cid));
        answer.push_back(2); // CTAPHID protocol version
        answer.push_back(1);
        answer.push_back(2);
        answer.push_back(3);
        std::uint8_t capabilities = 0;
        if (m_options.wink)
            capabilities |= 0x01;
        if (m_options.protocol != SoftOptions::Protocol::U2fOnly)
            capabilities |= 0x04;
        if (!speaks_u2f())
            capabilities |= 0x08;
        answer.push_back(capabilities);
        send_message(broadcast, 0x06, answer);
    }

    bool speaks_u2f() const { return m_options.protocol == SoftOptions::Protocol::U2fOnly || m_options.speaks_u2f; }

    void run()
    {
        while (!m_stop) {
            auto packet = m_link->device_read(std::chrono::milliseconds(20));
            if (m_link->unplugged())
                return;
            if (m_pending.active && SoftClock::now() - m_pending.started > std::chrono::milliseconds(m_options.message_timeout_ms)) {
                send_error(m_pending.cid, 0x05);
                m_pending = {};
            }
            if (!packet)
                continue;
            take_packet(*packet);
        }
    }

    void take_packet(std::span<std::uint8_t const> packet)
    {
        std::uint32_t const cid = cid_of(packet);
        bool const init = (packet[4] & 0x80) != 0;
        if (!init) {
            // A continuation: ours to append, or spurious (§11.2.5.4).
            if (!m_pending.active || cid != m_pending.cid)
                return;
            if (packet[4] != m_pending.next_seq) {
                send_error(cid, 0x04);
                m_pending = {};
                return;
            }
            ++m_pending.next_seq;
            std::size_t const chunk = std::min(m_pending.length - m_pending.data.size(), m_options.report_size - 5);
            m_pending.data.insert(m_pending.data.end(), packet.begin() + 5, packet.begin() + 5 + static_cast<std::ptrdiff_t>(chunk));
            if (m_pending.data.size() == m_pending.length) {
                Pending done = std::move(m_pending);
                m_pending = {};
                dispatch(done.cid, done.command, done.data);
            }
            return;
        }
        std::uint8_t const command = static_cast<std::uint8_t>(packet[4] & 0x7f);
        if (cid == 0) {
            send_error(cid, 0x0b);
            return;
        }
        if (command == 0x06) {
            if (cid == broadcast) {
                allocate_channel(packet);
                return;
            }
            if (!m_channels.contains(cid)) {
                send_error(cid, 0x0b);
                return;
            }
            // Resynchronization of an allocated channel (§11.2.9.1.3).
            if (m_pending.active && m_pending.cid == cid)
                m_pending = {};
            std::vector<std::uint8_t> answer(packet.begin() + 7, packet.begin() + 15);
            for (int shift = 24; shift >= 0; shift -= 8)
                answer.push_back(static_cast<std::uint8_t>(cid >> shift));
            answer.insert(answer.end(), { 2, 1, 2, 3, 0x05 });
            send_message(cid, 0x06, answer);
            return;
        }
        if (cid == broadcast || !m_channels.contains(cid)) {
            send_error(cid, 0x0b);
            return;
        }
        if (command == 0x11)
            return; // CANCEL with nothing running: ignored, never answered
        if ((m_pending.active && m_pending.cid != cid) || locked_by_other(cid)) {
            ++m_stats.busy_errors;
            send_error(cid, 0x06);
            return;
        }
        if (m_pending.active && m_pending.cid == cid) {
            // A new message before the last one was whole.
            send_error(cid, 0x04);
            m_pending = {};
            return;
        }
        std::size_t const length = std::size_t { packet[5] } << 8 | packet[6];
        if (length > (m_options.report_size - 7) + 128 * (m_options.report_size - 5)) {
            send_error(cid, 0x03);
            return;
        }
        Pending pending;
        pending.active = true;
        pending.cid = cid;
        pending.command = command;
        pending.length = length;
        pending.started = SoftClock::now();
        std::size_t const chunk = std::min(length, m_options.report_size - 7);
        pending.data.assign(packet.begin() + 7, packet.begin() + 7 + static_cast<std::ptrdiff_t>(chunk));
        if (pending.data.size() == length) {
            dispatch(cid, command, pending.data);
            return;
        }
        m_pending = std::move(pending);
    }

    void dispatch(std::uint32_t cid, std::uint8_t command, std::vector<std::uint8_t> const& payload)
    {
        m_active_cid = cid;
        switch (command) {
        case 0x01: // PING
            send_message(cid, 0x01, payload);
            break;
        case 0x08: // WINK
            if (!m_options.wink || !payload.empty()) {
                send_error(cid, m_options.wink ? 0x03 : 0x01);
                break;
            }
            ++m_stats.winks;
            send_message(cid, 0x08, {});
            break;
        case 0x04: // LOCK
            if (payload.size() != 1 || payload[0] > 10) {
                send_error(cid, payload.size() != 1 ? 0x03 : 0x02);
                break;
            }
            m_lock_cid = payload[0] == 0 ? 0 : cid;
            m_lock_until = SoftClock::now() + std::chrono::seconds(payload[0]);
            send_message(cid, 0x04, {});
            break;
        case 0x10: { // CBOR
            if (m_options.protocol == SoftOptions::Protocol::U2fOnly || payload.empty()) {
                send_error(cid, payload.empty() ? 0x03 : 0x01);
                break;
            }
            ++m_stats.cbor_requests;
            auto const response = handle_cbor(cid, payload);
            if (!m_aborted)
                send_message(cid, 0x10, response);
            m_aborted = false;
            break;
        }
        case 0x03: { // MSG
            if (!speaks_u2f()) {
                send_error(cid, 0x01);
                break;
            }
            ++m_stats.u2f_requests;
            auto const response = handle_apdu(payload);
            send_message(cid, 0x03, response);
            break;
        }
        default:
            send_error(cid, 0x01);
        }
        m_active_cid = 0;
    }

    // ---------- The scripted user ----------

    enum class Presence : std::uint8_t { Touched, Cancelled, TimedOut, Aborted };

    bool touched_after(SoftClock::duration waited)
    {
        std::lock_guard lock(m_touch_mutex);
        switch (m_touch.kind) {
        case TouchPlan::Kind::Immediately:
            return true;
        case TouchPlan::Kind::After:
            return waited >= std::chrono::milliseconds(m_touch.milliseconds);
        case TouchPlan::Kind::Never:
            return false;
        }
        return false;
    }

    // Waits for the touch, sending KEEPALIVE with `status` every
    // keepalive_ms and watching for CANCEL or INIT on the active channel;
    // other channels are told the device is busy.
    Presence wait(std::uint32_t cid, std::uint8_t status, std::chrono::milliseconds limit, bool want_touch)
    {
        auto const start = SoftClock::now();
        auto next_keepalive = start;
        while (true) {
            auto const now = SoftClock::now();
            if (want_touch && touched_after(now - start)) {
                ++m_stats.touches;
                return Presence::Touched;
            }
            if (now - start >= limit)
                return Presence::TimedOut;
            if (m_link->unplugged() || m_stop)
                return Presence::Aborted;
            if (now >= next_keepalive) {
                send_keepalive(cid, status);
                next_keepalive = now + std::chrono::milliseconds(m_options.keepalive_ms);
            }
            auto packet = m_link->device_read(std::chrono::milliseconds(5));
            if (!packet)
                continue;
            std::uint32_t const from = cid_of(*packet);
            bool const init = ((*packet)[4] & 0x80) != 0;
            if (!init)
                continue;
            std::uint8_t const command = static_cast<std::uint8_t>((*packet)[4] & 0x7f);
            if (from == cid && command == 0x11) {
                ++m_stats.cancels_honored;
                return Presence::Cancelled;
            }
            if (from == cid && command == 0x06) {
                take_packet(*packet); // answers the resynchronization
                m_aborted = true;
                return Presence::Aborted;
            }
            if (from == broadcast && command == 0x06) {
                allocate_channel(*packet);
                continue;
            }
            if (from != cid && command != 0x11) {
                ++m_stats.busy_errors;
                send_error(from, 0x06);
            }
        }
    }

    Presence collect_presence(std::uint32_t cid)
    {
        ++m_stats.presence_requests;
        return wait(cid, 2, std::chrono::milliseconds(m_options.up_timeout_ms), true);
    }

    // ---------- CTAP2 (§6) ----------

    static std::vector<std::uint8_t> status_only(Status status) { return { static_cast<std::uint8_t>(status) }; }

    static std::vector<std::uint8_t> ok_with(auth::cbor::Value const& body)
    {
        std::vector<std::uint8_t> out { 0x00 };
        auth::cbor::encode_into(body, out);
        return out;
    }

    std::vector<std::uint8_t> handle_cbor(std::uint32_t cid, std::vector<std::uint8_t> const& payload)
    {
        if (m_options.processing_ms > 0) {
            auto const outcome = wait(cid, 1, std::chrono::milliseconds(m_options.processing_ms), false);
            if (outcome == Presence::Cancelled)
                return status_only(Status::KeepaliveCancel);
            if (outcome == Presence::Aborted)
                return {};
        }
        std::uint8_t const command = payload[0];
        std::optional<auth::cbor::Value> params;
        if (payload.size() > 1) {
            params = auth::cbor::decode(std::span<std::uint8_t const>(payload).subspan(1), auth::cbor::Mode::Ctap2);
            if (!params)
                return status_only(Status::InvalidCbor);
            if (!params->is_map())
                return status_only(Status::CborUnexpectedType);
        }
        if (payload.size() > m_options.max_msg_size)
            return status_only(Status::RequestTooLarge);
        std::unique_lock lock(m_mutex);
        switch (command) {
        case auth::ctap2::cmd_get_info:
            return ok_with(get_info());
        case auth::ctap2::cmd_make_credential:
            return params ? make_credential(cid, *params, lock) : status_only(Status::MissingParameter);
        case auth::ctap2::cmd_get_assertion:
            return params ? get_assertion(cid, *params, lock) : status_only(Status::MissingParameter);
        case auth::ctap2::cmd_get_next_assertion:
            return get_next_assertion();
        case auth::ctap2::cmd_client_pin:
            return params ? client_pin(cid, *params, lock) : status_only(Status::MissingParameter);
        case auth::ctap2::cmd_selection: {
            if (m_options.protocol != SoftOptions::Protocol::Ctap21)
                return status_only(Status::InvalidCommand);
            auto const presence = unlocked_presence(cid, lock);
            if (presence == Presence::Cancelled)
                return status_only(Status::KeepaliveCancel);
            if (presence != Presence::Touched)
                return status_only(Status::UserActionTimeout);
            return status_only(Status::Ok);
        }
        case auth::ctap2::cmd_reset: {
            auto const presence = unlocked_presence(cid, lock);
            if (presence == Presence::Cancelled)
                return status_only(Status::KeepaliveCancel);
            if (presence != Presence::Touched)
                return status_only(Status::UserActionTimeout);
            m_credentials.clear();
            m_pin_hash.reset();
            m_pin_retries = 8;
            reset_token();
            return status_only(Status::Ok);
        }
        }
        return status_only(Status::InvalidCommand);
    }

    Presence unlocked_presence(std::uint32_t cid, std::unique_lock<std::mutex>& lock)
    {
        lock.unlock();
        auto const presence = collect_presence(cid);
        lock.lock();
        return presence;
    }

    bool is_21() const { return m_options.protocol == SoftOptions::Protocol::Ctap21; }
    bool uv_configured() const { return m_options.built_in_uv && m_options.uv_enrolled; }
    bool uv_protected() const { return m_pin_hash.has_value() || uv_configured(); }

    auth::cbor::Value get_info()
    {
        using auth::cbor::Value;
        Value map = Value::map();
        std::vector<Value> versions;
        if (speaks_u2f())
            versions.push_back(Value::text("U2F_V2"));
        versions.push_back(Value::text("FIDO_2_0"));
        if (is_21())
            versions.push_back(Value::text("FIDO_2_1"));
        map.set(0x01, Value::array(std::move(versions)));
        std::vector<Value> extensions;
        if (m_options.cred_protect)
            extensions.push_back(Value::text("credProtect"));
        if (m_options.hmac_secret)
            extensions.push_back(Value::text("hmac-secret"));
        if (m_options.cred_blob && is_21())
            extensions.push_back(Value::text("credBlob"));
        if (m_options.large_blob_key && is_21())
            extensions.push_back(Value::text("largeBlobKey"));
        if (m_options.min_pin_length_extension && is_21())
            extensions.push_back(Value::text("minPinLength"));
        if (!extensions.empty())
            map.set(0x02, Value::array(std::move(extensions)));
        map.set(0x03, Value::bytes(m_options.aaguid));
        Value options = Value::map();
        options.set("rk", Value::boolean(m_options.resident_keys));
        options.set("up", Value::boolean(true));
        if (m_options.built_in_uv)
            options.set("uv", Value::boolean(m_options.uv_enrolled));
        if (m_options.client_pin)
            options.set("clientPin", Value::boolean(m_pin_hash.has_value()));
        if (is_21()) {
            options.set("pinUvAuthToken", Value::boolean(true));
            options.set("makeCredUvNotRqd", Value::boolean(m_options.make_cred_uv_not_rqd));
            if (m_options.always_uv)
                options.set("alwaysUv", Value::boolean(true));
        }
        map.set(0x04, std::move(options));
        map.set(0x05, Value::unsigned_integer(m_options.max_msg_size));
        if (m_options.client_pin || m_options.built_in_uv) {
            std::vector<Value> protocols;
            for (auto const p : m_options.pin_protocols)
                protocols.push_back(Value::unsigned_integer(p));
            map.set(0x06, Value::array(std::move(protocols)));
        }
        map.set(0x07, Value::unsigned_integer(m_options.max_credential_count_in_list));
        map.set(0x08, Value::unsigned_integer(m_options.max_credential_id_length));
        if (is_21()) {
            map.set(0x09, Value::array({ Value::text("usb") }));
            std::vector<Value> algorithms;
            for (auto const alg : m_options.algorithms) {
                Value entry = Value::map();
                entry.set("alg", Value::integer(alg));
                entry.set("type", Value::text("public-key"));
                algorithms.push_back(std::move(entry));
            }
            map.set(0x0a, Value::array(std::move(algorithms)));
            map.set(0x0c, Value::boolean(m_force_pin_change));
            map.set(0x0d, Value::unsigned_integer(m_options.min_pin_length));
            map.set(0x0e, Value::unsigned_integer(0x010203));
            if (m_options.cred_blob)
                map.set(0x0f, Value::unsigned_integer(m_options.max_cred_blob_length));
            map.set(0x14, Value::unsigned_integer(25 - std::min<std::size_t>(25, discoverable_count())));
        }
        return map;
    }

    std::size_t discoverable_count() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(m_credentials, [](Credential const& c) { return c.discoverable; }));
    }

    // pinUvAuthToken state (§6.5.2.1).
    void reset_token()
    {
        platform::fill_random(m_token);
        m_token_in_use = false;
        m_token_permissions = 0;
        m_token_rp_id.reset();
        m_token_user_present = false;
        m_token_user_verified = false;
    }

    void regenerate_key_agreement()
    {
        m_key_agreement = *auth::pin::generate_key_agreement_key(platform::fill_random);
    }

    std::optional<auth::pin::Protocol> supported_protocol(auth::cbor::Value const* value) const
    {
        if (value == nullptr || !value->is_unsigned())
            return std::nullopt;
        auto const protocol = auth::pin::protocol_from_number(static_cast<std::int64_t>(value->raw_number()));
        if (!protocol || std::ranges::find(m_options.pin_protocols, static_cast<std::uint8_t>(*protocol)) == m_options.pin_protocols.end())
            return std::nullopt;
        return protocol;
    }

    // verify(pinUvAuthToken, message, param) as §6.1.2 step 11 and §6.2.2
    // step 6 use it, permissions and the RP ID included.
    Status check_token(auth::pin::Protocol protocol, std::span<std::uint8_t const> message, std::span<std::uint8_t const> param,
        std::uint8_t permission, std::string const& rp_id)
    {
        std::span<std::uint8_t const> const token(m_token.data(), protocol == auth::pin::Protocol::One ? 32 : 32);
        if (!m_token_in_use || !auth::pin::verify(protocol, token, message, param))
            return Status::PinAuthInvalid;
        if ((m_token_permissions & permission) == 0)
            return Status::PinAuthInvalid;
        if (m_token_rp_id && *m_token_rp_id != rp_id)
            return Status::PinAuthInvalid;
        if (!m_token_rp_id)
            m_token_rp_id = rp_id;
        if (!m_token_user_verified)
            return Status::PinAuthInvalid;
        return Status::Ok;
    }

    Status built_in_uv()
    {
        if (m_uv_retries <= 0)
            return Status::UvBlocked;
        if (m_uv_succeeds) {
            m_uv_retries = 8;
            return Status::Ok;
        }
        --m_uv_retries;
        return m_uv_retries <= 0 ? Status::UvBlocked : Status::UvInvalid;
    }

    static std::array<std::uint8_t, 32> sha256(std::string_view text)
    {
        return crypto::Sha256::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
    }

    auth::CoseKey public_key_of(Credential const& credential) const
    {
        if (credential.alg == auth::cose::alg_eddsa) {
            crypto::Ed25519Seed seed;
            std::ranges::copy(credential.private_key, seed.begin());
            auto const pk = crypto::ed25519_public_key(seed);
            return auth::cose_key_from_ed25519(std::span<std::uint8_t const, 32>(pk.data(), 32));
        }
        auto const point = crypto::ec_public_point(crypto::CurveId::P256, credential.private_key);
        return auth::cose_key_from_ec(crypto::CurveId::P256, *point, auth::cose::alg_es256);
    }

    static std::vector<std::uint8_t> sign_with(std::int64_t alg, std::span<std::uint8_t const, 32> key, std::span<std::uint8_t const> data)
    {
        if (alg == auth::cose::alg_eddsa) {
            crypto::Ed25519Seed seed;
            std::ranges::copy(key, seed.begin());
            auto const sig = crypto::ed25519_sign(seed, data);
            return std::vector<std::uint8_t>(sig.begin(), sig.end());
        }
        auto const digest = crypto::Sha256::hash(data);
        auto const rs = crypto::ecdsa_sign(crypto::CurveId::P256, key, digest, platform::fill_random);
        return auth::der::ecdsa_signature(rs->first, rs->second);
    }

    static std::array<std::uint8_t, 32> new_p256_scalar()
    {
        std::array<std::uint8_t, 32> scalar {};
        do {
            platform::fill_random(scalar);
        } while (!crypto::ec_private_scalar_valid(crypto::CurveId::P256, scalar));
        return scalar;
    }

    Credential const* find_credential(std::span<std::uint8_t const> id, std::array<std::uint8_t, 32> const& rp_id_hash) const
    {
        for (auto const& c : m_credentials) {
            if (c.rp_id_hash == rp_id_hash && std::ranges::equal(c.id, id))
                return &c;
        }
        return nullptr;
    }

    static std::vector<std::uint8_t> auth_data(std::array<std::uint8_t, 32> const& rp_id_hash, std::uint8_t flags, std::uint32_t count,
        std::vector<std::uint8_t> const& attested, std::optional<auth::cbor::Value> const& extensions)
    {
        std::vector<std::uint8_t> out(rp_id_hash.begin(), rp_id_hash.end());
        if (extensions)
            flags |= 0x80;
        if (!attested.empty())
            flags |= 0x40;
        out.push_back(flags);
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(count >> shift));
        out.insert(out.end(), attested.begin(), attested.end());
        if (extensions)
            auth::cbor::encode_into(*extensions, out);
        return out;
    }

    std::vector<std::uint8_t> make_credential(std::uint32_t cid, auth::cbor::Value const& params, std::unique_lock<std::mutex>& lock)
    {
        using auth::cbor::Value;
        Value const* const client_data_hash = params.find(0x01);
        Value const* const rp = params.find(0x02);
        Value const* const user = params.find(0x03);
        Value const* const algorithms = params.find(0x04);
        Value const* const exclude = params.find(0x05);
        Value const* const extensions = params.find(0x06);
        Value const* const options = params.find(0x07);
        Value const* const pin_param = params.find(0x08);
        Value const* const pin_protocol = params.find(0x09);

        // A zero-length pinUvAuthParam: the touch that picks a key (§6.1.2 step 1).
        if (pin_param != nullptr && pin_param->is_bytes() && pin_param->as_bytes().empty()) {
            auto const presence = unlocked_presence(cid, lock);
            if (presence == Presence::Cancelled)
                return status_only(Status::KeepaliveCancel);
            if (presence != Presence::Touched)
                return status_only(Status::OperationDenied);
            return status_only(m_pin_hash ? Status::PinInvalid : Status::PinNotSet);
        }
        if (client_data_hash == nullptr || rp == nullptr || user == nullptr || algorithms == nullptr)
            return status_only(Status::MissingParameter);
        if (!client_data_hash->is_bytes() || !rp->is_map() || !user->is_map() || !algorithms->is_array())
            return status_only(Status::CborUnexpectedType);
        Value const* const rp_id_value = rp->find("id");
        if (rp_id_value == nullptr || !rp_id_value->is_text())
            return status_only(Status::MissingParameter);
        std::string const rp_id(rp_id_value->as_text());
        auto const parsed_user = auth::user_from_cbor(*user);
        if (!parsed_user)
            return status_only(Status::MissingParameter);
        std::optional<std::int64_t> chosen;
        for (auto const& entry : algorithms->items()) {
            Value const* const alg = entry.find("alg");
            Value const* const type = entry.find("type");
            if (alg == nullptr || type == nullptr || !type->is_text() || !alg->as_int64())
                return status_only(Status::CborUnexpectedType);
            if (type->as_text() == "public-key" && std::ranges::find(m_options.algorithms, *alg->as_int64()) != m_options.algorithms.end()) {
                chosen = *alg->as_int64();
                break;
            }
        }
        if (!chosen)
            return status_only(Status::UnsupportedAlgorithm);
        if (pin_param != nullptr) {
            if (pin_protocol == nullptr)
                return status_only(Status::MissingParameter);
            if (!supported_protocol(pin_protocol))
                return status_only(Status::InvalidParameter);
        }
        bool rk = false;
        bool uv = false;
        if (options != nullptr) {
            if (Value const* const v = options->find("rk"); v != nullptr && v->as_bool())
                rk = *v->as_bool();
            if (Value const* const v = options->find("uv"); v != nullptr && v->as_bool())
                uv = *v->as_bool();
            if (Value const* const v = options->find("up"); v != nullptr && v->as_bool() == false)
                return status_only(Status::InvalidOption);
        }
        if (pin_param != nullptr)
            uv = false;
        if (rk && !m_options.resident_keys)
            return status_only(Status::UnsupportedOption);
        if (uv && !uv_configured())
            return status_only(Status::InvalidOption);

        bool uv_done = false;
        if (uv_protected()) {
            if (pin_param != nullptr) {
                auto const protocol = *supported_protocol(pin_protocol);
                Status const s = check_token(protocol, client_data_hash->as_bytes(), pin_param->as_bytes(), auth::ctap2::permission_make_credential, rp_id);
                if (s != Status::Ok)
                    return status_only(s);
                uv_done = true;
            } else if (uv) {
                Status const s = built_in_uv();
                if (s != Status::Ok)
                    return status_only(m_pin_hash && s != Status::UvBlocked ? Status::PuatRequired : s);
                uv_done = true;
            } else if (is_21() ? (m_options.always_uv || rk || !m_options.make_cred_uv_not_rqd) : m_pin_hash.has_value()) {
                // §6.1.2 step 6: without makeCredUvNotRqd (or for a
                // discoverable credential) a protected key wants UV. CTAP
                // 2.0 asked for it only when a PIN was set
                // (CTAP2_ERR_PIN_REQUIRED, the same code).
                return status_only(m_pin_hash ? Status::PuatRequired : Status::OperationDenied);
            }
        }

        auto const rp_id_hash = sha256(rp_id);
        // The exclude list (§6.1.2 step 12).
        if (exclude != nullptr && exclude->is_array()) {
            for (auto const& entry : exclude->items()) {
                Value const* const id = entry.find("id");
                if (id == nullptr || !id->is_bytes())
                    return status_only(Status::CborUnexpectedType);
                Credential const* const found = find_credential(id->as_bytes(), rp_id_hash);
                if (found == nullptr || (found->cred_protect == 3 && !uv_done))
                    continue;
                auto const presence = unlocked_presence(cid, lock);
                if (presence == Presence::Cancelled)
                    return status_only(Status::KeepaliveCancel);
                if (presence != Presence::Touched)
                    return status_only(Status::OperationDenied);
                return status_only(Status::CredentialExcluded);
            }
        }

        // Extensions (§12).
        int cred_protect = 1;
        bool want_cred_protect = false;
        bool want_hmac_secret = false;
        std::optional<std::vector<std::uint8_t>> cred_blob;
        bool want_cred_blob = false;
        bool want_large_blob_key = false;
        bool want_min_pin_length = false;
        if (extensions != nullptr && extensions->is_map()) {
            if (Value const* const v = extensions->find("credProtect"); v != nullptr && m_options.cred_protect) {
                if (!v->is_unsigned() || v->raw_number() < 1 || v->raw_number() > 3)
                    return status_only(Status::InvalidParameter);
                cred_protect = static_cast<int>(v->raw_number());
                want_cred_protect = true;
            }
            if (Value const* const v = extensions->find("hmac-secret"); v != nullptr && m_options.hmac_secret)
                want_hmac_secret = v->as_bool() == true;
            if (Value const* const v = extensions->find("credBlob"); v != nullptr && m_options.cred_blob && is_21()) {
                if (!v->is_bytes())
                    return status_only(Status::CborUnexpectedType);
                want_cred_blob = true;
                if (v->as_bytes().size() <= m_options.max_cred_blob_length)
                    cred_blob = v->byte_vector();
            }
            if (Value const* const v = extensions->find("largeBlobKey"); v != nullptr && m_options.large_blob_key && is_21()) {
                if (v->as_bool() != true || !rk)
                    return status_only(Status::InvalidOption);
                want_large_blob_key = true;
            }
            if (Value const* const v = extensions->find("minPinLength"); v != nullptr && m_options.min_pin_length_extension && is_21())
                want_min_pin_length = v->as_bool() == true;
        }

        // User presence (§6.1.2 step 14), unless the token carries it.
        bool up_from_token = pin_param != nullptr && m_token_user_present;
        if (!up_from_token) {
            auto const presence = unlocked_presence(cid, lock);
            if (presence == Presence::Cancelled)
                return status_only(Status::KeepaliveCancel);
            if (presence == Presence::Aborted)
                return {};
            if (presence != Presence::Touched)
                return status_only(Status::OperationDenied);
        }
        m_token_user_present = false;
        m_token_user_verified = false;
        m_token_permissions &= auth::ctap2::permission_large_blob_write;

        Credential credential;
        credential.id.resize(48);
        platform::fill_random(credential.id);
        credential.rp_id_hash = rp_id_hash;
        credential.rp_id = rp_id;
        credential.user = *parsed_user;
        credential.alg = *chosen;
        if (*chosen == auth::cose::alg_eddsa)
            platform::fill_random(credential.private_key);
        else
            credential.private_key = new_p256_scalar();
        credential.discoverable = rk;
        credential.cred_protect = cred_protect;
        platform::fill_random(credential.cred_random_with_uv);
        platform::fill_random(credential.cred_random_without_uv);
        credential.cred_blob = cred_blob;
        if (want_large_blob_key) {
            credential.large_blob_key.emplace(32);
            platform::fill_random(*credential.large_blob_key);
        }
        credential.created = ++m_creation_counter;
        if (rk) {
            // A new discoverable credential replaces one for the same user (§6.1.2 step 17).
            std::erase_if(m_credentials, [&](Credential const& c) { return c.discoverable && c.rp_id_hash == rp_id_hash && c.user.id == parsed_user->id; });
        }
        m_credentials.push_back(credential);

        std::optional<Value> extension_outputs;
        auto add_output = [&](std::string_view name, Value value) {
            if (!extension_outputs)
                extension_outputs = Value::map();
            extension_outputs->set(name, std::move(value));
        };
        if (want_cred_protect)
            add_output("credProtect", Value::unsigned_integer(static_cast<std::uint64_t>(cred_protect)));
        if (want_hmac_secret)
            add_output("hmac-secret", Value::boolean(true));
        if (want_cred_blob)
            add_output("credBlob", Value::boolean(cred_blob.has_value()));
        if (want_min_pin_length && std::ranges::find(m_options.min_pin_length_rp_ids, rp_id) != m_options.min_pin_length_rp_ids.end())
            add_output("minPinLength", Value::unsigned_integer(m_options.min_pin_length));

        std::vector<std::uint8_t> attested(m_options.aaguid.begin(), m_options.aaguid.end());
        attested.push_back(static_cast<std::uint8_t>(credential.id.size() >> 8));
        attested.push_back(static_cast<std::uint8_t>(credential.id.size()));
        attested.insert(attested.end(), credential.id.begin(), credential.id.end());
        auth::cbor::encode_into(auth::cose_key_to_cbor(public_key_of(credential)), attested);
        std::uint8_t const flags = static_cast<std::uint8_t>(0x01 | (uv_done ? 0x04 : 0));
        auto const data = auth_data(rp_id_hash, flags, 0, attested, extension_outputs);

        // "packed" (WebAuthn §8.2): sig over authData || clientDataHash.
        std::vector<std::uint8_t> signed_data = data;
        signed_data.insert(signed_data.end(), client_data_hash->as_bytes().begin(), client_data_hash->as_bytes().end());
        Value statement = Value::map();
        if (m_options.self_attestation) {
            statement.set("alg", Value::integer(credential.alg));
            statement.set("sig", Value::bytes(sign_with(credential.alg, credential.private_key, signed_data)));
        } else {
            statement.set("alg", Value::integer(auth::cose::alg_es256));
            statement.set("sig", Value::bytes(sign_with(auth::cose::alg_es256, m_attestation_scalar, signed_data)));
            statement.set("x5c", Value::array({ Value::bytes(m_attestation_cert) }));
        }
        Value response = Value::map();
        response.set(0x01, Value::text("packed"));
        response.set(0x02, Value::bytes(data));
        response.set(0x03, std::move(statement));
        if (credential.large_blob_key)
            response.set(0x05, Value::bytes(*credential.large_blob_key));
        return ok_with(response);
    }

    struct AssertionState {
        std::vector<std::size_t> credentials; // indexes, newest first
        std::size_t next = 0;
        std::vector<std::uint8_t> client_data_hash;
        std::array<std::uint8_t, 32> rp_id_hash {};
        std::uint8_t flags = 0;
        std::optional<auth::cbor::Value> extensions;
        bool uv = false;
        SoftClock::time_point started;
        std::optional<auth::cbor::Value> hmac_input;
        bool want_cred_blob = false;
        bool want_large_blob_key = false;
    };

    std::vector<std::uint8_t> assertion_for(std::size_t index, AssertionState const& state, bool with_count, std::size_t count, bool allow_list)
    {
        using auth::cbor::Value;
        Credential& credential = m_credentials[index];
        ++credential.sign_count;
        std::optional<Value> outputs;
        if (state.hmac_input) {
            auto const output = hmac_secret_output(*state.hmac_input, credential, state.uv);
            if (!output.first.empty())
                return output.first;
            if (output.second) {
                outputs = Value::map();
                outputs->set("hmac-secret", Value::bytes(*output.second));
            }
        }
        if (state.want_cred_blob) {
            if (!outputs)
                outputs = Value::map();
            outputs->set("credBlob", Value::bytes(credential.cred_blob.value_or(std::vector<std::uint8_t> {})));
        }
        auto const data = auth_data(state.rp_id_hash, state.flags, credential.sign_count, {}, outputs);
        std::vector<std::uint8_t> signed_data = data;
        signed_data.insert(signed_data.end(), state.client_data_hash.begin(), state.client_data_hash.end());
        Value response = Value::map();
        response.set(0x01, auth::descriptor_to_cbor({ credential.id, {} }));
        response.set(0x02, Value::bytes(data));
        response.set(0x03, Value::bytes(sign_with(credential.alg, credential.private_key, signed_data)));
        if (credential.discoverable && !allow_list) {
            auth::UserEntity user = credential.user;
            if (!state.uv) {
                // No identifying information without UV (§6.2.2 step 12).
                user.has_name = false;
                user.has_display_name = false;
            }
            response.set(0x04, auth::user_to_cbor(user));
        }
        if (with_count && count > 1)
            response.set(0x05, Value::unsigned_integer(count));
        if (state.want_large_blob_key && credential.large_blob_key)
            response.set(0x07, Value::bytes(*credential.large_blob_key));
        return ok_with(response);
    }

    // hmac-secret (§12.5): {error status} or the encrypted output.
    std::pair<std::vector<std::uint8_t>, std::optional<std::vector<std::uint8_t>>> hmac_secret_output(auth::cbor::Value const& input,
        Credential const& credential, bool uv)
    {
        using auth::cbor::Value;
        Value const* const key = input.find(0x01);
        Value const* const salt_enc = input.find(0x02);
        Value const* const salt_auth = input.find(0x03);
        Value const* const protocol_value = input.find(0x04);
        if (key == nullptr || salt_enc == nullptr || salt_auth == nullptr || !salt_enc->is_bytes() || !salt_auth->is_bytes())
            return { status_only(Status::MissingParameter), std::nullopt };
        auto protocol = auth::pin::Protocol::One;
        if (protocol_value != nullptr) {
            auto const p = supported_protocol(protocol_value);
            if (!p)
                return { status_only(Status::InvalidParameter), std::nullopt };
            protocol = *p;
        } else if (std::ranges::find(m_options.pin_protocols, 1) == m_options.pin_protocols.end()) {
            return { status_only(Status::PinAuthInvalid), std::nullopt };
        }
        auto const peer = auth::cose_key_from_cbor(*key);
        if (!peer)
            return { status_only(Status::InvalidParameter), std::nullopt };
        auto const shared = auth::pin::ecdh(protocol, m_key_agreement, *peer);
        if (!shared)
            return { status_only(Status::InvalidParameter), std::nullopt };
        if (!auth::pin::verify(protocol, *shared, salt_enc->as_bytes(), salt_auth->as_bytes()))
            return { status_only(Status::PinAuthInvalid), std::nullopt };
        auto const salts = auth::pin::decrypt(protocol, *shared, salt_enc->as_bytes());
        if (!salts || (salts->size() != 32 && salts->size() != 64))
            return { status_only(Status::InvalidParameter), std::nullopt };
        auto const& random = uv ? credential.cred_random_with_uv : credential.cred_random_without_uv;
        std::vector<std::uint8_t> outputs;
        for (std::size_t offset = 0; offset < salts->size(); offset += 32) {
            auto const mac = crypto::Hmac<crypto::Sha256>::mac(random, std::span<std::uint8_t const>(*salts).subspan(offset, 32));
            outputs.insert(outputs.end(), mac.begin(), mac.end());
        }
        auto encrypted = auth::pin::encrypt(protocol, *shared, outputs, platform::fill_random);
        return { {}, std::move(encrypted) };
    }

    std::vector<std::uint8_t> get_assertion(std::uint32_t cid, auth::cbor::Value const& params, std::unique_lock<std::mutex>& lock)
    {
        using auth::cbor::Value;
        Value const* const rp_id_value = params.find(0x01);
        Value const* const client_data_hash = params.find(0x02);
        Value const* const allow = params.find(0x03);
        Value const* const extensions = params.find(0x04);
        Value const* const options = params.find(0x05);
        Value const* const pin_param = params.find(0x06);
        Value const* const pin_protocol = params.find(0x07);

        if (pin_param != nullptr && pin_param->is_bytes() && pin_param->as_bytes().empty()) {
            auto const presence = unlocked_presence(cid, lock);
            if (presence == Presence::Cancelled)
                return status_only(Status::KeepaliveCancel);
            if (presence != Presence::Touched)
                return status_only(Status::OperationDenied);
            return status_only(m_pin_hash ? Status::PinInvalid : Status::PinNotSet);
        }
        if (rp_id_value == nullptr || client_data_hash == nullptr)
            return status_only(Status::MissingParameter);
        if (!rp_id_value->is_text() || !client_data_hash->is_bytes())
            return status_only(Status::CborUnexpectedType);
        std::string const rp_id(rp_id_value->as_text());
        if (pin_param != nullptr) {
            if (pin_protocol == nullptr)
                return status_only(Status::MissingParameter);
            if (!supported_protocol(pin_protocol))
                return status_only(Status::InvalidParameter);
        }
        bool up = true;
        bool uv = false;
        if (options != nullptr) {
            if (Value const* const v = options->find("up"); v != nullptr && v->as_bool())
                up = *v->as_bool();
            if (Value const* const v = options->find("uv"); v != nullptr && v->as_bool())
                uv = *v->as_bool();
            if (options->find("rk") != nullptr)
                return status_only(Status::UnsupportedOption);
        }
        if (pin_param != nullptr)
            uv = false;
        if (uv && !uv_configured())
            return status_only(Status::InvalidOption);
        if (m_options.always_uv && up && pin_param == nullptr && !uv) {
            if (!uv_protected())
                return status_only(m_options.client_pin ? Status::PuatRequired : Status::OperationDenied);
            if (uv_configured())
                uv = true;
            else
                return status_only(Status::PuatRequired);
        }
        bool uv_done = false;
        bool up_from_uv = false;
        if (uv_protected()) {
            if (pin_param != nullptr) {
                Status const s = check_token(*supported_protocol(pin_protocol), client_data_hash->as_bytes(), pin_param->as_bytes(),
                    auth::ctap2::permission_get_assertion, rp_id);
                if (s != Status::Ok)
                    return status_only(s);
                uv_done = true;
            } else if (uv) {
                Status const s = built_in_uv();
                if (s != Status::Ok)
                    return status_only(m_pin_hash ? Status::PuatRequired : (s == Status::UvBlocked ? Status::PinBlocked : Status::OperationDenied));
                uv_done = true;
                up_from_uv = true;
            }
        }

        auto const rp_id_hash = sha256(rp_id);
        std::vector<std::size_t> applicable;
        bool const has_allow = allow != nullptr && allow->is_array() && !allow->items().empty();
        if (has_allow) {
            for (auto const& entry : allow->items()) {
                Value const* const id = entry.find("id");
                if (id == nullptr || !id->is_bytes())
                    return status_only(Status::CborUnexpectedType);
                for (std::size_t i = 0; i < m_credentials.size(); ++i) {
                    if (m_credentials[i].rp_id_hash == rp_id_hash && std::ranges::equal(m_credentials[i].id, id->as_bytes())
                        && std::ranges::find(applicable, i) == applicable.end())
                        applicable.push_back(i);
                }
            }
        } else {
            for (std::size_t i = 0; i < m_credentials.size(); ++i) {
                if (m_credentials[i].discoverable && m_credentials[i].rp_id_hash == rp_id_hash)
                    applicable.push_back(i);
            }
        }
        std::erase_if(applicable, [&](std::size_t i) {
            int const level = m_credentials[i].cred_protect;
            return (level == 3 && !uv_done) || (level == 2 && !has_allow && !uv_done);
        });
        if (applicable.empty())
            return status_only(Status::NoCredentials);
        std::ranges::sort(applicable, [&](std::size_t a, std::size_t b) { return m_credentials[a].created > m_credentials[b].created; });

        AssertionState state;
        state.uv = uv_done;
        state.client_data_hash = client_data_hash->byte_vector();
        state.rp_id_hash = rp_id_hash;
        if (extensions != nullptr && extensions->is_map()) {
            if (Value const* const v = extensions->find("hmac-secret"); v != nullptr && m_options.hmac_secret) {
                if (!up)
                    return status_only(Status::UnsupportedOption);
                state.hmac_input = *v;
            }
            if (Value const* const v = extensions->find("credBlob"); v != nullptr && m_options.cred_blob && is_21())
                state.want_cred_blob = v->as_bool() == true;
            if (Value const* const v = extensions->find("largeBlobKey"); v != nullptr && m_options.large_blob_key && is_21()) {
                if (v->as_bool() != true)
                    return status_only(Status::InvalidOption);
                state.want_large_blob_key = true;
            }
        }

        bool up_done = up_from_uv;
        if (up && !up_done) {
            bool const cached = pin_param != nullptr && m_token_user_present;
            if (!cached) {
                auto const presence = unlocked_presence(cid, lock);
                if (presence == Presence::Cancelled)
                    return status_only(Status::KeepaliveCancel);
                if (presence == Presence::Aborted)
                    return {};
                if (presence != Presence::Touched)
                    return status_only(Status::OperationDenied);
            }
            up_done = true;
        }
        if (up) {
            m_token_user_present = false;
            m_token_user_verified = false;
            m_token_permissions &= auth::ctap2::permission_large_blob_write;
        }
        state.flags = static_cast<std::uint8_t>((up_done && up ? 0x01 : 0) | (uv_done ? 0x04 : 0));
        state.credentials = applicable;
        state.started = SoftClock::now();
        state.next = 1;
        std::size_t const count = applicable.size();
        auto response = assertion_for(applicable[0], state, !has_allow, count, has_allow);
        if (!has_allow && count > 1)
            m_assertion_state = std::move(state);
        else
            m_assertion_state.reset();
        return response;
    }

    std::vector<std::uint8_t> get_next_assertion()
    {
        if (!m_assertion_state || m_assertion_state->next >= m_assertion_state->credentials.size())
            return status_only(Status::NotAllowed);
        if (SoftClock::now() - m_assertion_state->started > std::chrono::seconds(30)) {
            m_assertion_state.reset();
            return status_only(Status::NotAllowed);
        }
        std::size_t const index = m_assertion_state->credentials[m_assertion_state->next++];
        m_assertion_state->started = SoftClock::now();
        return assertion_for(index, *m_assertion_state, false, m_assertion_state->credentials.size(), false);
    }

    std::vector<std::uint8_t> client_pin(std::uint32_t cid, auth::cbor::Value const& params, std::unique_lock<std::mutex>& lock)
    {
        using auth::cbor::Value;
        if (!m_options.client_pin && !m_options.built_in_uv)
            return status_only(Status::InvalidCommand);
        Value const* const protocol_value = params.find(0x01);
        Value const* const sub_value = params.find(0x02);
        Value const* const key_value = params.find(0x03);
        Value const* const auth_param = params.find(0x04);
        Value const* const new_pin_enc = params.find(0x05);
        Value const* const pin_hash_enc = params.find(0x06);
        Value const* const permissions_value = params.find(0x09);
        Value const* const rp_id_value = params.find(0x0a);
        if (sub_value == nullptr || !sub_value->is_unsigned())
            return status_only(Status::MissingParameter);
        auto const sub = sub_value->raw_number();

        if (sub == auth::ctap2::pin_get_retries) {
            Value response = Value::map();
            response.set(0x03, Value::unsigned_integer(static_cast<std::uint64_t>(m_pin_retries)));
            response.set(0x04, Value::boolean(m_power_cycle_needed));
            return ok_with(response);
        }
        if (sub == auth::ctap2::pin_get_uv_retries) {
            Value response = Value::map();
            response.set(0x05, Value::unsigned_integer(static_cast<std::uint64_t>(std::max(0, m_uv_retries))));
            return ok_with(response);
        }
        if (protocol_value == nullptr)
            return status_only(Status::MissingParameter);
        auto const protocol = supported_protocol(protocol_value);
        if (!protocol)
            return status_only(Status::InvalidParameter);
        if (sub == auth::ctap2::pin_get_key_agreement) {
            Value response = Value::map();
            response.set(0x01, auth::cose_key_to_cbor(m_key_agreement.public_key));
            return ok_with(response);
        }
        if (key_value == nullptr)
            return status_only(Status::MissingParameter);
        auto const peer = auth::cose_key_from_cbor(*key_value);
        if (!peer)
            return status_only(Status::InvalidParameter);
        auto const shared = auth::pin::ecdh(*protocol, m_key_agreement, *peer);
        if (!shared)
            return status_only(Status::InvalidParameter);

        auto const check_pin_hash = [&](Value const* encrypted) -> Status {
            // §6.5.5.5 and §6.5.5.7.1: the retries go down first, then the PIN is compared.
            if (m_pin_retries <= 0)
                return Status::PinBlocked;
            if (m_power_cycle_needed)
                return Status::PinAuthBlocked;
            --m_pin_retries;
            auto const hash = auth::pin::decrypt(*protocol, *shared, encrypted->as_bytes());
            if (!hash || hash->size() != 16 || !std::equal(hash->begin(), hash->end(), m_pin_hash->begin())) {
                regenerate_key_agreement();
                ++m_consecutive_mismatches;
                if (m_pin_retries <= 0)
                    return Status::PinBlocked;
                if (m_consecutive_mismatches >= 3) {
                    m_power_cycle_needed = true;
                    return Status::PinAuthBlocked;
                }
                return Status::PinInvalid;
            }
            m_pin_retries = 8;
            m_consecutive_mismatches = 0;
            return Status::Ok;
        };
        auto const store_new_pin = [&](Value const* encrypted) -> Status {
            auto const padded = auth::pin::decrypt(*protocol, *shared, encrypted->as_bytes());
            if (!padded || padded->size() < 64)
                return Status::PinPolicyViolation;
            std::size_t length = 0;
            while (length < padded->size() && (*padded)[length] != 0)
                ++length;
            std::size_t code_points = 0;
            for (std::size_t i = 0; i < length; ++i)
                code_points += ((*padded)[i] & 0xc0) != 0x80 ? 1 : 0;
            if (code_points < m_options.min_pin_length || length > 63)
                return Status::PinPolicyViolation;
            auto const digest = crypto::Sha256::hash(std::span<std::uint8_t const>(padded->data(), length));
            std::array<std::uint8_t, 16> hash {};
            std::copy_n(digest.begin(), 16, hash.begin());
            if (m_pin_hash && *m_pin_hash == hash && m_force_pin_change)
                return Status::PinPolicyViolation;
            m_pin_hash = hash;
            m_pin_retries = 8;
            m_force_pin_change = false;
            reset_token();
            return Status::Ok;
        };

        switch (sub) {
        case auth::ctap2::pin_set: {
            if (auth_param == nullptr || new_pin_enc == nullptr || !auth_param->is_bytes() || !new_pin_enc->is_bytes())
                return status_only(Status::MissingParameter);
            if (m_pin_hash)
                return status_only(Status::NotAllowed);
            if (!auth::pin::verify(*protocol, *shared, new_pin_enc->as_bytes(), auth_param->as_bytes()))
                return status_only(Status::PinAuthInvalid);
            return status_only(store_new_pin(new_pin_enc));
        }
        case auth::ctap2::pin_change: {
            if (auth_param == nullptr || new_pin_enc == nullptr || pin_hash_enc == nullptr || !auth_param->is_bytes() || !new_pin_enc->is_bytes()
                || !pin_hash_enc->is_bytes())
                return status_only(Status::MissingParameter);
            if (!m_pin_hash)
                return status_only(Status::PinNotSet);
            if (m_pin_retries <= 0)
                return status_only(Status::PinBlocked);
            std::vector<std::uint8_t> message = new_pin_enc->byte_vector();
            message.insert(message.end(), pin_hash_enc->as_bytes().begin(), pin_hash_enc->as_bytes().end());
            if (!auth::pin::verify(*protocol, *shared, message, auth_param->as_bytes()))
                return status_only(Status::PinAuthInvalid);
            Status const checked = check_pin_hash(pin_hash_enc);
            if (checked != Status::Ok)
                return status_only(checked);
            return status_only(store_new_pin(new_pin_enc));
        }
        case auth::ctap2::pin_get_token:
        case auth::ctap2::pin_get_token_using_pin: {
            if (pin_hash_enc == nullptr || !pin_hash_enc->is_bytes())
                return status_only(Status::MissingParameter);
            std::uint8_t permissions = auth::ctap2::permission_make_credential | auth::ctap2::permission_get_assertion;
            if (sub == auth::ctap2::pin_get_token_using_pin) {
                if (!is_21())
                    return status_only(Status::InvalidSubcommand);
                if (permissions_value == nullptr || !permissions_value->is_unsigned())
                    return status_only(Status::MissingParameter);
                if (permissions_value->raw_number() == 0)
                    return status_only(Status::InvalidParameter);
                permissions = static_cast<std::uint8_t>(permissions_value->raw_number());
                if ((permissions & (auth::ctap2::permission_make_credential | auth::ctap2::permission_get_assertion)) != 0 && rp_id_value == nullptr)
                    return status_only(Status::MissingParameter);
            } else if (permissions_value != nullptr || rp_id_value != nullptr) {
                return status_only(Status::InvalidParameter);
            }
            if (!m_pin_hash)
                return status_only(Status::PinNotSet);
            Status const checked = check_pin_hash(pin_hash_enc);
            if (checked != Status::Ok)
                return status_only(checked);
            if (m_force_pin_change)
                return status_only(Status::PinPolicyViolation);
            reset_token();
            m_token_in_use = true;
            m_token_user_verified = true;
            m_token_user_present = false;
            m_token_permissions = permissions;
            if (rp_id_value != nullptr && rp_id_value->is_text())
                m_token_rp_id = std::string(rp_id_value->as_text());
            auto const encrypted = auth::pin::encrypt(*protocol, *shared, m_token, platform::fill_random);
            Value response = Value::map();
            response.set(0x02, Value::bytes(*encrypted));
            return ok_with(response);
        }
        case auth::ctap2::pin_get_token_using_uv: {
            if (!is_21())
                return status_only(Status::InvalidSubcommand);
            if (permissions_value == nullptr || !permissions_value->is_unsigned())
                return status_only(Status::MissingParameter);
            if (permissions_value->raw_number() == 0)
                return status_only(Status::InvalidParameter);
            if (!uv_configured())
                return status_only(Status::NotAllowed);
            if (m_uv_retries <= 0)
                return status_only(Status::UvBlocked);
            // The fingerprint read stands in for a touch.
            auto const presence = unlocked_presence(cid, lock);
            if (presence == Presence::Cancelled)
                return status_only(Status::KeepaliveCancel);
            if (presence != Presence::Touched)
                return status_only(Status::UserActionTimeout);
            Status const s = built_in_uv();
            if (s != Status::Ok)
                return status_only(s);
            reset_token();
            m_token_in_use = true;
            m_token_user_verified = true;
            m_token_user_present = true;
            m_token_permissions = static_cast<std::uint8_t>(permissions_value->raw_number());
            if (rp_id_value != nullptr && rp_id_value->is_text())
                m_token_rp_id = std::string(rp_id_value->as_text());
            auto const encrypted = auth::pin::encrypt(*protocol, *shared, m_token, platform::fill_random);
            Value response = Value::map();
            response.set(0x02, Value::bytes(*encrypted));
            return ok_with(response);
        }
        }
        return status_only(Status::InvalidSubcommand);
    }

    // ---------- U2F raw messages (FIDO U2F Raw Message Formats v1.2) ----------

    static std::vector<std::uint8_t> sw(std::uint16_t word) { return { static_cast<std::uint8_t>(word >> 8), static_cast<std::uint8_t>(word) }; }

    bool u2f_touched()
    {
        // A U2F key answers "test of user presence required" until the
        // user touches it; the wait starts with the first such answer.
        auto const now = SoftClock::now();
        if (!m_u2f_wait_start) {
            m_u2f_wait_start = now;
            ++m_stats.presence_requests;
        }
        if (touched_after(now - *m_u2f_wait_start)) {
            m_u2f_wait_start.reset();
            ++m_stats.touches;
            return true;
        }
        return false;
    }

    std::vector<std::uint8_t> handle_apdu(std::vector<std::uint8_t> const& apdu)
    {
        // CLA INS P1 P2, then the extended (0 Lc1 Lc2) or short (Lc) form.
        if (apdu.size() < 4)
            return sw(0x6700);
        std::uint8_t const cla = apdu[0];
        std::uint8_t const ins = apdu[1];
        std::uint8_t const p1 = apdu[2];
        std::span<std::uint8_t const> data;
        if (apdu.size() > 4) {
            if (apdu[4] == 0 && apdu.size() >= 7) {
                std::size_t const lc = std::size_t { apdu[5] } << 8 | apdu[6];
                if (apdu.size() < 7 + lc)
                    return sw(0x6700);
                data = std::span<std::uint8_t const>(apdu).subspan(7, lc);
            } else if (apdu[4] != 0) {
                std::size_t const lc = apdu[4];
                if (apdu.size() < 5 + lc)
                    return sw(0x6700);
                data = std::span<std::uint8_t const>(apdu).subspan(5, lc);
            }
        }
        if (cla != 0)
            return sw(0x6e00);
        std::lock_guard lock(m_mutex);
        if (ins == 0x03) { // VERSION
            std::vector<std::uint8_t> out { 'U', '2', 'F', '_', 'V', '2' };
            out.insert(out.end(), { 0x90, 0x00 });
            return out;
        }
        if (ins == 0x01) { // REGISTER
            if (data.size() != 64)
                return sw(0x6700);
            if (!u2f_touched())
                return sw(0x6985);
            Credential credential;
            credential.id.resize(64);
            platform::fill_random(credential.id);
            std::ranges::copy(data.subspan(32, 32), credential.rp_id_hash.begin());
            credential.alg = auth::cose::alg_es256;
            credential.private_key = new_p256_scalar();
            credential.from_u2f = true;
            credential.created = ++m_creation_counter;
            platform::fill_random(credential.cred_random_with_uv);
            platform::fill_random(credential.cred_random_without_uv);
            m_credentials.push_back(credential);
            auto const point = crypto::ec_public_point(crypto::CurveId::P256, credential.private_key);
            auto const public_key = crypto::ec_encode_point(crypto::CurveId::P256, *point);
            std::vector<std::uint8_t> to_sign { 0x00 };
            to_sign.insert(to_sign.end(), data.begin() + 32, data.end()); // application parameter
            to_sign.insert(to_sign.end(), data.begin(), data.begin() + 32); // challenge parameter
            to_sign.insert(to_sign.end(), credential.id.begin(), credential.id.end());
            to_sign.insert(to_sign.end(), public_key.begin(), public_key.end());
            std::vector<std::uint8_t> out { 0x05 };
            out.insert(out.end(), public_key.begin(), public_key.end());
            out.push_back(static_cast<std::uint8_t>(credential.id.size()));
            out.insert(out.end(), credential.id.begin(), credential.id.end());
            out.insert(out.end(), m_attestation_cert.begin(), m_attestation_cert.end());
            auto const signature = sign_with(auth::cose::alg_es256, m_attestation_scalar, to_sign);
            out.insert(out.end(), signature.begin(), signature.end());
            out.insert(out.end(), { 0x90, 0x00 });
            return out;
        }
        if (ins == 0x02) { // AUTHENTICATE
            if (data.size() < 65 || data.size() != std::size_t { 65 } + data[64])
                return sw(0x6700);
            std::array<std::uint8_t, 32> application {};
            std::ranges::copy(data.subspan(32, 32), application.begin());
            auto const handle = data.subspan(65);
            Credential* found = nullptr;
            for (auto& c : m_credentials) {
                if (c.rp_id_hash == application && std::ranges::equal(c.id, handle) && c.alg == auth::cose::alg_es256)
                    found = &c;
            }
            if (found == nullptr)
                return sw(0x6a80);
            if (p1 == 0x07)
                return sw(0x6985); // check-only: the handle is ours
            if (p1 != 0x03 && p1 != 0x08)
                return sw(0x6a80);
            std::uint8_t const presence = p1 == 0x03 ? 1 : 0;
            if (p1 == 0x03 && !u2f_touched())
                return sw(0x6985);
            ++found->sign_count;
            std::vector<std::uint8_t> to_sign(application.begin(), application.end());
            to_sign.push_back(presence);
            for (int shift = 24; shift >= 0; shift -= 8)
                to_sign.push_back(static_cast<std::uint8_t>(found->sign_count >> shift));
            to_sign.insert(to_sign.end(), data.begin(), data.begin() + 32);
            std::vector<std::uint8_t> out { presence };
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(static_cast<std::uint8_t>(found->sign_count >> shift));
            auto const signature = sign_with(auth::cose::alg_es256, found->private_key, to_sign);
            out.insert(out.end(), signature.begin(), signature.end());
            out.insert(out.end(), { 0x90, 0x00 });
            return out;
        }
        return sw(0x6d00);
    }

    // ---------- The test attestation certificate ----------

    void attestation_setup()
    {
        // A fixed key, so that every run's certificate is the same.
        auto const digest = sha256("software authenticator attestation key");
        std::ranges::copy(digest, m_attestation_scalar.begin());
        m_attestation_point = *crypto::ec_public_point(crypto::CurveId::P256, m_attestation_scalar);
        m_attestation_cert = make_certificate(m_attestation_scalar, m_attestation_point, m_options.aaguid);
    }

public:
    // A self-signed X.509 v3 certificate as WebAuthn §8.2.1 asks of a
    // packed attestation certificate: subject C, O, OU "Authenticator
    // Attestation", CN; basic constraints CA false; the AAGUID extension
    // (1.3.6.1.4.1.45724.1.1.4).
    static std::vector<std::uint8_t> make_certificate(std::array<std::uint8_t, 32> const& scalar, crypto::EcPoint const& point,
        std::array<std::uint8_t, 16> const& aaguid)
    {
        namespace der = auth::der;
        auto const attribute = [](std::string_view oid, std::string_view value) {
            return der::set({ der::sequence({ der::oid(oid), der::utf8_string(value) }) });
        };
        auto const country = der::set({ der::sequence({ der::oid("2.5.4.6"),
            der::element(0x13, std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>("US"), 2)) }) });
        auto const name = der::sequence({ country, attribute("2.5.4.10", "Test Vendor"), attribute("2.5.4.11", "Authenticator Attestation"),
            attribute("2.5.4.3", "Software Authenticator") });
        auto const algorithm = der::sequence({ der::oid("1.2.840.10045.4.3.2") });
        auto const spki = der::sequence({ der::sequence({ der::oid("1.2.840.10045.2.1"), der::oid("1.2.840.10045.3.1.7") }),
            der::bit_string(crypto::ec_encode_point(crypto::CurveId::P256, point)) });
        auto const basic_constraints = der::sequence({ der::oid("2.5.29.19"), der::element(0x01, std::vector<std::uint8_t> { 0xff }),
            der::octet_string(der::sequence({})) });
        auto const aaguid_extension = der::sequence({ der::oid("1.3.6.1.4.1.45724.1.1.4"), der::octet_string(der::octet_string(aaguid)) });
        auto const extensions = der::explicit_tag(3, der::sequence({ basic_constraints, aaguid_extension }));
        std::uint8_t const serial[] = { 0x01, 0x23, 0x45, 0x67 };
        auto const tbs = der::sequence({ der::explicit_tag(0, der::small_integer(2)), der::integer(serial), algorithm, name,
            der::sequence({ der::generalized_time("20240101000000Z"), der::generalized_time("20991231235959Z") }), name, spki, extensions });
        auto const digest = crypto::Sha256::hash(tbs);
        auto const rs = crypto::ecdsa_sign(crypto::CurveId::P256, scalar, digest, platform::fill_random);
        return der::sequence({ tbs, algorithm, der::bit_string(der::ecdsa_signature(rs->first, rs->second)) });
    }

private:
    SoftOptions m_options;
    std::shared_ptr<FakeLink> m_link;
    std::thread m_thread;
    std::atomic<bool> m_stop { false };
    SoftStats m_stats;

    // CTAPHID state (the device thread's alone).
    std::set<std::uint32_t> m_channels;
    std::uint32_t m_next_cid = 0x10000000;
    Pending m_pending;
    std::uint32_t m_active_cid = 0;
    std::uint32_t m_lock_cid = 0;
    SoftClock::time_point m_lock_until;
    bool m_aborted = false;

    std::mutex m_touch_mutex;
    TouchPlan m_touch;
    std::atomic<bool> m_uv_succeeds { true };
    std::optional<SoftClock::time_point> m_u2f_wait_start;

    // Authenticator state, under m_mutex.
    std::mutex m_mutex;
    std::vector<Credential> m_credentials;
    std::uint64_t m_creation_counter = 0;
    std::optional<std::array<std::uint8_t, 16>> m_pin_hash;
    int m_pin_retries = 8;
    int m_consecutive_mismatches = 0;
    bool m_power_cycle_needed = false;
    bool m_force_pin_change = false;
    int m_uv_retries = 8;
    auth::pin::KeyAgreementKey m_key_agreement;
    std::array<std::uint8_t, 32> m_token {};
    bool m_token_in_use = false;
    std::uint8_t m_token_permissions = 0;
    std::optional<std::string> m_token_rp_id;
    bool m_token_user_present = false;
    bool m_token_user_verified = false;
    std::optional<AssertionState> m_assertion_state;
    std::array<std::uint8_t, 32> m_attestation_scalar {};
    crypto::EcPoint m_attestation_point;
    std::vector<std::uint8_t> m_attestation_cert;
};

// Several soft keys behind one auth::Transport: plugged and unplugged at
// will, each a DeviceInfo whose path is "soft:N".
class FakeTransport final : public auth::Transport {
public:
    std::shared_ptr<SoftAuthenticator> plug(SoftOptions options = {})
    {
        auto key = std::make_shared<SoftAuthenticator>(std::move(options));
        std::lock_guard lock(m_mutex);
        m_keys.push_back({ "soft:" + std::to_string(++m_counter), key });
        ++m_generation;
        m_changed.notify_all();
        return key;
    }
    void unplug(std::shared_ptr<SoftAuthenticator> const& key)
    {
        key->unplug();
        std::lock_guard lock(m_mutex);
        std::erase_if(m_keys, [&](auto const& entry) { return entry.second == key; });
        ++m_generation;
        m_changed.notify_all();
    }
    std::vector<auth::DeviceInfo> devices() override
    {
        std::lock_guard lock(m_mutex);
        std::vector<auth::DeviceInfo> out;
        for (auto const& [path, key] : m_keys) {
            auth::DeviceInfo info;
            info.path = path;
            info.product = "Software Authenticator";
            info.input_report_size = key->options().report_size;
            info.output_report_size = key->options().report_size;
            out.push_back(info);
        }
        return out;
    }
    std::unique_ptr<auth::HidConnection> open(auth::DeviceInfo const& device) override
    {
        std::lock_guard lock(m_mutex);
        for (auto const& [path, key] : m_keys) {
            if (path == device.path)
                return key->connect();
        }
        return nullptr;
    }
    bool wait_for_change(std::chrono::milliseconds timeout) override
    {
        std::unique_lock lock(m_mutex);
        std::uint64_t const seen = m_seen_generation;
        bool const changed = m_changed.wait_for(lock, timeout, [&] { return m_generation != seen; });
        m_seen_generation = m_generation;
        return changed;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::pair<std::string, std::shared_ptr<SoftAuthenticator>>> m_keys;
    int m_counter = 0;
    std::uint64_t m_generation = 0;
    std::uint64_t m_seen_generation = 0;
};

}
