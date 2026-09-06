#include "platform/linux/Wayland.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace sashfold::platform::wayland {

namespace {

// Wire words are native-endian 32-bit; the socket is local, so the
// compositor shares our byte order.
std::uint32_t read_word(std::span<std::uint8_t const> bytes, std::size_t offset)
{
    std::uint32_t word = 0;
    std::memcpy(&word, bytes.data() + offset, sizeof word);
    return word;
}

void append_word(std::vector<std::uint8_t>& out, std::uint32_t word)
{
    std::uint8_t bytes[4];
    std::memcpy(bytes, &word, sizeof bytes);
    out.insert(out.end(), bytes, bytes + 4);
}

std::size_t padded(std::size_t length)
{
    return (length + 3) & ~static_cast<std::size_t>(3);
}

// The control-message layout is public (a cmsghdr, then data aligned to
// its own size), spelled out here because the CMSG_* macros expand to the
// C casts this codebase's warnings forbid.
constexpr std::size_t cmsg_align(std::size_t length)
{
    return (length + sizeof(std::size_t) - 1) & ~(sizeof(std::size_t) - 1);
}
constexpr std::size_t cmsg_header_size = cmsg_align(sizeof(cmsghdr));
constexpr std::size_t max_fds_in_flight = 28; // libwayland's own ceiling per message batch
constexpr std::size_t cmsg_buffer_size = cmsg_header_size + cmsg_align(max_fds_in_flight * sizeof(int));

constexpr std::uint32_t client_id_limit = 0xff000000; // ids from here up belong to the server

} // namespace

// --- Message -----------------------------------------------------------------

Message::Message(std::span<std::uint8_t const> payload, int fd)
    : m_payload(payload)
    , m_fd(fd)
{
}

std::uint32_t Message::uint()
{
    if (m_offset + 4 > m_payload.size()) {
        m_ok = false;
        return 0;
    }
    std::uint32_t const word = read_word(m_payload, m_offset);
    m_offset += 4;
    return word;
}

std::int32_t Message::int_()
{
    return static_cast<std::int32_t>(uint());
}

double Message::fixed()
{
    return static_cast<double>(int_()) / 256.0;
}

std::string Message::string()
{
    std::uint32_t const length = uint(); // counts the terminating NUL
    if (!m_ok || length == 0)
        return {};
    if (m_offset + padded(length) > m_payload.size()) {
        m_ok = false;
        return {};
    }
    std::string text(reinterpret_cast<char const*>(m_payload.data() + m_offset), length - 1);
    m_offset += padded(length);
    return text;
}

std::span<std::uint8_t const> Message::array()
{
    std::uint32_t const length = uint();
    if (!m_ok)
        return {};
    if (m_offset + padded(length) > m_payload.size()) {
        m_ok = false;
        return {};
    }
    std::span<std::uint8_t const> const bytes = m_payload.subspan(m_offset, length);
    m_offset += padded(length);
    return bytes;
}

int Message::fd()
{
    int const descriptor = m_fd;
    m_fd = -1;
    return descriptor;
}

// --- Request -----------------------------------------------------------------

Request::Request(std::uint32_t object, std::uint16_t opcode)
{
    append_word(m_bytes, object);
    append_word(m_bytes, opcode); // the size joins it in bytes()
}

Request& Request::uint(std::uint32_t value)
{
    append_word(m_bytes, value);
    return *this;
}

Request& Request::int_(std::int32_t value)
{
    append_word(m_bytes, static_cast<std::uint32_t>(value));
    return *this;
}

Request& Request::fixed(double value)
{
    double const scaled = value * 256.0;
    double const clamped = std::clamp(scaled, -8388608.0, 8388607.0);
    return int_(static_cast<std::int32_t>(clamped < 0 ? clamped - 0.5 : clamped + 0.5));
}

Request& Request::string(std::string_view value)
{
    std::size_t const length = value.size() + 1;
    append_word(m_bytes, static_cast<std::uint32_t>(length));
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
    m_bytes.resize(m_bytes.size() + (padded(length) - value.size()), 0); // NUL and padding
    return *this;
}

Request& Request::array(std::span<std::uint8_t const> bytes)
{
    append_word(m_bytes, static_cast<std::uint32_t>(bytes.size()));
    m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
    m_bytes.resize(m_bytes.size() + (padded(bytes.size()) - bytes.size()), 0);
    return *this;
}

Request& Request::fd(int descriptor)
{
    m_fds.push_back(descriptor);
    return *this;
}

std::vector<std::uint8_t> const& Request::bytes()
{
    std::uint32_t const opcode = read_word(m_bytes, 4) & 0xffffu;
    std::uint32_t const size = static_cast<std::uint32_t>(m_bytes.size()) & 0xffffu;
    std::uint32_t const word = (size << 16) | opcode;
    std::memcpy(m_bytes.data() + 4, &word, sizeof word);
    return m_bytes;
}

// --- Connection --------------------------------------------------------------

Connection::Connection(int fd)
    : m_fd(fd)
{
}

Connection::~Connection()
{
    for (int const fd : m_in_fds)
        ::close(fd);
    if (m_fd >= 0)
        ::close(m_fd);
}

std::unique_ptr<Connection> Connection::connect(std::string& error)
{
    if (char const* inherited = std::getenv("WAYLAND_SOCKET"); inherited && *inherited) {
        int const fd = std::atoi(inherited);
        if (fd > 0)
            return adopt(fd);
    }
    char const* display = std::getenv("WAYLAND_DISPLAY");
    std::string path = display && *display ? display : "wayland-0";
    if (path.front() != '/') {
        char const* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            error = "XDG_RUNTIME_DIR is not set, so there is no compositor socket to open";
            return nullptr;
        }
        path = std::string(runtime) + "/" + path;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path) {
        error = "the compositor socket path is too long: " + path;
        return nullptr;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    int const fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return nullptr;
    }
    if (::connect(fd, reinterpret_cast<sockaddr const*>(&address), sizeof address) != 0) {
        error = "cannot connect to " + path + ": " + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    return adopt(fd);
}

std::unique_ptr<Connection> Connection::adopt(int fd)
{
    return std::unique_ptr<Connection>(new Connection(fd));
}

std::uint32_t Connection::allocate_id()
{
    if (!m_free_ids.empty()) {
        std::uint32_t const id = m_free_ids.back();
        m_free_ids.pop_back();
        return id;
    }
    return m_next_id++;
}

void Connection::listen(std::uint32_t id, Listener listener, std::vector<std::uint16_t> fd_opcodes)
{
    m_listeners[id] = std::move(listener);
    if (!fd_opcodes.empty())
        m_fd_opcodes[id] = std::move(fd_opcodes);
}

void Connection::forget(std::uint32_t id)
{
    m_listeners.erase(id);
    // m_fd_opcodes stays until delete_id: a descriptor sent to a dying
    // object must still be paired with its event and closed.
}

void Connection::send(Request& request)
{
    if (m_failed)
        return;
    if (request.fds().empty()) {
        std::vector<std::uint8_t> const& bytes = request.bytes();
        m_out.insert(m_out.end(), bytes.begin(), bytes.end());
        return;
    }
    // Descriptors ride with the bytes of their own message: flush what
    // came before, then send this message and its descriptors together.
    if (!flush())
        return;
    if (!write_all(request.bytes(), request.fds()))
        fail(std::string("cannot write to the compositor: ") + std::strerror(errno));
}

bool Connection::flush()
{
    if (m_failed)
        return false;
    if (m_out.empty())
        return true;
    std::vector<std::uint8_t> pending;
    pending.swap(m_out);
    if (!write_all(pending, {})) {
        fail(std::string("cannot write to the compositor: ") + std::strerror(errno));
        return false;
    }
    return true;
}

bool Connection::write_all(std::span<std::uint8_t const> bytes, std::vector<int> const& fds)
{
    std::size_t sent = 0;
    bool fds_pending = !fds.empty();
    while (sent < bytes.size()) {
        iovec vector {};
        vector.iov_base = const_cast<std::uint8_t*>(bytes.data() + sent);
        vector.iov_len = bytes.size() - sent;
        msghdr header {};
        header.msg_iov = &vector;
        header.msg_iovlen = 1;
        alignas(cmsghdr) std::uint8_t control[cmsg_buffer_size] = {};
        if (fds_pending) {
            std::size_t const count = std::min(fds.size(), max_fds_in_flight);
            auto* const cmsg = reinterpret_cast<cmsghdr*>(control);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = cmsg_header_size + count * sizeof(int);
            std::memcpy(control + cmsg_header_size, fds.data(), count * sizeof(int));
            header.msg_control = control;
            header.msg_controllen = cmsg_header_size + cmsg_align(count * sizeof(int));
        }
        ssize_t const written = ::sendmsg(m_fd, &header, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                pollfd waiter { m_fd, POLLOUT, 0 };
                ::poll(&waiter, 1, 1000);
                continue;
            }
            return false;
        }
        fds_pending = false; // they travelled with the first bytes
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool Connection::read_available()
{
    for (;;) {
        std::uint8_t chunk[4096];
        iovec vector {};
        vector.iov_base = chunk;
        vector.iov_len = sizeof chunk;
        alignas(cmsghdr) std::uint8_t control[cmsg_buffer_size];
        msghdr header {};
        header.msg_iov = &vector;
        header.msg_iovlen = 1;
        header.msg_control = control;
        header.msg_controllen = sizeof control;
        ssize_t const received = ::recvmsg(m_fd, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            fail(std::string("the compositor connection broke: ") + std::strerror(errno));
            return false;
        }
        if (received == 0) {
            fail("the compositor closed the connection");
            return false;
        }
        m_in.insert(m_in.end(), chunk, chunk + received);
        // Walk the control messages for SCM_RIGHTS.
        std::size_t offset = 0;
        while (offset + cmsg_header_size <= header.msg_controllen) {
            auto const* const cmsg = reinterpret_cast<cmsghdr const*>(control + offset);
            if (cmsg->cmsg_len < cmsg_header_size)
                break;
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                std::size_t const count = (cmsg->cmsg_len - cmsg_header_size) / sizeof(int);
                for (std::size_t i = 0; i < count; ++i) {
                    int fd = -1;
                    std::memcpy(&fd, control + offset + cmsg_header_size + i * sizeof(int), sizeof fd);
                    m_in_fds.push_back(fd);
                }
            }
            offset += cmsg_align(cmsg->cmsg_len);
        }
        if (static_cast<std::size_t>(received) < sizeof chunk)
            return true;
    }
}

bool Connection::takes_fd(std::uint32_t id, std::uint16_t opcode) const
{
    auto const found = m_fd_opcodes.find(id);
    if (found == m_fd_opcodes.end())
        return false;
    return std::find(found->second.begin(), found->second.end(), opcode) != found->second.end();
}

int Connection::dispatch_buffered()
{
    int count = 0;
    std::size_t offset = 0;
    while (m_in.size() - offset >= 8) {
        std::uint32_t const id = read_word(m_in, offset);
        std::uint32_t const word = read_word(m_in, offset + 4);
        std::uint16_t const opcode = static_cast<std::uint16_t>(word & 0xffffu);
        std::size_t const size = word >> 16;
        if (size < 8) {
            fail("the compositor sent a malformed message");
            break;
        }
        if (m_in.size() - offset < size)
            break; // the rest is still in flight
        int fd = -1;
        if (takes_fd(id, opcode) && !m_in_fds.empty()) {
            fd = m_in_fds.front();
            m_in_fds.pop_front();
        }
        Message message(std::span<std::uint8_t const>(m_in.data() + offset + 8, size - 8), fd);
        if (id == display_id) {
            if (opcode == 0) { // error
                std::uint32_t const object = message.object();
                std::uint32_t const code = message.uint();
                std::string const text = message.string();
                fail("protocol error on object " + std::to_string(object) + " (code "
                    + std::to_string(code) + "): " + text);
            } else if (opcode == 1) { // delete_id
                std::uint32_t const deleted = message.uint();
                m_listeners.erase(deleted);
                m_fd_opcodes.erase(deleted);
                if (deleted < client_id_limit)
                    m_free_ids.push_back(deleted);
            }
        } else if (auto const found = m_listeners.find(id); found != m_listeners.end()) {
            Listener listener = found->second; // a copy: the listener may forget itself
            listener(opcode, message);
        }
        if (int const untaken = message.fd(); untaken >= 0)
            ::close(untaken);
        ++count;
        offset += size;
        if (m_failed)
            break;
    }
    m_in.erase(m_in.begin(), m_in.begin() + static_cast<std::ptrdiff_t>(offset));
    return count;
}

int Connection::dispatch(int timeout_ms)
{
    if (m_failed)
        return -1;
    if (!flush())
        return -1;
    if (!read_available())
        return -1;
    int count = dispatch_buffered();
    if (count == 0 && timeout_ms != 0) {
        pollfd waiter { m_fd, POLLIN, 0 };
        int const ready = ::poll(&waiter, 1, timeout_ms);
        if (ready < 0 && errno != EINTR) {
            fail(std::string("poll: ") + std::strerror(errno));
            return -1;
        }
        if (ready > 0) {
            if (!read_available())
                return -1;
            count = dispatch_buffered();
        }
    }
    return m_failed ? -1 : count;
}

bool Connection::roundtrip()
{
    if (m_failed)
        return false;
    std::uint32_t const callback = allocate_id();
    bool done = false;
    listen(callback, [&done](std::uint16_t, Message&) { done = true; });
    Request sync(display_id, 0);
    sync.new_id(callback);
    send(sync);
    while (!done && !m_failed) {
        if (dispatch(-1) < 0)
            break;
    }
    forget(callback);
    return done && !m_failed;
}

void Connection::fail(std::string message)
{
    if (m_failed)
        return;
    m_failed = true;
    m_error = std::move(message);
}

}
