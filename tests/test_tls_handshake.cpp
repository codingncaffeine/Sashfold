// A real TLS 1.3 handshake, driven end to end against the openssl command's
// own server on the loopback interface: the engine negotiates a suite,
// checks the server's signature over the transcript, finishes, and then
// exchanges application data over protected records. Both suites are driven,
// a server that shares none of ours must be refused, and a server that asks
// for another hello (RFC 8446 §4.1.4) must be answered. Nothing here reaches
// the network — the peer is a child process on 127.0.0.1 — and the whole
// live half SKIPs with a printed reason when openssl is not on PATH, when no
// certificate can be made, or when no loopback port can be bound.
//
// The hand-written cases come first and need no openssl at all: they drive
// the retry-request path with crafted records, including every way §4.1.4
// says a retry must be refused, which no cooperating server would produce.
#include "Test.h"

#include "net/tls/Tls13.h"
#include "platform/Net.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t record_handshake = 22;
constexpr std::uint8_t record_alert = 21;
constexpr std::uint8_t record_change_cipher_spec = 20;
constexpr std::uint16_t suite_aes_128_gcm = 0x1301;
constexpr std::uint16_t suite_chacha20 = 0x1303;
constexpr std::uint16_t group_x25519 = 0x001d;
constexpr std::uint16_t group_secp256r1 = 0x0017;

std::string hex(std::span<std::uint8_t const> bytes)
{
    std::string out;
    char buffer[3];
    for (std::uint8_t const b : bytes) {
        std::snprintf(buffer, sizeof buffer, "%02x", b);
        out += buffer;
    }
    return out;
}

void put16(Bytes& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append(Bytes& out, Bytes const& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

Bytes extension(std::uint16_t type, Bytes const& data)
{
    Bytes out;
    put16(out, type);
    put16(out, static_cast<std::uint16_t>(data.size()));
    append(out, data);
    return out;
}

Bytes supported_versions()
{
    Bytes body;
    put16(body, 0x0304);
    return extension(43, body);
}

Bytes key_share(std::uint16_t group)
{
    Bytes body;
    put16(body, group);
    return extension(51, body);
}

Bytes cookie_value(std::string const& text)
{
    Bytes body;
    put16(body, static_cast<std::uint16_t>(text.size()));
    body.insert(body.end(), text.begin(), text.end());
    return body;
}

// §4.1.3: a ServerHello whose random is this value is a retry request.
Bytes retry_request(Bytes const& session_id, std::uint16_t suite, Bytes const& extensions)
{
    static constexpr std::uint8_t retry_random[32] = {
        0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
        0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c,
    };
    Bytes body;
    put16(body, 0x0303);
    body.insert(body.end(), std::begin(retry_random), std::end(retry_random));
    body.push_back(static_cast<std::uint8_t>(session_id.size()));
    append(body, session_id);
    put16(body, suite);
    body.push_back(0);
    put16(body, static_cast<std::uint16_t>(extensions.size()));
    append(body, extensions);

    Bytes message;
    message.push_back(2); // server_hello
    message.push_back(0);
    put16(message, static_cast<std::uint16_t>(body.size()));
    append(message, body);

    Bytes record;
    record.push_back(record_handshake);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(message.size()));
    append(record, message);
    return record;
}

// A plain ServerHello that is not a retry, for the rules that apply to the
// hello after one.
Bytes server_hello(Bytes const& session_id, std::uint16_t suite, Bytes const& extensions)
{
    Bytes body;
    put16(body, 0x0303);
    for (std::size_t i = 0; i < 32; ++i)
        body.push_back(static_cast<std::uint8_t>(0x30 + i));
    body.push_back(static_cast<std::uint8_t>(session_id.size()));
    append(body, session_id);
    put16(body, suite);
    body.push_back(0);
    put16(body, static_cast<std::uint16_t>(extensions.size()));
    append(body, extensions);

    Bytes message;
    message.push_back(2);
    message.push_back(0);
    put16(message, static_cast<std::uint16_t>(body.size()));
    append(message, body);

    Bytes record;
    record.push_back(record_handshake);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(message.size()));
    append(record, message);
    return record;
}

struct Hello {
    Bytes random;
    Bytes session_id;
    std::vector<std::uint16_t> suites;
    std::vector<std::pair<std::uint16_t, Bytes>> extensions;

    bool has(std::uint16_t type) const
    {
        for (auto const& entry : extensions)
            if (entry.first == type)
                return true;
        return false;
    }

    Bytes data(std::uint16_t type) const
    {
        for (auto const& entry : extensions)
            if (entry.first == type)
                return entry.second;
        return {};
    }
};

// The body of the first ClientHello handshake message in a flight of records.
Bytes client_hello_body(Bytes const& records)
{
    std::size_t offset = 0;
    while (offset + 5 <= records.size()) {
        std::size_t const length = (static_cast<std::size_t>(records[offset + 3]) << 8) | records[offset + 4];
        if (offset + 5 + length > records.size())
            break;
        if (records[offset] == record_handshake && length >= 4 && records[offset + 5] == 1) {
            std::size_t const body_length = (static_cast<std::size_t>(records[offset + 6]) << 16)
                | (static_cast<std::size_t>(records[offset + 7]) << 8) | records[offset + 8];
            if (9 + body_length <= 5 + length)
                return Bytes(records.begin() + static_cast<std::ptrdiff_t>(offset + 9),
                    records.begin() + static_cast<std::ptrdiff_t>(offset + 9 + body_length));
            break;
        }
        offset += 5 + length;
    }
    return {};
}

bool parse_hello(Bytes const& body, Hello& hello)
{
    std::size_t offset = 0;
    auto need = [&](std::size_t count) { return offset + count <= body.size(); };
    if (!need(2 + 32 + 1))
        return false;
    offset += 2;
    hello.random.assign(body.begin() + 2, body.begin() + 34);
    offset = 34;
    std::size_t const session_length = body[offset++];
    if (!need(session_length))
        return false;
    hello.session_id.assign(body.begin() + static_cast<std::ptrdiff_t>(offset),
        body.begin() + static_cast<std::ptrdiff_t>(offset + session_length));
    offset += session_length;
    if (!need(2))
        return false;
    std::size_t const suites_length = (static_cast<std::size_t>(body[offset]) << 8) | body[offset + 1];
    offset += 2;
    if (!need(suites_length) || suites_length % 2 != 0)
        return false;
    for (std::size_t i = 0; i < suites_length; i += 2)
        hello.suites.push_back(static_cast<std::uint16_t>((body[offset + i] << 8) | body[offset + i + 1]));
    offset += suites_length;
    if (!need(1))
        return false;
    std::size_t const compression_length = body[offset++];
    if (!need(compression_length))
        return false;
    offset += compression_length;
    if (!need(2))
        return false;
    std::size_t const extensions_length = (static_cast<std::size_t>(body[offset]) << 8) | body[offset + 1];
    offset += 2;
    if (!need(extensions_length))
        return false;
    std::size_t const end = offset + extensions_length;
    while (offset + 4 <= end) {
        std::uint16_t const type = static_cast<std::uint16_t>((body[offset] << 8) | body[offset + 1]);
        std::size_t const length = (static_cast<std::size_t>(body[offset + 2]) << 8) | body[offset + 3];
        offset += 4;
        if (offset + length > end)
            return false;
        hello.extensions.emplace_back(type,
            Bytes(body.begin() + static_cast<std::ptrdiff_t>(offset),
                body.begin() + static_cast<std::ptrdiff_t>(offset + length)));
        offset += length;
    }
    return offset == end;
}

// The description of the last alert in a flight, or −1 when there is none.
int alert_description(Bytes const& records)
{
    int description = -1;
    std::size_t offset = 0;
    while (offset + 5 <= records.size()) {
        std::size_t const length = (static_cast<std::size_t>(records[offset + 3]) << 8) | records[offset + 4];
        if (offset + 5 + length > records.size())
            break;
        if (records[offset] == record_alert && length == 2)
            description = records[offset + 6];
        offset += 5 + length;
    }
    return description;
}

tls::TlsConfig test_config()
{
    tls::TlsConfig config;
    config.server_name = "localhost";
    for (std::size_t i = 0; i < config.private_key.size(); ++i)
        config.private_key[i] = static_cast<std::uint8_t>(0x40 + i);
    for (std::size_t i = 0; i < config.client_random.size(); ++i)
        config.client_random[i] = static_cast<std::uint8_t>(0x10 + i);
    for (std::size_t i = 0; i < config.session_id.size(); ++i)
        config.session_id[i] = static_cast<std::uint8_t>(0x80 + i);
    // The chain is the validator's business and has its own tests; what is
    // under test here is the handshake.
    config.verify_chain = [](std::vector<tls::Certificate> const&, std::string&) { return true; };
    return config;
}

Bytes session_id_of(tls::TlsConfig const& config)
{
    return Bytes(config.session_id.begin(), config.session_id.end());
}

// ---- the retry request, against crafted records

void test_retry_request()
{
    tls::TlsConfig const config = test_config();
    Bytes const session_id = session_id_of(config);

    // A cookie is a change the client can make, so this retry is legal. The
    // second hello repeats the first exactly and adds the cookie.
    {
        tls::TlsEngine engine(config);
        Hello first;
        CHECK(parse_hello(client_hello_body(engine.start()), first));
        CHECK(!first.has(44));
        CHECK(first.has(51)); // a key share went out, so only a cookie can change the hello

        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("a-server-cookie")));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        CHECK_EQ(engine.hello_retry_requests(), 1);
        CHECK(engine.state() == tls::TlsState::WaitServerHello);
        CHECK_EQ(engine.error(), std::string());
        // §D.4: the dummy record leads the second flight.
        CHECK(!out.to_send.empty() && out.to_send[0] == record_change_cipher_spec);

        Hello second;
        CHECK(parse_hello(client_hello_body(out.to_send), second));
        CHECK_EQ(hex(second.random), hex(first.random));
        CHECK_EQ(hex(second.session_id), hex(first.session_id));
        CHECK(second.suites == first.suites);
        CHECK(second.has(44));
        CHECK_EQ(hex(second.data(44)), hex(cookie_value("a-server-cookie")));
        CHECK_EQ(hex(second.data(51)), hex(first.data(51))); // the same share, as §4.1.4 requires
    }

    // A hello that carries no key share at all asks the server to name the
    // group (§4.2.8); the second hello then carries the share it asked for.
    {
        tls::TlsConfig empty = test_config();
        empty.empty_key_share = true;
        tls::TlsEngine engine(empty);
        Hello first;
        CHECK(parse_hello(client_hello_body(engine.start()), first));
        CHECK(first.has(51));
        CHECK_EQ(hex(first.data(51)), std::string("0000")); // an empty list of shares

        Bytes extensions = supported_versions();
        append(extensions, key_share(group_x25519));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        CHECK_EQ(engine.hello_retry_requests(), 1);
        Hello second;
        CHECK(parse_hello(client_hello_body(out.to_send), second));
        CHECK_EQ(second.data(51).size(), std::size_t(2 + 2 + 2 + 32)); // one x25519 share
        CHECK(!second.has(44)); // no cookie was sent, so none comes back
    }

    // Every retry §4.1.4 forbids, with the alert it names.
    auto refusal = [&](Bytes const& extensions, std::uint16_t suite) {
        tls::TlsEngine engine(config);
        engine.start();
        tls::TlsOutput out;
        bool const ok = engine.feed(retry_request(session_id, suite, extensions), out);
        CHECK(!ok);
        CHECK(engine.state() == tls::TlsState::Failed);
        CHECK(!engine.error().empty());
        return alert_description(out.to_send);
    };
    // Nothing would change: no group, no cookie.
    CHECK_EQ(refusal(supported_versions(), suite_chacha20), 47);
    // The group this hello already carried a share for.
    {
        Bytes extensions = supported_versions();
        append(extensions, key_share(group_x25519));
        CHECK_EQ(refusal(extensions, suite_chacha20), 47);
    }
    // A group that was never offered.
    {
        Bytes extensions = supported_versions();
        append(extensions, key_share(group_secp256r1));
        CHECK_EQ(refusal(extensions, suite_chacha20), 47);
    }
    // No version at all, and an extension that was never offered.
    CHECK_EQ(refusal(extension(44, cookie_value("no-version")), suite_chacha20), 109);
    {
        Bytes extensions = supported_versions();
        append(extensions, extension(0xff01, Bytes { 0x00 }));
        CHECK_EQ(refusal(extensions, suite_chacha20), 110);
    }
    // A suite that was never offered (0x1302 is AES-256-GCM, which is not ours).
    {
        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("wrong-suite")));
        CHECK_EQ(refusal(extensions, 0x1302), 47);
    }

    // A second retry is fatal, and it is unexpected_message that says so.
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("first")));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        tls::TlsOutput second;
        CHECK(!engine.feed(retry_request(session_id, suite_chacha20, extensions), second));
        CHECK_EQ(alert_description(second.to_send), 10);
        CHECK(engine.state() == tls::TlsState::Failed);
    }

    // The hello after a retry may not change the suite the retry named.
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("first")));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        Bytes hello_extensions = supported_versions();
        append(hello_extensions, key_share(group_x25519));
        tls::TlsOutput second;
        CHECK(!engine.feed(server_hello(session_id, suite_aes_128_gcm, hello_extensions), second));
        CHECK_EQ(alert_description(second.to_send), 47);
    }
}

// ---- a whole handshake against openssl s_server

#ifndef _WIN32

std::string g_openssl;
std::string g_directory;
std::string g_certificate;
std::string g_key;
std::string g_setup_log;

void on_alarm(int)
{
    char const message[] = "FAIL test_tls_handshake: a wait passed its deadline\n";
    ssize_t const written = ::write(2, message, sizeof message - 1);
    static_cast<void>(written);
    ::_exit(2);
}

std::string read_file(std::string const& path)
{
    std::string out;
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return out;
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        out.append(buffer, read);
    std::fclose(file);
    return out;
}

pid_t spawn(std::vector<std::string> const& args, std::string const& log)
{
    pid_t const child = ::fork();
    if (child != 0)
        return child;
    int const handle = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (handle >= 0) {
        ::dup2(handle, 1);
        ::dup2(handle, 2);
        ::close(handle);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string const& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
}

// Waits for a child, killing it if it outstays the deadline; true when it
// exited cleanly of its own accord.
bool reap(pid_t child, int seconds, bool ask_first)
{
    if (child <= 0)
        return false;
    if (ask_first)
        ::kill(child, SIGTERM);
    for (int waited = 0; waited < seconds * 20; ++waited) {
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        ::usleep(50 * 1000);
    }
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    return false;
}

bool run(std::vector<std::string> const& args, int seconds)
{
    return reap(spawn(args, g_setup_log), seconds, false);
}

// A loopback port, bound to learn its number and then released for the
// child. Losing that race is a SKIP, never a failure.
bool free_port(std::uint16_t& port)
{
    std::optional<platform::TcpListener> listener = platform::TcpListener::listen_loopback();
    if (!listener)
        return false;
    port = listener->port();
    return true;
}

std::optional<platform::TcpSocket> connect_loopback(std::uint16_t port, int seconds, int& attempts)
{
    for (attempts = 1; attempts <= seconds * 20; ++attempts) {
        std::optional<platform::TcpSocket> socket = platform::TcpSocket::connect("127.0.0.1", port);
        if (socket)
            return socket;
        ::usleep(50 * 1000);
    }
    return std::nullopt;
}

struct Exchange {
    bool connected = false;
    // The plumbing got as far as the peer: the socket connected and our first
    // flight went out. Without this, a refusal case cannot tell a client that
    // refused from a server that never came up.
    bool reached_peer = false;
    // The engine ended the handshake, rather than the socket or the child
    // process ending it for us.
    bool engine_failed = false;
    int retries = 0;
    tls::CipherSuite suite = tls::CipherSuite::ChaCha20Poly1305Sha256;
    std::string response;
    std::string error;
};

// The handshake and one request over it, on a socket of our own.
Exchange exchange(tls::TlsConfig config, std::uint16_t port, std::string const& request)
{
    Exchange result;
    int attempts = 0;
    std::optional<platform::TcpSocket> socket = connect_loopback(port, 10, attempts);
    if (!socket) {
        result.error = "no loopback connect in " + std::to_string(attempts) + " attempts";
        return result;
    }
    tls::TlsEngine engine(std::move(config));
    std::vector<std::uint8_t> const hello = engine.start();
    if (!socket->send_all(hello.data(), hello.size())) {
        result.error = "the first flight did not go out";
        return result;
    }
    result.reached_peer = true;
    std::uint8_t buffer[16384];
    while (!engine.connected() && engine.state() != tls::TlsState::Failed) {
        std::ptrdiff_t const received = socket->receive(buffer, sizeof buffer);
        if (received <= 0) {
            result.error = "the server went away during the handshake";
            break;
        }
        tls::TlsOutput out;
        bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
        if (!out.to_send.empty() && !socket->send_all(out.to_send.data(), out.to_send.size())) {
            result.error = "a flight did not go out";
            break;
        }
        if (!ok)
            break;
    }
    result.retries = engine.hello_retry_requests();
    result.suite = engine.cipher_suite();
    result.engine_failed = engine.state() == tls::TlsState::Failed;
    if (!engine.connected()) {
        if (!engine.error().empty())
            result.error = engine.error();
        else if (result.error.empty())
            result.error = "the handshake did not finish";
        return result;
    }
    result.connected = true;
    std::vector<std::uint8_t> const sealed = engine.seal(
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(request.data()), request.size()));
    if (!socket->send_all(sealed.data(), sealed.size())) {
        result.error = "the request did not go out";
        return result;
    }
    for (;;) {
        std::ptrdiff_t const received = socket->receive(buffer, sizeof buffer);
        if (received <= 0)
            break;
        tls::TlsOutput out;
        bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
        if (!out.plaintext.empty())
            result.response.append(reinterpret_cast<char const*>(out.plaintext.data()), out.plaintext.size());
        if (!ok || engine.state() != tls::TlsState::Connected)
            break;
        if (result.response.find("</HTML>") != std::string::npos)
            break;
    }
    std::vector<std::uint8_t> const alert = engine.close_notify();
    if (!alert.empty())
        socket->send_all(alert.data(), alert.size());
    return result;
}

// One case: a server with the given options, our client with the given
// configuration, and what both ends say about what happened.
void live_case(std::string const& name, std::vector<std::string> const& server_options, tls::TlsConfig config,
    bool expect_connected, tls::CipherSuite expected_suite, int expected_retries,
    char const* expected_refusal = nullptr)
{
    std::uint16_t port = 0;
    if (!free_port(port)) {
        std::printf("SKIP %s: no loopback port could be bound\n", name.c_str());
        return;
    }
    std::string const log = g_directory + "/server-" + std::to_string(port) + ".log";
    std::vector<std::string> args = { g_openssl, "s_server", "-accept", "127.0.0.1:" + std::to_string(port),
        "-cert", g_certificate, "-key", g_key, "-tls1_3", "-naccept", "1", "-www" };
    args.insert(args.end(), server_options.begin(), server_options.end());
    pid_t const server = spawn(args, log);
    if (server < 0) {
        std::printf("SKIP %s: the server process could not be started\n", name.c_str());
        return;
    }

    ::alarm(60);
    Exchange const result = exchange(std::move(config), port, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n");
    // -naccept 1 means the server leaves of its own accord once it has served
    // us; it is killed only if it overstays that.
    reap(server, 5, false);
    ::alarm(0);

    std::string const server_said = read_file(log);
    if (!expect_connected) {
        CHECK(!result.connected);
        // A refusal counts only if OUR client made it: the socket reached the
        // server, our hello went out, and the engine is what stopped. Asserting
        // a non-empty error alone passes on any infrastructure failure — both
        // "no loopback connect in N attempts" and "the server went away during
        // the handshake" satisfy it — so the case would go green on a machine
        // where openssl never started, which is the opposite of a test.
        CHECK(result.reached_peer);
        CHECK(result.engine_failed);
        CHECK(expected_refusal != nullptr);
        bool const named = expected_refusal != nullptr && result.error.find(expected_refusal) != std::string::npos;
        CHECK(named);
        if (!result.reached_peer || !result.engine_failed || !named)
            std::printf("  %s: refused, but not for the reason this case names: reached_peer=%d engine_failed=%d"
                        " error=\"%s\" wanted=\"%s\"\n",
                name.c_str(), result.reached_peer ? 1 : 0, result.engine_failed ? 1 : 0, result.error.c_str(),
                expected_refusal != nullptr ? expected_refusal : "(none given)");
        if (result.connected)
            std::printf("  %s: expected no handshake, got one\n", name.c_str());
        std::remove(log.c_str());
        return;
    }
    if (!result.connected) {
        sashfold::test::fail(name + ": the handshake failed: " + result.error, __FILE__, __LINE__);
        std::printf("  server said: %s\n", server_said.c_str());
        std::remove(log.c_str());
        return;
    }
    CHECK(result.connected);
    CHECK_EQ(result.retries, expected_retries);
    CHECK_EQ(std::string(tls::cipher_suite_name(result.suite)), std::string(tls::cipher_suite_name(expected_suite)));
    // Application data went both ways over protected records.
    CHECK(result.response.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(result.response.find("</HTML>") != std::string::npos);
    // The page the server serves names the suite it believes it negotiated,
    // which is not our own bookkeeping: it has to be the one we think we
    // protected the records with, and not the other one.
    std::string const other = expected_suite == tls::CipherSuite::Aes128GcmSha256
        ? tls::cipher_suite_name(tls::CipherSuite::ChaCha20Poly1305Sha256)
        : tls::cipher_suite_name(tls::CipherSuite::Aes128GcmSha256);
    CHECK(result.response.find(std::string("Cipher is ") + tls::cipher_suite_name(expected_suite)) != std::string::npos);
    CHECK(result.response.find(std::string("Cipher is ") + other) == std::string::npos);
    std::remove(log.c_str());
}

bool live_setup()
{
    char const* const base = std::getenv("TMPDIR");
    std::string pattern = (base != nullptr && base[0] != '\0' ? std::string(base) : std::string("/tmp"))
        + "/sashfold-tls-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
        std::printf("SKIP test_tls_handshake: no temporary directory could be made\n");
        return false;
    }
    g_directory = buffer.data();
    g_certificate = g_directory + "/server.pem";
    g_key = g_directory + "/server.key";
    g_setup_log = g_directory + "/setup.log";
    // A throwaway key and a self-signed certificate for the child, made here
    // so that no private key is ever committed. The chain is not what this
    // test judges; the signature over the transcript is.
    if (!run({ g_openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=localhost",
                 "-keyout", g_key, "-out", g_certificate },
            90)) {
        std::printf("SKIP test_tls_handshake: openssl could not make a certificate:\n%s\n",
            read_file(g_setup_log).c_str());
        return false;
    }
    return true;
}

void live_teardown()
{
    // By name, one at a time.
    std::remove(g_certificate.c_str());
    std::remove(g_key.c_str());
    std::remove(g_setup_log.c_str());
    ::rmdir(g_directory.c_str());
}

void test_live_handshake()
{
    if (!live_setup())
        return;
    // AES-128-GCM, the suite this lane adds: the server will speak nothing else.
    live_case("aes-128-gcm", { "-ciphersuites", "TLS_AES_128_GCM_SHA256" }, test_config(), true,
        tls::CipherSuite::Aes128GcmSha256, 0);
    // ChaCha20-Poly1305, which the client has always had.
    live_case("chacha20-poly1305", { "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256" }, test_config(), true,
        tls::CipherSuite::ChaCha20Poly1305Sha256, 0);
    // The negotiation is real: a client that offers only AES cannot talk to a
    // server that offers only ChaCha.
    {
        tls::TlsConfig only_aes = test_config();
        only_aes.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
        // The server has nothing we offered, so it is the peer that refuses,
        // with an alert our engine reports: that string, not merely "something
        // went wrong", is what this case asserts.
        live_case("no-shared-suite", { "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256" }, only_aes, false,
            tls::CipherSuite::Aes128GcmSha256, 0, "fatal alert");
    }
    // A client that offers only AES and a server that speaks it: the suite is
    // forced from our side, not chosen by the peer.
    {
        tls::TlsConfig only_aes = test_config();
        only_aes.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
        live_case("aes-only-client", {}, only_aes, true, tls::CipherSuite::Aes128GcmSha256, 0);
    }
    // A hello with no key share: the server answers with a retry request, and
    // the handshake finishes through it — transcript substitution and all.
    {
        tls::TlsConfig retry = test_config();
        retry.empty_key_share = true;
        retry.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
        live_case("retry-then-aes", {}, retry, true, tls::CipherSuite::Aes128GcmSha256, 1);
    }
    {
        tls::TlsConfig retry = test_config();
        retry.empty_key_share = true;
        retry.cipher_suites = { tls::CipherSuite::ChaCha20Poly1305Sha256 };
        live_case("retry-then-chacha", {}, retry, true, tls::CipherSuite::ChaCha20Poly1305Sha256, 1);
    }
    live_teardown();
}

#endif

}

int main(int argc, char** argv)
{
    test_retry_request();
#ifndef _WIN32
    ::signal(SIGALRM, on_alarm);
    ::signal(SIGPIPE, SIG_IGN); // a peer that leaves early is a failed check, not a dead test
    if (argc < 2 || argv[1][0] == '\0' || ::access(argv[1], X_OK) != 0) {
        std::printf("SKIP test_tls_handshake live handshake: openssl is not on PATH\n");
        return sashfold::test::report("tls handshake");
    }
    g_openssl = argv[1];
    test_live_handshake();
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
    std::printf("SKIP test_tls_handshake live handshake: no posix process control\n");
#endif
    return sashfold::test::report("tls handshake");
}
