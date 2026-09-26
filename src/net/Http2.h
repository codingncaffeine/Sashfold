#pragma once

// HTTP/2 framing (RFC 9113): the connection preface, the nine-octet frame
// header, the ten frame types and their flags, SETTINGS and error codes,
// and the rules that make a frame malformed. Everything here is bytes in
// and bytes out; the connection that multiplexes requests over these
// frames is net/Http2Session.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::net::h2 {

// Section 3.4: what a client sends first, before its SETTINGS.
inline constexpr std::string_view client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

inline constexpr std::size_t frame_header_size = 9;
// Section 4.2: the size every peer accepts before SETTINGS says otherwise,
// and the largest SETTINGS_MAX_FRAME_SIZE may name.
inline constexpr std::uint32_t default_max_frame_size = 16384;
inline constexpr std::uint32_t largest_max_frame_size = 16777215;
// Section 6.9: a window starts at this, and may never pass 2^31 - 1.
inline constexpr std::uint32_t default_window = 65535;
inline constexpr std::int64_t largest_window = 0x7fffffff;

enum class FrameType : std::uint8_t {
    Data = 0x0,
    Headers = 0x1,
    Priority = 0x2,
    RstStream = 0x3,
    Settings = 0x4,
    PushPromise = 0x5,
    Ping = 0x6,
    Goaway = 0x7,
    WindowUpdate = 0x8,
    Continuation = 0x9,
};

namespace flags {
inline constexpr std::uint8_t end_stream = 0x1;
inline constexpr std::uint8_t ack = 0x1;
inline constexpr std::uint8_t end_headers = 0x4;
inline constexpr std::uint8_t padded = 0x8;
inline constexpr std::uint8_t priority = 0x20;
}

// Section 6.5.2.
enum class Setting : std::uint16_t {
    HeaderTableSize = 0x1,
    EnablePush = 0x2,
    MaxConcurrentStreams = 0x3,
    InitialWindowSize = 0x4,
    MaxFrameSize = 0x5,
    MaxHeaderListSize = 0x6,
};

// Section 7.
enum class ErrorCode : std::uint32_t {
    NoError = 0x0,
    ProtocolError = 0x1,
    InternalError = 0x2,
    FlowControlError = 0x3,
    SettingsTimeout = 0x4,
    StreamClosed = 0x5,
    FrameSizeError = 0x6,
    RefusedStream = 0x7,
    Cancel = 0x8,
    CompressionError = 0x9,
    ConnectError = 0xa,
    EnhanceYourCalm = 0xb,
    InadequateSecurity = 0xc,
    Http11Required = 0xd,
};

std::string_view error_name(ErrorCode code);

struct Frame {
    std::uint8_t type = 0; // a FrameType, or a type this side does not know
    std::uint8_t flags = 0;
    std::uint32_t stream = 0;
    std::vector<std::uint8_t> payload; // as on the wire, padding included

    bool is(FrameType t) const { return type == static_cast<std::uint8_t>(t); }
    bool has(std::uint8_t flag) const { return (flags & flag) != 0; }
};

// ---- writing

// A frame with its header. A payload longer than a frame may be is the
// caller's to split; these write what they are given.
void write_frame(std::vector<std::uint8_t>& out, FrameType type, std::uint8_t flags, std::uint32_t stream,
    std::span<std::uint8_t const> payload);

// DATA, padded with `padding` zero octets (and the Pad Length octet) when
// `padding` is set.
void write_data(std::vector<std::uint8_t>& out, std::uint32_t stream, std::span<std::uint8_t const> data,
    bool end_stream, std::optional<std::uint8_t> padding = std::nullopt);

// A header block as HEADERS followed by as many CONTINUATION frames as the
// peer's frame size calls for; `padding` pads the HEADERS frame.
void write_headers(std::vector<std::uint8_t>& out, std::uint32_t stream, std::span<std::uint8_t const> block,
    bool end_stream, std::uint32_t max_frame_size, std::optional<std::uint8_t> padding = std::nullopt);

void write_settings(std::vector<std::uint8_t>& out, std::span<std::pair<Setting, std::uint32_t> const> settings);
void write_settings_ack(std::vector<std::uint8_t>& out);
void write_ping(std::vector<std::uint8_t>& out, std::span<std::uint8_t const, 8> data, bool ack);
void write_goaway(std::vector<std::uint8_t>& out, std::uint32_t last_stream, ErrorCode code, std::string_view debug = {});
void write_rst_stream(std::vector<std::uint8_t>& out, std::uint32_t stream, ErrorCode code);
void write_window_update(std::vector<std::uint8_t>& out, std::uint32_t stream, std::uint32_t increment);
void write_priority(std::vector<std::uint8_t>& out, std::uint32_t stream, std::uint32_t depends_on, std::uint8_t weight);

// ---- reading

// Why a frame is refused: a connection error ends the connection with
// GOAWAY, a stream error only the stream with RST_STREAM.
struct FrameError {
    ErrorCode code = ErrorCode::NoError;
    bool connection = true;
    std::string reason;
};

// Frames out of a byte stream that arrives in any pieces. A frame longer
// than the size this side advertised is a FRAME_SIZE_ERROR (Section 4.2);
// after an error the reader stays failed.
class FrameReader {
public:
    explicit FrameReader(std::uint32_t max_frame_size = default_max_frame_size)
        : m_max_frame_size(max_frame_size)
    {
    }
    void set_max_frame_size(std::uint32_t size) { m_max_frame_size = size; }

    void feed(std::span<std::uint8_t const> bytes);
    // The next whole frame, or nothing yet (or on error()).
    std::optional<Frame> next();
    std::optional<FrameError> const& error() const { return m_error; }
    std::size_t buffered() const { return m_buffer.size() - m_at; }

private:
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_at = 0;
    std::uint32_t m_max_frame_size;
    std::optional<FrameError> m_error;
};

// The rules of Section 6 that one frame can break on its own: the stream
// it may name, its length, its padding. Nothing when it is well formed.
// A PUSH_PROMISE is refused here, as a client that sent
// SETTINGS_ENABLE_PUSH = 0 must (Section 8.4).
std::optional<FrameError> check_frame(Frame const& frame);

// The content of a DATA or HEADERS frame without its padding (and, for
// HEADERS, without the priority fields, which this client ignores). Only
// for a frame check_frame passed.
std::span<std::uint8_t const> frame_content(Frame const& frame);

// SETTINGS parameters in order, each checked against Section 6.5.2's
// bounds; an unknown identifier is skipped, as the section asks.
std::optional<std::vector<std::pair<Setting, std::uint32_t>>> parse_settings(Frame const& frame, FrameError& error);

struct Goaway {
    std::uint32_t last_stream = 0;
    ErrorCode code = ErrorCode::NoError;
    std::string debug;
};
Goaway parse_goaway(Frame const& frame);
ErrorCode parse_rst_stream(Frame const& frame);
std::uint32_t parse_window_update(Frame const& frame); // the increment
std::uint32_t read_u32(std::span<std::uint8_t const> bytes); // big-endian, the top bit kept

}
