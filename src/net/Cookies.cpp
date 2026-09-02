#include "net/Cookies.h"

#include "core/Ascii.h"

#include <algorithm>
#include <charconv>

namespace sashfold::net {

namespace {

// Modest caps; eviction drops expired cookies first, then the oldest.
constexpr std::size_t max_cookies_per_domain = 60;
constexpr std::size_t max_cookies_total = 3000;

std::string_view trim_ows(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

std::string ascii_lowercase(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm —
// pure integer math, no OS time machinery).
std::int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int const era = (year >= 0 ? year : year - 399) / 400;
    unsigned const year_of_era = static_cast<unsigned>(year - era * 400);
    unsigned const day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned const day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(day_of_era)
        - 719468;
}

// §5.1.3: host domain-matches the cookie domain string.
bool domain_matches(std::string_view host, std::string_view domain)
{
    if (host == domain)
        return true;
    return host.size() > domain.size() && host.ends_with(domain)
        && host[host.size() - domain.size() - 1] == '.';
}

// §5.1.4: the default path from the request URL.
std::string default_path(Url const& url)
{
    std::string const path = url.serialize_path();
    if (path.empty() || path[0] != '/')
        return "/";
    std::size_t const last_slash = path.rfind('/');
    if (last_slash == 0)
        return "/";
    return path.substr(0, last_slash);
}

bool path_matches(std::string_view request_path, std::string_view cookie_path)
{
    if (request_path == cookie_path)
        return true;
    if (!request_path.starts_with(cookie_path))
        return false;
    return cookie_path.ends_with("/") || request_path[cookie_path.size()] == '/';
}

// The third-party gate: with a distinct first-party host, no cookie
// moves in either direction. Host equality stands in for site equality until
// the public-suffix list lands (stricter, never looser).
bool third_party(Url const& url, Url const* first_party)
{
    return first_party && first_party->host != url.host;
}

bool parse_int(std::string_view text, int& out)
{
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc {} && ptr == text.data() + text.size();
}

}

// RFC 6265 §5.1.1. Tokens are the runs between delimiter octets; the first
// token that parses as a time/day/month/year wins that slot.
std::optional<std::int64_t> parse_cookie_date(std::string_view text)
{
    auto const is_delimiter = [](unsigned char c) {
        return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40)
            || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
    };
    auto const digits_prefix = [](std::string_view token, unsigned max_digits, int& value,
                                   std::size_t& digit_count) {
        digit_count = 0;
        while (digit_count < token.size() && digit_count < max_digits
            && is_ascii_digit(static_cast<unsigned char>(token[digit_count])))
            ++digit_count;
        if (digit_count == 0)
            return false;
        // Trailing garbage must be non-digit (a 5-digit year is not a year).
        if (digit_count < token.size()
            && is_ascii_digit(static_cast<unsigned char>(token[digit_count])))
            return false;
        return parse_int(token.substr(0, digit_count), value);
    };

    bool found_time = false, found_day = false, found_month = false, found_year = false;
    int hour = 0, minute = 0, second = 0, day = 0, month = 0, year = 0;

    static constexpr std::string_view month_names[]
        = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };

    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_delimiter(static_cast<unsigned char>(text[i])))
            ++i;
        std::size_t const start = i;
        while (i < text.size() && !is_delimiter(static_cast<unsigned char>(text[i])))
            ++i;
        std::string_view const token = text.substr(start, i - start);
        if (token.empty())
            continue;

        if (!found_time) {
            // hh:mm:ss, each field 1-2 digits; non-digit junk may trail.
            std::size_t pos = 0;
            int h = 0, m = 0, s = 0;
            auto const field = [&](int& out) {
                std::size_t n = 0;
                while (pos + n < token.size() && n < 2
                    && is_ascii_digit(static_cast<unsigned char>(token[pos + n])))
                    ++n;
                if (n == 0 || !parse_int(token.substr(pos, n), out))
                    return false;
                pos += n;
                return true;
            };
            bool const ok = field(h) && pos < token.size() && token[pos] == ':'
                && (++pos, field(m)) && pos < token.size() && token[pos] == ':'
                && (++pos, field(s))
                && !(pos < token.size()
                    && is_ascii_digit(static_cast<unsigned char>(token[pos])));
            if (ok) {
                found_time = true;
                hour = h;
                minute = m;
                second = s;
                continue;
            }
        }
        if (!found_day) {
            int value = 0;
            std::size_t digits = 0;
            if (digits_prefix(token, 2, value, digits)) {
                found_day = true;
                day = value;
                continue;
            }
        }
        if (!found_month && token.size() >= 3) {
            std::string const prefix = ascii_lowercase(token.substr(0, 3));
            for (int m = 0; m < 12; ++m) {
                if (prefix == month_names[m]) {
                    found_month = true;
                    month = m + 1;
                    break;
                }
            }
            if (found_month)
                continue;
        }
        if (!found_year) {
            int value = 0;
            std::size_t digits = 0;
            if (digits_prefix(token, 4, value, digits) && digits >= 2) {
                found_year = true;
                year = value;
                continue;
            }
        }
    }

    if (!found_time || !found_day || !found_month || !found_year)
        return std::nullopt;
    if (year >= 70 && year <= 99)
        year += 1900;
    else if (year >= 0 && year <= 69)
        year += 2000;
    if (year < 1601 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return std::nullopt;

    return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400
        + hour * 3600 + minute * 60 + second;
}

void CookieJar::store(Url const& url, Url const* first_party,
    std::vector<Header> const& response_headers, std::int64_t now)
{
    if (third_party(url, first_party))
        return;

    for (Header const& header : response_headers) {
        if (!ascii_ci_equals(header.name, "set-cookie"))
            continue;
        std::string_view input = header.value;

        // §5.2: name=value, then ;-separated attributes.
        std::string_view name_value = input;
        std::string_view attributes;
        if (std::size_t const semicolon = input.find(';'); semicolon != std::string_view::npos) {
            name_value = input.substr(0, semicolon);
            attributes = input.substr(semicolon + 1);
        }
        std::size_t const equals = name_value.find('=');
        if (equals == std::string_view::npos)
            continue;
        Cookie cookie;
        cookie.name = std::string(trim_ows(name_value.substr(0, equals)));
        cookie.value = std::string(trim_ows(name_value.substr(equals + 1)));
        if (cookie.name.empty() && cookie.value.empty())
            continue;

        std::optional<std::int64_t> max_age_expiry;
        std::optional<std::int64_t> expires_expiry;
        std::string domain_attribute;
        std::string path_attribute;

        while (!attributes.empty()) {
            std::string_view attribute = attributes;
            if (std::size_t const semicolon = attributes.find(';');
                semicolon != std::string_view::npos) {
                attribute = attributes.substr(0, semicolon);
                attributes.remove_prefix(semicolon + 1);
            } else {
                attributes = {};
            }
            std::string_view attr_name = attribute;
            std::string_view attr_value;
            if (std::size_t const eq = attribute.find('='); eq != std::string_view::npos) {
                attr_name = attribute.substr(0, eq);
                attr_value = trim_ows(attribute.substr(eq + 1));
            }
            attr_name = trim_ows(attr_name);

            if (ascii_ci_equals(attr_name, "expires")) {
                if (auto const date = parse_cookie_date(attr_value))
                    expires_expiry = *date;
            } else if (ascii_ci_equals(attr_name, "max-age")) {
                // digits with optional leading '-'; anything else ignored.
                std::string_view digits = attr_value;
                bool const negative = digits.starts_with("-");
                if (negative)
                    digits.remove_prefix(1);
                int seconds = 0;
                if (!digits.empty() && parse_int(digits, seconds))
                    max_age_expiry = negative || seconds <= 0 ? std::int64_t { 0 }
                                                              : now + seconds;
            } else if (ascii_ci_equals(attr_name, "domain")) {
                std::string_view value = attr_value;
                if (value.starts_with("."))
                    value.remove_prefix(1);
                domain_attribute = ascii_lowercase(value);
            } else if (ascii_ci_equals(attr_name, "path")) {
                path_attribute = std::string(attr_value);
            } else if (ascii_ci_equals(attr_name, "secure")) {
                cookie.secure = true;
            } else if (ascii_ci_equals(attr_name, "httponly")) {
                cookie.http_only = true;
            }
            // SameSite parses as an unknown attribute for now: the blanket
            // third-party policy above is stricter than any SameSite mode.
        }

        // Max-Age beats Expires (§5.3 step 3).
        cookie.expires = max_age_expiry ? max_age_expiry : expires_expiry;

        // Domain attribute must cover the request host, or the cookie drops.
        std::string const host = ascii_lowercase(url.host);
        if (!domain_attribute.empty()) {
            if (url.host_kind != Url::HostKind::Domain || !domain_matches(host, domain_attribute))
                continue;
            cookie.domain = domain_attribute;
            cookie.host_only = false;
        } else {
            cookie.domain = host;
            cookie.host_only = true;
        }

        cookie.path = !path_attribute.empty() && path_attribute[0] == '/' ? path_attribute
                                                                          : default_path(url);
        // Secure cookies only arrive over a secure channel.
        if (cookie.secure && url.scheme != "https")
            continue;

        // Replace any equal (name, domain, path) cookie; keep creation order.
        auto const match = std::find_if(m_cookies.begin(), m_cookies.end(), [&](Cookie const& c) {
            return c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path;
        });
        if (match != m_cookies.end()) {
            cookie.created = match->created;
            *match = std::move(cookie);
        } else if (!cookie.expires || *cookie.expires > now) {
            cookie.created = ++m_counter;
            m_cookies.push_back(std::move(cookie));
        }
    }

    // Expiry sweep, then caps.
    std::erase_if(m_cookies, [&](Cookie const& c) { return c.expires && *c.expires <= now; });
    auto const domain_count = [&](std::string const& domain) {
        return static_cast<std::size_t>(std::count_if(m_cookies.begin(), m_cookies.end(),
            [&](Cookie const& c) { return c.domain == domain; }));
    };
    for (bool evicted = true; evicted;) {
        evicted = false;
        for (Cookie const& cookie : m_cookies) {
            if (domain_count(cookie.domain) > max_cookies_per_domain) {
                std::string const domain = cookie.domain;
                auto oldest = m_cookies.end();
                for (auto it = m_cookies.begin(); it != m_cookies.end(); ++it)
                    if (it->domain == domain
                        && (oldest == m_cookies.end() || it->created < oldest->created))
                        oldest = it;
                m_cookies.erase(oldest);
                evicted = true;
                break;
            }
        }
    }
    while (m_cookies.size() > max_cookies_total) {
        auto oldest = m_cookies.begin();
        for (auto it = m_cookies.begin(); it != m_cookies.end(); ++it)
            if (it->created < oldest->created)
                oldest = it;
        m_cookies.erase(oldest);
    }
}

std::string CookieJar::cookie_header(
    Url const& url, Url const* first_party, std::int64_t now) const
{
    if (third_party(url, first_party))
        return "";

    std::string const host = ascii_lowercase(url.host);
    std::string request_path = url.serialize_path();
    if (request_path.empty())
        request_path = "/";
    bool const secure = url.scheme == "https";

    std::vector<Cookie const*> matched;
    for (Cookie const& cookie : m_cookies) {
        if (cookie.expires && *cookie.expires <= now)
            continue;
        if (cookie.host_only ? host != cookie.domain : !domain_matches(host, cookie.domain))
            continue;
        if (!path_matches(request_path, cookie.path))
            continue;
        if (cookie.secure && !secure)
            continue;
        matched.push_back(&cookie);
    }
    // §5.4: longer paths first, then earlier creation.
    std::sort(matched.begin(), matched.end(), [](Cookie const* a, Cookie const* b) {
        if (a->path.size() != b->path.size())
            return a->path.size() > b->path.size();
        return a->created < b->created;
    });

    std::string header;
    for (Cookie const* cookie : matched) {
        if (!header.empty())
            header += "; ";
        header += cookie->name;
        header += '=';
        header += cookie->value;
    }
    return header;
}

}
