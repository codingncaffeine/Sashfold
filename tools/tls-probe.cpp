// tls_probe drives the TLS client against one host and prints what the engine
// itself says — the state it stopped in and error(), which Connection::open
// throws away in favour of one message for every cause.
//
//   tls_probe <host> [port] [--suite aes128gcm|chacha20] [--insecure]
//
// Dev-only, not a CMake target: it links libsashfold_core.a, so RELINK it after
// every core build or it runs old code.

#include "net/tls/Tls13.h"
#include "platform/Net.h"
#include "platform/Random.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace sashfold;

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: tls_probe <host> [port] [--suite aes128gcm|chacha20] [--insecure]\n");
        return 2;
    }
    std::string const host = argv[1];
    std::uint16_t port = 443;
    std::string suite;
    bool insecure = false;
    for (int i = 2; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--suite" && i + 1 < argc)
            suite = argv[++i];
        else if (arg == "--insecure")
            insecure = true;
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
    config.verify_chain = [insecure](std::vector<tls::Certificate> const& chain, std::string& reason) {
        std::printf("  verify_chain reached: %zu certificate(s)\n", chain.size());
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
    return engine.connected() ? 0 : 1;
}
