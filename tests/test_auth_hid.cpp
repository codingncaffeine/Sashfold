// CTAPHID (CTAP 2.1 §11.2) from the client's side, against the software
// authenticator on a fake HID link: framing at its limits, channel
// allocation, busy channels and LOCK, another application's traffic on the
// same device, KEEPALIVE statuses, CANCEL, timeouts and a key pulled out
// mid-message.
#include "AuthTestSupport.h"
#include "SoftAuthenticator.h"
#include "Test.h"

#include "auth/CtapHid.h"

#include <chrono>
#include <thread>

using namespace sashfold;
using namespace sashfold::auth;
using sashfold::test::SoftAuthenticator;
using sashfold::test::SoftOptions;
using sashfold::test::TouchPlan;
using Clock = std::chrono::steady_clock;

namespace {

auto in(int ms) { return Clock::now() + std::chrono::milliseconds(ms); }

DeviceInfo info_for(SoftAuthenticator const& key)
{
    DeviceInfo info;
    info.path = "soft";
    info.input_report_size = key.options().report_size;
    info.output_report_size = key.options().report_size;
    return info;
}

void framing()
{
    // §11.2.4: 57 bytes in the first packet, 59 in each continuation, 7609 at most.
    CHECK_EQ(ctaphid::max_payload(64), std::size_t { 7609 });
    std::vector<std::uint8_t> payload(7609);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(i * 7);
    auto const packets = ctaphid::fragment(0x01020304, ctaphid::cmd_cbor, payload, 64);
    CHECK_EQ(packets.size(), std::size_t { 129 });
    CHECK_EQ(int(packets[0][4]), 0x90);
    CHECK_EQ(int(packets[0][5]), 0x1d);
    CHECK_EQ(int(packets[0][6]), 0xb9);
    CHECK_EQ(int(packets[1][4]), 0);
    CHECK_EQ(int(packets[128][4]), 127);
    CHECK(ctaphid::fragment(1, 1, std::vector<std::uint8_t>(7610), 64).empty());
    ctaphid::Assembler assembler(64);
    ctaphid::Assembler::Step step = ctaphid::Assembler::Step::NeedMore;
    for (auto const& p : packets)
        step = assembler.take(p);
    CHECK(step == ctaphid::Assembler::Step::Done);
    CHECK(assembler.payload() == payload);
    CHECK_EQ(int(assembler.command()), int(ctaphid::cmd_cbor));
    // Out of order, a continuation with nothing started, a length too long.
    assembler.reset();
    CHECK(assembler.take(packets[0]) == ctaphid::Assembler::Step::NeedMore);
    CHECK(assembler.take(packets[2]) == ctaphid::Assembler::Step::Malformed);
    CHECK(assembler.take(packets[1]) == ctaphid::Assembler::Step::Spurious);
    std::vector<std::uint8_t> long_init(64, 0);
    long_init[4] = 0x90;
    long_init[5] = 0x1d;
    long_init[6] = 0xba; // 7610
    CHECK(assembler.take(long_init) == ctaphid::Assembler::Step::Malformed);
    CHECK(assembler.take(std::vector<std::uint8_t>(10, 0x80)) == ctaphid::Assembler::Step::Malformed);
    // An empty message is one packet.
    auto const empty = ctaphid::fragment(5, ctaphid::cmd_wink, {}, 64);
    CHECK_EQ(empty.size(), std::size_t { 1 });
    assembler.reset();
    CHECK(assembler.take(empty[0]) == ctaphid::Assembler::Step::Done);
}

void channel_and_commands()
{
    SoftAuthenticator key;
    CtapHidDevice device(key.connect(), info_for(key));
    CHECK(device.init(in(1000)));
    CHECK(device.channel() != ctaphid::broadcast_cid);
    CHECK_EQ(int(device.protocol_version()), 2);
    CHECK(device.has_cbor());
    CHECK(device.has_msg());
    CHECK(device.has_wink());
    // PING of every size class: one packet, a few, the most there is.
    for (std::size_t size : { std::size_t { 0 }, std::size_t { 57 }, std::size_t { 58 }, std::size_t { 116 }, std::size_t { 1000 },
             std::size_t { 7609 } }) {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i)
            data[i] = static_cast<std::uint8_t>(i ^ size);
        CHECK(device.ping(data, in(3000)));
    }
    CHECK(device.wink(in(1000)));
    CHECK_EQ(key.stats().winks.load(), 1);

    // A second application on the same device gets its own channel.
    CtapHidDevice other(key.connect(), info_for(key));
    CHECK(other.init(in(1000)));
    CHECK(other.channel() != device.channel());
    // Its traffic reaches our connection too (hidraw hands every report to
    // every reader) and is skipped: interleave its pings with ours.
    std::thread noise([&] {
        for (int i = 0; i < 20; ++i)
            other.ping(std::vector<std::uint8_t>(100, 0xaa), in(2000));
    });
    for (int i = 0; i < 20; ++i)
        CHECK(device.ping(std::vector<std::uint8_t>(150, 0x55), in(4000)));
    noise.join();

    // LOCK: the other channel is told the device is busy until it ends.
    // (Held for ten seconds and let go by hand, so that a slow machine
    // cannot run the lock out before the other channel has asked.)
    CHECK(device.lock(10, in(1000)));
    auto const busy = other.transact(ctaphid::cmd_ping, std::vector<std::uint8_t> { 1 }, in(300));
    CHECK(busy.failure == HidFailure::Busy);
    CHECK(key.stats().busy_errors.load() > 0);
    CHECK(device.lock(0, in(1000)));
    // Whatever was still on its way to the other channel when it gave up
    // is not taken for the next answer.
    CHECK(other.ping(std::vector<std::uint8_t> { 1, 2, 3 }, in(2000)));
    CHECK(other.ping(std::vector<std::uint8_t> { 4, 5, 6, 7 }, in(2000)));
    // A device that is busy for a while: the client asks again and gets through.
    CHECK(device.lock(1, in(1000)));
    auto const start = Clock::now();
    CHECK(other.ping(std::vector<std::uint8_t> { 9 }, in(3000)));
    CHECK(Clock::now() - start >= std::chrono::milliseconds(800));
    // An unknown command is the device's ERR_INVALID_CMD.
    auto const unknown = device.transact(0x55, {}, in(1000));
    CHECK(unknown.failure == HidFailure::DeviceError && unknown.error_code == ctaphid::err_invalid_cmd);
}

void keepalive_cancel_timeout()
{
    // A touch after 350 ms: KEEPALIVE "up needed" at least three times.
    SoftOptions options;
    SoftAuthenticator key(options);
    key.set_touch(TouchPlan::after(350));
    CtapHidDevice device(key.connect(), info_for(key));
    CHECK(device.init(in(1000)));
    int up_needed = 0;
    std::uint8_t const selection[] = { ctap2::cmd_selection };
    auto const touched = device.transact(ctaphid::cmd_cbor, selection, in(3000), nullptr, [&](std::uint8_t status) {
        up_needed += status == ctaphid::status_up_needed ? 1 : 0;
    });
    CHECK(touched.ok());
    CHECK(touched.payload == std::vector<std::uint8_t> { 0x00 });
    CHECK(up_needed >= 3);

    // Cancelled while it waits: CTAPHID_CANCEL, then the device's own
    // CTAP2_ERR_KEEPALIVE_CANCEL as the answer.
    key.set_touch(TouchPlan::never());
    CancelFlag cancel { false };
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel = true;
    });
    auto const started = Clock::now();
    auto const cancelled = device.transact(ctaphid::cmd_cbor, selection, in(10000), &cancel);
    canceller.join();
    CHECK(cancelled.ok());
    CHECK(cancelled.payload == std::vector<std::uint8_t> { static_cast<std::uint8_t>(ctap2::Status::KeepaliveCancel) });
    CHECK(Clock::now() - started < std::chrono::milliseconds(1500));
    CHECK_EQ(key.stats().cancels_honored.load(), 1);

    // Nobody touches and nobody cancels: the client's deadline ends it.
    auto const timed = device.transact(ctaphid::cmd_cbor, selection, in(300));
    CHECK(timed.failure == HidFailure::Timeout);
    // The device is still waiting for the touch from that request, and
    // answers it when the touch comes: an answer to nothing the client
    // waits for any more. CTAPHID gives an answer no mark of its request,
    // so the next transaction would take it for its own; the client brings
    // the channel back in step first, and the ping gets the ping's answer
    // whether the late one had arrived by then or not.
    key.set_touch(TouchPlan::after(0));
    CHECK(device.ping(std::vector<std::uint8_t> { 7, 7, 7 }, in(3000)));
    CHECK(device.ping(std::vector<std::uint8_t> { 8 }, in(3000)));
    key.set_touch(TouchPlan::never());
    // A new channel is there for the asking as well.
    CHECK(device.init(in(1000)));

    // A slow key: "processing" keepalives come first.
    SoftOptions slow;
    slow.processing_ms = 300;
    SoftAuthenticator slow_key(slow);
    CtapHidDevice slow_device(slow_key.connect(), info_for(slow_key));
    CHECK(slow_device.init(in(1000)));
    int processing = 0;
    std::uint8_t const get_info[] = { ctap2::cmd_get_info };
    auto const info = slow_device.transact(ctaphid::cmd_cbor, get_info, in(3000), nullptr, [&](std::uint8_t status) {
        processing += status == ctaphid::status_processing ? 1 : 0;
    });
    CHECK(info.ok());
    CHECK(processing >= 2);
}

void unplugged_and_silent()
{
    // Pulled out while it waits for a touch.
    {
        SoftAuthenticator key;
        key.set_touch(TouchPlan::never());
        CtapHidDevice device(key.connect(), info_for(key));
        CHECK(device.init(in(1000)));
        std::thread puller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            key.unplug();
        });
        std::uint8_t const selection[] = { ctap2::cmd_selection };
        auto const result = device.transact(ctaphid::cmd_cbor, selection, in(5000));
        puller.join();
        CHECK(result.failure == HidFailure::Gone);
        CHECK(device.gone());
        CHECK(!device.ping(std::vector<std::uint8_t> { 1 }, in(100)));
    }
    // Pulled out halfway through a long answer: a fake link that delivers
    // the first packet of a PING echo and then goes away.
    {
        auto link = std::make_shared<test::FakeLink>(64);
        DeviceInfo info;
        info.path = "half";
        CtapHidDevice device(link->connect(), info);
        std::thread fake_device([&] {
            auto init = link->device_read(std::chrono::milliseconds(1000));
            if (!init)
                return;
            std::vector<std::uint8_t> answer(64, 0);
            std::fill_n(answer.begin(), 4, 0xff);
            answer[4] = 0x86;
            answer[6] = 17;
            std::copy_n(init->begin() + 7, 8, answer.begin() + 7);
            answer[15] = 0;
            answer[16] = 0;
            answer[17] = 0;
            answer[18] = 7;
            answer[19] = 2;
            link->device_write(answer);
            auto ping = link->device_read(std::chrono::milliseconds(1000));
            while (link->device_read(std::chrono::milliseconds(50))) {
            }
            std::vector<std::uint8_t> first(64, 0);
            first[3] = 7;
            first[4] = 0x81;
            first[5] = 0x01;
            first[6] = 0x00; // 256 bytes promised
            link->device_write(first);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            link->unplug();
        });
        CHECK(device.init(in(1000)));
        CHECK_EQ(device.channel(), std::uint32_t { 7 });
        auto const result = device.transact(ctaphid::cmd_ping, std::vector<std::uint8_t>(256, 1), in(3000));
        fake_device.join();
        CHECK(result.failure == HidFailure::Gone);
    }
    // A device that never answers INIT.
    {
        auto link = std::make_shared<test::FakeLink>(64);
        DeviceInfo info;
        CtapHidDevice device(link->connect(), info);
        auto const start = Clock::now();
        CHECK(!device.init(in(200)));
        CHECK(Clock::now() - start >= std::chrono::milliseconds(190));
    }
    // An answer of the wrong command on our channel is malformed; an
    // INIT answer with another nonce is someone else's and skipped.
    {
        auto link = std::make_shared<test::FakeLink>(64);
        DeviceInfo info;
        CtapHidDevice device(link->connect(), info);
        std::thread fake_device([&] {
            auto init = link->device_read(std::chrono::milliseconds(1000));
            if (!init)
                return;
            std::vector<std::uint8_t> answer(64, 0);
            std::fill_n(answer.begin(), 4, 0xff);
            answer[4] = 0x86;
            answer[6] = 17;
            // First another application's nonce, then ours.
            std::fill_n(answer.begin() + 7, 8, 0xee);
            answer[18] = 9;
            link->device_write(answer);
            std::copy_n(init->begin() + 7, 8, answer.begin() + 7);
            answer[18] = 8;
            link->device_write(answer);
            auto ping = link->device_read(std::chrono::milliseconds(1000));
            std::vector<std::uint8_t> wrong(64, 0);
            wrong[3] = 8;
            wrong[4] = 0x88; // WINK, not PING
            link->device_write(wrong);
        });
        CHECK(device.init(in(1000)));
        CHECK_EQ(device.channel(), std::uint32_t { 8 });
        auto const result = device.transact(ctaphid::cmd_ping, std::vector<std::uint8_t> { 1 }, in(1000));
        fake_device.join();
        CHECK(result.failure == HidFailure::Malformed);
    }
}

}

int main()
{
    framing();
    channel_and_commands();
    keepalive_cancel_timeout();
    unplugged_and_silent();
    return sashfold::test::report("test_auth_hid");
}
