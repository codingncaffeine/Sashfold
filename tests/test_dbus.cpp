#include "Test.h"

#include "platform/Notifier.h"

#if defined(__linux__)
#include "platform/linux/DBus.h"
#include "platform/linux/NotifierDBus.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>
#endif

using namespace sashfold;

#if defined(__linux__)
namespace {

using Bytes = std::vector<std::uint8_t>;
namespace dbus = platform::dbus;

Bytes bytes_of(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

// --- The wire format ---------------------------------------------------------

// The first message every client sends, written out by hand from the
// specification's layout — not by the encoder under test — in the order of
// fields the encoder uses: path, interface, member, destination.
Bytes hello_by_hand()
{
    using namespace std::string_view_literals;
    Bytes bytes = bytes_of("l\x01\x00\x01"sv); // little-endian, a method call, no flags, version 1
    auto const append = [&bytes](std::string_view more) { bytes.insert(bytes.end(), more.begin(), more.end()); };
    append("\x00\x00\x00\x00"sv); // no body
    append("\x01\x00\x00\x00"sv); // serial 1
    append("\x6d\x00\x00\x00"sv); // the fields: 109 bytes
    append("\x01\x01o\x00\x15\x00\x00\x00/org/freedesktop/DBus\x00\x00\x00"sv); // 30 bytes, 2 of padding
    append("\x02\x01s\x00\x14\x00\x00\x00org.freedesktop.DBus\x00\x00\x00\x00"sv); // 29, 3
    append("\x03\x01s\x00\x05\x00\x00\x00Hello\x00\x00\x00"sv); // 14, 2
    append("\x06\x01s\x00\x14\x00\x00\x00org.freedesktop.DBus\x00"sv); // 29: the array ends here
    append("\x00\x00\x00"sv); // the header ends on a multiple of eight
    return bytes;
}

// The same call as the reference library writes it: path, destination,
// interface, member — 110 bytes of fields.
Bytes hello_as_libdbus_writes_it()
{
    using namespace std::string_view_literals;
    Bytes bytes = bytes_of("l\x01\x00\x01\x00\x00\x00\x00\x01\x00\x00\x00\x6e\x00\x00\x00"sv);
    auto const append = [&bytes](std::string_view more) { bytes.insert(bytes.end(), more.begin(), more.end()); };
    append("\x01\x01o\x00\x15\x00\x00\x00/org/freedesktop/DBus\x00\x00\x00"sv);
    append("\x06\x01s\x00\x14\x00\x00\x00org.freedesktop.DBus\x00\x00\x00\x00"sv);
    append("\x02\x01s\x00\x14\x00\x00\x00org.freedesktop.DBus\x00\x00\x00\x00"sv);
    append("\x03\x01s\x00\x05\x00\x00\x00Hello\x00\x00\x00"sv);
    return bytes;
}

void test_wire()
{
    dbus::Message hello;
    hello.serial = 1;
    hello.destination = "org.freedesktop.DBus";
    hello.path = "/org/freedesktop/DBus";
    hello.interface = "org.freedesktop.DBus";
    hello.member = "Hello";
    CHECK(dbus::encode(hello) == hello_by_hand());
    CHECK_EQ(dbus::encode(hello).size(), 128u);

    std::size_t used = 0;
    bool malformed = false;
    Bytes const libdbus = hello_as_libdbus_writes_it();
    std::optional<dbus::Message> const read = dbus::decode(libdbus, used, malformed);
    CHECK(read.has_value());
    CHECK_EQ(used, libdbus.size());
    CHECK(!malformed);
    if (read) {
        CHECK(read->type == dbus::MessageType::MethodCall);
        CHECK_EQ(read->serial, 1u);
        CHECK_EQ(read->path, std::string("/org/freedesktop/DBus"));
        CHECK_EQ(read->destination, std::string("org.freedesktop.DBus"));
        CHECK_EQ(read->interface, std::string("org.freedesktop.DBus"));
        CHECK_EQ(read->member, std::string("Hello"));
        CHECK(read->body.empty());
    }

    // A body: Notify's arguments, each at its alignment. By hand: the
    // string "ab" is its length, its bytes and a NUL (7 bytes), the number
    // after it starts on the next multiple of four.
    dbus::Writer body;
    body.string("ab");
    body.uint32(7);
    body.array(4, [&] {
        body.string("x");
        body.string("yz");
    });
    body.array(8, [&] {
        body.align(8);
        body.string("k");
        body.variant_boolean(true);
    });
    body.int32(-1);
    using namespace std::string_view_literals;
    Bytes const by_hand = bytes_of(
        "\x02\x00\x00\x00" "ab\x00\x00" // "ab", padded to 8
        "\x07\x00\x00\x00" // 7
        "\x0f\x00\x00\x00" // the strings: 15 bytes from here
        "\x01\x00\x00\x00x\x00\x00\x00" // "x", padded for the next length
        "\x02\x00\x00\x00yz\x00" // "yz": the array ends at 31
        "\x00" // to the next multiple of four, for the dictionary's length
        "\x10\x00\x00\x00" // one entry: 16 bytes, from 40
        "\x00\x00\x00\x00" // the first entry starts on a multiple of eight
        "\x01\x00\x00\x00k\x00" // "k": 40 to 45
        "\x01" "b\x00" // a variant of signature "b": 46 to 48
        "\x00\x00\x00" // its value starts on a multiple of four
        "\x01\x00\x00\x00" // true
        "\xff\xff\xff\xff"sv); // -1
    CHECK(body.bytes() == by_hand);

    // A message with a body round-trips, and two in one buffer are taken
    // one at a time.
    dbus::Message signal;
    signal.type = dbus::MessageType::Signal;
    signal.serial = 9;
    signal.path = "/org/freedesktop/Notifications";
    signal.interface = "org.freedesktop.Notifications";
    signal.member = "ActionInvoked";
    signal.sender = ":1.5";
    signal.signature = "us";
    dbus::Writer arguments;
    arguments.uint32(42);
    arguments.string("default");
    signal.body = arguments.take();
    Bytes both = dbus::encode(signal);
    std::size_t const first_size = both.size();
    Bytes const second = dbus::encode(hello);
    both.insert(both.end(), second.begin(), second.end());
    std::optional<dbus::Message> const first = dbus::decode(both, used, malformed);
    CHECK(first.has_value());
    CHECK_EQ(used, first_size);
    if (first) {
        CHECK(first->type == dbus::MessageType::Signal);
        CHECK_EQ(first->member, std::string("ActionInvoked"));
        CHECK_EQ(first->sender, std::string(":1.5"));
        CHECK_EQ(first->signature, std::string("us"));
        dbus::Reader reader(first->body);
        CHECK_EQ(reader.uint32().value_or(0), 42u);
        CHECK_EQ(reader.string().value_or(""), std::string("default"));
        CHECK(!reader.failed());
        CHECK(!reader.uint32().has_value()); // nothing after it
        CHECK(reader.failed());
    }
    // Part of a message is not yet one; bytes that are no message at all
    // are said to be so.
    for (std::size_t cut : { std::size_t { 0 }, std::size_t { 15 }, std::size_t { 16 }, first_size - 1 }) {
        std::optional<dbus::Message> const partial = dbus::decode(std::span<std::uint8_t const>(both.data(), cut), used, malformed);
        CHECK(!partial.has_value());
        CHECK_EQ(used, 0u);
        CHECK(!malformed);
    }
    Bytes const garbage = bytes_of("HTTP/1.1 400 Bad Request\r\n\r\n");
    CHECK(!dbus::decode(garbage, used, malformed).has_value());
    CHECK(malformed);
    // A length that runs past the body is a read that fails, not a read
    // past the end.
    Bytes const liar = bytes_of("\xff\xff\xff\x7f" "ab"sv);
    dbus::Reader lying(liar);
    CHECK(!lying.string().has_value());
    CHECK(lying.failed());
    dbus::Reader lying_array(liar);
    CHECK(!lying_array.string_array().has_value());
}

void test_addresses()
{
    std::optional<dbus::SocketAddress> address = dbus::parse_address("unix:path=/run/user/1000/bus");
    CHECK(address && address->path == "/run/user/1000/bus" && !address->abstract);
    address = dbus::parse_address("unix:path=/run/user/1000/bus,guid=0123456789abcdef");
    CHECK(address && address->path == "/run/user/1000/bus");
    address = dbus::parse_address("unix:abstract=/tmp/dbus-AbCd,guid=01");
    CHECK(address && address->path == "/tmp/dbus-AbCd" && address->abstract);
    address = dbus::parse_address("tcp:host=localhost,port=1;unix:path=/a%20b/bus");
    CHECK(address && address->path == "/a b/bus");
    CHECK(!dbus::parse_address("tcp:host=localhost,port=1").has_value());
    CHECK(!dbus::parse_address("").has_value());
    CHECK(!dbus::parse_address("unix:tmpdir=/tmp").has_value());
}

// --- A bus and a notification service of the test's own -------------------------
//
// One listening socket in the test's folder; one client at a time. It
// speaks the handshake and then, in one, both the bus (Hello, AddMatch)
// and the service (GetCapabilities, Notify, CloseNotification), and sends
// the service's signals when the test says.

struct Notified {
    std::string application;
    std::uint32_t replaces = 0;
    std::string icon;
    std::string summary;
    std::string body;
    std::vector<std::string> actions;
    std::int32_t timeout = 0;
    std::uint32_t given = 0;
    std::vector<std::string> hints; // by name, in order
    // The "image-data" hint, as read: width, height, row, alpha, bits,
    // channels, the pixels' length, and the first pixel's four bytes.
    std::vector<std::uint32_t> image;
};

class FakeBus {
public:
    explicit FakeBus(std::string path, bool markup = true, bool refuse_notify = false)
        : m_path(std::move(path))
        , m_markup(markup)
        , m_refuse_notify(refuse_notify)
    {
        ::unlink(m_path.c_str());
        m_listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un name {};
        name.sun_family = AF_UNIX;
        std::strncpy(name.sun_path, m_path.c_str(), sizeof name.sun_path - 1);
        m_ok = m_listener >= 0 && bind(m_listener, reinterpret_cast<sockaddr const*>(&name), sizeof name) == 0 && listen(m_listener, 1) == 0;
        m_thread = std::thread([this] { serve(); });
    }

    ~FakeBus()
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
        if (m_listener >= 0)
            ::close(m_listener);
        ::unlink(m_path.c_str());
    }

    bool ok() const { return m_ok; }

    // What has been asked so far.
    std::vector<Notified> notified() const
    {
        std::lock_guard const lock(m_mutex);
        return m_notified;
    }
    std::vector<std::uint32_t> closed() const
    {
        std::lock_guard const lock(m_mutex);
        return m_closed;
    }
    std::string match_rule() const
    {
        std::lock_guard const lock(m_mutex);
        return m_match_rule;
    }
    std::string credentials() const
    {
        std::lock_guard const lock(m_mutex);
        return m_credentials;
    }

    // The service's signals, sent when the connection's thread next looks.
    void press(std::uint32_t number) { emit("ActionInvoked", number, 0, true); }
    // A press as version 1.2 of the specification has it: the window's
    // token first, then the action.
    void press_with_token(std::uint32_t number, std::string token)
    {
        std::lock_guard const lock(m_mutex);
        m_emissions.push_back(Emission { "ActivationToken", number, 0, true, std::move(token) });
        m_emissions.push_back(Emission { "ActionInvoked", number, 0, true, {} });
    }
    void dismiss(std::uint32_t number) { emit("NotificationClosed", number, 2, false); }
    // A signal about a notice from a sender that is not the service.
    void hang_up() { m_hang_up = true; }

private:
    struct Emission {
        std::string member;
        std::uint32_t number;
        std::uint32_t reason;
        bool action;
        std::string text = {}; // a token's, in place of the action's name
    };

    void emit(std::string member, std::uint32_t number, std::uint32_t reason, bool action)
    {
        std::lock_guard const lock(m_mutex);
        m_emissions.push_back(Emission { std::move(member), number, reason, action, {} });
    }

    static bool send_all(int fd, Bytes const& bytes)
    {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            ssize_t const wrote = send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (wrote <= 0)
                return false;
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    void reply(int fd, dbus::Message const& to, std::string signature, Bytes body, bool error = false)
    {
        dbus::Message message;
        message.type = error ? dbus::MessageType::Error : dbus::MessageType::MethodReturn;
        if (error)
            message.error_name = "org.freedesktop.DBus.Error.Failed";
        message.serial = ++m_serial;
        message.reply_serial = to.serial;
        message.sender = "org.freedesktop.DBus";
        message.signature = std::move(signature);
        message.body = std::move(body);
        send_all(fd, dbus::encode(message));
    }

    void serve()
    {
        while (!m_stop) {
            pollfd waiting { m_listener, POLLIN, 0 };
            if (poll(&waiting, 1, 20) <= 0)
                continue;
            int const fd = accept(m_listener, nullptr, nullptr);
            if (fd < 0)
                continue;
            serve_one(fd);
            ::close(fd);
        }
    }

    void serve_one(int fd)
    {
        // The handshake, a line at a time.
        std::string text;
        bool begun = false;
        Bytes pending;
        while (!m_stop && !begun) {
            pollfd waiting { fd, POLLIN, 0 };
            if (poll(&waiting, 1, 20) <= 0)
                continue;
            char buffer[512];
            ssize_t const got = recv(fd, buffer, sizeof buffer, 0);
            if (got <= 0)
                return;
            text.append(buffer, static_cast<std::size_t>(got));
            for (std::size_t end = text.find("\r\n"); end != std::string::npos; end = text.find("\r\n")) {
                std::string const line = text.substr(0, end);
                text.erase(0, end + 2);
                if (line.find("AUTH EXTERNAL ") != std::string::npos) {
                    {
                        std::lock_guard const lock(m_mutex);
                        m_credentials = line;
                    }
                    send_all(fd, bytes_of("OK 0123456789abcdef0123456789abcdef\r\n"));
                } else if (line == "BEGIN") {
                    begun = true;
                    pending.assign(text.begin(), text.end());
                    break;
                } else {
                    send_all(fd, bytes_of("ERROR\r\n"));
                }
            }
        }
        std::uint32_t next_number = 100;
        while (!m_stop) {
            if (m_hang_up.exchange(false))
                return;
            std::vector<Emission> emissions;
            {
                std::lock_guard const lock(m_mutex);
                emissions.swap(m_emissions);
            }
            for (Emission const& emission : emissions) {
                dbus::Message signal;
                signal.type = dbus::MessageType::Signal;
                signal.serial = ++m_serial;
                signal.path = "/org/freedesktop/Notifications";
                signal.interface = "org.freedesktop.Notifications";
                signal.member = emission.member;
                signal.sender = ":1.1";
                signal.signature = emission.action ? "us" : "uu";
                dbus::Writer arguments;
                arguments.uint32(emission.number);
                if (emission.action)
                    arguments.string(emission.text.empty() ? std::string("default") : emission.text);
                else
                    arguments.uint32(emission.reason);
                signal.body = arguments.take();
                send_all(fd, dbus::encode(signal));
            }
            std::size_t used = 0;
            bool malformed = false;
            std::optional<dbus::Message> const message = dbus::decode(pending, used, malformed);
            if (malformed)
                return;
            if (used == 0) {
                pollfd waiting { fd, POLLIN, 0 };
                if (poll(&waiting, 1, 10) <= 0)
                    continue;
                std::uint8_t buffer[4096];
                ssize_t const got = recv(fd, buffer, sizeof buffer, 0);
                if (got <= 0)
                    return;
                pending.insert(pending.end(), buffer, buffer + got);
                continue;
            }
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(used));
            if (!message)
                continue;
            dbus::Reader body(message->body);
            if (message->member == "Hello") {
                dbus::Writer name;
                name.string(":1.7");
                reply(fd, *message, "s", name.take());
            } else if (message->member == "AddMatch") {
                std::lock_guard const lock(m_mutex);
                m_match_rule = body.string().value_or("");
            } else if (message->member == "GetCapabilities") {
                dbus::Writer list;
                list.array(4, [&] {
                    list.string("body");
                    list.string("actions");
                    if (m_markup)
                        list.string("body-markup");
                });
                reply(fd, *message, "as", list.take());
            } else if (message->member == "Notify") {
                Notified notified;
                notified.application = body.string().value_or("?");
                notified.replaces = body.uint32().value_or(999);
                notified.icon = body.string().value_or("?");
                notified.summary = body.string().value_or("?");
                notified.body = body.string().value_or("?");
                notified.actions = body.string_array().value_or(std::vector<std::string> { "?" });
                // The hints: an array of entries, each read by its signature.
                std::uint32_t const hints = body.uint32().value_or(0);
                body.align(8);
                std::size_t const hints_end = body.position() + hints;
                while (!body.failed() && body.position() < hints_end) {
                    body.align(8);
                    std::string const name = body.string().value_or("?");
                    std::string const type = body.signature().value_or("?");
                    notified.hints.push_back(name);
                    if (type == "s") {
                        body.string();
                    } else if (type == "b" || type == "u" || type == "i") {
                        body.uint32();
                    } else if (type == "y") {
                        body.byte();
                    } else if (type == "(iiibiiay)") {
                        body.align(8);
                        for (int field = 0; field < 6; ++field)
                            notified.image.push_back(body.uint32().value_or(0));
                        std::uint32_t const length = body.uint32().value_or(0);
                        notified.image.push_back(length);
                        for (std::uint32_t at = 0; at < length; ++at) {
                            std::uint8_t const value = body.byte().value_or(0);
                            if (at < 4)
                                notified.image.push_back(value);
                        }
                    } else {
                        notified.hints.push_back("UNREAD " + type);
                        body.skip(hints_end - body.position());
                    }
                }
                notified.timeout = static_cast<std::int32_t>(body.uint32().value_or(77));
                if (body.failed() || message->signature != "susssasa{sv}i")
                    notified.summary = "UNREADABLE";
                if (m_refuse_notify) {
                    dbus::Writer why;
                    why.string("no");
                    reply(fd, *message, "s", why.take(), true);
                    continue;
                }
                notified.given = notified.replaces != 0 ? notified.replaces : next_number++;
                dbus::Writer number;
                number.uint32(notified.given);
                {
                    std::lock_guard const lock(m_mutex);
                    m_notified.push_back(notified);
                }
                reply(fd, *message, "u", number.take());
            } else if (message->member == "CloseNotification") {
                std::uint32_t const number = body.uint32().value_or(0);
                {
                    std::lock_guard const lock(m_mutex);
                    m_closed.push_back(number);
                }
                // The service says a notice it was told to close is closed.
                emit("NotificationClosed", number, 3, false);
            }
        }
    }

    std::string m_path;
    bool m_markup;
    bool m_refuse_notify;
    int m_listener = -1;
    bool m_ok = false;
    std::atomic<bool> m_stop { false };
    std::atomic<bool> m_hang_up { false };
    std::uint32_t m_serial = 0;
    mutable std::mutex m_mutex;
    std::vector<Notified> m_notified;
    std::vector<std::uint32_t> m_closed;
    std::vector<Emission> m_emissions;
    std::string m_match_rule;
    std::string m_credentials;
    std::thread m_thread;
};

// Waits, up to five seconds, for `ready` — on the thing waited for, never
// on a length of time.
template <typename Ready>
bool eventually(Ready&& ready)
{
    auto const give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < give_up) {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
}

// The notifier's events as they come, kept in order.
struct Heard {
    platform::Notifier& notifier;
    std::vector<platform::Notifier::Event> events;

    bool has(platform::Notifier::Event::Kind kind, std::uint64_t id)
    {
        for (platform::Notifier::Event const& event : notifier.take_events())
            events.push_back(event);
        for (platform::Notifier::Event const& event : events) {
            if (event.kind == kind && event.id == id)
                return true;
        }
        return false;
    }
};

// A socket's name holds some hundred bytes and a build folder's path can
// be longer than that by itself: the names are relative to the folder the
// test runs in.
std::string scratch_socket(char const* name)
{
    std::error_code error;
    std::filesystem::create_directories("dbus-scratch", error);
    return std::string("dbus-scratch/") + name;
}

void test_notifier()
{
    using Kind = platform::Notifier::Event::Kind;
    std::string const path = scratch_socket("bus");
    FakeBus bus(path);
    CHECK(bus.ok());
    std::atomic<int> wakes { 0 };
    std::unique_ptr<platform::Notifier> const notifier
        = platform::open_notifier_on(dbus::SocketAddress { path, false }, "Sashfold", [&wakes] { ++wakes; });
    CHECK(notifier != nullptr);
    // Not known to be absent, from the first moment; and a notice asked for
    // before the service has been reached waits for it — shown once it
    // answers, written as that answer says a body is to be written.
    CHECK(notifier->available());
    Heard heard { *notifier, {} };
    platform::Notice notice;
    notice.title = "New message";
    notice.body = "Tom & <b>Jerry</b>";
    notice.site = "chat.example";
    std::uint64_t const first = notifier->show(notice);
    CHECK(first != 0);
    CHECK(eventually([&] { return heard.has(Kind::Shown, first); }));
    CHECK(wakes > 0);
    CHECK(notifier->available());
    // Who it said it was: this user's number, in hexadecimal digits of its
    // decimal digits.
    std::string hex;
    for (char const c : std::to_string(getuid())) {
        static constexpr char digits[] = "0123456789abcdef";
        hex += digits[static_cast<unsigned char>(c) >> 4];
        hex += digits[static_cast<unsigned char>(c) & 15];
    }
    CHECK_EQ(bus.credentials(), std::string(1, '\0') + "AUTH EXTERNAL " + hex);
    // It asked for the service's signals, and for no one else's.
    CHECK_EQ(bus.match_rule(), std::string("type='signal',sender='org.freedesktop.Notifications',interface='org.freedesktop.Notifications',path='/org/freedesktop/Notifications'"));
    std::vector<Notified> notified = bus.notified();
    CHECK_EQ(notified.size(), 1u);
    if (notified.size() == 1) {
        CHECK_EQ(notified[0].application, std::string("Sashfold"));
        CHECK_EQ(notified[0].replaces, 0u);
        CHECK_EQ(notified[0].icon, std::string("sashfold"));
        CHECK_EQ(notified[0].summary, std::string("New message"));
        // The site first, and the page's text as text: no tag of its own.
        CHECK_EQ(notified[0].body, std::string("chat.example\nTom &amp; &lt;b&gt;Jerry&lt;/b&gt;"));
        CHECK(notified[0].actions == (std::vector<std::string> { "default", "Show" }));
        CHECK_EQ(notified[0].timeout, -1);
        CHECK_EQ(notified[0].given, 100u);
        // No picture: no image-data.
        CHECK(notified[0].hints == (std::vector<std::string> { "desktop-entry" }));
    }

    // Pressed, then dismissed: each heard once, under the name show() gave.
    bus.press(100);
    CHECK(eventually([&] { return heard.has(Kind::Clicked, first); }));
    CHECK(!heard.has(Kind::Closed, first));
    bus.dismiss(100);
    CHECK(eventually([&] { return heard.has(Kind::Closed, first); }));
    // A signal about a notice that is not this notifier's is not an event.
    bus.press(555);
    bus.dismiss(100); // and one already gone is not gone twice
    platform::Notice second_notice;
    second_notice.title = "";
    second_notice.site = "chat.example";
    second_notice.stays = true;
    std::uint64_t const second = notifier->show(second_notice);
    CHECK(eventually([&] { return heard.has(Kind::Shown, second); }));
    std::size_t closes = 0;
    std::size_t clicks = 0;
    for (platform::Notifier::Event const& event : heard.events) {
        closes += event.kind == Kind::Closed ? 1 : 0;
        clicks += event.kind == Kind::Clicked ? 1 : 0;
    }
    CHECK_EQ(closes, 1u);
    CHECK_EQ(clicks, 1u);
    notified = bus.notified();
    CHECK_EQ(notified.size(), 2u);
    if (notified.size() == 2) {
        // No title: the site stands for it. It stays until dismissed.
        CHECK_EQ(notified[1].summary, std::string("chat.example"));
        CHECK_EQ(notified[1].body, std::string("chat.example"));
        CHECK_EQ(notified[1].timeout, 0);
        CHECK_EQ(notified[1].given, 101u);
    }

    // One put in another's place takes its number at the service, and is
    // the one the service's later words are about.
    platform::Notice third_notice;
    third_notice.title = "Replaced";
    third_notice.site = "chat.example";
    std::uint64_t const third = notifier->show(third_notice, second);
    CHECK(eventually([&] { return heard.has(Kind::Shown, third); }));
    notified = bus.notified();
    CHECK_EQ(notified.size(), 3u);
    if (notified.size() == 3)
        CHECK_EQ(notified[2].replaces, 101u);
    bus.press(101);
    CHECK(eventually([&] { return heard.has(Kind::Clicked, third); }));
    CHECK(!heard.has(Kind::Clicked, second));

    // A picture goes as raw RGBA in "image-data"; a press the service sends
    // its window token ahead of comes with that token.
    platform::Notice pictured;
    pictured.title = "Pictured";
    pictured.site = "chat.example";
    pictured.silent = true;
    auto picture = std::make_shared<Bitmap>(3, 2, Color::rgba(10, 20, 30, 40));
    pictured.picture = picture;
    std::uint64_t const fifth_shown = notifier->show(pictured);
    CHECK(eventually([&] { return heard.has(Kind::Shown, fifth_shown); }));
    notified = bus.notified();
    CHECK_EQ(notified.size(), 4u);
    if (notified.size() == 4) {
        CHECK(notified[3].hints == (std::vector<std::string> { "desktop-entry", "suppress-sound", "image-data" }));
        CHECK(notified[3].image == (std::vector<std::uint32_t> { 3, 2, 12, 1, 8, 4, 24, 10, 20, 30, 40 }));
        CHECK_EQ(notified[3].timeout, -1);
        bus.press_with_token(notified[3].given, "kwin-token-7");
        CHECK(eventually([&] { return heard.has(Kind::Clicked, fifth_shown); }));
        for (platform::Notifier::Event const& event : heard.events) {
            if (event.kind == Kind::Clicked && event.id == fifth_shown)
                CHECK_EQ(event.activation_token, std::string("kwin-token-7"));
            if (event.kind == Kind::Clicked && event.id == first)
                CHECK_EQ(event.activation_token, std::string()); // pressed with no token
        }
    }

    // Closed from here: the service is told, and says it is closed.
    notifier->close(third);
    CHECK(eventually([&] { return heard.has(Kind::Closed, third); }));
    CHECK(bus.closed() == (std::vector<std::uint32_t> { 101 }));
    // Closed before the service has answered the show: still closed.
    std::uint64_t const fourth = notifier->show(notice);
    notifier->close(fourth);
    CHECK(eventually([&] { return heard.has(Kind::Closed, fourth); }));
    CHECK(heard.has(Kind::Shown, fourth));

    // The bus goes away: what is asked afterwards fails, and says so.
    bus.hang_up();
    CHECK(eventually([&] { return !notifier->available(); }));
    std::uint64_t const late = notifier->show(notice);
    CHECK(eventually([&] { return heard.has(Kind::Failed, late); }));
}

void test_notifier_without_markup_or_service()
{
    using Kind = platform::Notifier::Event::Kind;
    {
        // A service that reads no markup is given the text as it is.
        std::string const path = scratch_socket("bus-plain");
        FakeBus bus(path, false);
        std::unique_ptr<platform::Notifier> const notifier = platform::open_notifier_on(dbus::SocketAddress { path, false }, "Sashfold", {});
        Heard heard { *notifier, {} };
        platform::Notice notice;
        notice.title = "T";
        notice.body = "a < b & c";
        notice.site = "x.example";
        notice.silent = true;
        std::uint64_t const id = notifier->show(notice);
        CHECK(eventually([&] { return heard.has(Kind::Shown, id); }));
        std::vector<Notified> const notified = bus.notified();
        CHECK_EQ(notified.size(), 1u);
        if (notified.size() == 1) {
            CHECK_EQ(notified[0].body, std::string("x.example\na < b & c"));
            CHECK_EQ(notified[0].timeout, -1); // read past the hints, the silent one among them
        }
    }
    {
        // A service that refuses: the notice failed.
        std::string const path = scratch_socket("bus-refusing");
        FakeBus bus(path, true, true);
        std::unique_ptr<platform::Notifier> const notifier = platform::open_notifier_on(dbus::SocketAddress { path, false }, "Sashfold", {});
        Heard heard { *notifier, {} };
        std::uint64_t const id = notifier->show(platform::Notice { "T", "", "x.example", nullptr, false, false });
        CHECK(eventually([&] { return heard.has(Kind::Failed, id); }));
        CHECK(!heard.has(Kind::Shown, id));
        CHECK(notifier->available()); // a notice refused is not a service gone
    }
    {
        // No bus at the address: known to be absent a moment later, said
        // with a wake; a notice asked for before or after fails.
        std::atomic<int> wakes { 0 };
        std::unique_ptr<platform::Notifier> const notifier
            = platform::open_notifier_on(dbus::SocketAddress { scratch_socket("no-such-bus"), false }, "Sashfold", [&wakes] { ++wakes; });
        Heard heard { *notifier, {} };
        std::uint64_t const id = notifier->show(platform::Notice { "T", "", "x.example", nullptr, false, false });
        CHECK(eventually([&] { return heard.has(Kind::Failed, id); }));
        CHECK(eventually([&] { return !notifier->available(); }));
        CHECK(wakes > 0);
        std::uint64_t const later = notifier->show(platform::Notice { "T", "", "x.example", nullptr, false, false });
        CHECK(eventually([&] { return heard.has(Kind::Failed, later); }));
    }
}

}
#endif

int main()
{
    CHECK_EQ(platform::escape_notice_markup("a < b && <a href=\"x\">c</a>"), std::string("a &lt; b &amp;&amp; &lt;a href=\"x\"&gt;c&lt;/a&gt;"));
#if defined(__linux__)
    test_wire();
    test_addresses();
    test_notifier();
    test_notifier_without_markup_or_service();
    std::error_code error;
    std::filesystem::remove_all("dbus-scratch", error);
#else
    CHECK(platform::open_notifier("Sashfold", {}) == nullptr);
#endif
    return sashfold::test::report("dbus");
}
