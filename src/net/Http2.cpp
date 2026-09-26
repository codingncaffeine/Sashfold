#include "net/Http2.h"

#include <algorithm>

namespace sashfold::net::h2 {

namespace {

void put24(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_header(std::vector<std::uint8_t>& out, std::size_t length, FrameType type, std::uint8_t frame_flags,
    std::uint32_t stream)
{
    put24(out, static_cast<std::uint32_t>(length));
    out.push_back(static_cast<std::uint8_t>(type));
    out.push_back(frame_flags);
    put32(out, stream & 0x7fffffffu);
}

FrameError connection_error(ErrorCode code, std::string reason)
{
    return FrameError { code, true, std::move(reason) };
}

FrameError stream_error(ErrorCode code, std::string reason)
{
    return FrameError { code, false, std::move(reason) };
}

}

std::string_view error_name(ErrorCode code)
{
    switch (code) {
    case ErrorCode::NoError:
        return "NO_ERROR";
    case ErrorCode::ProtocolError:
        return "PROTOCOL_ERROR";
    case ErrorCode::InternalError:
        return "INTERNAL_ERROR";
    case ErrorCode::FlowControlError:
        return "FLOW_CONTROL_ERROR";
    case ErrorCode::SettingsTimeout:
        return "SETTINGS_TIMEOUT";
    case ErrorCode::StreamClosed:
        return "STREAM_CLOSED";
    case ErrorCode::FrameSizeError:
        return "FRAME_SIZE_ERROR";
    case ErrorCode::RefusedStream:
        return "REFUSED_STREAM";
    case ErrorCode::Cancel:
        return "CANCEL";
    case ErrorCode::CompressionError:
        return "COMPRESSION_ERROR";
    case ErrorCode::ConnectError:
        return "CONNECT_ERROR";
    case ErrorCode::EnhanceYourCalm:
        return "ENHANCE_YOUR_CALM";
    case ErrorCode::InadequateSecurity:
        return "INADEQUATE_SECURITY";
    case ErrorCode::Http11Required:
        return "HTTP_1_1_REQUIRED";
    }
    return "an unknown error code";
}

std::uint32_t read_u32(std::span<std::uint8_t const> bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16)
        | (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

void write_frame(std::vector<std::uint8_t>& out, FrameType type, std::uint8_t frame_flags, std::uint32_t stream,
    std::span<std::uint8_t const> payload)
{
    put_header(out, payload.size(), type, frame_flags, stream);
    out.insert(out.end(), payload.begin(), payload.end());
}

void write_data(std::vector<std::uint8_t>& out, std::uint32_t stream, std::span<std::uint8_t const> data,
    bool end_stream, std::optional<std::uint8_t> padding)
{
    std::uint8_t frame_flags = end_stream ? flags::end_stream : 0;
    std::size_t length = data.size();
    if (padding) {
        frame_flags |= flags::padded;
        length += 1 + *padding;
    }
    put_header(out, length, FrameType::Data, frame_flags, stream);
    if (padding)
        out.push_back(*padding);
    out.insert(out.end(), data.begin(), data.end());
    if (padding)
        out.insert(out.end(), *padding, 0);
}

void write_headers(std::vector<std::uint8_t>& out, std::uint32_t stream, std::span<std::uint8_t const> block,
    bool end_stream, std::uint32_t max_frame_size, std::optional<std::uint8_t> padding)
{
    std::size_t const overhead = padding ? 1u + *padding : 0u;
    std::size_t const room = max_frame_size > overhead ? max_frame_size - overhead : 1;
    std::size_t const first = std::min(block.size(), room);
    std::uint8_t frame_flags = end_stream ? flags::end_stream : 0;
    if (first == block.size())
        frame_flags |= flags::end_headers;
    if (padding)
        frame_flags |= flags::padded;
    put_header(out, first + overhead, FrameType::Headers, frame_flags, stream);
    if (padding)
        out.push_back(*padding);
    out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(first));
    if (padding)
        out.insert(out.end(), *padding, 0);
    std::size_t at = first;
    while (at < block.size()) {
        std::size_t const piece = std::min<std::size_t>(block.size() - at, max_frame_size);
        bool const last = at + piece == block.size();
        write_frame(out, FrameType::Continuation, last ? flags::end_headers : 0, stream, block.subspan(at, piece));
        at += piece;
    }
}

void write_settings(std::vector<std::uint8_t>& out, std::span<std::pair<Setting, std::uint32_t> const> settings)
{
    put_header(out, settings.size() * 6, FrameType::Settings, 0, 0);
    for (auto const& [identifier, value] : settings) {
        out.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(identifier) >> 8));
        out.push_back(static_cast<std::uint8_t>(identifier));
        put32(out, value);
    }
}

void write_settings_ack(std::vector<std::uint8_t>& out)
{
    put_header(out, 0, FrameType::Settings, flags::ack, 0);
}

void write_ping(std::vector<std::uint8_t>& out, std::span<std::uint8_t const, 8> data, bool ack)
{
    write_frame(out, FrameType::Ping, ack ? flags::ack : 0, 0, data);
}

void write_goaway(std::vector<std::uint8_t>& out, std::uint32_t last_stream, ErrorCode code, std::string_view debug)
{
    put_header(out, 8 + debug.size(), FrameType::Goaway, 0, 0);
    put32(out, last_stream & 0x7fffffffu);
    put32(out, static_cast<std::uint32_t>(code));
    out.insert(out.end(), debug.begin(), debug.end());
}

void write_rst_stream(std::vector<std::uint8_t>& out, std::uint32_t stream, ErrorCode code)
{
    put_header(out, 4, FrameType::RstStream, 0, stream);
    put32(out, static_cast<std::uint32_t>(code));
}

void write_window_update(std::vector<std::uint8_t>& out, std::uint32_t stream, std::uint32_t increment)
{
    put_header(out, 4, FrameType::WindowUpdate, 0, stream);
    put32(out, increment & 0x7fffffffu);
}

void write_priority(std::vector<std::uint8_t>& out, std::uint32_t stream, std::uint32_t depends_on, std::uint8_t weight)
{
    put_header(out, 5, FrameType::Priority, 0, stream);
    put32(out, depends_on);
    out.push_back(weight);
}

void FrameReader::feed(std::span<std::uint8_t const> bytes)
{
    if (m_at > 0 && m_at == m_buffer.size()) {
        m_buffer.clear();
        m_at = 0;
    }
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameReader::next()
{
    if (m_error || buffered() < frame_header_size)
        return std::nullopt;
    std::uint8_t const* const header = m_buffer.data() + m_at;
    std::uint32_t const length = (static_cast<std::uint32_t>(header[0]) << 16)
        | (static_cast<std::uint32_t>(header[1]) << 8) | header[2];
    if (length > m_max_frame_size) {
        m_error = connection_error(ErrorCode::FrameSizeError, "a frame longer than SETTINGS_MAX_FRAME_SIZE");
        return std::nullopt;
    }
    if (buffered() < frame_header_size + length)
        return std::nullopt;
    Frame frame;
    frame.type = header[3];
    frame.flags = header[4];
    frame.stream = read_u32(std::span<std::uint8_t const>(header + 5, 4)) & 0x7fffffffu;
    frame.payload.assign(header + frame_header_size, header + frame_header_size + length);
    m_at += frame_header_size + length;
    // What is consumed goes once it is most of the buffer, so a long
    // connection does not keep every frame it ever read.
    if (m_at > 64 * 1024 && m_at * 2 > m_buffer.size()) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_at));
        m_at = 0;
    }
    return frame;
}

std::optional<FrameError> check_frame(Frame const& frame)
{
    std::size_t const length = frame.payload.size();
    switch (static_cast<FrameType>(frame.type)) {
    case FrameType::Data:
    case FrameType::Headers: {
        bool const headers = frame.is(FrameType::Headers);
        if (frame.stream == 0)
            return connection_error(ErrorCode::ProtocolError, headers ? "HEADERS on stream 0" : "DATA on stream 0");
        std::size_t fixed = frame.has(flags::padded) ? 1 : 0;
        if (headers && frame.has(flags::priority))
            fixed += 5;
        if (length < fixed)
            return connection_error(ErrorCode::FrameSizeError, "a frame too short for its padding or priority fields");
        std::size_t const padding = frame.has(flags::padded) ? frame.payload[0] : 0;
        if (padding > length - fixed)
            return connection_error(ErrorCode::ProtocolError, "padding longer than the frame's content");
        return std::nullopt;
    }
    case FrameType::Priority:
        if (frame.stream == 0)
            return connection_error(ErrorCode::ProtocolError, "PRIORITY on stream 0");
        if (length != 5)
            return stream_error(ErrorCode::FrameSizeError, "PRIORITY that is not five octets");
        return std::nullopt;
    case FrameType::RstStream:
        if (frame.stream == 0)
            return connection_error(ErrorCode::ProtocolError, "RST_STREAM on stream 0");
        if (length != 4)
            return connection_error(ErrorCode::FrameSizeError, "RST_STREAM that is not four octets");
        return std::nullopt;
    case FrameType::Settings:
        if (frame.stream != 0)
            return connection_error(ErrorCode::ProtocolError, "SETTINGS on a stream");
        if (frame.has(flags::ack) && length != 0)
            return connection_error(ErrorCode::FrameSizeError, "a SETTINGS acknowledgement with a payload");
        if (length % 6 != 0)
            return connection_error(ErrorCode::FrameSizeError, "SETTINGS whose length is not a multiple of six");
        return std::nullopt;
    case FrameType::PushPromise:
        return connection_error(ErrorCode::ProtocolError, "PUSH_PROMISE after SETTINGS_ENABLE_PUSH = 0");
    case FrameType::Ping:
        if (frame.stream != 0)
            return connection_error(ErrorCode::ProtocolError, "PING on a stream");
        if (length != 8)
            return connection_error(ErrorCode::FrameSizeError, "PING that is not eight octets");
        return std::nullopt;
    case FrameType::Goaway:
        if (frame.stream != 0)
            return connection_error(ErrorCode::ProtocolError, "GOAWAY on a stream");
        if (length < 8)
            return connection_error(ErrorCode::FrameSizeError, "GOAWAY shorter than eight octets");
        return std::nullopt;
    case FrameType::WindowUpdate:
        if (length != 4)
            return connection_error(ErrorCode::FrameSizeError, "WINDOW_UPDATE that is not four octets");
        if ((read_u32(frame.payload) & 0x7fffffffu) == 0) {
            if (frame.stream == 0)
                return connection_error(ErrorCode::ProtocolError, "a WINDOW_UPDATE of zero for the connection");
            return stream_error(ErrorCode::ProtocolError, "a WINDOW_UPDATE of zero");
        }
        return std::nullopt;
    case FrameType::Continuation:
        if (frame.stream == 0)
            return connection_error(ErrorCode::ProtocolError, "CONTINUATION on stream 0");
        return std::nullopt;
    }
    // Section 5.5: a type this side does not know is ignored.
    return std::nullopt;
}

std::span<std::uint8_t const> frame_content(Frame const& frame)
{
    std::span<std::uint8_t const> content(frame.payload);
    std::size_t padding = 0;
    if (frame.has(flags::padded)) {
        padding = content[0];
        content = content.subspan(1);
    }
    if (frame.is(FrameType::Headers) && frame.has(flags::priority))
        content = content.subspan(5);
    return content.first(content.size() - padding);
}

std::optional<std::vector<std::pair<Setting, std::uint32_t>>> parse_settings(Frame const& frame, FrameError& error)
{
    std::vector<std::pair<Setting, std::uint32_t>> settings;
    for (std::size_t at = 0; at + 6 <= frame.payload.size(); at += 6) {
        std::uint16_t const identifier = static_cast<std::uint16_t>((frame.payload[at] << 8) | frame.payload[at + 1]);
        std::uint32_t const value = read_u32(std::span<std::uint8_t const>(frame.payload).subspan(at + 2, 4));
        switch (static_cast<Setting>(identifier)) {
        case Setting::EnablePush:
            // A server has no use for the setting; a value that is not a
            // boolean is broken whoever sends it. (Section 6.5.2 also has
            // a client refuse a server's 1, which shipping clients let
            // pass, since a server never pushes past our 0 anyway.)
            if (value > 1) {
                error = connection_error(ErrorCode::ProtocolError, "SETTINGS_ENABLE_PUSH that is not 0 or 1");
                return std::nullopt;
            }
            break;
        case Setting::InitialWindowSize:
            if (value > largest_window) {
                error = connection_error(ErrorCode::FlowControlError, "SETTINGS_INITIAL_WINDOW_SIZE above 2^31 - 1");
                return std::nullopt;
            }
            break;
        case Setting::MaxFrameSize:
            if (value < default_max_frame_size || value > largest_max_frame_size) {
                error = connection_error(ErrorCode::ProtocolError, "SETTINGS_MAX_FRAME_SIZE outside its bounds");
                return std::nullopt;
            }
            break;
        case Setting::HeaderTableSize:
        case Setting::MaxConcurrentStreams:
        case Setting::MaxHeaderListSize:
            break;
        default:
            continue;
        }
        settings.emplace_back(static_cast<Setting>(identifier), value);
    }
    return settings;
}

Goaway parse_goaway(Frame const& frame)
{
    std::span<std::uint8_t const> const payload(frame.payload);
    Goaway goaway;
    goaway.last_stream = read_u32(payload) & 0x7fffffffu;
    goaway.code = static_cast<ErrorCode>(read_u32(payload.subspan(4)));
    goaway.debug.assign(payload.begin() + 8, payload.end());
    return goaway;
}

ErrorCode parse_rst_stream(Frame const& frame)
{
    return static_cast<ErrorCode>(read_u32(frame.payload));
}

std::uint32_t parse_window_update(Frame const& frame)
{
    return read_u32(frame.payload) & 0x7fffffffu;
}

}
