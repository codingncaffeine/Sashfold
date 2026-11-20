// Hardening: every parser of the security-key stack fed truncated,
// bit-flipped, byte-replaced and spliced versions of real inputs, in a
// loop, and the whole client fed a device that answers with garbage on its
// channel. Nothing may crash, hang or read out of bounds (the sanitizer
// builds run this too). Deterministic: the same inputs every run.
#include "AuthCeremony.h"
#include "AuthTestSupport.h"
#include "SoftAuthenticator.h"
#include "Test.h"
#include "WebAuthnVectors.h"

#include "auth/Attestation.h"
#include "auth/Cbor.h"
#include "auth/Cose.h"
#include "auth/CredentialManagement.h"
#include "auth/Ctap2.h"
#include "auth/CtapHid.h"
#include "auth/HidDescriptor.h"
#include "auth/KeyManagement.h"
#include "auth/U2f.h"

#include <thread>

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::from_hex;
using sashfold::test::TestRandom;

namespace {

TestRandom g_random;

std::vector<std::uint8_t> mutate(std::vector<std::uint8_t> input)
{
    switch (g_random.below(6)) {
    case 0: // truncate
        input.resize(g_random.below(input.size() + 1));
        break;
    case 1: // flip bits
        for (std::size_t i = 0, n = 1 + g_random.below(4); i < n && !input.empty(); ++i)
            input[g_random.below(input.size())] ^= static_cast<std::uint8_t>(1u << g_random.below(8));
        break;
    case 2: // replace a byte with a boundary value
        if (!input.empty()) {
            std::uint8_t const values[] = { 0x00, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1f, 0x7f, 0x80, 0x9f, 0xbf, 0xff, 0x5b, 0x7b, 0x9b, 0xbb };
            input[g_random.below(input.size())] = values[g_random.below(sizeof values)];
        }
        break;
    case 3: // duplicate a slice
        if (!input.empty()) {
            std::size_t const at = g_random.below(input.size());
            std::size_t const length = g_random.below(input.size() - at) + 1;
            std::vector<std::uint8_t> slice(input.begin() + static_cast<std::ptrdiff_t>(at), input.begin() + static_cast<std::ptrdiff_t>(at + length));
            input.insert(input.begin() + static_cast<std::ptrdiff_t>(g_random.below(input.size())), slice.begin(), slice.end());
        }
        break;
    case 4: // remove a slice
        if (!input.empty()) {
            std::size_t const at = g_random.below(input.size());
            std::size_t const length = g_random.below(input.size() - at) + 1;
            input.erase(input.begin() + static_cast<std::ptrdiff_t>(at), input.begin() + static_cast<std::ptrdiff_t>(at + length));
        }
        break;
    default: // random tail
        for (std::size_t i = 0, n = g_random.below(16); i < n; ++i)
            input.push_back(static_cast<std::uint8_t>(g_random.next()));
        break;
    }
    return input;
}

// Everything that reads bytes from a device or a page, on one input.
std::size_t parse_everything(std::vector<std::uint8_t> const& bytes)
{
    std::size_t accepted = 0;
    for (auto const mode : { cbor::Mode::Ctap2, cbor::Mode::General }) {
        if (auto const value = cbor::decode(bytes, mode)) {
            ++accepted;
            // Whatever decodes must encode and, canonically written, decode again.
            auto const again = cbor::decode(cbor::encode(*value), cbor::Mode::General);
            CHECK(again.has_value());
            cose_key_from_cbor(*value);
            parse_authenticator_info(*value);
            parse_make_credential_response(*value);
            parse_get_assertion_response(*value);
            parse_client_pin_response(*value);
            cred_mgmt::parse_response(*value);
            descriptor_from_cbor(*value);
            user_from_cbor(*value);
            if (auto const key = cose_key_from_cbor(*value)) {
                cose_key_to_spki(*key);
                cose_verify(*key, cose::alg_es256, bytes, bytes);
                cose_verify(*key, cose::alg_eddsa, bytes, std::vector<std::uint8_t>(64));
            }
        }
        cbor::decode_prefix(bytes, mode);
    }
    if (auto const object = parse_attestation_object(bytes)) {
        ++accepted;
        std::array<std::uint8_t, 32> hash {};
        verify_attestation_statement(*object, hash);
        apply_conveyance(*object, AttestationConveyance::None);
        created_credential_fields(*object, { "usb" });
    }
    if (parse_authenticator_data(bytes))
        ++accepted;
    u2f::parse_registration(bytes);
    u2f::parse_authentication(bytes);
    u2f::split_answer(bytes);
    parse_fido_report_descriptor(bytes);
    der::parse_ecdsa_signature(bytes);
    return accepted;
}

void parsers()
{
    std::vector<std::vector<std::uint8_t>> seeds;
    for (auto const& v : webauthn_vectors) {
        seeds.push_back(from_hex(v.reg_attestationObject));
        seeds.push_back(from_hex(v.auth_authenticatorData));
        seeds.push_back(from_hex(v.auth_signature));
    }
    // A getInfo answer and a getAssertion answer from the software key.
    {
        test::SoftOptions options;
        options.pin = "1234";
        test::SoftAuthenticator key(options);
        DeviceInfo device;
        CtapHidDevice hid(key.connect(), device);
        if (hid.init(std::chrono::steady_clock::now() + std::chrono::seconds(1))) {
            std::uint8_t const get_info[] = { ctap2::cmd_get_info };
            auto const info = hid.transact(ctaphid::cmd_cbor, get_info, std::chrono::steady_clock::now() + std::chrono::seconds(1));
            if (info.ok() && info.payload.size() > 1)
                seeds.emplace_back(info.payload.begin() + 1, info.payload.end());
        }
    }
    seeds.push_back(from_hex("a5010203262001215820e87625896ee4e46dc032766e8087962f36df9dfe8b567f3763015b1990a60e1422582027de612d66418bda1950581ebc5c8c1dad710cb14c22f8c97045f4612fb20c91"));
    seeds.push_back(from_hex("06d0f10901a1010920150026ff00750895408102092115002 6ff0075089540910 2c0"));
    seeds.push_back(from_hex("bf6346756ef563416d7421ff"));
    seeds.push_back(from_hex("9f018202039f0405ffff"));
    seeds.push_back(from_hex("c249010000000000000000"));

    std::size_t runs = 0;
    std::size_t accepted = 0;
    for (auto const& seed : seeds) {
        accepted += parse_everything(seed) > 0 ? 1 : 0;
        // Every truncation.
        for (std::size_t n = 0; n < seed.size(); n += 1 + seed.size() / 200) {
            parse_everything(std::vector<std::uint8_t>(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(n)));
            ++runs;
        }
        // Mutations, compounding.
        auto current = seed;
        for (int i = 0; i < 1500; ++i) {
            current = (i % 50 == 0) ? seed : mutate(current);
            if (current.size() > 20000)
                current = seed;
            accepted += parse_everything(current) > 0 ? 1 : 0;
            ++runs;
        }
    }
    // Pure noise of every short length.
    for (int i = 0; i < 20000; ++i) {
        std::vector<std::uint8_t> noise(g_random.below(40));
        for (auto& b : noise)
            b = static_cast<std::uint8_t>(g_random.next());
        parse_everything(noise);
        ++runs;
    }
    std::cout << "parsers: " << runs << " inputs, " << accepted << " accepted by at least one parser\n";
    CHECK(runs > 30000);
    CHECK(accepted > 0);
}

void hid_packets()
{
    // The assembler on noise and on mutated real packet streams.
    std::size_t runs = 0;
    for (std::size_t size : { std::size_t { 8 }, std::size_t { 64 } }) {
        ctaphid::Assembler assembler(size);
        std::vector<std::uint8_t> payload(std::min<std::size_t>(1000, ctaphid::max_payload(size)));
        for (auto& b : payload)
            b = static_cast<std::uint8_t>(g_random.next());
        auto const packets = ctaphid::fragment(7, ctaphid::cmd_cbor, payload, size);
        for (int i = 0; i < 20000; ++i) {
            std::vector<std::uint8_t> packet = packets[g_random.below(packets.size())];
            if (g_random.below(2) == 0)
                packet = mutate(packet);
            assembler.take(packet);
            if (assembler.in_progress())
                CHECK(assembler.payload().size() <= ctaphid::max_payload(size));
            ++runs;
        }
    }
    CHECK(runs == 40000);
}

// A device that allocates a channel properly and then answers everything
// on it with random packets: the client must fail cleanly, not crash or hang.
void hostile_device()
{
    for (int round = 0; round < 6; ++round) {
        auto link = std::make_shared<test::FakeLink>(64);
        std::atomic<bool> stop { false };
        std::thread device([&, round] {
            TestRandom random;
            random.state += static_cast<std::uint64_t>(round) * 7919;
            while (!stop) {
                auto const request = link->device_read(std::chrono::milliseconds(20));
                if (!request)
                    continue;
                std::uint32_t const cid = ctaphid::packet_cid(*request);
                if (cid == ctaphid::broadcast_cid && ((*request)[4] & 0x7f) == ctaphid::cmd_init) {
                    std::vector<std::uint8_t> answer(64, 0);
                    std::fill_n(answer.begin(), 4, 0xff);
                    answer[4] = 0x86;
                    answer[6] = 17;
                    std::copy_n(request->begin() + 7, 8, answer.begin() + 7);
                    answer[18] = 0x42;
                    answer[19] = 2;
                    answer[23] = ctaphid::capability_cbor | ctaphid::capability_wink;
                    link->device_write(answer);
                    continue;
                }
                for (std::size_t i = 0, n = 1 + random.below(6); i < n; ++i) {
                    std::vector<std::uint8_t> packet(64);
                    for (auto& b : packet)
                        b = static_cast<std::uint8_t>(random.next());
                    packet[0] = 0;
                    packet[1] = 0;
                    packet[2] = 0;
                    packet[3] = 0x42;
                    if (random.below(2) == 0) {
                        // A plausible CBOR answer with a garbage body.
                        packet[4] = 0x90;
                        packet[5] = 0;
                        packet[6] = static_cast<std::uint8_t>(1 + random.below(56));
                        packet[7] = static_cast<std::uint8_t>(random.below(3) == 0 ? 0 : random.next());
                    }
                    link->device_write(packet);
                }
            }
        });
        class OneDevice final : public Transport {
        public:
            explicit OneDevice(std::shared_ptr<test::FakeLink> link)
                : m_link(std::move(link))
            {
            }
            std::vector<DeviceInfo> devices() override
            {
                DeviceInfo info;
                info.path = "hostile";
                return { info };
            }
            std::unique_ptr<HidConnection> open(DeviceInfo const&) override { return m_link->connect(); }
            bool wait_for_change(std::chrono::milliseconds timeout) override
            {
                std::this_thread::sleep_for(timeout);
                return false;
            }

        private:
            std::shared_ptr<test::FakeLink> m_link;
        };
        auto request = round % 2 == 0 ? test::create_request() : test::get_request({ { 1, 2, 3 } });
        request.timeout = std::chrono::milliseconds(800);
        auto const start = std::chrono::steady_clock::now();
        auto const outcome = test::run_ceremony(std::make_shared<OneDevice>(link), request, std::make_shared<test::ScriptedPrompt>());
        CHECK(outcome.error.has_value());
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
        // The key settings on the same device (auth/KeyManagement.h): its
        // garbage may even pass for a touch; nothing may crash, and a
        // cancel ends the session.
        {
            KeyManagement management(std::make_shared<OneDevice>(link), {});
            management.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            management.unlock(std::string("1234"));
            management.refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            management.cancel();
            auto const cancelled_at = std::chrono::steady_clock::now();
            while (!management.finished() && std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(60))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            CHECK(management.finished());
        }
        stop = true;
        link->unplug();
        device.join();
    }
}

}

int main()
{
    parsers();
    hid_packets();
    hostile_device();
    return sashfold::test::report("test_auth_fuzz");
}
