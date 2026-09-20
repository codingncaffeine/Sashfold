#include "Test.h"

#include "media/Wav.h"
#include "platform/Audio.h"
#include "platform/PulseProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#if defined(__linux__)
#    include <chrono>
#    include <filesystem>
#    include <poll.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <thread>
#    include <unistd.h>
#endif

// Sound: the WAVE reader at every depth, the sound server's wire format
// written and read back, and — where there is a socket to speak it over —
// the whole client against a server of the test's own, which checks what it
// is told and answers as a real one does.

using namespace sashfold;
using media::decode_wav;
using media::Sound;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(Bytes& out, std::uint32_t value)
{
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

void put_text(Bytes& out, char const* text)
{
    while (*text != '\0')
        out.push_back(static_cast<std::uint8_t>(*text++));
}

// A WAVE file of the given format around the sample bytes given.
Bytes wav_file(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits, Bytes const& data,
    bool extensible = false)
{
    Bytes fmt;
    put16(fmt, extensible ? 0xFFFE : format);
    put16(fmt, channels);
    put32(fmt, rate);
    put32(fmt, rate * channels * bits / 8u); // bytes a second
    put16(fmt, static_cast<std::uint16_t>(channels * bits / 8u)); // a frame's size
    put16(fmt, bits);
    if (extensible) {
        put16(fmt, 22); // the size of what follows
        put16(fmt, bits); // the bits that count
        put32(fmt, 3); // the channels' places
        put16(fmt, format); // the format, first of the sixteen-byte kind
        for (int i = 0; i < 14; ++i)
            fmt.push_back(0);
    }
    Bytes out;
    put_text(out, "RIFF");
    put32(out, static_cast<std::uint32_t>(4 + 8 + fmt.size() + 8 + data.size()));
    put_text(out, "WAVE");
    put_text(out, "fmt ");
    put32(out, static_cast<std::uint32_t>(fmt.size()));
    out.insert(out.end(), fmt.begin(), fmt.end());
    put_text(out, "data");
    put32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

void test_wav()
{
    // Sixteen bits: silence, the loudest either way, and a half.
    Bytes data;
    for (std::int16_t const value : { std::int16_t(0), std::int16_t(-32768), std::int16_t(32767), std::int16_t(16384) })
        put16(data, static_cast<std::uint16_t>(value));
    std::optional<Sound> const sound = decode_wav(wav_file(1, 2, 44100, 16, data));
    CHECK(sound.has_value());
    if (sound) {
        CHECK_EQ(sound->rate, 44100u);
        CHECK_EQ(sound->channels, 2u);
        CHECK_EQ(sound->frames(), std::size_t { 2 });
        CHECK_EQ(sound->samples[0], 0.0f);
        CHECK_EQ(sound->samples[1], -1.0f);
        CHECK(std::abs(sound->samples[2] - 0.99997f) < 0.0001f);
        CHECK_EQ(sound->samples[3], 0.5f);
        CHECK(std::abs(sound->seconds() - 2.0 / 44100.0) < 1e-9);
    }
    // Eight bits are unsigned, with silence in the middle of the range.
    std::optional<Sound> const bytes = decode_wav(wav_file(1, 1, 8000, 8, Bytes { 128, 0, 255, 192 }));
    CHECK(bytes.has_value());
    if (bytes) {
        CHECK_EQ(bytes->frames(), std::size_t { 4 });
        CHECK_EQ(bytes->samples[0], 0.0f);
        CHECK_EQ(bytes->samples[1], -1.0f);
        CHECK_EQ(bytes->samples[3], 0.5f);
    }
    // Twenty-four bits, the sign carried in the top byte.
    Bytes three { 0, 0, 0, 0, 0, 0x80, 0xFF, 0xFF, 0x7F };
    std::optional<Sound> const deep = decode_wav(wav_file(1, 1, 48000, 24, three));
    CHECK(deep.has_value());
    if (deep) {
        CHECK_EQ(deep->frames(), std::size_t { 3 });
        CHECK_EQ(deep->samples[0], 0.0f);
        CHECK_EQ(deep->samples[1], -1.0f);
        CHECK(deep->samples[2] > 0.999f && deep->samples[2] < 1.0f);
    }
    // Floats arrive as they are, and the extensible header hides the same.
    Bytes floats;
    for (float const value : { 0.0f, -0.25f, 1.0f }) {
        std::uint32_t whole = 0;
        std::memcpy(&whole, &value, sizeof whole);
        put32(floats, whole);
    }
    for (bool const extensible : { false, true }) {
        std::optional<Sound> const read = decode_wav(wav_file(3, 1, 48000, 32, floats, extensible));
        CHECK(read.has_value());
        if (read) {
            CHECK_EQ(read->frames(), std::size_t { 3 });
            CHECK_EQ(read->samples[1], -0.25f);
            CHECK_EQ(read->samples[2], 1.0f);
        }
    }
    // A chunk of another kind before the format is passed over, and a file
    // whose data chunk claims more than arrived plays what did arrive.
    Bytes with_junk = wav_file(1, 1, 8000, 16, Bytes { 0, 0, 0, 0x40 });
    Bytes junk;
    put_text(junk, "LIST");
    put32(junk, 5);
    put_text(junk, "INFOx");
    junk.push_back(0); // chunks sit at even offsets
    with_junk.insert(with_junk.begin() + 12, junk.begin(), junk.end());
    std::optional<Sound> const past_junk = decode_wav(with_junk);
    CHECK(past_junk.has_value());
    if (past_junk)
        CHECK_EQ(past_junk->frames(), std::size_t { 2 });
    Bytes cut = wav_file(1, 1, 8000, 16, Bytes { 0, 0, 0, 0x40 });
    cut.resize(cut.size() - 2);
    std::optional<Sound> const short_file = decode_wav(cut);
    CHECK(short_file.has_value());
    if (short_file)
        CHECK_EQ(short_file->frames(), std::size_t { 1 });
    // What is not a WAVE, has no data, or is of a kind nothing reads.
    CHECK(!media::looks_like_wav(Bytes { 'R', 'I', 'F', 'F' }));
    CHECK(!decode_wav(Bytes { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' }).has_value());
    CHECK(!decode_wav(wav_file(1, 1, 8000, 16, {})).has_value());
    CHECK(!decode_wav(wav_file(2, 1, 8000, 16, Bytes { 0, 0 })).has_value()); // compressed
    CHECK(!decode_wav(wav_file(1, 1, 8000, 12, Bytes { 0, 0 })).has_value()); // a width nothing writes
    CHECK(!decode_wav(wav_file(1, 0, 8000, 16, Bytes { 0, 0 })).has_value()); // no channels
    CHECK(!decode_wav(wav_file(1, 1, 8000, 16, Bytes { 0, 0, 0, 0 }), 1).has_value()); // longer than allowed
}

// --- the sound server's wire format ---------------------------------------------------------

using namespace sashfold::platform::pulse;

std::string hex(std::span<std::uint8_t const> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t const byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

void test_protocol_bytes()
{
    // The framing, byte for byte: a twenty-byte header of the payload's
    // length, the channel that marks a command, a zero offset and no flags,
    // and then the command and its serial as tagged numbers.
    Writer auth(Command::Auth, 0);
    Bytes const cookie { 0xAB, 0xCD };
    auth.u32(protocol_version).arbitrary(cookie);
    CHECK_EQ(hex(auth.packet()),
        std::string("00000016" "ffffffff" "00000000" "00000000" "00000000"
                    "4c00000008" "4c00000000" "4c00000023" "7800000002" "abcd"));

    // A block of samples carries the stream's number where a command packet
    // carries all ones.
    Bytes const samples { 1, 2, 3, 4 };
    CHECK_EQ(hex(block_packet(7, samples)),
        std::string("00000004" "00000007" "00000000" "00000000" "00000000" "01020304"));

    // Every kind of value, written and read back.
    Writer every(Command::SetClientName, 42);
    std::vector<std::uint8_t> const map { 1, 2 };
    std::vector<std::uint32_t> const volumes { 0x10000, 0 };
    std::vector<std::pair<std::string, std::string>> const properties { { "application.name", "Sashfold" }, { "media.role", "video" } };
    every.u8(9).u64(0x1122334455667788ull).boolean(true).boolean(false).string("hello").null_string().sample_spec(SampleFormat::Float32Le, 2, 48000).channel_map(map).cvolume(volumes).timeval(5, 6).proplist(properties).u32(11);
    Reader reader(every.payload());
    CHECK_EQ(reader.u32().value_or(0), static_cast<std::uint32_t>(Command::SetClientName));
    CHECK_EQ(reader.u32().value_or(0), 42u);
    CHECK_EQ(reader.u8().value_or(0), std::uint8_t { 9 });
    CHECK_EQ(reader.u64().value_or(0), 0x1122334455667788ull);
    CHECK_EQ(reader.boolean().value_or(false), true);
    CHECK_EQ(reader.boolean().value_or(true), false);
    CHECK_EQ(reader.string().value_or(""), std::string("hello"));
    CHECK(!reader.string().has_value()); // the null string reads as nothing
    CHECK(reader.ok());
    for (int i = 0; i < 5; ++i)
        CHECK(reader.skip()); // the spec, the map, the volumes, the time, the properties
    CHECK_EQ(reader.u32().value_or(0), 11u);
    CHECK_EQ(reader.remaining(), std::size_t { 0 });
    CHECK(reader.ok());

    // A value read as the wrong kind fails the reader, and it stays failed.
    Reader wrong(every.payload());
    CHECK(!wrong.u8().has_value());
    CHECK(!wrong.ok());
    CHECK(!wrong.u32().has_value());
    // A payload cut short cannot be walked off the end of.
    std::vector<std::uint8_t> const whole(every.payload().begin(), every.payload().end());
    for (std::size_t length = 0; length < whole.size(); ++length) {
        Reader cut(std::span<std::uint8_t const>(whole.data(), length));
        while (cut.skip()) { }
        CHECK(!cut.ok() || cut.remaining() == 0);
    }
    // A header is read whole or not at all.
    CHECK(!read_header(std::span<std::uint8_t const>(whole.data(), 19)).has_value());
    std::optional<Header> const header = read_header(auth.packet());
    CHECK(header.has_value());
    if (header) {
        CHECK_EQ(header->length, 0x16u); // three numbers of five bytes, and a two-byte secret in seven
        CHECK_EQ(header->channel, command_channel);
        CHECK_EQ(header->offset, std::uint64_t { 0 });
    }
}

#if defined(__linux__)

// --- the client, against a server of the test's own -------------------------------------------

constexpr int reply_wait_ms = 4000;

struct Server {
    int listening = -1;
    int client = -1;
    std::string path;
    std::vector<std::uint8_t> incoming;

    explicit Server(std::string socket_path)
        : path(std::move(socket_path))
    {
        listening = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        ::unlink(path.c_str());
        CHECK(::bind(listening, reinterpret_cast<sockaddr const*>(&address), sizeof address) == 0);
        CHECK(::listen(listening, 1) == 0);
    }
    ~Server()
    {
        if (client >= 0)
            ::close(client);
        if (listening >= 0)
            ::close(listening);
        ::unlink(path.c_str());
    }
    Server(Server const&) = delete;
    Server& operator=(Server const&) = delete;

    bool accept_one()
    {
        pollfd waiting {};
        waiting.fd = listening;
        waiting.events = POLLIN;
        if (::poll(&waiting, 1, reply_wait_ms) <= 0)
            return false;
        client = ::accept(listening, nullptr, nullptr);
        return client >= 0;
    }

    // The next whole packet the client sent, or nothing if it went quiet.
    std::optional<std::pair<Header, std::vector<std::uint8_t>>> read_packet()
    {
        for (;;) {
            std::optional<Header> const header = read_header(incoming);
            if (header && incoming.size() >= header_size + header->length) {
                std::vector<std::uint8_t> payload(incoming.begin() + header_size,
                    incoming.begin() + header_size + static_cast<std::ptrdiff_t>(header->length));
                incoming.erase(incoming.begin(), incoming.begin() + header_size + static_cast<std::ptrdiff_t>(header->length));
                return std::make_pair(*header, std::move(payload));
            }
            pollfd waiting {};
            waiting.fd = client;
            waiting.events = POLLIN;
            if (::poll(&waiting, 1, reply_wait_ms) <= 0)
                return std::nullopt;
            std::uint8_t buffer[8192];
            ssize_t const got = ::recv(client, buffer, sizeof buffer, 0);
            if (got <= 0)
                return std::nullopt;
            incoming.insert(incoming.end(), buffer, buffer + got);
        }
    }

    // The next COMMAND packet, its command and serial read off the front.
    struct Call {
        std::uint32_t command = 0;
        std::uint32_t serial = 0;
        std::vector<std::uint8_t> rest;
    };
    std::optional<Call> read_call()
    {
        for (;;) {
            std::optional<std::pair<Header, std::vector<std::uint8_t>>> const packet = read_packet();
            if (!packet)
                return std::nullopt;
            if (packet->first.channel != command_channel)
                continue;
            Reader reader(packet->second);
            Call call;
            call.command = reader.u32().value_or(0);
            call.serial = reader.u32().value_or(0);
            call.rest.assign(packet->second.end() - static_cast<std::ptrdiff_t>(reader.remaining()), packet->second.end());
            return call;
        }
    }

    void send(std::vector<std::uint8_t> const& packet)
    {
        ::send(client, packet.data(), packet.size(), MSG_NOSIGNAL);
    }
    void reply(std::uint32_t serial, Writer& body)
    {
        (void)serial;
        send(body.packet());
    }
};

void test_audio_device()
{
    std::filesystem::path const socket_path
        = std::filesystem::temp_directory_path() / ("sashfold-audio-test-" + std::to_string(::getpid()) + ".sock");
    Server server(socket_path.string());
    ::setenv("PULSE_SERVER", ("unix:" + socket_path.string()).c_str(), 1);
    ::setenv("PULSE_COOKIE", "/nonexistent/cookie", 1); // a server that wants none

    platform::AudioFormat format;
    format.rate = 48000;
    format.channels = 2;
    std::string error;
    std::unique_ptr<platform::AudioDevice> device = platform::AudioDevice::open(format, "Sashfold", error);
    CHECK(device != nullptr);
    if (!device)
        return;
    CHECK(server.accept_one());

    // It says hello with the protocol it speaks and whatever secret it found.
    std::optional<Server::Call> call = server.read_call();
    CHECK(call.has_value());
    if (!call)
        return;
    CHECK_EQ(call->command, static_cast<std::uint32_t>(Command::Auth));
    Reader auth(call->rest);
    CHECK_EQ(auth.u32().value_or(0), protocol_version);
    Writer auth_reply(Command::Reply, call->serial);
    auth_reply.u32(protocol_version);
    server.send(auth_reply.packet());

    // Then it names itself, so that a mixer can show the stream.
    call = server.read_call();
    CHECK(call.has_value());
    if (!call)
        return;
    CHECK_EQ(call->command, static_cast<std::uint32_t>(Command::SetClientName));
    CHECK(std::string(call->rest.begin(), call->rest.end()).find("Sashfold") != std::string::npos);
    Writer named(Command::Reply, call->serial);
    named.u32(7);
    server.send(named.packet());

    // The stream itself: floats at the rate asked for, and a buffer it
    // sizes itself rather than letting the server take everything.
    call = server.read_call();
    CHECK(call.has_value());
    if (!call)
        return;
    CHECK_EQ(call->command, static_cast<std::uint32_t>(Command::CreatePlaybackStream));
    Reader create(call->rest);
    CHECK(create.skip()); // the sample spec, read as bytes below
    CHECK_EQ(hex(std::span<std::uint8_t const>(call->rest.data(), 7)), std::string("61" "05" "02" "0000bb80"));
    CHECK(create.skip()); // the channel map
    CHECK_EQ(create.u32().value_or(0), 0xFFFFFFFFu); // no sink named
    CHECK(!create.string().has_value()); // nor by name
    std::uint32_t const maxlength = create.u32().value_or(0);
    CHECK_EQ(create.boolean().value_or(true), false); // it starts uncorked
    std::uint32_t const tlength = create.u32().value_or(0);
    std::uint32_t const prebuf = create.u32().value_or(0);
    std::uint32_t const minreq = create.u32().value_or(0);
    // A fifth of a second of stereo floats, and a request no smaller than
    // a fiftieth: the numbers the shipping browsers ask for.
    CHECK_EQ(tlength, 48000u * 8u / 5u);
    CHECK_EQ(minreq, 48000u * 8u / 50u);
    CHECK_EQ(prebuf, minreq);
    CHECK_EQ(maxlength, tlength * 4);
    CHECK(create.ok());
    Writer created(Command::Reply, call->serial);
    created.u32(0).u32(99).u32(0); // the stream, its place in the mixer, nothing wanted yet
    server.send(created.packet());

    // Nothing is sent before the server asks for it.
    std::vector<float> const tone(480 * 2, 0.25f);
    CHECK_EQ(device->write(tone), std::size_t { 480 });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Writer request(Command::Request, 0xFFFFFFFFu);
    request.u32(0).u32(480u * 8u);
    server.send(request.packet());

    // The samples, once whatever the client asked in the meantime is past.
    std::optional<std::pair<Header, std::vector<std::uint8_t>>> block = server.read_packet();
    while (block && block->first.channel == command_channel)
        block = server.read_packet();
    CHECK(block.has_value());
    if (block) {
        CHECK_EQ(block->first.channel, 0u); // the stream's own number
        CHECK_EQ(block->second.size(), std::size_t { 480 * 8 });
        float first = 0;
        std::memcpy(&first, block->second.data(), sizeof first);
        CHECK_EQ(first, 0.25f);
    }

    // The clock is unknown until the server answers for it, and then it is
    // what the server said, less the sound still on its way to the speakers.
    CHECK(!device->clock().valid);
    call = server.read_call();
    while (call && call->command != static_cast<std::uint32_t>(Command::GetPlaybackLatency))
        call = server.read_call();
    CHECK(call.has_value());
    if (!call)
        return;
    Writer latency(Command::Reply, call->serial);
    latency.u64(20000) // twenty milliseconds inside the sink
        .u64(0)
        .boolean(true)
        .timeval(1, 0)
        .timeval(1, 0)
        .u64(48000u * 8u) // a second written
        .u64(24000u * 8u); // half of it read
    server.send(latency.packet());
    for (int i = 0; i < 200 && !device->clock().valid; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    platform::AudioClock const clock = device->clock();
    CHECK(clock.valid);
    CHECK(std::abs(clock.played_seconds - 0.48) < 0.001); // half a second, less the sink's twenty milliseconds
    CHECK(std::abs(clock.latency_seconds - 0.52) < 0.001);

    // Pausing corks the stream where it stands; a seek throws away what was
    // held and tells the server to drop what it had.
    device->set_paused(true);
    call = server.read_call();
    while (call && call->command != static_cast<std::uint32_t>(Command::CorkPlaybackStream))
        call = server.read_call();
    CHECK(call.has_value());
    if (call) {
        Reader cork(call->rest);
        CHECK_EQ(cork.u32().value_or(99), 0u);
        CHECK_EQ(cork.boolean().value_or(false), true);
    }
    device->flush();
    call = server.read_call();
    while (call && call->command != static_cast<std::uint32_t>(Command::FlushPlaybackStream))
        call = server.read_call();
    CHECK(call.has_value());
    CHECK(device->ok());

    // A server that goes away is noticed, and what is written after that is
    // dropped rather than kept for ever.
    ::close(server.client);
    server.client = -1;
    for (int i = 0; i < 200 && device->ok(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!device->ok());
    device.reset();
    ::unsetenv("PULSE_SERVER");
    ::unsetenv("PULSE_COOKIE");
}

void test_no_server()
{
    ::setenv("PULSE_SERVER", "unix:/nonexistent/sashfold/socket", 1);
    std::string error;
    platform::AudioFormat format;
    std::unique_ptr<platform::AudioDevice> device = platform::AudioDevice::open(format, "Sashfold", error);
    // The path is named, so a device is made; it finds nothing there and
    // says so rather than playing into a void.
    CHECK(device != nullptr);
    if (device) {
        for (int i = 0; i < 200 && device->ok(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!device->ok());
        // What it is given while broken is taken and dropped, never kept.
        std::vector<float> const samples(2048, 0.0f);
        device->write(samples);
        CHECK(!device->ok());
    }
    // A rate nothing could carry is refused outright.
    format.rate = 3;
    std::string why;
    CHECK(platform::AudioDevice::open(format, "Sashfold", why) == nullptr);
    CHECK(!why.empty());
    ::unsetenv("PULSE_SERVER");
}

#endif

} // namespace

int main()
{
    test_wav();
    test_protocol_bytes();
#if defined(__linux__)
    test_audio_device();
    test_no_server();
#endif
    return sashfold::test::report("audio");
}
