#pragma once

// One HTTP/2 connection (RFC 9113), carrying every request to its origin at
// once. A page's fifty requests to one host cost one handshake, their
// headers go compressed against each other, and none waits for the one
// before it to finish, as they do on an HTTP/1.1 connection, which carries
// one exchange at a time.
//
// The socket is read by one thread the session starts, which dispatches
// every incoming frame by stream: it answers SETTINGS and PING, returns
// receive windows as data arrives, and hands each stream its response.
// Requests come from the fetch threads, each of which writes its own
// HEADERS (and DATA, within the peer's windows) under the session's write
// lock and then sleeps on the session's condition variable until its
// stream has ended. The write lock is taken before the state lock, never
// after it, and nothing blocks on the socket while holding the state lock,
// so the reader is never kept from reading by a writer that waits on it.
// The reader only tries the write lock: while a writer holds it (blocked,
// perhaps, sending a large body to a peer that is itself busy sending), the
// frames the reader owes are left queued, the holder sends them before it
// lets go, and the reader goes on reading, so neither side's sends wait on
// the other's.
//
// A request's answer comes back in the shape read_response gives an
// HTTP/1.1 exchange, so everything above (redirects, cookies, the cache,
// content decoding) is the same for either protocol.

#include "net/Connections.h"
#include "net/Hpack.h"
#include "net/Http.h"
#include "net/Http2.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sashfold::net {

struct Http2Config {
    // The receive windows this side grants: per stream, through
    // SETTINGS_INITIAL_WINDOW_SIZE, and for the whole connection. The
    // defaults are the ones Chrome ships (6 MiB and 15 MiB), which keep a
    // fast link busy without a round trip for every 64 KB.
    std::uint32_t stream_window = 6u * 1024u * 1024u;
    std::uint32_t connection_window = 15u * 1024u * 1024u;
    // How long a stream whose request set no receive timeout may go with
    // nothing arriving for it before it is reset and fails. Unbounded, a
    // stream the server never finishes would hold its slot under the
    // peer's SETTINGS_MAX_CONCURRENT_STREAMS for good, and the requests
    // queued behind it with it. Five minutes is Firefox's response timeout
    // (network.http.response.timeout), long past any server that is only
    // slow.
    int stall_ms = 300000;
};

class Http2Session {
public:
    // Sends the preface and SETTINGS over an established connection and
    // starts the reader. Nothing when the preface cannot be sent.
    static std::shared_ptr<Http2Session> start(Connection connection, Http2Config config = {});

    // Ends the session (GOAWAY if it is still up) and waits for the reader.
    ~Http2Session();
    Http2Session(Http2Session const&) = delete;
    Http2Session& operator=(Http2Session const&) = delete;

    struct Request {
        std::string method = "GET";
        std::string scheme = "https";
        std::string authority; // host, and the port when the URL names one
        std::string path = "/"; // with the query
        // The fields the HTTP/1.1 exchange sends, in any case: they go
        // lowercased, without the ones HTTP/2 forbids (Section 8.2.2).
        std::vector<Header> headers;
        std::vector<std::uint8_t> body;
    };

    enum class Outcome {
        Done,
        // The server never processed the request (it went away below this
        // stream, refused it, or the connection was lost before any answer):
        // it may go out again on another connection.
        Retry,
        // The server does not speak HTTP/2 properly (no SETTINGS where its
        // preface belongs, a protocol error, HTTP_1_1_REQUIRED): the request
        // may go out again over HTTP/1.1.
        ProtocolFailure,
        Failed,
    };

    struct Result {
        Outcome outcome = Outcome::Failed;
        std::optional<RawResponse> response;
        std::string error;
        // As FetchTiming counts them for HTTP/1.1: from the request's last
        // frame to the response's first, then to its end; and what came in
        // for the stream on the wire.
        double first_byte_ms = 0;
        double body_ms = 0;
        std::size_t bytes = 0;
    };

    // One request on a new stream, waited for. It waits first for a stream
    // to be free under the peer's SETTINGS_MAX_CONCURRENT_STREAMS. A
    // positive `receive_timeout_ms` bounds how long the stream may go with
    // nothing arriving for it, as the HTTP/1.1 exchange bounds each read,
    // and how long a request body may wait on a shut send window; without
    // one, Http2Config::stall_ms does.
    Result exchange(Request const& request, std::size_t max_body, bool head, int receive_timeout_ms = 0);

    // Whether a new request may start here: the session is up and the
    // server has not said GOAWAY.
    bool accepting() const;
    // How long, in seconds, no stream has been open; zero while one is.
    std::int64_t idle_seconds(std::int64_t now) const;
    // GOAWAY with NO_ERROR, and the connection shut; streams still open
    // end with an error. The reader ends on its own.
    void close();

    // What the connection did, for tests and --bench.
    struct Stats {
        std::size_t streams = 0; // streams opened
        std::size_t window_updates_sent = 0;
        std::size_t pings_answered = 0;
        std::uint32_t peer_max_streams = 0;
        std::uint32_t peer_initial_window = 0;
        std::uint32_t peer_max_frame_size = 0;
    };
    Stats stats() const;

private:
    struct Stream;
    using Clock = std::chrono::steady_clock;

    Http2Session(Connection connection, Http2Config config);
    void run();
    // Frame handling, on the reader's thread, under m_mutex. False ends
    // the session.
    bool handle(h2::Frame const& frame);
    bool handle_data(h2::Frame const& frame);
    bool handle_settings(h2::Frame const& frame);
    bool handle_goaway(h2::Frame const& frame);
    bool finish_header_block();
    void connection_error(h2::ErrorCode code, std::string const& reason);
    void lost(std::string const& reason);
    void end_stream(Stream& stream, Outcome outcome, std::string error);
    void reset_stream(Stream& stream, h2::ErrorCode code, Outcome outcome, std::string error);
    void finish_stream(Stream& stream);
    bool opened_by_us(std::uint32_t id) const;
    // Sends the frames the reader queued, if the write lock is free; if it
    // is not, its holder sends them.
    void flush_output();
    // With the write lock held: takes the queued frames (appending them to
    // `out`) and applies a table size the peer changed, so they reach the
    // wire ahead of whatever the holder writes next.
    void take_output(std::vector<std::uint8_t>& out);
    Result result_of(Stream& stream);

    Connection m_connection;
    Http2Config m_config;
    std::thread m_reader;

    // Held while writing to the socket, and while the encoder and stream
    // ids are used, so blocks reach the wire in the order they were
    // encoded and stream ids rise with them.
    std::mutex m_write_mutex;
    hpack::Encoder m_encoder;
    std::optional<std::size_t> m_encoder_table_size; // a change the reader leaves for the next write

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::map<std::uint32_t, std::shared_ptr<Stream>> m_streams;
    std::uint32_t m_next_stream = 1;
    std::uint32_t m_highest_stream = 0;
    std::size_t m_open = 0; // streams holding a slot under the peer's limit
    bool m_dead = false;
    bool m_protocol_failure = false;
    std::string m_dead_reason;
    bool m_going_away = false;
    std::uint32_t m_goaway_last = 0;
    std::int64_t m_idle_since = 0;

    // The peer's settings.
    bool m_settings_received = false;
    std::uint32_t m_peer_max_streams = 100; // until its SETTINGS say
    std::uint32_t m_peer_initial_window = h2::default_window;
    std::uint32_t m_peer_max_frame = h2::default_max_frame_size;
    std::int64_t m_send_window = h2::default_window; // the connection's, for our DATA

    // The reader's own state.
    h2::FrameReader m_frames;
    hpack::Decoder m_decoder;
    std::uint32_t m_block_stream = 0; // a header block waiting for CONTINUATION
    bool m_block_end_stream = false;
    std::vector<std::uint8_t> m_block;
    std::int64_t m_receive_window = 0; // the connection's, for the peer's DATA
    std::uint32_t m_receive_unacked = 0;
    std::vector<std::uint8_t> m_output; // control frames the reader owes

    Stats m_stats;
};

}
