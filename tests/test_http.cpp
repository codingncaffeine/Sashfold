#include "Test.h"

#include "net/Http.h"
#include "platform/Net.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// The response parser on canned bytes, content decoding against the inflate
// fixtures, and the real socket path against a loopback server thread.

using namespace sashfold;

namespace {

std::filesystem::path g_fixtures;

// Wraps a canned byte buffer as a read callback.
struct CannedSource {
    std::vector<std::uint8_t> data;
    std::size_t at = 0;

    std::ptrdiff_t operator()(std::uint8_t* buffer, std::size_t size)
    {
        if (at >= data.size())
            return 0;
        std::size_t const take = std::min(size, data.size() - at);
        std::memcpy(buffer, data.data() + at, take);
        at += take;
        return static_cast<std::ptrdiff_t>(take);
    }
};

std::optional<net::RawResponse> parse_canned(std::string_view text)
{
    CannedSource source { std::vector<std::uint8_t>(text.begin(), text.end()), 0 };
    auto const read = [&source](std::uint8_t* buffer, std::size_t size) {
        return source(buffer, size);
    };
    return net::read_response(read, 1u << 20);
}

std::string body_text(net::RawResponse const& response)
{
    return std::string(response.body.begin(), response.body.end());
}

std::vector<std::uint8_t> read_file(std::filesystem::path const& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream stream;
    stream << file.rdbuf();
    std::string const text = std::move(stream).str();
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

// Serves one connection with a fixed response, then exits.
void serve_once(platform::TcpListener& listener, std::string response)
{
    auto client = listener.accept();
    if (!client)
        return;
    // Drain the request head (we only need to unblock the client).
    std::uint8_t buffer[4096];
    (void)client->receive(buffer, sizeof buffer);
    (void)client->send_all(reinterpret_cast<std::uint8_t const*>(response.data()),
        response.size());
    client->close();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: test_http <inflate-fixtures-dir>\n";
        return 2;
    }
    g_fixtures = argv[1];

    // --- Status line and headers ---------------------------------------------
    {
        auto const response = parse_canned("HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/html; charset=utf-8\r\n"
                                           "Content-Length: 5\r\n"
                                           "\r\n"
                                           "hello");
        CHECK(response.has_value());
        if (response) {
            CHECK_EQ(response->status, 200);
            CHECK_EQ(response->status_text, "OK");
            CHECK_EQ(body_text(*response), "hello");
            std::string const* const type = net::find_header(response->headers, "CONTENT-TYPE");
            CHECK(type && *type == "text/html; charset=utf-8");
        }
    }

    // --- Framing: read to close, no Content-Length ---------------------------
    {
        auto const response = parse_canned("HTTP/1.0 200 OK\r\n\r\nstreamed until the end");
        CHECK(response.has_value());
        if (response)
            CHECK_EQ(body_text(*response), "streamed until the end");
    }

    // --- Chunked, with extensions and trailers -------------------------------
    {
        auto const response = parse_canned("HTTP/1.1 200 OK\r\n"
                                           "Transfer-Encoding: chunked\r\n"
                                           "\r\n"
                                           "4;ext=1\r\nWiki\r\n"
                                           "5\r\npedia\r\n"
                                           "E\r\n in\r\n\r\nchunks.\r\n"
                                           "0\r\n"
                                           "Trailer: ignored\r\n"
                                           "\r\n");
        CHECK(response.has_value());
        if (response)
            CHECK_EQ(body_text(*response), "Wikipedia in\r\n\r\nchunks.");
    }

    // --- Malformed responses are rejected ------------------------------------
    CHECK(!parse_canned("HTTP/2 200\r\n\r\n").has_value());
    CHECK(!parse_canned("HTTP/1.1 20 OK\r\n\r\n").has_value());
    CHECK(!parse_canned("HTTP/1.1 200 OK\r\nBad Header Name: x\r\n\r\n").has_value());
    CHECK(!parse_canned("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort").has_value());
    CHECK(!parse_canned("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n").has_value());

    // --- Content decoding against the independent fixtures -------------------
    {
        auto const raw = read_file(g_fixtures / "hello.raw");
        auto const gzipped = read_file(g_fixtures / "hello.gzip");
        auto const zlibbed = read_file(g_fixtures / "hello.zlib");
        auto const deflated = read_file(g_fixtures / "hello.deflate");
        auto const from_gzip = net::decode_content("gzip", gzipped, 1u << 20);
        CHECK(from_gzip && *from_gzip == raw);
        auto const from_zlib = net::decode_content("deflate", zlibbed, 1u << 20);
        CHECK(from_zlib && *from_zlib == raw);
        auto const from_raw_deflate = net::decode_content("deflate", deflated, 1u << 20);
        CHECK(from_raw_deflate && *from_raw_deflate == raw); // the raw spelling
        CHECK(net::decode_content("identity", raw, 1u << 20).has_value());
        CHECK(!net::decode_content("br", raw, 1u << 20).has_value()); // brotli: M7
    }

    // --- The real socket path, hermetically ----------------------------------
    {
        auto listener = platform::TcpListener::listen_loopback();
        CHECK(listener.has_value());
        if (listener) {
            std::thread server(serve_once, std::ref(*listener),
                std::string("HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nover the top"));
            auto const url = net::parse_url(
                "http://127.0.0.1:" + std::to_string(listener->port()) + "/probe");
            CHECK(url.has_value());
            if (url) {
                net::FetchResult const result = net::fetch(*url);
                CHECK(result.response.has_value());
                if (result.response) {
                    CHECK_EQ(result.response->status, 200);
                    CHECK_EQ(std::string(result.response->body.begin(),
                                 result.response->body.end()),
                        "over the top");
                }
            }
            server.join();
        }
    }

    // --- A redirect chain over real sockets ----------------------------------
    {
        auto second = platform::TcpListener::listen_loopback();
        auto first = platform::TcpListener::listen_loopback();
        CHECK(first.has_value() && second.has_value());
        if (first && second) {
            std::string const hop = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:"
                + std::to_string(second->port()) + "/landed\r\nContent-Length: 0\r\n\r\n";
            std::thread first_server(serve_once, std::ref(*first), hop);
            std::thread second_server(serve_once, std::ref(*second),
                std::string("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\narrived"));
            auto const url = net::parse_url(
                "http://127.0.0.1:" + std::to_string(first->port()) + "/start");
            CHECK(url.has_value());
            if (url) {
                net::FetchResult const result = net::fetch(*url);
                CHECK(result.response.has_value());
                if (result.response) {
                    CHECK_EQ(std::string(result.response->body.begin(),
                                 result.response->body.end()),
                        "arrived");
                    CHECK_EQ(result.response->final_url.serialize(),
                        "http://127.0.0.1:" + std::to_string(second->port()) + "/landed");
                }
            }
            first_server.join();
            second_server.join();
        }
    }

    // --- https states its business plainly -----------------------------------
    {
        auto const url = net::parse_url("https://example.com/");
        CHECK(url.has_value());
        if (url) {
            net::FetchResult const result = net::fetch(*url);
            CHECK(!result.response.has_value());
            CHECK(result.error.find("https") != std::string::npos);
        }
    }

    return sashfold::test::report("http");
}
