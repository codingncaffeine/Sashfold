#include "net/Url.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "net/Idna.h"

#include <algorithm>
#include <array>

namespace sashfold::net {

namespace {

constexpr char32_t eof_sentinel = 0xFFFFFFFF;

struct SpecialScheme {
    std::string_view scheme;
    std::optional<std::uint16_t> default_port;
};

constexpr std::array<SpecialScheme, 6> special_schemes { {
    { "ftp", std::uint16_t { 21 } },
    { "file", std::nullopt },
    { "http", std::uint16_t { 80 } },
    { "https", std::uint16_t { 443 } },
    { "ws", std::uint16_t { 80 } },
    { "wss", std::uint16_t { 443 } },
} };

std::optional<std::uint16_t> default_port_of(std::string_view scheme)
{
    for (SpecialScheme const& entry : special_schemes) {
        if (entry.scheme == scheme)
            return entry.default_port;
    }
    return std::nullopt;
}

bool is_special_scheme(std::string_view scheme)
{
    for (SpecialScheme const& entry : special_schemes) {
        if (entry.scheme == scheme)
            return true;
    }
    return false;
}

// --- Percent encoding ---------------------------------------------------------

enum class EncodeSet {
    C0Control,
    Fragment,
    Query,
    SpecialQuery,
    Path,
    Userinfo,
};

bool in_encode_set(char32_t c, EncodeSet set)
{
    if (c < 0x20 || c > 0x7E)
        return true; // C0 controls and everything non-ASCII-printable
    if (set == EncodeSet::C0Control)
        return false;
    // fragment: SPACE " < > `
    if (c == U' ' || c == U'"' || c == U'<' || c == U'>')
        return true;
    if (set == EncodeSet::Fragment)
        return c == U'`';
    // query: adds # but not `
    if (c == U'#')
        return true;
    if (set == EncodeSet::Query)
        return false;
    if (set == EncodeSet::SpecialQuery)
        return c == U'\'';
    // path: query plus ? ^ ` { }
    if (c == U'?' || c == U'^' || c == U'`' || c == U'{' || c == U'}')
        return true;
    if (set == EncodeSet::Path)
        return false;
    // userinfo: path plus / : ; = @ [ \ ] |
    return c == U'/' || c == U':' || c == U';' || c == U'=' || c == U'@'
        || (c >= U'[' && c <= U']') || c == U'|';
}

void percent_encode_utf8(char32_t c, EncodeSet set, std::string& out)
{
    if (!in_encode_set(c, set)) {
        out += static_cast<char>(c);
        return;
    }
    std::string const bytes = to_utf8(std::u32string_view(&c, 1));
    static constexpr char hex[] = "0123456789ABCDEF";
    for (char const byte : bytes) {
        unsigned char const b = static_cast<unsigned char>(byte);
        out += '%';
        out += hex[b >> 4];
        out += hex[b & 0xF];
    }
}

std::string percent_decode(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()
            && is_ascii_hex_digit(static_cast<unsigned char>(input[i + 1]))
            && is_ascii_hex_digit(static_cast<unsigned char>(input[i + 2]))) {
            unsigned const value = hex_digit_value(static_cast<unsigned char>(input[i + 1])) * 16
                + hex_digit_value(static_cast<unsigned char>(input[i + 2]));
            out += static_cast<char>(value);
            i += 2;
        } else {
            out += input[i];
        }
    }
    return out;
}

// --- Host parsing -------------------------------------------------------------

bool is_forbidden_host_code_point(char32_t c)
{
    return c == 0 || c == U'\t' || c == U'\n' || c == U'\r' || c == U' ' || c == U'#'
        || c == U'/' || c == U':' || c == U'<' || c == U'>' || c == U'?' || c == U'@'
        || c == U'[' || c == U'\\' || c == U']' || c == U'^' || c == U'|';
}

bool is_forbidden_domain_code_point(char32_t c)
{
    return is_forbidden_host_code_point(c) || c < 0x20 || c == U'%' || c == 0x7F;
}

// "Ends in a number" checker (spec §3.5) and the IPv4 number parser.
std::optional<std::uint64_t> parse_ipv4_number(std::string_view input, bool& seen_radix)
{
    if (input.empty())
        return std::nullopt;
    unsigned radix = 10;
    if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
        input.remove_prefix(2);
        radix = 16;
        seen_radix = true;
    } else if (input.size() >= 2 && input[0] == '0') {
        input.remove_prefix(1);
        radix = 8;
        seen_radix = true;
    }
    if (input.empty())
        return 0;
    std::uint64_t value = 0;
    for (char const c : input) {
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<unsigned>(c - '0');
        else if (radix == 16 && c >= 'a' && c <= 'f')
            digit = static_cast<unsigned>(c - 'a' + 10);
        else if (radix == 16 && c >= 'A' && c <= 'F')
            digit = static_cast<unsigned>(c - 'A' + 10);
        else
            return std::nullopt;
        if (digit >= radix)
            return std::nullopt;
        // Overflow still counts as a number (so the host IS an IPv4 candidate,
        // which then fails the range checks and the whole parse) — saturate.
        if (value > 0xFFFFFFFFull)
            continue;
        value = value * radix + digit;
    }
    return value;
}

bool ends_in_a_number(std::string_view input)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= input.size(); ++i) {
        if (i == input.size() || input[i] == '.') {
            parts.push_back(input.substr(start, i - start));
            start = i + 1;
        }
    }
    if (!parts.empty() && parts.back().empty()) {
        if (parts.size() == 1)
            return false;
        parts.pop_back();
    }
    if (parts.empty())
        return false;
    std::string_view const last = parts.back();
    if (!last.empty() && std::all_of(last.begin(), last.end(), [](char c) {
            return c >= '0' && c <= '9';
        }))
        return true;
    bool radix = false;
    return parse_ipv4_number(last, radix).has_value();
}

std::optional<std::uint32_t> parse_ipv4(std::string_view input)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= input.size(); ++i) {
        if (i == input.size() || input[i] == '.') {
            parts.push_back(input.substr(start, i - start));
            start = i + 1;
        }
    }
    if (!parts.empty() && parts.back().empty())
        parts.pop_back();
    if (parts.empty() || parts.size() > 4)
        return std::nullopt;
    std::vector<std::uint64_t> numbers;
    for (std::string_view const part : parts) {
        bool radix = false;
        std::optional<std::uint64_t> const number = parse_ipv4_number(part, radix);
        if (!number)
            return std::nullopt;
        numbers.push_back(*number);
    }
    for (std::size_t i = 0; i + 1 < numbers.size(); ++i) {
        if (numbers[i] > 255)
            return std::nullopt;
    }
    std::uint64_t const max_last = 1ull << (8 * (5 - numbers.size()));
    if (numbers.back() >= max_last)
        return std::nullopt;
    std::uint32_t address = static_cast<std::uint32_t>(numbers.back());
    for (std::size_t i = 0; i + 1 < numbers.size(); ++i)
        address += static_cast<std::uint32_t>(numbers[i]) << (8 * (3 - i));
    return address;
}

std::string serialize_ipv4(std::uint32_t address)
{
    std::string out;
    for (int i = 3; i >= 0; --i) {
        out += std::to_string((address >> (8 * i)) & 0xFF);
        if (i != 0)
            out += '.';
    }
    return out;
}

std::optional<std::array<std::uint16_t, 8>> parse_ipv6(std::u32string_view input)
{
    std::array<std::uint16_t, 8> address {};
    int piece_index = 0;
    int compress = -1;
    std::size_t pointer = 0;
    auto const c = [&]() { return pointer < input.size() ? input[pointer] : eof_sentinel; };

    if (c() == U':') {
        if (pointer + 1 >= input.size() || input[pointer + 1] != U':')
            return std::nullopt;
        pointer += 2;
        compress = ++piece_index;
    }
    while (c() != eof_sentinel) {
        if (piece_index == 8)
            return std::nullopt;
        if (c() == U':') {
            if (compress != -1)
                return std::nullopt;
            ++pointer;
            compress = ++piece_index;
            continue;
        }
        std::uint32_t value = 0;
        int length = 0;
        while (length < 4 && is_ascii_hex_digit(c())) {
            value = value * 16 + hex_digit_value(c());
            ++pointer;
            ++length;
        }
        if (c() == U'.') {
            if (length == 0)
                return std::nullopt;
            pointer -= static_cast<std::size_t>(length);
            if (piece_index > 6)
                return std::nullopt;
            int numbers_seen = 0;
            while (c() != eof_sentinel) {
                std::uint32_t ipv4_piece = 0xFFFFFFFF;
                if (numbers_seen > 0) {
                    if (c() == U'.' && numbers_seen < 4)
                        ++pointer;
                    else
                        return std::nullopt;
                }
                if (!is_ascii_digit(c()))
                    return std::nullopt;
                while (is_ascii_digit(c())) {
                    std::uint32_t const digit = static_cast<std::uint32_t>(c() - U'0');
                    if (ipv4_piece == 0xFFFFFFFF)
                        ipv4_piece = digit;
                    else if (ipv4_piece == 0)
                        return std::nullopt;
                    else
                        ipv4_piece = ipv4_piece * 10 + digit;
                    if (ipv4_piece > 255)
                        return std::nullopt;
                    ++pointer;
                }
                address[static_cast<std::size_t>(piece_index)] = static_cast<std::uint16_t>(
                    address[static_cast<std::size_t>(piece_index)] * 0x100 + ipv4_piece);
                ++numbers_seen;
                if (numbers_seen == 2 || numbers_seen == 4)
                    ++piece_index;
            }
            if (numbers_seen != 4)
                return std::nullopt;
            break;
        }
        if (c() == U':') {
            ++pointer;
            if (c() == eof_sentinel)
                return std::nullopt;
        } else if (c() != eof_sentinel) {
            return std::nullopt;
        }
        address[static_cast<std::size_t>(piece_index)] = static_cast<std::uint16_t>(value);
        ++piece_index;
    }
    if (compress != -1) {
        int swaps = piece_index - compress;
        piece_index = 7;
        while (piece_index != 0 && swaps > 0) {
            std::swap(address[static_cast<std::size_t>(piece_index)],
                address[static_cast<std::size_t>(compress + swaps - 1)]);
            --piece_index;
            --swaps;
        }
    } else if (piece_index != 8) {
        return std::nullopt;
    }
    return address;
}

std::string serialize_ipv6(std::array<std::uint16_t, 8> const& address)
{
    // Find the longest run of zero pieces (length > 1), leftmost.
    int best_start = -1;
    int best_length = 1;
    for (int i = 0; i < 8;) {
        if (address[static_cast<std::size_t>(i)] != 0) {
            ++i;
            continue;
        }
        int j = i;
        while (j < 8 && address[static_cast<std::size_t>(j)] == 0)
            ++j;
        if (j - i > best_length) {
            best_length = j - i;
            best_start = i;
        }
        i = j;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i) {
        if (best_start == i) {
            out += i == 0 ? "::" : ":";
            i += best_length - 1;
            continue;
        }
        std::uint16_t const piece = address[static_cast<std::size_t>(i)];
        bool leading = true;
        for (int shift = 12; shift >= 0; shift -= 4) {
            unsigned const digit = (piece >> shift) & 0xF;
            if (digit == 0 && leading && shift != 0)
                continue;
            leading = false;
            out += hex[digit];
        }
        if (i != 7)
            out += ':';
    }
    return out;
}

struct ParsedHost {
    Url::HostKind kind = Url::HostKind::Empty;
    std::string serialized;
};

std::optional<ParsedHost> parse_host(std::u32string_view input, bool is_not_special)
{
    if (!input.empty() && input.front() == U'[') {
        if (input.back() != U']')
            return std::nullopt;
        auto const address = parse_ipv6(input.substr(1, input.size() - 2));
        if (!address)
            return std::nullopt;
        return ParsedHost { Url::HostKind::Ipv6, serialize_ipv6(*address) };
    }
    if (is_not_special) {
        // Opaque host.
        if (input.empty())
            return ParsedHost { Url::HostKind::Empty, "" };
        std::string serialized;
        for (char32_t const c : input) {
            if (c != U'%' && is_forbidden_host_code_point(c))
                return std::nullopt;
            percent_encode_utf8(c, EncodeSet::C0Control, serialized);
        }
        return ParsedHost { Url::HostKind::Opaque, std::move(serialized) };
    }
    // Domain path: percent-decode, then domain to ASCII.
    std::string const decoded = percent_decode(to_utf8(input));
    std::optional<std::string> const ascii = domain_to_ascii(decoded);
    if (!ascii || ascii->empty())
        return std::nullopt;
    for (char const c : *ascii) {
        if (is_forbidden_domain_code_point(static_cast<unsigned char>(c)))
            return std::nullopt;
    }
    if (ends_in_a_number(*ascii)) {
        auto const address = parse_ipv4(*ascii);
        if (!address)
            return std::nullopt;
        return ParsedHost { Url::HostKind::Ipv4, serialize_ipv4(*address) };
    }
    return ParsedHost { Url::HostKind::Domain, *ascii };
}

// --- Path helpers -------------------------------------------------------------

bool is_windows_drive_letter(std::u32string_view s)
{
    return s.size() == 2 && is_ascii_alpha(s[0]) && (s[1] == U':' || s[1] == U'|');
}

bool is_normalized_windows_drive_letter(std::string_view s)
{
    return s.size() == 2 && is_ascii_alpha(static_cast<unsigned char>(s[0])) && s[1] == ':';
}

bool starts_with_windows_drive_letter(std::u32string_view s)
{
    if (s.size() < 2 || !is_windows_drive_letter(s.substr(0, 2)))
        return false;
    return s.size() == 2 || s[2] == U'/' || s[2] == U'\\' || s[2] == U'?' || s[2] == U'#';
}

bool is_single_dot(std::string_view segment)
{
    return segment == "." || ascii_ci_equals(segment, "%2e");
}

bool is_double_dot(std::string_view segment)
{
    return segment == ".." || ascii_ci_equals(segment, ".%2e") || ascii_ci_equals(segment, "%2e.")
        || ascii_ci_equals(segment, "%2e%2e");
}

void shorten_path(Url& url)
{
    if (url.scheme == "file" && url.path.size() == 1
        && is_normalized_windows_drive_letter(url.path[0]))
        return;
    if (!url.path.empty())
        url.path.pop_back();
}

enum class State {
    SchemeStart,
    Scheme,
    NoScheme,
    SpecialRelativeOrAuthority,
    PathOrAuthority,
    Relative,
    RelativeSlash,
    SpecialAuthoritySlashes,
    SpecialAuthorityIgnoreSlashes,
    Authority,
    Host,
    Port,
    File,
    FileSlash,
    FileHost,
    PathStart,
    Path,
    OpaquePath,
    Query,
    Fragment,
};

} // namespace

bool Url::is_special() const
{
    return is_special_scheme(scheme);
}

std::optional<Url> parse_url(std::string_view raw_input, Url const* base)
{
    // Preprocessing: strip leading/trailing C0 controls and space, then all
    // tabs and newlines.
    std::size_t begin = 0;
    std::size_t end = raw_input.size();
    while (begin < end && static_cast<unsigned char>(raw_input[begin]) <= 0x20)
        ++begin;
    while (end > begin && static_cast<unsigned char>(raw_input[end - 1]) <= 0x20)
        --end;
    std::string filtered;
    filtered.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        char const c = raw_input[i];
        if (c != '\t' && c != '\n' && c != '\r')
            filtered += c;
    }
    std::u32string const input = decode_utf8(filtered);

    Url url;
    State state = State::SchemeStart;
    std::u32string buffer;
    bool at_sign_seen = false;
    bool password_token_seen = false;
    bool inside_brackets = false;

    std::size_t pointer = 0;
    auto const c = [&]() { return pointer < input.size() ? input[pointer] : eof_sentinel; };
    auto const remaining_starts_with = [&](std::u32string_view prefix) {
        return input.size() - std::min(input.size(), pointer + 1) >= prefix.size()
            && std::u32string_view(input).substr(pointer + 1, prefix.size()) == prefix;
    };
    auto const remaining = [&]() {
        return std::u32string_view(input).substr(std::min(input.size(), pointer + 1));
    };

    while (true) {
        std::size_t const before = pointer;
        switch (state) {
        case State::SchemeStart:
            if (is_ascii_alpha(c())) {
                buffer.push_back(to_ascii_lowercase(c()));
                state = State::Scheme;
            } else {
                state = State::NoScheme;
                --pointer;
            }
            break;

        case State::Scheme:
            if (is_ascii_alphanumeric(c()) || c() == U'+' || c() == U'-' || c() == U'.') {
                buffer.push_back(to_ascii_lowercase(c()));
            } else if (c() == U':') {
                url.scheme = to_utf8(buffer);
                buffer.clear();
                if (url.scheme == "file") {
                    state = State::File;
                } else if (url.is_special() && base && base->scheme == url.scheme) {
                    state = State::SpecialRelativeOrAuthority;
                } else if (url.is_special()) {
                    state = State::SpecialAuthoritySlashes;
                } else if (remaining_starts_with(U"/")) {
                    state = State::PathOrAuthority;
                    ++pointer;
                } else {
                    url.has_opaque_path = true;
                    url.path = { "" };
                    state = State::OpaquePath;
                }
            } else {
                buffer.clear();
                state = State::NoScheme;
                pointer = static_cast<std::size_t>(-1); // start over
            }
            break;

        case State::NoScheme:
            if (!base || (base->has_opaque_path && c() != U'#'))
                return std::nullopt;
            if (base->has_opaque_path && c() == U'#') {
                url.scheme = base->scheme;
                url.has_opaque_path = true;
                url.path = base->path;
                url.query = base->query;
                url.fragment = "";
                state = State::Fragment;
            } else if (base->scheme != "file") {
                state = State::Relative;
                --pointer;
            } else {
                state = State::File;
                --pointer;
            }
            break;

        case State::SpecialRelativeOrAuthority:
            if (c() == U'/' && remaining_starts_with(U"/")) {
                state = State::SpecialAuthorityIgnoreSlashes;
                ++pointer;
            } else {
                state = State::Relative;
                --pointer;
            }
            break;

        case State::PathOrAuthority:
            if (c() == U'/') {
                state = State::Authority;
            } else {
                state = State::Path;
                --pointer;
            }
            break;

        case State::Relative:
            url.scheme = base->scheme;
            if (c() == U'/' || (url.is_special() && c() == U'\\')) {
                state = State::RelativeSlash;
            } else {
                url.username = base->username;
                url.password = base->password;
                url.host_kind = base->host_kind;
                url.host = base->host;
                url.port = base->port;
                url.path = base->path;
                url.has_opaque_path = base->has_opaque_path;
                url.query = base->query;
                if (c() == U'?') {
                    url.query = "";
                    state = State::Query;
                } else if (c() == U'#') {
                    url.fragment = "";
                    state = State::Fragment;
                } else if (c() != eof_sentinel) {
                    url.query.reset();
                    shorten_path(url);
                    state = State::Path;
                    --pointer;
                }
            }
            break;

        case State::RelativeSlash:
            if (url.is_special() && (c() == U'/' || c() == U'\\')) {
                state = State::SpecialAuthorityIgnoreSlashes;
            } else if (c() == U'/') {
                state = State::Authority;
            } else {
                url.username = base->username;
                url.password = base->password;
                url.host_kind = base->host_kind;
                url.host = base->host;
                url.port = base->port;
                state = State::Path;
                --pointer;
            }
            break;

        case State::SpecialAuthoritySlashes:
            if (c() == U'/' && remaining_starts_with(U"/")) {
                state = State::SpecialAuthorityIgnoreSlashes;
                ++pointer;
            } else {
                state = State::SpecialAuthorityIgnoreSlashes;
                --pointer;
            }
            break;

        case State::SpecialAuthorityIgnoreSlashes:
            if (c() != U'/' && c() != U'\\') {
                state = State::Authority;
                --pointer;
            }
            break;

        case State::Authority:
            if (c() == U'@') {
                if (at_sign_seen)
                    buffer.insert(0, U"%40");
                at_sign_seen = true;
                for (char32_t const code_point : buffer) {
                    if (code_point == U':' && !password_token_seen) {
                        password_token_seen = true;
                        continue;
                    }
                    percent_encode_utf8(code_point, EncodeSet::Userinfo,
                        password_token_seen ? url.password : url.username);
                }
                buffer.clear();
            } else if (c() == eof_sentinel || c() == U'/' || c() == U'?' || c() == U'#'
                || (url.is_special() && c() == U'\\')) {
                if (at_sign_seen && buffer.empty())
                    return std::nullopt;
                pointer -= buffer.size() + 1;
                buffer.clear();
                state = State::Host;
            } else {
                buffer.push_back(c());
            }
            break;

        case State::Host:
            if (c() == U':' && !inside_brackets) {
                if (buffer.empty())
                    return std::nullopt;
                auto host = parse_host(buffer, !url.is_special());
                if (!host)
                    return std::nullopt;
                url.host_kind = host->kind;
                url.host = std::move(host->serialized);
                buffer.clear();
                state = State::Port;
            } else if (c() == eof_sentinel || c() == U'/' || c() == U'?' || c() == U'#'
                || (url.is_special() && c() == U'\\')) {
                --pointer;
                if (url.is_special() && buffer.empty())
                    return std::nullopt;
                auto host = parse_host(buffer, !url.is_special());
                if (!host)
                    return std::nullopt;
                url.host_kind = host->kind;
                url.host = std::move(host->serialized);
                buffer.clear();
                state = State::PathStart;
            } else {
                if (c() == U'[')
                    inside_brackets = true;
                if (c() == U']')
                    inside_brackets = false;
                buffer.push_back(c());
            }
            break;

        case State::Port:
            if (is_ascii_digit(c())) {
                buffer.push_back(c());
            } else if (c() == eof_sentinel || c() == U'/' || c() == U'?' || c() == U'#'
                || (url.is_special() && c() == U'\\')) {
                if (!buffer.empty()) {
                    std::uint32_t port_value = 0;
                    for (char32_t const digit : buffer) {
                        port_value = port_value * 10 + static_cast<std::uint32_t>(digit - U'0');
                        if (port_value > 65535)
                            return std::nullopt;
                    }
                    auto const default_port = default_port_of(url.scheme);
                    if (default_port && *default_port == port_value)
                        url.port.reset();
                    else
                        url.port = static_cast<std::uint16_t>(port_value);
                    buffer.clear();
                }
                state = State::PathStart;
                --pointer;
            } else {
                return std::nullopt;
            }
            break;

        case State::File:
            url.scheme = "file";
            url.host_kind = Url::HostKind::Empty;
            url.host.clear();
            if (c() == U'/' || c() == U'\\') {
                state = State::FileSlash;
            } else if (base && base->scheme == "file") {
                url.host_kind = base->host_kind;
                url.host = base->host;
                url.path = base->path;
                url.query = base->query;
                if (c() == U'?') {
                    url.query = "";
                    state = State::Query;
                } else if (c() == U'#') {
                    url.fragment = "";
                    state = State::Fragment;
                } else if (c() != eof_sentinel) {
                    url.query.reset();
                    if (!starts_with_windows_drive_letter(
                            std::u32string_view(input).substr(pointer)))
                        shorten_path(url);
                    else
                        url.path.clear();
                    state = State::Path;
                    --pointer;
                }
            } else {
                state = State::Path;
                --pointer;
            }
            break;

        case State::FileSlash:
            if (c() == U'/' || c() == U'\\') {
                state = State::FileHost;
            } else {
                if (base && base->scheme == "file") {
                    url.host_kind = base->host_kind;
                    url.host = base->host;
                    if (!starts_with_windows_drive_letter(
                            std::u32string_view(input).substr(pointer))
                        && !base->path.empty()
                        && is_normalized_windows_drive_letter(base->path[0]))
                        url.path.push_back(base->path[0]);
                }
                state = State::Path;
                --pointer;
            }
            break;

        case State::FileHost:
            if (c() == eof_sentinel || c() == U'/' || c() == U'\\' || c() == U'?' || c() == U'#') {
                --pointer;
                if (is_windows_drive_letter(buffer)) {
                    state = State::Path; // buffer survives into path state
                } else if (buffer.empty()) {
                    url.host_kind = Url::HostKind::Empty;
                    url.host.clear();
                    state = State::PathStart;
                } else {
                    auto host = parse_host(buffer, !url.is_special());
                    if (!host)
                        return std::nullopt;
                    if (host->kind == Url::HostKind::Domain && host->serialized == "localhost") {
                        host->kind = Url::HostKind::Empty;
                        host->serialized.clear();
                    }
                    url.host_kind = host->kind;
                    url.host = std::move(host->serialized);
                    buffer.clear();
                    state = State::PathStart;
                }
            } else {
                buffer.push_back(c());
            }
            break;

        case State::PathStart:
            if (url.is_special()) {
                state = State::Path;
                if (c() != U'/' && c() != U'\\')
                    --pointer;
            } else if (c() == U'?') {
                url.query = "";
                state = State::Query;
            } else if (c() == U'#') {
                url.fragment = "";
                state = State::Fragment;
            } else if (c() != eof_sentinel) {
                state = State::Path;
                if (c() != U'/')
                    --pointer;
            }
            break;

        case State::Path:
            if (c() == eof_sentinel || c() == U'/' || (url.is_special() && c() == U'\\')
                || c() == U'?' || c() == U'#') {
                std::string segment = to_utf8(buffer);
                if (is_double_dot(segment)) {
                    shorten_path(url);
                    if (c() != U'/' && !(url.is_special() && c() == U'\\'))
                        url.path.push_back("");
                } else if (is_single_dot(segment)) {
                    if (c() != U'/' && !(url.is_special() && c() == U'\\'))
                        url.path.push_back("");
                } else {
                    if (url.scheme == "file" && url.path.empty()
                        && is_windows_drive_letter(buffer)) {
                        segment[1] = ':';
                    }
                    url.path.push_back(std::move(segment));
                }
                buffer.clear();
                if (c() == U'?') {
                    url.query = "";
                    state = State::Query;
                }
                if (c() == U'#') {
                    url.fragment = "";
                    state = State::Fragment;
                }
            } else {
                std::string encoded;
                percent_encode_utf8(c(), EncodeSet::Path, encoded);
                for (char const byte : encoded)
                    buffer.push_back(static_cast<char32_t>(static_cast<unsigned char>(byte)));
            }
            break;

        case State::OpaquePath:
            if (c() == U'?') {
                url.query = "";
                state = State::Query;
            } else if (c() == U'#') {
                url.fragment = "";
                state = State::Fragment;
            } else if (c() == U' ') {
                if (!remaining().empty() && (remaining()[0] == U'?' || remaining()[0] == U'#'))
                    url.path[0] += "%20";
                else
                    url.path[0] += ' ';
            } else if (c() != eof_sentinel) {
                percent_encode_utf8(c(), EncodeSet::C0Control, url.path[0]);
            }
            break;

        case State::Query:
            if (c() == U'#' || c() == eof_sentinel) {
                EncodeSet const set
                    = url.is_special() ? EncodeSet::SpecialQuery : EncodeSet::Query;
                std::string encoded;
                for (char32_t const code_point : buffer)
                    percent_encode_utf8(code_point, set, encoded);
                *url.query += encoded;
                buffer.clear();
                if (c() == U'#') {
                    url.fragment = "";
                    state = State::Fragment;
                }
            } else {
                buffer.push_back(c());
            }
            break;

        case State::Fragment:
            if (c() != eof_sentinel)
                percent_encode_utf8(c(), EncodeSet::Fragment, *url.fragment);
            break;
        }

        // The spec's loop: after a state processes the EOF code point without
        // rewinding the pointer, parsing is done; a rewind means the (possibly
        // new) state reprocesses from the adjusted position.
        if (pointer == before && before == input.size())
            break;
        ++pointer;
    }
    return url;
}

// --- Serializers --------------------------------------------------------------

std::string Url::serialize_host() const
{
    if (host_kind == HostKind::Ipv6)
        return "[" + host + "]";
    return host;
}

std::string Url::serialize_path() const
{
    if (has_opaque_path)
        return path.empty() ? "" : path[0];
    std::string out;
    for (std::string const& segment : path) {
        out += '/';
        out += segment;
    }
    return out;
}

std::string Url::serialize(bool exclude_fragment) const
{
    std::string out = scheme + ":";
    if (has_host()) {
        out += "//";
        if (includes_credentials()) {
            out += username;
            if (!password.empty()) {
                out += ':';
                out += password;
            }
            out += '@';
        }
        out += serialize_host();
        if (port) {
            out += ':';
            out += std::to_string(*port);
        }
    } else if (!has_opaque_path && path.size() > 1 && !path.empty() && path[0].empty()) {
        // No host + path starting with "//": prefix "/." to disambiguate.
        out += "/.";
    }
    out += serialize_path();
    if (query) {
        out += '?';
        out += *query;
    }
    if (!exclude_fragment && fragment) {
        out += '#';
        out += *fragment;
    }
    return out;
}

std::string Url::host_with_port() const
{
    if (!has_host() || host_kind == HostKind::None)
        return "";
    std::string out = serialize_host();
    if (port) {
        out += ':';
        out += std::to_string(*port);
    }
    return out;
}

std::string Url::port_string() const
{
    return port ? std::to_string(*port) : "";
}

std::string Url::serialize_origin() const
{
    if (scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss"
        || scheme == "ftp") {
        std::string out = scheme + "://" + serialize_host();
        if (port) {
            out += ':';
            out += std::to_string(*port);
        }
        return out;
    }
    if (scheme == "blob" && has_opaque_path && !path.empty()) {
        // A blob: URL adopts the origin of the http(s) URL in its path.
        std::optional<Url> const inner = parse_url(path[0]);
        if (inner && (inner->scheme == "http" || inner->scheme == "https"))
            return inner->serialize_origin();
    }
    return "null"; // file and non-special schemes: opaque origin
}

}
