#include "net/Csp.h"

#include "core/Ascii.h"
#include "core/Base64.h"

#include <algorithm>
#include <array>
#include <span>
#include <tuple>

namespace sashfold::net {

namespace {

bool is_policy_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && is_policy_whitespace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_policy_whitespace(text.back()))
        text.remove_suffix(1);
    return text;
}

std::string lowercased(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string_view> split_on_whitespace(std::string_view text)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_policy_whitespace(text[i]))
            ++i;
        std::size_t const start = i;
        while (i < text.size() && !is_policy_whitespace(text[i]))
            ++i;
        if (i > start)
            out.push_back(text.substr(start, i - start));
    }
    return out;
}

std::vector<std::string_view> split_on(std::string_view text, char separator)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        std::size_t const at = text.find(separator, start);
        if (at == std::string_view::npos) {
            out.push_back(text.substr(start));
            return out;
        }
        out.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

// RFC 9110's token characters: what a directive name may be made of.
bool is_token_char(char c)
{
    return is_ascii_alphanumeric(static_cast<unsigned char>(c))
        || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+'
        || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

bool is_scheme_char(char c, bool first)
{
    if (is_ascii_alpha(static_cast<unsigned char>(c)))
        return true;
    return !first && (is_ascii_digit(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.');
}

bool is_scheme_text(std::string_view text)
{
    if (text.empty())
        return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (!is_scheme_char(text[i], i == 0))
            return false;
    return true;
}

bool is_host_char(char c)
{
    return is_ascii_alphanumeric(static_cast<unsigned char>(c)) || c == '-' || c == '.';
}

std::string percent_decoded(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()
            && is_ascii_hex_digit(static_cast<unsigned char>(text[i + 1]))
            && is_ascii_hex_digit(static_cast<unsigned char>(text[i + 2]))) {
            unsigned const value = hex_digit_value(static_cast<unsigned char>(text[i + 1])) * 16u
                + hex_digit_value(static_cast<unsigned char>(text[i + 2]));
            out += static_cast<char>(value);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::optional<std::uint16_t> default_port(std::string_view scheme)
{
    if (scheme == "http" || scheme == "ws")
        return 80;
    if (scheme == "https" || scheme == "wss")
        return 443;
    if (scheme == "ftp")
        return 21;
    return std::nullopt;
}

// §6.7.2.5: a scheme matches itself and its secure form.
bool scheme_part_matches(std::string_view expression_scheme, std::string_view url_scheme)
{
    if (expression_scheme == url_scheme)
        return true;
    if (expression_scheme == "http" && url_scheme == "https")
        return true;
    return expression_scheme == "ws" && url_scheme == "wss";
}

// §6.7.2.6, both lowercased.
bool host_part_matches(std::string_view host_part, std::string_view host)
{
    if (host_part == "*")
        return true;
    if (host_part.starts_with("*.")) {
        std::string_view const remaining = host_part.substr(1); // ".example.com"
        return host.size() > remaining.size() && host.ends_with(remaining);
    }
    return host_part == host;
}

// §6.7.2.7: an expression without a port means the scheme's default.
bool port_part_matches(std::optional<std::string> const& port_part, Url const& url)
{
    if (port_part && *port_part == "*")
        return true;
    if (!port_part)
        return !url.port.has_value();
    std::optional<std::uint16_t> const port = url.port ? url.port : default_port(url.scheme);
    if (!port)
        return false;
    if (std::to_string(*port) == *port_part)
        return true;
    // The upgrade allowance: 80 written, 443 reached.
    return *port_part == "80" && *port == 443 && (url.scheme == "https" || url.scheme == "wss");
}

// §6.7.2.8: a path ending in "/" is a prefix, any other is exact; the
// pieces compare percent-decoded, case-sensitively.
bool path_part_matches(std::string_view path_a, Url const& url)
{
    if (path_a.empty())
        return true;
    std::string const path_b = url.serialize_path();
    if (path_a == "/" && path_b.empty())
        return true;
    bool const exact = !path_a.ends_with('/');
    std::vector<std::string_view> list_a = split_on(path_a, '/');
    std::vector<std::string_view> const list_b = split_on(path_b, '/');
    if (list_a.size() > list_b.size())
        return false;
    if (exact && list_a.size() != list_b.size())
        return false;
    if (!exact)
        list_a.pop_back();
    for (std::size_t i = 0; i < list_a.size(); ++i) {
        if (percent_decoded(list_a[i]) != percent_decoded(list_b[i]))
            return false;
    }
    return true;
}

// The fallback chain a directive is looked for along (§6.8.4), most
// specific first; empty for a name that is not a fetch directive.
std::vector<std::string_view> fallback_list(std::string_view effective)
{
    if (effective == "script-src-elem" || effective == "script-src-attr")
        return { effective, "script-src", "default-src" };
    if (effective == "style-src-elem" || effective == "style-src-attr")
        return { effective, "style-src", "default-src" };
    if (effective == "worker-src")
        return { "worker-src", "child-src", "script-src", "default-src" };
    if (effective == "frame-src")
        return { "frame-src", "child-src", "default-src" };
    if (effective == "script-src" || effective == "style-src" || effective == "connect-src" || effective == "img-src"
        || effective == "font-src" || effective == "media-src" || effective == "object-src" || effective == "manifest-src"
        || effective == "prefetch-src" || effective == "child-src")
        return { effective, "default-src" };
    if (effective == "form-action")
        return { "form-action" };
    return {};
}

// §6.8.1: the directive a request's destination is judged by.
std::string_view effective_directive_for(ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Script:
        return "script-src-elem";
    case ResourceKind::Stylesheet:
        return "style-src-elem";
    case ResourceKind::Image:
        return "img-src";
    case ResourceKind::Font:
        return "font-src";
    case ResourceKind::Media:
        return "media-src";
    case ResourceKind::Subdocument:
        return "frame-src";
    case ResourceKind::Xhr:
    case ResourceKind::Other:
        return "connect-src";
    case ResourceKind::Document:
        return "";
    }
    return "";
}

std::string_view effective_directive_for(InlineKind kind)
{
    switch (kind) {
    case InlineKind::Script:
        return "script-src-elem";
    case InlineKind::ScriptAttribute:
        return "script-src-attr";
    case InlineKind::Style:
        return "style-src-elem";
    case InlineKind::StyleAttribute:
        return "style-src-attr";
    }
    return "";
}

// What the console calls the thing refused.
std::string_view thing_named(ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Script:
        return "script";
    case ResourceKind::Stylesheet:
        return "stylesheet";
    case ResourceKind::Image:
        return "image";
    case ResourceKind::Font:
        return "font";
    case ResourceKind::Media:
        return "media";
    case ResourceKind::Subdocument:
        return "frame";
    case ResourceKind::Xhr:
    case ResourceKind::Other:
    case ResourceKind::Document:
        return "resource";
    }
    return "resource";
}

bool is_script_directive_name(std::string_view effective)
{
    return effective == "script-src-elem" || effective == "script-src-attr" || effective == "script-src";
}

std::string fallback_note(std::string_view effective, std::string_view governing)
{
    if (effective == governing)
        return "";
    return " Note that '" + std::string(effective) + "' was not explicitly set, so '" + std::string(governing)
        + "' is used as a fallback.";
}

std::optional<ContentSecurityPolicy::Source> parse_source(std::string_view token)
{
    using Source = ContentSecurityPolicy::Source;
    Source source;
    if (token == "*") {
        source.kind = Source::Kind::Any;
        return source;
    }
    if (token.size() >= 2 && token.front() == '\'' && token.back() == '\'') {
        std::string const inner = lowercased(token.substr(1, token.size() - 2));
        static constexpr std::array<std::string_view, 10> keywords {
            "self", "none", "unsafe-inline", "unsafe-eval", "strict-dynamic", "unsafe-hashes",
            "report-sample", "wasm-unsafe-eval", "unsafe-allow-redirects", "inline-speculation-rules"
        };
        if (std::find(keywords.begin(), keywords.end(), inner) != keywords.end()) {
            source.kind = Source::Kind::Keyword;
            source.keyword = inner;
            return source;
        }
        if (inner.starts_with("nonce-") && inner.size() > 6) {
            source.kind = Source::Kind::Nonce;
            source.nonce = std::string(token.substr(7, token.size() - 8)); // as written, case kept
            return source;
        }
        for (auto const& [name, id, size] : std::array<std::tuple<std::string_view, crypto::HashId, std::size_t>, 3> {
                 std::tuple { "sha256-", crypto::HashId::Sha256, std::size_t { 32 } },
                 std::tuple { "sha384-", crypto::HashId::Sha384, std::size_t { 48 } },
                 std::tuple { "sha512-", crypto::HashId::Sha512, std::size_t { 64 } } }) {
            if (!inner.starts_with(name))
                continue;
            std::string encoded(token.substr(1 + name.size(), token.size() - 2 - name.size()));
            for (char& c : encoded) { // base64url is accepted as base64
                if (c == '-')
                    c = '+';
                else if (c == '_')
                    c = '/';
            }
            std::optional<std::vector<std::uint8_t>> digest = base64_decode(encoded);
            if (!digest || digest->size() != size)
                return std::nullopt;
            source.kind = Source::Kind::Hash;
            source.algorithm = id;
            source.digest = std::move(*digest);
            return source;
        }
        return std::nullopt;
    }
    // A scheme-source: "https:".
    if (token.ends_with(':') && is_scheme_text(token.substr(0, token.size() - 1))) {
        source.kind = Source::Kind::Scheme;
        source.scheme = lowercased(token.substr(0, token.size() - 1));
        return source;
    }
    // A host-source: [scheme "://"] host [":" port] [path].
    std::string_view rest = token;
    if (std::size_t const at = rest.find("://"); at != std::string_view::npos) {
        if (!is_scheme_text(rest.substr(0, at)))
            return std::nullopt;
        source.scheme = lowercased(rest.substr(0, at));
        rest.remove_prefix(at + 3);
    }
    std::size_t host_end = 0;
    while (host_end < rest.size() && rest[host_end] != ':' && rest[host_end] != '/')
        ++host_end;
    std::string_view const host = rest.substr(0, host_end);
    if (host.empty())
        return std::nullopt;
    if (host != "*") {
        std::string_view const named = host.starts_with("*.") ? host.substr(2) : host;
        if (named.empty() || named.front() == '.' || named.back() == '.' || host.find('*', 1) != std::string_view::npos)
            return std::nullopt;
        for (char const c : named)
            if (!is_host_char(c))
                return std::nullopt;
    }
    source.host = lowercased(host);
    rest.remove_prefix(host_end);
    if (rest.starts_with(':')) {
        rest.remove_prefix(1);
        std::size_t port_end = 0;
        while (port_end < rest.size() && rest[port_end] != '/')
            ++port_end;
        std::string_view const port = rest.substr(0, port_end);
        if (port.empty())
            return std::nullopt;
        if (port != "*")
            for (char const c : port)
                if (!is_ascii_digit(static_cast<unsigned char>(c)))
                    return std::nullopt;
        source.port = std::string(port);
        rest.remove_prefix(port_end);
    }
    if (!rest.empty()) {
        if (!rest.starts_with('/'))
            return std::nullopt;
        // A query or a fragment is not part of a path-part.
        std::size_t const cut = rest.find_first_of("?#");
        source.path = std::string(cut == std::string_view::npos ? rest : rest.substr(0, cut));
    }
    source.kind = Source::Kind::Host;
    return source;
}

bool is_fetch_directive(std::string_view name)
{
    return !fallback_list(name).empty() || name == "default-src";
}

bool is_known_directive(std::string_view name)
{
    static constexpr std::array<std::string_view, 12> others {
        "default-src", "base-uri", "sandbox", "form-action", "frame-ancestors", "report-uri", "report-to",
        "upgrade-insecure-requests", "block-all-mixed-content", "require-trusted-types-for", "trusted-types",
        "webrtc"
    };
    return is_fetch_directive(name) || std::find(others.begin(), others.end(), name) != others.end();
}

} // namespace

Url upgraded_insecure(Url url)
{
    if (url.scheme == "http") {
        url.scheme = "https";
        if (url.port && *url.port == 80)
            url.port.reset();
        else if (url.port && *url.port == 443)
            url.port.reset();
    } else if (url.scheme == "ws") {
        url.scheme = "wss";
        if (url.port && (*url.port == 80 || *url.port == 443))
            url.port.reset();
    }
    return url;
}

std::string ContentSecurityPolicy::Directive::text() const
{
    std::string out = name;
    for (std::string const& value : values) {
        out += ' ';
        out += value;
    }
    return out;
}

bool ContentSecurityPolicy::Directive::has_keyword(std::string_view keyword) const
{
    for (Source const& source : sources)
        if (source.kind == Source::Kind::Keyword && source.keyword == keyword)
            return true;
    return false;
}

ContentSecurityPolicy::Directive const* ContentSecurityPolicy::Policy::find(std::string_view name) const
{
    for (Directive const& directive : directives)
        if (directive.name == name)
            return &directive;
    return nullptr;
}

ContentSecurityPolicy::ContentSecurityPolicy(Url document_url)
    : m_document_url(std::move(document_url))
{
}

void ContentSecurityPolicy::set_reporter(std::function<void(std::string_view)> report)
{
    m_report = std::move(report);
}

void ContentSecurityPolicy::report(std::string_view message)
{
    if (!m_report)
        return;
    if (m_reported.size() > 512)
        m_reported.clear();
    if (!m_reported.emplace(message).second)
        return;
    m_report(message);
}

std::string ContentSecurityPolicy::sha256_source(std::string_view text)
{
    crypto::Sha256::Digest const digest = crypto::Sha256::hash(
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
    return "sha256-" + base64_encode(digest);
}

// §6.9 "Parse a serialized CSP": directives separated by ";", each a name
// and its whitespace-separated values; the first directive of a name wins.
ContentSecurityPolicy::Policy ContentSecurityPolicy::parse_policy(std::string_view text, bool report_only, bool from_meta)
{
    Policy policy;
    policy.report_only = report_only;
    policy.from_meta = from_meta;
    for (std::string_view const raw : split_on(text, ';')) {
        std::string_view const token = trimmed(raw);
        if (token.empty())
            continue;
        std::size_t name_end = 0;
        while (name_end < token.size() && !is_policy_whitespace(token[name_end]))
            ++name_end;
        std::string const name = lowercased(token.substr(0, name_end));
        if (!std::all_of(name.begin(), name.end(), is_token_char))
            continue;
        if (policy.find(name) != nullptr) {
            report("Ignoring duplicate Content-Security-Policy directive '" + name + "'.");
            continue;
        }
        if (from_meta && (name == "report-uri" || name == "frame-ancestors" || name == "sandbox")) {
            report("The Content-Security-Policy directive '" + name + "' is ignored when delivered via a <meta> element.");
            continue;
        }
        if (!is_known_directive(name))
            report("Unrecognized Content-Security-Policy directive '" + name + "'.");
        Directive directive;
        directive.name = name;
        for (std::string_view const value : split_on_whitespace(token.substr(name_end)))
            directive.values.emplace_back(value);
        if (is_fetch_directive(name) || name == "form-action" || name == "base-uri" || name == "frame-ancestors") {
            for (std::string const& value : directive.values) {
                if (std::optional<Source> source = parse_source(value))
                    directive.sources.push_back(std::move(*source));
                else
                    report("The source list for the Content-Security-Policy directive '" + name + "' contains an invalid source: '"
                        + value + "'. It will be ignored.");
            }
        }
        policy.directives.push_back(std::move(directive));
    }
    return policy;
}

void ContentSecurityPolicy::add_policy(Policy policy)
{
    if (policy.directives.empty())
        return;
    if (!policy.report_only) {
        if (policy.find("upgrade-insecure-requests") != nullptr)
            m_upgrade_insecure = true;
        if (Directive const* sandbox = policy.find("sandbox")) {
            m_sandboxed = true;
            bool scripts = false;
            bool forms = false;
            for (std::string const& value : sandbox->values) {
                std::string const flag = lowercased(value);
                scripts = scripts || flag == "allow-scripts";
                forms = forms || flag == "allow-forms";
            }
            m_sandbox_scripts = m_sandbox_scripts && scripts;
            m_sandbox_forms = m_sandbox_forms && forms;
        }
    }
    m_policies.push_back(std::move(policy));
}

void ContentSecurityPolicy::add_header(std::string_view value, bool report_only)
{
    for (std::string_view const one : split_on(value, ','))
        add_policy(parse_policy(one, report_only, false));
}

void ContentSecurityPolicy::add_meta(std::string_view content)
{
    std::string const key(trimmed(content));
    if (key.empty() || !m_meta_seen.emplace(key).second)
        return;
    add_policy(parse_policy(key, false, true));
}

ContentSecurityPolicy::Directive const* ContentSecurityPolicy::governing(Policy const& policy, std::string_view effective)
{
    for (std::string_view const name : fallback_list(effective))
        if (Directive const* directive = policy.find(name))
            return directive;
    return nullptr;
}

// §6.7.2.9 "Does url match expression in origin with redirect count".
bool ContentSecurityPolicy::source_matches(Source const& source, Url const& url, bool redirected) const
{
    switch (source.kind) {
    case Source::Kind::Any:
        return url.scheme == "http" || url.scheme == "https" || url.scheme == "ws" || url.scheme == "wss"
            || (!m_document_url.scheme.empty() && url.scheme == m_document_url.scheme);
    case Source::Kind::Scheme:
        return scheme_part_matches(source.scheme, url.scheme);
    case Source::Kind::Host: {
        if (!source.scheme.empty()) {
            if (!scheme_part_matches(source.scheme, url.scheme))
                return false;
        } else if (!m_document_url.scheme.empty() && !scheme_part_matches(m_document_url.scheme, url.scheme)) {
            return false;
        }
        if (!url.has_host())
            return false;
        if (!host_part_matches(source.host, lowercased(url.host)))
            return false;
        if (!port_part_matches(source.port, url))
            return false;
        // A redirect's target is judged without the paths (§4.2.4 step 3).
        return redirected || path_part_matches(source.path, url);
    }
    case Source::Kind::Keyword: {
        if (source.keyword != "self")
            return false;
        // §6.7.2.10: the document's origin, and its secure form.
        if (m_document_url.scheme.empty() || m_document_url.scheme == "data" || m_document_url.scheme == "about"
            || m_document_url.scheme == "blob" || m_document_url.scheme == "javascript")
            return false;
        if (url.host != m_document_url.host)
            return false;
        if (url.scheme == m_document_url.scheme && url.port == m_document_url.port)
            return true;
        return (url.scheme == "https" || url.scheme == "wss") && url.port == m_document_url.port;
    }
    case Source::Kind::Nonce:
    case Source::Kind::Hash:
        return false;
    }
    return false;
}

// §6.7.2.2 "Does url match source list": nothing matches an empty list or
// 'none' alone.
bool ContentSecurityPolicy::list_matches(Directive const& directive, Url const& url, bool redirected) const
{
    if (directive.sources.empty())
        return false;
    if (directive.sources.size() == 1 && directive.sources[0].kind == Source::Kind::Keyword
        && directive.sources[0].keyword == "none")
        return false;
    for (Source const& source : directive.sources)
        if (source_matches(source, url, redirected))
            return true;
    return false;
}

std::optional<std::string> ContentSecurityPolicy::request_refusal(ResourceKind kind, Url const& url, bool redirected,
    std::string_view nonce, bool parser_inserted)
{
    std::string_view const effective = effective_directive_for(kind);
    if (effective.empty())
        return std::nullopt;
    std::optional<std::string> refusal;
    for (Policy const& policy : m_policies) {
        Directive const* const directive = governing(policy, effective);
        if (!directive)
            continue;
        bool allowed = false;
        if (!nonce.empty()) {
            for (Source const& source : directive->sources)
                if (source.kind == Source::Kind::Nonce && source.nonce == nonce)
                    allowed = true;
        }
        if (!allowed && is_script_directive_name(effective) && directive->has_keyword("strict-dynamic")) {
            // §6.7.2.3 with 'strict-dynamic': host and scheme sources are
            // set aside; a script that a trusted script inserted may load.
            allowed = !parser_inserted;
        } else if (!allowed) {
            allowed = list_matches(*directive, url, redirected);
        }
        if (allowed)
            continue;
        std::string message = policy.report_only ? "[Report Only] " : "";
        if (effective == "connect-src")
            message += "Refused to connect to '" + url.serialize() + "'";
        else
            message += "Refused to load the " + std::string(thing_named(kind)) + " '" + url.serialize() + "'";
        message += " because it violates the following Content Security Policy directive: \"" + directive->text() + "\"."
            + fallback_note(effective, directive->name);
        report(message);
        if (!policy.report_only && !refusal) {
            ++m_refusals;
            refusal = "refused by the page's Content Security Policy: " + directive->text();
        }
    }
    return refusal;
}

RequestGuard ContentSecurityPolicy::guard(ResourceKind kind, std::string nonce, bool parser_inserted)
{
    RequestGuard guard;
    guard.upgrade_insecure = m_upgrade_insecure;
    if (!m_policies.empty()) {
        guard.refusal = [this, kind, nonce = std::move(nonce), parser_inserted](Url const& url, bool redirected) {
            return request_refusal(kind, url, redirected, nonce, parser_inserted);
        };
    }
    return guard;
}

// §6.4.2, frame-ancestors' navigation response check: every document the
// frame is inside, by its origin — serialized and parsed back into a URL,
// so a source naming a path beyond "/" matches none of them, and an opaque
// origin parses to nothing that matches.
std::optional<std::string> ContentSecurityPolicy::frame_ancestors_refusal(std::vector<Url> const& ancestors)
{
    std::optional<std::string> refusal;
    for (Policy const& policy : m_policies) {
        Directive const* const directive = policy.find("frame-ancestors");
        if (!directive)
            continue;
        for (Url const& ancestor : ancestors) {
            std::optional<Url> const origin = parse_url(ancestor.serialize_origin());
            if (origin && list_matches(*directive, *origin, false))
                continue;
            std::string const message = std::string(policy.report_only ? "[Report Only] " : "") + "Refused to frame '"
                + m_document_url.serialize()
                + "' because an ancestor violates the following Content Security Policy directive: \""
                + directive->text() + "\".";
            report(message);
            if (!policy.report_only && !refusal) {
                ++m_refusals;
                refusal = "refused by the framed document's Content Security Policy: " + directive->text();
            }
            break;
        }
    }
    return refusal;
}

bool ContentSecurityPolicy::governs_framing() const
{
    for (Policy const& policy : m_policies) {
        if (!policy.report_only && policy.find("frame-ancestors") != nullptr)
            return true;
    }
    return false;
}

// §6.7.3.3 "Does element match source list for type and source".
bool ContentSecurityPolicy::inline_matches(Directive const& directive, InlineKind kind, std::string_view nonce,
    std::string_view source) const
{
    bool const script = kind == InlineKind::Script || kind == InlineKind::ScriptAttribute;
    bool const attribute = kind == InlineKind::ScriptAttribute || kind == InlineKind::StyleAttribute;
    // §6.7.3.1 "Does a source list allow all inline behavior": 'unsafe-inline'
    // with no nonce or hash beside it, and no 'strict-dynamic' for scripts.
    bool unsafe_inline = false;
    bool nonce_or_hash = false;
    bool strict_dynamic = false;
    bool unsafe_hashes = false;
    for (Source const& candidate : directive.sources) {
        if (candidate.kind == Source::Kind::Nonce || candidate.kind == Source::Kind::Hash)
            nonce_or_hash = true;
        else if (candidate.kind == Source::Kind::Keyword && candidate.keyword == "unsafe-inline")
            unsafe_inline = true;
        else if (candidate.kind == Source::Kind::Keyword && candidate.keyword == "strict-dynamic")
            strict_dynamic = true;
        else if (candidate.kind == Source::Kind::Keyword && candidate.keyword == "unsafe-hashes")
            unsafe_hashes = true;
    }
    if (unsafe_inline && !nonce_or_hash && !(script && strict_dynamic))
        return true;
    if (!attribute && !nonce.empty()) {
        for (Source const& candidate : directive.sources)
            if (candidate.kind == Source::Kind::Nonce && candidate.nonce == nonce)
                return true;
    }
    // A hash matches an attribute only under 'unsafe-hashes'.
    if (attribute && !unsafe_hashes)
        return false;
    std::span<std::uint8_t const> const bytes(reinterpret_cast<std::uint8_t const*>(source.data()), source.size());
    for (Source const& candidate : directive.sources) {
        if (candidate.kind != Source::Kind::Hash)
            continue;
        if (crypto::hash_with(candidate.algorithm, bytes) == candidate.digest)
            return true;
    }
    return false;
}

std::optional<std::string> ContentSecurityPolicy::inline_refusal(InlineKind kind, std::string_view nonce, std::string_view source)
{
    std::string_view const effective = effective_directive_for(kind);
    std::optional<std::string> refusal;
    for (Policy const& policy : m_policies) {
        Directive const* const directive = governing(policy, effective);
        if (!directive || inline_matches(*directive, kind, nonce, source))
            continue;
        std::string message = policy.report_only ? "[Report Only] " : "";
        switch (kind) {
        case InlineKind::Script:
            message += "Refused to execute inline script";
            break;
        case InlineKind::ScriptAttribute:
            message += "Refused to execute inline event handler";
            break;
        case InlineKind::Style:
            message += "Refused to apply inline style";
            break;
        case InlineKind::StyleAttribute:
            message += "Refused to apply inline style";
            break;
        }
        message += " because it violates the following Content Security Policy directive: \"" + directive->text()
            + "\". Either the 'unsafe-inline' keyword, a hash ('" + sha256_source(source)
            + "'), or a nonce ('nonce-...') is required to enable inline execution.";
        if (kind == InlineKind::ScriptAttribute || kind == InlineKind::StyleAttribute)
            message += " Note that hashes do not apply to event handlers, style attributes and javascript: navigations unless the 'unsafe-hashes' keyword is present.";
        message += fallback_note(effective, directive->name);
        report(message);
        if (!policy.report_only && !refusal) {
            ++m_refusals;
            refusal = "refused by the page's Content Security Policy: " + directive->text();
        }
    }
    return refusal;
}

// EnsureCSPDoesNotBlockStringCompilation (§8.2): script-src, else
// default-src, must carry 'unsafe-eval'.
std::optional<std::string> ContentSecurityPolicy::eval_refusal()
{
    std::optional<std::string> refusal;
    for (Policy const& policy : m_policies) {
        Directive const* const directive = governing(policy, "script-src");
        if (!directive || directive->has_keyword("unsafe-eval"))
            continue;
        std::string message = policy.report_only ? "[Report Only] " : "";
        message += "Refused to evaluate a string as JavaScript because 'unsafe-eval' is not an allowed source of script in the following Content Security Policy directive: \""
            + directive->text() + "\".";
        report(message);
        if (!policy.report_only && !refusal) {
            ++m_refusals;
            refusal = "Refused to evaluate a string as JavaScript because 'unsafe-eval' is not an allowed source of script in the following Content Security Policy directive: \""
                + directive->text() + "\"";
        }
    }
    return refusal;
}

std::optional<std::string> ContentSecurityPolicy::form_action_refusal(Url const& action)
{
    std::optional<std::string> refusal;
    for (Policy const& policy : m_policies) {
        Directive const* const directive = policy.find("form-action");
        if (!directive || list_matches(*directive, action, false))
            continue;
        std::string message = policy.report_only ? "[Report Only] " : "";
        message += "Refused to send form data to '" + action.serialize()
            + "' because it violates the following Content Security Policy directive: \"" + directive->text() + "\".";
        report(message);
        if (!policy.report_only && !refusal) {
            ++m_refusals;
            refusal = "refused by the page's Content Security Policy: " + directive->text();
        }
    }
    return refusal;
}

}
