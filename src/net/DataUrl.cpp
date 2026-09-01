#include "net/DataUrl.h"

#include "core/Ascii.h"
#include "core/Base64.h"

#include <string_view>

namespace sashfold::net {

namespace {

bool is_data_whitespace(char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

std::string_view strip_whitespace(std::string_view text)
{
    while (!text.empty() && is_data_whitespace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_data_whitespace(text.back()))
        text.remove_suffix(1);
    return text;
}

// Percent-decoding over bytes (the URL parser's own decoder works on the
// string form; the body here becomes response bytes).
std::vector<std::uint8_t> percent_decode_bytes(std::string_view input)
{
    std::vector<std::uint8_t> out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()
            && is_ascii_hex_digit(static_cast<unsigned char>(input[i + 1]))
            && is_ascii_hex_digit(static_cast<unsigned char>(input[i + 2]))) {
            out.push_back(static_cast<std::uint8_t>(
                hex_digit_value(static_cast<unsigned char>(input[i + 1])) * 16
                + hex_digit_value(static_cast<unsigned char>(input[i + 2]))));
            i += 2;
        } else {
            out.push_back(static_cast<std::uint8_t>(input[i]));
        }
    }
    return out;
}

// mimeType "ends with ;base64" per the spec's pattern: a ';', zero or more
// spaces, then a case-insensitive "base64". Returns the prefix to keep, or
// nullopt when the pattern is absent.
std::optional<std::string_view> strip_base64_suffix(std::string_view mime)
{
    if (mime.size() < 7)
        return std::nullopt;
    std::string_view rest = mime;
    if (!ascii_ci_equals(rest.substr(rest.size() - 6), "base64"))
        return std::nullopt;
    rest.remove_suffix(6);
    while (!rest.empty() && rest.back() == ' ')
        rest.remove_suffix(1);
    if (rest.empty() || rest.back() != ';')
        return std::nullopt;
    rest.remove_suffix(1);
    return rest;
}

}

std::optional<DataUrlPayload> parse_data_url(Url const& url)
{
    if (url.scheme != "data")
        return std::nullopt;
    // Everything after "data:", fragment excluded.
    std::string const serialized = url.serialize(/* exclude_fragment */ true);
    std::string_view input = std::string_view(serialized).substr(5);

    std::size_t const comma = input.find(',');
    if (comma == std::string_view::npos)
        return std::nullopt;
    std::string_view mime = strip_whitespace(input.substr(0, comma));
    std::vector<std::uint8_t> body = percent_decode_bytes(input.substr(comma + 1));

    if (std::optional<std::string_view> const stripped = strip_base64_suffix(mime)) {
        std::string const text(body.begin(), body.end());
        std::optional<std::vector<std::uint8_t>> decoded = base64_decode(text);
        if (!decoded)
            return std::nullopt;
        body = std::move(*decoded);
        mime = *stripped;
    }

    DataUrlPayload payload;
    if (mime.starts_with(";"))
        payload.mime_type = "text/plain" + std::string(mime);
    else if (mime.empty())
        payload.mime_type = "text/plain;charset=US-ASCII";
    else
        payload.mime_type = std::string(mime);
    payload.bytes = std::move(body);
    return payload;
}

}
