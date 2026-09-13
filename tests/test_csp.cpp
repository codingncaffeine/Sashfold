#include "Test.h"

#include "core/Base64.h"
#include "crypto/Sha2.h"
#include "net/Csp.h"
#include "net/Url.h"

#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Content Security Policy: the serialized policy parsed, source
// expressions matched the way the specification's algorithms match them
// (schemes and their secure forms, wildcard hosts, ports, path prefixes,
// 'self' and its upgrade), the fallback chains, nonces and hashes for
// inline scripts and styles, 'strict-dynamic', the eval gate, report-only
// dispositions, the meta restrictions, sandbox, upgrade-insecure-requests
// and form-action — and the console lines each violation leaves.

using namespace sashfold;
using namespace sashfold::net;

namespace {

Url url_of(std::string_view text)
{
    std::optional<Url> const url = parse_url(text);
    if (!url)
        std::abort();
    return *url;
}

struct Reports {
    std::vector<std::string> lines;
    bool has(std::string_view part) const
    {
        for (std::string const& line : lines)
            if (line.find(part) != std::string::npos)
                return true;
        return false;
    }
};

// A policy for a page, from one header value.
ContentSecurityPolicy policy_for(std::string_view page, std::string_view header, Reports* reports = nullptr,
    bool report_only = false)
{
    ContentSecurityPolicy policy(url_of(page));
    if (reports)
        policy.set_reporter([reports](std::string_view message) { reports->lines.emplace_back(message); });
    policy.add_header(header, report_only);
    return policy;
}

bool refused(ContentSecurityPolicy& policy, ResourceKind kind, std::string_view target, bool redirected = false,
    std::string_view nonce = {}, bool parser_inserted = true)
{
    return policy.request_refusal(kind, url_of(target), redirected, nonce, parser_inserted).has_value();
}

std::string hash_source(std::string_view name, crypto::HashId id, std::string_view text)
{
    std::vector<std::uint8_t> const digest = crypto::hash_with(id,
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(text.data()), text.size()));
    return "'" + std::string(name) + "-" + base64_encode(digest) + "'";
}

void test_parsing()
{
    Reports reports;
    ContentSecurityPolicy policy(url_of("https://example.test/"));
    policy.set_reporter([&reports](std::string_view message) { reports.lines.emplace_back(message); });
    policy.add_header("  Default-Src 'self' ; img-src https: data: ;; script-src 'nonce-Abc+/=' 'sha256-notbase64!' ; "
                      "img-src 'none'; made-up-src x",
        false);
    CHECK_EQ(policy.policies().size(), std::size_t { 1 });
    ContentSecurityPolicy::Policy const& parsed = policy.policies()[0];
    CHECK_EQ(parsed.directives.size(), std::size_t { 4 });
    CHECK_EQ(parsed.directives[0].name, "default-src");
    CHECK_EQ(parsed.directives[0].text(), "default-src 'self'");
    CHECK_EQ(parsed.directives[1].values.size(), std::size_t { 2 });
    CHECK_EQ(parsed.directives[1].sources.size(), std::size_t { 2 });
    // The nonce keeps its case; the hash that is not base64 is dropped.
    CHECK_EQ(parsed.directives[2].sources.size(), std::size_t { 1 });
    CHECK_EQ(parsed.directives[2].sources[0].nonce, "Abc+/=");
    CHECK(reports.has("directive 'script-src' contains an invalid source: ''sha256-notbase64!''. It will be ignored."));
    // The second img-src is a duplicate and the made-up one unknown.
    CHECK(reports.has("Ignoring duplicate Content-Security-Policy directive 'img-src'"));
    CHECK(reports.has("Unrecognized Content-Security-Policy directive 'made-up-src'"));
    CHECK(parsed.find("img-src") != nullptr);
    CHECK(!parsed.find("img-src")->has_keyword("none"));

    // A comma separates policies in one header value; an empty one is nothing.
    ContentSecurityPolicy two = policy_for("https://example.test/", "img-src 'none', script-src 'self',  ");
    CHECK_EQ(two.policies().size(), std::size_t { 2 });
    CHECK(two.policies()[0].find("img-src") != nullptr);
    CHECK(two.policies()[1].find("script-src") != nullptr);
    CHECK(!two.empty());
    ContentSecurityPolicy none = policy_for("https://example.test/", "");
    CHECK(none.empty());
}

void test_host_sources()
{
    ContentSecurityPolicy policy = policy_for("https://example.test/page",
        "img-src example.com *.wild.com ports.com:8443 star.com:* paths.com/images/ exact.com/one.png "
        "Upper.COM http://plain.com ws://sock.com");
    // A bare host takes the page's scheme, and its secure form: on an
    // https page, http is neither.
    CHECK(!refused(policy, ResourceKind::Image, "https://example.com/a.png"));
    CHECK(refused(policy, ResourceKind::Image, "http://example.com/a.png"));
    CHECK(refused(policy, ResourceKind::Image, "https://sub.example.com/a.png"));
    CHECK(refused(policy, ResourceKind::Image, "https://example.com:8443/a.png"));
    CHECK(!refused(policy, ResourceKind::Image, "https://example.com:443/a.png")); // the default port, as written
    // A wildcard host is the subdomains, never the apex.
    CHECK(!refused(policy, ResourceKind::Image, "https://a.wild.com/x"));
    CHECK(!refused(policy, ResourceKind::Image, "https://a.b.wild.com/x"));
    CHECK(refused(policy, ResourceKind::Image, "https://wild.com/x"));
    CHECK(refused(policy, ResourceKind::Image, "https://notwild.com/x"));
    // Ports: the one written, or any.
    CHECK(!refused(policy, ResourceKind::Image, "https://ports.com:8443/x"));
    CHECK(refused(policy, ResourceKind::Image, "https://ports.com/x"));
    CHECK(!refused(policy, ResourceKind::Image, "https://star.com:1234/x"));
    CHECK(!refused(policy, ResourceKind::Image, "https://star.com/x"));
    // A path ending in "/" is a prefix, any other is exact; a redirect's
    // target is judged without the paths.
    CHECK(!refused(policy, ResourceKind::Image, "https://paths.com/images/a.png"));
    CHECK(!refused(policy, ResourceKind::Image, "https://paths.com/images/deep/a.png"));
    CHECK(refused(policy, ResourceKind::Image, "https://paths.com/imagesx/a.png"));
    CHECK(refused(policy, ResourceKind::Image, "https://paths.com/other/a.png"));
    CHECK(!refused(policy, ResourceKind::Image, "https://paths.com/other/a.png", true));
    CHECK(!refused(policy, ResourceKind::Image, "https://exact.com/one.png"));
    CHECK(!refused(policy, ResourceKind::Image, "https://exact.com/one.png?v=2#f"));
    CHECK(refused(policy, ResourceKind::Image, "https://exact.com/one.png/x"));
    CHECK(refused(policy, ResourceKind::Image, "https://exact.com/two.png"));
    // Hosts compare without case; an explicit scheme part matches itself
    // and its secure form.
    CHECK(!refused(policy, ResourceKind::Image, "https://upper.com/x"));
    CHECK(!refused(policy, ResourceKind::Image, "http://plain.com/x"));
    CHECK(!refused(policy, ResourceKind::Image, "https://plain.com/x"));
    CHECK(!refused(policy, ResourceKind::Image, "wss://sock.com/x"));
    CHECK(refused(policy, ResourceKind::Image, "ftp://plain.com/x"));

    // Scheme sources, the star, and 'none'.
    ContentSecurityPolicy schemes = policy_for("https://example.test/", "img-src https: data:; font-src *; media-src 'none'; object-src");
    CHECK(!refused(schemes, ResourceKind::Image, "https://anywhere.example/a.png"));
    CHECK(!refused(schemes, ResourceKind::Image, "data:image/png;base64,AAAA"));
    CHECK(refused(schemes, ResourceKind::Image, "http://anywhere.example/a.png"));
    CHECK(refused(schemes, ResourceKind::Image, "blob:https://example.test/uuid"));
    CHECK(!refused(schemes, ResourceKind::Font, "http://anywhere.example/f.woff"));
    CHECK(!refused(schemes, ResourceKind::Font, "wss://anywhere.example/f"));
    CHECK(refused(schemes, ResourceKind::Font, "data:font/woff;base64,AAAA"));
    CHECK(refused(schemes, ResourceKind::Media, "https://example.test/a.mp4"));
    CHECK(refused(schemes, ResourceKind::Media, "https://example.test/a.mp4", true));
    // 'none' beside another source is ignored; an empty list matches nothing.
    ContentSecurityPolicy mixed = policy_for("https://example.test/", "img-src 'none' https:");
    CHECK(!refused(mixed, ResourceKind::Image, "https://x.example/a.png"));
    // http: as a scheme source lets https through too.
    ContentSecurityPolicy plain = policy_for("http://example.test/", "img-src http:");
    CHECK(!refused(plain, ResourceKind::Image, "https://x.example/a.png"));
    CHECK(!refused(plain, ResourceKind::Image, "http://x.example/a.png"));
    // The star on an http page: every network scheme, never data:.
    ContentSecurityPolicy star = policy_for("http://example.test/", "img-src *");
    CHECK(!refused(star, ResourceKind::Image, "http://x.example/a.png"));
    CHECK(refused(star, ResourceKind::Image, "data:image/png;base64,AAAA"));
}

void test_self()
{
    ContentSecurityPolicy secure = policy_for("https://example.test/dir/page", "img-src 'self'");
    CHECK(!refused(secure, ResourceKind::Image, "https://example.test/a.png"));
    CHECK(!refused(secure, ResourceKind::Image, "https://example.test:443/a.png"));
    CHECK(refused(secure, ResourceKind::Image, "http://example.test/a.png"));
    CHECK(refused(secure, ResourceKind::Image, "https://other.test/a.png"));
    CHECK(refused(secure, ResourceKind::Image, "https://example.test:8443/a.png"));
    CHECK(refused(secure, ResourceKind::Image, "https://sub.example.test/a.png"));
    // On a plain page 'self' also names the same host over https.
    ContentSecurityPolicy plain = policy_for("http://example.test:8080/", "img-src 'self'");
    CHECK(!refused(plain, ResourceKind::Image, "http://example.test:8080/a.png"));
    CHECK(!refused(plain, ResourceKind::Image, "https://example.test:8080/a.png"));
    CHECK(refused(plain, ResourceKind::Image, "http://example.test/a.png"));
    ContentSecurityPolicy plain_default = policy_for("http://example.test/", "img-src 'self'");
    CHECK(!refused(plain_default, ResourceKind::Image, "https://example.test/a.png"));
    // A data: page has an opaque origin: 'self' is nothing there.
    ContentSecurityPolicy opaque = policy_for("data:text/html,x", "img-src 'self'");
    CHECK(refused(opaque, ResourceKind::Image, "data:image/png;base64,AAAA"));
    // A local page: its own files.
    ContentSecurityPolicy local = policy_for("file:///home/me/page.html", "img-src 'self'");
    CHECK(!refused(local, ResourceKind::Image, "file:///home/me/a.png"));
    CHECK(refused(local, ResourceKind::Image, "https://example.test/a.png"));
}

void test_fallback_chains()
{
    // default-src governs what no fetch directive names.
    ContentSecurityPolicy defaults = policy_for("https://example.test/", "default-src 'self'; font-src *");
    CHECK(refused(defaults, ResourceKind::Image, "https://cdn.test/a.png"));
    CHECK(refused(defaults, ResourceKind::Script, "https://cdn.test/a.js"));
    CHECK(refused(defaults, ResourceKind::Stylesheet, "https://cdn.test/a.css"));
    CHECK(refused(defaults, ResourceKind::Xhr, "https://api.test/x"));
    CHECK(refused(defaults, ResourceKind::Other, "https://api.test/x"));
    CHECK(refused(defaults, ResourceKind::Subdocument, "https://frame.test/x"));
    CHECK(!refused(defaults, ResourceKind::Font, "https://cdn.test/a.woff"));
    CHECK(!refused(defaults, ResourceKind::Image, "https://example.test/a.png"));
    // A navigation is never a fetch directive's business.
    CHECK(!refused(defaults, ResourceKind::Document, "https://elsewhere.test/"));
    // script-src governs the element requests; script-src-elem beats it.
    ContentSecurityPolicy scripts = policy_for("https://example.test/", "default-src 'none'; script-src a.test; script-src-elem b.test");
    CHECK(!refused(scripts, ResourceKind::Script, "https://b.test/x.js"));
    CHECK(refused(scripts, ResourceKind::Script, "https://a.test/x.js"));
    ContentSecurityPolicy scripts_only = policy_for("https://example.test/", "default-src 'none'; script-src a.test");
    CHECK(!refused(scripts_only, ResourceKind::Script, "https://a.test/x.js"));
    CHECK(refused(scripts_only, ResourceKind::Image, "https://a.test/x.png"));
    // connect-src without default-src: images are free, connections not.
    ContentSecurityPolicy connect = policy_for("https://example.test/", "connect-src 'self'");
    CHECK(!refused(connect, ResourceKind::Image, "https://cdn.test/a.png"));
    CHECK(refused(connect, ResourceKind::Xhr, "https://api.test/x"));
    CHECK(!refused(connect, ResourceKind::Xhr, "https://example.test/api"));
    // form-action has no fallback: default-src 'none' leaves forms alone.
    ContentSecurityPolicy forms = policy_for("https://example.test/", "default-src 'none'");
    CHECK(!forms.form_action_refusal(url_of("https://elsewhere.test/submit")));
    ContentSecurityPolicy forms_named = policy_for("https://example.test/", "form-action 'self'");
    CHECK(!forms_named.form_action_refusal(url_of("https://example.test/submit")));
    CHECK(forms_named.form_action_refusal(url_of("https://elsewhere.test/submit")).has_value());
    CHECK_EQ(forms_named.refusals(), std::size_t { 1 });
    // Every policy must allow: two policies, the second stricter.
    ContentSecurityPolicy both = policy_for("https://example.test/", "img-src *, img-src 'self'");
    CHECK(refused(both, ResourceKind::Image, "https://cdn.test/a.png"));
    CHECK(!refused(both, ResourceKind::Image, "https://example.test/a.png"));
}

void test_nonces_hashes_and_strict_dynamic_for_requests()
{
    ContentSecurityPolicy nonced = policy_for("https://example.test/", "script-src 'nonce-r4nd0m' 'self'");
    CHECK(!refused(nonced, ResourceKind::Script, "https://cdn.test/a.js", false, "r4nd0m"));
    CHECK(refused(nonced, ResourceKind::Script, "https://cdn.test/a.js", false, "R4ND0M"));
    CHECK(refused(nonced, ResourceKind::Script, "https://cdn.test/a.js"));
    CHECK(!refused(nonced, ResourceKind::Script, "https://example.test/a.js"));
    // The nonce carries across a redirect.
    CHECK(!refused(nonced, ResourceKind::Script, "https://cdn.test/b.js", true, "r4nd0m"));
    // A hash source names no URL.
    ContentSecurityPolicy hashed = policy_for("https://example.test/", "script-src 'sha256-47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU='");
    CHECK(refused(hashed, ResourceKind::Script, "https://example.test/a.js"));
    // 'strict-dynamic': the hosts are set aside; a nonce lets a
    // parser-inserted script load, and a script a script inserted loads.
    ContentSecurityPolicy dynamic = policy_for("https://example.test/", "script-src 'strict-dynamic' 'nonce-n1' https: 'self'");
    CHECK(refused(dynamic, ResourceKind::Script, "https://cdn.test/a.js"));
    CHECK(refused(dynamic, ResourceKind::Script, "https://example.test/a.js"));
    CHECK(!refused(dynamic, ResourceKind::Script, "https://cdn.test/a.js", false, "n1"));
    CHECK(!refused(dynamic, ResourceKind::Script, "https://cdn.test/a.js", false, {}, false));
    // Not for stylesheets: their hosts still count.
    ContentSecurityPolicy styles = policy_for("https://example.test/", "style-src 'strict-dynamic' https:");
    CHECK(!refused(styles, ResourceKind::Stylesheet, "https://cdn.test/a.css"));
}

void test_inline_checks()
{
    std::string const source = "alert(1)";
    ContentSecurityPolicy strict = policy_for("https://example.test/", "script-src 'self'; style-src 'self'");
    CHECK(strict.inline_refusal(InlineKind::Script, {}, source).has_value());
    CHECK(strict.inline_refusal(InlineKind::ScriptAttribute, {}, source).has_value());
    CHECK(strict.inline_refusal(InlineKind::Style, {}, "p{}").has_value());
    CHECK(strict.inline_refusal(InlineKind::StyleAttribute, {}, "color:red").has_value());
    CHECK_EQ(strict.refusals(), std::size_t { 4 });

    ContentSecurityPolicy unsafe = policy_for("https://example.test/", "script-src 'unsafe-inline'; style-src 'unsafe-inline'");
    CHECK(!unsafe.inline_refusal(InlineKind::Script, {}, source));
    CHECK(!unsafe.inline_refusal(InlineKind::ScriptAttribute, {}, source));
    CHECK(!unsafe.inline_refusal(InlineKind::Style, {}, "p{}"));
    CHECK(!unsafe.inline_refusal(InlineKind::StyleAttribute, {}, "color:red"));

    // A nonce or a hash beside 'unsafe-inline' turns it off.
    ContentSecurityPolicy nonced = policy_for("https://example.test/", "script-src 'unsafe-inline' 'nonce-abc'");
    CHECK(nonced.inline_refusal(InlineKind::Script, {}, source).has_value());
    CHECK(!nonced.inline_refusal(InlineKind::Script, "abc", source));
    CHECK(nonced.inline_refusal(InlineKind::Script, "abd", source).has_value());
    // A nonce never matches an attribute.
    CHECK(nonced.inline_refusal(InlineKind::ScriptAttribute, "abc", source).has_value());

    // Hashes, in each algorithm, base64url spelled too.
    std::string const sha256 = hash_source("sha256", crypto::HashId::Sha256, source);
    std::string const sha384 = hash_source("sha384", crypto::HashId::Sha384, source);
    std::string const sha512 = hash_source("sha512", crypto::HashId::Sha512, source);
    CHECK_EQ(sha256, "'" + ContentSecurityPolicy::sha256_source(source) + "'");
    for (std::string const& hash : { sha256, sha384, sha512 }) {
        ContentSecurityPolicy hashed = policy_for("https://example.test/", "script-src " + hash);
        CHECK(!hashed.inline_refusal(InlineKind::Script, {}, source));
        CHECK(hashed.inline_refusal(InlineKind::Script, {}, source + " ").has_value());
        // An attribute needs 'unsafe-hashes' for a hash to count.
        CHECK(hashed.inline_refusal(InlineKind::ScriptAttribute, {}, source).has_value());
        ContentSecurityPolicy attributes = policy_for("https://example.test/", "script-src 'unsafe-hashes' " + hash);
        CHECK(!attributes.inline_refusal(InlineKind::ScriptAttribute, {}, source));
    }
    std::string url_spelled = sha256;
    for (char& c : url_spelled) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    ContentSecurityPolicy url_hashed = policy_for("https://example.test/", "script-src " + url_spelled);
    CHECK(!url_hashed.inline_refusal(InlineKind::Script, {}, source));
    // Styles hash the same way.
    ContentSecurityPolicy style_hashed = policy_for("https://example.test/", "style-src 'unsafe-hashes' " + hash_source("sha256", crypto::HashId::Sha256, "color:red"));
    CHECK(!style_hashed.inline_refusal(InlineKind::StyleAttribute, {}, "color:red"));
    CHECK(style_hashed.inline_refusal(InlineKind::StyleAttribute, {}, "color:blue").has_value());
    CHECK(!style_hashed.inline_refusal(InlineKind::Style, {}, "color:red"));

    // 'strict-dynamic' sets 'unsafe-inline' aside for scripts, not styles.
    ContentSecurityPolicy dynamic = policy_for("https://example.test/", "default-src 'strict-dynamic' 'unsafe-inline'");
    CHECK(dynamic.inline_refusal(InlineKind::Script, {}, source).has_value());
    CHECK(!dynamic.inline_refusal(InlineKind::Style, {}, "p{}"));

    // The elem and attr directives, and their fallbacks.
    ContentSecurityPolicy split = policy_for("https://example.test/", "script-src 'unsafe-inline'; script-src-attr 'none'; style-src-elem 'none'");
    CHECK(!split.inline_refusal(InlineKind::Script, {}, source));
    CHECK(split.inline_refusal(InlineKind::ScriptAttribute, {}, source).has_value());
    CHECK(split.inline_refusal(InlineKind::Style, {}, "p{}").has_value());
    CHECK(!split.inline_refusal(InlineKind::StyleAttribute, {}, "color:red"));

    // No policy, or none that governs: everything applies.
    ContentSecurityPolicy empty = policy_for("https://example.test/", "");
    CHECK(!empty.inline_refusal(InlineKind::Script, {}, source));
    ContentSecurityPolicy images = policy_for("https://example.test/", "img-src 'none'");
    CHECK(!images.inline_refusal(InlineKind::Script, {}, source));
    CHECK(!images.inline_refusal(InlineKind::StyleAttribute, {}, "color:red"));
}

void test_eval()
{
    ContentSecurityPolicy strict = policy_for("https://example.test/", "script-src 'self'");
    CHECK(strict.eval_refusal().has_value());
    CHECK(strict.eval_refusal()->find("'unsafe-eval'") != std::string::npos);
    ContentSecurityPolicy allowed = policy_for("https://example.test/", "script-src 'self' 'unsafe-eval'");
    CHECK(!allowed.eval_refusal());
    ContentSecurityPolicy defaults = policy_for("https://example.test/", "default-src 'self'");
    CHECK(defaults.eval_refusal().has_value());
    // Only script-src and default-src govern string compilation.
    ContentSecurityPolicy elements = policy_for("https://example.test/", "script-src-elem 'none'; img-src 'none'");
    CHECK(!elements.eval_refusal());
    ContentSecurityPolicy empty = policy_for("https://example.test/", "");
    CHECK(!empty.eval_refusal());
}

void test_report_only_and_messages()
{
    Reports reports;
    ContentSecurityPolicy watching = policy_for("https://example.test/", "img-src 'self'", &reports, true);
    CHECK(!refused(watching, ResourceKind::Image, "https://cdn.test/a.png"));
    CHECK_EQ(watching.refusals(), std::size_t { 0 });
    CHECK_EQ(reports.lines.size(), std::size_t { 1 });
    CHECK_EQ(reports.lines[0],
        "[Report Only] Refused to load the image 'https://cdn.test/a.png' because it violates the following "
        "Content Security Policy directive: \"img-src 'self'\".");
    CHECK(!watching.inline_refusal(InlineKind::Script, {}, "x"));
    CHECK(!watching.eval_refusal());
    CHECK(!watching.form_action_refusal(url_of("https://x.test/")));
    // The same violation is one line.
    CHECK(!refused(watching, ResourceKind::Image, "https://cdn.test/a.png"));
    CHECK_EQ(reports.lines.size(), std::size_t { 1 });

    // Enforced: the line names the fallback the directive came through.
    Reports enforced_reports;
    ContentSecurityPolicy enforced = policy_for("https://example.test/", "default-src 'self'", &enforced_reports);
    std::optional<std::string> const refusal = enforced.request_refusal(ResourceKind::Script, url_of("https://cdn.test/a.js"), false);
    CHECK(refusal.has_value());
    CHECK_EQ(*refusal, "refused by the page's Content Security Policy: default-src 'self'");
    CHECK_EQ(enforced_reports.lines.size(), std::size_t { 1 });
    CHECK_EQ(enforced_reports.lines[0],
        "Refused to load the script 'https://cdn.test/a.js' because it violates the following Content Security "
        "Policy directive: \"default-src 'self'\". Note that 'script-src-elem' was not explicitly set, so "
        "'default-src' is used as a fallback.");
    CHECK(enforced.request_refusal(ResourceKind::Xhr, url_of("https://api.test/x"), false).has_value());
    CHECK(enforced_reports.has("Refused to connect to 'https://api.test/x'"));
    CHECK(enforced.inline_refusal(InlineKind::Script, {}, "alert(1)").has_value());
    CHECK(enforced_reports.has("Refused to execute inline script because it violates the following Content Security Policy "
                               "directive: \"default-src 'self'\". Either the 'unsafe-inline' keyword, a hash ('"
        + ContentSecurityPolicy::sha256_source("alert(1)") + "'), or a nonce ('nonce-...') is required"));
    CHECK(enforced.inline_refusal(InlineKind::ScriptAttribute, {}, "go()").has_value());
    CHECK(enforced_reports.has("Refused to execute inline event handler"));
    CHECK(enforced_reports.has("unless the 'unsafe-hashes' keyword is present"));
    CHECK(enforced.inline_refusal(InlineKind::StyleAttribute, {}, "color:red").has_value());
    CHECK(enforced_reports.has("Refused to apply inline style"));
    CHECK(enforced.eval_refusal().has_value());
    CHECK(enforced_reports.has("Refused to evaluate a string as JavaScript because 'unsafe-eval' is not an allowed source of script"));
    CHECK_EQ(enforced.refusals(), std::size_t { 6 });

    // A report-only policy beside an enforced one: both speak, one refuses.
    Reports pair_reports;
    ContentSecurityPolicy pair(url_of("https://example.test/"));
    pair.set_reporter([&pair_reports](std::string_view message) { pair_reports.lines.emplace_back(message); });
    pair.add_header("img-src 'self'", false);
    pair.add_header("img-src 'none'", true);
    CHECK(refused(pair, ResourceKind::Image, "https://cdn.test/a.png"));
    CHECK_EQ(pair_reports.lines.size(), std::size_t { 2 });
    CHECK(!refused(pair, ResourceKind::Image, "https://example.test/a.png"));
    CHECK_EQ(pair_reports.lines.size(), std::size_t { 3 });
    CHECK(pair_reports.lines[2].starts_with("[Report Only]"));
}

void test_meta_policies()
{
    Reports reports;
    ContentSecurityPolicy policy(url_of("https://example.test/"));
    policy.set_reporter([&reports](std::string_view message) { reports.lines.emplace_back(message); });
    policy.add_meta("img-src 'self'; report-uri /report; frame-ancestors 'none'; sandbox");
    CHECK_EQ(policy.policies().size(), std::size_t { 1 });
    CHECK(policy.policies()[0].from_meta);
    CHECK_EQ(policy.policies()[0].directives.size(), std::size_t { 1 });
    CHECK(reports.has("'report-uri' is ignored when delivered via a <meta> element"));
    CHECK(reports.has("'frame-ancestors' is ignored"));
    CHECK(reports.has("'sandbox' is ignored"));
    CHECK(!policy.sandboxed());
    CHECK(refused(policy, ResourceKind::Image, "https://cdn.test/a.png"));
    // The same content again is the same policy; another is another.
    policy.add_meta("img-src 'self'; report-uri /report; frame-ancestors 'none'; sandbox");
    policy.add_meta("  ");
    CHECK_EQ(policy.policies().size(), std::size_t { 1 });
    policy.add_meta("font-src 'none'");
    CHECK_EQ(policy.policies().size(), std::size_t { 2 });
    CHECK(refused(policy, ResourceKind::Font, "https://example.test/a.woff"));
}

void test_sandbox_and_upgrade()
{
    ContentSecurityPolicy open = policy_for("https://example.test/", "img-src 'self'");
    CHECK(!open.sandboxed());
    CHECK(open.sandbox_allows_scripts());
    CHECK(open.sandbox_allows_forms());
    CHECK(!open.upgrade_insecure_requests());
    ContentSecurityPolicy boxed = policy_for("https://example.test/", "sandbox");
    CHECK(boxed.sandboxed());
    CHECK(!boxed.sandbox_allows_scripts());
    CHECK(!boxed.sandbox_allows_forms());
    ContentSecurityPolicy scripts = policy_for("https://example.test/", "sandbox allow-scripts ALLOW-SAME-ORIGIN");
    CHECK(scripts.sandbox_allows_scripts());
    CHECK(!scripts.sandbox_allows_forms());
    ContentSecurityPolicy both = policy_for("https://example.test/", "sandbox allow-forms allow-scripts");
    CHECK(both.sandbox_allows_scripts());
    CHECK(both.sandbox_allows_forms());
    // The strictest of several sandboxes wins; a report-only one is nothing.
    ContentSecurityPolicy several = policy_for("https://example.test/", "sandbox allow-scripts, sandbox allow-forms");
    CHECK(!several.sandbox_allows_scripts());
    CHECK(!several.sandbox_allows_forms());
    ContentSecurityPolicy watching = policy_for("https://example.test/", "sandbox", nullptr, true);
    CHECK(!watching.sandboxed());

    ContentSecurityPolicy upgrading = policy_for("https://example.test/", "upgrade-insecure-requests; img-src 'self'");
    CHECK(upgrading.upgrade_insecure_requests());
    CHECK(upgrading.guard(ResourceKind::Image).upgrade_insecure);
    CHECK(!policy_for("https://example.test/", "upgrade-insecure-requests", nullptr, true).upgrade_insecure_requests());
    CHECK_EQ(upgraded_insecure(url_of("http://example.test/a.png")).serialize(), "https://example.test/a.png");
    CHECK_EQ(upgraded_insecure(url_of("http://example.test:80/a.png")).serialize(), "https://example.test/a.png");
    CHECK_EQ(upgraded_insecure(url_of("http://example.test:8080/a.png")).serialize(), "https://example.test:8080/a.png");
    CHECK_EQ(upgraded_insecure(url_of("ws://example.test/s")).serialize(), "wss://example.test/s");
    CHECK_EQ(upgraded_insecure(url_of("https://example.test/a.png")).serialize(), "https://example.test/a.png");
    CHECK_EQ(upgraded_insecure(url_of("data:text/plain,x")).serialize(), "data:text/plain,x");
}

void test_guards()
{
    ContentSecurityPolicy policy = policy_for("https://example.test/", "img-src example.test/images/ 'nonce-n'");
    RequestGuard const guard = policy.guard(ResourceKind::Image);
    CHECK(static_cast<bool>(guard));
    CHECK(static_cast<bool>(guard.refusal));
    CHECK(!guard.refusal(url_of("https://example.test/images/a.png"), false));
    CHECK(guard.refusal(url_of("https://example.test/other/a.png"), false).has_value());
    CHECK(!guard.refusal(url_of("https://example.test/other/a.png"), true));
    CHECK(guard.refusal(url_of("https://cdn.test/a.png"), true).has_value());
    CHECK_EQ(policy.refusals(), std::size_t { 2 });
    RequestGuard const nonced = policy.guard(ResourceKind::Image, "n");
    CHECK(!nonced.refusal(url_of("https://cdn.test/a.png"), false));
    // No policy: a guard that says nothing.
    ContentSecurityPolicy empty = policy_for("https://example.test/", "");
    RequestGuard const none = empty.guard(ResourceKind::Image);
    CHECK(!static_cast<bool>(none));
    CHECK(!none.refusal);
}

} // namespace

int main()
{
    test_parsing();
    test_host_sources();
    test_self();
    test_fallback_chains();
    test_nonces_hashes_and_strict_dynamic_for_requests();
    test_inline_checks();
    test_eval();
    test_report_only_and_messages();
    test_meta_policies();
    test_sandbox_and_upgrade();
    test_guards();
    return sashfold::test::report("csp");
}
