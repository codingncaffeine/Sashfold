#pragma once

// Content Security Policy (Level 3): a document's policies, from the
// Content-Security-Policy and Content-Security-Policy-Report-Only headers
// and from <meta http-equiv=content-security-policy>, and what they say
// about every request the page makes, every inline script and style it
// carries, every string it would compile into code, and every form it
// submits. The policy belongs to the document, on the page's side of the
// loader seam; the loader is handed a guard that asks it again on every
// redirect hop, paths ignored, as the specification has it. A report-only
// policy refuses nothing. Every violation, refused or not, is described to
// the page's console once, and no report ever leaves the machine:
// report-uri and report-to are read and left alone.

#include "crypto/Sha2.h"
#include "net/Filters.h"
#include "net/Url.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace sashfold::net {

// What an inline check is for (§6.7.3): a <script>'s text, an event
// handler attribute, a <style>'s text, a style="" attribute.
enum class InlineKind : std::uint8_t { Script, ScriptAttribute, Style, StyleAttribute };

// The page's say on a request the loader carries out for it: the refusal
// for a URL — the one asked for, or one a redirect leads to — or nullopt
// when it may go; and whether an insecure URL is upgraded before anything
// else. An empty guard says nothing.
struct RequestGuard {
    std::function<std::optional<std::string>(Url const& url, bool redirected)> refusal;
    bool upgrade_insecure = false;
    explicit operator bool() const { return static_cast<bool>(refusal) || upgrade_insecure; }
};

// http → https and ws → wss, an explicit default port along with the
// scheme: what upgrade-insecure-requests does to a request.
Url upgraded_insecure(Url url);

class ContentSecurityPolicy {
public:
    // One source expression of a directive's value, parsed (§6.7.2).
    struct Source {
        enum class Kind : std::uint8_t { Keyword, Nonce, Hash, Scheme, Host, Any };
        Kind kind = Kind::Keyword;
        std::string keyword; // 'self', 'unsafe-inline', … without the quotes, lowercased
        std::string nonce; // as written
        crypto::HashId algorithm = crypto::HashId::Sha256;
        std::vector<std::uint8_t> digest;
        std::string scheme; // a scheme-source, or a host-source's scheme part; lowercased
        std::string host; // lowercased: "*", "*.rest", or a host
        std::optional<std::string> port; // "*" or digits
        std::string path; // as written; empty for none
    };

    struct Directive {
        std::string name; // lowercased
        std::vector<std::string> values; // as written
        std::vector<Source> sources; // the values a fetch directive's list parses to
        std::string text() const; // the name and the values, space-separated
        bool has_keyword(std::string_view keyword) const;
    };

    struct Policy {
        std::vector<Directive> directives;
        bool report_only = false;
        bool from_meta = false;
        Directive const* find(std::string_view name) const;
    };

    // The document's URL is what 'self' means.
    explicit ContentSecurityPolicy(Url document_url = {});

    // Every violation — refused or report-only — and every parse problem,
    // described once each. Set it before the policies are added.
    void set_reporter(std::function<void(std::string_view message)> report);

    // A header's value: one policy, or several separated by commas.
    void add_header(std::string_view value, bool report_only);
    // A <meta http-equiv=content-security-policy> content attribute:
    // report-uri, frame-ancestors and sandbox are dropped from it, as the
    // HTML specification says. The same content twice is one policy.
    void add_meta(std::string_view content);

    bool empty() const { return m_policies.empty(); }
    std::vector<Policy> const& policies() const { return m_policies; }
    // How many requests, inline blocks, compilations and submissions an
    // enforced policy refused.
    std::size_t refusals() const { return m_refusals; }

    // The guard for a request of `kind`, made for an element carrying
    // `nonce` (empty for none) that the parser inserted or a script did
    // ('strict-dynamic' lets the latter through).
    RequestGuard guard(ResourceKind kind, std::string nonce = {}, bool parser_inserted = true);
    // The refusal for a request, reported; nullopt when it may go. A
    // redirect's target is judged with the paths of the sources ignored.
    std::optional<std::string> request_refusal(ResourceKind kind, Url const& url, bool redirected,
        std::string_view nonce = {}, bool parser_inserted = true);
    // An inline script, an event handler attribute, a <style> or a style=""
    // attribute: `source` is its text, hashed against the hash sources.
    std::optional<std::string> inline_refusal(InlineKind kind, std::string_view nonce, std::string_view source);
    // eval, Function and their kin: HostEnsureCanCompileStrings.
    std::optional<std::string> eval_refusal();
    // A form's submission URL, judged by form-action.
    std::optional<std::string> form_action_refusal(Url const& action);

    bool upgrade_insecure_requests() const { return m_upgrade_insecure; }
    // The sandbox directive, from a header only: scripts and forms are
    // off unless every sandboxing policy allows them.
    bool sandboxed() const { return m_sandboxed; }
    bool sandbox_allows_scripts() const { return !m_sandboxed || m_sandbox_scripts; }
    bool sandbox_allows_forms() const { return !m_sandboxed || m_sandbox_forms; }

    // The base64 of a text's SHA-256, as a policy would name it.
    static std::string sha256_source(std::string_view text);

    // Exposed for tests: one serialized policy, parsed.
    Policy parse_policy(std::string_view text, bool report_only, bool from_meta);

private:
    void add_policy(Policy policy);
    void report(std::string_view message);
    // The directive of a policy that governs `effective` after the
    // fallback chain, or null when none does.
    static Directive const* governing(Policy const& policy, std::string_view effective);
    bool source_matches(Source const& source, Url const& url, bool redirected) const;
    bool list_matches(Directive const& directive, Url const& url, bool redirected) const;
    bool inline_matches(Directive const& directive, InlineKind kind, std::string_view nonce, std::string_view source) const;

    Url m_document_url;
    std::function<void(std::string_view)> m_report;
    std::vector<Policy> m_policies;
    std::unordered_set<std::string> m_reported;
    std::unordered_set<std::string> m_meta_seen;
    std::size_t m_refusals = 0;
    bool m_upgrade_insecure = false;
    bool m_sandboxed = false;
    bool m_sandbox_scripts = true;
    bool m_sandbox_forms = true;
};

}
