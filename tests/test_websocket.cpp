// net::WebSocket (RFC 6455) against a server written here on the loopback:
// the handshake's request and the server's accept key, a text and a binary
// message echoed, a message the server fragments, a ping answered with its
// pong, and both closing handshakes — the server's and the client's — with
// their codes; then the client's checks of a server that breaks the rules
// (a wrong accept key, a masked frame, invalid UTF-8) failing the
// connection with 1006.
#include "Test.h"

#include "core/Base64.h"
#include "crypto/Sha1.h"
#include "net/WebSocket.h"
#include "platform/Net.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace sashfold;

namespace {

// What the client told: every event in order, as text.
struct Log {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> events;
    bool closed = false;

    void add(std::string line)
    {
        std::lock_guard<std::mutex> const lock(mutex);
        events.push_back(std::move(line));
        changed.notify_all();
    }
    void close(std::string line)
    {
        std::lock_guard<std::mutex> const lock(mutex);
        events.push_back(std::move(line));
        closed = true;
        changed.notify_all();
    }
    bool wait_closed()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [this] { return closed; });
    }
    bool wait_count(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [&] { return events.size() >= count; });
    }
};

net::WebSocket::Events events_for(Log& log)
{
    net::WebSocket::Events events;
    events.open = [&log](std::string const& protocol) { log.add("open " + protocol); };
    events.message = [&log](bool binary, std::vector<std::uint8_t> data) {
        log.add(std::string(binary ? "binary " : "text ") + std::string(data.begin(), data.end()));
    };
    events.failed = [&log](std::string const&) { log.add("failed"); };
    events.closed = [&log](int code, std::string reason, bool clean) {
        log.close("closed " + std::to_string(code) + " " + reason + (clean ? " clean" : " unclean"));
    };
    return events;
}

// The server's side of a connection, as a test drives it.
struct Peer {
    platform::TcpSocket socket;
    std::vector<std::uint8_t> buffer;

    bool fill(std::size_t count)
    {
        while (buffer.size() < count) {
            std::uint8_t chunk[4096];
            std::ptrdiff_t const got = socket.receive(chunk, sizeof chunk);
            if (got <= 0)
                return false;
            buffer.insert(buffer.end(), chunk, chunk + got);
        }
        return true;
    }

    // The client's request head; nullopt when it never came.
    std::optional<std::string> request()
    {
        for (;;) {
            std::string const seen(buffer.begin(), buffer.end());
            if (std::size_t const end = seen.find("\r\n\r\n"); end != std::string::npos) {
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(end + 4));
                return seen.substr(0, end);
            }
            if (!fill(buffer.size() + 1))
                return std::nullopt;
        }
    }

    // One frame from the client: its opcode and unmasked payload.
    std::optional<std::pair<int, std::string>> frame(bool& masked)
    {
        if (!fill(2))
            return std::nullopt;
        int const opcode = buffer[0] & 0x0F;
        masked = (buffer[1] & 0x80) != 0;
        std::size_t length = buffer[1] & 0x7F;
        std::size_t header = 2;
        if (length == 126) {
            if (!fill(4))
                return std::nullopt;
            length = (std::size_t { buffer[2] } << 8) | buffer[3];
            header = 4;
        }
        std::size_t const mask_at = header;
        if (masked)
            header += 4;
        if (!fill(header + length))
            return std::nullopt;
        std::string payload;
        for (std::size_t i = 0; i < length; ++i)
            payload.push_back(static_cast<char>(buffer[header + i] ^ (masked ? buffer[mask_at + i % 4] : 0)));
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(header + length));
        return std::pair { opcode, payload };
    }

    void send_frame(int opcode, std::string const& payload, bool fin = true, bool mask = false)
    {
        std::vector<std::uint8_t> out { static_cast<std::uint8_t>((fin ? 0x80 : 0) | opcode) };
        out.push_back(static_cast<std::uint8_t>((mask ? 0x80 : 0) | payload.size()));
        if (mask)
            out.insert(out.end(), { 0, 0, 0, 0 });
        out.insert(out.end(), payload.begin(), payload.end());
        socket.send_all(out.data(), out.size());
    }
};

std::string header_value(std::string const& head, std::string const& name)
{
    std::size_t const at = head.find("\r\n" + name + ": ");
    if (at == std::string::npos)
        return std::string();
    std::size_t const start = at + 4 + name.size();
    return head.substr(start, head.find("\r\n", start) - start);
}

std::string accept_for(std::string const& key)
{
    std::string const source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return base64_encode(crypto::Sha1::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(source.data()), source.size())));
}

void answer(Peer& peer, std::string const& head, std::string const& accept_override = {}, std::string const& protocol = {})
{
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
        + (accept_override.empty() ? accept_for(header_value(head, "Sec-WebSocket-Key")) : accept_override) + "\r\n";
    if (!protocol.empty())
        response += "Sec-WebSocket-Protocol: " + protocol + "\r\n";
    response += "\r\n";
    peer.socket.send_all(reinterpret_cast<std::uint8_t const*>(response.data()), response.size());
}

net::WebSocket::Request request_to(std::uint16_t port, std::vector<std::string> protocols = {})
{
    net::WebSocket::Request request;
    request.url = *net::parse_url("ws://127.0.0.1:" + std::to_string(port) + "/chat?room=1");
    request.protocols = std::move(protocols);
    request.origin = "http://example.test";
    request.user_agent = "Sashfold-test";
    return request;
}

void test_conversation()
{
    std::optional<platform::TcpListener> listener = platform::TcpListener::listen_loopback(0);
    CHECK(listener.has_value());
    std::string head;
    bool all_masked = true;
    std::vector<std::string> seen_by_server;
    std::thread server([&] {
        std::optional<platform::TcpSocket> accepted = listener->accept();
        if (!accepted)
            return;
        Peer peer { std::move(*accepted), {} };
        head = peer.request().value_or("");
        answer(peer, head, {}, "chat");
        bool masked = false;
        // Echo two messages, then a fragmented one and a ping.
        for (int i = 0; i < 2; ++i) {
            std::optional<std::pair<int, std::string>> const frame = peer.frame(masked);
            all_masked = all_masked && masked;
            if (!frame)
                return;
            seen_by_server.push_back(std::to_string(frame->first) + ":" + frame->second);
            peer.send_frame(frame->first, frame->second);
        }
        peer.send_frame(0x1, "frag", false);
        peer.send_frame(0x0, "mented", true);
        peer.send_frame(0x9, "are you there");
        std::optional<std::pair<int, std::string>> const pong = peer.frame(masked);
        if (pong)
            seen_by_server.push_back(std::to_string(pong->first) + ":" + pong->second);
        // The server closes with 1000 "bye" and waits for the echo.
        peer.send_frame(0x8, std::string("\x03\xe8", 2) + "bye");
        std::optional<std::pair<int, std::string>> const echo = peer.frame(masked);
        if (echo)
            seen_by_server.push_back(std::to_string(echo->first) + ":" + std::to_string((static_cast<unsigned char>(echo->second[0]) << 8) | static_cast<unsigned char>(echo->second[1])));
    });
    Log log;
    std::shared_ptr<net::WebSocket> socket = net::WebSocket::start(request_to(listener->port(), { "chat", "superchat" }), events_for(log));
    CHECK(log.wait_count(1));
    CHECK(socket->send(false, std::vector<std::uint8_t> { 'h', 'i' }));
    CHECK(socket->send(true, std::vector<std::uint8_t> { 1, 2, 3 }));
    CHECK(log.wait_closed());
    socket->abort(); // a server still waiting on a client that failed the check ends
    server.join();
    CHECK(head.starts_with("GET /chat?room=1 HTTP/1.1\r\n"));
    CHECK_EQ(header_value(head, "Upgrade"), "websocket");
    CHECK_EQ(header_value(head, "Sec-WebSocket-Version"), "13");
    CHECK_EQ(header_value(head, "Origin"), "http://example.test");
    CHECK_EQ(header_value(head, "Sec-WebSocket-Protocol"), "chat, superchat");
    CHECK_EQ(header_value(head, "Sec-WebSocket-Key").size(), 24u);
    CHECK(all_masked);
    CHECK_EQ(seen_by_server.size(), 4u);
    if (seen_by_server.size() == 4) {
        CHECK_EQ(seen_by_server[0], "1:hi");
        CHECK_EQ(seen_by_server[1], std::string("2:\x01\x02\x03"));
        CHECK_EQ(seen_by_server[2], "10:are you there");
        CHECK_EQ(seen_by_server[3], "8:1000");
    }
    std::lock_guard<std::mutex> const lock(log.mutex);
    CHECK_EQ(log.events.size(), 5u);
    if (log.events.size() == 5) {
        CHECK_EQ(log.events[0], "open chat");
        CHECK_EQ(log.events[1], "text hi");
        CHECK_EQ(log.events[2], std::string("binary \x01\x02\x03"));
        CHECK_EQ(log.events[3], "text fragmented");
        CHECK_EQ(log.events[4], "closed 1000 bye clean");
    }
}

// The client closes: its close frame carries the code and reason, and the
// server's echo ends the connection cleanly.
void test_client_close()
{
    std::optional<platform::TcpListener> listener = platform::TcpListener::listen_loopback(0);
    std::string close_payload;
    std::thread server([&] {
        std::optional<platform::TcpSocket> accepted = listener->accept();
        if (!accepted)
            return;
        Peer peer { std::move(*accepted), {} };
        answer(peer, peer.request().value_or(""));
        bool masked = false;
        std::optional<std::pair<int, std::string>> const frame = peer.frame(masked);
        if (!frame || frame->first != 0x8)
            return;
        close_payload = frame->second;
        peer.send_frame(0x8, frame->second);
    });
    Log log;
    std::shared_ptr<net::WebSocket> socket = net::WebSocket::start(request_to(listener->port()), events_for(log));
    CHECK(log.wait_count(1));
    socket->close(4001, "done here");
    CHECK(log.wait_closed());
    socket->abort(); // a server still waiting on a client that failed the check ends
    server.join();
    CHECK_EQ(close_payload, std::string("\x0f\xa1", 2) + "done here");
    std::lock_guard<std::mutex> const lock(log.mutex);
    CHECK_EQ(log.events.back(), "closed 4001 done here clean");
}

// A server that breaks the protocol is failed: an error, then 1006.
void test_failures()
{
    enum class Fault { WrongAccept, MaskedFrame, BadText, NoProtocol };
    for (Fault const fault : { Fault::WrongAccept, Fault::MaskedFrame, Fault::BadText, Fault::NoProtocol }) {
        std::optional<platform::TcpListener> listener = platform::TcpListener::listen_loopback(0);
        std::thread server([&] {
            std::optional<platform::TcpSocket> accepted = listener->accept();
            if (!accepted)
                return;
            Peer peer { std::move(*accepted), {} };
            std::string const head = peer.request().value_or("");
            answer(peer, head, fault == Fault::WrongAccept ? "AAAAAAAAAAAAAAAAAAAAAAAAAAA=" : std::string());
            if (fault == Fault::MaskedFrame)
                peer.send_frame(0x1, "masked", true, true);
            if (fault == Fault::BadText)
                peer.send_frame(0x1, std::string("\xc3\x28", 2));
            bool masked = false;
            peer.frame(masked); // the client's close, or the end
        });
        Log log;
        std::shared_ptr<net::WebSocket> socket = net::WebSocket::start(
            request_to(listener->port(), fault == Fault::NoProtocol ? std::vector<std::string> { "chat" } : std::vector<std::string> {}),
            events_for(log));
        CHECK(log.wait_closed());
        socket->abort();
        server.join();
        std::lock_guard<std::mutex> const lock(log.mutex);
        CHECK(!log.events.empty() && log.events.back() == "closed 1006  unclean");
        CHECK(std::find(log.events.begin(), log.events.end(), "failed") != log.events.end());
    }
}

}

int main()
{
    test_conversation();
    test_client_close();
    test_failures();
    return sashfold::test::report("websocket");
}
