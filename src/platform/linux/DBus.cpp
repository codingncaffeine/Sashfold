#include "platform/linux/DBus.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace sashfold::platform::dbus {

namespace {

void pad(std::vector<std::uint8_t>& bytes, std::size_t alignment)
{
    while (bytes.size() % alignment != 0)
        bytes.push_back(0);
}

void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void set32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t get32(std::span<std::uint8_t const> bytes, std::size_t at)
{
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8
        | static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

// The header's fields, by the numbers the specification gives them.
enum Field : std::uint8_t {
    Path = 1,
    Interface = 2,
    Member = 3,
    ErrorName = 4,
    ReplySerial = 5,
    Destination = 6,
    Sender = 7,
    Signature = 8,
};

// The longest message a bus passes on (2^27 bytes), and the longest array.
constexpr std::size_t longest_message = 1u << 27;
constexpr std::size_t longest_array = 1u << 26;

}

void Writer::byte(std::uint8_t value) { m_bytes.push_back(value); }

void Writer::boolean(bool value) { uint32(value ? 1 : 0); }

void Writer::int32(std::int32_t value) { uint32(static_cast<std::uint32_t>(value)); }

void Writer::uint32(std::uint32_t value)
{
    pad(m_bytes, 4);
    put32(m_bytes, value);
}

void Writer::string(std::string_view text)
{
    uint32(static_cast<std::uint32_t>(text.size()));
    m_bytes.insert(m_bytes.end(), text.begin(), text.end());
    m_bytes.push_back(0);
}

void Writer::signature(std::string_view text)
{
    m_bytes.push_back(static_cast<std::uint8_t>(text.size()));
    m_bytes.insert(m_bytes.end(), text.begin(), text.end());
    m_bytes.push_back(0);
}

void Writer::byte_array(std::span<std::uint8_t const> values)
{
    uint32(static_cast<std::uint32_t>(values.size()));
    m_bytes.insert(m_bytes.end(), values.begin(), values.end());
}

void Writer::align(std::size_t alignment) { pad(m_bytes, alignment); }

std::size_t Writer::begin_array(std::size_t element_alignment)
{
    pad(m_bytes, 4);
    std::size_t const length_at = m_bytes.size();
    put32(m_bytes, 0);
    // The padding before the first element is there even when the array is
    // empty, and is not counted in its length.
    pad(m_bytes, element_alignment);
    return length_at;
}

void Writer::end_array(std::size_t length_at, std::size_t element_alignment)
{
    std::size_t first = length_at + 4;
    while (first % element_alignment != 0)
        ++first;
    set32(m_bytes, length_at, static_cast<std::uint32_t>(m_bytes.size() - first));
}

void Writer::variant_string(std::string_view text)
{
    signature("s");
    string(text);
}

void Writer::variant_boolean(bool value)
{
    signature("b");
    boolean(value);
}

void Writer::variant_byte(std::uint8_t value)
{
    signature("y");
    byte(value);
}

void Writer::variant_uint32(std::uint32_t value)
{
    signature("u");
    uint32(value);
}

bool Reader::align(std::size_t alignment)
{
    while (m_at % alignment != 0)
        ++m_at;
    if (m_at > m_bytes.size())
        m_failed = true;
    return !m_failed;
}

bool Reader::skip(std::size_t count)
{
    if (m_failed || count > m_bytes.size() - m_at) {
        m_failed = true;
        return false;
    }
    m_at += count;
    return true;
}

std::optional<std::uint8_t> Reader::byte()
{
    if (m_failed || m_at >= m_bytes.size()) {
        m_failed = true;
        return std::nullopt;
    }
    return m_bytes[m_at++];
}

std::optional<std::uint32_t> Reader::uint32()
{
    if (m_failed || !align(4) || m_bytes.size() - m_at < 4) {
        m_failed = true;
        return std::nullopt;
    }
    std::uint32_t const value = get32(m_bytes, m_at);
    m_at += 4;
    return value;
}

std::optional<std::string> Reader::string()
{
    std::optional<std::uint32_t> const length = uint32();
    if (!length || *length >= m_bytes.size() - m_at) { // its bytes and the NUL after them
        m_failed = true;
        return std::nullopt;
    }
    std::string text(reinterpret_cast<char const*>(m_bytes.data() + m_at), *length);
    m_at += *length + 1;
    return text;
}

std::optional<std::string> Reader::signature()
{
    std::optional<std::uint8_t> const length = byte();
    if (!length || *length >= m_bytes.size() - m_at) {
        m_failed = true;
        return std::nullopt;
    }
    std::string text(reinterpret_cast<char const*>(m_bytes.data() + m_at), *length);
    m_at += static_cast<std::size_t>(*length) + 1;
    return text;
}

std::optional<std::vector<std::string>> Reader::string_array()
{
    std::optional<std::uint32_t> const length = uint32();
    if (!length || *length > longest_array || *length > m_bytes.size() - m_at) {
        m_failed = true;
        return std::nullopt;
    }
    std::size_t const end = m_at + *length;
    std::vector<std::string> strings;
    while (m_at < end) {
        std::optional<std::string> text = string();
        if (!text || m_at > end) {
            m_failed = true;
            return std::nullopt;
        }
        strings.push_back(std::move(*text));
    }
    return strings;
}

std::vector<std::uint8_t> encode(Message const& message)
{
    Writer header;
    header.byte('l');
    header.byte(static_cast<std::uint8_t>(message.type));
    header.byte(message.no_reply_expected ? 1 : 0);
    header.byte(1); // the protocol's version
    header.uint32(static_cast<std::uint32_t>(message.body.size()));
    header.uint32(message.serial);
    header.array(8, [&] {
        auto const text_field = [&header](Field field, std::string_view kind, std::string const& value) {
            if (value.empty())
                return;
            header.align(8);
            header.byte(field);
            header.signature(kind);
            if (kind == "g")
                header.signature(value);
            else
                header.string(value);
        };
        text_field(Path, "o", message.path);
        text_field(Interface, "s", message.interface);
        text_field(Member, "s", message.member);
        text_field(ErrorName, "s", message.error_name);
        if (message.reply_serial != 0) {
            header.align(8);
            header.byte(ReplySerial);
            header.variant_uint32(message.reply_serial);
        }
        text_field(Destination, "s", message.destination);
        text_field(Sender, "s", message.sender);
        text_field(Signature, "g", message.signature);
    });
    header.align(8);
    std::vector<std::uint8_t> bytes = header.take();
    bytes.insert(bytes.end(), message.body.begin(), message.body.end());
    return bytes;
}

std::optional<Message> decode(std::span<std::uint8_t const> bytes, std::size_t& used, bool& malformed)
{
    used = 0;
    malformed = false;
    if (bytes.size() < 16)
        return std::nullopt;
    if ((bytes[0] != 'l' && bytes[0] != 'B') || bytes[1] < 1 || bytes[1] > 4 || bytes[3] != 1) {
        malformed = true;
        return std::nullopt;
    }
    bool const little = bytes[0] == 'l';
    auto const number = [&](std::size_t at) {
        std::uint32_t const value = get32(bytes, at);
        return little ? value : __builtin_bswap32(value);
    };
    std::size_t const body_length = number(4);
    std::size_t const fields_length = number(12);
    if (body_length > longest_message || fields_length > longest_array) {
        malformed = true;
        return std::nullopt;
    }
    std::size_t header_length = 16 + fields_length;
    while (header_length % 8 != 0)
        ++header_length;
    std::size_t const whole = header_length + body_length;
    if (whole > longest_message) {
        malformed = true;
        return std::nullopt;
    }
    if (bytes.size() < whole)
        return std::nullopt;
    used = whole;
    // Every bus and every service on this machine writes in the machine's
    // order; a message in the other is passed over.
    if (!little)
        return std::nullopt;

    Message message;
    message.type = static_cast<MessageType>(bytes[1]);
    message.no_reply_expected = (bytes[2] & 1) != 0;
    message.serial = number(8);
    Reader fields(bytes.first(16 + fields_length));
    fields.skip(16);
    while (fields.position() < 16 + fields_length) {
        if (!fields.align(8))
            return std::nullopt;
        if (fields.position() >= 16 + fields_length)
            break;
        std::optional<std::uint8_t> const field = fields.byte();
        std::optional<std::string> const kind = fields.signature();
        if (!field || !kind)
            return std::nullopt;
        if (*kind == "s" || *kind == "o") {
            std::optional<std::string> value = fields.string();
            if (!value)
                return std::nullopt;
            switch (*field) {
            case Path: message.path = std::move(*value); break;
            case Interface: message.interface = std::move(*value); break;
            case Member: message.member = std::move(*value); break;
            case ErrorName: message.error_name = std::move(*value); break;
            case Destination: message.destination = std::move(*value); break;
            case Sender: message.sender = std::move(*value); break;
            default: break;
            }
        } else if (*kind == "g") {
            std::optional<std::string> value = fields.signature();
            if (!value)
                return std::nullopt;
            if (*field == Signature)
                message.signature = std::move(*value);
        } else if (*kind == "u") {
            std::optional<std::uint32_t> const value = fields.uint32();
            if (!value)
                return std::nullopt;
            if (*field == ReplySerial)
                message.reply_serial = *value;
        } else {
            // A field of a kind no header field has: the header cannot be
            // walked past it.
            return std::nullopt;
        }
    }
    message.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(header_length),
        bytes.begin() + static_cast<std::ptrdiff_t>(whole));
    return message;
}

std::optional<SocketAddress> parse_address(std::string_view address)
{
    while (!address.empty()) {
        std::size_t const end = address.find(';');
        std::string_view one = address.substr(0, end);
        address = end == std::string_view::npos ? std::string_view() : address.substr(end + 1);
        if (!one.starts_with("unix:"))
            continue;
        one.remove_prefix(5);
        while (!one.empty()) {
            std::size_t const comma = one.find(',');
            std::string_view const pair = one.substr(0, comma);
            one = comma == std::string_view::npos ? std::string_view() : one.substr(comma + 1);
            bool const abstract = pair.starts_with("abstract=");
            if (!abstract && !pair.starts_with("path="))
                continue;
            // A value's bytes outside the unreserved set are written %xx.
            std::string_view const escaped = pair.substr(pair.find('=') + 1);
            std::string value;
            for (std::size_t i = 0; i < escaped.size(); ++i) {
                auto const hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                if (escaped[i] == '%' && i + 2 < escaped.size() && hex(escaped[i + 1]) >= 0 && hex(escaped[i + 2]) >= 0) {
                    value += static_cast<char>(hex(escaped[i + 1]) * 16 + hex(escaped[i + 2]));
                    i += 2;
                } else {
                    value += escaped[i];
                }
            }
            if (!value.empty())
                return SocketAddress { std::move(value), abstract };
        }
    }
    return std::nullopt;
}

std::optional<SocketAddress> session_bus_address()
{
    if (char const* const address = std::getenv("DBUS_SESSION_BUS_ADDRESS"); address != nullptr && *address)
        return parse_address(address);
    if (char const* const runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime) {
        std::string path = std::string(runtime) + "/bus";
        if (access(path.c_str(), F_OK) == 0)
            return SocketAddress { std::move(path), false };
    }
    return std::nullopt;
}

struct Connection::Lock {
    std::mutex mutex;
};

namespace {

bool said(std::string* why_not, std::string reason)
{
    if (why_not != nullptr)
        *why_not = std::move(reason);
    return false;
}

bool write_all(int fd, std::uint8_t const* bytes, std::size_t count)
{
    while (count > 0) {
        ssize_t const wrote = send(fd, bytes, count, MSG_NOSIGNAL);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                pollfd waiting { fd, POLLOUT, 0 };
                poll(&waiting, 1, 1000);
                continue;
            }
            return false;
        }
        bytes += wrote;
        count -= static_cast<std::size_t>(wrote);
    }
    return true;
}

// A line of the handshake, up to its "\r\n", waiting at most `timeout_ms`.
std::optional<std::string> read_line(int fd, int timeout_ms)
{
    std::string line;
    while (line.size() < 4096) {
        pollfd waiting { fd, POLLIN, 0 };
        if (poll(&waiting, 1, timeout_ms) <= 0)
            return std::nullopt;
        char c = 0;
        ssize_t const got = recv(fd, &c, 1, 0);
        if (got <= 0)
            return std::nullopt;
        line += c;
        if (line.ends_with("\r\n")) {
            line.resize(line.size() - 2);
            return line;
        }
    }
    return std::nullopt;
}

}

std::unique_ptr<Connection> Connection::open(SocketAddress const& address, std::string* why_not)
{
    sockaddr_un name {};
    name.sun_family = AF_UNIX;
    if (address.path.size() + 1 >= sizeof name.sun_path) {
        said(why_not, "the bus's socket name is too long");
        return nullptr;
    }
    // An abstract name starts with a NUL and has no NUL after it.
    std::memcpy(name.sun_path + (address.abstract ? 1 : 0), address.path.data(), address.path.size());
    socklen_t const name_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address.path.size() + (address.abstract ? 1 : 0));
    int const fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        said(why_not, std::string("no socket: ") + std::strerror(errno));
        return nullptr;
    }
    std::unique_ptr<Connection> connection(new Connection);
    connection->m_fd = fd;
    connection->m_send_lock = std::make_unique<Lock>();
    if (connect(fd, reinterpret_cast<sockaddr const*>(&name), name_length) != 0) {
        said(why_not, "the bus at " + address.path + " cannot be reached: " + std::strerror(errno));
        return nullptr;
    }
    // The handshake: a NUL, then who this is — the user's number, as the
    // socket's own credentials will bear out — and the binary protocol begins.
    std::string const uid = std::to_string(getuid());
    std::string hex;
    for (char const c : uid) {
        static constexpr char digits[] = "0123456789abcdef";
        hex += digits[static_cast<unsigned char>(c) >> 4];
        hex += digits[static_cast<unsigned char>(c) & 15];
    }
    std::string const hello = std::string(1, '\0') + "AUTH EXTERNAL " + hex + "\r\n";
    if (!write_all(fd, reinterpret_cast<std::uint8_t const*>(hello.data()), hello.size())) {
        said(why_not, "the bus closed the socket");
        return nullptr;
    }
    std::optional<std::string> const answer = read_line(fd, 5000);
    if (!answer || !answer->starts_with("OK")) {
        said(why_not, "the bus did not accept this user" + (answer ? ": " + *answer : std::string()));
        return nullptr;
    }
    static constexpr char begin[] = "BEGIN\r\n";
    if (!write_all(fd, reinterpret_cast<std::uint8_t const*>(begin), sizeof begin - 1)) {
        said(why_not, "the bus closed the socket");
        return nullptr;
    }
    // Hello: the bus names the connection, and passes nothing on before.
    Message call;
    call.destination = "org.freedesktop.DBus";
    call.path = "/org/freedesktop/DBus";
    call.interface = "org.freedesktop.DBus";
    call.member = "Hello";
    std::uint32_t const serial = connection->send(std::move(call));
    for (int waited = 0; waited < 50 && serial != 0; ++waited) {
        std::optional<Message> const reply = connection->receive(100);
        if (connection->closed())
            break;
        if (!reply || reply->reply_serial != serial)
            continue;
        if (reply->type == MessageType::MethodReturn) {
            Reader body(reply->body);
            if (std::optional<std::string> name_given = body.string()) {
                connection->m_unique_name = std::move(*name_given);
                return connection;
            }
        }
        break;
    }
    said(why_not, "the bus did not answer the hello");
    return nullptr;
}

Connection::~Connection()
{
    if (m_fd >= 0)
        close(m_fd);
}

std::uint32_t Connection::send(Message message)
{
    std::lock_guard const lock(m_send_lock->mutex);
    if (m_closed)
        return 0;
    message.serial = m_next_serial++;
    if (m_next_serial == 0)
        m_next_serial = 1;
    std::vector<std::uint8_t> const bytes = encode(message);
    if (!write_all(m_fd, bytes.data(), bytes.size())) {
        m_closed = true;
        return 0;
    }
    return message.serial;
}

std::optional<Message> Connection::receive(int timeout_ms, int also_fd)
{
    for (;;) {
        std::size_t used = 0;
        bool malformed = false;
        std::optional<Message> message = decode(m_pending, used, malformed);
        if (malformed) {
            m_closed = true;
            return std::nullopt;
        }
        if (used != 0) {
            m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(used));
            if (message)
                return message;
            continue; // one that could not be read: the next
        }
        if (m_closed)
            return std::nullopt;
        pollfd waiting[2] = { { m_fd, POLLIN, 0 }, { also_fd, POLLIN, 0 } };
        int const ready = poll(waiting, also_fd >= 0 ? 2 : 1, timeout_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (waiting[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            return std::nullopt;
        std::uint8_t buffer[4096];
        ssize_t const got = recv(m_fd, buffer, sizeof buffer, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (got <= 0) {
            m_closed = true;
            return std::nullopt;
        }
        m_pending.insert(m_pending.end(), buffer, buffer + got);
    }
}

}
