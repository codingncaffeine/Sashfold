#include "net/Http.h"

#include "core/Ascii.h"
#include "core/Inflate.h"
#include "net/Cache.h"
#include "net/Connections.h"
#include "net/Cookies.h"
#include "net/DataUrl.h"
#include "platform/Tls.h"

#include <algorithm>
#include <chrono>
#include <variant>

namespace sashfold::net {

namespace {

constexpr std::size_t receive_chunk = 64 * 1024;

std::int64_t unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Buffered reader over the read callback: line- and byte-oriented access.
class ResponseReader {
public:
    explicit ResponseReader(std::function<std::ptrdiff_t(std::uint8_t*, std::size_t)> const& read)
        : m_read(read)
    {
    }

    // Reads until CRLF (tolerating bare LF); nullopt on error/EOF-before-line.
    std::optional<std::string> read_line(std::size_t max_length = 64 * 1024)
    {
        std::string line;
        while (line.size() < max_length) {
            std::optional<std::uint8_t> const byte = next_byte();
            if (!byte)
                return std::nullopt;
            if (*byte == '\n') {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return line;
            }
            line += static_cast<char>(*byte);
        }
        return std::nullopt;
    }

    bool read_exact(std::vector<std::uint8_t>& out, std::size_t count)
    {
        while (count > 0) {
            if (m_position < m_buffer.size()) {
                std::size_t const take = std::min(count, m_buffer.size() - m_position);
                out.insert(out.end(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_position),
                    m_buffer.begin() + static_cast<std::ptrdiff_t>(m_position + take));
                m_position += take;
                count -= take;
                continue;
            }
            if (!refill())
                return false;
        }
        return true;
    }

    // Bytes received but not consumed — past the end of a delimited body,
    // they belong to nothing we asked for.
    std::size_t buffered() const { return m_buffer.size() - m_position; }

    // Reads until the peer closes, appending to out (cap enforced).
    bool read_to_close(std::vector<std::uint8_t>& out, std::size_t max_total)
    {
        while (true) {
            if (m_position < m_buffer.size()) {
                std::size_t const take = m_buffer.size() - m_position;
                if (out.size() + take > max_total)
                    return false;
                out.insert(out.end(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_position),
                    m_buffer.end());
                m_position = m_buffer.size();
            }
            if (m_closed)
                return true;
            if (!refill() && !m_closed)
                return false;
        }
    }

private:
    std::optional<std::uint8_t> next_byte()
    {
        if (m_position >= m_buffer.size() && !refill())
            return std::nullopt;
        if (m_position >= m_buffer.size())
            return std::nullopt;
        return m_buffer[m_position++];
    }

    bool refill()
    {
        if (m_closed)
            return false;
        m_buffer.resize(receive_chunk);
        m_position = 0;
        std::ptrdiff_t const received = m_read(m_buffer.data(), m_buffer.size());
        if (received < 0) {
            m_buffer.clear();
            return false;
        }
        if (received == 0) {
            m_buffer.clear();
            m_closed = true;
            return false;
        }
        m_buffer.resize(static_cast<std::size_t>(received));
        return true;
    }

    std::function<std::ptrdiff_t(std::uint8_t*, std::size_t)> const& m_read;
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_position = 0;
    bool m_closed = false;
};

std::string_view trim_ows(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

} // namespace

std::string const* find_header(std::vector<Header> const& headers, std::string_view name)
{
    for (Header const& header : headers) {
        if (ascii_ci_equals(header.name, name))
            return &header.value;
    }
    return nullptr;
}

std::string_view user_agent()
{
    // The compat-shaped token every engine ships (documented in the README).
#ifdef _WIN32
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Sashfold/" SASHFOLD_VERSION;
#elif defined(__APPLE__)
    return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Sashfold/" SASHFOLD_VERSION;
#else
    return "Mozilla/5.0 (X11; Linux x86_64) Sashfold/" SASHFOLD_VERSION;
#endif
}

std::optional<RawResponse> read_response(
    std::function<std::ptrdiff_t(std::uint8_t*, std::size_t)> const& read,
    std::size_t max_body)
{
    ResponseReader reader(read);
    RawResponse response;
    bool http11 = false;

    // The final response, past any interim 1xx ones (100 Continue, 103
    // Early Hints) — a bounded number, so a server cannot keep us here.
    int interim = 0;
    while (true) {
        std::optional<std::string> const status_line = reader.read_line();
        if (!status_line || status_line->size() < 12)
            return std::nullopt;
        if (!status_line->starts_with("HTTP/1.1 ") && !status_line->starts_with("HTTP/1.0 "))
            return std::nullopt;
        if (!is_ascii_digit(static_cast<unsigned char>((*status_line)[9]))
            || !is_ascii_digit(static_cast<unsigned char>((*status_line)[10]))
            || !is_ascii_digit(static_cast<unsigned char>((*status_line)[11])))
            return std::nullopt;
        http11 = status_line->starts_with("HTTP/1.1 ");
        response.status = ((*status_line)[9] - '0') * 100 + ((*status_line)[10] - '0') * 10
            + ((*status_line)[11] - '0');
        response.status_text.clear();
        if (status_line->size() > 13)
            response.status_text = status_line->substr(13);
        response.headers.clear();

        while (true) {
            std::optional<std::string> const line = reader.read_line();
            if (!line)
                return std::nullopt;
            if (line->empty())
                break;
            std::size_t const colon = line->find(':');
            if (colon == std::string::npos || colon == 0)
                return std::nullopt; // malformed header (or obsolete folding)
            Header header;
            header.name = line->substr(0, colon);
            for (char const c : header.name) {
                if (c == ' ' || c == '\t')
                    return std::nullopt; // no whitespace in field names
            }
            header.value = std::string(trim_ows(std::string_view(*line).substr(colon + 1)));
            response.headers.push_back(std::move(header));
            if (response.headers.size() > 500)
                return std::nullopt;
        }

        bool const is_interim
            = response.status >= 100 && response.status < 200 && response.status != 101;
        if (!is_interim)
            break;
        if (++interim > 8)
            return std::nullopt;
    }

    // Framing: 204 and 304 carry no body whatever their headers say; then
    // chunked wins over Content-Length; neither means read-to-close, which
    // uses the connection up.
    bool delimited = true;
    std::string const* const transfer_encoding
        = find_header(response.headers, "transfer-encoding");
    if (response.status == 204 || response.status == 304) {
        // Nothing to read.
    } else if (transfer_encoding
        && ascii_ci_equals(trim_ows(*transfer_encoding), "chunked")) {
        while (true) {
            std::optional<std::string> const size_line = reader.read_line();
            if (!size_line)
                return std::nullopt;
            std::string_view size_text = *size_line;
            if (std::size_t const semicolon = size_text.find(';');
                semicolon != std::string_view::npos)
                size_text = size_text.substr(0, semicolon); // chunk extensions: ignored
            size_text = trim_ows(size_text);
            if (size_text.empty())
                return std::nullopt;
            std::size_t chunk_size = 0;
            for (char const c : size_text) {
                if (!is_ascii_hex_digit(static_cast<unsigned char>(c)))
                    return std::nullopt;
                if (chunk_size > (max_body >> 4))
                    return std::nullopt;
                chunk_size = chunk_size * 16 + hex_digit_value(static_cast<unsigned char>(c));
            }
            if (chunk_size == 0)
                break;
            if (response.body.size() + chunk_size > max_body)
                return std::nullopt;
            if (!reader.read_exact(response.body, chunk_size))
                return std::nullopt;
            std::optional<std::string> const crlf = reader.read_line();
            if (!crlf || !crlf->empty())
                return std::nullopt;
        }
        // Trailer section: read and discard until the blank line.
        while (true) {
            std::optional<std::string> const trailer = reader.read_line();
            if (!trailer)
                return std::nullopt;
            if (trailer->empty())
                break;
        }
    } else if (std::string const* const content_length
        = find_header(response.headers, "content-length")) {
        std::string_view const text = trim_ows(*content_length);
        if (text.empty())
            return std::nullopt;
        std::size_t length = 0;
        for (char const c : text) {
            if (!is_ascii_digit(static_cast<unsigned char>(c)))
                return std::nullopt;
            if (length > max_body)
                return std::nullopt;
            length = length * 10 + static_cast<std::size_t>(c - '0');
        }
        if (length > max_body)
            return std::nullopt;
        if (!reader.read_exact(response.body, length))
            return std::nullopt;
    } else {
        delimited = false;
        if (!reader.read_to_close(response.body, max_body))
            return std::nullopt;
    }

    // Persistence: HTTP/1.1 keeps the connection unless a Connection header
    // says close; HTTP/1.0 closes unless one says keep-alive. Every
    // Connection header counts, token by token. Bytes past the body mean
    // the stream is out of step with us, so that connection is not trusted.
    bool close_asked = false;
    bool keep_alive_asked = false;
    for (Header const& header : response.headers) {
        if (!ascii_ci_equals(header.name, "connection"))
            continue;
        std::string_view rest = header.value;
        while (true) {
            std::size_t const comma = rest.find(',');
            std::string_view const token = trim_ows(rest.substr(0, comma));
            if (ascii_ci_equals(token, "close"))
                close_asked = true;
            else if (ascii_ci_equals(token, "keep-alive"))
                keep_alive_asked = true;
            if (comma == std::string_view::npos)
                break;
            rest.remove_prefix(comma + 1);
        }
    }
    response.keep_alive = delimited && !close_asked && (http11 || keep_alive_asked)
        && reader.buffered() == 0;
    return response;
}

std::optional<std::vector<std::uint8_t>> decode_content(std::string_view encoding,
    std::vector<std::uint8_t> const& body, std::size_t max_output)
{
    std::string_view const trimmed = trim_ows(encoding);
    if (trimmed.empty() || ascii_ci_equals(trimmed, "identity"))
        return body;
    if (ascii_ci_equals(trimmed, "gzip") || ascii_ci_equals(trimmed, "x-gzip"))
        return gzip_decompress(body, max_output);
    if (ascii_ci_equals(trimmed, "deflate")) {
        // The web's "deflate" is zlib-wrapped in the spec but shipped raw by
        // some servers; accept both.
        if (auto zlibbed = zlib_decompress(body, max_output))
            return zlibbed;
        return inflate(body, max_output);
    }
    return std::nullopt; // unknown encoding
}

FetchResult fetch(Url const& url, FetchOptions const& options)
{
    // Synthesized schemes resolve without touching the network.
    if (url.scheme == "data") {
        std::optional<DataUrlPayload> payload = parse_data_url(url);
        if (!payload)
            return { std::nullopt, "malformed data: URL" };
        if (payload->bytes.size() > options.max_body)
            return { std::nullopt, "data: URL body exceeds the cap" };
        FetchResponse response;
        response.status = 200;
        response.status_text = "OK";
        response.headers.push_back({ "Content-Type", std::move(payload->mime_type) });
        response.body = std::move(payload->bytes);
        response.final_url = url;
        return { std::move(response), "" };
    }
    if (url.scheme == "about") {
        // about:blank answers at the choke point so every pipeline can load
        // it; the shell's richer about: pages sit above fetch.
        if (url.has_opaque_path && url.serialize_path() == "blank") {
            FetchResponse response;
            response.status = 200;
            response.status_text = "OK";
            response.headers.push_back({ "Content-Type", "text/html;charset=utf-8" });
            response.final_url = url;
            return { std::move(response), "" };
        }
        return { std::nullopt, "no such about: page: " + url.serialize() };
    }

    Url current = url;
    for (int hop = 0; hop <= options.max_redirects; ++hop) {
        bool const secure = current.scheme == "https";
        if (secure && !platform::TlsSocket::available())
            return { std::nullopt,
                "https needs the platform TLS backend (our own TLS 1.3 client for Linux is "
                "not written yet)" };
        if (!secure && current.scheme != "http")
            return { std::nullopt, "unsupported scheme: " + current.scheme };
        if (!current.has_host() || current.host.empty())
            return { std::nullopt, "no host in URL" };

        // A fresh cached copy answers before any connection is made — on
        // every hop, so a redirect into a cached page costs one round trip.
        if (options.cache) {
            if (FetchResponse const* const hit = options.cache->lookup(current, unix_now()))
                return { *hit, "" };
        }

        std::uint16_t const port = current.port.value_or(secure ? 443 : 80);
        std::string const key = origin_key(secure, current.host, port);

        // An idle pooled connection to the origin first; a fresh one otherwise.
        std::optional<Connection> connection;
        bool reused = false;
        if (options.pool) {
            connection = options.pool->take(key, unix_now());
            reused = connection.has_value();
        }
        if (!connection) {
            std::string error;
            connection = Connection::open(current.host, port, secure, error);
            if (!connection)
                return { std::nullopt, std::move(error) };
            if (options.pool)
                options.pool->note_opened();
        }

        std::string target = current.serialize_path();
        if (target.empty())
            target = "/";
        if (current.query)
            target += "?" + *current.query;
        std::string request = "GET " + target + " HTTP/1.1\r\n";
        request += "Host: " + current.serialize_host();
        if (current.port) {
            request += ':';
            request += std::to_string(*current.port);
        }
        request += "\r\n";
        request += "User-Agent: " + std::string(user_agent()) + "\r\n";
        request += "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n";
        request += "Accept-Encoding: gzip, deflate\r\n";
        if (!options.referrer.empty())
            request += "Referer: " + options.referrer + "\r\n";
        if (options.cookie_jar) {
            std::string const cookies
                = options.cookie_jar->cookie_header(current, options.first_party, unix_now());
            if (!cookies.empty())
                request += "Cookie: " + cookies + "\r\n";
        }
        request += options.pool ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";

        // One request-response exchange over a connection; the returned
        // error is empty on success.
        auto const exchange = [&](Connection& over, std::optional<RawResponse>& raw) {
            if (!over.send_all(reinterpret_cast<std::uint8_t const*>(request.data()), request.size()))
                return std::string("send failed");
            auto const read = [&over](std::uint8_t* buffer, std::size_t size) {
                return over.receive(buffer, size);
            };
            raw = read_response(read, options.max_body);
            if (!raw)
                return "malformed HTTP response from " + current.serialize_host();
            return std::string();
        };
        std::optional<RawResponse> raw;
        std::string failure = exchange(*connection, raw);
        if (!raw && reused) {
            // The pooled connection was dead — the server's idle timeout won
            // the race — so the request goes out once more, on a fresh one.
            options.pool->note_retried();
            std::string error;
            connection = Connection::open(current.host, port, secure, error);
            if (!connection)
                return { std::nullopt, std::move(error) };
            options.pool->note_opened();
            failure = exchange(*connection, raw);
        }
        if (!raw)
            return { std::nullopt, std::move(failure) };

        // The connection outlives the response when the server left it open.
        if (options.pool && raw->keep_alive)
            options.pool->give(key, std::move(*connection), unix_now());
        connection.reset();

        // Set-Cookie applies on every hop, redirects included.
        if (options.cookie_jar)
            options.cookie_jar->store(current, options.first_party, raw->headers, unix_now());

        if (raw->status >= 300 && raw->status < 400) {
            if (std::string const* const location = find_header(raw->headers, "location")) {
                std::optional<Url> const next = parse_url(*location, &current);
                if (!next)
                    return { std::nullopt, "unparseable redirect Location: " + *location };
                current = *next;
                current.fragment.reset(); // fragments do not travel
                continue;
            }
        }

        FetchResponse response;
        response.status = raw->status;
        response.status_text = std::move(raw->status_text);
        response.headers = std::move(raw->headers);
        response.final_url = current;
        std::string const* const content_encoding
            = find_header(response.headers, "content-encoding");
        std::optional<std::vector<std::uint8_t>> decoded = decode_content(
            content_encoding ? *content_encoding : "", raw->body, options.max_body);
        if (!decoded)
            return { std::nullopt, "could not decode response body" };
        response.body = std::move(*decoded);
        if (options.cache && response.status == 200)
            options.cache->store(current, response, unix_now());
        return { std::move(response), "" };
    }
    return { std::nullopt, "too many redirects" };
}

}
