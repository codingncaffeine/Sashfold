#include "net/Filters.h"

#include "core/Ascii.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sashfold::net {

namespace {

constexpr std::uint16_t all_kinds = 0x1FF;
// What a rule with no type option applies to: everything a page requests;
// a navigation only when the rule says `document`.
constexpr std::uint16_t page_kinds = all_kinds & ~static_cast<std::uint16_t>(ResourceKind::Document);

bool is_token_char(char c)
{
    return is_ascii_alphanumeric(static_cast<unsigned char>(c)) || c == '%';
}

// The characters `^` stands for: anything but a letter, a digit, or one of
// `_ - . %`; and the end of the URL.
bool is_separator(char c)
{
    return !is_ascii_alphanumeric(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.' && c != '%';
}

std::string lowercase(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// The pattern from `p` against the URL from `u`: `*` any run, `^` a
// separator or the end, the rest itself; the whole pattern must be used,
// and the whole URL too when the rule is end-anchored.
bool glob(std::string_view pattern, std::size_t p, std::string_view url, std::size_t u, bool to_end)
{
    while (p < pattern.size()) {
        char const c = pattern[p];
        if (c == '*') {
            // A run of stars is one; try every length the run could take.
            while (p < pattern.size() && pattern[p] == '*')
                ++p;
            if (p == pattern.size())
                return true; // the star takes the rest, anchored or not
            for (std::size_t k = u; k <= url.size(); ++k) {
                if (glob(pattern, p, url, k, to_end))
                    return true;
            }
            return false;
        }
        if (c == '^') {
            if (u == url.size()) {
                ++p;
                continue; // the end counts as a separator, and nothing may follow it but ^ and *
            }
            if (!is_separator(url[u]))
                return false;
            ++p;
            ++u;
            continue;
        }
        if (u >= url.size() || url[u] != c)
            return false;
        ++p;
        ++u;
    }
    return !to_end || u == url.size();
}

bool host_within(std::string_view host, std::string_view domain)
{
    if (host.size() < domain.size())
        return false;
    if (host == domain)
        return true;
    return host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.';
}

bool third_party(FilterRequest const& request)
{
    if (!request.first_party || request.first_party->host.empty() || request.url->host.empty())
        return true;
    return registrable_domain(request.first_party->host) != registrable_domain(request.url->host);
}

} // namespace

std::string registrable_domain(std::string_view host)
{
    std::string const lower = lowercase(host);
    if (lower.empty() || lower.front() == '[' || std::all_of(lower.begin(), lower.end(), [](char c) {
            return is_ascii_digit(static_cast<unsigned char>(c)) || c == '.';
        }))
        return lower;
    std::vector<std::string_view> labels;
    std::string_view rest = lower;
    while (!rest.empty()) {
        std::size_t const dot = rest.find('.');
        labels.push_back(rest.substr(0, dot));
        if (dot == std::string_view::npos)
            break;
        rest.remove_prefix(dot + 1);
    }
    if (labels.size() <= 2)
        return lower;
    std::size_t keep = 2;
    static constexpr std::string_view second_levels[]
        = { "co", "com", "org", "net", "gov", "edu", "ac", "ne", "or", "go", "mil", "nom", "ltd", "plc", "sch" };
    std::string_view const tld = labels.back();
    std::string_view const second = labels[labels.size() - 2];
    if (tld.size() == 2 && std::find(std::begin(second_levels), std::end(second_levels), second) != std::end(second_levels))
        keep = 3;
    std::string out;
    for (std::size_t i = labels.size() - keep; i < labels.size(); ++i) {
        if (!out.empty())
            out += '.';
        out += labels[i];
    }
    return out;
}

// --- FilterList --------------------------------------------------------------

FilterList FilterList::parse(std::string_view text, std::string name, bool everything)
{
    FilterList list;
    list.m_name = std::move(name);
    list.m_everything = everything;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t const end = text.find('\n', at);
        std::string_view const line = trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (!line.empty())
            list.add_rule(line);
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return list;
}

bool FilterList::add_rule(std::string_view line)
{
    if (line.front() == '!' || line.front() == '[')
        return false; // a comment, or the header
    if (line.find("##") != std::string_view::npos || line.find("#@#") != std::string_view::npos
        || line.find("#?#") != std::string_view::npos || line.find("#$#") != std::string_view::npos
        || line.find("#%#") != std::string_view::npos)
        return add_cosmetic_rule(line);
    Rule rule;
    rule.text = std::string(line);
    std::string_view body = line;

    // A hosts file line: an address, then the host it names.
    if (body.starts_with("0.0.0.0 ") || body.starts_with("127.0.0.1 ") || body.starts_with("::1 ") || body.starts_with("0.0.0.0\t")
        || body.starts_with("127.0.0.1\t")) {
        body = trim(body.substr(body.find_first_of(" \t")));
        if (std::size_t const comment = body.find('#'); comment != std::string_view::npos)
            body = trim(body.substr(0, comment));
        if (body.empty() || body == "localhost" || body == "localhost.localdomain" || body == "broadcasthost"
            || body == "local" || body == "ip6-localhost" || body == "ip6-loopback")
            return false;
        rule.pattern = lowercase(body);
        rule.domain_anchor = true;
        rule.end_anchor = false;
        rule.pattern += '^';
        rule.types = m_everything ? all_kinds : page_kinds;
        m_rules.push_back(std::move(rule));
        index(static_cast<std::uint32_t>(m_rules.size() - 1));
        ++m_counts.rules;
        return true;
    }

    if (body.starts_with("@@")) {
        if (m_everything) {
            // A list of sites to keep away from admits no exceptions.
            ++m_counts.unsupported;
            return false;
        }
        rule.exception = true;
        body.remove_prefix(2);
    }
    // A bare URL or a bare host, as the lists of sites are written.
    bool const bare_url = body.starts_with("http://") || body.starts_with("https://");
    bool const bare_host = !bare_url && !body.empty() && body.find_first_of("/|^*$@ \t") == std::string_view::npos
        && body.find('.') != std::string_view::npos;
    if (bare_url || bare_host) {
        if (bare_url) {
            rule.pattern = lowercase(body);
            rule.start_anchor = true;
        } else {
            rule.pattern = lowercase(body) + "^";
            rule.domain_anchor = true;
        }
        rule.types = m_everything ? all_kinds : page_kinds;
        bool const bare_exception = rule.exception;
        m_rules.push_back(std::move(rule));
        index(static_cast<std::uint32_t>(m_rules.size() - 1));
        ++(bare_exception ? m_counts.exceptions : m_counts.rules);
        return true;
    }

    // Options after the last `$`.
    std::string_view pattern = body;
    std::string_view options;
    if (std::size_t const dollar = body.rfind('$'); dollar != std::string_view::npos && dollar > 0) {
        pattern = body.substr(0, dollar);
        options = body.substr(dollar + 1);
    }
    if (pattern.size() >= 2 && pattern.front() == '/' && pattern.back() == '/') {
        ++m_counts.unsupported; // a regular expression
        return false;
    }
    std::uint16_t types_in = 0;
    std::uint16_t types_out = 0;
    while (!options.empty()) {
        std::size_t const comma = options.find(',');
        std::string_view option = trim(options.substr(0, comma));
        options = comma == std::string_view::npos ? std::string_view() : options.substr(comma + 1);
        if (option.empty())
            continue;
        bool negated = false;
        if (option.front() == '~') {
            negated = true;
            option.remove_prefix(1);
        }
        std::string const name = lowercase(option.substr(0, option.find('=')));
        std::string_view const value = option.find('=') == std::string_view::npos ? std::string_view() : option.substr(option.find('=') + 1);
        auto const type = [&](ResourceKind kind) {
            (negated ? types_out : types_in) |= static_cast<std::uint16_t>(kind);
        };
        if (name == "script")
            type(ResourceKind::Script);
        else if (name == "image")
            type(ResourceKind::Image);
        else if (name == "stylesheet")
            type(ResourceKind::Stylesheet);
        else if (name == "font")
            type(ResourceKind::Font);
        else if (name == "xmlhttprequest" || name == "xhr")
            type(ResourceKind::Xhr);
        else if (name == "subdocument" || name == "frame")
            type(ResourceKind::Subdocument);
        else if (name == "document" || name == "doc")
            type(ResourceKind::Document);
        else if (name == "media")
            type(ResourceKind::Media);
        else if (name == "other" || name == "object" || name == "ping" || name == "beacon" || name == "websocket")
            type(ResourceKind::Other);
        else if (name == "all")
            (negated ? types_out : types_in) |= all_kinds;
        else if (name == "third-party" || name == "3p")
            rule.party = negated ? 0 : 1;
        else if (name == "first-party" || name == "1p")
            rule.party = negated ? 1 : 0;
        else if (name == "match-case")
            rule.match_case = !negated;
        else if (name == "important")
            rule.important = true;
        else if (name == "domain" || name == "from") {
            std::string_view rest = value;
            while (!rest.empty()) {
                std::size_t const bar = rest.find('|');
                std::string_view item = rest.substr(0, bar);
                rest = bar == std::string_view::npos ? std::string_view() : rest.substr(bar + 1);
                if (item.empty())
                    continue;
                if (item.front() == '~')
                    rule.domains_out.push_back(lowercase(item.substr(1)));
                else
                    rule.domains_in.push_back(lowercase(item));
            }
        } else {
            // csp=, redirect=, removeparam=, to=, denyallow=, badfilter, popup,
            // the hiding options and the rest this engine does not carry.
            ++m_counts.unsupported;
            return false;
        }
    }
    rule.types = types_in != 0 ? types_in : (m_everything ? all_kinds : page_kinds);
    rule.types = static_cast<std::uint16_t>(rule.types & ~types_out);
    if (m_everything && types_in == 0)
        rule.types = all_kinds;
    if (rule.types == 0) {
        ++m_counts.unsupported;
        return false;
    }

    // Anchors.
    if (pattern.starts_with("||")) {
        rule.domain_anchor = true;
        pattern.remove_prefix(2);
    } else if (pattern.starts_with("|")) {
        rule.start_anchor = true;
        pattern.remove_prefix(1);
    }
    if (pattern.ends_with("|")) {
        rule.end_anchor = true;
        pattern.remove_suffix(1);
    }
    while (pattern.starts_with("*"))
        pattern.remove_prefix(1);
    while (pattern.ends_with("*") && !rule.end_anchor)
        pattern.remove_suffix(1);
    if (pattern.empty() && !rule.domain_anchor && !rule.start_anchor) {
        // A rule matching every URL is a list's mistake, or a `$` option
        // this engine treats as a whole-list statement; neither is applied.
        ++m_counts.unsupported;
        return false;
    }
    rule.pattern = rule.match_case ? std::string(pattern) : lowercase(pattern);
    bool const exception = rule.exception;
    m_rules.push_back(std::move(rule));
    index(static_cast<std::uint32_t>(m_rules.size() - 1));
    ++(exception ? m_counts.exceptions : m_counts.rules);
    return true;
}

// The rule's longest token: a run of token characters bounded on both
// sides by something a URL cannot carry inside a token (a star could).
void FilterList::index(std::uint32_t id)
{
    Rule const& rule = m_rules[id];
    std::string const pattern = rule.match_case ? lowercase(rule.pattern) : rule.pattern;
    std::string best;
    std::size_t i = 0;
    while (i < pattern.size()) {
        if (!is_token_char(pattern[i])) {
            ++i;
            continue;
        }
        std::size_t const start = i;
        while (i < pattern.size() && is_token_char(pattern[i]))
            ++i;
        bool const bounded_before = start == 0 ? (rule.start_anchor || rule.domain_anchor) : pattern[start - 1] != '*';
        bool const bounded_after = i == pattern.size() ? rule.end_anchor : pattern[i] != '*';
        if (bounded_before && bounded_after && i - start > best.size())
            best = pattern.substr(start, i - start);
    }
    if (best.empty())
        m_untokened.push_back(id);
    else
        m_by_token[best].push_back(id);
}

bool FilterList::matches(Rule const& rule, FilterRequest const& request, std::string const& url_text,
    std::size_t host_at, std::size_t host_end) const
{
    if ((rule.types & static_cast<std::uint16_t>(request.kind)) == 0)
        return false;
    if (rule.party != -1 && third_party(request) != (rule.party == 1))
        return false;
    if (!rule.domains_in.empty() || !rule.domains_out.empty()) {
        std::string const page_host = request.first_party ? lowercase(request.first_party->host) : std::string();
        if (!rule.domains_in.empty()) {
            bool inside = false;
            for (std::string const& domain : rule.domains_in)
                inside = inside || host_within(page_host, domain);
            if (!inside)
                return false;
        }
        for (std::string const& domain : rule.domains_out) {
            if (host_within(page_host, domain))
                return false;
        }
    }
    std::string const& url = url_text;
    std::string cased;
    std::string_view subject = url;
    if (rule.match_case) {
        cased = request.url->serialize();
        subject = cased;
    }
    if (rule.start_anchor)
        return glob(rule.pattern, 0, subject, 0, rule.end_anchor);
    if (rule.domain_anchor) {
        if (host_at == std::string::npos)
            return false;
        for (std::size_t at = host_at; at < host_end; ++at) {
            if (at == host_at || subject[at - 1] == '.') {
                if (glob(rule.pattern, 0, subject, at, rule.end_anchor))
                    return true;
            }
        }
        return false;
    }
    for (std::size_t at = 0; at < subject.size(); ++at) {
        if (rule.pattern.empty() || rule.pattern.front() == '*' || rule.pattern.front() == '^'
            || subject[at] == rule.pattern.front()) {
            if (glob(rule.pattern, 0, subject, at, rule.end_anchor))
                return true;
        }
    }
    return false;
}

FilterList::Decision FilterList::decide(FilterRequest const& request) const
{
    Decision decision;
    if (!request.url || m_rules.empty())
        return decision;
    std::string const url = lowercase(request.url->serialize());
    // Where the host lies in the text, for the domain anchor.
    std::size_t host_at = std::string::npos;
    std::size_t host_end = std::string::npos;
    if (!request.url->host.empty()) {
        std::size_t const scheme_end = url.find("://");
        if (scheme_end != std::string::npos) {
            host_at = scheme_end + 3;
            if (host_at < url.size() && url[host_at] == '[') {
                host_end = url.find(']', host_at);
                if (host_end != std::string::npos)
                    ++host_end;
            } else {
                host_end = url.find_first_of(":/?#", host_at);
            }
            if (host_end == std::string::npos)
                host_end = url.size();
        }
    }
    // The candidates: every rule filed under a token the URL carries, and
    // the rules filed under none.
    std::vector<std::uint32_t> candidates(m_untokened);
    std::size_t i = 0;
    while (i < url.size()) {
        if (!is_token_char(url[i])) {
            ++i;
            continue;
        }
        std::size_t const start = i;
        while (i < url.size() && is_token_char(url[i]))
            ++i;
        if (auto const found = m_by_token.find(url.substr(start, i - start)); found != m_by_token.end())
            candidates.insert(candidates.end(), found->second.begin(), found->second.end());
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    Rule const* block = nullptr;
    Rule const* allow = nullptr;
    for (std::uint32_t const id : candidates) {
        Rule const& rule = m_rules[id];
        if ((rule.exception ? allow : block) != nullptr && !(rule.important && block && !block->important))
            continue;
        if (!matches(rule, request, url, host_at, host_end))
            continue;
        if (rule.exception)
            allow = &rule;
        else if (!block || (rule.important && !block->important))
            block = &rule;
    }
    if (block && (!allow || block->important)) {
        decision.verdict = Verdict::Block;
        decision.rule = block->text;
        decision.important = block->important;
    } else if (allow) {
        decision.verdict = Verdict::Allow;
        decision.rule = allow->text;
    }
    return decision;
}

// --- Blocklists --------------------------------------------------------------

void Blocklists::load_directory(std::string const& directory, std::vector<std::string>* problems)
{
    namespace fs = std::filesystem;
    for (auto const& [folder, everything] : { std::pair { "filters", false }, std::pair { "nefarious", true } }) {
        std::error_code error;
        std::vector<fs::path> files;
        for (fs::directory_entry const& entry : fs::directory_iterator(fs::path(directory) / folder, error)) {
            if (entry.is_regular_file(error) && entry.path().extension() == ".txt")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        for (fs::path const& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                if (problems)
                    problems->push_back("blocklists: cannot read " + file.string());
                continue;
            }
            std::ostringstream text;
            text << in.rdbuf();
            add(FilterList::parse(std::move(text).str(), file.filename().string(), everything));
        }
    }
}

void Blocklists::add(FilterList list)
{
    m_lists.push_back(std::move(list));
}

std::optional<Blocklists::Block> Blocklists::blocks(FilterRequest const& request) const
{
    // The sites to keep away from first: nothing lifts those.
    for (FilterList const& list : m_lists) {
        if (!list.everything())
            continue;
        FilterList::Decision const decision = list.decide(request);
        if (decision.verdict == FilterList::Verdict::Block)
            return Block { std::string(decision.rule), list.name(), true };
    }
    std::optional<Block> block;
    bool important = false;
    bool allowed = false;
    for (FilterList const& list : m_lists) {
        if (list.everything())
            continue;
        FilterList::Decision const decision = list.decide(request);
        if (decision.verdict == FilterList::Verdict::Block && (!block || (decision.important && !important))) {
            block = Block { std::string(decision.rule), list.name(), false };
            important = decision.important;
        } else if (decision.verdict == FilterList::Verdict::Allow) {
            allowed = true;
        }
    }
    if (block && allowed && !important)
        return std::nullopt;
    return block;
}

std::size_t Blocklists::rule_count() const
{
    std::size_t count = 0;
    for (FilterList const& list : m_lists)
        count += list.size();
    return count;
}

// --- Cosmetic rules ----------------------------------------------------------------

// `[domains]##selector` hides, `[domains]#@#selector` lifts; the domains
// are a comma list, each negated by a leading `~`. The procedural (`#?#`),
// style-injecting (`#$#`), snippet (`#%#`) and scriptlet (`##+js(`) forms
// are counted and left, as is a hide in a list of sites to keep away from
// — that list decides navigations, not what a page shows.
bool FilterList::add_cosmetic_rule(std::string_view line)
{
    if (m_everything) {
        ++m_counts.unsupported;
        return false;
    }
    std::size_t at = std::string_view::npos;
    bool exception = false;
    for (std::string_view const marker : { "#?#", "#$#", "#%#" }) {
        if (line.find(marker) != std::string_view::npos) {
            ++m_counts.unsupported;
            return false;
        }
    }
    if (std::size_t const lift = line.find("#@#"); lift != std::string_view::npos) {
        at = lift;
        exception = true;
    } else {
        at = line.find("##");
    }
    if (at == std::string_view::npos)
        return false;
    std::string_view const selector = trim(line.substr(at + (exception ? 3 : 2)));
    if (selector.empty() || selector.starts_with("+js(")) {
        ++m_counts.unsupported;
        return false;
    }
    // The procedural pseudo-classes of the extended syntax are not CSS: a
    // rule using one is left, not handed to the selector parser to drop.
    for (std::string_view const procedural : { ":has-text(", ":matches-css", ":xpath(", ":min-text-length(", ":upward(",
             ":remove(", ":style(", ":matches-path(", ":watch-attr(", ":matches-media(", ":matches-prop(", ":others(",
             ":if(", ":if-not(", ":nth-ancestor(", ":matches-attr(" }) {
        if (selector.find(procedural) != std::string_view::npos) {
            ++m_counts.unsupported;
            return false;
        }
    }
    Cosmetic rule;
    rule.selector = std::string(selector);
    rule.exception = exception;
    std::string_view domains = line.substr(0, at);
    while (!domains.empty()) {
        std::size_t const comma = domains.find(',');
        std::string_view const item = trim(domains.substr(0, comma));
        domains = comma == std::string_view::npos ? std::string_view() : domains.substr(comma + 1);
        if (item.empty())
            continue;
        if (item.front() == '~')
            rule.domains_out.push_back(lowercase(item.substr(1)));
        else
            rule.domains_in.push_back(lowercase(item));
    }
    m_cosmetic.push_back(std::move(rule));
    ++m_counts.cosmetic;
    return true;
}

void FilterList::cosmetic_for(std::string_view host, std::vector<std::string>& hide, std::vector<std::string>& lift) const
{
    std::string const page_host = lowercase(host);
    for (Cosmetic const& rule : m_cosmetic) {
        if (!rule.domains_in.empty()) {
            bool inside = false;
            for (std::string const& domain : rule.domains_in)
                inside = inside || host_within(page_host, domain);
            if (!inside)
                continue;
        }
        bool excluded = false;
        for (std::string const& domain : rule.domains_out)
            excluded = excluded || host_within(page_host, domain);
        if (excluded)
            continue;
        (rule.exception ? lift : hide).push_back(rule.selector);
    }
}

std::vector<std::string> Blocklists::hidden_selectors(std::string_view host) const
{
    std::vector<std::string> hide;
    std::vector<std::string> lift;
    for (FilterList const& list : m_lists) {
        if (!list.everything())
            list.cosmetic_for(host, hide, lift);
    }
    std::vector<std::string> out;
    for (std::string const& selector : hide) {
        if (std::find(lift.begin(), lift.end(), selector) != lift.end())
            continue;
        if (std::find(out.begin(), out.end(), selector) != out.end())
            continue;
        out.push_back(selector);
    }
    return out;
}

std::size_t Blocklists::cosmetic_count() const
{
    std::size_t count = 0;
    for (FilterList const& list : m_lists)
        count += list.cosmetic_rules().size();
    return count;
}

}
