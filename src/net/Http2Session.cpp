#include "net/Http2Session.h"

#include "core/Ascii.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>

namespace sashfold::net {

namespace {

// What this side advertises: a decoder table as large as Chrome's and the
// header list bound it sends. SETTINGS_MAX_FRAME_SIZE stays at its default,
// so every frame the peer sends fits in 16 KB.
constexpr std::uint32_t advertised_table_size = 65536;
constexpr std::uint32_t advertised_header_list = 262144;
// The encoder's own table never grows past the protocol's default, whatever
// the peer allows: 4 KB already holds every field a page's requests repeat.
constexpr std::size_t encoder_table_limit = 4096;
// A header block this long is past any list the decoder would accept.
constexpr std::size_t largest_header_block = 4 * advertised_header_list;
constexpr std::size_t receive_chunk = 64 * 1024;

std::int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string lowercase(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

// Section 8.2.2: the fields that describe an HTTP/1.1 connection have no
// meaning on a stream, and a peer treats a request carrying one as
// malformed. The host goes as :authority.
bool connection_specific(std::string_view name, std::string_view value)
{
    if (name == "connection" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding"
        || name == "upgrade" || name == "host")
        return true;
    return name == "te" && !ascii_ci_equals(trim(value), "trailers");
}

// A GOAWAY or RST_STREAM with one of these says the server found this
// side's HTTP/2 wanting, or will not speak it: HTTP/1.1 is the way on.
bool says_http2_is_broken(h2::ErrorCode code)
{
    switch (code) {
    case h2::ErrorCode::ProtocolError:
    case h2::ErrorCode::FlowControlError:
    case h2::ErrorCode::FrameSizeError:
    case h2::ErrorCode::CompressionError:
    case h2::ErrorCode::InadequateSecurity:
    case h2::ErrorCode::Http11Required:
        return true;
    case h2::ErrorCode::NoError:
    case h2::ErrorCode::InternalError:
    case h2::ErrorCode::SettingsTimeout:
    case h2::ErrorCode::StreamClosed:
    case h2::ErrorCode::RefusedStream:
    case h2::ErrorCode::Cancel:
    case h2::ErrorCode::ConnectError:
    case h2::ErrorCode::EnhanceYourCalm:
        return false;
    }
    return false;
}

// RFC 9110 Section 9.2.2. A connection lost after a request went out may
// have lost it before or after the server acted on it; only these methods
// may be sent again without knowing which.
bool idempotent(std::string_view method)
{
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE" || method == "PUT"
        || method == "DELETE";
}

}

struct Http2Session::Stream {
    std::uint32_t id = 0;
    std::size_t max_body = 0;
    bool head = false;
    bool replayable = false; // the method is idempotent
    bool ended = false;
    Outcome outcome = Outcome::Done;
    std::string error;
    bool headers_done = false;
    RawResponse response;
    std::optional<std::uint64_t> content_length;
    std::int64_t send_window = 0; // the peer's, for our DATA
    std::int64_t receive_window = 0; // ours, for the peer's DATA
    std::uint32_t unacked = 0; // received since the last WINDOW_UPDATE
    Clock::time_point sent;
    Clock::time_point progress; // the last time anything arrived for it
    std::optional<Clock::time_point> first;
    Clock::time_point finished;
    std::size_t bytes = 0;
};

Http2Session::Http2Session(Connection connection, Http2Config config)
    : m_connection(std::move(connection))
    , m_config(config)
    , m_encoder(encoder_table_limit)
    , m_idle_since(unix_now())
    , m_decoder(advertised_table_size, advertised_header_list)
    , m_receive_window(h2::default_window)
{
}

std::shared_ptr<Http2Session> Http2Session::start(Connection connection, Http2Config config)
{
    if (config.stream_window == 0 || config.stream_window > h2::largest_window)
        config.stream_window = Http2Config {}.stream_window;
    if (config.connection_window < h2::default_window || config.connection_window > h2::largest_window)
        config.connection_window = std::max<std::uint32_t>(h2::default_window, Http2Config {}.connection_window);
    if (config.stall_ms <= 0)
        config.stall_ms = Http2Config {}.stall_ms;
    std::shared_ptr<Http2Session> session(new Http2Session(std::move(connection), config));

    std::vector<std::uint8_t> out(h2::client_preface.begin(), h2::client_preface.end());
    std::pair<h2::Setting, std::uint32_t> const settings[] = {
        { h2::Setting::HeaderTableSize, advertised_table_size },
        { h2::Setting::EnablePush, 0 },
        { h2::Setting::InitialWindowSize, config.stream_window },
        { h2::Setting::MaxHeaderListSize, advertised_header_list },
    };
    h2::write_settings(out, settings);
    // The connection's window starts at 64 KB whatever SETTINGS say; it
    // opens to the configured size at once.
    if (config.connection_window > h2::default_window) {
        h2::write_window_update(out, 0, config.connection_window - h2::default_window);
        session->m_receive_window = config.connection_window;
    }
    if (!session->m_connection.send_all(out.data(), out.size()))
        return nullptr;
    Http2Session* const raw = session.get();
    session->m_reader = std::thread([raw] { raw->run(); });
    return session;
}

Http2Session::~Http2Session()
{
    close();
    if (m_reader.joinable())
        m_reader.join();
}

void Http2Session::close()
{
    // A writer blocked on a peer that stopped reading holds the write lock;
    // then the GOAWAY is skipped and the shutdown below ends that write too.
    std::unique_lock<std::mutex> write(m_write_mutex, std::try_to_lock);
    bool say_goodbye = false;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        if (!m_dead) {
            say_goodbye = write.owns_lock();
            m_dead = true;
            m_dead_reason = "the HTTP/2 session was closed";
            while (!m_streams.empty()) {
                Stream& stream = *m_streams.begin()->second;
                end_stream(stream, unanswered(stream, false), m_dead_reason);
            }
        }
    }
    if (say_goodbye) {
        std::vector<std::uint8_t> out;
        h2::write_goaway(out, 0, h2::ErrorCode::NoError);
        m_connection.send_all(out.data(), out.size());
    }
    if (write.owns_lock())
        write.unlock();
    m_connection.shutdown();
    m_changed.notify_all();
}

bool Http2Session::accepting() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    return !m_dead && !m_going_away;
}

std::int64_t Http2Session::idle_seconds(std::int64_t now) const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    if (m_open > 0)
        return 0;
    return std::max<std::int64_t>(0, now - m_idle_since);
}

Http2Session::Stats Http2Session::stats() const
{
    std::lock_guard<std::mutex> const lock(m_mutex);
    Stats stats = m_stats;
    stats.peer_max_streams = m_peer_max_streams;
    stats.peer_initial_window = m_peer_initial_window;
    stats.peer_max_frame_size = m_peer_max_frame;
    return stats;
}

bool Http2Session::opened_by_us(std::uint32_t id) const
{
    return (id & 1u) == 1u && id <= m_highest_stream;
}

void Http2Session::end_stream(Stream& stream, Outcome outcome, std::string error)
{
    if (stream.ended)
        return;
    stream.ended = true;
    stream.outcome = outcome;
    stream.error = std::move(error);
    stream.finished = Clock::now();
    if (m_open > 0)
        --m_open;
    if (m_open == 0)
        m_idle_since = unix_now();
    m_streams.erase(stream.id);
}

void Http2Session::reset_stream(Stream& stream, h2::ErrorCode code, Outcome outcome, std::string error)
{
    h2::write_rst_stream(m_output, stream.id, code);
    end_stream(stream, outcome, std::move(error));
}

void Http2Session::finish_stream(Stream& stream)
{
    int const status = stream.response.status;
    bool const bodiless = stream.head || status == 204 || status == 304;
    if (stream.content_length && !bodiless && *stream.content_length != stream.response.body.size()) {
        end_stream(stream, Outcome::Failed, "an HTTP/2 response whose body is not as long as its content-length");
        return;
    }
    end_stream(stream, Outcome::Done, {});
}

void Http2Session::connection_error(h2::ErrorCode code, std::string const& reason)
{
    if (m_dead)
        return;
    h2::write_goaway(m_output, 0, code, reason);
    m_dead = true;
    m_protocol_failure = true;
    m_dead_reason = "HTTP/2 " + std::string(h2::error_name(code)) + ": " + reason;
    while (!m_streams.empty()) {
        Stream& stream = *m_streams.begin()->second;
        end_stream(stream, unanswered(stream, true), m_dead_reason);
    }
}

Http2Session::Outcome Http2Session::unanswered(Stream const& stream, bool http2_broken) const
{
    // Part of the answer came back: the server acted on the request, and
    // the rest of it cannot be had by asking again.
    if (stream.headers_done)
        return Outcome::Failed;
    // Before the server's SETTINGS nothing it was sent was read as HTTP/2,
    // so any request may go over HTTP/1.1.
    if (http2_broken && !m_settings_received)
        return Outcome::ProtocolFailure;
    // Otherwise the server may have acted on it; only a request that does
    // no harm run twice goes again.
    if (!stream.replayable)
        return Outcome::Failed;
    return http2_broken ? Outcome::ProtocolFailure : Outcome::Retry;
}

void Http2Session::lost(std::string const& reason)
{
    if (m_dead)
        return;
    m_dead = true;
    // No SETTINGS where the server's preface belongs: whatever answered,
    // it was not an HTTP/2 server.
    if (!m_settings_received)
        m_protocol_failure = true;
    m_dead_reason = reason;
    while (!m_streams.empty()) {
        Stream& stream = *m_streams.begin()->second;
        end_stream(stream, unanswered(stream, m_protocol_failure), reason);
    }
}

void Http2Session::take_output(std::vector<std::uint8_t>& out)
{
    std::optional<std::size_t> table_size;
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        out.insert(out.end(), m_output.begin(), m_output.end());
        m_output.clear();
        table_size = std::exchange(m_encoder_table_size, std::nullopt);
    }
    // The peer's new table size takes effect before its acknowledgement
    // goes out, so the first block after it begins with the update.
    if (table_size)
        m_encoder.set_max_table_size(*table_size);
}

void Http2Session::flush_output()
{
    // Whoever lets go of the write lock looks for queued frames after it
    // has, so frames queued while the lock was held are never stranded:
    // a failed try means the holder has yet to look.
    std::unique_lock<std::mutex> write(m_write_mutex, std::try_to_lock);
    while (write.owns_lock()) {
        std::vector<std::uint8_t> out;
        take_output(out);
        if (!out.empty() && !m_connection.send_all(out.data(), out.size()))
            m_connection.shutdown();
        write.unlock();
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            if (m_output.empty() && !m_encoder_table_size)
                return;
        }
        if (!write.try_lock())
            return;
    }
}

void Http2Session::run()
{
    std::vector<std::uint8_t> buffer(receive_chunk);
    while (true) {
        std::ptrdiff_t const got = m_connection.receive(buffer.data(), buffer.size());
        bool alive = true;
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            if (got <= 0) {
                lost(got == 0 ? "the server closed the HTTP/2 connection" : "the HTTP/2 connection failed");
                alive = false;
            } else if (m_dead) {
                alive = false;
            } else {
                m_frames.feed(std::span<std::uint8_t const>(buffer.data(), static_cast<std::size_t>(got)));
                while (alive) {
                    std::optional<h2::Frame> const frame = m_frames.next();
                    if (!frame)
                        break;
                    alive = handle(*frame);
                }
                if (alive && m_frames.error()) {
                    connection_error(m_frames.error()->code, m_frames.error()->reason);
                    alive = false;
                }
                // Past GOAWAY, the connection lasts as long as the streams
                // the server said it would finish.
                if (alive && m_going_away && m_streams.empty()) {
                    m_dead = true;
                    m_dead_reason = "the server went away";
                    alive = false;
                }
            }
        }
        m_changed.notify_all();
        flush_output();
        if (!alive) {
            m_connection.shutdown();
            return;
        }
    }
}

bool Http2Session::handle(h2::Frame const& frame)
{
    if (std::optional<h2::FrameError> const error = h2::check_frame(frame)) {
        if (error->connection) {
            connection_error(error->code, error->reason);
            return false;
        }
        auto const found = m_streams.find(frame.stream);
        if (found != m_streams.end())
            reset_stream(*found->second, error->code, Outcome::Failed, "HTTP/2 " + std::string(h2::error_name(error->code)) + ": " + error->reason);
        else
            h2::write_rst_stream(m_output, frame.stream, error->code);
        return true;
    }
    // Section 6.10: a header block is contiguous; nothing else may come
    // between its frames.
    if (m_block_stream != 0 && !(frame.is(h2::FrameType::Continuation) && frame.stream == m_block_stream)) {
        connection_error(h2::ErrorCode::ProtocolError, "a frame inside another stream's header block");
        return false;
    }
    // Section 3.4: the server's preface is a SETTINGS frame.
    if (!m_settings_received && !frame.is(h2::FrameType::Settings)) {
        connection_error(h2::ErrorCode::ProtocolError, "the server's preface was not SETTINGS");
        return false;
    }
    if (frame.stream != 0) {
        auto const found = m_streams.find(frame.stream);
        if (found != m_streams.end()) {
            found->second->bytes += h2::frame_header_size + frame.payload.size();
            found->second->progress = Clock::now();
        }
    }

    switch (static_cast<h2::FrameType>(frame.type)) {
    case h2::FrameType::Data:
        return handle_data(frame);
    case h2::FrameType::Headers: {
        std::span<std::uint8_t const> const content = h2::frame_content(frame);
        m_block_stream = frame.stream;
        m_block_end_stream = frame.has(h2::flags::end_stream);
        m_block.assign(content.begin(), content.end());
        if (frame.has(h2::flags::end_headers))
            return finish_header_block();
        return true;
    }
    case h2::FrameType::Continuation:
        if (m_block_stream == 0) {
            connection_error(h2::ErrorCode::ProtocolError, "CONTINUATION with no header block open");
            return false;
        }
        m_block.insert(m_block.end(), frame.payload.begin(), frame.payload.end());
        if (m_block.size() > largest_header_block) {
            connection_error(h2::ErrorCode::ProtocolError, "a header block past any list this side accepts");
            return false;
        }
        if (frame.has(h2::flags::end_headers))
            return finish_header_block();
        return true;
    case h2::FrameType::Priority:
        return true;
    case h2::FrameType::RstStream: {
        if (!opened_by_us(frame.stream)) {
            connection_error(h2::ErrorCode::ProtocolError, "RST_STREAM on a stream never opened");
            return false;
        }
        auto const found = m_streams.find(frame.stream);
        if (found == m_streams.end())
            return true;
        h2::ErrorCode const code = h2::parse_rst_stream(frame);
        Stream const& reset = *found->second;
        // REFUSED_STREAM and HTTP_1_1_REQUIRED both say the request was not
        // acted on (Sections 8.7 and 7); any other broken-protocol code may
        // come after it was.
        Outcome const outcome = code == h2::ErrorCode::RefusedStream ? Outcome::Retry
            : code == h2::ErrorCode::Http11Required  ? (reset.headers_done ? Outcome::Failed : Outcome::ProtocolFailure)
            : says_http2_is_broken(code)             ? unanswered(reset, true)
                                                     : Outcome::Failed;
        end_stream(*found->second, outcome, "the server reset the HTTP/2 stream (" + std::string(h2::error_name(code)) + ")");
        return true;
    }
    case h2::FrameType::Settings:
        return handle_settings(frame);
    case h2::FrameType::PushPromise:
        // check_frame refused it already.
        return false;
    case h2::FrameType::Ping:
        if (!frame.has(h2::flags::ack)) {
            h2::write_ping(m_output, std::span<std::uint8_t const, 8>(frame.payload.data(), 8), true);
            ++m_stats.pings_answered;
        }
        return true;
    case h2::FrameType::Goaway:
        return handle_goaway(frame);
    case h2::FrameType::WindowUpdate: {
        std::uint32_t const increment = h2::parse_window_update(frame);
        if (frame.stream == 0) {
            m_send_window += increment;
            if (m_send_window > h2::largest_window) {
                connection_error(h2::ErrorCode::FlowControlError, "the connection's send window passed 2^31 - 1");
                return false;
            }
            return true;
        }
        auto const found = m_streams.find(frame.stream);
        if (found != m_streams.end()) {
            found->second->send_window += increment;
            if (found->second->send_window > h2::largest_window)
                reset_stream(*found->second, h2::ErrorCode::FlowControlError, Outcome::Failed, "a stream's send window passed 2^31 - 1");
        }
        return true;
    }
    }
    // Section 5.5: frames of a type this side does not know are ignored.
    return true;
}

bool Http2Session::handle_data(h2::Frame const& frame)
{
    std::size_t const length = frame.payload.size();
    // Section 6.9: all of a DATA frame counts against both windows,
    // padding included.
    m_receive_window -= static_cast<std::int64_t>(length);
    if (m_receive_window < 0) {
        connection_error(h2::ErrorCode::FlowControlError, "DATA past the connection's receive window");
        return false;
    }
    m_receive_unacked += static_cast<std::uint32_t>(length);
    if (m_receive_unacked >= m_config.connection_window / 2) {
        h2::write_window_update(m_output, 0, m_receive_unacked);
        m_receive_window += m_receive_unacked;
        m_receive_unacked = 0;
        ++m_stats.window_updates_sent;
    }

    auto const found = m_streams.find(frame.stream);
    if (found == m_streams.end()) {
        // A stream this side reset or gave up on may still have data in
        // flight; one it never opened may not.
        if (!opened_by_us(frame.stream)) {
            connection_error(h2::ErrorCode::ProtocolError, "DATA on a stream never opened");
            return false;
        }
        return true;
    }
    Stream& stream = *found->second;
    if (!stream.headers_done) {
        reset_stream(stream, h2::ErrorCode::ProtocolError, Outcome::Failed, "HTTP/2 DATA before the response's headers");
        return true;
    }
    stream.receive_window -= static_cast<std::int64_t>(length);
    if (stream.receive_window < 0) {
        reset_stream(stream, h2::ErrorCode::FlowControlError, Outcome::Failed, "HTTP/2 DATA past the stream's receive window");
        return true;
    }
    std::span<std::uint8_t const> const content = h2::frame_content(frame);
    if (stream.response.body.size() + content.size() > stream.max_body) {
        reset_stream(stream, h2::ErrorCode::Cancel, Outcome::Failed, "the response body exceeds the cap");
        return true;
    }
    stream.response.body.insert(stream.response.body.end(), content.begin(), content.end());
    if (frame.has(h2::flags::end_stream)) {
        finish_stream(stream);
        return true;
    }
    // The body is consumed as it arrives (it goes straight into the
    // response), so the window goes back once half of it is used.
    stream.unacked += static_cast<std::uint32_t>(length);
    if (stream.unacked >= m_config.stream_window / 2) {
        h2::write_window_update(m_output, stream.id, stream.unacked);
        stream.receive_window += stream.unacked;
        stream.unacked = 0;
        ++m_stats.window_updates_sent;
    }
    return true;
}

bool Http2Session::finish_header_block()
{
    std::uint32_t const id = std::exchange(m_block_stream, 0);
    std::optional<std::vector<hpack::Field>> fields = m_decoder.decode(m_block);
    m_block.clear();
    if (!fields) {
        connection_error(h2::ErrorCode::CompressionError, m_decoder.error());
        return false;
    }
    auto const found = m_streams.find(id);
    if (found == m_streams.end()) {
        // The block still had to be decoded, to keep the table in step.
        if (!opened_by_us(id)) {
            connection_error(h2::ErrorCode::ProtocolError, "HEADERS on a stream never opened");
            return false;
        }
        return true;
    }
    Stream& stream = *found->second;
    if (!stream.first)
        stream.first = Clock::now();
    if (stream.headers_done) {
        // Trailers: they end the stream, and nothing in them is kept.
        if (!m_block_end_stream)
            reset_stream(stream, h2::ErrorCode::ProtocolError, Outcome::Failed, "an HTTP/2 header block after the response that does not end it");
        else
            finish_stream(stream);
        return true;
    }

    // Section 8.3.2: :status, and nothing else, before the regular fields;
    // field names in lowercase.
    int status = -1;
    bool regular_seen = false;
    bool malformed = false;
    std::vector<Header> headers;
    for (hpack::Field& field : *fields) {
        if (!field.name.empty() && field.name[0] == ':') {
            if (regular_seen || field.name != ":status" || status != -1 || field.value.size() != 3
                || !std::all_of(field.value.begin(), field.value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                malformed = true;
                break;
            }
            status = (field.value[0] - '0') * 100 + (field.value[1] - '0') * 10 + (field.value[2] - '0');
            continue;
        }
        regular_seen = true;
        if (field.name.empty() || std::any_of(field.name.begin(), field.name.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) {
            malformed = true;
            break;
        }
        headers.push_back(Header { std::move(field.name), std::move(field.value) });
    }
    if (malformed || status < 100 || status == 101 || (status < 200 && m_block_end_stream)) {
        reset_stream(stream, h2::ErrorCode::ProtocolError, Outcome::Failed, "malformed HTTP/2 response headers");
        return true;
    }
    // An interim response (100, 103) is passed over, as on HTTP/1.1.
    if (status < 200)
        return true;
    if (std::string const* const length = find_header(headers, "content-length")) {
        std::string_view const text = trim(*length);
        std::uint64_t value = 0;
        bool digits = !text.empty() && text.size() <= 19;
        for (char const c : text) {
            if (c < '0' || c > '9') {
                digits = false;
                break;
            }
            value = value * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (!digits) {
            reset_stream(stream, h2::ErrorCode::ProtocolError, Outcome::Failed, "an HTTP/2 response with a malformed content-length");
            return true;
        }
        stream.content_length = value;
    }
    stream.headers_done = true;
    stream.response.status = status;
    stream.response.headers = std::move(headers);
    if (m_block_end_stream)
        finish_stream(stream);
    return true;
}

bool Http2Session::handle_settings(h2::Frame const& frame)
{
    if (frame.has(h2::flags::ack))
        return true;
    h2::FrameError error;
    std::optional<std::vector<std::pair<h2::Setting, std::uint32_t>>> const settings = h2::parse_settings(frame, error);
    if (!settings) {
        connection_error(error.code, error.reason);
        return false;
    }
    for (auto const& [setting, value] : *settings) {
        switch (setting) {
        case h2::Setting::HeaderTableSize:
            m_encoder_table_size = std::min<std::size_t>(value, encoder_table_limit);
            break;
        case h2::Setting::MaxConcurrentStreams:
            m_peer_max_streams = value;
            break;
        case h2::Setting::InitialWindowSize: {
            // Section 6.9.2: every open stream's window moves by the change.
            std::int64_t const change = static_cast<std::int64_t>(value) - m_peer_initial_window;
            for (auto& [id, stream] : m_streams) {
                stream->send_window += change;
                if (stream->send_window > h2::largest_window) {
                    connection_error(h2::ErrorCode::FlowControlError, "SETTINGS_INITIAL_WINDOW_SIZE pushed a window past 2^31 - 1");
                    return false;
                }
            }
            m_peer_initial_window = value;
            break;
        }
        case h2::Setting::MaxFrameSize:
            m_peer_max_frame = value;
            break;
        case h2::Setting::EnablePush:
        case h2::Setting::MaxHeaderListSize:
            break;
        }
    }
    m_settings_received = true;
    h2::write_settings_ack(m_output);
    return true;
}

bool Http2Session::handle_goaway(h2::Frame const& frame)
{
    h2::Goaway const goaway = h2::parse_goaway(frame);
    m_goaway_last = m_going_away ? std::min(m_goaway_last, goaway.last_stream) : goaway.last_stream;
    m_going_away = true;
    // The streams past the last one the server names were never processed:
    // they may go again, on another connection, or over HTTP/1.1 when the
    // server's reason says its HTTP/2 and ours do not agree.
    Outcome const outcome = says_http2_is_broken(goaway.code) ? Outcome::ProtocolFailure : Outcome::Retry;
    std::string const reason = "the server went away (" + std::string(h2::error_name(goaway.code)) + ")";
    if (outcome == Outcome::ProtocolFailure)
        m_protocol_failure = true;
    std::vector<std::uint32_t> late;
    for (auto const& [id, stream] : m_streams) {
        if (id > m_goaway_last)
            late.push_back(id);
    }
    for (std::uint32_t const id : late)
        end_stream(*m_streams.at(id), outcome, reason);
    if (m_streams.empty()) {
        m_dead = true;
        m_dead_reason = reason;
        return false;
    }
    return true;
}

Http2Session::Result Http2Session::result_of(Stream& stream)
{
    using ms = std::chrono::duration<double, std::milli>;
    Result result;
    result.outcome = stream.outcome;
    result.error = stream.error;
    result.first_byte_ms = ms(stream.first.value_or(stream.finished) - stream.sent).count();
    if (stream.first)
        result.body_ms = ms(stream.finished - *stream.first).count();
    result.bytes = stream.bytes;
    if (stream.outcome == Outcome::Done)
        result.response = std::move(stream.response);
    return result;
}

Http2Session::Result Http2Session::exchange(Request const& request, std::size_t max_body, bool head, int receive_timeout_ms)
{
    std::vector<hpack::Field> fields;
    fields.reserve(request.headers.size() + 4);
    fields.push_back({ ":method", request.method, false });
    fields.push_back({ ":scheme", request.scheme, false });
    fields.push_back({ ":authority", request.authority, false });
    fields.push_back({ ":path", request.path.empty() ? std::string("/") : request.path, false });
    for (Header const& header : request.headers) {
        std::string name = lowercase(header.name);
        if (connection_specific(name, header.value))
            continue;
        fields.push_back({ std::move(name), header.value, false });
    }

    auto const refused = [this]() {
        Result result;
        result.outcome = m_protocol_failure ? Outcome::ProtocolFailure : Outcome::Retry;
        result.error = m_dead ? m_dead_reason : "the server is going away";
        return result;
    };

    auto stream = std::make_shared<Stream>();
    stream->max_body = max_body;
    stream->head = head;
    stream->replayable = idempotent(request.method);
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [&] { return m_dead || m_going_away || m_peer_max_streams == 0 || m_open < m_peer_max_streams; });
        if (m_dead || m_going_away || m_peer_max_streams == 0)
            return refused();
        ++m_open;
    }

    bool sent = false;
    {
        std::lock_guard<std::mutex> const write(m_write_mutex);
        std::uint32_t max_frame = 0;
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            if (m_dead || m_going_away || m_next_stream > 0x7fffffffu) {
                // Stream ids run out after about a billion requests: the
                // session stops taking new ones, and the pool opens another.
                if (m_next_stream > 0x7fffffffu)
                    m_going_away = true;
                --m_open;
                m_changed.notify_all();
                return refused();
            }
            stream->id = m_next_stream;
            m_next_stream += 2;
            m_highest_stream = stream->id;
            stream->send_window = m_peer_initial_window;
            stream->receive_window = m_config.stream_window;
            stream->sent = stream->progress = Clock::now();
            m_streams.emplace(stream->id, stream);
            ++m_stats.streams;
            max_frame = m_peer_max_frame;
        }
        // What the reader queued goes first: a RST_STREAM that freed the
        // slot this stream takes must reach the peer before this stream's
        // HEADERS, or the peer counts one stream too many.
        std::vector<std::uint8_t> out;
        take_output(out);
        std::vector<std::uint8_t> const block = m_encoder.encode(fields);
        h2::write_headers(out, stream->id, block, request.body.empty(), max_frame);
        sent = m_connection.send_all(out.data(), out.size());
    }
    // A failed write ends the connection; the reader sees it and ends
    // every stream, this one included.
    if (!sent)
        m_connection.shutdown();
    flush_output();

    // The body, as the peer's windows let it go. A peer that keeps a window
    // shut is held to the same allowance as a silent one: the time since
    // the body last moved or anything last arrived for the stream.
    auto const allowance = std::chrono::milliseconds(receive_timeout_ms > 0 ? receive_timeout_ms : m_config.stall_ms);
    Clock::time_point moved = Clock::now();
    bool stalled = false;
    std::size_t at = 0;
    while (sent && at < request.body.size()) {
        std::size_t chunk = 0;
        std::uint32_t id = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto const ready = [&] { return stream->ended || m_dead || (m_send_window > 0 && stream->send_window > 0); };
            while (!ready()) {
                Clock::time_point const deadline = std::max(moved, stream->progress) + allowance;
                if (Clock::now() >= deadline)
                    break;
                m_changed.wait_until(lock, deadline);
            }
            if (stream->ended || m_dead)
                break;
            if (!ready()) {
                reset_stream(*stream, h2::ErrorCode::Cancel, Outcome::Failed, "no room from the server to send the request body in the time allowed");
                m_changed.notify_all();
                stalled = true;
                break;
            }
            chunk = std::min<std::size_t>({ request.body.size() - at, static_cast<std::size_t>(m_send_window),
                static_cast<std::size_t>(stream->send_window), m_peer_max_frame });
            m_send_window -= static_cast<std::int64_t>(chunk);
            stream->send_window -= static_cast<std::int64_t>(chunk);
            id = stream->id;
        }
        std::vector<std::uint8_t> data;
        h2::write_data(data, id, std::span<std::uint8_t const>(request.body).subspan(at, chunk), at + chunk == request.body.size());
        bool wrote = false;
        {
            std::lock_guard<std::mutex> const write(m_write_mutex);
            std::vector<std::uint8_t> out;
            take_output(out);
            out.insert(out.end(), data.begin(), data.end());
            wrote = m_connection.send_all(out.data(), out.size());
        }
        flush_output();
        if (!wrote) {
            m_connection.shutdown();
            break;
        }
        at += chunk;
        moved = Clock::now();
    }
    if (stalled)
        flush_output();

    std::unique_lock<std::mutex> lock(m_mutex);
    // Section 8.1: a server may answer before the whole body is sent; the
    // rest is not wanted, and the stream is closed from this side too.
    if (sent && !stalled && at < request.body.size() && stream->ended && !m_dead) {
        h2::write_rst_stream(m_output, stream->id, h2::ErrorCode::NoError);
        lock.unlock();
        flush_output();
        lock.lock();
    }
    while (!stream->ended) {
        m_changed.wait_until(lock, stream->progress + allowance);
        if (!stream->ended && Clock::now() >= stream->progress + allowance) {
            reset_stream(*stream, h2::ErrorCode::Cancel, Outcome::Failed, "no answer from the server in the time allowed");
            m_changed.notify_all();
            lock.unlock();
            flush_output();
            lock.lock();
        }
    }
    return result_of(*stream);
}

}
