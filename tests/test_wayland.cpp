// The Wayland wire client with a socketpair for a compositor: message
// framing in both directions, the argument types, descriptors carried as
// SCM_RIGHTS both ways, object ids recycled on delete_id, protocol errors
// and a closed socket. No display is needed, so this runs in CI.
#include "Test.h"

#include "platform/linux/Wayland.h"

#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

using namespace sashfold;
using namespace sashfold::platform::wayland;

namespace {

std::uint32_t word_at(std::vector<std::uint8_t> const& bytes, std::size_t index)
{
    std::uint32_t word = 0;
    std::memcpy(&word, bytes.data() + index * 4, 4);
    return word;
}

void put_word(std::vector<std::uint8_t>& out, std::uint32_t word)
{
    std::uint8_t bytes[4];
    std::memcpy(bytes, &word, 4);
    out.insert(out.end(), bytes, bytes + 4);
}

void put_string(std::vector<std::uint8_t>& out, std::string const& text)
{
    put_word(out, static_cast<std::uint32_t>(text.size() + 1));
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
    while (out.size() % 4 != 0)
        out.push_back(0);
}

// An event as the compositor would frame it.
std::vector<std::uint8_t> event(std::uint32_t object, std::uint16_t opcode, std::vector<std::uint8_t> const& args)
{
    std::vector<std::uint8_t> out;
    put_word(out, object);
    put_word(out, (static_cast<std::uint32_t>(8 + args.size()) << 16) | opcode);
    out.insert(out.end(), args.begin(), args.end());
    return out;
}

// The control-message layout by hand, as the client does it.
constexpr std::size_t cmsg_align(std::size_t length)
{
    return (length + sizeof(std::size_t) - 1) & ~(sizeof(std::size_t) - 1);
}
constexpr std::size_t cmsg_header_size = cmsg_align(sizeof(cmsghdr));

bool send_with_fd(int socket, std::vector<std::uint8_t> const& bytes, int fd)
{
    iovec vector {};
    vector.iov_base = const_cast<std::uint8_t*>(bytes.data());
    vector.iov_len = bytes.size();
    alignas(cmsghdr) std::uint8_t control[cmsg_header_size + cmsg_align(sizeof(int))] = {};
    auto* const cmsg = reinterpret_cast<cmsghdr*>(control);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = cmsg_header_size + sizeof(int);
    std::memcpy(control + cmsg_header_size, &fd, sizeof fd);
    msghdr header {};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof control;
    return ::sendmsg(socket, &header, 0) == static_cast<ssize_t>(bytes.size());
}

// Reads what the client sent, with any descriptor that rode along.
std::vector<std::uint8_t> receive(int socket, int* fd_out = nullptr)
{
    std::uint8_t chunk[4096];
    iovec vector {};
    vector.iov_base = chunk;
    vector.iov_len = sizeof chunk;
    alignas(cmsghdr) std::uint8_t control[cmsg_header_size + cmsg_align(4 * sizeof(int))] = {};
    msghdr header {};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof control;
    ssize_t const count = ::recvmsg(socket, &header, MSG_DONTWAIT);
    if (fd_out) {
        *fd_out = -1;
        if (header.msg_controllen >= cmsg_header_size + sizeof(int)) {
            auto const* const cmsg = reinterpret_cast<cmsghdr const*>(control);
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
                std::memcpy(fd_out, control + cmsg_header_size, sizeof(int));
        }
    }
    if (count <= 0)
        return {};
    return std::vector<std::uint8_t>(chunk, chunk + count);
}

void test_request_encoding()
{
    Request request(3, 1);
    request.uint(7).string("hi").int_(-2).fixed(1.5).new_id(9);
    std::vector<std::uint8_t> const& bytes = request.bytes();
    CHECK_EQ(bytes.size(), 32u);
    CHECK_EQ(word_at(bytes, 0), 3u);
    CHECK_EQ(word_at(bytes, 1), (32u << 16) | 1u);
    CHECK_EQ(word_at(bytes, 2), 7u);
    CHECK_EQ(word_at(bytes, 3), 3u); // "hi" plus its NUL
    CHECK_EQ(bytes[16], static_cast<std::uint8_t>('h'));
    CHECK_EQ(bytes[17], static_cast<std::uint8_t>('i'));
    CHECK_EQ(bytes[18], 0u);
    CHECK_EQ(bytes[19], 0u); // padding
    CHECK_EQ(static_cast<std::int32_t>(word_at(bytes, 5)), -2);
    CHECK_EQ(word_at(bytes, 6), 384u); // 1.5 in 24.8
    CHECK_EQ(word_at(bytes, 7), 9u);

    Request empty(1, 0);
    empty.string("");
    CHECK_EQ(empty.bytes().size(), 16u); // a word for the length 1, the NUL, three of padding
    CHECK_EQ(word_at(empty.bytes(), 2), 1u);

    Request with_array(1, 0);
    std::uint8_t const payload[5] = { 1, 2, 3, 4, 5 };
    with_array.array(payload);
    CHECK_EQ(with_array.bytes().size(), 8u + 4u + 8u);
    CHECK_EQ(word_at(with_array.bytes(), 2), 5u);
}

void test_message_decoding()
{
    std::vector<std::uint8_t> payload;
    put_word(payload, 42);
    put_word(payload, static_cast<std::uint32_t>(-7));
    put_word(payload, 640); // 2.5
    put_string(payload, "wl_seat");
    put_word(payload, 3);
    payload.push_back(9);
    payload.push_back(8);
    payload.push_back(7);
    payload.push_back(0);
    Message message(payload, -1);
    CHECK_EQ(message.uint(), 42u);
    CHECK_EQ(message.int_(), -7);
    CHECK_EQ(message.fixed(), 2.5);
    CHECK_EQ(message.string(), std::string("wl_seat"));
    std::span<std::uint8_t const> const array = message.array();
    CHECK_EQ(array.size(), 3u);
    if (array.size() == 3)
        CHECK_EQ(array[2], 7u);
    CHECK(message.ok());
    CHECK_EQ(message.remaining(), 0u);
    CHECK_EQ(message.uint(), 0u); // past the end: zero, and no longer ok
    CHECK(!message.ok());
    CHECK_EQ(message.fd(), -1);

    std::vector<std::uint8_t> null_string;
    put_word(null_string, 0);
    Message nulled(null_string, -1);
    CHECK_EQ(nulled.string(), std::string());
    CHECK(nulled.ok());

    std::vector<std::uint8_t> short_string;
    put_word(short_string, 100); // claims 100 bytes it does not have
    Message truncated(short_string, -1);
    CHECK_EQ(truncated.string(), std::string());
    CHECK(!truncated.ok());
}

void test_connection()
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        CHECK(false);
        return;
    }
    int const compositor = sockets[1];
    std::unique_ptr<Connection> client = Connection::adopt(sockets[0]);
    CHECK(client != nullptr);

    // A request reaches the compositor framed.
    std::uint32_t const registry = client->allocate_id();
    CHECK_EQ(registry, 2u);
    Request get_registry(Connection::display_id, 1);
    get_registry.new_id(registry);
    client->send(get_registry);
    CHECK(client->flush());
    std::vector<std::uint8_t> const sent = receive(compositor);
    CHECK_EQ(sent.size(), 12u);
    if (sent.size() == 12) {
        CHECK_EQ(word_at(sent, 0), 1u);
        CHECK_EQ(word_at(sent, 1), (12u << 16) | 1u);
        CHECK_EQ(word_at(sent, 2), 2u);
    }

    // An event reaches its listener parsed.
    std::string seen_interface;
    std::uint32_t seen_name = 0;
    std::uint32_t seen_version = 0;
    int calls = 0;
    client->listen(registry, [&](std::uint16_t opcode, Message& message) {
        ++calls;
        if (opcode == 0) {
            seen_name = message.uint();
            seen_interface = message.string();
            seen_version = message.uint();
        }
    });
    std::vector<std::uint8_t> args;
    put_word(args, 5);
    put_string(args, "wl_compositor");
    put_word(args, 6);
    std::vector<std::uint8_t> const global = event(registry, 0, args);
    // Two events in one write, the second for an object nobody listens to.
    std::vector<std::uint8_t> batch = global;
    std::vector<std::uint8_t> const stray = event(77, 3, {});
    batch.insert(batch.end(), stray.begin(), stray.end());
    CHECK_EQ(::write(compositor, batch.data(), batch.size()), static_cast<ssize_t>(batch.size()));
    CHECK_EQ(client->dispatch(100), 2);
    CHECK_EQ(calls, 1);
    CHECK_EQ(seen_name, 5u);
    CHECK_EQ(seen_interface, std::string("wl_compositor"));
    CHECK_EQ(seen_version, 6u);
    CHECK_EQ(client->dispatch_pending(), 0);

    // A descriptor from the compositor lands on the event that carries it.
    std::uint32_t const keyboard = client->allocate_id();
    CHECK_EQ(keyboard, 3u);
    int received_fd = -1;
    std::uint32_t received_size = 0;
    client->listen(
        keyboard,
        [&](std::uint16_t opcode, Message& message) {
            if (opcode == 0) {
                message.uint(); // format
                received_fd = message.fd();
                received_size = message.uint();
            }
        },
        { 0 });
    int pipe_fds[2];
    CHECK_EQ(::pipe(pipe_fds), 0);
    CHECK_EQ(::write(pipe_fds[1], "keymap", 6), 6);
    std::vector<std::uint8_t> keymap_args;
    put_word(keymap_args, 1);
    put_word(keymap_args, 7);
    CHECK(send_with_fd(compositor, event(keyboard, 0, keymap_args), pipe_fds[0]));
    ::close(pipe_fds[0]);
    CHECK_EQ(client->dispatch(100), 1);
    CHECK(received_fd >= 0);
    CHECK_EQ(received_size, 7u);
    if (received_fd >= 0) {
        char text[16] = {};
        CHECK_EQ(::read(received_fd, text, sizeof text), 6);
        CHECK_EQ(std::string(text), std::string("keymap"));
        ::close(received_fd);
    }
    ::close(pipe_fds[1]);

    // A descriptor from the client travels beside its request.
    int pool_fds[2];
    CHECK_EQ(::pipe(pool_fds), 0);
    CHECK_EQ(::write(pool_fds[1], "pool", 4), 4);
    Request create_pool(3, 0);
    create_pool.new_id(4).fd(pool_fds[0]).int_(4096);
    client->send(create_pool);
    CHECK(client->flush());
    int arrived_fd = -1;
    std::vector<std::uint8_t> const pool_request = receive(compositor, &arrived_fd);
    CHECK_EQ(pool_request.size(), 16u);
    CHECK(arrived_fd >= 0);
    if (arrived_fd >= 0) {
        char text[16] = {};
        CHECK_EQ(::read(arrived_fd, text, sizeof text), 4);
        CHECK_EQ(std::string(text), std::string("pool"));
        ::close(arrived_fd);
    }
    ::close(pool_fds[0]);
    ::close(pool_fds[1]);

    // delete_id recycles the id; the listener is gone with it.
    std::vector<std::uint8_t> delete_args;
    put_word(delete_args, registry);
    std::vector<std::uint8_t> const deleted = event(Connection::display_id, 1, delete_args);
    CHECK_EQ(::write(compositor, deleted.data(), deleted.size()), static_cast<ssize_t>(deleted.size()));
    CHECK_EQ(client->dispatch(100), 1);
    CHECK_EQ(client->allocate_id(), registry);
    CHECK_EQ(::write(compositor, global.data(), global.size()), static_cast<ssize_t>(global.size()));
    CHECK_EQ(client->dispatch(100), 1);
    CHECK_EQ(calls, 1); // not called again

    // A roundtrip: the sync callback takes the next id, which the test answers.
    std::uint32_t const next_id = client->allocate_id();
    std::vector<std::uint8_t> done_args;
    put_word(done_args, 0);
    std::vector<std::uint8_t> const done = event(next_id + 1, 0, done_args);
    CHECK_EQ(::write(compositor, done.data(), done.size()), static_cast<ssize_t>(done.size()));
    CHECK(client->roundtrip());
    std::vector<std::uint8_t> const sync = receive(compositor);
    CHECK_EQ(sync.size(), 12u);
    if (sync.size() == 12) {
        CHECK_EQ(word_at(sync, 1), (12u << 16) | 0u);
        CHECK_EQ(word_at(sync, 2), next_id + 1);
    }

    // A protocol error fails the connection with the compositor's words.
    std::vector<std::uint8_t> error_args;
    put_word(error_args, 3);
    put_word(error_args, 1);
    put_string(error_args, "bad surface");
    std::vector<std::uint8_t> const error = event(Connection::display_id, 0, error_args);
    CHECK_EQ(::write(compositor, error.data(), error.size()), static_cast<ssize_t>(error.size()));
    CHECK(!client->failed());
    CHECK_EQ(client->dispatch(100), -1);
    CHECK(client->failed());
    CHECK(client->error().find("bad surface") != std::string::npos);
    CHECK(client->error().find("object 3") != std::string::npos);
    ::close(compositor);
}

void test_closed_socket()
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        CHECK(false);
        return;
    }
    std::unique_ptr<Connection> client = Connection::adopt(sockets[0]);
    ::close(sockets[1]);
    CHECK_EQ(client->dispatch(100), -1);
    CHECK(client->failed());
    CHECK(client->error().find("closed") != std::string::npos);
    CHECK(!client->roundtrip());
}

} // namespace

int main()
{
    test_request_encoding();
    test_message_decoding();
    test_connection();
    test_closed_socket();
    return test::report("test_wayland");
}
