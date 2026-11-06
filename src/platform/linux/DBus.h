#pragma once

// D-Bus, as much of it as a client that calls a service and hears its
// signals needs (https://dbus.freedesktop.org/doc/dbus-specification.html):
// the wire format of a message, the handshake a bus asks for, and a
// connection over the bus's socket. Written here, as the Wayland and the
// sound server's protocols are, so that the browser links nothing for it.
//
// What is not here: file descriptors passed in a message, the types a
// notification never carries (doubles, 16- and 64-bit numbers are written
// but only 32-bit ones and strings are read), being a service.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::platform::dbus {

// A message body, or part of one, being written: each value at the
// alignment its type asks for, counted from the start of the body.
class Writer {
public:
    void byte(std::uint8_t value);
    void boolean(bool value);
    void int32(std::int32_t value);
    void uint32(std::uint32_t value);
    void string(std::string_view text); // also an object path
    void signature(std::string_view text);
    // An array: `fill` writes its elements, each aligned to
    // `element_alignment` (1 byte, 4 a number or a string, 8 a struct or a
    // dictionary entry).
    template <typename Fill>
    void array(std::size_t element_alignment, Fill&& fill)
    {
        std::size_t const length_at = begin_array(element_alignment);
        fill();
        end_array(length_at, element_alignment);
    }
    // The start of a struct or a dictionary entry.
    void align(std::size_t alignment);
    // A variant holding one value of a basic type.
    void variant_string(std::string_view text);
    void variant_boolean(bool value);
    void variant_byte(std::uint8_t value);
    void variant_uint32(std::uint32_t value);

    std::vector<std::uint8_t> const& bytes() const { return m_bytes; }
    std::vector<std::uint8_t> take() { return std::move(m_bytes); }

private:
    std::size_t begin_array(std::size_t element_alignment);
    void end_array(std::size_t length_at, std::size_t element_alignment);
    std::vector<std::uint8_t> m_bytes;
};

// A message body being read. A value that is not there — the body ends, a
// length runs past it — makes every later read answer nothing.
class Reader {
public:
    explicit Reader(std::span<std::uint8_t const> bytes)
        : m_bytes(bytes)
    {
    }
    std::optional<std::uint8_t> byte();
    std::optional<std::uint32_t> uint32();
    std::optional<std::string> string();
    std::optional<std::string> signature();
    std::optional<std::vector<std::string>> string_array();
    bool align(std::size_t alignment);
    bool skip(std::size_t count);
    std::size_t position() const { return m_at; }
    bool failed() const { return m_failed; }

private:
    std::span<std::uint8_t const> m_bytes;
    std::size_t m_at = 0;
    bool m_failed = false;
};

enum class MessageType : std::uint8_t {
    MethodCall = 1,
    MethodReturn = 2,
    Error = 3,
    Signal = 4,
};

struct Message {
    MessageType type = MessageType::MethodCall;
    bool no_reply_expected = false;
    std::uint32_t serial = 0;
    std::uint32_t reply_serial = 0; // of a return or an error: the call it answers
    std::string path;
    std::string interface;
    std::string member;
    std::string error_name;
    std::string destination;
    std::string sender;
    std::string signature; // of the body
    std::vector<std::uint8_t> body;
};

// The message as it goes on the wire, little-endian.
std::vector<std::uint8_t> encode(Message const& message);

// The first message in `bytes` and how many bytes it took: nothing with
// `used` 0 when the bytes do not yet hold a whole message, nothing with
// `used` set past it when it is whole and cannot be read (big-endian, a
// header that does not parse) — the caller drops those bytes and goes on.
// `malformed` is set when what is there cannot be the start of a message
// at all, and the connection is not worth keeping.
std::optional<Message> decode(std::span<std::uint8_t const> bytes, std::size_t& used, bool& malformed);

// Where the session's bus listens: the socket's path, or its name in the
// abstract namespace (`abstract` true), from an address as
// DBUS_SESSION_BUS_ADDRESS gives it ("unix:path=/run/user/1000/bus", several
// separated by ';' — the first that names a unix socket is taken).
struct SocketAddress {
    std::string path;
    bool abstract = false;
};
std::optional<SocketAddress> parse_address(std::string_view address);
// The session bus's address: the environment's, else the socket the user's
// runtime folder holds; nothing when neither is there.
std::optional<SocketAddress> session_bus_address();

// A connection to a bus, authenticated as this user and said hello to.
// One thread reads it; send() may be called from any.
class Connection {
public:
    // Nothing, with the reason, when the socket cannot be reached, the bus
    // refuses this user or the hello is not answered within a few seconds.
    static std::unique_ptr<Connection> open(SocketAddress const& address, std::string* why_not = nullptr);
    ~Connection();
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;

    // Sends the message under a serial of its own, which is answered: the
    // reply's reply_serial. 0 when the socket is gone.
    std::uint32_t send(Message message);
    // The next message, waiting up to `timeout_ms` for one (negative: for
    // as long as it takes; `also_fd` readable ends the wait too, with
    // nothing). Nothing also when the bus has gone — closed() says which.
    std::optional<Message> receive(int timeout_ms, int also_fd = -1);
    bool closed() const { return m_closed; }
    // The name the bus gave this connection (":1.42").
    std::string const& unique_name() const { return m_unique_name; }

private:
    Connection() = default;
    int m_fd = -1;
    std::atomic<bool> m_closed { false };
    std::uint32_t m_next_serial = 1;
    std::string m_unique_name;
    std::vector<std::uint8_t> m_pending; // read and not yet a whole message
    struct Lock;
    std::unique_ptr<Lock> m_send_lock;
};

}
