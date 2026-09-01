#include "Test.h"

#include "core/Base64.h"
#include "net/DataUrl.h"
#include "net/Http.h"
#include "net/Url.h"

#include <string>

using namespace sashfold;

namespace {

std::string bytes_to_string(std::vector<std::uint8_t> const& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

net::DataUrlPayload must_parse(std::string const& input)
{
    auto const url = net::parse_url(input);
    CHECK(url.has_value());
    auto payload = net::parse_data_url(*url);
    CHECK(payload.has_value());
    return payload ? *payload : net::DataUrlPayload {};
}

}

int main()
{
    // Forgiving base64: plain, padded, whitespace-riddled, and refused forms.
    {
        auto const abc = base64_decode("YWJj");
        CHECK(abc && bytes_to_string(*abc) == "abc");
        auto const padded = base64_decode("YWI=");
        CHECK(padded && bytes_to_string(*padded) == "ab");
        auto const one = base64_decode("YQ==");
        CHECK(one && bytes_to_string(*one) == "a");
        auto const spaced = base64_decode(" Y W\tJ\nj ");
        CHECK(spaced && bytes_to_string(*spaced) == "abc");
        auto const unpadded = base64_decode("YWI"); // length % 4 == 3, no padding
        CHECK(unpadded && bytes_to_string(*unpadded) == "ab");
        CHECK(!base64_decode("Y").has_value()); // length % 4 == 1
        CHECK(!base64_decode("YW*j").has_value()); // outside the alphabet
        auto const empty = base64_decode("");
        CHECK(empty && empty->empty());
    }

    // data: URL forms.
    {
        auto const payload = must_parse("data:,Hello%2C%20World%21");
        CHECK_EQ(payload.mime_type, std::string("text/plain;charset=US-ASCII"));
        CHECK_EQ(bytes_to_string(payload.bytes), std::string("Hello, World!"));
    }
    {
        auto const payload = must_parse("data:text/html,<h1>hi</h1>");
        CHECK_EQ(payload.mime_type, std::string("text/html"));
        CHECK_EQ(bytes_to_string(payload.bytes), std::string("<h1>hi</h1>"));
    }
    {
        auto const payload = must_parse("data:text/plain;base64,SGVsbG8=");
        CHECK_EQ(payload.mime_type, std::string("text/plain"));
        CHECK_EQ(bytes_to_string(payload.bytes), std::string("Hello"));
    }
    {
        // Case-insensitive base64 marker, spaces before it, charset kept.
        auto const payload = must_parse("data:text/plain;charset=utf-8;  Base64,SGk=");
        CHECK_EQ(payload.mime_type, std::string("text/plain;charset=utf-8"));
        CHECK_EQ(bytes_to_string(payload.bytes), std::string("Hi"));
    }
    {
        // A bare parameter grows the text/plain prefix.
        auto const payload = must_parse("data:;charset=utf-8,hi");
        CHECK_EQ(payload.mime_type, std::string("text/plain;charset=utf-8"));
    }
    {
        // The query is part of the payload; the fragment is not.
        auto const payload = must_parse("data:,body?also-body#not-body");
        CHECK_EQ(bytes_to_string(payload.bytes), std::string("body?also-body"));
    }
    {
        // No comma, or bad base64: refused.
        auto const no_comma = net::parse_url("data:text/plain");
        CHECK(no_comma && !net::parse_data_url(*no_comma).has_value());
        auto const bad = net::parse_url("data:;base64,@@@");
        CHECK(bad && !net::parse_data_url(*bad).has_value());
    }

    // Through the choke point: data: and about:blank fetch without a network.
    {
        auto const url = net::parse_url("data:text/html;base64,PGgxPmhpPC9oMT4=");
        CHECK(url.has_value());
        net::FetchResult const result = net::fetch(*url);
        CHECK(result.response.has_value());
        if (result.response) {
            CHECK_EQ(result.response->status, 200);
            CHECK_EQ(bytes_to_string(result.response->body), std::string("<h1>hi</h1>"));
            auto const* type = net::find_header(result.response->headers, "content-type");
            CHECK(type && *type == "text/html");
        }
    }
    {
        auto const url = net::parse_url("about:blank");
        CHECK(url.has_value());
        net::FetchResult const result = net::fetch(*url);
        CHECK(result.response.has_value());
        if (result.response) {
            CHECK_EQ(result.response->status, 200);
            CHECK(result.response->body.empty());
        }
        auto const missing = net::parse_url("about:nonexistent");
        CHECK(missing && !net::fetch(*missing).response.has_value());
    }

    return sashfold::test::report("data_url");
}
