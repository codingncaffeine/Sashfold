// tls_probe drives the TLS client against one host and prints what the engine
// itself says — the state it stopped in and error(), which Connection::open
// throws away in favour of one message for every cause.
//
//   tls_probe <host> [port] [--suite aes128gcm|chacha20] [--insecure] [--stall <seconds>] [--tls12] [--resume]
//
// --stall sleeps inside the chain verification, where the real client spends
// its time between the server's flight and its own Finished, and then sends
// a GET and reports whether the server still answers: the way to measure how
// long a server waits for a slow client before hanging up. --tls12 offers
// TLS 1.2 alone, to drive that path against a server that would take 1.3.
// --resume connects a second time offering the ticket or the session the
// first connection was given, and reports whether the server resumed it and
// how long each handshake took: the measurement behind resumption.
//
// Dev-only, not a CMake target: it links libsashfold_core.a, so RELINK it after
// every core build or it runs old code.

#include "net/Http.h"
#include "net/tls/Tls13.h"
#include "platform/Net.h"
#include "platform/Random.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace sashfold;

namespace {

struct Options {
    std::string host;
    std::uint16_t port = 443;
    std::string suite;
    bool insecure = false;
    bool tls12_only = false;
    int stall = 0;
};

// What one connection was given to resume by next time.
struct Left {
    std::vector<tls::Ticket> tickets;
    std::optional<tls::Session12> session;
};

tls::TlsConfig make_config(Options const& options)
{
    tls::TlsConfig config;
    config.server_name = options.host;
    config.offer_tls13 = !options.tls12_only;
    platform::fill_random(std::span<std::uint8_t>(config.client_random.data(), config.client_random.size()));
    platform::fill_random(std::span<std::uint8_t>(config.session_id.data(), config.session_id.size()));
    platform::fill_random(std::span<std::uint8_t>(config.private_key.data(), config.private_key.size()));
    platform::fill_random(std::span<std::uint8_t>(config.p256_private_key.data(), config.p256_private_key.size()));
    if (options.suite == "aes128gcm")
        config.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
    else if (options.suite == "chacha20")
        config.cipher_suites = { tls::CipherSuite::ChaCha20Poly1305Sha256 };
    // The chain is accepted or refused here, and the reason is printed either
    // way, so a validation failure is never confused with a handshake one.
    bool const insecure = options.insecure;
    int const stall = options.stall;
    config.verify_chain = [insecure, stall](std::vector<tls::Certificate> const& chain, std::string& reason) {
        std::printf("  verify_chain reached: %zu certificate(s)\n", chain.size());
        if (stall > 0) {
            std::printf("  stalling %d s before the Finished, as a slow verification would\n", stall);
            std::this_thread::sleep_for(std::chrono::seconds(stall));
        }
        if (insecure)
            return true;
        reason = "the probe refuses every chain unless --insecure";
        return false;
    };
    return config;
}

// One connection: the handshake, flight by flight, then a request and the
// first line of its answer. What the server hands over for next time lands
// in `left`. 0 when the request was answered.
int connect_once(Options const& options, tls::TlsConfig config, Left& left)
{
    config.on_ticket = [&left](tls::Ticket ticket) {
        std::printf("  ticket received: %zu bytes, good for %u s\n", ticket.ticket.size(), ticket.lifetime_seconds);
        left.tickets.push_back(std::move(ticket));
    };
    config.on_session = [&left](tls::Session12 session) {
        std::printf("  session kept: %s, id %zu bytes, ticket %zu bytes\n", session.ticket.empty() ? "by id" : "by ticket",
            session.session_id.size(), session.ticket.size());
        left.session = std::move(session);
    };
    auto const started = std::chrono::steady_clock::now();
    auto tcp = platform::TcpSocket::connect(options.host, options.port);
    if (!tcp) {
        std::printf("RESULT no-tcp: could not connect to %s:%u\n", options.host.c_str(), options.port);
        return 1;
    }
    auto const connected_at = std::chrono::steady_clock::now();
    bool const offering = config.resume_ticket.has_value() || config.resume_session.has_value();
    tls::TlsEngine engine(std::move(config));
    std::vector<std::uint8_t> const hello = engine.start();
    std::printf("  hello %zu bytes%s\n", hello.size(), offering ? " (offering to resume)" : "");
    if (!tcp->send_all(hello.data(), hello.size())) {
        std::printf("RESULT no-send: the first flight did not go out\n");
        return 1;
    }
    std::uint8_t buffer[16384];
    int flights = 0;
    while (!engine.connected() && engine.state() != tls::TlsState::Failed) {
        std::ptrdiff_t const received = tcp->receive(buffer, sizeof buffer);
        if (received <= 0) {
            std::printf("  the peer sent nothing more (received=%td) after %d flight(s)\n", received, flights);
            break;
        }
        ++flights;
        tls::TlsOutput out;
        bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
        std::printf("  flight %d: %td in, %zu to send, state=%d ok=%d\n", flights, received, out.to_send.size(),
            static_cast<int>(engine.state()), ok ? 1 : 0);
        if (!out.to_send.empty() && !tcp->send_all(out.to_send.data(), out.to_send.size())) {
            std::printf("RESULT no-send: a later flight did not go out\n");
            return 1;
        }
        if (!ok)
            break;
    }
    auto const finished_at = std::chrono::steady_clock::now();
    auto const ms = [](std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
    };
    std::printf("RESULT %s state=%d version=%s retries=%d resumed=%d suite=%s tcp_ms=%lld handshake_ms=%lld error=\"%s\"\n",
        engine.connected() ? "connected" : "failed", static_cast<int>(engine.state()),
        engine.version() == 0x0304 ? "1.3" : engine.version() == 0x0303 ? "1.2" : "none", engine.hello_retry_requests(),
        engine.resumed() ? 1 : 0, tls::cipher_suite_name(engine.cipher_suite()), ms(started, connected_at),
        ms(connected_at, finished_at), engine.error().c_str());
    if (!engine.connected())
        return 1;
    // A request over the connection, and what comes back: the status line,
    // or nothing at all when the server has already hung up. The request
    // carries the browser's own headers: an edge with a bot wall holds one
    // that does not look like a browser's (an Akamai front held a bare GET,
    // and one with a User-Agent alone, for minutes), and a probe should be
    // answered the way the browser is.
    std::string const request = "GET / HTTP/1.1\r\nHost: " + options.host + "\r\nUser-Agent: " + std::string(net::user_agent())
        + "\r\nAccept: text/html,application/xhtml+xml,*/*;q=0.8\r\nAccept-Encoding: gzip, deflate\r\nConnection: close\r\n\r\n";
    std::vector<std::uint8_t> const sealed = engine.seal(std::span<std::uint8_t const>(
        reinterpret_cast<std::uint8_t const*>(request.data()), request.size()));
    if (!tcp->send_all(sealed.data(), sealed.size())) {
        std::printf("REQUEST no-send: the request did not go out\n");
        return 1;
    }
    for (int reads = 0; reads < 20; ++reads) {
        std::ptrdiff_t const received = tcp->receive(buffer, sizeof buffer);
        if (received <= 0) {
            std::printf("REQUEST no-answer: the peer closed (received=%td) after %d read(s)\n", received, reads);
            return 1;
        }
        tls::TlsOutput out;
        if (!engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out)) {
            std::printf("REQUEST failed: %s\n", engine.error().c_str());
            return 1;
        }
        if (!out.plaintext.empty()) {
            std::string const text(out.plaintext.begin(), out.plaintext.end());
            std::printf("REQUEST answered: %s\n", text.substr(0, text.find("\r\n")).c_str());
            // The tickets may follow the answer; one more read gathers them.
            std::ptrdiff_t const more = tcp->receive(buffer, sizeof buffer);
            if (more > 0)
                engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(more)), out);
            return 0;
        }
    }
    std::printf("REQUEST no-answer: nothing readable in 20 reads\n");
    return 1;
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: tls_probe <host> [port] [--suite aes128gcm|chacha20] [--insecure] [--stall <seconds>] [--tls12] [--resume]\n");
        return 2;
    }
    // A diagnostic's lines must reach the pipe as they happen: a probe that
    // hangs has to show its last step, not swallow it in a buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options options;
    options.host = argv[1];
    bool resume = false;
    for (int i = 2; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--suite" && i + 1 < argc)
            options.suite = argv[++i];
        else if (arg == "--insecure")
            options.insecure = true;
        else if (arg == "--tls12")
            options.tls12_only = true;
        else if (arg == "--resume")
            resume = true;
        else if (arg == "--stall" && i + 1 < argc)
            options.stall = std::atoi(argv[++i]);
        else
            options.port = static_cast<std::uint16_t>(std::atoi(arg.c_str()));
    }

    Left left;
    std::printf("connection 1\n");
    int const first = connect_once(options, make_config(options), left);
    if (first != 0 || !resume)
        return first;
    if (left.tickets.empty() && !left.session) {
        std::printf("RESUME nothing: the server gave neither a ticket nor a session\n");
        return 1;
    }
    tls::TlsConfig config = make_config(options);
    if (!left.tickets.empty()) {
        config.resume_ticket = left.tickets.front();
        config.ticket_age_ms = 100;
    }
    config.resume_session = left.session;
    Left second_left;
    std::printf("connection 2\n");
    return connect_once(options, std::move(config), second_left);
}
