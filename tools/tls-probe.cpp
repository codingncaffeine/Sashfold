// tls_probe drives the TLS client against one host and prints what the engine
// itself says — the state it stopped in and error(), which Connection::open
// throws away in favour of one message for every cause.
//
//   tls_probe <host> [port] [--suite aes128gcm|chacha20] [--insecure] [--stall <seconds>]
//
// --stall sleeps inside the chain verification, where the real client spends
// its time between the server's flight and its own Finished, and then sends
// a GET and reports whether the server still answers: the way to measure how
// long a server waits for a slow client before hanging up.
//
// Dev-only, not a CMake target: it links libsashfold_core.a, so RELINK it after
// every core build or it runs old code.

#include "net/tls/Tls13.h"
#include "platform/Net.h"
#include "platform/Random.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace sashfold;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: tls_probe <host> [port] [--suite aes128gcm|chacha20] [--insecure] [--stall <seconds>]\n");
        return 2;
    }
    std::string const host = argv[1];
    std::uint16_t port = 443;
    std::string suite;
    bool insecure = false;
    int stall = 0;
    for (int i = 2; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--suite" && i + 1 < argc)
            suite = argv[++i];
        else if (arg == "--insecure")
            insecure = true;
        else if (arg == "--stall" && i + 1 < argc)
            stall = std::atoi(argv[++i]);
        else
            port = static_cast<std::uint16_t>(std::atoi(arg.c_str()));
    }

    tls::TlsConfig config;
    config.server_name = host;
    platform::fill_random(std::span<std::uint8_t>(config.client_random.data(), config.client_random.size()));
    platform::fill_random(std::span<std::uint8_t>(config.session_id.data(), config.session_id.size()));
    platform::fill_random(std::span<std::uint8_t>(config.private_key.data(), config.private_key.size()));
    if (suite == "aes128gcm")
        config.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
    else if (suite == "chacha20")
        config.cipher_suites = { tls::CipherSuite::ChaCha20Poly1305Sha256 };
    // The chain is accepted or refused here, and the reason is printed either
    // way, so a validation failure is never confused with a handshake one.
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

    auto tcp = platform::TcpSocket::connect(host, port);
    if (!tcp) {
        std::printf("RESULT no-tcp: could not connect to %s:%u\n", host.c_str(), port);
        return 1;
    }
    tls::TlsEngine engine(std::move(config));
    std::vector<std::uint8_t> const hello = engine.start();
    std::printf("  hello %zu bytes\n", hello.size());
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
    std::printf("RESULT %s state=%d retries=%d suite=%s error=\"%s\"\n",
        engine.connected() ? "connected" : "failed", static_cast<int>(engine.state()), engine.hello_retry_requests(),
        tls::cipher_suite_name(engine.cipher_suite()), engine.error().c_str());
    if (!engine.connected())
        return 1;
    // A request over the connection, and what comes back: the status line,
    // or nothing at all when the server has already hung up.
    std::string const request = "GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
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
            return 0;
        }
    }
    std::printf("REQUEST no-answer: nothing readable in 20 reads\n");
    return 1;
}
