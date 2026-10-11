#pragma once
// A WebSocket client (RFC 6455) on a connection of its own: the opening
// handshake over HTTP/1.1 (a TLS connection offers only http/1.1, as the
// browsers' WebSocket over TLS does without the HTTP/2 extended CONNECT),
// then frames both ways — the client's masked, the server's checked, a
// message put together from its fragments, text checked as UTF-8, a ping
// answered, the closing handshake — until either end closes.
//
// One thread per socket reads: it connects, shakes hands and then waits on
// the connection, telling its owner what happens through the events below,
// on that thread. Sends are written from the caller's thread under a lock.
// No extension is offered, so none is in use (permessage-deflate is not
// written); a server that answers with one is refused, as RFC 6455 says.
#include "net/Http.h"
#include "net/Url.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace sashfold::net {

class Connection;

class WebSocket : public std::enable_shared_from_this<WebSocket> {
public:
    struct Request {
        Url url; // ws: or wss:
        std::vector<std::string> protocols;
        std::string origin; // the page's, serialized
        std::string cookie; // the Cookie header for the URL, or empty
        std::string user_agent;
    };
    // Told on the socket's thread, in this order: open once the server has
    // agreed; a message for each that arrives; and closed last, always —
    // `clean` when the closing handshake completed, with the code and reason
    // the server gave (1005 for none, 1006 for a connection that ended
    // without one). failed comes before closed when the connection failed
    // (the page's "error" event). set_cookie is each Set-Cookie line of the
    // handshake's response.
    struct Events {
        std::function<void(std::string const& protocol)> open;
        std::function<void(bool binary, std::vector<std::uint8_t> data)> message;
        std::function<void(std::string const& why)> failed;
        std::function<void(int code, std::string reason, bool clean)> closed;
        std::function<void(std::string const& line)> set_cookie;
    };

    static std::shared_ptr<WebSocket> start(Request request, Events events);
    ~WebSocket();
    WebSocket(WebSocket const&) = delete;
    WebSocket& operator=(WebSocket const&) = delete;

    // A message sent whole; false when the socket is not open (any more).
    bool send(bool binary, std::span<std::uint8_t const> data);
    // Begins the closing handshake (RFC 6455 §7.1.2) with this code and
    // reason, or fails a connection still opening; closed follows.
    void close(std::optional<int> code, std::string const& reason);
    // Ends at once, telling nobody: the owner is going away.
    void abort();

private:
    WebSocket(Request request, Events events);
    void run();
    bool handshake(std::vector<std::uint8_t>& leftover, std::string& protocol, std::string& why);
    void read_frames(std::vector<std::uint8_t> leftover);
    bool write_frame(std::uint8_t opcode, std::span<std::uint8_t const> payload);
    void finish(int code, std::string reason, bool clean);

    Request m_request;
    Events m_events;
    std::thread m_thread;
    std::mutex m_write_mutex; // one frame at a time onto the connection
    std::unique_ptr<Connection> m_connection;
    std::mutex m_connection_mutex; // guards m_connection's arrival and shutdown
    std::atomic<bool> m_open { false };
    std::atomic<bool> m_close_sent { false };
    std::atomic<bool> m_aborted { false };
    std::atomic<bool> m_finished { false };
};

}
