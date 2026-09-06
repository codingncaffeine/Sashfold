#pragma once

// The Wayland wire protocol, spoken directly over the compositor's unix
// socket: no libwayland, per the pledge — a display server's protocol is
// the machine's own interface, the same way HTTP is the network's. This is
// the transport only: message framing, the 24.8 fixed type, strings and
// arrays, file descriptors carried as SCM_RIGHTS, object ids and their
// recycling, and a dispatcher that hands each event to the object's
// listener. Which interfaces exist and what their opcodes mean is the
// window's business (WindowWayland.cpp); it reads them off the protocol XML.

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sashfold::platform::wayland {

// One event's arguments, read in wire order. A read past the end sets
// ok() false and yields zeros, so a short message cannot walk off the buffer.
class Message {
public:
    Message(std::span<std::uint8_t const> payload, int fd);

    std::uint32_t uint();
    std::int32_t int_();
    double fixed(); // 24.8 fixed point
    std::string string(); // empty for a null string
    std::span<std::uint8_t const> array();
    std::uint32_t new_id() { return uint(); }
    std::uint32_t object() { return uint(); }
    // The descriptor that arrived with this event; -1 when none did. The
    // caller owns it once taken; an untaken one is closed after dispatch.
    int fd();
    bool ok() const { return m_ok; }
    std::size_t remaining() const { return m_payload.size() - m_offset; }

private:
    std::span<std::uint8_t const> m_payload;
    std::size_t m_offset = 0;
    int m_fd;
    bool m_ok = true;
};

// One request, built argument by argument.
class Request {
public:
    Request(std::uint32_t object, std::uint16_t opcode);

    Request& uint(std::uint32_t value);
    Request& int_(std::int32_t value);
    Request& fixed(double value);
    Request& string(std::string_view value);
    Request& array(std::span<std::uint8_t const> bytes);
    Request& new_id(std::uint32_t id) { return uint(id); }
    Request& object(std::uint32_t id) { return uint(id); }
    // The descriptor travels beside the bytes; the caller keeps ownership.
    Request& fd(int descriptor);

    // The bytes with the size patched into the header.
    std::vector<std::uint8_t> const& bytes();
    std::vector<int> const& fds() const { return m_fds; }

private:
    std::vector<std::uint8_t> m_bytes;
    std::vector<int> m_fds;
};

using Listener = std::function<void(std::uint16_t opcode, Message& message)>;

class Connection {
public:
    // Connects to $WAYLAND_DISPLAY under $XDG_RUNTIME_DIR (wayland-0 when
    // unset; an absolute path is used as it is), or adopts the socket a
    // parent handed down in $WAYLAND_SOCKET. Null with `error` set when
    // there is no compositor to talk to.
    static std::unique_ptr<Connection> connect(std::string& error);
    // Speaks over a socket that already exists — a test's socketpair end.
    // Takes the descriptor.
    static std::unique_ptr<Connection> adopt(int fd);
    ~Connection();
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;

    int fd() const { return m_fd; }
    static constexpr std::uint32_t display_id = 1;

    // A fresh client-side object id; ids the server has confirmed deleted
    // are reused.
    std::uint32_t allocate_id();
    // Events for `id` go to `listener`. `fd_opcodes` names the events that
    // carry a descriptor: the connection pairs the descriptor with its
    // event even after the listener is gone, so a stray one never lands on
    // the wrong event.
    void listen(std::uint32_t id, Listener listener, std::vector<std::uint16_t> fd_opcodes = {});
    // After a destructor request: no more events are wanted. The id is
    // recycled once the server's delete_id arrives.
    void forget(std::uint32_t id);

    // Queues a request; it leaves with the next flush, dispatch or roundtrip.
    void send(Request& request);
    bool flush();
    // Waits up to timeout_ms (negative: forever; zero: not at all) for
    // events, then dispatches every complete one. The count dispatched, or
    // -1 once the connection has failed.
    int dispatch(int timeout_ms);
    int dispatch_pending() { return dispatch(0); }
    // wl_display.sync, then dispatch until the compositor answers: every
    // request sent before it has been processed.
    bool roundtrip();

    bool failed() const { return m_failed; }
    std::string const& error() const { return m_error; }
    void fail(std::string message);

private:
    explicit Connection(int fd);
    bool write_all(std::span<std::uint8_t const> bytes, std::vector<int> const& fds);
    // Reads what the socket has (non-blocking); false when it is closed or broken.
    bool read_available();
    int dispatch_buffered();
    bool takes_fd(std::uint32_t id, std::uint16_t opcode) const;

    int m_fd;
    std::vector<std::uint8_t> m_out;
    std::vector<std::uint8_t> m_in;
    std::deque<int> m_in_fds;
    std::unordered_map<std::uint32_t, Listener> m_listeners;
    std::unordered_map<std::uint32_t, std::vector<std::uint16_t>> m_fd_opcodes;
    std::vector<std::uint32_t> m_free_ids;
    std::uint32_t m_next_id = 2;
    bool m_failed = false;
    std::string m_error;
};

}
