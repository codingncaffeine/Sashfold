#include "net/WebSocket.h"

#include "core/Base64.h"
#include "crypto/Sha1.h"
#include "net/Connections.h"
#include "platform/Random.h"

#include <algorithm>
#include <cstring>

namespace sashfold::net {

namespace {

// RFC 6455 §4.2.2: what the server's Sec-WebSocket-Accept is made from.
constexpr std::string_view accept_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// The largest message put together before the connection is failed with
// 1009 (message too big): no browser takes more in one message.
constexpr std::size_t max_message = std::size_t { 64 } << 20;

constexpr std::uint8_t op_continuation = 0x0;
constexpr std::uint8_t op_text = 0x1;
constexpr std::uint8_t op_binary = 0x2;
constexpr std::uint8_t op_close = 0x8;
constexpr std::uint8_t op_ping = 0x9;
constexpr std::uint8_t op_pong = 0xA;

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
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

// Whether the bytes are well-formed UTF-8 (no overlongs, no surrogates,
// nothing past U+10FFFF), as a text message must be (RFC 6455 §8.1).
bool valid_utf8(std::span<std::uint8_t const> bytes)
{
    std::size_t i = 0;
    while (i < bytes.size()) {
        std::uint8_t const c = bytes[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            code = c & 0x1Fu;
            minimum = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            code = c & 0x0Fu;
            minimum = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            code = c & 0x07u;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + length > bytes.size())
            return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (bytes[i + k] & 0x3Fu);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

bool valid_close_code(int code)
{
    return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) || (code >= 3000 && code <= 4999);
}

}

std::shared_ptr<WebSocket> WebSocket::start(Request request, Events events)
{
    std::shared_ptr<WebSocket> socket(new WebSocket(std::move(request), std::move(events)));
    std::shared_ptr<WebSocket> held = socket;
    socket->m_thread = std::thread([held] { held->run(); });
    return socket;
}

WebSocket::WebSocket(Request request, Events events)
    : m_request(std::move(request))
    , m_events(std::move(events))
{
}

WebSocket::~WebSocket()
{
    // The reader holds the last reference while it runs, so the object ends
    // on that thread as it returns, or on the owner's once it has.
    if (m_thread.joinable()) {
        if (m_thread.get_id() == std::this_thread::get_id())
            m_thread.detach();
        else
            m_thread.join();
    }
}

void WebSocket::run()
{
    bool const secure = m_request.url.scheme == "wss";
    std::uint16_t const port = m_request.url.port.value_or(secure ? 443 : 80);
    std::string error;
    std::optional<Connection> opened = Connection::open(m_request.url.host, port, secure, error, nullptr, false);
    if (!opened) {
        if (!m_aborted && m_events.failed)
            m_events.failed(error);
        finish(1006, std::string(), false);
        return;
    }
    {
        std::lock_guard<std::mutex> const lock(m_connection_mutex);
        if (m_aborted) {
            opened->shutdown();
            return;
        }
        m_connection = std::make_unique<Connection>(std::move(*opened));
    }
    // The handshake is waited for half a minute, as a page's connection
    // attempt is; after it, the socket waits as long as the server is quiet.
    m_connection->set_receive_timeout(30000);
    std::vector<std::uint8_t> leftover;
    std::string protocol;
    std::string why;
    if (!handshake(leftover, protocol, why)) {
        if (!m_aborted && m_events.failed)
            m_events.failed(why);
        finish(1006, std::string(), false);
        return;
    }
    m_connection->set_receive_timeout(0);
    m_open = true;
    if (m_close_sent) {
        // close() came while the handshake was in flight: fail it now.
        if (!m_aborted && m_events.failed)
            m_events.failed("closed before the connection was established");
        finish(1006, std::string(), false);
        return;
    }
    if (!m_aborted && m_events.open)
        m_events.open(protocol);
    read_frames(std::move(leftover));
}

bool WebSocket::handshake(std::vector<std::uint8_t>& leftover, std::string& protocol, std::string& why)
{
    Url const& url = m_request.url;
    std::uint8_t nonce[16];
    platform::fill_random(nonce);
    std::string const key = base64_encode(nonce);
    std::string target = url.serialize_path();
    if (target.empty())
        target = "/";
    if (url.query)
        target += "?" + *url.query;
    std::string host = url.serialize_host();
    if (url.port)
        host += ":" + std::to_string(*url.port);
    std::string request = "GET " + target + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Pragma: no-cache\r\n";
    request += "Cache-Control: no-cache\r\n";
    if (!m_request.user_agent.empty())
        request += "User-Agent: " + m_request.user_agent + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Origin: " + m_request.origin + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "Accept-Encoding: gzip, deflate, br\r\n";
    request += "Accept-Language: en-US,en;q=0.9\r\n";
    if (!m_request.cookie.empty())
        request += "Cookie: " + m_request.cookie + "\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    if (!m_request.protocols.empty()) {
        std::string list;
        for (std::string const& name : m_request.protocols)
            list += (list.empty() ? "" : ", ") + name;
        request += "Sec-WebSocket-Protocol: " + list + "\r\n";
    }
    request += "\r\n";
    if (!m_connection->send_all(reinterpret_cast<std::uint8_t const*>(request.data()), request.size())) {
        why = "the request could not be sent";
        return false;
    }
    // The response's head, read up to its blank line; what follows it is
    // already frames.
    std::vector<std::uint8_t> buffer;
    std::size_t head_end = std::string::npos;
    while (head_end == std::string::npos) {
        std::uint8_t chunk[4096];
        std::ptrdiff_t const got = m_connection->receive(chunk, sizeof chunk);
        if (got <= 0) {
            why = "the server closed the connection during the handshake";
            return false;
        }
        buffer.insert(buffer.end(), chunk, chunk + got);
        std::string_view const seen(reinterpret_cast<char const*>(buffer.data()), buffer.size());
        head_end = seen.find("\r\n\r\n");
        if (head_end == std::string::npos && buffer.size() > 64 * 1024) {
            why = "the handshake response is too large";
            return false;
        }
    }
    std::string const head(reinterpret_cast<char const*>(buffer.data()), head_end);
    leftover.assign(buffer.begin() + static_cast<std::ptrdiff_t>(head_end + 4), buffer.end());
    std::vector<Header> headers;
    std::size_t line_start = 0;
    std::string status_line;
    while (line_start <= head.size()) {
        std::size_t const line_end = head.find("\r\n", line_start);
        std::string_view const line = std::string_view(head).substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
        if (status_line.empty() && line_start == 0) {
            status_line = std::string(line);
        } else if (!line.empty()) {
            std::size_t const colon = line.find(':');
            if (colon != std::string_view::npos)
                headers.push_back(Header { std::string(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))) });
        }
        if (line_end == std::string::npos)
            break;
        line_start = line_end + 2;
    }
    for (Header const& header : headers) {
        if (lower(header.name) == "set-cookie" && m_events.set_cookie)
            m_events.set_cookie(header.value);
    }
    // RFC 6455 §4.1, the client's checks of the server's answer.
    std::size_t const first_space = status_line.find(' ');
    if (first_space == std::string::npos || status_line.compare(first_space + 1, 3, "101") != 0) {
        why = "the server answered " + status_line.substr(first_space == std::string::npos ? 0 : first_space + 1) + " instead of switching protocols";
        return false;
    }
    auto const value_of = [&](std::string_view name) -> std::optional<std::string> {
        for (Header const& header : headers) {
            if (lower(header.name) == name)
                return header.value;
        }
        return std::nullopt;
    };
    std::optional<std::string> const upgrade = value_of("upgrade");
    if (!upgrade || lower(*upgrade) != "websocket") {
        why = "the server did not upgrade to websocket";
        return false;
    }
    std::optional<std::string> const connection = value_of("connection");
    if (!connection || lower(*connection).find("upgrade") == std::string::npos) {
        why = "the server's Connection header does not say upgrade";
        return false;
    }
    std::string const expected_source = key + std::string(accept_guid);
    crypto::Sha1::Digest const digest = crypto::Sha1::hash(std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(expected_source.data()), expected_source.size()));
    std::optional<std::string> const accept = value_of("sec-websocket-accept");
    if (!accept || *accept != base64_encode(digest)) {
        why = "the server's Sec-WebSocket-Accept is wrong";
        return false;
    }
    if (std::optional<std::string> const extensions = value_of("sec-websocket-extensions"); extensions && !trim(*extensions).empty()) {
        why = "the server chose an extension that was not offered";
        return false;
    }
    if (std::optional<std::string> const chosen = value_of("sec-websocket-protocol")) {
        if (std::find(m_request.protocols.begin(), m_request.protocols.end(), *chosen) == m_request.protocols.end()) {
            why = "the server chose a subprotocol that was not offered";
            return false;
        }
        protocol = *chosen;
    } else if (!m_request.protocols.empty()) {
        // The browsers fail a connection whose server picks none of the
        // protocols the page asked for.
        why = "the server chose none of the subprotocols";
        return false;
    }
    return true;
}

bool WebSocket::write_frame(std::uint8_t opcode, std::span<std::uint8_t const> payload)
{
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<std::uint8_t>(0x80 | opcode));
    if (payload.size() < 126) {
        frame.push_back(static_cast<std::uint8_t>(0x80 | payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        frame.push_back(static_cast<std::uint8_t>(payload.size()));
    } else {
        frame.push_back(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(payload.size()) >> shift));
    }
    std::uint8_t mask[4];
    platform::fill_random(mask);
    frame.insert(frame.end(), mask, mask + 4);
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<std::uint8_t>(payload[i] ^ mask[i % 4]));
    std::lock_guard<std::mutex> const lock(m_write_mutex);
    std::lock_guard<std::mutex> const connection_lock(m_connection_mutex);
    return m_connection && m_connection->send_all(frame.data(), frame.size());
}

void WebSocket::read_frames(std::vector<std::uint8_t> buffer)
{
    std::size_t at = 0;
    // Waits until `count` bytes past `at` are in the buffer; false when the
    // connection ended first.
    auto const need = [&](std::size_t count) {
        while (buffer.size() - at < count) {
            if (at > 0 && at == buffer.size()) {
                buffer.clear();
                at = 0;
            }
            std::uint8_t chunk[16384];
            std::ptrdiff_t const got = m_connection->receive(chunk, sizeof chunk);
            if (got <= 0)
                return false;
            buffer.insert(buffer.end(), chunk, chunk + got);
        }
        return true;
    };
    // Fails the connection (RFC 6455 §7.1.7): a close frame with the code,
    // when one can still be sent, then the page told of the error.
    auto const fail = [&](int code, std::string const& why) {
        if (!m_close_sent.exchange(true)) {
            std::uint8_t const payload[2] = { static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code) };
            write_frame(op_close, payload);
        }
        if (!m_aborted && m_events.failed)
            m_events.failed(why);
        finish(1006, std::string(), false);
    };
    std::vector<std::uint8_t> message;
    std::uint8_t message_opcode = 0;
    bool in_message = false;
    for (;;) {
        if (!need(2)) {
            finish(1006, std::string(), false);
            return;
        }
        std::uint8_t const first = buffer[at];
        std::uint8_t const second = buffer[at + 1];
        bool const fin = (first & 0x80) != 0;
        std::uint8_t const opcode = first & 0x0F;
        if ((first & 0x70) != 0)
            return fail(1002, "a frame set a reserved bit");
        if ((second & 0x80) != 0)
            return fail(1002, "the server masked a frame");
        std::uint64_t length = second & 0x7F;
        std::size_t header = 2;
        if (length == 126) {
            if (!need(4)) {
                finish(1006, std::string(), false);
                return;
            }
            length = (std::uint64_t { buffer[at + 2] } << 8) | buffer[at + 3];
            header = 4;
        } else if (length == 127) {
            if (!need(10)) {
                finish(1006, std::string(), false);
                return;
            }
            length = 0;
            for (std::size_t k = 0; k < 8; ++k)
                length = (length << 8) | buffer[at + 2 + k];
            header = 10;
        }
        bool const control = (opcode & 0x08) != 0;
        if (control && (length > 125 || !fin))
            return fail(1002, "a control frame was fragmented or too long");
        if (length > max_message || (!control && message.size() + length > max_message))
            return fail(1009, "a message was too big");
        if (!need(header + static_cast<std::size_t>(length))) {
            finish(1006, std::string(), false);
            return;
        }
        std::span<std::uint8_t const> const payload(buffer.data() + at + header, static_cast<std::size_t>(length));
        at += header + static_cast<std::size_t>(length);
        switch (opcode) {
        case op_continuation:
        case op_text:
        case op_binary: {
            if (opcode == op_continuation && !in_message)
                return fail(1002, "a continuation came with no message to continue");
            if (opcode != op_continuation && in_message)
                return fail(1002, "a new message began before the last one ended");
            if (opcode != op_continuation) {
                message_opcode = opcode;
                message.clear();
                in_message = true;
            }
            message.insert(message.end(), payload.begin(), payload.end());
            if (!fin)
                break;
            in_message = false;
            if (message_opcode == op_text && !valid_utf8(message))
                return fail(1007, "a text message was not UTF-8");
            if (!m_aborted && !m_close_sent && m_events.message)
                m_events.message(message_opcode == op_binary, std::move(message));
            message = {};
            break;
        }
        case op_ping:
            if (!m_close_sent)
                write_frame(op_pong, payload);
            break;
        case op_pong:
            break;
        case op_close: {
            int code = 1005;
            std::string reason;
            if (payload.size() == 1)
                return fail(1002, "a close frame had a one-byte body");
            if (payload.size() >= 2) {
                code = (payload[0] << 8) | payload[1];
                if (!valid_close_code(code))
                    return fail(1002, "a close frame had an invalid code");
                if (!valid_utf8(payload.subspan(2)))
                    return fail(1007, "a close frame's reason was not UTF-8");
                reason.assign(reinterpret_cast<char const*>(payload.data() + 2), payload.size() - 2);
            }
            // Answered with the same code when this end has not closed yet.
            if (!m_close_sent.exchange(true)) {
                std::vector<std::uint8_t> echo;
                if (code != 1005) {
                    echo.push_back(static_cast<std::uint8_t>(code >> 8));
                    echo.push_back(static_cast<std::uint8_t>(code));
                }
                write_frame(op_close, echo);
            }
            finish(code, reason, true);
            return;
        }
        default:
            return fail(1002, "a frame had an unknown opcode");
        }
    }
}

bool WebSocket::send(bool binary, std::span<std::uint8_t const> data)
{
    if (!m_open || m_close_sent)
        return false;
    return write_frame(binary ? op_binary : op_text, data);
}

void WebSocket::close(std::optional<int> code, std::string const& reason)
{
    if (!m_open) {
        // Still connecting: fail it; the reader says so when it notices.
        m_close_sent = true;
        std::lock_guard<std::mutex> const lock(m_connection_mutex);
        if (m_connection)
            m_connection->shutdown();
        return;
    }
    if (m_close_sent.exchange(true))
        return;
    std::vector<std::uint8_t> payload;
    if (code) {
        payload.push_back(static_cast<std::uint8_t>(*code >> 8));
        payload.push_back(static_cast<std::uint8_t>(*code));
        payload.insert(payload.end(), reason.begin(), reason.end());
    }
    write_frame(op_close, payload);
    // The server has a while to answer before the connection is dropped.
    std::lock_guard<std::mutex> const lock(m_connection_mutex);
    if (m_connection)
        m_connection->set_receive_timeout(10000);
}

void WebSocket::abort()
{
    m_aborted = true;
    std::lock_guard<std::mutex> const lock(m_connection_mutex);
    if (m_connection)
        m_connection->shutdown();
}

void WebSocket::finish(int code, std::string reason, bool clean)
{
    if (m_finished.exchange(true))
        return;
    m_open = false;
    {
        std::lock_guard<std::mutex> const lock(m_connection_mutex);
        if (m_connection)
            m_connection->shutdown();
    }
    if (!m_aborted && m_events.closed)
        m_events.closed(code, std::move(reason), clean);
}

}
