#include "platform/Audio.h"
#include "platform/PulseProtocol.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

// Sound on Linux: the sound server's native protocol over its own socket,
// no client library (the pledge's line, the same one the Wayland window
// keeps). Every current desktop runs a server that speaks it — PipeWire
// answers it as well as PulseAudio itself — and a stream through it mixes
// with the rest of the machine's sound, which the kernel's own interface
// would not.
//
// One thread owns the socket for the stream's life. The engine's threads
// only put samples in a ring and wake it; the window's thread is never in
// this file. The server asks for bytes as its buffer drains, and the thread
// answers out of the ring, so what a decoder produces early is held here
// and not in the server.

namespace sashfold::platform {

namespace {

using namespace pulse;

constexpr std::size_t ring_seconds_max = 2; // what the ring holds before a decoder must wait
constexpr double latency_query_ms = 100;
// How much sound the server is asked to hold. Left to itself it will take
// everything offered — seconds of it — and a seek would then wait for all
// of it to drain; the shipping browsers keep a fifth of a second there and
// the rest on their own side, where it can be thrown away at once.
constexpr double target_buffer_seconds = 0.2;
constexpr double smallest_request_seconds = 0.02;

std::string environment(char const* name)
{
    char const* const value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// Where the server listens: what the environment names, else the socket in
// this session's runtime directory.
std::string server_path()
{
    std::string named = environment("PULSE_SERVER");
    if (!named.empty()) {
        // "unix:/path", or a bare path. Anything else (a host for the
        // network transport) is not ours to speak.
        if (named.starts_with("unix:"))
            named = named.substr(5);
        if (!named.empty() && named.front() == '/') {
            std::size_t const comma = named.find(' ');
            return comma == std::string::npos ? named : named.substr(0, comma);
        }
        return {};
    }
    std::string const runtime = environment("XDG_RUNTIME_DIR");
    if (runtime.empty())
        return {};
    return runtime + "/pulse/native";
}

// The secret every client of this session shares with the server. A server
// that asks for none is happy with an empty one.
std::vector<std::uint8_t> read_cookie()
{
    std::string path = environment("PULSE_COOKIE");
    if (path.empty()) {
        std::string const home = environment("HOME");
        if (home.empty())
            return {};
        path = home + "/.config/pulse/cookie";
    }
    int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    std::vector<std::uint8_t> cookie(256);
    ssize_t const read_bytes = ::read(fd, cookie.data(), cookie.size());
    ::close(fd);
    if (read_bytes <= 0)
        return {};
    cookie.resize(static_cast<std::size_t>(read_bytes));
    return cookie;
}

class PulseDevice final : public AudioDevice {
public:
    PulseDevice(AudioFormat const& format, std::string name)
        : AudioDevice(format)
        , m_name(std::move(name))
        , m_frame_bytes(sizeof(float) * format.channels)
        , m_ring_frames(static_cast<std::size_t>(format.rate) * ring_seconds_max)
    {
        m_ring.resize(m_ring_frames * format.channels);
        m_wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        m_thread = std::thread([this] { run(); });
    }

    ~PulseDevice() override
    {
        m_stop.store(true);
        wake();
        if (m_thread.joinable())
            m_thread.join();
        if (m_wake >= 0)
            ::close(m_wake);
    }

    std::size_t write(std::span<float const> interleaved) override
    {
        std::size_t const channels = m_format.channels;
        if (channels == 0 || interleaved.empty())
            return 0;
        std::size_t taken = 0;
        {
            std::lock_guard<std::mutex> const guard(m_lock);
            std::size_t const room = m_ring_frames - m_held_frames - 1;
            std::size_t const frames = std::min(interleaved.size() / channels, room);
            for (std::size_t i = 0; i < frames * channels; ++i) {
                m_ring[(m_write_at + i) % m_ring.size()] = interleaved[i];
            }
            m_write_at = (m_write_at + frames * channels) % m_ring.size();
            m_held_frames += frames;
            taken = frames;
        }
        if (taken > 0)
            wake();
        return taken;
    }

    std::size_t writable_frames() const override
    {
        std::lock_guard<std::mutex> const guard(m_lock);
        return m_ring_frames - m_held_frames - 1;
    }

    AudioClock clock() const override
    {
        std::lock_guard<std::mutex> const guard(m_lock);
        // What the ring still holds was never handed over, so it is not in
        // these numbers: they are the stream as the server knows it.
        return m_clock;
    }

    void set_paused(bool paused) override
    {
        std::lock_guard<std::mutex> const guard(m_lock);
        if (m_paused_wanted != paused) {
            m_paused_wanted = paused;
            m_paused_pending = true;
        }
        wake();
    }

    void set_volume(double volume) override
    {
        std::lock_guard<std::mutex> const guard(m_lock);
        std::uint32_t const scaled = static_cast<std::uint32_t>(std::clamp(volume, 0.0, 1.0) * volume_normal);
        if (m_volume != scaled) {
            m_volume = scaled;
            m_volume_pending = true;
        }
        wake();
    }

    void flush() override
    {
        std::lock_guard<std::mutex> const guard(m_lock);
        m_held_frames = 0;
        m_write_at = 0;
        m_read_at = 0;
        m_flush_pending = true;
        m_clock = {};
        m_written_frames = 0;
        wake();
    }

    bool ok() const override { return !m_failed.load(); }
    std::string const& error() const { return m_error; }

private:
    void wake()
    {
        if (m_wake < 0)
            return;
        std::uint64_t const one = 1;
        [[maybe_unused]] ssize_t const written = ::write(m_wake, &one, sizeof one);
    }

    void fail(std::string why)
    {
        if (m_error.empty())
            m_error = std::move(why);
        m_failed.store(true);
    }

    // --- the socket, all of it on this thread ------------------------------------------

    bool connect_socket()
    {
        std::string const path = server_path();
        if (path.empty()) {
            fail("no sound server socket is named by the environment");
            return false;
        }
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (path.size() + 1 > sizeof address.sun_path) {
            fail("the sound server's socket path is too long");
            return false;
        }
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        m_socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_socket < 0) {
            fail("no socket for the sound server: " + std::string(std::strerror(errno)));
            return false;
        }
        if (::connect(m_socket, reinterpret_cast<sockaddr const*>(&address), sizeof address) != 0) {
            fail("the sound server did not answer at " + path + ": " + std::string(std::strerror(errno)));
            return false;
        }
        int const flags = ::fcntl(m_socket, F_GETFL, 0);
        ::fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    void send(std::vector<std::uint8_t> const& packet)
    {
        m_outgoing.insert(m_outgoing.end(), packet.begin(), packet.end());
    }

    // Writes what the socket will take; false once it has gone.
    bool flush_outgoing()
    {
        while (!m_outgoing.empty()) {
            ssize_t const written = ::send(m_socket, m_outgoing.data(), m_outgoing.size(), MSG_NOSIGNAL);
            if (written > 0) {
                m_outgoing.erase(m_outgoing.begin(), m_outgoing.begin() + written);
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return true;
            fail("the sound server stopped listening: " + std::string(std::strerror(errno)));
            return false;
        }
        return true;
    }

    // Waits for one whole reply to the serial given, dispatching whatever
    // else arrives; used only for the handshake.
    std::optional<std::vector<std::uint8_t>> await_reply(std::uint32_t serial)
    {
        for (int turns = 0; turns < 1000 && !m_stop.load(); ++turns) {
            if (!flush_outgoing())
                return std::nullopt;
            while (std::optional<Packet> packet = next_packet()) {
                if (packet->header.channel != command_channel)
                    continue;
                Reader reader(packet->payload);
                std::optional<std::uint32_t> const command = reader.u32();
                std::optional<std::uint32_t> const answered = reader.u32();
                if (!command || !answered)
                    continue;
                if (*command == static_cast<std::uint32_t>(Command::Error) && *answered == serial) {
                    std::optional<std::uint32_t> const code = reader.u32();
                    fail("the sound server refused the stream (error " + std::to_string(code.value_or(0)) + ")");
                    return std::nullopt;
                }
                if (*command == static_cast<std::uint32_t>(Command::Reply) && *answered == serial) {
                    std::vector<std::uint8_t> const rest(packet->payload.begin() + static_cast<std::ptrdiff_t>(packet->payload.size() - reader.remaining()),
                        packet->payload.end());
                    return rest;
                }
            }
            if (!receive_more(1000))
                return std::nullopt;
        }
        fail("the sound server did not answer");
        return std::nullopt;
    }

    struct Packet {
        Header header;
        std::vector<std::uint8_t> payload;
    };

    // One whole packet out of what has arrived, if there is one.
    std::optional<Packet> next_packet()
    {
        std::optional<Header> const header = read_header(m_incoming);
        if (!header)
            return std::nullopt;
        if (m_incoming.size() < header_size + header->length)
            return std::nullopt;
        Packet packet;
        packet.header = *header;
        packet.payload.assign(m_incoming.begin() + header_size, m_incoming.begin() + header_size + static_cast<std::ptrdiff_t>(header->length));
        m_incoming.erase(m_incoming.begin(), m_incoming.begin() + header_size + static_cast<std::ptrdiff_t>(header->length));
        return packet;
    }

    // Waits up to `wait_ms` for the socket or a wake, and reads what came.
    bool receive_more(int wait_ms)
    {
        pollfd fds[2] {};
        fds[0].fd = m_socket;
        fds[0].events = POLLIN | (m_outgoing.empty() ? 0 : POLLOUT);
        fds[1].fd = m_wake;
        fds[1].events = POLLIN;
        int const ready = ::poll(fds, m_wake >= 0 ? 2 : 1, wait_ms);
        if (ready < 0 && errno != EINTR) {
            fail("the sound server's socket failed: " + std::string(std::strerror(errno)));
            return false;
        }
        if (m_wake >= 0 && (fds[1].revents & POLLIN) != 0) {
            std::uint64_t counter = 0;
            [[maybe_unused]] ssize_t const read_bytes = ::read(m_wake, &counter, sizeof counter);
        }
        if ((fds[0].revents & (POLLERR | POLLHUP)) != 0) {
            fail("the sound server closed the connection");
            return false;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            std::uint8_t buffer[16384];
            for (;;) {
                ssize_t const got = ::recv(m_socket, buffer, sizeof buffer, 0);
                if (got > 0) {
                    m_incoming.insert(m_incoming.end(), buffer, buffer + got);
                    if (static_cast<std::size_t>(got) < sizeof buffer)
                        break;
                    continue;
                }
                if (got == 0) {
                    fail("the sound server closed the connection");
                    return false;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                fail("the sound server's socket failed: " + std::string(std::strerror(errno)));
                return false;
            }
        }
        return true;
    }

    bool handshake()
    {
        std::vector<std::uint8_t> const cookie = read_cookie();
        Writer auth(Command::Auth, m_serial);
        auth.u32(protocol_version).arbitrary(cookie);
        send(auth.packet());
        std::optional<std::vector<std::uint8_t>> reply = await_reply(m_serial++);
        if (!reply)
            return false;
        Reader version_reader(*reply);
        m_server_version = version_reader.u32().value_or(0);
        if (m_server_version < 13) {
            fail("the sound server speaks too old a protocol (" + std::to_string(m_server_version) + ")");
            return false;
        }

        std::vector<std::pair<std::string, std::string>> properties {
            { "application.name", m_name },
            { "application.id", "com.sashfold.Sashfold" },
            { "application.process.binary", "sashfold" },
            { "media.role", "video" },
        };
        Writer client_name(Command::SetClientName, m_serial);
        client_name.proplist(properties);
        send(client_name.packet());
        if (!await_reply(m_serial++))
            return false;

        // The stream itself. The fields are the protocol's, in its order;
        // everything we do not ask to be chosen for us is left invalid so
        // the server sizes its own buffer.
        std::uint8_t const channels = static_cast<std::uint8_t>(m_format.channels);
        std::vector<std::uint8_t> map;
        if (channels == 1) {
            map.push_back(0); // mono
        } else {
            for (std::uint8_t i = 0; i < channels; ++i)
                map.push_back(static_cast<std::uint8_t>(i + 1)); // front left, front right, ...
        }
        double const bytes_a_second = static_cast<double>(m_format.rate) * static_cast<double>(m_frame_bytes);
        std::size_t const target = static_cast<std::size_t>(bytes_a_second * target_buffer_seconds) / m_frame_bytes * m_frame_bytes;
        std::size_t const smallest = static_cast<std::size_t>(bytes_a_second * smallest_request_seconds) / m_frame_bytes * m_frame_bytes;
        std::vector<std::uint32_t> volumes(channels, m_volume);
        std::vector<std::pair<std::string, std::string>> stream_properties {
            { "media.name", m_name },
            { "media.role", "video" },
        };
        Writer stream(Command::CreatePlaybackStream, m_serial);
        stream.sample_spec(SampleFormat::Float32Le, channels, m_format.rate)
            .channel_map(map)
            .u32(invalid_index) // the default sink
            .null_string() // named by nothing
            .u32(static_cast<std::uint32_t>(target * 4)) // maxlength: never more than four targets
            .boolean(false) // not corked: it plays when it has sound
            .u32(static_cast<std::uint32_t>(target)) // tlength: what it holds
            .u32(static_cast<std::uint32_t>(smallest)) // prebuf: it starts on this much
            .u32(static_cast<std::uint32_t>(smallest)) // minreq: the least it asks for
            .u32(0) // synchronised with no other stream
            .cvolume(volumes);
        for (int i = 0; i < 9; ++i)
            stream.boolean(false); // no remap, no remix, no fixing, movable, not muted
        stream.proplist(stream_properties)
            .boolean(true) // the volume above is meant
            .boolean(false) // not asking for its requests early
            .boolean(false) // the mute above is not meant
            .boolean(false) // the machine may still suspend
            .boolean(false) // a suspended sink is not a failure
            .boolean(false) // the volume is the stream's own, not the sink's
            .boolean(false) // not passed through untouched
            .u8(0); // no format of our own to offer: the sample spec says it
        send(stream.packet());
        std::optional<std::vector<std::uint8_t>> const created = await_reply(m_serial++);
        if (!created)
            return false;
        Reader reader(*created);
        std::optional<std::uint32_t> const index = reader.u32();
        std::optional<std::uint32_t> const sink_input = reader.u32();
        std::optional<std::uint32_t> const missing = reader.u32();
        if (!index || !sink_input || !missing) {
            fail("the sound server's answer made no sense");
            return false;
        }
        m_stream = *index;
        m_sink_input = *sink_input;
        {
            std::lock_guard<std::mutex> const guard(m_lock);
            m_credit_bytes = *missing;
        }
        return true;
    }

    // Hands the server as many bytes as it has asked for and the ring holds.
    void feed()
    {
        for (;;) {
            std::vector<float> samples;
            {
                std::lock_guard<std::mutex> const guard(m_lock);
                std::size_t const frames = std::min(m_held_frames, m_credit_bytes / m_frame_bytes);
                if (frames == 0)
                    return;
                samples.resize(frames * m_format.channels);
                for (std::size_t i = 0; i < samples.size(); ++i)
                    samples[i] = m_ring[(m_read_at + i) % m_ring.size()];
                m_read_at = (m_read_at + samples.size()) % m_ring.size();
                m_held_frames -= frames;
                m_credit_bytes -= frames * m_frame_bytes;
                m_written_frames += frames;
                m_clock.written_seconds = static_cast<double>(m_written_frames) / static_cast<double>(m_format.rate);
            }
            std::span<std::uint8_t const> const bytes(reinterpret_cast<std::uint8_t const*>(samples.data()), samples.size() * sizeof(float));
            send(block_packet(m_stream, bytes));
        }
    }

    void ask_for_latency()
    {
        ::timeval now {};
        ::gettimeofday(&now, nullptr);
        Writer query(Command::GetPlaybackLatency, m_serial);
        query.u32(m_stream).timeval(static_cast<std::uint32_t>(now.tv_sec), static_cast<std::uint32_t>(now.tv_usec));
        send(query.packet());
        m_latency_serial = m_serial++;
    }

    void handle(Packet const& packet)
    {
        if (packet.header.channel != command_channel)
            return;
        Reader reader(packet.payload);
        std::optional<std::uint32_t> const command = reader.u32();
        std::optional<std::uint32_t> const serial = reader.u32();
        if (!command || !serial)
            return;
        switch (static_cast<Command>(*command)) {
        case Command::Request: {
            std::optional<std::uint32_t> const stream = reader.u32();
            std::optional<std::uint32_t> const bytes = reader.u32();
            if (stream && *stream == m_stream && bytes) {
                std::lock_guard<std::mutex> const guard(m_lock);
                m_credit_bytes += *bytes;
            }
            break;
        }
        case Command::PlaybackStreamKilled:
            fail("the sound server ended the stream");
            break;
        case Command::Reply: {
            if (*serial != m_latency_serial)
                break;
            // Sink latency, then the source's, whether it plays, the two
            // times, and where the server has read to and we have written.
            std::optional<std::uint64_t> const sink_latency = reader.u64();
            reader.u64();
            reader.boolean();
            reader.skip();
            reader.skip();
            std::optional<std::uint64_t> const write_index = reader.u64();
            std::optional<std::uint64_t> const read_index = reader.u64();
            if (!reader.ok() || !sink_latency || !write_index || !read_index)
                break;
            double const per_second = static_cast<double>(m_format.rate) * static_cast<double>(m_frame_bytes);
            double const played = static_cast<double>(static_cast<std::int64_t>(*read_index)) / per_second;
            double const written = static_cast<double>(static_cast<std::int64_t>(*write_index)) / per_second;
            std::lock_guard<std::mutex> const guard(m_lock);
            m_clock.played_seconds = std::max(0.0, played - static_cast<double>(*sink_latency) / 1e6);
            m_clock.latency_seconds = std::max(0.0, written - m_clock.played_seconds);
            m_clock.valid = true;
            break;
        }
        case Command::Error:
            fail("the sound server refused a request");
            break;
        default:
            break;
        }
    }

    void run()
    {
        if (!connect_socket() || !handshake())
            return;
        double since_latency = 0;
        while (!m_stop.load() && !m_failed.load()) {
            bool paused = false;
            bool volume = false;
            bool flushed = false;
            std::uint32_t wanted_volume = 0;
            bool wanted_paused = false;
            {
                std::lock_guard<std::mutex> const guard(m_lock);
                paused = std::exchange(m_paused_pending, false);
                volume = std::exchange(m_volume_pending, false);
                flushed = std::exchange(m_flush_pending, false);
                wanted_volume = m_volume;
                wanted_paused = m_paused_wanted;
            }
            if (flushed) {
                Writer flush_request(Command::FlushPlaybackStream, m_serial++);
                flush_request.u32(m_stream);
                send(flush_request.packet());
            }
            if (paused) {
                Writer cork(Command::CorkPlaybackStream, m_serial++);
                cork.u32(m_stream).boolean(wanted_paused);
                send(cork.packet());
            }
            if (volume) {
                std::vector<std::uint32_t> const volumes(m_format.channels, wanted_volume);
                Writer set_volume(Command::SetSinkInputVolume, m_serial++);
                set_volume.u32(m_sink_input).cvolume(volumes);
                send(set_volume.packet());
            }
            feed();
            if (!flush_outgoing())
                break;
            if (since_latency >= latency_query_ms) {
                since_latency = 0;
                ask_for_latency();
                if (!flush_outgoing())
                    break;
            }
            if (!receive_more(20))
                break;
            since_latency += 20;
            while (std::optional<Packet> const packet = next_packet())
                handle(*packet);
        }
        if (m_socket >= 0) {
            ::close(m_socket);
            m_socket = -1;
        }
    }

    std::string m_name;
    std::size_t m_frame_bytes;
    std::size_t m_ring_frames;

    mutable std::mutex m_lock;
    std::vector<float> m_ring;
    std::size_t m_write_at = 0;
    std::size_t m_read_at = 0;
    std::size_t m_held_frames = 0;
    std::size_t m_credit_bytes = 0;
    std::uint64_t m_written_frames = 0;
    AudioClock m_clock;
    std::uint32_t m_volume = volume_normal;
    bool m_volume_pending = false;
    bool m_paused_wanted = false;
    bool m_paused_pending = false;
    bool m_flush_pending = false;

    int m_socket = -1;
    int m_wake = -1;
    std::vector<std::uint8_t> m_incoming;
    std::vector<std::uint8_t> m_outgoing;
    std::uint32_t m_serial = 0;
    std::uint32_t m_latency_serial = 0xFFFFFFFFu;
    std::uint32_t m_stream = 0;
    std::uint32_t m_sink_input = 0;
    std::uint32_t m_server_version = 0;
    std::string m_error;
    std::atomic<bool> m_failed { false };
    std::atomic<bool> m_stop { false };
    std::thread m_thread;
};

}

std::unique_ptr<AudioDevice> AudioDevice::open(AudioFormat const& format, std::string const& name, std::string& error)
{
    if (format.channels == 0 || format.channels > 8 || format.rate < 4000 || format.rate > 384000) {
        error = "that sample rate or channel count is not one a stream can carry";
        return nullptr;
    }
    if (server_path().empty()) {
        error = "no sound server socket is named by the environment";
        return nullptr;
    }
    return std::make_unique<PulseDevice>(format, name);
}

}
